package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.os.Bundle
import android.provider.Settings
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import com.google.android.material.materialswitch.MaterialSwitch

/**
 * Settings > Bluetooth: the phone's radio, which paired devices are connected, and connecting or
 * disconnecting one. A connected device can be chosen as the call output (CallAudio). Windows
 * Mobile does not talk to these devices; sending a file still uses Start > Send via Bluetooth.
 */
class BluetoothSettingsActivity : SettingsPage() {
    private val radio by lazy { BluetoothRadio(this) }
    private lateinit var statusNote: TextView
    private lateinit var radioSwitch: MaterialSwitch
    private lateinit var deviceBox: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title("Bluetooth")
        section("Radio")
        statusNote = note("")
        radioSwitch = MaterialSwitch(this).apply {
            text = "Bluetooth"
            minHeight = dp(48)
        }
        card.addView(radioSwitch, LinearLayout.LayoutParams(-1, -2))
        note("Turning Bluetooth on or off asks this phone to confirm. PocketStylus cannot change it on its own.")

        section("Paired devices")
        note("Connection status for each paired device. Connect and disconnect use the phone's Bluetooth " +
            "profiles; if the phone reserves that for system settings, those settings open instead.")
        deviceBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        card.addView(deviceBox)

        section("Call audio")
        note("A connected headset or speaker can be the output for phone calls. Windows Mobile's Speaker " +
            "button does not switch this phone, so choose the device here or in Phone calls. During a call, " +
            "long-press the lock button and choose Call audio. Windows Mobile itself does not use the device.")

        section("Files")
        note("To send from Windows Mobile, use Start > Send via Bluetooth. Receiving is switched in Device " +
            "Settings > Hardware and needs All files access (Bluetooth saves received files in Download).")
        show()
    }

    override fun onStart() {
        super.onStart()
        if (!BluetoothRadio.permissionGranted(this)) {
            val need = BluetoothRadio.permissions()
            if (need.isNotEmpty()) requestPermissions(need, REQ)
        }
        radio.start { refresh() }
        refresh()
    }

    override fun onStop() {
        radio.stop()
        super.onStop()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ) refresh()
    }

    private fun refresh() {
        statusNote.text = radio.stateLabel()
        val on = radio.isOn()
        radioSwitch.setOnCheckedChangeListener(null)
        radioSwitch.isEnabled = radio.hasRadio()
        radioSwitch.isChecked = on
        radioSwitch.setOnCheckedChangeListener { _, checked ->
            if (checked == radio.isOn()) return@setOnCheckedChangeListener
            radioSwitch.isChecked = radio.isOn()
            if (!BluetoothRadio.permissionGranted(this)) {
                val need = BluetoothRadio.permissions()
                if (need.isNotEmpty()) requestPermissions(need, REQ)
                return@setOnCheckedChangeListener
            }
            if (checked) radio.requestEnable(this) else radio.requestDisable(this)
        }
        fillDevices()
    }

    private fun fillDevices() {
        deviceBox.removeAllViews()
        if (!radio.hasRadio()) return
        if (!BluetoothRadio.permissionGranted(this)) {
            deviceBox.addView(line("Allow Nearby devices to see paired devices and whether they are connected.", 0.65f))
            return
        }
        val devices = radio.devices()
        if (devices.isEmpty()) {
            deviceBox.addView(line("No paired devices. Pair them in this phone's Bluetooth settings.", 0.65f))
            buttonInto(deviceBox, "Bluetooth settings…") { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
            return
        }
        val chosen = CallAudio.saved(this)
        for (d in devices) {
            deviceBox.addView(line(d.name, 1f))
            val using = chosen == CallAudio.btId(d.address)
            deviceBox.addView(line(d.status + if (using) " · used for calls" else "", 0.65f))
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL }
            buttonInto(row, if (d.connected) "Disconnect" else "Connect") { link(d, !d.connected) }
            if (d.connected) buttonInto(row, "Use for calls") {
                CallAudio.select(this, CallAudio.btId(d.address))
                Toast.makeText(this, "Calls use ${d.name}", Toast.LENGTH_SHORT).show()
                refresh()
            }
            deviceBox.addView(row)
        }
    }

    private fun link(d: BluetoothRadio.Entry, connect: Boolean) {
        if (!radio.isOn()) {
            Toast.makeText(this, "Turn Bluetooth on first", Toast.LENGTH_SHORT).show()
            return
        }
        Toast.makeText(this, if (connect) "Connecting ${d.name}…" else "Disconnecting ${d.name}…", Toast.LENGTH_SHORT).show()
        radio.setLink(d.address, connect) { ok ->
            runOnUiThread {
                if (!ok) {
                    Toast.makeText(this, "This phone only connects Bluetooth devices from its own settings.", Toast.LENGTH_LONG).show()
                    startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS))
                }
                refresh()
            }
        }
    }

    private fun line(t: String, alpha: Float) = TextView(this).apply {
        text = t
        textSize = if (alpha < 1f) 13f else 16f
        this.alpha = alpha
        setPadding(0, dp(8), 0, dp(2))
    }

    private fun buttonInto(parent: LinearLayout, t: String, onClick: () -> Unit) {
        val b = com.google.android.material.button.MaterialButton(this, null,
            com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
            text = t
            setOnClickListener { onClick() }
        }
        val across = parent.orientation == LinearLayout.HORIZONTAL
        parent.addView(b, LinearLayout.LayoutParams(if (across) 0 else -1, -2, if (across) 1f else 0f).apply {
            topMargin = dp(4)
            if (across) marginEnd = dp(4)
        })
    }

    companion object {
        private const val REQ = 32
    }
}
