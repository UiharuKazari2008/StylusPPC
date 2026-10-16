package jp.net.seventhreeseven.wmdroid

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager

/**
 * Mirrors the phone's status into the guest. Battery: charge level and whether it is plugged in
 * drive Device Emulator's battery device, which the WM battery driver polls.
 * Signal/carrier and Wi-Fi follow once the guest radio interface is emulated.
 */
class DeviceStatusBridge(private val ctx: Context, private val handle: () -> Long) {
    private val receiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) = push(i)
    }

    fun start() {
        val sticky = ctx.registerReceiver(receiver, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (sticky != null) push(sticky)
    }

    fun stop() {
        runCatching { ctx.unregisterReceiver(receiver) }
    }

    private fun push(i: Intent) {
        val h = handle()
        if (h == 0L) return
        val level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, 100)
        val plugged = i.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0
        val percent = if (level >= 0 && scale > 0) level * 100 / scale else 100
        val temp = i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 280)
        NativeBridge.setBattery(h, plugged, percent, temp)
    }
}
