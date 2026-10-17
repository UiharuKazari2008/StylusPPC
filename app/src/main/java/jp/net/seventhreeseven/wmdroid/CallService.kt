package jp.net.seventhreeseven.wmdroid

import android.app.Service
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.Message
import android.os.Messenger
import android.os.Process
import android.os.RemoteException
import android.telecom.Call
import android.telecom.CallAudioState
import android.telecom.DisconnectCause
import android.telecom.InCallService
import android.telecom.TelecomManager
import android.telecom.VideoProfile
import android.util.Log

/*
 * Phone calls, the part that runs in its own process (":call", see AndroidManifest.xml).
 *
 * With WMDroid holding the dialer role, Telecom binds WmInCallService for every call. Calls that
 * Windows Mobile placed (and that the running emulator can show) stay here: Windows Mobile's own
 * call screen is the UI, and its End / Mute / Speaker / Hold reach the real call through this
 * service. So do incoming calls while the emulator is on screen: Windows Mobile rings, and its
 * Answer / Ignore / End act on the real call. Every other call, and any WM call once the emulator
 * cannot show it, is handed to the
 * phone's own dialer by ending this process: Telecom then connects the system dialer's in-call UI
 * to the ongoing calls (AOSP InCallController: when the default dialer's InCallService dies, its
 * EmergencyInCallServiceConnection switches to the system dialer). The same happens by itself if
 * this process crashes, so a WMDroid fault never leaves a call without a screen.
 *
 * The emulator (main process, CallBridge) talks to this process through CallLinkService with
 * Messengers; when the main process dies during a WM call, this process hands the call off.
 * docs/research-phone-calls-pim.md, section E.
 */

/** Messages between CallBridge (main process) and the call process. */
object CallLink {
    // main -> call
    const val HELLO = 1          // replyTo = the main process's Messenger
    const val DISCONNECT = 2     // data: token
    const val MUTE = 3           // arg1: 0/1
    const val SPEAKER = 4        // arg1: 0/1
    const val HOLD = 5           // data: token, arg1: 0/1
    const val HANDOFF = 6        // give every call to the phone's dialer
    const val DTMF = 7           // data: token, arg1: the character
    const val PLACING = 8        // data: token, number (sent just before TelecomManager.placeCall)
    const val ANSWER = 9         // data: token (an incoming call: WM's Answer)
    const val REJECT = 10        // data: token (an incoming call: WM's Ignore)
    const val KEEP = 11          // data: token (Windows Mobile rings for this incoming call)
    const val RELEASE = 12       // data: token (Windows Mobile cannot ring: the phone's dialer takes it)
    const val SHOWING = 13       // arg1: 1 while the emulator can ring and show incoming calls
    const val ROUTE = 14         // arg1: CallAudioState route, data: Bluetooth address when it is one device
    // call -> main
    const val STATE = 101        // data: token, state, cause
    const val AUDIO = 102        // arg1: muted, arg2: CallAudioState route
    const val HANDED_OFF = 103   // the call process is about to end
    const val RINGING = 104      // data: token, number ("" when withheld): an incoming call to ring in WM

    const val KEY_TOKEN = "jp.net.seventhreeseven.wmdroid.WM_CALL"
    const val KEY_STATE = "state"
    const val KEY_CAUSE = "cause"
    const val KEY_NUMBER = "number"
    const val KEY_ADDRESS = "address"
    const val KEY_CONNECT = "connect"   // connectTimeMillis, with an active or held call

    fun token(m: Message): String? = m.data?.getString(KEY_TOKEN)
}

/** The in-call service Telecom binds while WMDroid is the default dialer. */
class WmInCallService : InCallService() {
    private val main = Handler(Looper.getMainLooper())
    /** Calls Windows Mobile shows, by WM token. */
    private val owned = HashMap<String, Call>()

    override fun onCreate() {
        super.onCreate()
        instance = this
    }

