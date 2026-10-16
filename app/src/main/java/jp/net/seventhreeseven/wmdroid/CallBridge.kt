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
 * Incoming calls ring in Windows Mobile while the emulator is on screen ("call ring <number>"; the
 * helper makes FakeRIL raise the call, and cprog shows its own incoming-call screen). WM's Answer /
 * Ignore / End answer, reject or end the real call; answering or ending it on the phone answers or
 * ends WM's. With the dialer role the call stays in WmInCallService (no Android screen); without it
 * the phone's dialer shows the call too and WM follows the phone state. "Use Phone's Ringer" picks
 * which ringtone sounds: Windows Mobile's (the phone's ringer is silenced for that call) or the
 * phone's (Windows Mobile's sound is muted while it rings).
 *
 * Call audio never passes through the emulator: it stays on the phone's earpiece, speaker or headset.
 * docs/research-phone-calls-pim.md, sections E and F.
 */
class CallBridge(private val activity: Activity, private val guest: GuestBridge, private val handle: () -> Long = { 0L }) {
    private val ctx: Context = activity
    private val ui = Handler(Looper.getMainLooper())
    private val telecom = ctx.getSystemService(TelecomManager::class.java)
    private val telephony = ctx.getSystemService(TelephonyManager::class.java)
    private val audio = ctx.getSystemService(AudioManager::class.java)

    private class WmCall(val id: Int, val number: String, val token: String, var owned: Boolean, val incoming: Boolean = false) {
        var state = if (incoming) "ringing" else "dialing"
        val placedAt = SystemClock.uptimeMillis()
        /** The phone went off-hook since this call was placed (an idle state before that is stale). */
        var offHook = false
        fun stale() = !incoming && !offHook && SystemClock.uptimeMillis() - placedAt > 30_000
    }

    /** Windows Mobile's calls by its call id. */
    private val calls = HashMap<Int, WmCall>()

    /**
     * The phone rings and WM was asked to ring too ("call ring"), until the helper reports WM's call
     * ("call ringing <id>"). token: the call in WmInCallService (dialer role), or null.
     */
    private class Ring(val number: String, val token: String?) {
        /** The real call was answered or ended before WM rang: WM's call ends as soon as it appears. */
        var gone = false
    }
    private var ring: Ring? = null
    /**
     * The helper's RIL client is up on a FakeRIL image that WM's phone knows ("call ready" without
     * restart); null: not heard since the app started (a restored session's helper does not repeat it).
     */
    private var callsReady: Boolean? = null
    /** WM's sound is muted while it rings ("Use Phone's Ringer" off). */
    private var hushed = false
    /** The phone's ring stream was muted for WM's ringtone (no dialer role, "Use Phone's Ringer" on). */
    private var ringMuted = false
    private var lastShowing: Boolean? = null
    /** The modem event "smssend <mr> <pdu>" (SmsBridge). */
    var onModemSms: ((Int, String) -> Unit)? = null
    /** Wakes a session kept in memory for an incoming call (EmulatorActivity.wakeForEvent). */
    var wake: ((Long) -> Boolean)? = null
    /** Windows Mobile over other apps while a call rings in the background (CallOverlay). */
    var overlay: CallOverlay? = null
    /** The guest's size and whether the frame is turned (fold mode), for the overlay. */
    var guestFrame: (() -> Triple<Int, Int, Boolean>)? = null
    /** Brings the emulator to the front (an incoming call answered from the overlay). */
    var bringFront: (() -> Unit)? = null
    /** Gives the guest's picture back to the activity's own surface (after the overlay had it). */
    var reattach: (() -> Unit)? = null

