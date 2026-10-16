package jp.net.seventhreeseven.wmdroid

import android.content.Context

/**
 * Persistent user choices: which OS/image to boot and emulator options. Which OS and image, and the
 * cover screen's measured shape, are shared; every other setting belongs to the chosen OS (each
 * Windows Mobile keeps its own device settings, as separate phones would). Switching OS takes
 * effect with a new Settings (the activities are recreated).
 */
class Settings(ctx: Context) {
    private val g = ctx.getSharedPreferences("wmdroid", Context.MODE_PRIVATE)
    private val p = prefs(ctx)

    var profileId: String?
        get() = g.getString("profile", null)
        set(v) = g.edit().putString("profile", v).apply()

    var variantId: String?
        get() = g.getString("variant", null)
        set(v) = g.edit().putString("variant", v).apply()

    /** 0 = profile default. */
    var memMB: Int
        get() = p.getInt("memMB", 0)
        set(v) = p.edit().putInt("memMB", v).apply()

    var showStats: Boolean
        get() = p.getBoolean("stats", true)
        set(v) = p.edit().putBoolean("stats", v).apply()

    var storageMode: String
        get() = p.getString("storage.mode", StorageBridge.MODE_PRIVATE)!!
        set(v) = p.edit().putString("storage.mode", v).apply()

    /** A skin JSON imported by the user (tools/skin-designer.html format); empty = built-in skin. */
    var customSkin: String
        get() = p.getString("skin.json", "")!!
        set(v) = p.edit().putString("skin.json", v).apply()

    /** Chosen-folders mode: the card's own (writable) root; empty = WMDroid's own folder. */
    var storageRoot: String
        get() = p.getString("storage.root", "")!!
        set(v) = p.edit().putString("storage.root", v).apply()

    /** Chosen-folders mode: names given to folders on the card (path to name), else the folder's own. */
    var storageFolderNames: Map<String, String>
        get() = p.getString("storage.names", "")!!.split('\n').mapNotNull {
            val t = it.indexOf('\t'); if (t > 0) it.substring(0, t) to it.substring(t + 1) else null
        }.toMap()
        set(v) = p.edit().putString("storage.names", v.entries.joinToString("\n") { "${it.key}\t${it.value}" }).apply()

    var storageFolders: List<String>
        get() = p.getString("storage.folders", "")!!.split('\n').filter { it.isNotEmpty() }
        set(v) = p.edit().putString("storage.folders", v.joinToString("\n")).apply()

    /** Guest screen: the image's native size, or the phone's aspect ratio (taller than 800 allowed). */
    var screenMode: String
        get() = p.getString("screen.mode", SCREEN_NATIVE)!!
        set(v) = p.edit().putString("screen.mode", v).apply()

    /**
     * Windows Mobile's logical DPI (HKLM\Drivers\Display\GPE LogicalPixelsX/Y), 0 = the image's own.
     * The screen keeps its pixels; 96 draws everything half the size of 192. The helper applies it
     * with one Windows Mobile restart (guest/Helper/Dpi.cs).
     */
    var guestDpi: Int
        get() = p.getInt("guest.dpi", 0)
        set(v) = p.edit().putInt("guest.dpi", v).apply()

    /** Upscaling filter: 0 smooth, 1 sharp pixels, 2 enhanced. */
    var screenFilter: Int
        get() = p.getInt("screen.filter", 1)
        set(v) = p.edit().putInt("screen.filter", v).apply()

    /** Save the session when leaving the app and continue it on the next start. */
    var resumeSession: Boolean
        get() = p.getBoolean("resume", true)
        set(v) = p.edit().putBoolean("resume", v).apply()

    /** Carrier bar and on-screen keys around the display. */
    var keysOn: Boolean
        get() = p.getBoolean("skin.keys", true)
        set(v) = p.edit().putBoolean("skin.keys", v).apply()

    /** The phone's GPS on the guest's COM1 (used only while Windows Mobile has the port open). */
    var gpsOn: Boolean
        get() = p.getBoolean("bridge.gps", true)
        set(v) = p.edit().putBoolean("bridge.gps", v).apply()

    /** Camera bridge: Windows Mobile's camera uses the phone's front camera instead of the back one. */
    var cameraFront: Boolean
        get() = p.getBoolean("camera.front", false)
        set(v) = p.edit().putBoolean("camera.front", v).apply()

    /** Camera bridge: how strong the 2009 phone-camera look is when on, 1..100. */
    var cameraEraLevel: Int
        get() = p.getInt("camera.era.level", 60).coerceIn(1, 100)
        set(v) = p.edit().putInt("camera.era.level", v.coerceIn(1, 100)).apply()

