package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.ApplicationInfo
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.telecom.TelecomManager
import android.util.Log

/**
 * The dial pad Android opens for tel: links and the "Phone" shortcut (ACTION_DIAL). Holding the
 * dialer role requires an app to handle it; WMDroid passes it on to the phone's own dialer, so tel:
 * links from other apps keep working as before. (Calls placed in Windows Mobile never come here.)
 */
class DialActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val forward = Intent(intent).apply {
            component = null
            if (action != Intent.ACTION_DIAL && action != Intent.ACTION_VIEW) action = Intent.ACTION_DIAL
            systemDialerPackage(this@DialActivity)?.let { setPackage(it) }
            flags = flags and (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION).inv()
        }
        try {
            startActivity(forward)
        } catch (e: RuntimeException) {
            Log.w("WMDroidCall", "no dialer to pass ${intent?.action} to", e)
        }
        finish()
    }

    companion object {
        /** The phone's own dialer (TelecomManager.getSystemDialerPackage is API 29). */
        fun systemDialerPackage(ctx: Context): String? {
            if (Build.VERSION.SDK_INT >= 29) return ctx.getSystemService(TelecomManager::class.java)?.systemDialerPackage
            // Android 8/9: the preinstalled app that handles tel: dial pads, other than PocketStylus.
            return runCatching {
                ctx.packageManager.queryIntentActivities(Intent(Intent.ACTION_DIAL, Uri.parse("tel:")), 0)
                    .map { it.activityInfo }
                    .firstOrNull { it.packageName != ctx.packageName && (it.applicationInfo.flags and ApplicationInfo.FLAG_SYSTEM) != 0 }
                    ?.packageName
            }.getOrNull()
        }
    }
}
