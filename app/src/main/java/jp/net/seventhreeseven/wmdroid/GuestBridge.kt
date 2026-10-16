package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.util.Log
import java.io.File
import java.util.TimeZone

/**
 * Talks to the WMDroid helper inside Windows Mobile.
 *
 * The app adds two hidden folders to every \Storage Card (FolderShare overlays):
 *   2577\autorun.exe  WM runs it whenever the card mounts: a native bootstrap that installs .NET CF 3.5
 *                     if needed and starts the helper, 2577\helper.exe
 *   .wmdroid          mailbox: in\*.cmd (app -> guest), out\*.msg (guest -> app), state.ini
 */
class GuestBridge(private val ctx: Context, private val settings: Settings, private val listener: Listener) {
    interface Listener {
        fun onSettingsChanged(needsRestart: Boolean)
        fun onOpenSettings()
        /** The guest's settings app asked to edit the storage folder mapping. */
        fun onOpenStorage() = onOpenSettings()
        /** The guest's Storage Card page needs Android's All files access screen. */
        fun onOpenAllFiles() {}
        /** The helper (re)started: resend state it keeps in memory. */
        fun onGuestHello()
        /** The helper patched the system and is resetting the guest. */
        fun onGuestPatched()
        /** The helper runs on a patched system. */
        fun onGuestReady()
        /** What the guest is sounding: "media", "alert" or "ring". */
        fun onGuestAudio(kind: String) {}
        /** "Send via Bluetooth": [name] is in the mailbox's send folder (BluetoothBridge). */
        fun onGuestShare(name: String) {}
        /** WM 6 / 6.1 / 6.1.4: the helper named the network for the RIL; the modem re-announces registration. */
        fun onGuestRadioRefresh() {}
        /** Phone calls: a "call ..." line from the helper, without the prefix (CallBridge). */
        fun onGuestCall(line: String) {}
        /** Contacts and calendar sync: a "pim ..." line from the helper, without the prefix (PimSync). */
        fun onGuestPim(line: String) {}
        /** Infrared: an "irda ..." line from the helper, without the prefix (InfraredBridge). */
        fun onGuestIrda(line: String) {}
        /** Text messages: a "sms ..." line from the helper, without the prefix (SmsBridge). */
        fun onGuestSms(line: String) {}
        /** "clip": Windows Mobile's clipboard text is in clip_out.txt (ClipboardBridge). */
        fun onGuestClip() {}
        /** "ime open" / "ime hide", without the prefix (ImeBridge). */
        fun onGuestIme(what: String) {}
        /** The helper found the card mounted under another name (\Storage Card2): put it back. */
        fun onGuestRemount() {}
        /** The helper reported WM's Buttons assignments (now in Settings.hwButtonMap). */
        fun onGuestButtons() {}
        /** "android ..." from a launcher in WM's Android Apps folder, without the prefix (LauncherBridge). */
        fun onGuestAndroid(line: String) {}
        /** A Launcher page button in WM's settings app: AndroidApps.CMD_* ("launcher role", ...). */
        fun onGuestLauncher(command: String) {}
        /** The answer to a certificate warning: "<id> yes|no|timeout" (SecureProxy). */
        fun onGuestTls(line: String) {}
    }

    private val root = File(ctx.filesDir, "overlay")
    private val autorunDir = File(root, "2577")
    private val box = File(root, ".wmdroid")
    private val inDir = File(box, "in")
    private val outDir = File(box, "out")
    private var seq = 0
    private var lastTzOffset = Int.MIN_VALUE
    private var helperSum = ""

    /** FNV-1a, as the helper reports for its own file. */
    private fun fnv(b: ByteArray): Int {
        var h = 0x811C9DC5.toInt()
        for (x in b) { h = h xor (x.toInt() and 0xFF); h *= 16777619 }
        return h
    }

