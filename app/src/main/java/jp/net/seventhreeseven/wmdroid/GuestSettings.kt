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
    enum class Kind(val id: String) { BOOL("bool"), CHOICE("choice"), SLIDER("slider"), ACTION("action"), COLOR("color"), TEXT("text") }

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

    /** A line of text. Tabs and line breaks are spaces, so the catalogue line stays one row. */
    private fun text(page: String, key: String, label: String, get: (Settings) -> String, set: (Settings, String) -> Unit,
                     password: Boolean = false) =
        Entry(page, key, Kind.TEXT, label, { plain(get(it)) }, { s, v -> set(s, plain(v)) },
            options = if (password) listOf("password" to "1") else emptyList())

    private fun plain(s: String) = s.replace('\t', ' ').replace('\r', ' ').replace('\n', ' ')

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
        // Unrestricted keeps the session, so hibernation stays on and the timer does not apply.
        if (kind == "run") s.resumeSession = true
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
        slider("Design", "keyoutline", "Key Outline", 0, 100, { it.keyOutline }, { s, v -> s.keyOutline = v }),
        slider("Design", "keyglyph", "Key Glyph", 0, 100, { it.keyGlyph }, { s, v -> s.keyGlyph = v }),
        choice("Design", "keytheme", "Key Style", listOf("glass" to "Glass", "flat" to "Flat",
            "bubbled" to "Bubbled", "metallic" to "Metallic"), { it.keyTheme }, { s, v -> s.keyTheme = v }),
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
        // ---- Realism: how a 2003-2010 handheld actually looked and felt (GlRenderer::Realism) ----
        choice("Realism", "panel", "Display Panel", listOf("off" to "Clean (no panel effects)",
            "tn" to "TFT LCD (TN, most WM phones)", "stn" to "Passive Matrix (CSTN, early PDAs)", "amoled" to "AMOLED (Omnia II era)"),
            { it.panel }, { s, v -> s.panel = v }),
        slider("Realism", "lcdgrid", "Pixel Grid", 0, 100, { it.lcdGrid }, { s, v -> s.lcdGrid = v }),
        slider("Realism", "lcdcontrast", "Colour and Contrast", 0, 100, { it.lcdContrast }, { s, v -> s.lcdContrast = v }),
        slider("Realism", "lcdxtalk", "Crosstalk / Load Effects", 0, 100, { it.lcdCrosstalk }, { s, v -> s.lcdCrosstalk = v }),
        slider("Realism", "lcdlight", "Backlight Unevenness", 0, 100, { it.lcdBacklight }, { s, v -> s.lcdBacklight = v }),
        slider("Realism", "lcdghost", "Ghosting / Smear", 0, 100, { it.lcdGhost }, { s, v -> s.lcdGhost = v }),
        slider("Realism", "lcdangle", "Viewing Angle (Tilt)", 0, 100, { it.lcdAngle }, { s, v -> s.lcdAngle = v }),
        bool("Realism", "lcdcold", "Cold Slows the Screen", { it.lcdCold }, { s, v -> s.lcdCold = v }),
        slider("Realism", "touchlayer", "Resistive Touch Layer", 0, 100, { it.touchLayer }, { s, v -> s.touchLayer = v }),
        slider("Realism", "ripple", "Press Ripple", 0, 100, { it.pressRipple }, { s, v -> s.pressRipple = v }),
        slider("Realism", "wear", "Screen Wear (Scratches, Smudges)", 0, 100, { it.screenWear }, { s, v -> s.screenWear = v }),
        slider("Realism", "glare", "Sunlight Glare", 0, 100, { it.sunGlare }, { s, v -> s.sunGlare = v }),
        slider("Realism", "dead", "Dead / Stuck Pixels", 0, 8, { it.deadPixels }, { s, v -> s.deadPixels = v }),
        slider("Realism", "tear", "Screen Tearing", 0, 100, { it.screenTear }, { s, v -> s.screenTear = v }),
        bool("Realism", "blfade", "Backlight Fade on Wake and Sleep", { it.backlightFade }, { s, v -> s.backlightFade = v }),
        slider("Realism", "hdr", "HDR Screen and Key Backlights", 0, 100, { it.hdrLevel }, { s, v -> s.hdrLevel = v }),
        bool("Realism", "cpulimit", "Limit CPU Speed", { it.cpuLimitOn }, { s, v -> s.cpuLimitOn = v }),
        slider("Realism", "cpumhz", "CPU Speed (MHz)", 50, 1000, { it.cpuMhz }, { s, v -> s.cpuMhz = v }),
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
        bool("Hardware", "lockclose", "Lock Button Closes App", { it.lockClosesApp }, { s, v -> s.lockClosesApp = v }),
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
        bool("Power", "resume", "Enable Hibernation", { it.resumeSession },
            { s, v -> s.resumeSession = if (s.backgroundMode == Settings.BG_RUN) true else v }),
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
        bool("Dock", "dockunfold", "Disable Dock in Unfolded Mode", { it.dockHideUnfolded }, { s, v -> s.dockHideUnfolded = v }),
        bool("Dock", "dockresize", "Resize Screen for Dock", { it.dockResizeScreen }, { s, v -> s.dockResizeScreen = v }, restart = true),
        choice("Dock", "dockpos", "Position", listOf(Settings.DOCK_AUTO to "Auto (free space on the skin)",
            Settings.DOCK_TOP to "Top of Screen", Settings.DOCK_BOTTOM to "Bottom of Screen",
            Settings.DOCK_FLOOR to "Very Bottom"),
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
        bool("Phone", "callbg", "Ring Windows Mobile in the Background", { Settings.global(it.context).getBoolean("calls.background", true) },
            { s, v -> CallOverlay.setEnabled(s.context, v) }),
        bool("Phone", "sms", "Text Messages in Windows Mobile", { SmsBridge.enabled(it.context) }, { s, v -> SmsBridge.setEnabled(s.context, v) }),
        bool("Phone", "smsdefer", "Defer Incoming SMS Messages", { SmsBridge.defer(it.context) }, { s, v -> SmsBridge.setDefer(s.context, v) }),
        action("Phone", "smsload", "Load Messages", CMD_SMS_LOAD),
        text("Phone", "mynumber", "My Phone Number", { it.myNumber }, { s, v -> s.myNumber = v.trim() }),
        action("Phone", "callrole", "Use Windows Mobile's Call Screen…", CMD_CALL_ROLE),
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
        text("Infrared", "irname", "This Phone's Name for Others", { InfraredSettings(it.context).name },
            { s, v -> InfraredSettings(s.context).name = v.trim() }),
        text("Infrared", "irserver", "InfraWAN Server", { InfraredSettings(it.context).server },
            { s, v -> InfraredSettings(s.context).server = v.trim() }),
        text("Infrared", "irroom", "InfraWAN Room", { InfraredSettings(it.context).room },
            { s, v -> InfraredSettings(s.context).room = v.trim().ifEmpty { "lobby" } }),
        text("Infrared", "iruser", "InfraWAN User", { InfraredSettings(it.context).user },
            { s, v -> InfraredSettings(s.context).user = v.trim() }),
        text("Infrared", "irtoken", "InfraWAN Token", { InfraredSettings(it.context).token },
            { s, v -> InfraredSettings(s.context).token = v.trim() }, password = true),
        action("Infrared", "irble", "Allow Nearby Devices", CMD_IR_BLE),
        action("Infrared", "irtest", "Test InfraWAN Connection", CMD_IR_TEST),
        // Today items are not installed with the helper. Each button installs that one package.
        // A later helper update replaces a widget that is already in \Windows.
        action("Today", "widgetweather", "Install Weather", "widget weather"),
        action("Today", "widgetinfra", "Install InfraWAN", "widget infrawan"),
        // ---- Device (applies on restart) ----
        choice("Device", "res", "Screen resolution", listOf(Settings.SCREEN_AUTO to "Phone shape (auto)",
            "480x854" to "480×854 (FWVGA)", "480x800" to "480×800 (WVGA; HTC games)", "480x640" to "480×640 (VGA)",
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
        if (profile == null) return liveEntries(settings, entries)
        val ram = memOptions(profile)
        val ramEntry: (Entry) -> Entry = { e ->
            choice(e.page, e.key, e.label, ram, { s -> s.memMB.toString().takeIf { v -> ram.any { it.first == v } } ?: "0" },
                { s, v -> s.memMB = v.toInt() }, restart = true)
        }
        if (profile.touch) return liveEntries(settings, withoutHelper(profile, entries.map { e ->
            when {
                e.key == "ram" -> ramEntry(e)
                e.key == "res" && profile.maxHeight > 0 -> resCapped(e, profile.maxHeight)
                else -> e
            }
        }))
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
        }.let { liveEntries(settings, withoutHelper(profile, it)) }
    }

    /** Call audio's choices are the outputs connected right now (CallAudio). */
    private fun liveEntries(settings: Settings, list: List<Entry>): List<Entry> {
        if (settings.backgroundMode == Settings.BG_RUN && !settings.resumeSession) settings.resumeSession = true
        val outs = CallAudio.outputs(settings.context).map { it.id to it.name.replace(';', ',').replace('=', '-') }
            .ifEmpty { listOf(CallAudio.EARPIECE to "Earpiece") }
        val audio = choice("Phone", "callaudio", "Call Audio", outs,
            { CallAudio.saved(it.context)?.takeIf { id -> outs.any { o -> o.first == id } } ?: outs.first().first },
            { s, v -> CallAudio.select(s.context, v) })
        val at = list.indexOfFirst { it.key == "wmringer" }
        val withAudio = if (at < 0) list else list.toMutableList().apply { add(at + 1, audio) }
        if (batteryExempt(settings.context)) return withAudio
        val power = withAudio.indexOfFirst { it.page == "Power" }
        val row = action("Power", "batteryopt", "Disable Battery Optimization", CMD_BATTERY)
        return withAudio.toMutableList().apply { add(if (power < 0) size else power, row) }
    }

    /** Android is not allowed to defer this app. The Power page shows a button until this is true. */
    fun batteryExempt(ctx: android.content.Context): Boolean {
        val pm = ctx.getSystemService(android.os.PowerManager::class.java) ?: return true
        return pm.isIgnoringBatteryOptimizations(ctx.packageName)
    }

    /** Screen sizes at or under [maxHeight]. Taller choices (854, and the phone's 852) are not offered. */
    private fun resCapped(e: Entry, maxHeight: Int): Entry {
        val opts = e.options.filter { (id, _) ->
            id == Settings.SCREEN_AUTO || (id.substringAfter('x').toIntOrNull() ?: 0) <= maxHeight
        }
        return choice(e.page, e.key, e.label, opts,
            { s -> s.screenRes.takeIf { r -> opts.any { it.first == r } } ?: Settings.SCREEN_AUTO },
            { s, v -> s.screenRes = v }, restart = true)
    }

    /** Memory choices: the release's default and the sizes it runs with (OsProfile.memSizes). */
    private fun memOptions(profile: OsProfile?): List<Pair<String, String>> =
        listOf("0" to (profile?.let { "Default (${it.defaultMemMB} MB)" } ?: "Default")) +
            (profile?.memSizes ?: OsProfiles.MEM_SIZES).map { "$it" to "$it MB" }

    /** Settings a Standard (keypad) release doesn't have (entriesFor). */
    private val TOUCH_ONLY = setOf("rotate", "rotdelay", "rotsens", "fold", "foldkb", "hinge", "hingestyle", "hingethick",
        "hingeloc", "hingealpha", "button1", "button2", "button3", "button4", "dpi")

    /**
     * Pages and rows that only the .NET CF 3.5 helper can apply. Windows Mobile 2003 has no helper
     * (OsProfile.helper): the skin, screen, memory and power options stay.
     */
    private val HELPER_PAGES = setOf("Phone", "Contacts", "Infrared", "Today", "Launcher")
    private val HELPER_KEYS = setOf(
        "rotate", "rotdelay", "rotsens", "brightness", "theme", "homewall", "androidime", "netind",
        "camera", "camfilter", "camlevel", "gps", "clipboard", "dpi", "secureproxy", "escargot", "editfolders")

    private fun withoutHelper(profile: OsProfile, list: List<Entry>) =
        if (profile.helper) list else list.filter { it.page !in HELPER_PAGES && it.key !in HELPER_KEYS }

    // Buttons whose screens are Android's (role requests, permissions, text entry, a connection
    // test): the guest sends the command, EmulatorActivity / DeviceSettingsActivity open the screen.
    const val CMD_CALLS = "open calls"
    const val CMD_CALL_ROLE = "open callrole"
    const val CMD_PIM = "open pim"
    const val CMD_INFRARED = "open infrared"
    const val CMD_IR_TEST = "open irtest"
    const val CMD_IR_BLE = "open irble"
    const val CMD_PIM_SYNC = "open pimsync"
    const val CMD_SMS_LOAD = "open smsload"
    const val CMD_BATTERY = "open battery"
    /** The commands of such buttons (GuestBridge routes them to onGuestLauncher). */
    val ANDROID_COMMANDS = setOf(CMD_CALLS, CMD_CALL_ROLE, CMD_PIM, CMD_INFRARED, CMD_IR_TEST, CMD_IR_BLE,
        CMD_PIM_SYNC, CMD_SMS_LOAD, CMD_BATTERY)

    /** The Android screen an action's [command] opens, or null if it opens none. */
    fun screenFor(ctx: android.content.Context, command: String): android.content.Intent? = when (command) {
        CMD_CALLS -> android.content.Intent(ctx, CallSettingsActivity::class.java)
        CMD_PIM -> android.content.Intent(ctx, PimSettingsActivity::class.java)
        CMD_INFRARED -> android.content.Intent(ctx, InfraredSettingsActivity::class.java)
        CMD_BATTERY -> android.content.Intent(android.provider.Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
            .setData(android.net.Uri.parse("package:" + ctx.packageName))
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
                    Kind.TEXT -> if (e.options.any { it.first == "password" }) "password" else ""
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
