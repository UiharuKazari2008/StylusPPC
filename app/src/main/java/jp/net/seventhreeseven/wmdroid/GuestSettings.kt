package jp.net.seventhreeseven.wmdroid

import java.io.File

/**
 * The settings Windows Mobile's own WMDroid settings app shows (guest/Helper/SettingsApp.cs).
 * The app writes this catalogue with current values to the mailbox as settings.ini; the guest
 * builds its pages from it and sends "set key=value" back. Live settings apply at once; the few
 * marked restart apply when the guest app is closed with Done (Windows Mobile restarts).
 *
 * settings.ini: one entry per line, tab-separated:
 *   page, key, kind (bool|choice|slider|action), label, value, restart (0|1),
 *   options (value=label;...) for a choice, min:max for a slider; an action's value is its command
 */
object GuestSettings {
    enum class Kind(val id: String) { BOOL("bool"), CHOICE("choice"), SLIDER("slider"), ACTION("action"), COLOR("color") }

    class Entry(
        val page: String, val key: String, val kind: Kind, val label: String,
        val get: (Settings) -> String, val set: (Settings, String) -> Unit,
        val options: List<Pair<String, String>> = emptyList(), val min: Int = 0, val max: Int = 100,
        val restart: Boolean = false,
    )

    private fun bool(page: String, key: String, label: String, get: (Settings) -> Boolean, set: (Settings, Boolean) -> Unit, restart: Boolean = false) =
        Entry(page, key, Kind.BOOL, label, { if (get(it)) "1" else "0" }, { s, v -> set(s, v == "1") }, restart = restart)

    private fun choice(page: String, key: String, label: String, options: List<Pair<String, String>>,
                       get: (Settings) -> String, set: (Settings, String) -> Unit, restart: Boolean = false) =
        Entry(page, key, Kind.CHOICE, label, get, { s, v -> if (options.any { it.first == v }) set(s, v) }, options, restart = restart)

    private fun slider(page: String, key: String, label: String, min: Int, max: Int, get: (Settings) -> Int, set: (Settings, Int) -> Unit) =
        Entry(page, key, Kind.SLIDER, label, { get(it).toString() }, { s, v -> v.toIntOrNull()?.let { set(s, it.coerceIn(min, max)) } },
            min = min, max = max)

    /**
     * A colour ("#RRGGBB", or "auto"). Android shows a colour picker; Windows Mobile's settings app
     * gets it as a choice of the preset colours (plus the current one when it is custom).
     */
    private fun color(page: String, key: String, label: String, get: (Settings) -> String, set: (Settings, String) -> Unit) =
        Entry(page, key, Kind.COLOR, label, get, { s, v -> if (v == "auto" || Regex("#[0-9A-Fa-f]{6}").matches(v)) set(s, v.uppercase().replace("AUTO", "auto")) },
            options = COLOR_PRESETS)

    val COLOR_PRESETS = listOf("auto" to "Glass (skin default)", "#101215" to "Black", "#F2F3F5" to "White",
        "#4A4F57" to "Graphite", "#B3262E" to "Red", "#1F5FC0" to "Blue", "#1E8E4A" to "Green",
        "#6B3FB8" to "Purple", "#E07A1F" to "Orange", "#D9468F" to "Pink")

    /** A button: the guest sends [command] (handled by the app like any mailbox message). */
    private fun action(page: String, key: String, label: String, command: String) =
        Entry(page, key, Kind.ACTION, label, { command }, { _, _ -> })

    private val keyActions = Keymap.KeyAction.entries.map { it.id to it.label }

    // Windows Mobile's hardware Buttons 1-4 (Settings > Personal > Buttons): the special actions,
    // ids as guest/Helper/Buttons.cs takes them, or BUTTON_WM to leave WM's own assignment alone.
    const val BUTTON_WM = "wm"
    private val buttonActions = listOf(
        BUTTON_WM to "Windows Mobile's own setting", "start" to "Start Menu", "home" to "Home",
        "okclose" to "OK/Close", "contextmenu" to "Context Menu", "inputpanel" to "Input Panel",
        "leftsoft" to "Left Softkey", "rightsoft" to "Right Softkey", "rotate" to "Rotate Screen",
        "scrollup" to "Scroll Up", "scrolldown" to "Scroll Down", "scrollleft" to "Scroll Left",
        "scrollright" to "Scroll Right", "none" to "None")
    private fun hwButton(n: Int, key: String, label: String) =
        choice("Hardware", key, label, buttonActions, { it.hwButton(n) }, { s, v -> s.setHwButton(n, v) })