    override fun onDestroy() {
        if (instance === this) instance = null
        super.onDestroy()
    }

    // Call.Details.getState is API 31 and getCallDirection API 29; Android 8-11 use Call.getState,
    // where a call added while ringing is the incoming one.
    @Suppress("DEPRECATION")
    private fun stateOf(call: Call): Int =
        if (android.os.Build.VERSION.SDK_INT >= 31) call.details.state else call.state

    private fun isIncoming(call: Call): Boolean =
        if (android.os.Build.VERSION.SDK_INT >= 29) call.details.callDirection == Call.Details.DIRECTION_INCOMING
        else stateOf(call) == Call.STATE_RINGING

    private fun isOutgoing(call: Call): Boolean =
        if (android.os.Build.VERSION.SDK_INT >= 29) call.details.callDirection == Call.Details.DIRECTION_OUTGOING
        else !isIncoming(call)

    override fun onCallAdded(call: Call) {
        super.onCallAdded(call)
        if (isIncoming(call)) {
            incoming(call)
            return
        }
        val token = tokenOf(call)
        val link = CallLinkService.peer
        Log.i(TAG, "call added: token=$token linked=${link != null} state=${stateOf(call)}")
        if (token == null || link == null || !isOutgoing(call)) {
            // Not Windows Mobile's (another app) or no emulator to show it.
            handOff("not a Windows Mobile call")
            return
        }
        owned[token] = call
        call.registerCallback(callback)
        report(call)
    }

    /** The token of the incoming call waiting for Windows Mobile to ring (KEEP). */
    private var keepToken: String? = null
    private val keepTimeout = Runnable { handOff("Windows Mobile did not ring in time") }

    /**
     * An incoming call rings in Windows Mobile when it comes in alone while the emulator shows
     * Windows Mobile; otherwise (or if WM does not ring within a few seconds) the phone's dialer
     * takes it. Telecom plays the phone's ringtone meanwhile (this service has no
     * IN_CALL_SERVICE_RINGING), and the main process silences it when Windows Mobile rings instead.
     */
    private fun incoming(call: Call) {
        val linked = CallLinkService.peer != null
        Log.i(TAG, "incoming call: linked=$linked showing=${CallLinkService.showing} calls=${calls.size} state=${stateOf(call)}")
        if (!linked || !CallLinkService.showing || owned.isNotEmpty() || calls.size > 1) {
            handOff("incoming call Windows Mobile cannot show")
            return
        }
        val token = "in-${System.currentTimeMillis()}"
        owned[token] = call
        call.registerCallback(callback)
        val number = if (call.details.handlePresentation == TelecomManager.PRESENTATION_ALLOWED)
            call.details.handle?.schemeSpecificPart.orEmpty() else ""
        keepToken = token
        main.postDelayed(keepTimeout, 6000)
        CallLinkService.send(Message.obtain(null, CallLink.RINGING).apply {
            data = Bundle().apply {
                putString(CallLink.KEY_TOKEN, token)
                putString(CallLink.KEY_NUMBER, number)
            }
        })
    }

    override fun onCallRemoved(call: Call) {
        call.unregisterCallback(callback)
        if (keepToken != null && owned[keepToken] == call) {
            main.removeCallbacks(keepTimeout)
            keepToken = null
        }
        owned.entries.removeAll { it.value == call }
        super.onCallRemoved(call)
    }

    override fun onCallAudioStateChanged(audioState: CallAudioState) {
        CallLinkService.send(Message.obtain(null, CallLink.AUDIO, if (audioState.isMuted) 1 else 0, audioState.route))
    }

    private val callback = object : Call.Callback() {
        override fun onStateChanged(call: Call, state: Int) = report(call)
    }

