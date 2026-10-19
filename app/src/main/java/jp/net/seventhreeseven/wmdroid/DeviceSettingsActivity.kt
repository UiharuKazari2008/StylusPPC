package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.os.Bundle
import androidx.core.widget.doAfterTextChanged
import com.google.android.material.button.MaterialButton

/**
 * Emergency copy of the settings Windows Mobile can change, for when that app can't be opened.
 * Phone and Infrared are edited only inside Windows Mobile. Built from one catalogue
 * (GuestSettings). Changes apply at once; settings that need a restart wait for the Apply button.
 * Result OK = restart Windows Mobile.
 */
class DeviceSettingsActivity : SettingsPage() {
    private val pending = LinkedHashMap<String, String>()
    private lateinit var apply: MaterialButton
    private var resumed = false
    private var batteryShown = false
    private var unrestricted = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title("Device Settings")
        apply = MaterialButton(this).apply {
            text = "Apply and restart Windows Mobile"
            isEnabled = false
            setOnClickListener {
                // Another screen or memory size: ask first unless Windows Mobile's storage carries over (GuestStates).
                val target = GuestStates.resolveElsewhere(this@DeviceSettingsActivity, settings,
                    pending["res"] ?: settings.screenRes, pending["ram"]?.toIntOrNull() ?: settings.memMB)
                GuestStates.confirm(this@DeviceSettingsActivity, GuestStates.change(this@DeviceSettingsActivity, settings, target), apply = {
                    for ((k, v) in pending) GuestSettings.entriesFor(settings).firstOrNull { it.key == k }?.set?.invoke(settings, v)
                    setResult(RESULT_OK)
                    finish()
                }, cancel = {})
            }
        }
        var page = ""
        // Phone and Infrared are edited in Windows Mobile. This screen is the emergency copy.
        unrestricted = settings.backgroundMode == Settings.BG_RUN
        val catalogue = GuestSettings.entriesFor(settings).filter {
            it.page != "Phone" && it.page != "Infrared" &&
                !(unrestricted && (it.key == "bgtimer" || it.key == "resume"))
        }
        batteryShown = catalogue.any { it.key == "batteryopt" }
        for (e in catalogue) {
            if (e.page != page) {
                page = e.page
                section(page)
                if (page == "Display") note("Incognito hides the skin and fills the window now. The guest's pixel size follows the phone on the next boot or restore.")
                if (catalogue.any { it.page == page && it.restart })
                    note("Changes here apply with the button at the bottom: Windows Mobile restarts.")
            }
            val value = e.get(settings)
            when (e.kind) {
                GuestSettings.Kind.BOOL -> switch(e.label, value == "1") { change(e, if (it) "1" else "0") }
                GuestSettings.Kind.CHOICE -> spinner(e.label, e.options.map { it.second },
                    e.options.indexOfFirst { it.first == value }.coerceAtLeast(0)) { change(e, e.options[it].first) }
                GuestSettings.Kind.SLIDER -> slider(e.label, e.min, e.max, value.toIntOrNull() ?: e.min) { change(e, it.toString()) }
                GuestSettings.Kind.COLOR -> colorRow(e, value)
                GuestSettings.Kind.TEXT -> textRow(e, value)
                GuestSettings.Kind.ACTION -> button(e.label) {
                    @Suppress("DEPRECATION")
                    if (value == "local:storage") startActivityForResult(Intent(this, StorageFoldersActivity::class.java), REQ_FOLDERS)
                    // Launcher page (AndroidApps): the home-app role, and the app lists.
                    else if (value == AndroidApps.CMD_ROLE) AndroidApps.requestHome(this, REQ_ROLE)
                    else if (value == GuestSettings.CMD_PIM_SYNC) {
                        PimSync.requested = true
                        android.widget.Toast.makeText(this, "Syncs the next time Windows Mobile is on screen", android.widget.Toast.LENGTH_SHORT).show()
                    }
                    else if (value == GuestSettings.CMD_SMS_LOAD) {
                        val note = SmsBridge.loadHistory(this)
                        android.widget.Toast.makeText(this,
                            if (note == "permission") "Allow messages, then press Load Messages again" else note,
                            android.widget.Toast.LENGTH_SHORT).show()
                        if (note == "permission") {
                            val missing = SmsBridge.permissions().filter { checkSelfPermission(it) != android.content.pm.PackageManager.PERMISSION_GRANTED }
                            if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), REQ_SMS)
                        }
                    }
                    else if (value.startsWith("widget ")) {
                        settings.requestWidget(value.removePrefix("widget ").trim())
                        android.widget.Toast.makeText(this, "Installs the next time Windows Mobile is on screen",
                            android.widget.Toast.LENGTH_SHORT).show()
                    }
                    else if (value.startsWith("install ")) {
                        settings.requestInstall(value.removePrefix("install ").trim())
                    }
                    else GuestSettings.screenFor(this, value)?.let { startActivity(it) }
                }
            }
        }
        show(apply)
    }

    override fun onResume() {
        super.onResume()
        val show = !GuestSettings.batteryExempt(this)
        if (!resumed) { resumed = true; return }
        if (show != batteryShown) recreate()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        // The folder mapping was applied: restart Windows Mobile with it.
        if (requestCode == REQ_FOLDERS && resultCode == RESULT_OK) { setResult(RESULT_OK); finish() }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_SMS && grantResults.any { it == android.content.pm.PackageManager.PERMISSION_GRANTED }) {
            val note = SmsBridge.loadHistory(this)
            if (note != "permission") android.widget.Toast.makeText(this, note, android.widget.Toast.LENGTH_SHORT).show()
        }
    }

    private fun textRow(e: GuestSettings.Entry, value: String) {
        label(e.label)
        card.addView(android.widget.EditText(this).apply {
            setText(value)
            isSingleLine = true
            if (e.options.any { it.first == "password" })
                inputType = android.text.InputType.TYPE_CLASS_TEXT or android.text.InputType.TYPE_TEXT_VARIATION_PASSWORD
            doAfterTextChanged { change(e, it?.toString() ?: "") }
        }, android.widget.LinearLayout.LayoutParams(-1, -2))
    }

    /** A colour: the presets, plus "Custom…" with hue / saturation / brightness sliders. */
    private fun colorRow(e: GuestSettings.Entry, value: String) {
        val presets = e.options
        var current = value
        lateinit var spin: android.widget.Spinner
        fun items() = presets.map { it.second } + (if (presets.none { it.first.equals(current, true) }) listOf("Custom $current") else emptyList()) + "Custom…"
        fun select() {
            val list = items()
            spin.adapter = android.widget.ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, list)
            val i = presets.indexOfFirst { it.first.equals(current, true) }
            spin.setSelection(if (i >= 0) i else presets.size)
        }
        spin = spinner(e.label, items(), 0) { pos ->
            val list = items()
            when {
                pos < presets.size -> if (!presets[pos].first.equals(current, true)) { current = presets[pos].first; change(e, current) }
                list[pos] == "Custom…" -> pickColor(current) { picked -> current = picked; change(e, picked); select() }
            }
        }
        select()
    }

    private fun pickColor(start: String, done: (String) -> Unit) {
        val hsv = FloatArray(3)
        android.graphics.Color.colorToHSV(runCatching { android.graphics.Color.parseColor(start) }.getOrDefault(0xFF1F5FC0.toInt()), hsv)
        val box = android.widget.LinearLayout(this).apply { orientation = android.widget.LinearLayout.VERTICAL; setPadding(dp(20), dp(12), dp(20), 0) }
        val preview = android.view.View(this).apply { minimumHeight = dp(56) }
        box.addView(preview)
        fun paint() { preview.setBackgroundColor(android.graphics.Color.HSVToColor(hsv)) }
        for ((i, name, max) in listOf(Triple(0, "Hue", 360), Triple(1, "Saturation", 100), Triple(2, "Brightness", 100))) {
            box.addView(android.widget.TextView(this).apply { text = name; setPadding(0, dp(10), 0, 0) })
            box.addView(android.widget.SeekBar(this).apply {
                this.max = max
                progress = if (i == 0) hsv[0].toInt() else (hsv[i] * 100).toInt()
                setOnSeekBarChangeListener(object : android.widget.SeekBar.OnSeekBarChangeListener {
                    override fun onProgressChanged(sb: android.widget.SeekBar, v: Int, user: Boolean) {
                        hsv[i] = if (i == 0) v.toFloat() else v / 100f
                        paint()
                    }
                    override fun onStartTrackingTouch(sb: android.widget.SeekBar) {}
                    override fun onStopTrackingTouch(sb: android.widget.SeekBar) {}
                })
            })
        }
        paint()
        com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
            .setTitle("Key color")
            .setView(box)
            .setPositiveButton("Use") { _, _ -> done("#%06X".format(android.graphics.Color.HSVToColor(hsv) and 0xFFFFFF)) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    private fun change(e: GuestSettings.Entry, value: String) {
        if (e.restart) {
            if (e.get(settings) == value) pending.remove(e.key) else pending[e.key] = value
            apply.isEnabled = pending.isNotEmpty()
            return
        }
        e.set(settings, value)
        // Unrestricted drops the timer and the hibernation toggle; another mode brings them back.
        // The spinner reports its current row once as it is attached, which is not a change.
        if (e.key == "bgkind" && (settings.backgroundMode == Settings.BG_RUN) != unrestricted) recreate()
    }

    companion object {
        const val REQ_FOLDERS = 61
        const val REQ_ROLE = 62
        private const val REQ_SMS = 63
    }
}