    /**
     * The helper command for the Buttons choices. The buttons are sent as stock keys (Keymap.HwButtons),
     * so only the choices no stock key reaches are listed, for WM to store; the helper answers with
     * WM's assignments ("buttons ..." -> Settings.hwButtonMap) either way. It stores a choice only
     * when it differs from the one it stored last, so it is safe to send on every hello and settings
     * change, and changes made in WM's own Buttons page stay otherwise (Buttons.cs).
     */
    fun buttonsCommand(s: Settings) = ("buttons sync " + (1..4).filter {
        Keymap.HwButtons.needsStore(it, s.hwButton(it), s.hwButtonMap)
    }.joinToString(" ") { "$it=${s.hwButton(it)}" }).trim()

    // Background: kind (unrestricted / suspend / hibernate) and the timer before it acts.
    private fun bgKind(s: Settings) = when (s.backgroundMode) {
        Settings.BG_RUN -> "run"
        Settings.BG_HIBERNATE, Settings.BG_HIBERNATE_AFTER -> "hibernate"
        else -> "suspend"
    }
    private fun bgTimer(s: Settings) =
        if (s.backgroundMode == Settings.BG_SUSPEND_AFTER || s.backgroundMode == Settings.BG_HIBERNATE_AFTER) s.backgroundMinutes else 0
    private fun setBg(s: Settings, kind: String, minutes: Int) {
        s.backgroundMode = when {
            kind == "run" -> Settings.BG_RUN
            kind == "hibernate" -> if (minutes > 0) Settings.BG_HIBERNATE_AFTER else Settings.BG_HIBERNATE
            else -> if (minutes > 0) Settings.BG_SUSPEND_AFTER else Settings.BG_SUSPEND
        }
        if (minutes > 0) s.backgroundMinutes = minutes
    }

