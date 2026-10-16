package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.DynamicColors
import com.google.android.material.materialswitch.MaterialSwitch

/** Settings > Contacts & calendar: syncing them with Windows Mobile's Contacts and Calendar (PimSync). */
class PimSettingsActivity : AppCompatActivity() {
    private val d get() = resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()
    private lateinit var status: TextView
    private var pendingKind: String? = null
    private lateinit var switches: Map<String, MaterialSwitch>

    override fun onCreate(savedInstanceState: Bundle?) {
        DynamicColors.applyToActivityIfAvailable(this)
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(16), dp(24), dp(24))
        }
        fun text(t: String, size: Float = 14f, alpha: Float = 1f) = TextView(this).apply {
            text = t
            textSize = size
            this.alpha = alpha
            setPadding(0, dp(8), 0, dp(4))
            root.addView(this)
        }
        text("Contacts & calendar", 28f)
        text("Windows Mobile's Contacts and Calendar follow this phone's, and changes made in Windows Mobile " +
            "come back. This phone always wins: when both sides changed the same item, and whenever Windows " +
            "Mobile was restored to an older state, this phone's version is put back into Windows Mobile. " +
            "Items new in Windows Mobile are added here (contacts as phone contacts, appointments to your " +
            "main calendar).", alpha = 0.8f)
        switches = linkedMapOf(
            PimSync.KIND_CONTACT to toggle(root, "Sync contacts", PimSync.KIND_CONTACT),
            PimSync.KIND_APPOINTMENT to toggle(root, "Sync calendar", PimSync.KIND_APPOINTMENT))
        root.addView(MaterialSwitch(this).apply {
            text = "Prompt before accepting changes from Device"
            isChecked = PimSync.prompt(this@PimSettingsActivity)
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on ->
                PimSync.setPrompt(this@PimSettingsActivity, on)
                if (on && android.os.Build.VERSION.SDK_INT >= 33 &&
                    checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
                    requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ + 1)
            }
        }, LinearLayout.LayoutParams(-1, -2))
        text("Changes made in Windows Mobile (edited, new or deleted contacts and appointments) wait in a notification " +
            "until you Accept them for this phone; Reject puts this phone's version back into Windows Mobile and removes " +
            "items that are new there. Changes made on this phone always go to Windows Mobile.", 13f, 0.65f)
        text("Calendar: your shown calendars, from ${PimCalendar.PAST_DAYS} days back to a year ahead. Repeating " +
            "events appear as their single occurrences and are changed on this phone only. Contacts: names, " +
            "phone numbers (in Windows Mobile's fixed slots), e-mail and IM addresses, addresses, company, " +
            "birthday, anniversary, web page and notes; anything that does not fit stays on this phone untouched. " +
            "Syncing runs while Windows Mobile is on screen: shortly after it starts, after changes here, and " +
            "every 15 minutes.", 13f, 0.65f)
        status = text("", 13f, 0.8f)
        root.addView(MaterialButton(this).apply {
            text = "Sync now"
            setOnClickListener {
                PimSync.requested = true
                status.text = "Syncs the next time Windows Mobile is on screen (right away if it is running)."
            }
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })

        val scroll = ScrollView(this).apply { addView(root) }
        ViewCompat.setOnApplyWindowInsetsListener(scroll) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(scroll)
    }

    private fun toggle(root: LinearLayout, label: String, kind: String) = MaterialSwitch(this).apply {
        text = label
        isChecked = if (kind == PimSync.KIND_CONTACT) PimSync.contactsOn(this@PimSettingsActivity) else PimSync.calendarOn(this@PimSettingsActivity)
        minHeight = dp(48)
        setOnCheckedChangeListener { _, on ->
            PimSync.setOn(this@PimSettingsActivity, kind, on)
            if (!on) return@setOnCheckedChangeListener
            val missing = perms(kind).filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
            if (missing.isNotEmpty()) {
                pendingKind = kind
                requestPermissions(missing.toTypedArray(), REQ)
            } else PimSync.requested = true
        }
        root.addView(this, LinearLayout.LayoutParams(-1, -2))
    }

    private fun perms(kind: String) = if (kind == PimSync.KIND_CONTACT)
        listOf(Manifest.permission.READ_CONTACTS, Manifest.permission.WRITE_CONTACTS)
    else listOf(Manifest.permission.READ_CALENDAR, Manifest.permission.WRITE_CALENDAR)

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        val kind = pendingKind ?: return
        pendingKind = null
        val granted = perms(kind).all { checkSelfPermission(it) == PackageManager.PERMISSION_GRANTED }
        if (!granted) { PimSync.setOn(this, kind, false); switches[kind]?.isChecked = false }
        else PimSync.requested = true
    }

    override fun onResume() {
        super.onResume()
        status.text = PimSync.status(this)
    }

    companion object {
        private const val REQ = 41
    }
}
