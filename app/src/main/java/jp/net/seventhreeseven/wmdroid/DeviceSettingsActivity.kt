package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.os.Bundle
import com.google.android.material.button.MaterialButton

/**
 * Device Settings: the same pages and settings as WMDroid's settings app inside Windows Mobile,
 * built from one catalogue (GuestSettings), so both stay in step. Changes apply at once; settings
 * that need a restart wait for the Apply button. Result OK = restart Windows Mobile.
 */
class DeviceSettingsActivity : SettingsPage() {
    private val pending = LinkedHashMap<String, String>()
    private lateinit var apply: MaterialButton

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        title("Device Settings")
        apply = MaterialButton(this).apply {
            text = "Apply and restart Windows Mobile"
            isEnabled = false
            setOnClickListener {
                for ((k, v) in pending) GuestSettings.entries.firstOrNull { it.key == k }?.set?.invoke(settings, v)
                setResult(RESULT_OK)
                finish()
            }
        }
        var page = ""
        for (e in GuestSettings.entries) {
            if (e.page != page) {
                page = e.page
                section(page)
                if (GuestSettings.entries.any { it.page == page && it.restart })
                    note("Changes here apply with the button at the bottom: Windows Mobile restarts.")
            }
            val value = e.get(settings)
            when (e.kind) {
                GuestSettings.Kind.BOOL -> switch(e.label, value == "1") { change(e, if (it) "1" else "0") }
                GuestSettings.Kind.CHOICE -> spinner(e.label, e.options.map { it.second },
                    e.options.indexOfFirst { it.first == value }.coerceAtLeast(0)) { change(e, e.options[it].first) }
                GuestSettings.Kind.SLIDER -> slider(e.label, e.min, e.max, value.toIntOrNull() ?: e.min) { change(e, it.toString()) }
                GuestSettings.Kind.COLOR -> colorRow(e, value)
                GuestSettings.Kind.ACTION -> button(e.label) {
                    @Suppress("DEPRECATION")
                    if (value == "local:storage") startActivityForResult(Intent(this, StorageFoldersActivity::class.java), REQ_FOLDERS)
                    // Launcher page (AndroidApps): the home-app role, and the app lists.
                    else if (value == AndroidApps.CMD_ROLE) AndroidApps.requestHome(this, REQ_ROLE)
                    else AndroidAppsActivity.intentFor(this, value)?.let { startActivity(it) }
                }
            }
        }
        show(apply)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        // The folder mapping was applied: restart Windows Mobile with it.
        if (requestCode == REQ_FOLDERS && resultCode == RESULT_OK) { setResult(RESULT_OK); finish() }
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
    }

    companion object {
        const val REQ_FOLDERS = 61
        const val REQ_ROLE = 62
    }
}
