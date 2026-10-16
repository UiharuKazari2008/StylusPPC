package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.app.role.RoleManager
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.LauncherActivityInfo
import android.content.pm.LauncherApps
import android.content.pm.ShortcutInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.drawable.Drawable
import android.os.Handler
import android.os.UserHandle
import android.os.UserManager
import android.util.Log
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import kotlin.concurrent.thread

/**
 * Android apps for Windows Mobile (Device Settings > Launcher).
 *
 * Start > Programs > Android Apps: one program per Android launcher app (minus Settings.hiddenApps).
 * Each is a copy of AppLink.exe (guest/AppLink, tools/make-applink.py) with the app's icon written
 * into its icon resource, served from the card's .android folder (an overlay of [hostDir]) as
 * <id>_<ver>.exe: id names the app (FNV of its component and user), ver its icon and label, so a
 * changed icon is a new file (WM caches icons by path) and an unchanged one is never rewritten.
 * android.ini in the mailbox lists them; "android sync" has the helper make the shortcuts
 * (guest/Helper/AndroidApps.cs). Tapping one makes the launcher write "android launch <id>_<ver>"
 * to the mailbox and [launch] starts the app.
 *
 * Pinned shortcuts: with PocketStylus as the phone's launcher, an app's "add to home screen" lands in
 * PinShortcutActivity (and the old INSTALL_SHORTCUT broadcast in InstallShortcutReceiver); each one
 * becomes a program of its own in Start > Programs, kept in [pins] (shared by every OS).
 */
object AndroidApps {
    const val TAG = "WMDroidApps"

    // Commands of the Launcher page's buttons (GuestSettings), from WM or Device Settings.
    const val CMD_ROLE = "launcher role"
    const val CMD_HIDDEN = "launcher hidden"
    const val CMD_PINS = "launcher pins"
    const val CMD_DOCK = "launcher dock"

    /** The card folder the launchers are in (GuestBridge.overlays). */
    const val GUEST_DIR = ".android"
    /** Icon sizes in AppLink.exe, as tools/make-applink.py SIZES. */
    private val ICON_SIZES = intArrayOf(16, 24, 32, 48, 64)

    /** An Android launcher app; [key] is what Settings stores (component and user). */
    class App(val key: String, val label: String, val component: ComponentName, val user: UserHandle, val info: LauncherActivityInfo) {
        val id: String get() = idOf(key)
    }

    /** A pinned shortcut: a LauncherApps shortcut ([shortcutId]) or a legacy one (an intent [uri]). */
    class Pin(val id: String, val label: String, val pkg: String, val shortcutId: String?, val userSerial: Long, val uri: String?)

    fun hostDir(ctx: Context) = File(ctx.filesDir, "overlay/android")
    private fun pinDir(ctx: Context) = File(ctx.filesDir, "android-pins")
    fun pinIcon(ctx: Context, id: String) = File(pinDir(ctx), "$id.png")

    fun fnv(s: String): Int {
        var h = 0x811C9DC5.toInt()
        for (b in s.toByteArray()) { h = h xor (b.toInt() and 0xFF); h *= 16777619 }
        return h
    }

    fun idOf(key: String) = "%08x".format(fnv(key))

    /** Every launcher app of every profile on the phone (work profile included), PocketStylus aside, by name. */
    fun apps(ctx: Context): List<App> {
        val la = ctx.getSystemService(LauncherApps::class.java) ?: return emptyList()
        val um = ctx.getSystemService(UserManager::class.java)
        val out = ArrayList<App>()
        for (user in runCatching { la.profiles }.getOrDefault(emptyList())) {
            val serial = um?.getSerialNumberForUser(user) ?: 0L
            for (info in runCatching { la.getActivityList(null, user) }.getOrDefault(emptyList())) {
                if (info.componentName.packageName == ctx.packageName) continue
                out += App("${info.componentName.flattenToShortString()}#$serial", info.label.toString(), info.componentName, user, info)
            }
        }
        return out.sortedBy { it.label.lowercase() }
    }

    fun appIcon(ctx: Context, a: App): Drawable = a.info.getBadgedIcon(ctx.resources.displayMetrics.densityDpi).mutate()