    /** Camera bridge: the 2009 look on or off. */
    var cameraEraOn: Boolean
        get() = p.getBoolean("camera.era.on", p.getBoolean("camera.era", false))
        set(v) = p.edit().putBoolean("camera.era.on", v).apply()

    /** The look's strength as the core takes it (0 = off). */
    val cameraEraEffective: Int get() = if (cameraEraOn) cameraEraLevel else 0

    /** Windows Mobile's camera: "off" (no phone camera), "rear" or "front". */
    var cameraMode: String
        get() = p.getString("camera.mode", null) ?: if (cameraFront) "front" else "rear"
        set(v) { p.edit().putString("camera.mode", v).apply(); if (v != "off") cameraFront = v == "front" }

    /** Rotation: how long the phone must hold a new orientation, in ms. */
    var rotationDelayMs: Int
        get() = p.getInt("rot.delay", 900)
        set(v) = p.edit().putInt("rot.delay", v.coerceIn(200, 3000)).apply()

    /** Rotation: 0 (needs a firm turn) .. 100 (turns at a slight tilt). */
    var rotationSensitivity: Int
        get() = p.getInt("rot.sens", 50)
        set(v) = p.edit().putInt("rot.sens", v.coerceIn(0, 100)).apply()

    /** Light reflections on the keys that follow the phone's tilt (uses the motion sensors). */
    /** Gyro key shine: brightness and width (how far the reflection spreads), 0..100. */
    var shineBrightness: Int
        get() = p.getInt("skin.shine.bright", 50)
        set(v) = p.edit().putInt("skin.shine.bright", v.coerceIn(0, 100)).apply()

    var shineWidth: Int
        get() = p.getInt("skin.shine.width", 50)
        set(v) = p.edit().putInt("skin.shine.width", v.coerceIn(0, 100)).apply()

    var keyShine: Boolean
        get() = p.getBoolean("skin.shine", true)
        set(v) = p.edit().putBoolean("skin.shine", v).apply()

    /** Key backlight on the keys' glyphs (SkinLayout.setBacklight), lit on a key press or screen touch. */
    var backlightOn: Boolean
        get() = p.getBoolean("skin.backlight", false)
        set(v) = p.edit().putBoolean("skin.backlight", v).apply()

    /** Key backlight: brightness 0..100. */
    var backlightIntensity: Int
        get() = p.getInt("skin.backlight.intensity", 70)
        set(v) = p.edit().putInt("skin.backlight.intensity", v.coerceIn(0, 100)).apply()

    /** Key backlight: seconds it stays lit after the last touch (0 = always on). */
    var backlightTimeout: Int
        get() = p.getInt("skin.backlight.timeout", 10)
        set(v) = p.edit().putInt("skin.backlight.timeout", v.coerceAtLeast(0)).apply()

    /** Key backlight colour: "white", "blue", "green", "amber", "red" or "theme" (the generated theme's accent). */
    var backlightColor: String
        get() = p.getString("skin.backlight.color", "white")!!
        set(v) = p.edit().putString("skin.backlight.color", v).apply()

    /** Key backlight: 0 (even) .. 100 (a cheap membrane keypad lit from the side: uneven, patchy, bleeding). */
    var backlightDirt: Int
        get() = p.getInt("skin.backlight.dirt", 25)
        set(v) = p.edit().putInt("skin.backlight.dirt", v.coerceIn(0, 100)).apply()

    /** What the four on-screen keys send, left to right (Windows virtual keys). */
    var softKeyVks: IntArray
        get() {
            // vk3: App Launch 1/2 became WM's Button 2/3 (Button 1 is the Start key); older
            // assignments (vk2) move up one button.
            // Button 1 (Start) moved from VK 0xC1 (193, the guest keyboard's Fn key) to 0xC5 (197):
            // a vk3 assignment of 193 becomes 197.
            val saved = p.getString("skin.keys.vk3", null)?.split(',')
                ?.map { v -> if (v.trim() == "193") "197" else v.trim() }?.joinToString(",")
                ?: p.getString("skin.keys.vk2", null)?.split(',')
                ?.map { v -> when (v.trim()) { "193" -> "194"; "194" -> "195"; else -> v.trim() } }?.joinToString(",")
            return (saved ?: "114,194,195,115").split(',').mapNotNull { it.toIntOrNull() }
                .toIntArray().takeIf { it.size == 4 } ?: intArrayOf(0x72, 0xC2, 0xC3, 0x73)
        }
        set(v) = p.edit().putString("skin.keys.vk3", v.joinToString(",")).apply()

