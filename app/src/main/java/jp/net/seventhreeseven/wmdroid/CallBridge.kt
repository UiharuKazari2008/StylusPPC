package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.app.role.RoleManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.media.AudioManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.PowerManager
import android.os.RemoteException
import android.os.SystemClock
import android.telecom.TelecomManager
import android.telephony.PhoneStateListener
import android.telephony.TelephonyCallback
import android.telephony.TelephonyManager
import android.util.Log
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import java.util.concurrent.Executor

/**
 * Phone calls from Windows Mobile's own Phone app (main process side).
 *
 * The guest helper (guest/Helper/Calls.cs) reports each call cprog dials ("call dial <id> <number>")
 * and WM's End / Hold / Mute / Speaker; this class places the real call and reports its progress
 * back ("call state <id> dialing|active|held|ended"), which drives WM's call screen.
 *
 * Two modes:
 *  - WMDroid holds the dialer role: WmInCallService (process ":call", CallService.kt) gets the call,
 *    no Android in-call screen appears, and the WM controls act on the call through it. Whenever
 *    Windows Mobile cannot show the call (the app leaves the screen, the emulator dies) the call is
 *    handed to the phone's own dialer.
 *  - Without the role: TelecomManager.placeCall shows the phone's dialer as usual and WM mirrors the
 *    call from the phone state (off-hook counts as connected; there is no finer state without the
 *    role). End uses TelecomManager.endCall; mute and speaker are best effort.
 *
 * Call audio never passes through the emulator: it stays on the phone's earpiece, speaker or headset.
 * docs/research-phone-calls-pim.md, section E.
 */
class CallBridge(private val activity: Activity, private val guest: GuestBridge, private val handle: () -> Long = { 0L }) {
    private val ctx: Context = activity
    private val ui = Handler(Looper.getMainLooper())
    private val telecom = ctx.getSystemService(TelecomManager::class.java)
    private val telephony = ctx.getSystemService(TelephonyManager::class.java)
    private val audio = ctx.getSystemService(AudioManager::class.java)

    private class WmCall(val id: Int, val number: String, val token: String, var owned: Boolean) {
        var state = "dialing"
        val placedAt = SystemClock.uptimeMillis()
        /** The phone went off-hook since this call was placed (an idle state before that is stale). */
        var offHook = false
        fun stale() = !offHook && SystemClock.uptimeMillis() - placedAt > 30_000
    }

    /** Windows Mobile's calls by its call id. */
    private val calls = HashMap<Int, WmCall>()
    /**
     * Calls dialled through the virtual modem (WM 6 / 6.1 / 6.1.4, core/src/board/modem.h) rather than
     * reported by the helper: their progress goes back to the modem.
     */
    private val modemIds = HashSet<Int>()
    private var waitingPermission: Pair<Int, String>? = null
    private var foreground = false
    private var restartAsked = false
    private var lastEnabled: Boolean? = null

    // ---- Settings ----

    companion object {
        private const val TAG = "WMDroidCall"
        const val REQ_CALL = 30
        private const val PREF = "calls.enabled"

        fun enabled(ctx: Context) = Settings.prefs(ctx).getBoolean(PREF, false)
        fun setEnabled(ctx: Context, on: Boolean) =
            Settings.prefs(ctx).edit().putBoolean(PREF, on).apply()

        /** WMDroid is the default phone app, so WM's call screen can be the only one. */
        fun holdsRole(ctx: Context): Boolean =
            ctx.getSystemService(RoleManager::class.java)?.isRoleHeld(RoleManager.ROLE_DIALER) == true

        fun roleRequest(ctx: Context): Intent? = ctx.getSystemService(RoleManager::class.java)
            ?.takeIf { it.isRoleAvailable(RoleManager.ROLE_DIALER) }?.createRequestRoleIntent(RoleManager.ROLE_DIALER)

        /** Permissions placing and ending calls need (the dialer role grants them too). */
        fun permissions(): Array<String> = arrayOf(Manifest.permission.CALL_PHONE, Manifest.permission.ANSWER_PHONE_CALLS,
            Manifest.permission.READ_PHONE_STATE, PhoneNumber.PERMISSION)

        fun hasCallPermission(ctx: Context) =
            ctx.checkSelfPermission(Manifest.permission.CALL_PHONE) == PackageManager.PERMISSION_GRANTED

        /** Lines for the helper's state.ini (GuestBridge.writeState). */
        fun stateLines(ctx: Context): String =
            if (enabled(ctx)) "calls=1\npatch.fakeril=on\n" else "calls=0\n"
    }