    /** [d] drawn [s] pixels square (drawn large, then halved down: smooth small icons). */
    fun render(d: Drawable, s: Int): Bitmap {
        val big = maxOf(128, s)
        val b = Bitmap.createBitmap(big, big, Bitmap.Config.ARGB_8888)
        d.setBounds(0, 0, big, big)
        d.draw(Canvas(b))
        return scaleTo(b, s)
    }

    private fun scaleTo(src: Bitmap, s: Int): Bitmap {
        var b = src
        while (b.width / 2 >= s) b = Bitmap.createScaledBitmap(b, b.width / 2, b.height / 2, true)
        return if (b.width == s && b.height == s) b else Bitmap.createScaledBitmap(b, s, s, true)
    }

    // ---- The phone's launcher (HomeActivity) ----

    fun isHome(ctx: Context): Boolean =
        ctx.getSystemService(RoleManager::class.java)?.isRoleHeld(RoleManager.ROLE_HOME) == true

    /** Asks Android to make PocketStylus the home app; when it already is, opens the default-home choice. */
    fun requestHome(a: Activity, requestCode: Int) {
        val rm = a.getSystemService(RoleManager::class.java)
        if (rm != null && rm.isRoleAvailable(RoleManager.ROLE_HOME) && !rm.isRoleHeld(RoleManager.ROLE_HOME)) {
            @Suppress("DEPRECATION")
            if (runCatching { a.startActivityForResult(rm.createRequestRoleIntent(RoleManager.ROLE_HOME), requestCode) }.isSuccess) return
        }
        if (runCatching { a.startActivity(Intent(android.provider.Settings.ACTION_HOME_SETTINGS)) }.isFailure)
            runCatching { a.startActivity(Intent(android.provider.Settings.ACTION_SETTINGS)) }
    }

    // ---- Launching ----

    /** "android launch <name>" from a launcher in WM: <name> is its file name, <id>_<ver>. */
    fun launch(ctx: Context, name: String): Boolean {
        val id = name.substringBefore('_')
        if (id.startsWith("p")) return pins(ctx).firstOrNull { it.id == id }?.let { launchPin(ctx, it) } ?: false
        val app = apps(ctx).firstOrNull { it.id == id }
        if (app == null) { Log.w(TAG, "no app for $name"); return false }
        return launchApp(ctx, app)
    }

    fun launchKey(ctx: Context, key: String): Boolean = apps(ctx).firstOrNull { it.key == key }?.let { launchApp(ctx, it) } ?: false

    fun launchApp(ctx: Context, a: App): Boolean = runCatching {
        ctx.getSystemService(LauncherApps::class.java).startMainActivity(a.component, a.user, null, null)
        true
    }.getOrElse { Log.w(TAG, "launch ${a.key}: $it"); false }