    /** Boost for the guest's sound in dB (WM's driver is quiet by today's standards). */
    var audioBoostDb: Int
        get() = p.getInt("audio.boost", 12)
        set(v) = p.edit().putInt("audio.boost", v).apply()

    /** On-screen D-pad row under the keys. */
    var dpadOn: Boolean
        get() = p.getBoolean("skin.dpad", false)
        set(v) = p.edit().putBoolean("skin.dpad", v).apply()

    /** SkinLayout.BACKDROP_* */
    var backdrop: String
        get() = p.getString("skin.backdrop", SkinLayout.BACKDROP_BLACK)!!
        set(v) = p.edit().putString("skin.backdrop", v).apply()

    /** Show the phone's wallpaper behind the backdrop. */
    var showWallpaper: Boolean
        get() = p.getBoolean("skin.wallpaper", false)
        set(v) = p.edit().putBoolean("skin.wallpaper", v).apply()

    /** 0..100: how opaque the backdrop is over the wallpaper (with [showWallpaper]). */
    var backdropOpacity: Int
        get() = p.getInt("skin.backdrop.opacity", 40)
        set(v) = p.edit().putInt("skin.backdrop.opacity", v).apply()

    /** Wallpaper blur radius 0..25. */
    var wallpaperBlur: Int
        get() = p.getInt("skin.wp.blur", 12)
        set(v) = p.edit().putInt("skin.wp.blur", v).apply()

    /** Turn Windows Mobile's display with the phone (the app itself stays portrait). */
    var rotateGuest: Boolean
        get() = p.getBoolean("rotate.guest", true)
        set(v) = p.edit().putBoolean("rotate.guest", v).apply()

    /** The phone's own number for Windows Mobile ("My number"); empty = what Android knows (PhoneNumber). Shared by all OSes. */
    var myNumber: String
        get() = g.getString("phone.number", "")!!
        set(v) = g.edit().putString("phone.number", v).apply()

    /** Carrier name drawn on the skin; empty = the phone's current network. */
    var skinCarrier: String
        get() = p.getString("skin.carrier", "")!!
        set(v) = p.edit().putString("skin.carrier", v).apply()

    var volumeUp: Keymap.KeyAction
        get() = Keymap.KeyAction.of(p.getString("key.volup", null), Keymap.KeyAction.VOLUME)
        set(v) = p.edit().putString("key.volup", v.id).apply()

    var volumeDown: Keymap.KeyAction
        get() = Keymap.KeyAction.of(p.getString("key.voldown", null), Keymap.KeyAction.VOLUME)
        set(v) = p.edit().putString("key.voldown", v.id).apply()

    /**
     * What Windows Mobile's hardware Button [n] (1-4) does: an action id of guest/Helper/Buttons.cs
     * ("okclose", "start", ...), or GuestSettings.BUTTON_WM = leave Windows Mobile's own setting.
     * Button 4 (the OK key) defaults to OK/Close.
     */
    fun hwButton(n: Int): String = p.getString("hwbutton.$n", null) ?: if (n == 4) "okclose" else GuestSettings.BUTTON_WM
    fun setHwButton(n: Int, v: String) = p.edit().putString("hwbutton.$n", v).apply()

    /** WM's own Buttons assignments as the helper last reported them (Keymap.HwButtons; null = stock). */
    var hwButtonMap: String?
        get() = p.getString("hwbutton.map", null)
        set(v) = p.edit().putString("hwbutton.map", v).apply()

    /** Lock Windows Mobile when the phone's screen turns off while it runs. */
    var lockOnScreenOff: Boolean
        get() = p.getBoolean("lock.screenoff", false)
        set(v) = p.edit().putBoolean("lock.screenoff", v).apply()

    /** Put the phone's wallpaper (cut to the display) on Windows Mobile's home screen. */
    var syncWallpaper: Boolean
        get() = p.getBoolean("sync.wallpaper", true)
        set(v) = p.edit().putBoolean("sync.wallpaper", v).apply()

    /** Theme Windows Mobile in the phone's current (Material You) colours. */
    var syncTheme: Boolean
        get() = p.getBoolean("sync.theme", true)
        set(v) = p.edit().putBoolean("sync.theme", v).apply()

    /** Whether our theme is on the guest (so turning [syncTheme] off can put the old one back). */
    var themeApplied: Boolean
        get() = p.getBoolean("theme.applied", false)
        set(v) = p.edit().putBoolean("theme.applied", v).apply()

    /** Mirror the phone's screen brightness into Windows Mobile's backlight level. */
    var syncBrightness: Boolean
        get() = p.getBoolean("sync.brightness", true)
        set(v) = p.edit().putBoolean("sync.brightness", v).apply()