    val entries: List<Entry> = listOf(
        // ---- Display ----
        bool("Display", "stats", "Performance overlay", { it.showStats }, { s, v -> s.showStats = v }),
        choice("Display", "filter", "Screen scaling", listOf("0" to "Smooth", "1" to "Sharp pixels", "2" to "Enhanced"),
            { it.screenFilter.toString() }, { s, v -> s.screenFilter = v.toInt() }),
        bool("Display", "rotate", "Auto-Rotate", { it.rotateGuest }, { s, v -> s.rotateGuest = v }),
        slider("Display", "rotdelay", "Rotation Delay (ms)", 200, 3000, { it.rotationDelayMs }, { s, v -> s.rotationDelayMs = v }),
        slider("Display", "rotsens", "Rotation Sensitivity", 0, 100, { it.rotationSensitivity }, { s, v -> s.rotationSensitivity = v }),
        bool("Display", "brightness", "Backlight follows Android", { it.syncBrightness }, { s, v -> s.syncBrightness = v }),
        // ---- Design ----
        choice("Design", "hwkeys", "Hardware Keys", listOf("slim" to "Slim 4 Button Row", "full" to "Full 6 Button + D-Pad", "none" to "None"),
            { if (!it.keysOn) "none" else if (it.dpadOn) "full" else "slim" },
            { s, v -> s.keysOn = v != "none"; if (v != "none") s.dpadOn = v == "full" }),
        choice("Design", "backdrop", "Shell Skin", listOf(SkinLayout.BACKDROP_BLACK to "AMOLED Black", SkinLayout.BACKDROP_GRAY to "Dark Gray",
            SkinLayout.BACKDROP_SILVER to "Sleek Silver", SkinLayout.BACKDROP_WHITE to "Glossy White"),
            { it.backdrop }, { s, v -> s.backdrop = v }),
        bool("Design", "wallpaper", "Android Wallpaper under Skin", { it.showWallpaper }, { s, v -> s.showWallpaper = v }),
        slider("Design", "blur", "Android Wallpaper Blur", 0, 25, { it.wallpaperBlur }, { s, v -> s.wallpaperBlur = v }),
        slider("Design", "opacity", "Skin Opacity", 0, 100, { it.backdropOpacity }, { s, v -> s.backdropOpacity = v }),
        bool("Design", "theme", "Sync Android's Color Theme", { it.syncTheme }, { s, v -> s.syncTheme = v }),
        bool("Design", "homewall", "Sync Android's Wallpaper", { it.syncWallpaper }, { s, v -> s.syncWallpaper = v }),
        slider("Design", "keyopacity", "Key Opacity", 0, 100, { it.keyOpacity }, { s, v -> s.keyOpacity = v }),
        slider("Design", "keyblur", "Key Blur", 0, 25, { it.keyBlur }, { s, v -> s.keyBlur = v }),
        color("Design", "keycolor", "Key Color", { it.keyColor }, { s, v -> s.keyColor = v }),
        bool("Design", "shine", "Enable Gyro Key Shine", { it.keyShine }, { s, v -> s.keyShine = v }),
        slider("Design", "shinebright", "Brightness", 0, 100, { it.shineBrightness }, { s, v -> s.shineBrightness = v }),
        slider("Design", "shinewidth", "Width", 0, 100, { it.shineWidth }, { s, v -> s.shineWidth = v }),
        bool("Design", "backlight", "Enable Key Backlights", { it.backlightOn }, { s, v -> s.backlightOn = v }),
        slider("Design", "blintensity", "Intensity", 0, 100, { it.backlightIntensity }, { s, v -> s.backlightIntensity = v }),
        choice("Design", "bltimeout", "Timeout", listOf("5" to "5 seconds", "10" to "10 seconds", "30" to "30 seconds",
            "60" to "1 minute", "0" to "Always on"), { it.backlightTimeout.toString() }, { s, v -> s.backlightTimeout = v.toInt() }),
        choice("Design", "blcolor", "Color", listOf("white" to "White", "blue" to "Blue", "green" to "Green", "amber" to "Amber",
            "red" to "Red", "theme" to "Match Theme"), { it.backlightColor }, { s, v -> s.backlightColor = v }),
        slider("Design", "bldirt", "Dirtiness", 0, 100, { it.backlightDirt }, { s, v -> s.backlightDirt = v }),
        bool("Design", "fold", "Slide Out Keyboard (Foldables)", { it.foldMode }, { s, v -> s.foldMode = v }),
        choice("Design", "foldkb", "Keyboard Layout", listOf("skin" to "Skin Layout", "imefn" to "Android IME + Function Row", "ime" to "Android IME"),
            { if (it.foldKeyboard != "ime") "skin" else if (it.foldHotkeys) "imefn" else "ime" },
            { s, v -> s.foldKeyboard = if (v == "skin") "skin" else "ime"; if (v != "skin") s.foldHotkeys = v == "imefn" }),
        choice("Design", "netind", "Network Indicator Type", listOf("auto" to "Automatic (phone's network)",
            "gsm" to "GSM (G / E / 3G / H)", "cdma" to "CDMA (1X / EV)"), { it.netIndicator }, { s, v -> s.netIndicator = v }),
        bool("Design", "hinge", "Show Mid-Display Hinge", { it.hingeOn }, { s, v -> s.hingeOn = v }),
        choice("Design", "hingestyle", "Style", listOf(SkinLayout.HINGE_SHADOW to "Drop Shadow",
            SkinLayout.HINGE_CLASSIC to "Classic (Rounded Plastic Hinge)", SkinLayout.HINGE_CREASE to "Crease (Modern)",
            SkinLayout.HINGE_VOID to "Void (Black Line)"), { it.hingeStyle }, { s, v -> s.hingeStyle = v }),
        slider("Design", "hingethick", "Thickness", 0, 100, { it.hingeThickness }, { s, v -> s.hingeThickness = v }),
        slider("Design", "hingeloc", "Location", -50, 50, { it.hingeLocation }, { s, v -> s.hingeLocation = v }),
        slider("Design", "hingealpha", "Opacity", 0, 100, { it.hingeOpacity }, { s, v -> s.hingeOpacity = v }),
        bool("Design", "androidime", "Use Android IME over Device IME", { it.androidIme }, { s, v -> s.androidIme = v }),  // ImeBridge
        // ---- Hardware ----
        choice("Hardware", "camera", "Camera", listOf("off" to "Disabled", "rear" to "Rear", "front" to "Front"),
            { it.cameraMode }, { s, v -> s.cameraMode = v }),
        bool("Hardware", "camfilter", "Apply 2009 Filter to Camera", { it.cameraEraOn }, { s, v -> s.cameraEraOn = v }),
        slider("Hardware", "camlevel", "Filter Intensity", 1, 100, { it.cameraEraLevel }, { s, v -> s.cameraEraLevel = v }),
        bool("Hardware", "gps", "GPS", { it.gpsOn }, { s, v -> s.gpsOn = v }),
        bool("Hardware", "btreceive", "Receive Bluetooth Files", { it.bluetoothReceive }, { s, v -> s.bluetoothReceive = v }),
        bool("Hardware", "clipboard", "Sync Clipboard", { it.syncClipboard }, { s, v -> s.syncClipboard = v }),  // ClipboardBridge
        choice("Hardware", "boost", "Volume Boost", listOf("0" to "Off", "6" to "+6 dB", "12" to "+12 dB", "18" to "+18 dB"),
            { it.audioBoostDb.toString() }, { s, v -> s.audioBoostDb = v.toInt() }),
        choice("Hardware", "volup", "Phone Volume Up Key", keyActions, { it.volumeUp.id },
            { s, v -> s.volumeUp = Keymap.KeyAction.of(v, s.volumeUp) }),
        choice("Hardware", "voldown", "Phone Volume Down Key", keyActions, { it.volumeDown.id },
            { s, v -> s.volumeDown = Keymap.KeyAction.of(v, s.volumeDown) }),
        // Windows Mobile's Buttons 1-4 (the skin's keys send them; applied by buttonsCommand)
        hwButton(1, "button1", "Button 1 (Start key)"),
        hwButton(2, "button2", "Button 2 (App Launch 1)"),
        hwButton(3, "button3", "Button 3 (App Launch 2)"),
        hwButton(4, "button4", "Button 4 (OK key)"),
        // ---- Network ----
        // HTTPS through the app's secure web proxy (SecureProxy): applies at once, with the root
        // certificate added to or removed from WM's root store by the helper (TlsCa.cs).
        bool("Network", "secureproxy", "Secure Web Proxy (modern HTTPS)", { it.secureProxy }, { s, v -> s.secureProxy = v }),
        // Messenger / Passport names to Escargot's MSN revival servers (NativeBridge.setEscargot;
        // docs/research-escargot.md: classic MSNP clients only, not the built-in mobile clients)
        bool("Network", "escargot", "Windows Live Messenger via Escargot", { it.escargot }, { s, v -> s.escargot = v }),
        // ---- Power ----
        bool("Power", "displayoff", "Lock Android on Screen Off", { it.mirrorDisplayOff }, { s, v -> s.mirrorDisplayOff = v }),
        bool("Power", "lockoff", "Lock Device when Android Screen's Off", { it.lockOnScreenOff }, { s, v -> s.lockOnScreenOff = v }),
        bool("Power", "lockleave", "Lock when Device closes", { it.lockOnLeave }, { s, v -> s.lockOnLeave = v }),
        bool("Power", "resume", "Enable Hibernation", { it.resumeSession }, { s, v -> s.resumeSession = v }),
        choice("Power", "bgkind", "Run in Background", listOf("run" to "Unrestricted", "suspend" to "Suspend", "hibernate" to "Hibernate"),
            { bgKind(it) }, { s, v -> setBg(s, v, bgTimer(s)) }),
        choice("Power", "bgtimer", "After timer",
            listOf("0" to "Immediately") + Settings.BACKGROUND_MINUTES.map { it.toString() to (if (it == 1) "1 minute" else "$it minutes") },
            { bgTimer(it).toString() }, { s, v -> setBg(s, bgKind(s), v.toInt()) }),
        // ---- Launcher (AndroidApps.kt): PocketStylus as the phone's home app, Android apps in WM, the dock ----
        // The actions' commands come back from WM as mailbox lines ("launcher role", ...) and open
        // Android's screens; DeviceSettingsActivity runs the same commands.
        action("Launcher", "launcherrole", "Use as Phone Launcher…", AndroidApps.CMD_ROLE),
        choice("Launcher", "homegoes", "Home Goes To", listOf("today" to "Today", "start" to "Start Menu"),
            { it.launcherHome }, { s, v -> s.launcherHome = v }),
        bool("Launcher", "androidfolder", "Show Android Apps Folder", { it.androidFolder }, { s, v -> s.androidFolder = v }),
        action("Launcher", "hiddenapps", "Hidden Apps…", AndroidApps.CMD_HIDDEN),
        action("Launcher", "pinnedapps", "Pinned Shortcuts…", AndroidApps.CMD_PINS),
        // ---- Dock: Android apps on the skin (DeviceSkin dock, LauncherBridge) ----
        bool("Dock", "dockon", "Show Android App Dock", { it.dockOn }, { s, v -> s.dockOn = v }),
        bool("Dock", "dockresize", "Resize Screen for Dock", { it.dockResizeScreen }, { s, v -> s.dockResizeScreen = v }, restart = true),
        choice("Dock", "dockpos", "Position", listOf(Settings.DOCK_AUTO to "Auto (free space on the skin)",
            Settings.DOCK_TOP to "Top of Screen", Settings.DOCK_BOTTOM to "Bottom of Screen"),
            { it.dockPlace }, { s, v -> s.dockPlace = v }),
        slider("Dock", "docksize", "Number of Apps", 4, 8, { it.dockSize }, { s, v -> s.dockSize = v }),
        action("Dock", "dockapps", "Dock Apps…", AndroidApps.CMD_DOCK),
        slider("Dock", "dockicon", "Icon Size", 24, 96, { it.dockIcon }, { s, v -> s.dockIcon = v }),
        slider("Dock", "dockpad", "Padding", 0, 32, { it.dockPad }, { s, v -> s.dockPad = v }),
        slider("Dock", "dockopacity", "Background Opacity", 0, 100, { it.dockOpacity }, { s, v -> s.dockOpacity = v }),
        slider("Dock", "dockblur", "Background Blur", 0, 25, { it.dockBlur }, { s, v -> s.dockBlur = v }),
        // ---- Phone: calls and texts (CallBridge, CallOverlay, SmsBridge; shared by every OS) ----
        // Turning one on from Windows Mobile has the app ask for its permissions (EmulatorActivity).
        bool("Phone", "calls", "Phone Calls in Windows Mobile", { CallBridge.enabled(it.context) },
            { s, v -> CallBridge.setEnabled(s.context, v) }),
        bool("Phone", "wmringer", "Use Phone's Ringer", { CallBridge.wmRinger(it.context) }, { s, v -> CallBridge.setWmRinger(s.context, v) }),
        // (the stored choice: ringing in the background also needs "Display over other apps")
        bool("Phone", "callbg", "Ring Windows Mobile in the Background", { Settings.global(it.context).getBoolean("calls.background", true) },
            { s, v -> CallOverlay.setEnabled(s.context, v) }),
        bool("Phone", "sms", "Text Messages in Windows Mobile", { SmsBridge.enabled(it.context) }, { s, v -> SmsBridge.setEnabled(s.context, v) }),
        bool("Phone", "smsdefer", "Defer Incoming SMS Messages", { SmsBridge.defer(it.context) }, { s, v -> SmsBridge.setDefer(s.context, v) }),
        action("Phone", "phonemore", "Phone Number, Call Screen…", CMD_CALLS),
        // ---- Contacts & calendar sync (PimSync; shared by every OS) ----
        bool("Contacts", "pimcontacts", "Sync Contacts", { Settings.global(it.context).getBoolean("pim.contacts", false) },
            { s, v -> PimSync.setOn(s.context, PimSync.KIND_CONTACT, v) }),
        bool("Contacts", "pimcalendar", "Sync Calendar", { Settings.global(it.context).getBoolean("pim.calendar", false) },
            { s, v -> PimSync.setOn(s.context, PimSync.KIND_APPOINTMENT, v) }),
        bool("Contacts", "pimprompt", "Prompt before Accepting Changes from Device", { PimSync.prompt(it.context) },
            { s, v -> PimSync.setPrompt(s.context, v) }),
        action("Contacts", "pimsync", "Sync Now", CMD_PIM_SYNC),
        // ---- Infrared (InfraredSettings, its own shared file) ----
        choice("Infrared", "irmode", "When Windows Mobile Uses Infrared", listOf("ask" to "Ask each time",
            "ble" to "Nearby phones (Bluetooth LE)", "wan" to "InfraWAN (Internet)", "off" to "Off"),
            { InfraredSettings(it.context).mode }, { s, v -> InfraredSettings(s.context).mode = v }),
        bool("Infrared", "irlisten", "Stay Reachable while PocketStylus is Open", { InfraredSettings(it.context).listen },
            { s, v -> InfraredSettings(s.context).listen = v }),
        action("Infrared", "irmore", "InfraWAN Account, Test…", CMD_INFRARED),
        // ---- Device (applies on restart) ----
        choice("Device", "res", "Screen resolution", listOf(Settings.SCREEN_AUTO to "Phone shape (auto)",
            "480x854" to "480×854 (FWVGA)", "480x800" to "480×800 (WVGA)", "480x640" to "480×640 (VGA)",
            "480x480" to "480×480 (square)"), { it.screenRes }, { s, v -> s.screenRes = v }, restart = true),
        // Sizes per release (entriesFor, OsProfile.memSizes); 256 MB is the board's ceiling.
        choice("Device", "ram", "Memory", memOptions(null), { it.memMB.toString() }, { s, v -> s.memMB = v.toInt() }, restart = true),
        // Windows Mobile's logical DPI; the helper writes it and restarts WM once (guest/Helper/Dpi.cs).
        choice("Device", "dpi", "Screen DPI", listOf("0" to "Image default", "192" to "High DPI (192, large)",
            // Below 192 WM 6.5's home screen breaks; 6.1.4 and 5.0 work but lose some title-bar icons.
            "128" to "Medium (128; not for 6.5)", "96" to "Classic (96, more space; not for 6.5)"),
            { it.guestDpi.toString() }, { s, v -> s.guestDpi = v.toInt() }, restart = true),
        // Opens the Storage Card page in Windows Mobile (mode, root, folders; applied there).
        action("Device", "editfolders", "Storage Card…", "local:storage"),
    )