    // ---- Lifecycle (EmulatorActivity) ----

    fun resume() {
        foreground = true
        val on = enabled(ctx)
        if (lastEnabled != null && lastEnabled != on) guest.writeState()  // the helper re-reads it every 5 s
        lastEnabled = on
        if (on && holdsRole(ctx)) bindLink()
        resync()
        updateFastPoll()
    }

    fun pause() {
        foreground = false
        updateFastPoll()
        updateWakeLock()
    }

    /**
     * The app leaves the screen: Windows Mobile can no longer show its calls, so calls held through
     * the dialer role go to the phone's dialer (without the role they are there already).
     */
    fun leave() {
        if (calls.values.any { it.owned }) {
            Log.i(TAG, "leaving the screen during a call: handing it to the phone's dialer")
            sendLink(Message.obtain(null, CallLink.HANDOFF))
            handedOff()
        }
    }

    fun stop() {
        leave()
        unbindLink()
        stopListening()
        updateWakeLock()
        ui.removeCallbacks(fastPoll)
    }

    // ---- From the guest ----

    /** Events from the virtual modem: the same verbs as the helper's "call ..." lines. */
    fun pollModem() {
        val h = handle()
        if (h == 0L) return
        val events = NativeBridge.modemEvents(h)
        if (events.isEmpty()) return
        for (line in events.split('\n')) {
            if (line.isBlank()) continue
            Log.i(TAG, "modem: $line")
            val p = line.split(' ')
            if (p[0] == "dial") p.getOrNull(1)?.toIntOrNull()?.let { modemIds.add(it) }
            onGuest(line)
            if (p[0] == "end") p.getOrNull(1)?.toIntOrNull()?.let { modemIds.remove(it) }
        }
    }

    /** A "call ..." line from the helper, without the "call " prefix. */
    fun onGuest(line: String) {
        val p = line.split(' ')
        when (p[0]) {
            "dial" -> if (p.size >= 3) dial(p[1].toIntOrNull() ?: return, p[2])
            "end" -> calls[p.getOrNull(1)?.toIntOrNull() ?: return]?.let { wmEnded(it) }
            "hold" -> calls[p.getOrNull(1)?.toIntOrNull() ?: return]?.let { c ->
                if (c.owned) sendLink(Message.obtain(null, CallLink.HOLD, if (p.getOrNull(2) == "1") 1 else 0, 0).withToken(c.token))
            }
            "mute" -> mute(p.getOrNull(1) == "1")
            "speaker" -> speaker(p.getOrNull(1) == "1")
            "ready" -> if (line.contains("restart")) askRestart()
            else -> Log.i(TAG, "guest: call $line")
        }
    }

    private fun dial(id: Int, number: String) {
        calls[id]?.let { return }  // already placed (the helper repeats nothing, but be safe)
        if (!enabled(ctx)) { state(id, "ended", "disabled"); return }
        if (isEmergency(number)) {
            // Emergency calls always go through the phone's own dialer, never blocked or hidden.
            Log.i(TAG, "emergency number: opening the phone's dialer")
            openSystemDialer(number)
            state(id, "ended", "emergency")
            return
        }
        if (!hasCallPermission(ctx)) {
            state(id, "pending")
            waitingPermission = id to number
            activity.requestPermissions(permissions(), REQ_CALL)
            return
        }
        place(id, number)
    }

