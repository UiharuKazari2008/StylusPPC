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
                GuestSettings.Kind.ACTION -> button(e.label) {
                    @Suppress("DEPRECATION")
                    if (value == "local:storage") startActivityForResult(Intent(this, StorageFoldersActivity::class.java), REQ_FOLDERS)
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
    }
}
