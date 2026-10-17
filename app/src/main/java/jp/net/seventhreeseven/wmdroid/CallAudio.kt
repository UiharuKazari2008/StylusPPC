package jp.net.seventhreeseven.wmdroid

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager
import android.os.Build
import android.telecom.CallAudioState
import android.util.Log

/**
 * Where a phone call is heard: the earpiece, the loudspeaker, a wired headset, or one Bluetooth
 * device (CallBridge, BluetoothSettingsActivity).
 *
 * Windows Mobile's Speaker button does not reach the phone. FakeRIL answers RIL_GetAudioDevices
 * with E_NOTIMPL, so cprog never switches anything and never writes SNAPI Hardware\Speaker
 * (docs/research-phone-calls-pim.md, E.5). This chooses the phone's output instead. With the
 * dialer role, [onRoute] is WmInCallService.setAudioRoute (and, on Android 14, one Bluetooth
 * device). AudioManager is used as well so a call the system dialer is showing can follow.
 */
object CallAudio {
    const val EARPIECE = "earpiece"
    const val SPEAKER = "speaker"
    const val WIRED = "wired"

    /** One place the call can be played. [id] is [EARPIECE], [SPEAKER], [WIRED] or "bt:<mac>". */
    class Output(val id: String, val name: String)

    /**
     * Set by CallBridge while the emulator is running. Route mask (CallAudioState.ROUTE_*) and the
     * Bluetooth address when the route is one device. Returns true when WmInCallService owns the
     * call and will apply it; false leaves it to [applyLocal].
     */
    var onRoute: ((Int, String?) -> Boolean)? = null

    private const val PREF = "calls.audio"
    /** The output to return to when the speaker is turned off (a headset that was in use). */
    private var restore: String? = null

    fun btId(address: String) = "bt:$address"

    /** The user's choice, or null until they pick one (the phone then routes a new call itself). */
    fun saved(ctx: Context): String? = Settings.global(ctx).getString(PREF, null)

    fun savedName(ctx: Context): String {
        val id = saved(ctx) ?: return "This phone decides"
        return outputs(ctx).firstOrNull { it.id == id }?.name ?: when (id) {
            SPEAKER -> "Speaker"
            WIRED -> "Wired headset"
            EARPIECE -> "Earpiece"
            else -> id.removePrefix("bt:")
        }
    }

    /** Earpiece, speaker, a plugged-in wired headset, and each connected Bluetooth audio device. */
    @SuppressLint("MissingPermission")
    fun outputs(ctx: Context): List<Output> {
        val audio = ctx.getSystemService(AudioManager::class.java)
        val out = ArrayList<Output>()
        out.add(Output(EARPIECE, "Earpiece"))
        out.add(Output(SPEAKER, "Speaker"))
        if (audio != null && wired(audio)) out.add(Output(WIRED, "Wired headset"))
        if (!BluetoothRadio.permissionGranted(ctx)) return out
        val seen = HashSet<String>()
        for (e in BluetoothRadio.snapshot(ctx)) {
            if (!e.connected || !seen.add(e.address)) continue
            out.add(Output(btId(e.address), e.name))
        }
        if (Build.VERSION.SDK_INT >= 31 && audio != null) {
            for (d in audio.availableCommunicationDevices) {
                if (!isBt(d.type)) continue
                val addr = d.address ?: continue
                if (!seen.add(addr)) continue
                val name = d.productName?.toString()?.takeIf { it.isNotBlank() } ?: addr
                out.add(Output(btId(addr), name))
            }
        }
        return out
    }

    /** Speaker on, or back to the headset that was in use (else the earpiece). No call: nothing. */
    fun speaker(ctx: Context, on: Boolean) {
        if (on) {
            val cur = saved(ctx)
            if (cur != SPEAKER) restore = cur ?: headsetNow(ctx)
            select(ctx, SPEAKER)
        } else {
            val back = restore ?: headsetNow(ctx) ?: EARPIECE
            restore = null
            select(ctx, back)
        }
    }

    /**
     * Switches to [id] and remembers it for the next call. A choice that is not connected
     * (a Bluetooth device that has gone) is still remembered; it applies again when it returns.
     */
    fun select(ctx: Context, id: String) {
        Settings.global(ctx).edit().putString(PREF, id).apply()
        Log.i(TAG, "call audio -> $id")
        apply(ctx, id)
    }

    /** The remembered output, when it is available. A new call uses this; otherwise the phone decides. */
    fun applySaved(ctx: Context) {
        val id = saved(ctx) ?: return
        if (outputs(ctx).any { it.id == id }) apply(ctx, id)
    }

    private fun apply(ctx: Context, id: String) {
        val (route, address) = routeOf(id)
        val viaCall = onRoute?.invoke(route, address) == true
        // Below Android 14 the in-call service can only say "Bluetooth", not which device, so the
        // communication device is set too. A failure is left alone: clearing it would undo the route.
        applyLocal(ctx, id, viaCall)
    }

    private fun routeOf(id: String): Pair<Int, String?> = when (id) {
        SPEAKER -> CallAudioState.ROUTE_SPEAKER to null
        WIRED -> CallAudioState.ROUTE_WIRED_HEADSET to null
        EARPIECE -> CallAudioState.ROUTE_EARPIECE to null
        else -> CallAudioState.ROUTE_BLUETOOTH to id.removePrefix("bt:")
    }

    /** A connected headset to fall back to, Bluetooth first. */
    private fun headsetNow(ctx: Context): String? {
        val outs = outputs(ctx)
        return outs.firstOrNull { it.id.startsWith("bt:") }?.id ?: outs.firstOrNull { it.id == WIRED }?.id
    }

    @Suppress("DEPRECATION")
    private fun applyLocal(ctx: Context, id: String, viaCall: Boolean) {
        val audio = ctx.getSystemService(AudioManager::class.java) ?: return
        if (Build.VERSION.SDK_INT >= 31) {
            val want = audio.availableCommunicationDevices.firstOrNull { matches(it, id) }
            if (want != null && runCatching { audio.setCommunicationDevice(want) }.getOrDefault(false)) return
            if (viaCall) return
        }
        if (!viaCall) audio.isSpeakerphoneOn = id == SPEAKER
    }

    private fun matches(d: AudioDeviceInfo, id: String): Boolean = when (id) {
        EARPIECE -> d.type == AudioDeviceInfo.TYPE_BUILTIN_EARPIECE
        SPEAKER -> d.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER
        WIRED -> d.type == AudioDeviceInfo.TYPE_WIRED_HEADSET || d.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES ||
            d.type == AudioDeviceInfo.TYPE_USB_HEADSET
        else -> isBt(d.type) && d.address.equals(id.removePrefix("bt:"), ignoreCase = true)
    }

    private fun isBt(type: Int) = type == AudioDeviceInfo.TYPE_BLUETOOTH_SCO ||
        (Build.VERSION.SDK_INT >= 28 && type == AudioDeviceInfo.TYPE_HEARING_AID) ||
        (Build.VERSION.SDK_INT >= 31 && (type == AudioDeviceInfo.TYPE_BLE_HEADSET || type == AudioDeviceInfo.TYPE_BLE_SPEAKER))

    private fun wired(audio: AudioManager) = audio.getDevices(AudioManager.GET_DEVICES_OUTPUTS).any {
        it.type == AudioDeviceInfo.TYPE_WIRED_HEADSET || it.type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES ||
            it.type == AudioDeviceInfo.TYPE_USB_HEADSET
    }

    private const val TAG = "WMDroidCall"
}
