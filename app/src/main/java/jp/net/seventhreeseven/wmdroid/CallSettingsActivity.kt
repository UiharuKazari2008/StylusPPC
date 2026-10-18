package jp.net.seventhreeseven.wmdroid

import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.provider.Settings as AndroidSettings
import android.text.InputType
import android.widget.EditText
import android.widget.LinearLayout
import androidx.core.widget.doAfterTextChanged
import android.widget.ScrollView
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.ViewCompat
import androidx.core.view.WindowInsetsCompat
import com.google.android.material.button.MaterialButton
import com.google.android.material.color.DynamicColors
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import com.google.android.material.materialswitch.MaterialSwitch

/**
 * Settings > Phone calls: placing real calls from Windows Mobile's Phone app and ringing it for
 * incoming ones (CallBridge), which ringtone sounds ("Use Phone's Ringer"), and optionally making
 * WMDroid the phone's dialer so Windows Mobile's call screen is the only one.
 */
class CallSettingsActivity : AppCompatActivity() {
    private val d get() = resources.displayMetrics.density
    private fun dp(v: Int) = (v * d).toInt()
    private lateinit var roleState: TextView
    private lateinit var roleButton: MaterialButton
    private lateinit var audioBox: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        DynamicColors.applyToActivityIfAvailable(this)
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(24), dp(16), dp(24), dp(24))
        }
        fun text(t: String, size: Float = 14f, alpha: Float = 1f) = root.addView(TextView(this).apply {
            text = t
            textSize = size
            this.alpha = alpha
            setPadding(0, dp(8), 0, dp(4))
        })
        text("Phone calls", 28f)
        text("Dial in Windows Mobile's Phone app and the call goes out on this phone. Windows Mobile's call " +
            "screen shows it: Dialing, Connected with the timer, Ended, and the call history entry. Calls " +
            "to this phone ring in Windows Mobile while PocketStylus is on screen: Answer, Ignore and End " +
            "there act on the real call, and missed calls show in Windows Mobile. The sound stays on this " +
            "phone; it does not pass through the emulator.",
            alpha = 0.8f)

        text("Call audio", 16f)
        text("Windows Mobile's Speaker button does not switch this phone: its radio driver does not report " +
            "audio devices, so the button never changes. Choose the earpiece, the speaker, or a connected " +
            "Bluetooth device here. During a call, long-press the lock button and choose Call audio; that " +
            "keeps Windows Mobile's call screen. With several Bluetooth devices connected, each one is listed.",
            13f, 0.65f)
        audioBox = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(audioBox, LinearLayout.LayoutParams(-1, -2))

        val enable = MaterialSwitch(this).apply {
            text = "Phone calls in Windows Mobile"
            isChecked = CallBridge.enabled(this@CallSettingsActivity)
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on ->
                CallBridge.setEnabled(this@CallSettingsActivity, on)
                if (on) askPermissions()
            }
        }
        root.addView(enable, LinearLayout.LayoutParams(-1, -2))
        // Calls already on from before incoming calls: the caller's number needs the call log permission.
        if (enable.isChecked && savedInstanceState == null) askPermissions()
        text("The first time, Windows Mobile restarts once to turn its phone on. Emergency numbers always " +
            "open this phone's own dialer.", 13f, 0.65f)

        val ringer = MaterialSwitch(this).apply {
            text = "Use Phone's Ringer"
            isChecked = CallBridge.wmRinger(this@CallSettingsActivity)
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on -> CallBridge.setWmRinger(this@CallSettingsActivity, on) }
        }
        root.addView(ringer, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        text("On: Windows Mobile's Phone rings with its own ringtone, on this phone's ringer volume, and this " +
            "phone's ringtone is silenced for that call (without the default phone app role below, the ringer " +
            "is muted for the call and Windows Mobile's ringtone plays on the media volume).\nOff: this phone " +
            "rings as usual and Windows Mobile stays quiet while it rings.", 13f, 0.65f)

        root.addView(MaterialSwitch(this).apply {
            text = "Ring Windows Mobile in the background"
            isChecked = CallOverlay.enabled(this@CallSettingsActivity)
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on -> CallOverlay.setEnabled(this@CallSettingsActivity, on) }
        }, LinearLayout.LayoutParams(-1, -2))
        text("While PocketStylus is in the background (kept in memory), a call opens it in full. If you ignore " +
            "the call or it goes unanswered, you return to the app you were in, or the home screen. With the " +
            "phone locked it opens over the lock screen.", 13f, 0.65f)

        text("Text messages", 16f)
        root.addView(MaterialSwitch(this).apply {
            text = "Text messages in Windows Mobile"
            isChecked = SmsBridge.enabled(this@CallSettingsActivity)
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on ->
                SmsBridge.setEnabled(this@CallSettingsActivity, on)
                if (on) {
                    val missing = SmsBridge.permissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
                    if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), CallBridge.REQ_CALL)
                }
            }
        }, LinearLayout.LayoutParams(-1, -2))
        text("Texts to this phone go into Windows Mobile's Messaging, in the order they came, and what you send " +
            "there goes out from this phone. Messages that arrive while Windows Mobile is closed wait for it.", 13f, 0.65f)
        root.addView(MaterialSwitch(this).apply {
            text = "Defer Incoming SMS Messages"
            isChecked = SmsBridge.defer(this@CallSettingsActivity)
            minHeight = dp(48)
            setOnCheckedChangeListener { _, on -> SmsBridge.setDefer(this@CallSettingsActivity, on) }
        }, LinearLayout.LayoutParams(-1, -2))
        text("While Windows Mobile is not on screen, new messages wait for it instead of waking it: PocketStylus shows " +
            "them in a notification, where you can read them and reply right away. Windows Mobile gets them when you " +
            "open it.", 13f, 0.65f)
        root.addView(MaterialButton(this).apply {
            text = "Load Messages"
            setOnClickListener { loadMessages() }
        }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })
        text("Copies this phone's existing texts into Windows Mobile, once. New messages still arrive on their own.", 13f, 0.65f)

        text("My phone number", 16f)
        val settings = Settings(this)
        val number = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_PHONE
            hint = "Same as this phone"
            setText(settings.myNumber)
            minHeight = dp(48)
            doAfterTextChanged { settings.myNumber = it?.toString()?.trim().orEmpty() }
        }
        root.addView(number, LinearLayout.LayoutParams(-1, -2))
        val detected = PhoneNumber.own(this).takeIf { settings.myNumber.isBlank() }
        text("Windows Mobile shows it as its own number (Phone > Menu > Options). Leave it empty to use this phone's number" +
            when {
                !detected.isNullOrEmpty() -> " ($detected)."
                PhoneNumber.hasPermission(this) -> "; your carrier does not provide it, so type it here."
                else -> ", which PocketStylus can read once calls are on."
            } + " Windows Mobile reads it when it starts.", 13f, 0.65f)

        text("Call screen", 16f)
        text("For Windows Mobile's screen to be the only call screen, PocketStylus has to be this phone's " +
            "default phone app. It then shows calls you dial in Windows Mobile, and calls coming in, while " +
            "PocketStylus is on screen. Everything else goes straight to your usual phone app: calls dialled " +
            "elsewhere, calls coming in while PocketStylus is in the background or the phone is locked, a " +
            "second call during a call, and any Windows Mobile call as soon as PocketStylus leaves the screen " +
            "or stops. Tel: links open your usual dial pad.\n\nWithout it, your usual phone app shows each " +
            "call and Windows Mobile follows along: its timer starts when the call is placed, Answer, Ignore " +
            "and End work, and Hold does not.", alpha = 0.8f)
        roleState = TextView(this).apply { setPadding(0, dp(8), 0, 0) }
        root.addView(roleState)
        roleButton = MaterialButton(this).apply { setOnClickListener { onRoleButton() } }
        root.addView(roleButton, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(8) })

        val scroll = ScrollView(this).apply { addView(root) }
        ViewCompat.setOnApplyWindowInsetsListener(scroll) { v, insets ->
            val bars = insets.getInsets(WindowInsetsCompat.Type.systemBars() or WindowInsetsCompat.Type.displayCutout())
            v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            insets
        }
        setContentView(scroll)
    }

    override fun onResume() {
        super.onResume()
        val held = CallBridge.holdsRole(this)
        roleState.text = if (held) "PocketStylus is the default phone app." else "Your usual phone app is the default."
        roleButton.text = if (held) "Change the default phone app…" else "Use Windows Mobile's call screen…"
        fillAudio()
    }

    private fun fillAudio() {
        audioBox.removeAllViews()
        if (!BluetoothRadio.permissionGranted(this) && BluetoothRadio.permissions().isNotEmpty()) {
            audioBox.addView(MaterialButton(this, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = "Show Bluetooth devices"
                setOnClickListener { requestPermissions(BluetoothRadio.permissions(), REQ_BT) }
            }, LinearLayout.LayoutParams(-1, -2))
        }
        val saved = CallAudio.saved(this)
        for (o in CallAudio.outputs(this)) {
            audioBox.addView(MaterialButton(this, null, com.google.android.material.R.attr.materialButtonOutlinedStyle).apply {
                text = if (o.id == saved) "${o.name}  ·  in use" else o.name
                setOnClickListener {
                    CallAudio.select(this@CallSettingsActivity, o.id)
                    fillAudio()
                }
            }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(4) })
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_BT) fillAudio()
        if (requestCode == REQ_SMS && grantResults.any { it == PackageManager.PERMISSION_GRANTED }) loadMessages()
    }

    /** Copies this phone's existing texts into Windows Mobile. New ones still arrive on their own. */
    private fun loadMessages() {
        val note = SmsBridge.loadHistory(this)
        if (note == "permission") {
            val missing = SmsBridge.permissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
            if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), REQ_SMS)
            return
        }
        android.widget.Toast.makeText(this, note, android.widget.Toast.LENGTH_SHORT).show()
    }

    private fun askPermissions() {
        val missing = CallBridge.permissions().filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isNotEmpty()) requestPermissions(missing.toTypedArray(), CallBridge.REQ_CALL)
    }

    private fun onRoleButton() {
        if (CallBridge.holdsRole(this)) {
            // Apps cannot give a role back themselves: Android's default apps page does it.
            startActivity(Intent(AndroidSettings.ACTION_MANAGE_DEFAULT_APPS_SETTINGS))
            return
        }
        val request = CallBridge.roleRequest(this)
        if (request == null) {
            roleState.text = "This phone does not let other apps be the phone app."
            return
        }
        MaterialAlertDialogBuilder(this)
            .setTitle("Make PocketStylus the phone app?")
            .setMessage("Android asks next. PocketStylus passes every call it does not show to your usual phone " +
                "app, and you can switch back any time in Settings > Apps > Default apps > Phone app.")
            .setPositiveButton("Continue") { _, _ ->
                @Suppress("DEPRECATION")
                startActivityForResult(request, REQ_ROLE)
            }
            .setNegativeButton("Cancel", null)
            .show()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_ROLE) onResume()
    }

    companion object {
        private const val REQ_ROLE = 31
        private const val REQ_BT = 33
        private const val REQ_SMS = 34
    }
}