    fun launchPin(ctx: Context, p: Pin): Boolean = runCatching {
        if (p.shortcutId != null) {
            val user = ctx.getSystemService(UserManager::class.java).getUserForSerialNumber(p.userSerial) ?: android.os.Process.myUserHandle()
            // Only the phone's launcher may start pinned shortcuts.
            ctx.getSystemService(LauncherApps::class.java).startShortcut(p.pkg, p.shortcutId, null, null, user)
        } else {
            val i = safeIntent(ctx, p.uri ?: return@runCatching false) ?: return@runCatching false
            ctx.startActivity(i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
        }
        true
    }.getOrElse { Log.w(TAG, "shortcut ${p.id}: $it"); false }

    /**
     * A legacy shortcut's intent, if it is safe to start from PocketStylus: it must open another
     * app (never one of PocketStylus' own, unexported, screens) and carries no URI grants.
     */
    fun safeIntent(ctx: Context, uri: String): Intent? {
        val i = runCatching { Intent.parseUri(uri, Intent.URI_INTENT_SCHEME) }.getOrNull() ?: return null
        i.selector = null
        i.flags = i.flags and (Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION or
            Intent.FLAG_GRANT_PERSISTABLE_URI_PERMISSION or Intent.FLAG_GRANT_PREFIX_URI_PERMISSION).inv()
        val target = i.resolveActivity(ctx.packageManager)?.packageName ?: return null
        if (target == ctx.packageName || i.`package` == ctx.packageName) return null
        return i
    }

    // ---- Pinned shortcuts ----

    /** Notified when a shortcut is pinned or removed (the running session's LauncherBridge). */
    @Volatile var pinsListener: (() -> Unit)? = null

    private fun pinsFile(ctx: Context) = File(pinDir(ctx), "pins.json")

    @Synchronized fun pins(ctx: Context): List<Pin> = runCatching {
        val a = JSONArray(pinsFile(ctx).readText())
        (0 until a.length()).map { i ->
            val o = a.getJSONObject(i)
            Pin(o.getString("id"), o.getString("label"), o.getString("pkg"), o.optString("sid").ifEmpty { null },
                o.optLong("user"), o.optString("uri").ifEmpty { null })
        }
    }.getOrDefault(emptyList())

    @Synchronized private fun savePins(ctx: Context, list: List<Pin>) {
        val a = JSONArray()
        for (p in list) a.put(JSONObject().put("id", p.id).put("label", p.label).put("pkg", p.pkg)
            .put("sid", p.shortcutId ?: "").put("user", p.userSerial).put("uri", p.uri ?: ""))
        pinDir(ctx).mkdirs()
        val f = pinsFile(ctx)
        val tmp = File(f.path + ".tmp")
        tmp.writeText(a.toString())
        tmp.renameTo(f)
    }

    @Synchronized fun addPin(ctx: Context, label: String, pkg: String, shortcutId: String?, userSerial: Long, uri: String?, icon: Bitmap?): Pin {
        val id = "p" + "%07x".format(fnv("$pkg|$shortcutId|$uri|$userSerial") and 0x0FFFFFFF)
        val p = Pin(id, label, pkg, shortcutId, userSerial, uri)
        savePins(ctx, pins(ctx).filter { it.id != id } + p)
        if (icon != null) {
            pinDir(ctx).mkdirs()
            pinIcon(ctx, id).outputStream().use { scaleTo(icon, 128).compress(Bitmap.CompressFormat.PNG, 100, it) }
        }
        Log.i(TAG, "pinned $label ($pkg ${shortcutId ?: uri})")
        pinsListener?.invoke()
        return p
    }

    @Synchronized fun removePins(ctx: Context, which: (Pin) -> Boolean) {
        val all = pins(ctx)
        val gone = all.filter(which)
        if (gone.isEmpty()) return
        savePins(ctx, all - gone.toSet())
        gone.forEach { pinIcon(ctx, it.id).delete() }
        pinsListener?.invoke()
    }

    // ---- The card's launchers and the helper's list ----

    private class Template(val bytes: ByteArray) {
        /** Where each icon's BITMAPINFOHEADER is in the template. */
        val headers = ICON_SIZES.map { s -> find(s) }

        private fun find(s: Int): Int {
            val sig = java.nio.ByteBuffer.allocate(16).order(java.nio.ByteOrder.LITTLE_ENDIAN)
                .putInt(40).putInt(s).putInt(2 * s).putShort(1).putShort(32).array()
            outer@ for (i in 0..bytes.size - sig.size) {
                for (j in sig.indices) if (bytes[i + j] != sig[j]) continue@outer
                return i
            }
            error("AppLink.exe has no ${s}px icon")
        }
    }

    /** A launcher: the template with [icon] in each icon size (rows bottom-up BGRA, then the AND mask). */
    private fun makeExe(t: Template, icon: Bitmap): ByteArray {
        val out = t.bytes.copyOf()
        for ((i, s) in ICON_SIZES.withIndex()) {
            val b = scaleTo(icon, s)
            val px = IntArray(s * s)
            b.getPixels(px, 0, s, 0, 0, s, s)  // ARGB, not premultiplied: as .ico wants it
            var o = t.headers[i] + 40
            for (y in s - 1 downTo 0) for (x in 0 until s) {
                val c = px[y * s + x]
                out[o++] = c.toByte(); out[o++] = (c shr 8).toByte(); out[o++] = (c shr 16).toByte(); out[o++] = (c ushr 24).toByte()
            }
            val row = (s + 31) / 32 * 4
            for (y in s - 1 downTo 0) {
                for (k in 0 until row) out[o + k] = 0
                for (x in 0 until s) if ((px[y * s + x] ushr 24) < 128) out[o + x / 8] = (out[o + x / 8].toInt() or (0x80 shr (x % 8))).toByte()
                o += row
            }
        }
        return out
    }

    private fun clean(label: String) = label.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ').trim()

    /**
     * Brings the card's launchers and the helper's list ([ini], android.ini in the mailbox) up to
     * date. Draws icons only for new or changed apps. True when the helper should sync.
     */
    @Synchronized fun sync(ctx: Context, s: Settings, ini: File): Boolean {
        val dir = hostDir(ctx).apply { mkdirs() }
        val template by lazy { Template(ctx.assets.open("guest/AppLink.exe").use { it.readBytes() }) }
        val want = LinkedHashMap<String, () -> Bitmap?>()
        val text = StringBuilder()
        text.append("folder=").append(if (s.androidFolder) 1 else 0).append("\r\nname=Android Apps\r\n")
        // Icons follow the phone's theme and density: a change there redraws them all.
        val cfg = ctx.resources.configuration
        val look = "${cfg.uiMode and android.content.res.Configuration.UI_MODE_NIGHT_MASK}/${cfg.densityDpi}"
        if (s.androidFolder) {
            val hidden = s.hiddenApps
            for (a in apps(ctx)) {
                if (a.key in hidden) continue
                val src = a.info.applicationInfo.sourceDir ?: ""
                val ver = "%04x".format(fnv("${a.label}|$src|${File(src).lastModified()}|$look") and 0xFFFF)
                val file = "${a.id}_$ver.exe"
                want[file] = { render(appIcon(ctx, a), 128) }
                text.append("app\t").append(clean(a.label)).append('\t').append(file).append("\r\n")
            }
        }
        for (p in pins(ctx)) {
            val icon = pinIcon(ctx, p.id)
            val ver = "%04x".format(fnv("${p.label}|${icon.lastModified()}") and 0xFFFF)
            val file = "${p.id}_$ver.exe"
            want[file] = {
                BitmapFactory.decodeFile(icon.path) ?: runCatching { render(ctx.packageManager.getApplicationIcon(p.pkg), 128) }.getOrNull()
            }
            text.append("pin\t").append(clean(p.label)).append('\t').append(file).append("\r\n")
        }
        var made = 0
        for ((file, draw) in want) {
            val f = File(dir, file)
            if (f.exists()) continue
            val bmp = runCatching { draw() }.getOrNull() ?: render(ctx.packageManager.defaultActivityIcon, 128)
            val tmp = File(dir, "$file.tmp")
            tmp.writeBytes(makeExe(template, bmp))
            tmp.renameTo(f)
            made++
        }
        var removed = 0
        dir.listFiles()?.forEach { if (it.name !in want.keys) { it.delete(); removed++ } }
        val body = text.toString()
        val changed = !ini.exists() || runCatching { ini.readText() }.getOrNull() != body
        if (changed) {
            val tmp = File(ini.path + ".tmp")
            tmp.writeText(body)
            tmp.renameTo(ini)
        }
        if (changed || made > 0 || removed > 0) Log.i(TAG, "sync: ${want.size} programs, $made drawn, $removed removed")
        return changed || made > 0 || removed > 0
    }
}

/**
 * The running session's side of [AndroidApps]: keeps WM's Android Apps folder in step with the
 * phone (installs, removals, updates, pins, settings), launches what WM asks for, and fills the
 * skin's dock.
 */
class LauncherBridge(
    private val ctx: Context, private val settings: Settings, private val guest: GuestBridge,
    private val ui: Handler, private val skin: SkinLayout,
) {
    private val la = ctx.getSystemService(LauncherApps::class.java)
    private var syncing = false
    private var again = false
    private var forceNext = false
    private var lastKey = ""
    private var lastDockKey = ""
    private var dockKeys = emptyList<String>()

    private val callback = object : LauncherApps.Callback() {
        override fun onPackageRemoved(pkg: String, user: UserHandle) {
            // A removed app's legacy shortcuts go with it (LauncherApps drops its pinned ones).
            AndroidApps.removePins(ctx) { it.pkg == pkg && it.shortcutId == null }
            changed()
        }
        override fun onPackageAdded(pkg: String, user: UserHandle) = changed()
        override fun onPackageChanged(pkg: String, user: UserHandle) = changed()
        override fun onPackagesAvailable(pkgs: Array<out String>, user: UserHandle, replacing: Boolean) = changed()
        override fun onPackagesUnavailable(pkgs: Array<out String>, user: UserHandle, replacing: Boolean) = changed()
        override fun onShortcutsChanged(pkg: String, shortcuts: MutableList<ShortcutInfo>, user: UserHandle) {
            // A pinned shortcut its app removed goes from WM too.
            val ids = shortcuts.map { it.id }.toSet()
            AndroidApps.removePins(ctx) { it.pkg == pkg && it.shortcutId != null && it.shortcutId !in ids }
        }
    }

    private val changedRun = Runnable { sync(false); refreshDock() }

    /** Apps come and go in bursts (an update is removed + added): act once they settle. */
    private fun changed() {
        ui.removeCallbacks(changedRun)
        ui.postDelayed(changedRun, 1500)
    }

    fun start() {
        runCatching { la?.registerCallback(callback, ui) }
        AndroidApps.pinsListener = { ui.post { sync(false) } }
        apply()
    }

    fun stop() {
        ui.removeCallbacks(changedRun)
        runCatching { la?.unregisterCallback(callback) }
        if (AndroidApps.pinsListener != null) AndroidApps.pinsListener = null
    }

    /** The helper (re)started, maybe in an older saved state: it gets the list again. */
    fun onHello() = sync(true)

    /** Settings may have changed: resync and redraw only what depends on what changed. */
    fun apply() {
        if (settingsKey() != lastKey) sync(false)
        skin.dockPos = settings.dockPos
        skin.dockSlots = settings.dockSize
        if (dockKey() != lastDockKey) refreshDock()
    }

    private fun settingsKey() = "${settings.androidFolder}|${settings.hiddenApps.sorted()}"
    private fun dockKey() = "${settings.dockPos}|${settings.dockSize}|${settings.dockApps}"

    fun sync(force: Boolean) {
        if (syncing) { again = true; forceNext = forceNext || force; return }
        syncing = true
        lastKey = settingsKey()
        val send = force || forceNext
        forceNext = false
        val ini = guest.boxFile("android.ini")
        thread(name = "android-apps") {
            val changed = runCatching { AndroidApps.sync(ctx, settings, ini) }
                .onFailure { Log.w(AndroidApps.TAG, "sync failed", it) }.getOrDefault(false)
            ui.post {
                syncing = false
                if (changed || send) guest.send("android sync")
                if (again) { again = false; sync(false) }
            }
        }
    }

    fun refreshDock() {
        lastDockKey = dockKey()
        skin.dockPos = settings.dockPos
        skin.dockSlots = settings.dockSize
        if (settings.dockPos == Settings.DOCK_OFF) { dockKeys = emptyList(); skin.dockIcons = emptyList(); return }
        val keys = settings.dockApps.take(settings.dockSize)
        thread(name = "dock-icons") {
            val all = AndroidApps.apps(ctx).associateBy { it.key }
            val icons = keys.map { k -> all[k]?.let { a -> runCatching { AndroidApps.render(AndroidApps.appIcon(ctx, a), 144) }.getOrNull() } }
            ui.post {
                if (lastDockKey != dockKey()) return@post  // changed again meanwhile: that refresh wins
                dockKeys = keys
                skin.dockIcons = icons
            }
        }
    }

    /** A dock slot was tapped: false when it is empty (the caller offers the dock picker). */
    fun dockTap(slot: Int): Boolean {
        val key = dockKeys.getOrNull(slot) ?: return false
        if (!AndroidApps.launchKey(ctx, key)) android.widget.Toast.makeText(ctx, "That app isn't installed any more", android.widget.Toast.LENGTH_SHORT).show()
        return true
    }

    /** "android ..." from the guest (a launcher in WM was tapped). */
    fun onGuest(line: String) {
        if (!line.startsWith("launch ")) return
        val name = line.removePrefix("launch ").trim()
        if (!AndroidApps.launch(ctx, name)) {
            val msg = if (name.startsWith("p") && !AndroidApps.isHome(ctx)) "Pinned shortcuts open only while PocketStylus is the phone's launcher"
                else "Couldn't open that Android app"
            android.widget.Toast.makeText(ctx, msg, android.widget.Toast.LENGTH_SHORT).show()
        }
    }
}
