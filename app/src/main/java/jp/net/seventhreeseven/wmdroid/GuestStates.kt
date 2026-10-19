package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.Context
import android.util.Log
import java.io.File
import java.text.DateFormat
import java.util.Date

/**
 * One saved state per ROM image: states/<profile>-<variant>. Windows Mobile's internal storage
 * (files, registry, installed programs) lives in that snapshot's RAM. Screen size, and on WM 6 the
 * memory size, are read again at boot (NativeBridge.adoptState), so changing them restarts Windows
 * Mobile into the same state. WM 5 clears its storage volume on a cold boot, so its folder also
 * names the memory size and only a screen change carries storage (a warm restart). A different
 * image (a Standard size served by another ROM) still starts fresh. Checked in the CLI for 6.5.3
 * Pro/Standard, 6.1.4 and 5.0.
 *
 * Older builds kept a folder per memory and resolution (states/<profile>-<variant>-<MB>-<W>x<H>).
 * The first start after the update asks once which of those to keep ([needsChoice], StateKeepActivity).
 */
object GuestStates {
    private const val TAG = "WMDroidStates"
    /** Smallest memory size a state can move to: the 64 MB bank plus the 32 MB storage volume (snapshot.cpp). */
    const val ADOPT_MIN_MB = 96
    /** Set once the per-size folders have been folded into one folder per ROM. */
    private const val UNIFIED = "state.unified"

    data class Target(val profile: OsProfile, val variant: ImageVariant, val file: File, val w: Int, val h: Int, val mem: Int) {
        /** WM 6 shares one folder across memory sizes. WM 5 keeps a folder per memory size. */
        val dirName get() = if (profile.coldBootKeepsStorage) "${profile.id}-${variant.id}" else "${profile.id}-${variant.id}-$mem"
    }

    /** A state folder from before one-folder-per-ROM, named <profile>-<variant>-<MB>-<W>x<H>. */
    data class SavedVersion(val dir: File, val mem: Int, val w: Int, val h: Int, val modified: Long, val resumed: Boolean) {
        fun describe(): String {
            val whenText = DateFormat.getDateTimeInstance(DateFormat.MEDIUM, DateFormat.SHORT).format(Date(modified))
            val kind = if (resumed) "last session" else "first boot"
            return "$mem MB · ${w}×$h · $kind · $whenText"
        }
    }

    /** One ROM image and the per-size states saved for it. */
    data class RomSaves(val profile: OsProfile, val variant: ImageVariant, val versions: List<SavedVersion>) {
        val key get() = "${profile.id}-${variant.id}"
        fun dirName(mem: Int) = if (profile.coldBootKeepsStorage) key else "$key-$mem"
    }

    /** What applying a configuration does to Windows Mobile's internal storage. */
    enum class Change {
        NONE,    // same state folder, or nothing saved that could be lost
        CARRY,   // the new folder starts from the current state: storage carried over
        RETURN,  // the new folder has a saved state of its own and returns to it
        FRESH,   // the new folder boots a new system
    }

    /** The phone's display aspect the emulator screen last used (EmulatorActivity), for [resolve] elsewhere. */
    @Volatile var lastAspect = 0.0

    fun statesDir(ctx: Context) = File(ctx.filesDir, "states")

    private fun hasState(dir: File) = File(dir, EmulatorActivity.RESUME).exists() || File(dir, EmulatorActivity.BOOT).exists()

    private fun global(ctx: Context) = ctx.getSharedPreferences("wmdroid", Context.MODE_PRIVATE)

    private fun osPrefs(ctx: Context, profileId: String) =
        ctx.getSharedPreferences("wmdroid.os.$profileId", Context.MODE_PRIVATE)

    private fun unified(ctx: Context) = global(ctx).getBoolean(UNIFIED, false)