    /**
     * The catalogue for [settings]' release. Windows Mobile Standard (keypad, OsProfile.touch false)
     * leaves out what needs a touch screen or a Professional image (rotation, the foldable slider and
     * hinge, the Pro hardware buttons, DPI), offers its own screen sizes (OsProfile.screens) and the
     * keypad skin as its Hardware Keys.
     */
    fun entriesFor(settings: Settings): List<Entry> {
        val profile = settings.profile
        if (profile == null) return entries
        val ram = memOptions(profile)
        val ramEntry: (Entry) -> Entry = { e ->
            choice(e.page, e.key, e.label, ram, { s -> s.memMB.toString().takeIf { v -> ram.any { it.first == v } } ?: "0" },
                { s, v -> s.memMB = v.toInt() }, restart = true)
        }
        if (profile.touch) return entries.map { if (it.key == "ram") ramEntry(it) else it }
        val res = listOf(Settings.SCREEN_AUTO to "Phone shape (auto)") + profile.screens.map { it.id to it.label }
        return entries.filter { it.key !in TOUCH_ONLY }.map { e ->
            when (e.key) {
                "res" -> choice(e.page, e.key, e.label, res,
                    { s -> s.screenRes.takeIf { r -> res.any { it.first == r } } ?: Settings.SCREEN_AUTO },
                    { s, v -> s.screenRes = v }, restart = true)
                "ram" -> ramEntry(e)
                "hwkeys" -> choice(e.page, e.key, e.label,
                    listOf("full" to "Keypad (soft keys, D-pad, number pad)", "none" to "None (the phone's own keypad)"),
                    { if (it.keysOn) "full" else "none" }, { s, v -> s.keysOn = v != "none" })
                else -> e
            }
        }
    }

