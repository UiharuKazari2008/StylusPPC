package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.os.Bundle
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import com.google.android.material.button.MaterialButton

/**
 * Emergency settings for when Windows Mobile itself can't be used: the operating system, skin and
 * storage card. Display, design, hardware, power and device live under Device Settings. Phone and
 * Infrared are changed inside Windows Mobile (Settings > System > PocketStylus). Result OK =
 * restart Windows Mobile; otherwise live changes are applied in place.
 */
class SettingsActivity : SettingsPage() {
    private lateinit var picker: OsPicker

    private val bridges = listOf(
        "status" to "Device status (battery, signal, Wi-Fi)",
        "audio" to "Audio in / out",
        "time" to "Time & time-zone sync",
        "storage" to "Phone storage passthrough",
        "gps" to "GPS (NMEA serial)",
        "camera" to "Cameras",
        "phone" to "Phone calls",
        "devbridge" to "DevBridge (agent/debug port)",
        "gpu" to "GPU acceleration",
    )

    /** Bridges that are implemented (always on for now). */
    private val available = setOf("status", "audio", "time", "storage", "gps", "camera", "phone")

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        picker = OsPicker(this, settings)
        title("Settings")

        // ---- Device Settings (shared with Windows Mobile's own settings app) ----
        section("Device Settings")
        note("Display, design, hardware, power and device options. The same settings are in Windows Mobile " +
            "under Settings > System > PocketStylus.")
        card.addView(MaterialButton(this).apply {
            text = "Device Settings…"
            setOnClickListener {
                @Suppress("DEPRECATION")
                startActivityForResult(Intent(this@SettingsActivity, DeviceSettingsActivity::class.java), REQ_DEVICE)
            }
        })

        // ---- System ----
        section("System")
        group("Operating system")
        card.addView(picker.view)
        button("Cold boot this OS next start (delete its saved states)") {
            EmulatorActivity.clearSnapshots(this)
            android.widget.Toast.makeText(this, "Saved states of this OS deleted", android.widget.Toast.LENGTH_SHORT).show()
        }

        // ---- HTC apps (HtcAppsActivity): from an HD2 ROM the user owns ----
        group("HTC HD2 apps")
        note("Install apps such as Teeter from an HTC HD2 ROM file you own.")
        button("HTC HD2 apps…") {
            @Suppress("DEPRECATION")
            startActivityForResult(Intent(this, HtcAppsActivity::class.java), REQ_HTC)
        }