    /** Two-way clipboard between the phone and Windows Mobile (ClipboardBridge). */
    var syncClipboard: Boolean
        get() = p.getBoolean("sync.clipboard", true)
        set(v) = p.edit().putBoolean("sync.clipboard", v).apply()

    /** The phone's keyboard instead of Windows Mobile's on-screen keyboard (ImeBridge). */
    var androidIme: Boolean
        get() = p.getBoolean("ime.android", false)
        set(v) = p.edit().putBoolean("ime.android", v).apply()

    /** Copy files the phone receives over Bluetooth to \Storage Card\Bluetooth (BluetoothBridge). */
    var bluetoothReceive: Boolean
        get() = p.getBoolean("bt.receive", true)
        set(v) = p.edit().putBoolean("bt.receive", v).apply()

    /** MediaStore Downloads id at first use; earlier Bluetooth files are not copied. -1 = unset. */
    var bluetoothBaseline: Long
        get() = p.getLong("bt.baseline", -1L)
        set(v) = p.edit().putLong("bt.baseline", v).apply()

    /** MediaStore ids of Bluetooth files already copied into the guest. */
    var bluetoothCopied: Set<String>
        get() = p.getStringSet("bt.copied", emptySet())!!.toSet()
        set(v) = p.edit().putStringSet("bt.copied", v).apply()

    /** Open foldable: landscape display on top, keyboard below (see FoldSkin). */
    var foldMode: Boolean
        get() = p.getBoolean("fold.mode", true)
        set(v) = p.edit().putBoolean("fold.mode", v).apply()

    /** Fold keyboard: "skin" (drawn slide-out keyboard) or "ime" (the phone's own keyboard). */
    var foldKeyboard: String
        get() = p.getString("fold.keyboard", "skin")!!
        set(v) = p.edit().putString("fold.keyboard", v).apply()

    /** Fold: row of hotkeys (soft keys etc.) above the keyboard. */
    var foldHotkeys: Boolean
        get() = p.getBoolean("fold.hotkeys", true)
        set(v) = p.edit().putBoolean("fold.hotkeys", v).apply()

    /** Fold: hinge size over the crease, 0..100 (0 = hidden, 50 = default). */
    var hingeSize: Int
        get() = p.getInt("fold.hinge", 50)
        set(v) = p.edit().putInt("fold.hinge", v).apply()

    /** Fold: draw a hinge band across the middle of the inner display (see SkinLayout.hingeOn). */
    var hingeOn: Boolean
        get() = p.getBoolean("fold.hinge.on", false)
        set(v) = p.edit().putBoolean("fold.hinge.on", v).apply()

    /** Status bar data letters: "auto" (the phone's network), "gsm" (G/E/3G/H) or "cdma" (1X/EV). */
    var netIndicator: String
        get() = p.getString("status.net", "auto")!!
        set(v) = p.edit().putString("status.net", v).apply()

    /** Fold hinge look: "shadow", "classic", "crease" or "void". */
    var hingeStyle: String
        get() = p.getString("fold.hinge.style", "crease")!!
        set(v) = p.edit().putString("fold.hinge.style", v).apply()

    /** Fold hinge: thickness 0..100, offset from the fold -50..50, opacity 0..100. */
    var hingeThickness: Int
        get() = p.getInt("fold.hinge.thick", 30)
        set(v) = p.edit().putInt("fold.hinge.thick", v.coerceIn(0, 100)).apply()

    var hingeLocation: Int
        get() = p.getInt("fold.hinge.loc", 0)
        set(v) = p.edit().putInt("fold.hinge.loc", v.coerceIn(-50, 50)).apply()

    var hingeOpacity: Int
        get() = p.getInt("fold.hinge.alpha", 70)
        set(v) = p.edit().putInt("fold.hinge.alpha", v.coerceIn(0, 100)).apply()

    /**
     * Height/width of the display area on the outer (cover) screen, remembered so the boot
     * resolution follows the cover screen even when the app starts with the phone open.
     */
    var coverAspect: Float
        get() = g.getFloat("fold.cover.aspect", 0f)
        set(v) = g.edit().putFloat("fold.cover.aspect", v).apply()

    /** Lock the phone (screen off) when Windows Mobile turns its display off. Needs [LockAdmin]. */
    var mirrorDisplayOff: Boolean
        get() = p.getBoolean("display.lockphone", false)
        set(v) = p.edit().putBoolean("display.lockphone", v).apply()

    /** Lock Windows Mobile when the app is closed or goes to the background. */
    var lockOnLeave: Boolean
        get() = p.getBoolean("lock.leave", false)
        set(v) = p.edit().putBoolean("lock.leave", v).apply()