    private fun report(call: Call) {
        val token = owned.entries.firstOrNull { it.value == call }?.key ?: return
        val state = when (stateOf(call)) {
            Call.STATE_NEW, Call.STATE_CONNECTING, Call.STATE_DIALING, Call.STATE_PULLING_CALL -> "dialing"
            Call.STATE_RINGING -> "ringing"
            Call.STATE_ACTIVE -> "active"
            Call.STATE_HOLDING -> "held"
            Call.STATE_DISCONNECTING -> return
            Call.STATE_DISCONNECTED -> "ended"
            Call.STATE_SELECT_PHONE_ACCOUNT -> {
                // Choosing a SIM needs a screen Windows Mobile does not have: the phone's dialer asks.
                handOff("SIM choice")
                return
            }
            else -> return
        }
        val cause = if (state == "ended") causeName(call.details.disconnectCause) else ""
        CallLinkService.send(Message.obtain(null, CallLink.STATE).apply {
            data = Bundle().apply {
                putString(CallLink.KEY_TOKEN, token)
                putString(CallLink.KEY_STATE, state)
                putString(CallLink.KEY_CAUSE, cause)
                if (state == "active" || state == "held") putLong(CallLink.KEY_CONNECT, call.details.connectTimeMillis)
            }
        })
    }

    /** Commands from Windows Mobile (CallLinkService, main thread). */
    fun command(m: Message) {
        when (m.what) {
            CallLink.DISCONNECT -> CallLink.token(m)?.let { owned[it] }?.disconnect()
            CallLink.MUTE -> setMuted(m.arg1 != 0)
            CallLink.SPEAKER -> setAudioRoute(if (m.arg1 != 0) CallAudioState.ROUTE_SPEAKER else CallAudioState.ROUTE_WIRED_OR_EARPIECE)
            CallLink.ROUTE -> routeAudio(m.arg1, m.data?.getString(CallLink.KEY_ADDRESS))
            CallLink.HOLD -> CallLink.token(m)?.let { owned[it] }?.let { if (m.arg1 != 0) it.hold() else it.unhold() }
            CallLink.DTMF -> CallLink.token(m)?.let { owned[it] }?.let { it.playDtmfTone(m.arg1.toChar()); it.stopDtmfTone() }
            CallLink.HANDOFF -> handOff("asked by the emulator")
            CallLink.ANSWER -> CallLink.token(m)?.let { owned[it] }?.answer(VideoProfile.STATE_AUDIO_ONLY)
            CallLink.REJECT -> CallLink.token(m)?.let { owned[it] }?.reject(false, null)
            CallLink.KEEP -> if (CallLink.token(m) == keepToken) {
                main.removeCallbacks(keepTimeout)
                keepToken = null
            }
            CallLink.RELEASE -> if (CallLink.token(m)?.let { owned.containsKey(it) } == true) handOff("Windows Mobile cannot ring")
        }
    }

    /**
     * Earpiece, speaker, wired headset, or one Bluetooth device (CallAudio). On Android 14 a
     * specific device is requested after the route, so several connected headsets can be told apart.
     */
    private fun routeAudio(route: Int, address: String?) {
        setAudioRoute(route)
        if (route != CallAudioState.ROUTE_BLUETOOTH || address.isNullOrEmpty() || android.os.Build.VERSION.SDK_INT < 34) return
        val dev = runCatching {
            getSystemService(android.bluetooth.BluetoothManager::class.java)?.adapter?.getRemoteDevice(address)
        }.getOrNull() ?: return
        runCatching { requestBluetoothAudio(dev) }.onFailure { Log.w(TAG, "Bluetooth audio $address refused", it) }
    }

    /** The emulator went away while a Windows Mobile call is up. */
    fun peerGone() {
        if (owned.isNotEmpty()) handOff("emulator gone")
    }

    /**
     * Gives every current call to the phone's own dialer: this process ends, Telecom notices that the
     * default dialer's in-call service died and connects the system dialer's (see the file comment).
     */
    private fun handOff(why: String) {
        Log.i(TAG, "handing calls to the phone's dialer: $why")
        CallLinkService.send(Message.obtain(null, CallLink.HANDED_OFF))
        // Let the message leave and Telecom finish this binder call first.
        main.post { Process.killProcess(Process.myPid()) }
    }