    /** Memory choices: the release's default and the sizes it runs with (OsProfile.memSizes). */
    private fun memOptions(profile: OsProfile?): List<Pair<String, String>> =
        listOf("0" to (profile?.let { "Default (${it.defaultMemMB} MB)" } ?: "Default")) +
            (profile?.memSizes ?: OsProfiles.MEM_SIZES).map { "$it" to "$it MB" }

    /** Settings a Standard (keypad) release doesn't have (entriesFor). */
    private val TOUCH_ONLY = setOf("rotate", "rotdelay", "rotsens", "fold", "foldkb", "hinge", "hingestyle", "hingethick",
        "hingeloc", "hingealpha", "button1", "button2", "button3", "button4", "dpi")

    // Buttons whose screens are Android's (role requests, permissions, text entry, a connection
    // test): the guest sends the command, EmulatorActivity / DeviceSettingsActivity open the screen.
    const val CMD_CALLS = "open calls"
    const val CMD_PIM = "open pim"
    const val CMD_INFRARED = "open infrared"
    const val CMD_PIM_SYNC = "open pimsync"
    /** The commands of such buttons (GuestBridge routes them to onGuestLauncher). */
    val ANDROID_COMMANDS = setOf(CMD_CALLS, CMD_PIM, CMD_INFRARED, CMD_PIM_SYNC)

