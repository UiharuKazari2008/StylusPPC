package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.Intent
import android.os.Bundle

/** Routes to first-run setup until an OS has been chosen, then straight to the device. */
class LauncherActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val s = Settings(this)
        val p = s.profile
        val v = s.variant
        val ready = p != null && v != null && ImageStore.find(this, p, v) != null
        startActivity(Intent(this, if (ready) EmulatorActivity::class.java else SetupActivity::class.java))
        finish()
    }
}