    /** What happens when the app goes to the background: BG_*. */
    var backgroundMode: String
        get() = p.getString("bg.mode", BG_SUSPEND)!!
        set(v) = p.edit().putString("bg.mode", v).apply()

    /** Minutes of running in the background before BG_SUSPEND_AFTER / BG_HIBERNATE_AFTER act. */
    var backgroundMinutes: Int
        get() = p.getInt("bg.minutes", 5)
        set(v) = p.edit().putInt("bg.minutes", v).apply()

    /** The background mode keeps Windows Mobile running (at least for a while). */
    val runsInBackground: Boolean get() = backgroundMode in RUNNING_MODES

    /** Starts continue from resume.wms: "resume" is on, or hibernating (which relies on it) is chosen. */
    val restoresSession: Boolean
        get() = resumeSession || backgroundMode == BG_HIBERNATE || backgroundMode == BG_HIBERNATE_AFTER

    /** Debugging: keep the helper from patching the system on its own (set through adb). */
    val debugNoPatch: Boolean get() = p.getBoolean("debug.nopatch", false)

    fun bridgeEnabled(key: String): Boolean = p.getBoolean("bridge.$key", false)
    fun setBridgeEnabled(key: String, on: Boolean) = p.edit().putBoolean("bridge.$key", on).apply()

    val profile: OsProfile? get() = OsProfiles.byId(profileId)
    val variant: ImageVariant?
        get() = profile?.let { pr -> pr.variants.firstOrNull { it.id == variantId } ?: pr.variants.first() }

    /** One-time moves to new defaults. */
    fun migrate() {
        if (p.getString("skin.backdrop", null) == "wallpaper") {
            // The wallpaper used to be a backdrop of its own: now black over the wallpaper.
            val shown = p.getInt("skin.wp.opacity", 60)
            p.edit().putString("skin.backdrop", SkinLayout.BACKDROP_BLACK).putBoolean("skin.wallpaper", true)
                .putInt("skin.backdrop.opacity", 100 - shown).apply()
        }
        if (!p.getBoolean("migr.fwvga", false)) {
            // 6.5.3 Pro now defaults to the tall 480x854 image.
            if (profileId == "wm653pro" && (variantId == null || variantId == "wvga")) variantId = "fwvga"
            p.edit().putBoolean("migr.fwvga", true).apply()
        }
    }

    fun effectiveMemMB(): Int = if (memMB > 0) memMB else profile?.defaultMemMB ?: 128

    companion object {
        /**
         * The chosen OS's own settings ("wmdroid.os.<profile>"). The first time an OS is used its
         * file starts as a copy of the shared one (where all settings lived before), so the current
         * setup carries over and each OS then changes only its own.
         */
        fun prefs(ctx: Context): android.content.SharedPreferences {
            val g = ctx.getSharedPreferences("wmdroid", Context.MODE_PRIVATE)
            val id = g.getString("profile", null) ?: return g
            val p = ctx.getSharedPreferences("wmdroid.os.$id", Context.MODE_PRIVATE)
            if (!p.getBoolean("os.seeded", false)) {
                val e = p.edit()
                for ((k, v) in g.all) {
                    // RAM size is the image's own (0 = its profile default): not carried to another OS.
                    if (k == "profile" || k == "variant" || k == "fold.cover.aspect" || k == "memMB") continue
                    when (v) {
                        is Boolean -> e.putBoolean(k, v)
                        is Int -> e.putInt(k, v)
                        is Long -> e.putLong(k, v)
                        is Float -> e.putFloat(k, v)
                        is String -> e.putString(k, v)
                        is Set<*> -> e.putStringSet(k, v.filterIsInstance<String>().toSet())
                    }
                }
                e.putBoolean("os.seeded", true).commit()
            }
            return p
        }

        const val SCREEN_NATIVE = "native"
        const val SCREEN_PHONE = "phone"

        // Background behaviour. Suspend: paused in memory. Hibernate: saved to resume.wms and
        // released. Running: on at a reduced CPU share under a foreground service.
        const val BG_RUN = "run"
        const val BG_SUSPEND = "suspend"
        const val BG_SUSPEND_AFTER = "suspend.after"
        const val BG_HIBERNATE_AFTER = "hibernate.after"
        const val BG_HIBERNATE = "hibernate"
        val RUNNING_MODES = setOf(BG_RUN, BG_SUSPEND_AFTER, BG_HIBERNATE_AFTER)
        val BACKGROUND_MINUTES = listOf(1, 5, 15, 30, 60)
    }
}
