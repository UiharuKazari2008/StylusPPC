package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.Gravity
import android.widget.CheckBox
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.Toast
import kotlin.concurrent.thread

/**
 * Settings > Launcher lists (AndroidApps): which apps the Android Apps folder leaves out
 * ([MODE_HIDDEN]), the dock's apps in order ([MODE_DOCK]), and the pinned shortcuts ([MODE_PINS]).
 * Changes are saved at once; the running session picks them up when this closes.
 */
class AndroidAppsActivity : SettingsPage() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        when (intent.getStringExtra(EXTRA_MODE)) {
            MODE_DOCK -> dock()
            MODE_PINS -> pins()
            else -> hidden()
        }
        show()
    }

    /** A row: the app's icon (loaded in the background) and a check box. */
    private fun appRow(a: AndroidApps.App, checked: Boolean, onChange: (CheckBox, Boolean) -> Unit): CheckBox {
        val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; minimumHeight = dp(52) }
        val icon = ImageView(this)
        row.addView(icon, LinearLayout.LayoutParams(dp(36), dp(36)).apply { marginEnd = dp(12) })
        val box = CheckBox(this).apply { text = a.label; isChecked = checked }
        box.setOnCheckedChangeListener { b, v -> onChange(b as CheckBox, v) }
        row.addView(box, LinearLayout.LayoutParams(0, -2, 1f))
        card.addView(row, LinearLayout.LayoutParams(-1, -2))
        icons += icon to a
        return box
    }

    private val icons = ArrayList<Pair<ImageView, AndroidApps.App>>()

    private fun loadIcons() {
        val list = icons.toList()
        thread(name = "app-icons") {
            for ((view, a) in list) {
                val d = runCatching { AndroidApps.appIcon(this, a) }.getOrNull() ?: continue
                runOnUiThread { view.setImageDrawable(d) }
            }
        }
    }

    private fun hidden() {
        title("Hidden apps")
        section("Android Apps folder")
        note("Ticked apps are left out of Windows Mobile's Start > Programs > Android Apps.")
        val hidden = settings.hiddenApps.toMutableSet()
        for (a in AndroidApps.apps(this)) appRow(a, a.key in hidden) { _, on ->
            if (on) hidden += a.key else hidden -= a.key
            settings.hiddenApps = hidden
        }
        loadIcons()
    }

    private fun dock() {
        val size = settings.dockSize
        title("Dock apps")
        section("Up to $size apps")
        note("Tick the apps the dock shows. Up and Down change their order. Position and size are in Settings > Dock.")
        val order = settings.dockApps.toMutableList()
        val apps = AndroidApps.apps(this)
        // The dock's current apps first, in their order, then the rest.
        val sorted = order.mapNotNull { k -> apps.firstOrNull { it.key == k } } + apps.filter { it.key !in order }
        for (a in sorted) {
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL; minimumHeight = dp(52) }
            val icon = ImageView(this)
            row.addView(icon, LinearLayout.LayoutParams(dp(36), dp(36)).apply { marginEnd = dp(12) })
            val box = CheckBox(this).apply { text = a.label; isChecked = a.key in order }
            box.setOnCheckedChangeListener { b, on ->
                if (on && a.key !in order) {
                    if (order.size >= size) {
                        b.isChecked = false
                        Toast.makeText(this, "The dock holds $size apps: untick one first", Toast.LENGTH_SHORT).show()
                        return@setOnCheckedChangeListener
                    }
                    order += a.key
                } else if (!on) order -= a.key
                settings.dockApps = order
                recreate()
            }
            row.addView(box, LinearLayout.LayoutParams(0, -2, 1f))
            val up = buttonSlot("Up")
            val down = buttonSlot("Down")
            val at = order.indexOf(a.key)
            up.isEnabled = at > 0
            down.isEnabled = at >= 0 && at < order.lastIndex
            if (at < 0) { up.visibility = android.view.View.GONE; down.visibility = android.view.View.GONE }
            up.setOnClickListener {
                val i = order.indexOf(a.key)
                if (i > 0) { val swap = order[i - 1]; order[i - 1] = order[i]; order[i] = swap; settings.dockApps = order; recreate() }
            }
            down.setOnClickListener {
                val i = order.indexOf(a.key)
                if (i >= 0 && i < order.lastIndex) { val swap = order[i + 1]; order[i + 1] = order[i]; order[i] = swap; settings.dockApps = order; recreate() }
            }
            row.addView(up)
            row.addView(down)
            card.addView(row, LinearLayout.LayoutParams(-1, -2))
            icons += icon to a
        }
        loadIcons()
    }

    private fun buttonSlot(label: String) = android.widget.Button(this).apply {
        text = label
        minimumWidth = dp(48)
        minimumHeight = dp(40)
    }

    private fun pins() {
        title("Pinned shortcuts")
        section("In Start > Programs")
        val list = AndroidApps.pins(this)
        note(if (list.isEmpty()) "None yet. With PocketStylus as the phone's launcher, an app's “Add to home screen” " +
            "puts its shortcut here and in Windows Mobile's Start > Programs."
            else "Tap one to remove it from Windows Mobile.")
        for (p in list) button(p.label) {
            com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle("Remove “${p.label}”?")
                .setMessage("It goes from Windows Mobile's Start > Programs.")
                .setPositiveButton("Remove") { _, _ ->
                    AndroidApps.removePins(this) { it.id == p.id }
                    recreate()
                }
                .setNegativeButton("Cancel", null)
                .show()
        }
    }

    companion object {
        const val EXTRA_MODE = "mode"
        const val MODE_HIDDEN = "hidden"
        const val MODE_DOCK = "dock"
        const val MODE_PINS = "pins"

        /** The screen for a Launcher page command (AndroidApps.CMD_*), or null for another command. */
        fun intentFor(ctx: Context, command: String): Intent? = when (command) {
            AndroidApps.CMD_HIDDEN -> MODE_HIDDEN
            AndroidApps.CMD_DOCK -> MODE_DOCK
            AndroidApps.CMD_PINS -> MODE_PINS
            else -> null
        }?.let { Intent(ctx, AndroidAppsActivity::class.java).putExtra(EXTRA_MODE, it) }
    }
}