    /** (guest folder name, host path) pairs for NativeBridge.setShareOverlays. */
    fun prepare(): List<Pair<String, String>> {
        autorunDir.mkdirs()
        inDir.mkdirs()
        outDir.mkdirs()
        // Leftovers of an earlier run (or another OS) are stale, and this run numbers its commands
        // from 0 again: start with an empty mailbox (the state is sent afresh below).
        inDir.listFiles()?.forEach { it.delete() }
        outDir.listFiles()?.forEach { it.delete() }
        // autorun.exe is a small native bootstrap (guest/Autorun): it installs .NET CF 3.5 from the CAB
        // when the image lacks it (WM 5, 6.0, 6.1), then starts the helper, helper.exe, with the same
        // arguments. Plus the other guest programs the helper installs (GPS Test, Send via Bluetooth).
        for ((asset, name) in listOf("Autorun.exe" to "autorun.exe", "WMDroid.exe" to "helper.exe",
                "GpsTest.exe" to "GpsTest.exe", "SendBluetooth.exe" to "SendBluetooth.exe",
                "NETCFv35.wm.armv4i.cab" to "NETCFv35.wm.armv4i.cab",
                "MAPIlib.dll" to "MAPIlib.dll", "OpenNETCF.dll" to "OpenNETCF.dll")) {  // texts on WM 6.5.3 (Sms.cs)
            val exe = File(autorunDir, name)
            ctx.assets.open("guest/$asset").use { input ->
                val bytes = input.readBytes()
                if (asset == "WMDroid.exe") helperSum = "%08x".format(fnv(bytes))
                if (!exe.exists() || exe.length() != bytes.size.toLong() || !exe.readBytes().contentEquals(bytes))
                    exe.writeBytes(bytes)
            }
        }
        writeState()
        // A restored session already has its helper running and won't say hello by itself: ask, so an
        // outdated helper is noticed (one too old to know "hello" answers "unknown").
        helloSeq = "%08d".format(seq)
        helloSeen = false
        markAlive()  // a new session: its helper gets its full time to answer
        send("hello")
        return overlays()
    }

    private var helloSeq = ""

    /** The phone's wallpaper cut to where the display sits, at the guest's resolution. */
    val deviceWallpaper = File(root, "device_wallpaper.bmp")

    /** A file in the helper's mailbox folder (\Storage Card\.wmdroid). */
    fun boxFile(name: String) = File(box, name)

    fun overlays(): List<Pair<String, String>> = buildList {
        add("2577" to autorunDir.path)
        add(".wmdroid" to box.path)
        if (deviceWallpaper.exists()) add("device_wallpaper.bmp" to deviceWallpaper.path)
        add("Bluetooth" to BluetoothBridge.receiveDir(ctx).path)  // files received over Bluetooth
        add(AndroidApps.GUEST_DIR to AndroidApps.hostDir(ctx).apply { mkdirs() }.path)  // Android apps' launchers (AndroidApps)
        addAll(StorageBridge.folderOverlays(ctx, settings))
    }

    /** Current settings for the helper's settings page. */
    fun writeState() {
        val text = buildString {
            appendLine("storage.mode=${settings.storageMode}")
            appendLine("screen.res=${settings.screenRes}")
            appendLine("resume=${settings.resumeSession}")
            appendLine("stats=${settings.showStats}")
            // Screen DPI (guest/Helper/Dpi.cs); Standard keeps its image's own (each size has its image).
            appendLine("dpi=${if (settings.profile?.touch == false) 0 else settings.guestDpi}")
            if (settings.debugNoPatch) appendLine("patch.auto=0")
            append(CallBridge.stateLines(ctx))  // phone calls: calls=1 also keeps FakeRIL loading (Calls.cs)
        }
        File(box, "state.ini").writeText(text)
        GuestSettings.write(File(box, "settings.ini"), settings)
        StorageGuest.write(box, ctx, settings)
    }

    fun send(command: String) {
        val name = "%08d".format(seq++)
        val tmp = File(inDir, "$name.tmp")
        tmp.writeText(command)
        tmp.renameTo(File(inDir, "$name.cmd"))
    }

    /** Keeps the guest's time zone equal to the phone's (current offset, including DST). */
    fun syncTimeZone(force: Boolean = false) {
        val tz = TimeZone.getDefault()
        val offsetMin = tz.getOffset(System.currentTimeMillis()) / 60000
        if (!force && offsetMin == lastTzOffset) return
        lastTzOffset = offsetMin
        // Windows bias: UTC = local + bias.
        send("tz ${-offsetMin} 0 ${tz.getDisplayName(tz.inDaylightTime(java.util.Date()), TimeZone.SHORT)}")
    }

    /** Handles messages from the guest; call periodically. */
    fun poll() {
        val files = outDir.listFiles { f -> f.name.endsWith(".msg") } ?: return
        files.sortBy { it.name }
        for (f in files) {
            val text = runCatching { f.readText().removePrefix("\uFEFF") }.getOrNull()
            f.delete()
            text?.lines()?.forEach { handle(it.trim()) }
        }
    }

    /** When the helper was last heard from (uptime ms), and how many commands wait for it. */
    var lastHeard = android.os.SystemClock.uptimeMillis()
        private set

    /**
     * The guest (re)starts running: silence is counted from now. Time spent paused, hibernated or
     * in the background (where nothing is polled) is not the helper's silence.
     */
    fun markAlive() { lastHeard = android.os.SystemClock.uptimeMillis() }
    fun pendingCommands(): Int = inDir.list()?.count { it.endsWith(".cmd") } ?: 0

