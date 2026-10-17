package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.os.Build
import android.provider.Settings
import android.util.Log

/**
 * This phone's Bluetooth radio: on or off, which paired devices are connected, and connecting or
 * disconnecting one (BluetoothSettingsActivity). Windows Mobile does not see these devices; call
 * audio uses a connected one only when PocketStylus routes the call there (CallAudio).
 *
 * Turning the radio on or off is a system dialog (apps targeting Android 13 cannot flip it
 * themselves). Connecting is the profile's connect/disconnect, which some phones reserve for
 * system settings; the caller then opens those settings.
 */
class BluetoothRadio(private val ctx: Context) {
    class Entry(
        val address: String,
        val name: String,
        val calls: Boolean,
        val media: Boolean,
        val hearing: Boolean,
        val le: Boolean,
    ) {
        val connected get() = calls || media || hearing || le
        /** "Connected · calls, media" or "Not connected". */
        val status: String
            get() {
                val parts = ArrayList<String>()
                if (calls) parts.add("calls")
                if (media) parts.add("media")
                if (hearing) parts.add("hearing aid")
                if (le) parts.add("LE audio")
                return if (parts.isEmpty()) "Not connected" else "Connected · " + parts.joinToString(", ")
            }
    }

    private val manager = ctx.getSystemService(BluetoothManager::class.java)
    private val adapter: BluetoothAdapter? get() = manager?.adapter
    private var onChange: (() -> Unit)? = null
    private var watching = false

    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, intent: Intent?) { onChange?.invoke() }
    }

    fun hasRadio() = adapter != null

    fun isOn() = adapter?.isEnabled == true

    fun stateLabel(): String {
        val a = adapter ?: return "This phone has no Bluetooth radio."
        return when (a.state) {
            BluetoothAdapter.STATE_ON -> "Bluetooth is on."
            BluetoothAdapter.STATE_OFF -> "Bluetooth is off."
            BluetoothAdapter.STATE_TURNING_ON -> "Turning Bluetooth on…"
            BluetoothAdapter.STATE_TURNING_OFF -> "Turning Bluetooth off…"
            else -> "Bluetooth is off."
        }
    }

    /** Follows radio and connection changes until [stop]. */
    fun start(onChange: () -> Unit) {
        this.onChange = onChange
        if (watching) return
        val filter = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
        filter.addAction(BluetoothDevice.ACTION_ACL_CONNECTED)
        filter.addAction(BluetoothDevice.ACTION_ACL_DISCONNECTED)
        filter.addAction(BluetoothAdapter.ACTION_CONNECTION_STATE_CHANGED)
        if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED)
        else ctx.registerReceiver(receiver, filter)
        watching = true
    }

    fun stop() {
        onChange = null
        if (!watching) return
        runCatching { ctx.unregisterReceiver(receiver) }
        watching = false
    }

    /** The system asks before the radio changes. */
    fun requestEnable(activity: Activity) {
        runCatching { activity.startActivity(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE)) }
            .onFailure {
                Log.w(TAG, "cannot ask to enable Bluetooth", it)
                activity.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
            }
    }

    fun requestDisable(activity: Activity) {
        runCatching { activity.startActivity(Intent(ACTION_REQUEST_DISABLE)) }
            .onFailure {
                Log.w(TAG, "cannot ask to disable Bluetooth", it)
                activity.startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
            }
    }

    /** Paired devices, with which audio profiles are up. Needs [permissionGranted]. */
    fun devices(): List<Entry> = snapshot(ctx)

    /**
     * Connects or disconnects [address] on the call and media profiles. [done] is false when this
     * phone refuses (the caller opens system Bluetooth settings).
     */
    fun setLink(address: String, connect: Boolean, done: (Boolean) -> Unit) {
        val a = adapter
        val device = if (a == null || !permissionGranted(ctx)) null else runCatching { a.getRemoteDevice(address) }.getOrNull()
        if (a == null || device == null) { done(false); return }
        val ids = profileIds()
        val proxies = ArrayList<Pair<Int, BluetoothProfile>>()
        val lock = Any()
        var left = 0
        var collecting = true
        var finished = false
        var ok = false
        fun settle(delta: Int, proxy: Pair<Int, BluetoothProfile>?, worked: Boolean) {
            val ready = synchronized(lock) {
                if (worked) ok = true
                if (proxy != null) proxies.add(proxy)
                left += delta
                val go = !collecting && left == 0 && !finished
                if (go) finished = true
                go
            }
            if (!ready) return
            for ((id, proxy) in proxies) runCatching { a.closeProfileProxy(id, proxy) }
            done(ok)
        }
        val listener = object : BluetoothProfile.ServiceListener {
            override fun onServiceConnected(profile: Int, proxy: BluetoothProfile) {
                // The callback can arrive on this thread, inside getProfileProxy: don't hold [lock] then.
                settle(-1, profile to proxy, invoke(proxy, if (connect) "connect" else "disconnect", device))
            }
            override fun onServiceDisconnected(profile: Int) {}
        }
        for (id in ids) {
            settle(1, null, false)
            val accepted = runCatching { a.getProfileProxy(ctx, listener, id) }.getOrDefault(false)
            if (!accepted) settle(-1, null, false)
        }
        synchronized(lock) { collecting = false }
        settle(0, null, false)
    }

    private fun invoke(proxy: BluetoothProfile, method: String, device: BluetoothDevice): Boolean {
        val m = proxy.javaClass.methods.firstOrNull {
            it.name == method && it.parameterTypes.contentEquals(arrayOf(BluetoothDevice::class.java))
        } ?: return false
        return runCatching {
            val r = m.invoke(proxy, device)
            r !is Boolean || r
        }.onFailure { Log.w(TAG, "$method ${device.address} refused", it) }.getOrDefault(false)
    }

    companion object {
        private const val TAG = "WMDroidBluetooth"
        /** Hidden system action: the confirm-to-turn-off dialog. Not a public constant. */
        private const val ACTION_REQUEST_DISABLE = "android.bluetooth.adapter.action.REQUEST_DISABLE"

        fun permissions(): Array<String> =
            if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_CONNECT) else emptyArray()

        fun permissionGranted(ctx: Context) =
            permissions().all { ctx.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }

        /** Paired devices and which audio profiles are connected. Empty without permission. */
        @SuppressLint("MissingPermission")
        fun snapshot(ctx: Context): List<Entry> {
            if (!permissionGranted(ctx)) return emptyList()
            val manager = ctx.getSystemService(BluetoothManager::class.java) ?: return emptyList()
            val a = manager.adapter ?: return emptyList()
            val calls = connected(manager, BluetoothProfile.HEADSET)
            val media = connected(manager, BluetoothProfile.A2DP)
            val hearing = if (Build.VERSION.SDK_INT >= 29) connected(manager, BluetoothProfile.HEARING_AID) else emptySet()
            val le = if (Build.VERSION.SDK_INT >= 31) connected(manager, BluetoothProfile.LE_AUDIO) else emptySet()
            return runCatching { a.bondedDevices }.getOrNull().orEmpty()
                .sortedBy { nameOf(it).lowercase() }
                .map { d -> Entry(d.address, nameOf(d), d.address in calls, d.address in media, d.address in hearing, d.address in le) }
        }

        @SuppressLint("MissingPermission")
        private fun nameOf(d: BluetoothDevice): String {
            val alias = if (Build.VERSION.SDK_INT >= 30) runCatching { d.alias }.getOrNull() else null
            return alias?.takeIf { it.isNotBlank() } ?: d.name?.takeIf { it.isNotBlank() } ?: d.address
        }

        private fun connected(manager: BluetoothManager, profile: Int): Set<String> =
            runCatching { manager.getConnectedDevices(profile).map { it.address }.toSet() }.getOrDefault(emptySet())

        private fun profileIds(): List<Int> = buildList {
            add(BluetoothProfile.HEADSET)
            add(BluetoothProfile.A2DP)
            if (Build.VERSION.SDK_INT >= 29) add(BluetoothProfile.HEARING_AID)
            if (Build.VERSION.SDK_INT >= 31) add(BluetoothProfile.LE_AUDIO)
        }
    }
}