    /**
     * The configuration [settings] boot: release, image, screen and memory. [screenRes] and [memMB]
     * (the stored values, 0 = the release's default) may be pending changes. [aspect] is the phone's
     * display aspect (height / width), asked only when the screen follows the phone's shape.
     */
    fun resolve(ctx: Context, settings: Settings, aspect: () -> Double, screenRes: String = settings.screenRes,
                memMB: Int = settings.memMB): Target? {
        val profile = settings.profile
        // One image per release: the installed one (older setups may name another variant).
        val variant = profile?.let { pr -> ImageStore.available(ctx, pr).firstOrNull() } ?: settings.variant
        val file = if (profile != null && variant != null) ImageStore.find(ctx, profile, variant) else null
        if (profile == null || variant == null || file == null) return null
        val mem = profile.memFor(memMB)
        // Screen resolution: the board tells Windows Mobile its screen size at boot, so every release
        // runs at any of these from its one image (checked in the CLI for WM 5, 6.1.4 and 6.5.3).
        // Standard releases instead run one of their own sizes, each from the image built for it.
        if (!profile.touch) {
            val std = OsProfiles.standardScreen(profile, ImageStore.available(ctx, profile), screenRes, aspect())
            val stdVariant = std?.let { s -> profile.variants.firstOrNull { it.id == s.variantId } }
            if (std != null && stdVariant != null)
                return Target(profile, stdVariant, ImageStore.find(ctx, profile, stdVariant) ?: file, std.width, std.height, mem)
        }
        val res = Regex("""(\d+)x(\d+)""").matchEntire(if (settings.incognito) Settings.SCREEN_AUTO else screenRes)
        val w = res?.groupValues?.get(1)?.toInt() ?: variant.width
        var h = res?.groupValues?.get(2)?.toInt() ?: variant.height
        if (res == null) {
            // Phone shape: as tall as the phone's display area; within 2% the image's own size stays.
            val a = aspect()
            if (a > 1.0 && Math.abs(Math.log((variant.height.toDouble() / variant.width) / a)) > 0.02)
                h = (Math.round(w * a).toInt() / 2 * 2).coerceIn(w, w * 2)
        }
        // A release with no layout above its cap (original 6.5: 800) stays there. The phone shape
        // on a tall cover is 852, and 854 is the same empty Today page.
        if (profile.maxHeight > 0 && h > profile.maxHeight) h = profile.maxHeight
        // "Resize screen for dock": the tall 480-wide screen (the phone's 852, or 854) gives up
        // the bottom strip and Windows Mobile boots at 480×800, which is where the dock sits.
        if (!settings.incognito && settings.dockResizeScreen && w == 480 && h > 800) h = 800
        // An installed image built for exactly this size boots instead: 6.5.3 Pro's FWVGA image at
        // 480×800 leaves the Start button stuck pressed, its WVGA image does not.
        val exact = profile.variants.firstOrNull { it.width == w && it.height == h && it.id != variant.id }
        val exactFile = exact?.let { ImageStore.find(ctx, profile, it) }
        if (exact != null && exactFile != null) return Target(profile, exact, exactFile, w, h, mem)
        return Target(profile, variant, file, w, h, mem)
    }

    /** [resolve] outside the emulator screen: the aspect it last used, else the remembered cover screen's. */
    fun resolveElsewhere(ctx: Context, settings: Settings, screenRes: String = settings.screenRes, memMB: Int = settings.memMB) =
        resolve(ctx, settings, {
            lastAspect.takeIf { it > 0 } ?: settings.coverAspect.takeIf { it > 0f }?.toDouble() ?: (854.0 / 480.0)
        }, screenRes, memMB)

    /**
     * The state [to] starts from when it has none of its own: the configuration last run
     * (Settings.lastStateDir) if it is the same image, else null.
     */
    fun migrationSource(ctx: Context, settings: Settings, to: Target): File? {
        val from = settings.lastStateDir.takeIf { it.isNotEmpty() && it != to.dirName } ?: return null
        val prefix = "${to.profile.id}-${to.variant.id}-"
        if (!from.startsWith(prefix)) return null
        val mem = from.removePrefix(prefix).substringBefore('-').toIntOrNull() ?: return null
        if (mem != to.mem && (to.mem < ADOPT_MIN_MB || !to.profile.coldBootKeepsStorage)) return null
        val dir = File(statesDir(ctx), from)
        return listOfNotNull(if (settings.restoresSession) File(dir, EmulatorActivity.RESUME) else null, File(dir, EmulatorActivity.BOOT))
            .firstOrNull { it.exists() }
    }

