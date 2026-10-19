package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.os.Bundle
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.Toast
import com.google.android.material.button.MaterialButton

/**
 * Once after the update that folds every resolution and memory size into one state per ROM:
 * each ROM that has more than one saved version asks which to keep. The others are removed.
 */
class StateKeepActivity : SettingsPage() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val roms = GuestStates.legacyRoms(this).filter { it.versions.size > 1 }
        if (roms.isEmpty()) {
            GuestStates.keep(this, emptyMap())
            openNext()
            return
        }
        title("Keep a saved version")
        section("Saved states")
        note("Each Windows Mobile keeps one internal storage from now on. Pick the version to keep for each one. " +
            "The others are removed. Files on the storage card stay.")

        var nextId = 1000
        val idToDir = HashMap<Int, String>()
        val groups = roms.map { rom ->
            section(rom.profile.title)
            val extra = buildString {
                if (rom.profile.variants.size > 1) append(rom.variant.label)
                if (!rom.profile.coldBootKeepsStorage) {
                    if (isNotEmpty()) append(". ")
                    append("The memory size you pick is the one this Windows Mobile uses.")
                }
            }
            if (extra.isNotEmpty()) note(extra)
            val prefer = GuestStates.preferredIndex(this, rom)
            val group = RadioGroup(this)
            rom.versions.forEachIndexed { i, v ->
                val id = nextId++
                idToDir[id] = v.dir.name
                group.addView(RadioButton(this).apply {
                    this.id = id
                    text = v.describe()
                    isChecked = i == prefer
                })
            }
            card.addView(group)
            rom to group
        }
        val keep = MaterialButton(this).apply {
            text = "Keep selected versions"
            setOnClickListener {
                isEnabled = false
                val picks = groups.associate { (rom, group) ->
                    rom.key to (idToDir[group.checkedRadioButtonId] ?: rom.versions.first().dir.name)
                }
                if (!GuestStates.keep(this@StateKeepActivity, picks)) {
                    isEnabled = true
                    Toast.makeText(this@StateKeepActivity, "Could not keep that version", Toast.LENGTH_LONG).show()
                    return@setOnClickListener
                }
                openNext()
            }
        }
        show(keep)
    }

    private fun openNext() {
        val profile = settings.profile
        val variant = settings.variant
        val ready = profile != null && variant != null && ImageStore.find(this, profile, variant) != null
        val i = Intent(this, if (ready) EmulatorActivity::class.java else SetupActivity::class.java)
        if (ready && intent.getBooleanExtra(EmulatorActivity.EXTRA_HOME, false))
            i.putExtra(EmulatorActivity.EXTRA_HOME, true)
        startActivity(i.addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP))
        finish()
    }
}
