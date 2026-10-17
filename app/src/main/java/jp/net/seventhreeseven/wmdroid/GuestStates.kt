package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.content.Context
import android.util.Log
import java.io.File

/**
 * Saved states are kept per configuration: states/<profile>-<variant>-<MB>-<W>x<H>. Windows Mobile's
 * internal storage (files, registry, installed programs) lives in those states, so a Memory or
 * Screen resolution change used to boot an empty system.
 *
 * Now a configuration without a state of its own starts from the one last run when that is the
 * same image (NativeBridge.adoptState: the core restores RAM and flash and Windows Mobile boots
 * again at the new size; its storage volume is the first 32 MB of the RAM extension, so any size
 * from 96 MB keeps it). WM 5 clears that volume on a cold boot, so there only screen changes carry
 * it (a warm restart, OsProfile.coldBootKeepsStorage). When it is not possible (WM 5 memory, a
 * Standard screen size served by another image) or the target has a state of its own, the user is
 * asked first ([confirm]); checked in the CLI for 6.5.3 Pro/Standard, 6.1.4 and 5.0.
 */
object GuestStates {
    private const val TAG = "WMDroidStates"
    /** Smallest memory size a state can move to: the 64 MB bank plus the 32 MB storage volume (snapshot.cpp). */
    const val ADOPT_MIN_MB = 96

    data class Target(val profile: OsProfile, val variant: ImageVariant, val file: File, val w: Int, val h: Int, val mem: Int) {
        val dirName get() = "${profile.id}-${variant.id}-$mem-${w}x$h"
    }

    /** What applying a configuration does to Windows Mobile's internal storage. */
    enum class Change {
        NONE,    // same configuration, or nothing saved that could be lost
        CARRY,   // the new configuration starts from the current state: storage carried over
        RETURN,  // the new configuration has a saved state of its own and returns to it
        FRESH,   // the new configuration boots a new system
    }

    /** The phone's display aspect the emulator screen last used (EmulatorActivity), for [resolve] elsewhere. */
    @Volatile var lastAspect = 0.0

    fun statesDir(ctx: Context) = File(ctx.filesDir, "states")

    private fun hasState(dir: File) = File(dir, EmulatorActivity.RESUME).exists() || File(dir, EmulatorActivity.BOOT).exists()

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
        val res = Regex("""(\d+)x(\d+)""").matchEntire(screenRes)
        val w = res?.groupValues?.get(1)?.toInt() ?: variant.width
        var h = res?.groupValues?.get(2)?.toInt() ?: variant.height
        if (res == null) {
            // Phone shape: as tall as the phone's display area; within 2% the image's own size stays.
            val a = aspect()
            if (a > 1.0 && Math.abs(Math.log((variant.height.toDouble() / variant.width) / a)) > 0.02)
                h = (Math.round(w * a).toInt() / 2 * 2).coerceIn(w, w * 2)
        }
        // "Resize screen for dock": the tall 480-wide screen (the phone's 852, or 854) gives up
        // the bottom strip and Windows Mobile boots at 480×800, which is where the dock sits.
        if (settings.dockResizeScreen && w == 480 && h > 800) h = 800
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
            Change.FRESH -> "This configuration starts fresh: the files, settings and programs in Windows Mobile's " +
                "internal storage are not carried over to it. " +
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
}