    /** What switching from the configuration last run to [to] does to the internal storage. */
    fun change(ctx: Context, settings: Settings, to: Target?): Change {
        if (to == null) return Change.NONE
        val current = settings.lastStateDir
        if (current.isEmpty() || to.dirName == current) return Change.NONE
        if (hasState(File(statesDir(ctx), to.dirName))) return Change.RETURN
        if (migrationSource(ctx, settings, to) != null) return Change.CARRY
        // Nothing saved for the current configuration: nothing is lost either.
        return if (hasState(File(statesDir(ctx), current))) Change.FRESH else Change.NONE
    }

    /**
     * Asks before a change that does not carry the internal storage over ([Change.FRESH] or
     * [Change.RETURN]); runs [apply] at once otherwise, or when the user agrees, else [cancel].
     */
    fun confirm(activity: Activity, change: Change, apply: () -> Unit, cancel: () -> Unit) {
        Log.i(TAG, "settings change: $change")
        val message = when (change) {
            Change.FRESH -> "Windows Mobile starts fresh: the files, settings and programs in its " +
                "internal storage are not carried over. " +
                "The storage card is not affected, and switching back returns to the current state."
            Change.RETURN -> "Windows Mobile returns to the state saved when it last ran with this setting. Changes " +
                "made since then to files, settings and programs in internal storage stay with the current " +
                "setting and come back when you switch back. The storage card is not affected."
            else -> { apply(); return }
        }
        com.google.android.material.dialog.MaterialAlertDialogBuilder(activity)
            .setTitle(if (change == Change.FRESH) "Start with empty internal storage?" else "Return to an earlier state?")
            .setMessage(message)
            .setPositiveButton("Restart") { _, _ -> apply() }
            .setNegativeButton("Cancel") { _, _ -> cancel() }
            .setOnCancelListener { cancel() }
            .show()
    }

    /** True when an update left a ROM with more than one saved version and the user has not picked yet. */
    fun needsChoice(ctx: Context) = !unified(ctx) && legacyRoms(ctx).any { it.versions.size > 1 }

    /**
     * Folds per-size folders into one per ROM when every ROM has a single version, and pins the RAM
     * size of ROMs that already exist so only a new install or a new ROM starts at the maximum.
     */
    fun settleQuiet(ctx: Context) {
        if (unified(ctx) || needsChoice(ctx)) return
        keep(ctx, emptyMap())
    }

    /** Per-size state folders, grouped by ROM image, newest version first. */
    fun legacyRoms(ctx: Context): List<RomSaves> {
        val versions = linkedMapOf<String, MutableList<SavedVersion>>()
        val roms = linkedMapOf<String, Pair<OsProfile, ImageVariant>>()
        val dirs = statesDir(ctx).listFiles()?.filter { it.isDirectory } ?: return emptyList()
        for (dir in dirs.sortedBy { it.name }) {
            val parsed = parseLegacy(dir) ?: continue
            val (profile, variant, version) = parsed
            val key = "${profile.id}-${variant.id}"
            roms[key] = profile to variant
            versions.getOrPut(key) { mutableListOf() }.add(version)
        }
        return roms.map { (key, rom) ->
            RomSaves(rom.first, rom.second, versions[key].orEmpty().sortedByDescending { it.modified })
        }
    }

    /** The version [rom] should offer first: the folder this OS last booted, else the newest. */
    fun preferredIndex(ctx: Context, rom: RomSaves): Int {
        val last = osPrefs(ctx, rom.profile.id).getString("state.last", "") ?: ""
        val i = rom.versions.indexOfFirst { it.dir.name == last }
        return if (i >= 0) i else 0
    }

    /**
     * Keeps [picks] (ROM key to the legacy folder name) and, for a ROM with one version, that version.
     * Removes the other per-size folders. False when a ROM with several versions has no pick.
     */
    fun keep(ctx: Context, picks: Map<String, String>): Boolean {
        val roms = legacyRoms(ctx)
        if (roms.any { it.versions.size > 1 && picks[it.key] == null }) return false
        val g = global(ctx)
        val currentProfile = g.getString("profile", null)
        val currentVariant = g.getString("variant", null)
        val keptMem = HashMap<String, Int>()
        for (rom in roms) {
            val chosen = rom.versions.firstOrNull { it.dir.name == picks[rom.key] } ?: rom.versions.singleOrNull() ?: continue
            val dest = File(statesDir(ctx), rom.dirName(chosen.mem))
            if (!moveDir(chosen.dir, dest)) return false
            for (other in rom.versions)
                if (other.dir.exists() && other.dir.absolutePath != dest.absolutePath) other.dir.deleteRecursively()
            val prefs = osPrefs(ctx, rom.profile.id)
            val last = prefs.getString("state.last", "") ?: ""
            if (rom.versions.any { it.dir.name == last }) prefs.edit().putString("state.last", dest.name).apply()
            val current = rom.profile.id == currentProfile && rom.variant.id == currentVariant
            if (current || !keptMem.containsKey(rom.profile.id)) keptMem[rom.profile.id] = chosen.mem
            Log.i(TAG, "kept ${chosen.dir.name} as ${dest.name}")
        }
        pinExistingRam(ctx, keptMem)
        g.edit().putBoolean(UNIFIED, true).apply()
        return true
    }

