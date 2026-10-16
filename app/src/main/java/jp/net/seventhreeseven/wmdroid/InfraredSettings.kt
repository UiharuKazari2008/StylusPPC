package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import androidx.core.widget.doAfterTextChanged
import kotlin.concurrent.thread

/**
 * Infrared settings (docs/research-irda.md), in their own preferences file so they never collide
 * with the per-OS settings: which way Windows Mobile's IR beams leave the phone, and the InfraWAN
 * relay account.
 */
class InfraredSettings(ctx: Context) {
    private val p = ctx.applicationContext.getSharedPreferences("wmdroid.irda", Context.MODE_PRIVATE)

    /** "ask" (the popup in Windows Mobile), "ble", "wan" or "off". */
    var mode: String
        get() = p.getString("mode", "ask") ?: "ask"
        set(v) = p.edit().putString("mode", v).apply()
    /** The last transport picked in the popup (shown first next time). */
    var last: String
        get() = p.getString("last", "ble") ?: "ble"
        set(v) = p.edit().putString("last", v).apply()
    /** Keep the chosen transport connected while PocketStylus runs, so others can beam to this phone. */
    var listen: Boolean
        get() = p.getBoolean("listen", false)
        set(v) = p.edit().putBoolean("listen", v).apply()
    var server: String
        get() = p.getString("server", "") ?: ""
        set(v) = p.edit().putString("server", v.trim()).apply()
    var room: String
        get() = p.getString("room", "lobby") ?: "lobby"
        set(v) = p.edit().putString("room", v.trim()).apply()
    var user: String
        get() = p.getString("user", "") ?: ""
        set(v) = p.edit().putString("user", v.trim()).apply()
    var token: String
        get() = p.getString("token", "") ?: ""
        set(v) = p.edit().putString("token", v.trim()).apply()
    /** This phone's name for the other devices (relay presence; Windows Mobile has its own device name). */
    var name: String
        get() = p.getString("name", null) ?: Build.MODEL
        set(v) = p.edit().putString("name", v.trim()).apply()

    /** A random id for this phone on Bluetooth LE (advertised; picks which phone of a pair connects). */
    val nodeId: Int
        get() {
            var id = p.getInt("node", 0)
            if (id == 0) {
                id = (java.security.SecureRandom().nextInt() and 0x7FFFFFFF).coerceAtLeast(1)
                p.edit().putInt("node", id).apply()
            }
            return id
        }

    val wanConfigured get() = server.isNotEmpty() && user.isNotEmpty()

    companion object {
        val MODES = listOf("ask", "ble", "wan", "off")

        /** Android 12+ runtime permissions for Bluetooth LE (scan without location, advertise, connect). */
        fun blePermissions(): Array<String> =
            if (Build.VERSION.SDK_INT >= 31) arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_ADVERTISE,
                Manifest.permission.BLUETOOTH_CONNECT)
            else arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)

        fun hasBlePermissions(ctx: Context) =
            blePermissions().all { ctx.checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
    }
}

/** Settings > Infrared. */
class InfraredSettingsActivity : SettingsPage() {
    private lateinit var ir: InfraredSettings
    private lateinit var testResult: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        ir = InfraredSettings(this)
        title("Infrared")
        section("Beaming")
        note("Windows Mobile's infrared port (Beam a contact, a picture or a file, IrDA programs, IrCOMM) " +
            "reaches other devices through this phone: nearby PocketStylus phones over Bluetooth LE, or " +
            "everyone in a room on an InfraWAN relay on the Internet. When a program starts using infrared, " +
            "Windows Mobile asks which way to go.")
        val names = listOf("Ask each time", "Nearby phones (Bluetooth LE)", "InfraWAN (Internet)", "Off")
        spinner("When Windows Mobile uses infrared", names, InfraredSettings.MODES.indexOf(ir.mode).coerceAtLeast(0)) {
            ir.mode = InfraredSettings.MODES[it]
            if (ir.mode == "ble") askBle()
        }
        switch("Stay reachable while PocketStylus is open", ir.listen) { ir.listen = it }
        note("Keeps the chosen connection open so other devices can beam to this one even when Windows " +
            "Mobile itself isn't sending. Turn on \"Receive all incoming beams\" in Windows Mobile's Beam " +
            "settings too.")
        edit("This phone's name for others", ir.name) { ir.name = it }

        section("Nearby phones (Bluetooth LE)")
        note("Phones running PocketStylus find each other over Bluetooth LE while infrared is in use, as if " +
            "their IR ports faced each other. Pairing is not needed. Android asks for the Nearby devices " +
            "permission the first time.")
        button("Allow Nearby devices") { askBle() }

        section("InfraWAN relay")
        note("A small server that passes infrared traffic between everyone in the same room " +
            "(server/infrawan in PocketStylus's source). Ask the server's owner for a user name and token.")
        edit("Server (wss://host/infrawan)", ir.server, InputType.TYPE_TEXT_VARIATION_URI) { ir.server = it }
        edit("Room", ir.room) { ir.room = it }
        edit("User", ir.user) { ir.user = it }
        edit("Token", ir.token, InputType.TYPE_TEXT_VARIATION_PASSWORD) { ir.token = it }
        button("Test connection") { test() }
        testResult = note("")
        show()
    }

    private fun edit(hint: String, value: String, type: Int = 0, live: (String) -> Unit) {
        label(hint)
        card.addView(EditText(this).apply {
            setText(value)
            isSingleLine = true
            inputType = InputType.TYPE_CLASS_TEXT or type
            doAfterTextChanged { live(it?.toString() ?: "") }
        }, LinearLayout.LayoutParams(-1, -2))
    }

    private fun askBle() {
        if (!InfraredSettings.hasBlePermissions(this)) requestPermissions(InfraredSettings.blePermissions(), 41)
    }

    // Connects once with the current account and reports who is in the room.
    private fun test() {
        if (!ir.wanConfigured) { testResult.text = "Enter the server and user first."; return }
        testResult.text = "Connecting…"
        thread(name = "infrawan-test") {
            val r = InfraWanTransport.probe(ir)
            runOnUiThread { testResult.text = r }
        }
    }
}