    private fun tokenOf(call: Call): String? {
        val extras = call.details.intentExtras ?: return CallLinkService.matchPending(call)
        extras.getString(CallLink.KEY_TOKEN)?.let { return it }
        @Suppress("DEPRECATION")
        val nested = extras.get(TelecomManager.EXTRA_OUTGOING_CALL_EXTRAS) as? Bundle
        return nested?.getString(CallLink.KEY_TOKEN) ?: CallLinkService.matchPending(call)
    }

    private fun causeName(c: DisconnectCause?): String = when (c?.code) {
        DisconnectCause.LOCAL -> "local"
        DisconnectCause.REMOTE -> "remote"
        DisconnectCause.BUSY -> "busy"
        DisconnectCause.REJECTED -> "rejected"
        DisconnectCause.CANCELED -> "canceled"
        DisconnectCause.MISSED -> "missed"
        DisconnectCause.RESTRICTED -> "restricted"
        DisconnectCause.ERROR -> "error"
        else -> "other"
    }

    companion object {
        private const val TAG = "WMDroidCall"
        @Volatile var instance: WmInCallService? = null
    }
}

/**
 * The emulator's link into the call process. CallBridge binds it while calls are on and WMDroid is
 * the dialer; the binding also keeps this process ready, so Telecom's bind is quick.
 */
class CallLinkService : Service() {
    private val incoming = Messenger(object : Handler(Looper.getMainLooper()) {
        override fun handleMessage(m: Message) {
            when (m.what) {
                CallLink.HELLO -> { hello(m.replyTo); return }
                CallLink.SHOWING -> { showing = m.arg1 != 0; return }
                CallLink.PLACING -> {
                    val token = CallLink.token(m) ?: return
                    pending = Triple(token, m.data?.getString(CallLink.KEY_NUMBER) ?: "", System.currentTimeMillis())
                    return
                }
            }
            WmInCallService.instance?.command(m)
        }
    })

    override fun onBind(intent: Intent?): IBinder = incoming.binder

    private fun hello(replyTo: Messenger?) {
        val old = peer
        if (old != null && old.binder !== replyTo?.binder) runCatching { old.binder.unlinkToDeath(death, 0) }
        peer = replyTo
        runCatching { replyTo?.binder?.linkToDeath(death, 0) }
    }

    /** The main process died (crash, killed): a WM call can no longer be shown there. */
    private val death = IBinder.DeathRecipient {
        Handler(Looper.getMainLooper()).post {
            peer = null
            showing = false
            WmInCallService.instance?.peerGone()
        }
    }

    override fun onUnbind(intent: Intent?): Boolean {
        // The emulator closed normally; it hands its calls off itself before that.
        peer = null
        showing = false
        return false
    }

    companion object {
        private const val TAG = "WMDroidCall"
        @Volatile var peer: Messenger? = null
        /** The emulator is on screen with Windows Mobile's phone ready: incoming calls may ring there. */
        @Volatile var showing = false
        /** Token, number and time of the call the emulator is placing, for when Telecom drops our extras. */
        @Volatile private var pending: Triple<String, String, Long>? = null

        fun send(m: Message) {
            val p = peer ?: return
            try { p.send(m) } catch (e: RemoteException) { Log.w(TAG, "emulator unreachable", e) }
        }

        /** Falls back to the number of the call WM placed in the last 20 s. */
        fun matchPending(call: Call): String? {
            val (token, number, at) = pending ?: return null
            if (System.currentTimeMillis() - at > 20_000) return null
            val dialled = call.details.handle?.schemeSpecificPart ?: return null
            if (digits(dialled).takeLast(7) != digits(number).takeLast(7)) return null
            pending = null
            return token
        }

        private fun digits(s: String) = s.filter { it.isDigit() || it == '+' || it == '*' || it == '#' }
    }
}