    /** The Android screen an action's [command] opens, or null if it opens none. */
    fun screenFor(ctx: android.content.Context, command: String): android.content.Intent? = when (command) {
        CMD_CALLS -> android.content.Intent(ctx, CallSettingsActivity::class.java)
        CMD_PIM -> android.content.Intent(ctx, PimSettingsActivity::class.java)
        CMD_INFRARED -> android.content.Intent(ctx, InfraredSettingsActivity::class.java)
        else -> AndroidAppsActivity.intentFor(ctx, command)
    }

    /** Writes the catalogue with current values for the guest. */
    fun write(file: File, settings: Settings) {
        val text = buildString {
            for (e in entriesFor(settings)) {
                val value = e.get(settings)
                val extra = when (e.kind) {
                    Kind.CHOICE -> e.options.joinToString(";") { "${it.first}=${it.second}" }
                    Kind.COLOR -> (e.options + if (e.options.none { it.first.equals(value, true) }) listOf(value to "Custom $value") else emptyList())
                        .joinToString(";") { "${it.first}=${it.second}" }
                    Kind.SLIDER -> "${e.min}:${e.max}"
                    Kind.BOOL, Kind.ACTION -> ""
                }
                // The guest app knows bool/choice/slider/action: a colour goes as a choice.
                val kind = if (e.kind == Kind.COLOR) Kind.CHOICE.id else e.kind.id
                append(listOf(e.page, e.key, kind, e.label, value, if (e.restart) "1" else "0", extra)
                    .joinToString("\t")).append("\r\n")
            }
        }
        val tmp = File(file.path + ".tmp")
        tmp.writeText(text)
        tmp.renameTo(file)
    }

    /** Applies "set key=value" from the guest: null if unknown or unchanged, else whether it needs a restart. */
    fun apply(settings: Settings, key: String, value: String): Boolean? {
        val e = entriesFor(settings).firstOrNull { it.key == key && it.kind != Kind.ACTION } ?: return null
        if (e.get(settings) == value) return null
        e.set(settings, value)
        return e.restart
    }
}