    /** A current helper said hello this session (cleared by [prepare]). */
    private var helloSeen = false

    /** The helper runs and reads its mailbox: it said hello, and no command has waited 10 s. */
    fun helperResponsive(): Boolean {
        if (!helloSeen) return false
        val oldest = inDir.listFiles { f -> f.name.endsWith(".cmd") }?.minOfOrNull { it.lastModified() } ?: return true
        return System.currentTimeMillis() - oldest < 10_000
    }

    private fun handle(line: String) {
        if (line.isEmpty()) return
        lastHeard = android.os.SystemClock.uptimeMillis()
        Log.i(TAG, "guest: $line")
        when {
            line.startsWith("hello") -> {
                // A different helper build hands over to the one on the card and restarts.
                val sum = Regex("""sum=([0-9a-f]{8})""").find(line)?.groupValues?.get(1)
                if (sum != helperSum) {
                    Log.i(TAG, "guest helper is outdated ($sum != $helperSum): updating")
                    send("update")
                    return
                }
                syncTimeZone(force = true)
                helloSeen = true
                listener.onGuestHello()
            }
            line == "result $helloSeq unknown" -> {
                Log.i(TAG, "guest helper predates the hello command: updating")
                send("update")
            }
            line == "open settings" -> listener.onOpenSettings()
            line == "open storage" -> listener.onOpenStorage()
            // The Storage Card page in Windows Mobile (StorageGuest).
            line.startsWith("hostdir") -> StorageGuest.listDir(box, ctx, line.removePrefix("hostdir").trim())
            line == "storage refresh" -> writeState()
            line == "storage apply" -> if (StorageGuest.apply(box, settings)) { writeState(); listener.onSettingsChanged(true) }
            line == "open allfiles" -> listener.onOpenAllFiles()
            line.startsWith("patched ") -> listener.onGuestPatched()
            line.startsWith("ready ") -> listener.onGuestReady()
            line.startsWith("audio ") -> listener.onGuestAudio(line.removePrefix("audio ").trim())
            line.startsWith("share ") -> listener.onGuestShare(line.removePrefix("share ").trim())
            line == "radio refresh" -> listener.onGuestRadioRefresh()  // virtual modem (core/src/board/modem.h)
            line.startsWith("call ") -> listener.onGuestCall(line.removePrefix("call ").trim())  // CallBridge
            line.startsWith("pim ") -> listener.onGuestPim(line.removePrefix("pim ").trim())  // PimSync
            line.startsWith("irda ") -> listener.onGuestIrda(line.removePrefix("irda ").trim())  // InfraredBridge
            line.startsWith("sms ") -> listener.onGuestSms(line.removePrefix("sms ").trim())  // SmsBridge
            line == "clip" -> listener.onGuestClip()  // ClipboardBridge
            line == "remount" -> listener.onGuestRemount()
            line.startsWith("ime ") -> listener.onGuestIme(line.removePrefix("ime ").trim())  // ImeBridge
            line.startsWith("tlsask ") -> listener.onGuestTls(line.removePrefix("tlsask ").trim())  // SecureProxy
            line.startsWith("android ") -> listener.onGuestAndroid(line.removePrefix("android ").trim())  // LauncherBridge
            line.startsWith("launcher ") -> listener.onGuestLauncher(line)  // Launcher page buttons (AndroidApps.CMD_*)
            line in GuestSettings.ANDROID_COMMANDS -> listener.onGuestLauncher(line)  // Phone / Contacts / Infrared buttons
            line.startsWith("buttons ") -> {  // Keymap.HwButtons routes the buttons by it, helper or not
                settings.hwButtonMap = line.removePrefix("buttons ").trim()
                listener.onGuestButtons()
            }
            line.startsWith("set ") -> {
                val kv = line.removePrefix("set ").split('=', limit = 2)
                if (kv.size == 2) applySetting(kv[0], kv[1])
            }
        }
    }

    private fun applySetting(key: String, value: String) {
        // The WMDroid settings app in Windows Mobile (GuestSettings catalogue).
        if (GuestSettings.entries.any { it.key == key }) {
            val restart = GuestSettings.apply(settings, key, value) ?: return
            writeState()
            listener.onSettingsChanged(restart)
            return
        }
        var restart = false
        when (key) {
            "storage.mode" -> if (settings.storageMode != value) { settings.storageMode = value; restart = true }
            "screen.res" -> if (settings.screenRes != value) { settings.screenRes = value; restart = true }
            "resume" -> settings.resumeSession = value == "true"
            "stats" -> settings.showStats = value == "true"
            else -> return
        }
        writeState()
        listener.onSettingsChanged(restart)
    }

    companion object {
        const val TAG = "WMDroidGuest"
    }
}