    fun onPermissionsResult(requestCode: Int) {
        if (requestCode != REQ_CALL) return
        val (id, number) = waitingPermission ?: return
        waitingPermission = null
        if (hasCallPermission(ctx)) place(id, number) else state(id, "ended", "permission")
    }

    @SuppressLint("MissingPermission")
    private fun place(id: Int, number: String) {
        val token = "wm-$id-${SystemClock.uptimeMillis()}"
        val owned = holdsRole(ctx) && link != null
        if (owned) sendLink(Message.obtain(null, CallLink.PLACING).apply {
            data = Bundle().apply { putString(CallLink.KEY_TOKEN, token); putString(CallLink.KEY_NUMBER, number) }
        })
        val extras = Bundle().apply {
            putString(CallLink.KEY_TOKEN, token)
            putBundle(TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS, Bundle().apply { putString(CallLink.KEY_TOKEN, token) })
            // A fixed SIM, so Telecom does not stop to ask which one (that needs a screen).
            defaultAccount()?.let { putParcelable(TelecomManager.EXTRA_PHONE_ACCOUNT_HANDLE, it) }
        }
        try {
            telecom.placeCall(Uri.fromParts("tel", number, null), extras)
        } catch (e: RuntimeException) {
            Log.w(TAG, "placeCall failed", e)
            state(id, "ended", "error")
            return
        }
        Log.i(TAG, "placed WM call $id ($number), ${if (owned) "WM shows it" else "the phone's dialer shows it"}")
        if (!owned) {
            // Without the dialer role Android's in-call screen comes up over us: once it has, bring
            // Windows Mobile back in front (the call carries on; WM mirrors it, and the phone's
            // screen stays a tap away in the notification).
            android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                if (!activity.isFinishing && calls.containsKey(id)) runCatching {
                    activity.startActivity(android.content.Intent(activity, activity.javaClass)
                        .addFlags(android.content.Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or android.content.Intent.FLAG_ACTIVITY_SINGLE_TOP))
                }
            }, 1500)
        }
        calls[id] = WmCall(id, number, token, owned)
        state(id, "dialing")
        startListening()
        updateFastPoll()
    }

    @SuppressLint("MissingPermission")
    private fun defaultAccount() = runCatching {
        telecom.getDefaultOutgoingPhoneAccount("tel") ?: telecom.callCapablePhoneAccounts.singleOrNull()
    }.getOrNull()

    /** WM's call is gone (its End, or the hang-up we asked for): end the phone's call too. */
    @SuppressLint("MissingPermission")
    private fun wmEnded(c: WmCall) {
        calls.remove(c.id)
        if (c.state == "ended") return finishCalls()
        if (c.owned) sendLink(Message.obtain(null, CallLink.DISCONNECT).withToken(c.token))
        else endCallWithoutRole()
        finishCalls()
    }

    /** Without the role: the deprecated, still documented TelecomManager.endCall (ANSWER_PHONE_CALLS). */
    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    private fun endCallWithoutRole() {
        runCatching { telecom.endCall() }.onFailure { Log.w(TAG, "endCall failed", it) }
    }

    private fun mute(on: Boolean) {
        if (calls.isEmpty()) return
        if (calls.values.any { it.owned }) sendLink(Message.obtain(null, CallLink.MUTE, if (on) 1 else 0, 0))
        else runCatching { audio.isMicrophoneMute = on }  // best effort without the role
    }

    @Suppress("DEPRECATION")
    private fun speaker(on: Boolean) {
        if (calls.isEmpty()) return
        if (calls.values.any { it.owned }) sendLink(Message.obtain(null, CallLink.SPEAKER, if (on) 1 else 0, 0))
        else runCatching { audio.isSpeakerphoneOn = on }  // best effort without the role
    }

    /** Tells WM the real call's progress. */
    private fun state(id: Int, s: String, cause: String = "") {
        calls[id]?.state = s
        if (id in modemIds) {
            NativeBridge.modemCallState(handle(), id, s)
            if (s == "ended") modemIds.remove(id)
        } else {
            guest.send("call state $id $s${if (cause.isNotEmpty()) " $cause" else ""}")
        }
        if (s == "ended") { calls.remove(id); finishCalls() } else updateWakeLock()
    }

    private fun finishCalls() {
        if (calls.isNotEmpty()) return
        stopListening()
        updateWakeLock()
        updateFastPoll()
    }

    // ---- Link to the call process (dialer role) ----

    private var link: Messenger? = null
    private var bound = false
    private val replies = Messenger(object : Handler(Looper.getMainLooper()) {
        override fun handleMessage(m: Message) {
            when (m.what) {
                CallLink.STATE -> {
                    val token = CallLink.token(m) ?: return
                    val c = calls.values.firstOrNull { it.token == token } ?: return
                    val s = m.data.getString(CallLink.KEY_STATE) ?: return
                    if (s != c.state) state(c.id, s, m.data.getString(CallLink.KEY_CAUSE) ?: "")
                }
                CallLink.HANDED_OFF -> handedOff()
            }
        }
    })

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            link = Messenger(binder)
            sendLink(Message.obtain(null, CallLink.HELLO).apply { replyTo = replies })
        }

        override fun onServiceDisconnected(name: ComponentName) {
            // The call process ended (a hand-off, or it crashed: Telecom gives the call to the phone's dialer).
            link = null
            handedOff()
        }
    }

    private fun bindLink() {
        if (bound) return
        bound = ctx.bindService(Intent(ctx, CallLinkService::class.java), connection, Context.BIND_AUTO_CREATE)
    }

    private fun unbindLink() {
        if (!bound) return
        runCatching { ctx.unbindService(connection) }
        bound = false
        link = null
    }

    private fun sendLink(m: Message) {
        val l = link ?: return
        try { l.send(m) } catch (e: RemoteException) { Log.w(TAG, "call process unreachable", e) }
    }

    private fun Message.withToken(token: String) = apply { data = Bundle().apply { putString(CallLink.KEY_TOKEN, token) } }

    /** The phone's dialer has the calls now: WM's calls end there (the real call goes on). */
    private fun handedOff() {
        for (c in calls.values.filter { it.owned }.toList()) state(c.id, "ended", "handoff")
    }

    // ---- Phone state (calls without the role, and a backstop for the rest) ----

    private var listener: Any? = null

    @SuppressLint("MissingPermission")
    private fun startListening() {
        if (listener != null || ctx.checkSelfPermission(Manifest.permission.READ_PHONE_STATE) != PackageManager.PERMISSION_GRANTED) return
        if (Build.VERSION.SDK_INT >= 31) {
            val cb = object : TelephonyCallback(), TelephonyCallback.CallStateListener {
                override fun onCallStateChanged(state: Int) {
                    ui.post { phoneState(state) }
                }
            }
            runCatching { telephony.registerTelephonyCallback(Executor { it.run() }, cb); listener = cb }
        } else {
            @Suppress("DEPRECATION")
            val l = object : PhoneStateListener() {
                @Deprecated("Deprecated in Java")
                override fun onCallStateChanged(state: Int, phoneNumber: String?) = phoneState(state)
            }
            @Suppress("DEPRECATION")
            runCatching { telephony.listen(l, PhoneStateListener.LISTEN_CALL_STATE); listener = l }
        }
    }

    @Suppress("DEPRECATION")
    private fun stopListening() {
        val l = listener ?: return
        listener = null
        if (Build.VERSION.SDK_INT >= 31 && l is TelephonyCallback) runCatching { telephony.unregisterTelephonyCallback(l) }
        if (l is PhoneStateListener) runCatching { telephony.listen(l, PhoneStateListener.LISTEN_NONE) }
    }

    private fun phoneState(state: Int) {
        when (state) {
            TelephonyManager.CALL_STATE_OFFHOOK -> for (c in calls.values.toList()) {
                c.offHook = true
                // No finer state without the role: off-hook counts as connected (WM's timer starts).
                if (!c.owned && c.state == "dialing") state(c.id, "active")
            }
            TelephonyManager.CALL_STATE_IDLE -> for (c in calls.values.toList()) {
                // Registering reports the current state, which can still be idle just after placeCall.
                if (!c.offHook && !c.stale()) continue
                // Owned calls end through the call process; this is the backstop if that went missing.
                if (!c.owned || link == null) state(c.id, "ended", "idle")
            }
        }
    }

    /** Back on screen: calls that ended while the emulator was paused end in WM too. */
    @SuppressLint("MissingPermission")
    private fun resync() {
        if (calls.isEmpty() || ctx.checkSelfPermission(Manifest.permission.READ_PHONE_STATE) != PackageManager.PERMISSION_GRANTED) return
        val inCall = runCatching { telecom.isInCall }.getOrDefault(true)
        if (!inCall) for (c in calls.values.toList()) state(c.id, "ended", "idle")
    }

    // ---- While a call is up ----

    /** The mailbox is read every 250 ms during calls (the activity's own tick is 1 s). */
    private val fastPoll = object : Runnable {
        override fun run() {
            guest.poll()
            pollModem()
            // A call that never went off-hook (refused, no service) ends in WM too.
            if (calls.values.any { it.stale() }) resync()
            ui.postDelayed(this, 250)
        }
    }

    private fun updateFastPoll() {
        ui.removeCallbacks(fastPoll)
        if (foreground && calls.isNotEmpty()) ui.postDelayed(fastPoll, 250)
    }

    private var proximity: PowerManager.WakeLock? = null

    /** With WM as the call screen, WMDroid turns the screen off at the ear, as a phone app does. */
    private fun updateWakeLock() {
        val want = foreground && calls.values.any { it.owned && it.state != "ended" }
        val pm = ctx.getSystemService(PowerManager::class.java) ?: return
        if (want && proximity == null && pm.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK)) {
            proximity = pm.newWakeLock(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK, "WMDroid:call").apply {
                setReferenceCounted(false)
                acquire(3 * 60 * 60 * 1000L)
            }
        } else if (!want) {
            proximity?.let { runCatching { it.release() } }
            proximity = null
        }
    }

    // ---- Helpers ----

    private fun isEmergency(number: String): Boolean {
        val n = number.filter { it.isDigit() || it == '+' }
        val known = runCatching { telephony.isEmergencyNumber(n) }.getOrNull()
        return known ?: (n in setOf("911", "112", "999", "000", "110", "119", "118", "08", "100", "101", "102", "103"))
    }

    private fun openSystemDialer(number: String) {
        val intent = Intent(Intent.ACTION_DIAL, Uri.fromParts("tel", number, null)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        telecom.systemDialerPackage?.let { intent.setPackage(it) }
        runCatching { ctx.startActivity(intent) }.onFailure { Log.w(TAG, "no dialer", it) }
    }

    /** FakeRIL was loaded after boot: WM's phone stack needs one restart to see it. */
    private fun askRestart() {
        if (restartAsked || !enabled(ctx) || activity.isFinishing) return
        restartAsked = true
        MaterialAlertDialogBuilder(activity)
            .setTitle("Turn on Windows Mobile's phone")
            .setMessage("Windows Mobile restarts once so its Phone app can place calls through this phone. " +
                "Your programs and data stay; open programs close.")
            .setPositiveButton("Restart") { _, _ -> guest.send("patch") }
            .setNegativeButton("Later", null)
            .show()
    }
}