    init {
        // The app ended while WM rang with the phone's ringer muted: give the ringer back.
        if (ringState(ctx).getBoolean(RING_MUTED, false)) { ringMuted = true; muteRingStream(false) }
    }
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
        private const val PREF_RINGER = "calls.wmRinger"
        /** How long WM may take to ring (the helper's poll, FakeRIL's 1 s, the mailbox) before the phone keeps the call. */
        private const val RING_BUDGET = 4500L
        private const val CALL_CHANNEL = "wm.call"
        private const val CALL_NOTE = 0x7C41
        /** EmulatorActivity opened from the full-screen call notification: show over the lock screen. */
        const val EXTRA_CALL = "jp.net.seventhreeseven.wmdroid.INCOMING_CALL"

        fun enabled(ctx: Context) = Settings.global(ctx).getBoolean(PREF, false)
        fun setEnabled(ctx: Context, on: Boolean) =
            Settings.global(ctx).edit().putBoolean(PREF, on).apply()

        /**
         * "Use Phone's Ringer": on, Windows Mobile's Phone rings with its own ringtone (on the ringer
         * volume) and this phone's ringtone is silenced for that call; off, this phone rings as usual
         * and Windows Mobile stays quiet while it rings.
         */
        fun wmRinger(ctx: Context) = Settings.global(ctx).getBoolean(PREF_RINGER, false)
        fun setWmRinger(ctx: Context, on: Boolean) =
            Settings.global(ctx).edit().putBoolean(PREF_RINGER, on).apply()

        /** Not per OS: the phone's ring stream muted for a call, to unmute if the app died during it. */
        private fun ringState(ctx: Context) = ctx.getSharedPreferences("wmdroid.calls", Context.MODE_PRIVATE)
        private const val RING_MUTED = "ringStreamMuted"

        /** WMDroid is the default phone app, so WM's call screen can be the only one. */
        fun holdsRole(ctx: Context): Boolean =
            if (Build.VERSION.SDK_INT >= 29)
                ctx.getSystemService(RoleManager::class.java)?.isRoleHeld(RoleManager.ROLE_DIALER) == true
            else  // Android 8/9: no roles; the default dialer app is the same thing.
                ctx.getSystemService(TelecomManager::class.java)?.defaultDialerPackage == ctx.packageName

        fun roleRequest(ctx: Context): Intent? =
            if (Build.VERSION.SDK_INT >= 29) ctx.getSystemService(RoleManager::class.java)
                ?.takeIf { it.isRoleAvailable(RoleManager.ROLE_DIALER) }?.createRequestRoleIntent(RoleManager.ROLE_DIALER)
            else Intent(TelecomManager.ACTION_CHANGE_DEFAULT_DIALER)
                .putExtra(TelecomManager.EXTRA_CHANGE_DEFAULT_DIALER_PACKAGE_NAME, ctx.packageName)

        /**
         * Permissions placing, answering and ending calls need (the dialer role grants them too);
         * READ_CALL_LOG gives the incoming caller's number without the role.
         */
        fun permissions(): Array<String> = arrayOf(Manifest.permission.CALL_PHONE, Manifest.permission.ANSWER_PHONE_CALLS,
            Manifest.permission.READ_PHONE_STATE, PhoneNumber.PERMISSION, Manifest.permission.READ_CALL_LOG)

        fun hasCallPermission(ctx: Context) =
            ctx.checkSelfPermission(Manifest.permission.CALL_PHONE) == PackageManager.PERMISSION_GRANTED

