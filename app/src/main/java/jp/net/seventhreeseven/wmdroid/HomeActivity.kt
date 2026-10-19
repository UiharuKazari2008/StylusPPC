package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.LauncherApps
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.Build
import android.os.Bundle
import android.widget.Toast

/**
 * PocketStylus as the phone's home app (Device Settings > Launcher > Use as Phone Launcher).
 *
 * Android starts this for Home. It never shows: it brings the one EmulatorActivity to the front
 * (CLEAR_TOP + SINGLE_TOP: the running session gets onNewIntent, never a second instance), which
 * then shows Today or the Start menu (Settings.launcherHome). With no OS set up yet it opens setup.
 * It lives in a task of its own (taskAffinity ""), so the emulator's task stays an ordinary one:
 * Recents, Back and other apps work as with any launcher whose home screen is an app.
 */
class HomeActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        route()
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        route()
    }

    private fun route() {
        val s = Settings(this)
        GuestStates.settleQuiet(this)
        val p = s.profile
        val v = s.variant
        val ready = p != null && v != null && ImageStore.find(this, p, v) != null
        val i = when {
            GuestStates.needsChoice(this) -> Intent(this, StateKeepActivity::class.java)
                .putExtra(EmulatorActivity.EXTRA_HOME, true)
            ready -> Intent(this, EmulatorActivity::class.java)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TOP or Intent.FLAG_ACTIVITY_SINGLE_TOP)
                .putExtra(EmulatorActivity.EXTRA_HOME, true)
            else -> Intent(this, SetupActivity::class.java)
        }
        startActivity(i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        finish()
        @Suppress("DEPRECATION") overridePendingTransition(0, 0)
    }
}

/**
 * "Add to home screen" from another app while PocketStylus is the launcher (a pin request): asks,
 * then adds the shortcut to Windows Mobile's Start > Programs (AndroidApps pins).
 */
class PinShortcutActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val la = getSystemService(LauncherApps::class.java)
        val req = runCatching { la.getPinItemRequest(intent) }.getOrNull()
        val info = req?.shortcutInfo
        if (req == null || !req.isValid || req.requestType != LauncherApps.PinItemRequest.REQUEST_TYPE_SHORTCUT || info == null) {
            finish()
            return
        }
        val label = (info.shortLabel ?: info.longLabel ?: info.`package`).toString()
        AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("Add to Windows Mobile?")
            .setMessage("“$label” becomes a program in Windows Mobile's Start > Programs.")
            .setPositiveButton("Add") { _, _ ->
                if (req.isValid && req.accept()) {
                    val icon = runCatching { la.getShortcutIconDrawable(info, resources.displayMetrics.densityDpi) }.getOrNull()
                        ?.let { AndroidApps.render(it, 128) }
                    val serial = getSystemService(android.os.UserManager::class.java).getSerialNumberForUser(info.userHandle)
                    AndroidApps.addPin(this, label, info.`package`, info.id, serial, null, icon)
                    Toast.makeText(this, "Added “$label” to Start > Programs", Toast.LENGTH_SHORT).show()
                }
            }
            .setNegativeButton("Cancel", null)
            .setOnDismissListener { finish() }
            .show()
    }
}

/**
 * The old way apps add shortcuts (com.android.launcher.action.INSTALL_SHORTCUT): still sent by
 * some apps to the default launcher. The shortcut's intent is kept only if it opens another app
 * (AndroidApps.safeIntent).
 */
class InstallShortcutReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        @Suppress("DEPRECATION")
        val target = (if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_SHORTCUT_INTENT, Intent::class.java)
            else intent.getParcelableExtra(Intent.EXTRA_SHORTCUT_INTENT)) ?: return
        val uri = target.toUri(Intent.URI_INTENT_SCHEME)
        val safe = AndroidApps.safeIntent(ctx, uri) ?: return
        val pkg = safe.resolveActivity(ctx.packageManager)?.packageName ?: return
        @Suppress("DEPRECATION")
        val label = intent.getStringExtra(Intent.EXTRA_SHORTCUT_NAME) ?: return
        @Suppress("DEPRECATION")
        val icon: Bitmap? = (if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_SHORTCUT_ICON, Bitmap::class.java)
            else intent.getParcelableExtra(Intent.EXTRA_SHORTCUT_ICON)) ?: runCatching {
                @Suppress("DEPRECATION")
                val res = (if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(Intent.EXTRA_SHORTCUT_ICON_RESOURCE, Intent.ShortcutIconResource::class.java)
                    else intent.getParcelableExtra(Intent.EXTRA_SHORTCUT_ICON_RESOURCE))!!
                val r = ctx.packageManager.getResourcesForApplication(res.packageName)
                BitmapFactory.decodeResource(r, r.getIdentifier(res.resourceName, null, null))
            }.getOrNull()
        AndroidApps.addPin(ctx, label, pkg, null, 0, uri, icon)
    }
}
