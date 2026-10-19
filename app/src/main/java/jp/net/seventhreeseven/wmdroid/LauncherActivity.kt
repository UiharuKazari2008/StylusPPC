package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.Intent
import android.os.Bundle

/** Routes to first-run setup until an OS has been chosen, then straight to the device. */
class LauncherActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val s = Settings(this)
        GuestStates.settleQuiet(this)
        val p = s.profile
        val v = s.variant
        val ready = p != null && v != null && ImageStore.find(this, p, v) != null
        val next = when {
            GuestStates.needsChoice(this) -> StateKeepActivity::class.java
            ready -> EmulatorActivity::class.java
            else -> SetupActivity::class.java
        }
        // A running emulator (e.g. the phone's home screen, HomeActivity) is brought forward, not doubled.
        startActivity(Intent(this, next)
            .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP))
        finish()
    }
}