        /** Lines for the helper's state.ini (GuestBridge.writeState). */
        fun stateLines(ctx: Context): String =
            (if (enabled(ctx)) "calls=1\npatch.fakeril=on\n" else "calls=0\n") +
                (if (SmsBridge.enabled(ctx)) "sms=1\n" else "sms=0\n")  // texts on WM 6.5.3 (Sms.cs)
    }

    // ---- Lifecycle (EmulatorActivity) ----

    fun resume() {
        foreground = true
        if (overlay?.showing == true) { overlay?.hide(); reattach?.invoke() }  // opened while WM rang over other apps
        val on = enabled(ctx)
        if (lastEnabled != null && lastEnabled != on) guest.writeState()  // the helper re-reads it every 5 s
        lastEnabled = on
        if (on && holdsRole(ctx)) bindLink()
        resync()
        updateShowing()
        updateListening()
        updateFastPoll()
    }

    fun pause() {
        foreground = false
        updateShowing()
        updateListening()
        updateFastPoll()
        updateWakeLock()
    }

    /**
     * The app leaves the screen: Windows Mobile can no longer show its calls, so calls held through
     * the dialer role go to the phone's dialer (without the role they are there already).
     */
    fun leave() {
        foreground = false
        updateShowing()
        if (calls.values.any { it.owned } || ring?.token != null) {
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
        ui.removeCallbacks(ringTimeout)
        endRingerOverrides()
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
            if (p[0] == "smssend") { onModemSms?.invoke(p.getOrNull(1)?.toIntOrNull() ?: continue, p.getOrNull(2) ?: continue); continue }
            if (p[0] == "dial" || p[0] == "ringing") p.getOrNull(1)?.toIntOrNull()?.let { modemIds.add(it) }
            onGuest(line)
            if (p[0] == "end") p.getOrNull(1)?.toIntOrNull()?.let { modemIds.remove(it) }
        }
    }

    /** A "call ..." line from the helper, without the "call " prefix. */
    fun onGuest(line: String) {
        val p = line.split(' ')
        when (p[0]) {
            "dial" -> if (p.size >= 3) dial(p[1].toIntOrNull() ?: return, p[2])
            "ringing" -> ringing(p.getOrNull(1)?.toIntOrNull() ?: return)
            "noring" -> ringFailed(p.getOrNull(1) ?: "")
            "answer" -> calls[p.getOrNull(1)?.toIntOrNull() ?: return]?.let { wmAnswered(it) }
            "end" -> calls[p.getOrNull(1)?.toIntOrNull() ?: return]?.let { wmEnded(it) }
            "hold" -> calls[p.getOrNull(1)?.toIntOrNull() ?: return]?.let { c ->
                if (c.owned) sendLink(Message.obtain(null, CallLink.HOLD, if (p.getOrNull(2) == "1") 1 else 0, 0).withToken(c.token))
            }
            "mute" -> mute(p.getOrNull(1) == "1")
            "speaker" -> speaker(p.getOrNull(1) == "1")
            "ready" -> {
                // "ready modem": rilgsm images, whose calls go through the virtual modem (it rings too).
                callsReady = !line.contains("restart")
                updateShowing()
                if (line.contains("restart")) askRestart()
            }
            "off" -> { callsReady = false; updateShowing() }
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

    /** Without the role the phone's in-call screen comes up over us: bring Windows Mobile back once it has. */
    private fun bringBack(id: Int) {
        ui.postDelayed({
            if (!activity.isFinishing && calls.containsKey(id)) runCatching {
                activity.startActivity(Intent(activity, activity.javaClass)
                    .addFlags(Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP))
            }
        }, 1500)
    }

    // ---- Incoming calls ----

    /** Windows Mobile can ring for an incoming call now: on screen, calls on, its phone up. */
    private fun canRing() = (foreground || CallOverlay.enabled(ctx)) && enabled(ctx) && callsReady != false && handle() != 0L

    /** Tells the call process (dialer role) whether to keep incoming calls for Windows Mobile. */
    private fun updateShowing() {
        val on = canRing()
        if (on == lastShowing || link == null) return
        lastShowing = on
        sendLink(Message.obtain(null, CallLink.SHOWING, if (on) 1 else 0, 0))
    }

    /** The phone rings: ring Windows Mobile too. token: the call held by WmInCallService (dialer role). */
    private fun startRing(number: String, token: String?) {
        if (ring != null || calls.isNotEmpty() || !canRing()) {
            Log.i(TAG, "incoming call: Windows Mobile cannot ring now")
            token?.let { sendLink(Message.obtain(null, CallLink.RELEASE).withToken(it)) }
            return
        }
        if (!foreground && !showInBackground()) {
            Log.i(TAG, "incoming call: Windows Mobile is not in memory; the phone keeps the call")
            token?.let { sendLink(Message.obtain(null, CallLink.RELEASE).withToken(it)) }
            return
        }
        val n = number.filter { it.isDigit() || it == '+' || it == '*' || it == '#' }.ifEmpty { "-" }
        Log.i(TAG, "incoming call: ringing Windows Mobile (${if (token != null) "dialer role" else "phone state"})")
        ring = Ring(n, token)
        // The phone's ringer keeps the call: WM's ring stays quiet from its first note.
        if (!wmRinger(ctx)) hushGuest(true)
        // WM 6 / 6.1 / 6.1.4: the virtual modem rings (RING, +CLIP); WM 6.5.3: the helper has FakeRIL ring.
        val h = handle()
        if (NativeBridge.modemActive(h)) NativeBridge.modemRing(h, n) else guest.send("call ring $n")
        ui.removeCallbacks(ringTimeout)
        ui.postDelayed(ringTimeout, RING_BUDGET)
        updateListening()
        updateFastPoll()
    }

    /**
     * In the background: the session wakes at full speed (EmulatorActivity.wakeForEvent) and Windows
     * Mobile's screen comes up over other apps, or, with the phone locked, through a full-screen call
     * notification that opens the emulator over the lock screen. False if no session is in memory.
     */
    private fun showInBackground(): Boolean {
        if (wake?.invoke(120_000) != true) return false
        val pm = ctx.getSystemService(PowerManager::class.java)
        val km = ctx.getSystemService(android.app.KeyguardManager::class.java)
        val locked = pm?.isInteractive == false || km?.isKeyguardLocked == true
        if (!locked) {
            val f = guestFrame?.invoke() ?: return false
            if (overlay?.show(handle(), f.first, f.second, f.third) == true) return true
        }
        return fullScreenCall()
    }

    private fun fullScreenCall(): Boolean {
        if (Build.VERSION.SDK_INT >= 33 && ctx.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) return false
        val nm = ctx.getSystemService(android.app.NotificationManager::class.java) ?: return false
        if (nm.getNotificationChannel(CALL_CHANNEL) == null)
            nm.createNotificationChannel(android.app.NotificationChannel(CALL_CHANNEL, "Incoming calls in Windows Mobile",
                android.app.NotificationManager.IMPORTANCE_HIGH).apply { setSound(null, null) })
        val open = android.app.PendingIntent.getActivity(ctx, 3, Intent(ctx, activity.javaClass).putExtra(EXTRA_CALL, true)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_REORDER_TO_FRONT or Intent.FLAG_ACTIVITY_SINGLE_TOP),
            android.app.PendingIntent.FLAG_IMMUTABLE or android.app.PendingIntent.FLAG_UPDATE_CURRENT)
        nm.notify(CALL_NOTE, android.app.Notification.Builder(ctx, CALL_CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_running)
            .setContentTitle("Incoming call")
            .setContentText("Windows Mobile is ringing")
            .setCategory(android.app.Notification.CATEGORY_CALL)
            .setFullScreenIntent(open, true)
            .setContentIntent(open)
            .setOngoing(true)
            .build())
        return true
    }

    /** The ringing is over: the overlay or the call notification goes; answered means the full emulator. */
    private fun endRingUi(answered: Boolean) {
        val wasShown = overlay?.showing == true
        overlay?.hide()
        ctx.getSystemService(android.app.NotificationManager::class.java)?.cancel(CALL_NOTE)
        if (answered && !foreground && wasShown) bringFront?.invoke()
    }

    /** WM did not ring in time (helper busy or gone): the phone handles the call as usual. */
    private val ringTimeout = Runnable { ringFailed("timeout") }

    private fun ringFailed(why: String) {
        val r = ring ?: return
        Log.i(TAG, "incoming call: Windows Mobile cannot ring ($why)")
        ring = null
        ui.removeCallbacks(ringTimeout)
        r.token?.let { sendLink(Message.obtain(null, CallLink.RELEASE).withToken(it)) }
        endRingerOverrides()
        endRingUi(false)
        finishCalls()
    }

    /** WM rings for the call ("call ringing <id>"): it is WM's call now. */
    private fun ringing(id: Int) {
        val r = ring
        ring = null
        ui.removeCallbacks(ringTimeout)
        if (r == null || r.gone) {
            // The real call was answered elsewhere or ended meanwhile (or WM was too late).
            guest.send("call state $id ended missed")
            endRingerOverrides()
            finishCalls()
            return
        }
        val c = WmCall(id, r.number, r.token ?: "", r.token != null, incoming = true)
        calls[id] = c
        state(id, "ringing")
        r.token?.let { sendLink(Message.obtain(null, CallLink.KEEP).withToken(it)) }
        if (wmRinger(ctx)) silencePhoneRinger(c)
        updateListening()
        updateFastPoll()
    }

    /** WM's Answer: answer the real call. */
    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    private fun wmAnswered(c: WmCall) {
        if (!c.incoming || c.state != "ringing") return  // our own answer coming back
        Log.i(TAG, "answered in Windows Mobile")
        // WM's call is active already (its RIL answered): only our side changes.
        c.state = "active"
        endRingerOverrides()
        endRingUi(true)
        updateWakeLock()
        if (c.owned) sendLink(Message.obtain(null, CallLink.ANSWER).withToken(c.token))
        else {
            runCatching { telecom.acceptRingingCall() }.onFailure { Log.w(TAG, "acceptRingingCall failed", it) }
            bringBack(c.id)
        }
    }

    /** "Use Phone's Ringer" on: the phone's own ringtone stops for this call; WM's plays as a ringtone. */
    private fun silencePhoneRinger(c: WmCall) {
        val h = handle()
        if (c.owned || holdsRole(ctx)) {
            // The default dialer may silence Telecom's ringer.
            runCatching { telecom.silenceRinger() }.onFailure { Log.w(TAG, "silenceRinger failed", it) }
            if (h != 0L) NativeBridge.setAudioUsage(h, 6)
        } else {
            // Without the role only the ring volume can do it; WM's ring then plays on the media volume.
            muteRingStream(true)
            if (h != 0L) NativeBridge.setAudioUsage(h, if (ringMuted) 1 else 6)
        }
    }

    private fun muteRingStream(on: Boolean) {
        if (on) {
            if (ringMuted || audio.isStreamMute(AudioManager.STREAM_RING)) return
            try {
                audio.adjustStreamVolume(AudioManager.STREAM_RING, AudioManager.ADJUST_MUTE, 0)
                ringMuted = true
                ringState(ctx).edit().putBoolean(RING_MUTED, true).apply()
            } catch (e: SecurityException) {
                Log.w(TAG, "cannot mute the phone's ringer", e)
            }
        } else if (ringMuted) {
            ringMuted = false
            runCatching { audio.adjustStreamVolume(AudioManager.STREAM_RING, AudioManager.ADJUST_UNMUTE, 0) }
                .onFailure { Log.w(TAG, "cannot unmute the phone's ringer", it) }
            ringState(ctx).edit().remove(RING_MUTED).apply()
        }
    }

    /** "Use Phone's Ringer" off: WM's sound is muted while it rings (the phone's ringer sounds). */
    private fun hushGuest(on: Boolean) {
        if (on == hushed) return
        hushed = on
        val h = handle()
        if (h == 0L) return
        NativeBridge.setAudioGain(h, if (on) 0f else Math.pow(10.0, Settings(ctx).audioBoostDb / 20.0).toFloat())
    }

    /** The ringing is over (answered, ended, never rang): the phone's and WM's sound as they were. */
    private fun endRingerOverrides() {
        hushGuest(false)
        muteRingStream(false)
    }

    /** The usage for the guest's sound ("audio ring|media|alert" from the helper). */
    fun audioUsage(kind: String): Int = when (kind) {
        "ring" -> if (ringMuted) 1 else 6  // the ring stream is muted for this call: media volume
        "alert" -> 5
        else -> 1
    }

    @SuppressLint("MissingPermission")
    private fun defaultAccount() = runCatching {
        telecom.getDefaultOutgoingPhoneAccount("tel") ?: telecom.callCapablePhoneAccounts.singleOrNull()
    }.getOrNull()

    /** WM's call is gone (its End, or the hang-up we asked for): end the phone's call too. */
    @SuppressLint("MissingPermission")
    private fun wmEnded(c: WmCall) {
        calls.remove(c.id)
        if (c.incoming) endRingUi(false)
        if (c.state == "ended") return finishCalls()
        // A ringing call ended in WM is WM's Ignore: the real call is rejected.
        if (c.owned) sendLink(Message.obtain(null, if (c.state == "ringing") CallLink.REJECT else CallLink.DISCONNECT).withToken(c.token))
        else endCallWithoutRole()
        finishCalls()
    }

    /** Without the role: the deprecated, still documented TelecomManager.endCall (ANSWER_PHONE_CALLS; rejects a ringing call). */
    @Suppress("DEPRECATION")
    @SuppressLint("MissingPermission")
    private fun endCallWithoutRole() {
        // API 28; Android 8.x has no public way to end a call it does not own (the call stays up).
        if (Build.VERSION.SDK_INT < 28) { Log.w(TAG, "endCall needs Android 9"); return }
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
        val was = calls[id]?.state
        calls[id]?.state = s
        if (id in modemIds) {
            if (s == "active" && was == "ringing" && calls[id]?.incoming == true) {
                // Answered on the phone: the helper answers through WM's RIL, as WM's Answer does (the
                // call screen then keeps the caller's number); the modem connects it itself if not.
                guest.send("call answer")
                ui.postDelayed({ if (id in modemIds) NativeBridge.modemCallState(handle(), id, "active") }, 3000)
            } else NativeBridge.modemCallState(handle(), id, s)
            if (s == "ended") modemIds.remove(id)
        } else {
            guest.send("call state $id $s${if (cause.isNotEmpty()) " $cause" else ""}")
        }
        if (s != "ringing" && calls[id]?.incoming == true) { endRingerOverrides(); endRingUi(s == "active") }
        if (s == "ended") { calls.remove(id); finishCalls() } else updateWakeLock()
    }

    private fun finishCalls() {
        if (calls.isNotEmpty() || ring != null) return
        endRingerOverrides()
        updateListening()
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
                    val s = m.data.getString(CallLink.KEY_STATE) ?: return
                    val r = ring
                    if (r != null && r.token == token) {
                        // Answered (a headset) or gone before WM rang: WM's call ends as it appears.
                        if (s == "active" || s == "ended") r.gone = true
                        if (s == "active") sendLink(Message.obtain(null, CallLink.RELEASE).withToken(token))
                        return
                    }
                    val c = calls.values.firstOrNull { it.token == token } ?: return
                    if (c.incoming && (s == "dialing" || s == "ringing")) return
                    if (s != c.state) state(c.id, s, m.data.getString(CallLink.KEY_CAUSE) ?: "")
                }
                CallLink.RINGING -> startRing(m.data?.getString(CallLink.KEY_NUMBER) ?: "", CallLink.token(m) ?: return)
                CallLink.HANDED_OFF -> handedOff()
            }
        }
    })

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
            link = Messenger(binder)
            sendLink(Message.obtain(null, CallLink.HELLO).apply { replyTo = replies })
            lastShowing = null
            updateShowing()
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
        ring?.takeIf { it.token != null }?.gone = true
        for (c in calls.values.filter { it.owned }.toList()) state(c.id, "ended", "handoff")
    }

    // ---- Phone state (calls without the role, and a backstop for the rest) ----

    private var listener: Any? = null

    /**
     * The phone state is followed during WM's calls, and while the emulator is on screen with calls
     * on and no dialer role (incoming calls; with the role WmInCallService reports them).
     */
    private fun updateListening() {
        if (calls.isNotEmpty() || ring != null || ((foreground || CallOverlay.enabled(ctx)) && enabled(ctx) && !holdsRole(ctx))) startListening()
        else stopListening()
    }

    @SuppressLint("MissingPermission")
    private fun startListening() {
        if (listener != null || ctx.checkSelfPermission(Manifest.permission.READ_PHONE_STATE) != PackageManager.PERMISSION_GRANTED) return
        // Only the old listener tells the incoming caller's number (with READ_CALL_LOG).
        val withNumber = ctx.checkSelfPermission(Manifest.permission.READ_CALL_LOG) == PackageManager.PERMISSION_GRANTED
        if (Build.VERSION.SDK_INT >= 31 && !withNumber) {
            val cb = object : TelephonyCallback(), TelephonyCallback.CallStateListener {
                override fun onCallStateChanged(state: Int) {
                    ui.post { phoneState(state, "") }
                }
            }
            runCatching { telephony.registerTelephonyCallback(Executor { it.run() }, cb); listener = cb }
        } else {
            @Suppress("DEPRECATION")
            val l = object : PhoneStateListener() {
                @Deprecated("Deprecated in Java")
                override fun onCallStateChanged(state: Int, phoneNumber: String?) = phoneState(state, phoneNumber.orEmpty())
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

    private fun phoneState(state: Int, number: String) {
        when (state) {
            // Without the role: an incoming call rings in WM too (with the role WmInCallService reports it).
            TelephonyManager.CALL_STATE_RINGING ->
                if (!holdsRole(ctx) && ring == null && calls.isEmpty()) startRing(number, null)
            TelephonyManager.CALL_STATE_OFFHOOK -> {
                ring?.takeIf { it.token == null }?.gone = true  // answered before WM rang
                for (c in calls.values.toList()) {
                    c.offHook = true
                    // No finer state without the role: off-hook counts as connected (WM's timer starts).
                    if (!c.owned && (c.state == "dialing" || c.state == "ringing")) state(c.id, "active")
                }
            }
            TelephonyManager.CALL_STATE_IDLE -> {
                ring?.takeIf { it.token == null }?.gone = true  // the caller gave up before WM rang
                for (c in calls.values.toList()) {
                    // Registering reports the current state, which can still be idle just after placeCall.
                    if (!c.incoming && !c.offHook && !c.stale()) continue
                    // Owned calls end through the call process; this is the backstop if that went missing.
                    if (!c.owned || link == null) state(c.id, "ended", if (c.state == "ringing") "missed" else "idle")
                }
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
        // Also in the background while WM rings or shows a call (the activity's own tick has stopped).
        if (calls.isNotEmpty() || ring != null) ui.postDelayed(fastPoll, 250)
    }

    private var proximity: PowerManager.WakeLock? = null

    /** With WM as the call screen, WMDroid turns the screen off at the ear, as a phone app does. */
    private fun updateWakeLock() {
        val want = foreground && calls.values.any { it.owned && it.state != "ended" && it.state != "ringing" }
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
        @Suppress("DEPRECATION")
        val known = if (Build.VERSION.SDK_INT >= 29) runCatching { telephony.isEmergencyNumber(n) }.getOrNull()
            else runCatching { android.telephony.PhoneNumberUtils.isEmergencyNumber(n) }.getOrNull()
        return known ?: (n in setOf("911", "112", "999", "000", "110", "119", "118", "08", "100", "101", "102", "103"))
    }

    private fun openSystemDialer(number: String) {
        val intent = Intent(Intent.ACTION_DIAL, Uri.fromParts("tel", number, null)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        DialActivity.systemDialerPackage(ctx)?.let { intent.setPackage(it) }
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