    /**
     * Memory and screen stored in a snapshot header (snapshot.cpp: mem, then width, then height,
     * after four little-endian words). Null when the file is shorter than that header.
     */
    fun snapshotConfig(file: File): Triple<Int, Int, Int>? {
        val buf = ByteArray(28)
        val n = file.inputStream().use { it.read(buf) }
        if (n < 28) return null
        fun u32(o: Int) = (buf[o].toInt() and 0xff) or ((buf[o + 1].toInt() and 0xff) shl 8) or
            ((buf[o + 2].toInt() and 0xff) shl 16) or ((buf[o + 3].toInt() and 0xff) shl 24)
        return Triple(u32(16), u32(20), u32(24))
    }

    private val LEGACY_NAME = Regex("""^(.+)-(\d+)-(\d+)x(\d+)$""")

    private fun parseLegacy(dir: File): Triple<OsProfile, ImageVariant, SavedVersion>? {
        val m = LEGACY_NAME.matchEntire(dir.name) ?: return null
        val prefix = m.groupValues[1]
        val profile = OsProfiles.all.sortedByDescending { it.id.length }
            .firstOrNull { prefix.startsWith("${it.id}-") } ?: return null
        val variant = profile.variants.firstOrNull { it.id == prefix.removePrefix("${profile.id}-") } ?: return null
        val resume = File(dir, EmulatorActivity.RESUME)
        val boot = File(dir, EmulatorActivity.BOOT)
        val file = when {
            resume.isFile -> resume
            boot.isFile -> boot
            else -> return null
        }
        val version = SavedVersion(dir, m.groupValues[2].toInt(), m.groupValues[3].toInt(), m.groupValues[4].toInt(),
            file.lastModified(), resume.isFile)
        return Triple(profile, variant, version)
    }

    /** Move a per-size folder onto its ROM's folder. A rename stays on the same filesystem. */
    private fun moveDir(from: File, dest: File): Boolean {
        if (from.absolutePath == dest.absolutePath) return hasState(dest)
        dest.parentFile?.mkdirs()
        if (!dest.exists() && from.renameTo(dest)) return hasState(dest)
        if (!dest.exists()) dest.mkdirs()
        for (f in from.listFiles().orEmpty())
            if (f.isFile) f.copyTo(File(dest, f.name), overwrite = true)
        if (!hasState(dest)) return false
        from.deleteRecursively()
        return true
    }

    /**
     * A ROM that already has settings or saves keeps the RAM it was using. Memory left at 0 (the old
     * "default", 128 MB except 6.5.3 Professional at 256) is written down so the new default of 256 MB
     * applies only to a new install and to a ROM opened for the first time. WM 5 takes the memory of
     * the version that was kept, because a different size would boot without that storage.
     */
    private fun pinExistingRam(ctx: Context, keptMem: Map<String, Int>) {
        for (profile in OsProfiles.all) {
            val p = osPrefs(ctx, profile.id)
            if (!p.getBoolean("os.seeded", false) && !keptMem.containsKey(profile.id)) continue
            val current = if (p.contains("memMB")) p.getInt("memMB", 0) else 0
            val kept = keptMem[profile.id]
            if (!profile.coldBootKeepsStorage && kept != null) {
                p.edit().putInt("memMB", kept).apply()
                continue
            }
            if (current >= profile.minMemMB) continue
            p.edit().putInt("memMB", kept ?: previousDefaultMem(profile)).apply()
        }
    }

    /** The Memory a stored 0 meant before new ROMs started at 256 MB. */
    private fun previousDefaultMem(profile: OsProfile) = if (profile.id == "wm653pro") 256 else 128
}