        // ---- Skin ----
        section("Skin")
        skinNote = note("")
        showSkin()
        button("Import skin file…") {
            @Suppress("DEPRECATION")
            startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE)
                .setType("*/*").putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("application/json", "text/plain")), REQ_SKIN)
        }
        button("Paste skin JSON…", outlined = false) { pasteSkin() }
        button("Use the built-in skin", outlined = false) { settings.customSkin = ""; showSkin() }
        note("Design skins with tools/skin-designer.html (Front, Front + D-pad, Foldable, Foldable + phone keyboard, Foldable Max, Foldable Max _ Keyboard). A Dock rectangle sets where the app dock sits.")

        // ---- Storage card ----
        section("Storage card")
        note("What Windows Mobile sees as its storage card.")
        val modes = listOf(
            StorageBridge.MODE_PRIVATE to "PocketStylus's own folder (no permission needed)",
            StorageBridge.MODE_FULL to "All of the phone's storage",
            StorageBridge.MODE_FOLDERS to "Chosen folders",
        )
        val modeGroup = RadioGroup(this)
        modes.forEachIndexed { i, (key, label) ->
            modeGroup.addView(RadioButton(this).apply {
                id = 1000 + i
                text = label
                isChecked = settings.storageMode == key
            })
        }
        card.addView(modeGroup)
        fun chosenMode() = modes[(modeGroup.checkedRadioButtonId - 1000).coerceIn(0, modes.size - 1)].first
        val edit = button("Edit folders…") {
            @Suppress("DEPRECATION")
            startActivityForResult(Intent(this, StorageFoldersActivity::class.java), REQ_FOLDERS)
        }
        val applyStorage = MaterialButton(this).apply {
            text = "Apply and restart Windows Mobile"
            setOnClickListener {
                settings.storageMode = chosenMode()
                setResult(RESULT_OK)
                finish()
            }
        }
        card.addView(applyStorage)
        fun syncStorage() {
            edit.visibility = if (chosenMode() == StorageBridge.MODE_FOLDERS) android.view.View.VISIBLE else android.view.View.GONE
            applyStorage.isEnabled = chosenMode() != settings.storageMode
        }
        modeGroup.setOnCheckedChangeListener { _, _ -> syncStorage() }
        syncStorage()
        accessNote = note("")
        button("Grant all files access", outlined = false) { startActivity(StorageBridge.allFilesAccessIntent(this)) }
        updateAccessNote()

        // ---- Phone ----
        // Calls and infrared are the Phone and Infrared tabs inside Windows Mobile.
        section("Phone")
        group("Contacts & calendar")
        button("Contacts & calendar…") { startActivity(Intent(this, PimSettingsActivity::class.java)) }
        group("Bluetooth")
        button("Bluetooth…") { startActivity(Intent(this, BluetoothSettingsActivity::class.java)) }
        group("Phone hardware")
        note("Your phone's hardware bridged into Windows Mobile. Each switches on here once it is implemented.")
        for ((key, label) in bridges) switch(label, settings.bridgeEnabled(key) || key in available).isEnabled = false

        val save = MaterialButton(this).apply {
            text = "Save and restart device"
            setOnClickListener {
                picker.save()
                setResult(RESULT_OK)
                finish()
            }
        }
        show(save)
    }

    // ---- Skin ----

    private lateinit var skinNote: TextView

    private fun showSkin() {
        skinNote.text = if (settings.customSkin.isBlank()) "Using the built-in skin."
        else "Using your imported skin (" + settings.customSkin.length / 1024 + " KB)."
    }

    /** Checks a skin, makes it the current one and takes its backdrop; false (with a message) if invalid. */
    private fun useSkin(json: String): Boolean {
        val spec = try { SkinSpec.parse(json) } catch (e: IllegalArgumentException) {
            android.widget.Toast.makeText(this, "Not a skin: ${e.message}", android.widget.Toast.LENGTH_LONG).show()
            return false
        }
        settings.customSkin = json
        spec.backdrop?.let { b -> if (b in listOf("black", "gray", "silver", "white")) settings.backdrop = b }
        spec.opacity?.let { settings.backdropOpacity = it.coerceIn(0, 100) }
        showSkin()
        android.widget.Toast.makeText(this, "Skin loaded. Go back to see it.", android.widget.Toast.LENGTH_SHORT).show()
        return true
    }

    private fun pasteSkin() {
        val box = android.widget.EditText(this).apply {
            hint = "Paste skin JSON"
            minLines = 6
            maxLines = 12
            textSize = 12f
            val clip = getSystemService(android.content.ClipboardManager::class.java)?.primaryClip
            if (clip != null && clip.itemCount > 0) setText(clip.getItemAt(0).coerceToText(this@SettingsActivity))
        }
        com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
            .setTitle("Skin JSON")
            .setView(box)
            .setPositiveButton("Use") { _, _ -> useSkin(box.text.toString()) }
            .setNegativeButton("Cancel", null)
            .show()
    }

    // ---- Storage ----

    private lateinit var accessNote: TextView

    private fun updateAccessNote() {
        accessNote.text = if (StorageBridge.hasAllFilesAccess()) "All files access: granted"
        else "All files access is needed for phone storage and chosen folders; until then PocketStylus's own folder is used."
    }

    override fun onResume() {
        super.onResume()
        if (::accessNote.isInitialized) updateAccessNote()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        when (requestCode) {
            REQ_SKIN -> if (resultCode == RESULT_OK) data?.data?.let { uri ->
                runCatching { contentResolver.openInputStream(uri)?.bufferedReader()?.use { it.readText() } }.getOrNull()
                    ?.let { useSkin(it) }
            }
            // Device Settings or the folder mapping asked for a restart.
            REQ_DEVICE, REQ_FOLDERS, REQ_HTC -> if (resultCode == RESULT_OK) { setResult(RESULT_OK); finish() }
            else -> picker.onActivityResult(requestCode, resultCode, data)
        }
    }

    companion object {
        const val REQ_FOLDER = 52
        const val REQ_SKIN = 53
        const val REQ_DEVICE = 54
        const val REQ_FOLDERS = 55
        const val REQ_HTC = 56
    }
}
