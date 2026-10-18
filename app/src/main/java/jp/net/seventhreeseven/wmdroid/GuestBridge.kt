package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.util.Log
import java.io.File
import java.util.TimeZone

/**
 * Talks to the WMDroid helper inside Windows Mobile.
 *
 * The app adds two hidden folders to every \Storage Card (FolderShare overlays):
 *   2577\autorun.exe  WM runs it whenever the card mounts: a native bootstrap that installs .NET CF
 *                     (3.5, or 2.0 on Windows Mobile 2003) if needed and starts the helper, 2577\helper.exe
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
        /** What the guest is sounding: "media", "notify" (or "alert") or "ring". */
        fun onGuestAudio(kind: String) {}
        /** Windows Mobile's own sliders, 0–100: media, ringtone, notification. */
        fun onGuestVolume(media: Int, ring: Int, notify: Int) {}
        /** Window title of the media player, when one is running (AudioClass). */
        fun onGuestTrack(title: String) {}
        /**
         * Now playing from the Windows Media Player plug-in (guest/WmpHook): [state] is
         * "playing", "paused" or "stopped", positions are milliseconds.
         */
        fun onGuestMeta(state: String, posMs: Long, durMs: Long, title: String, artist: String, album: String) {}
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
        /** InfraLobby is open (false) or closed (true). InfraredBridge writes lobby.txt. */
        fun onGuestLobby(stop: Boolean) {}
        /** The Today item is showing (false) or unloaded (true). Same list as InfraLobby. */
        fun onGuestLobbyToday(stop: Boolean) {}
        /** Text messages: a "sms ..." line from the helper, without the prefix (SmsBridge). */
        fun onGuestSms(line: String) {}
        /** "clip": Windows Mobile's clipboard text is in clip_out.txt (ClipboardBridge). */
        fun onGuestClip() {}
        /** "ime open" / "ime hide", without the prefix (ImeBridge). */
        fun onGuestIme(what: String) {}
        /** The helper found the card mounted under another name (\Storage Card2): put it back. */
        fun onGuestRemount() {}
        /** The guest clock jumped. Resend the current phone status so it can catch up. */
        fun onGuestSync() {}
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
    private val stashes = HashMap<String, ArrayDeque<ByteArray>>()
    private val net = GuestNet(
        onLine = { ingest(it) },
        onBlob = { name, data -> stash(name, data) },
    ).also { it.start() }
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
        // Windows Mobile 2003 is CE 4.2. Its helper is the CF 2.0 build (OsProfile.cf2); autorun
        // installs NETCFv2.wce4.ARMV4.cab before starting it. Later releases get the CF 3.5 helper.
        if (settings.profile?.helper == false) {
            writeState()
            synchronized(held) { held.clear(); running = false }
            return overlays()
        }
        val cf2 = settings.profile?.cf2 == true
        val guestExe = if (cf2) "WMDroid2003.exe" else "WMDroid.exe"
        val programs = mutableListOf(
            "Autorun.exe" to "autorun.exe", guestExe to "helper.exe",
        )
        for (name in listOf("GpsTest", "InfraLobby", "SendBluetooth")) {
            val asset = if (cf2) "${name}2003.exe" else "$name.exe"
            programs.add(asset to "$name.exe")
        }
        if (cf2) programs.add("NETCFv2.wce4.ARMV4.cab" to "NETCFv2.wce4.ARMV4.cab")
        else {
            programs.add("NETCFv35.wm.armv4i.cab" to "NETCFv35.wm.armv4i.cab")
            programs.add("MAPIlib.dll" to "MAPIlib.dll")
            programs.add("OpenNETCF.dll" to "OpenNETCF.dll")
        }
        for ((asset, name) in programs) {
            val exe = File(autorunDir, name)
            ctx.assets.open("guest/$asset").use { input ->
                val bytes = input.readBytes()
                if (name == "helper.exe") helperSum = "%08x".format(fnv(bytes))
                if (!exe.exists() || exe.length() != bytes.size.toLong() || !exe.readBytes().contentEquals(bytes))
                    exe.writeBytes(bytes)
            }
        }
        if (cf2) {
            File(autorunDir, "NETCFv35.wm.armv4i.cab").delete()
            File(autorunDir, "MAPIlib.dll").delete()
            File(autorunDir, "OpenNETCF.dll").delete()
        } else File(autorunDir, "NETCFv2.wce4.ARMV4.cab").delete()
        File(autorunDir, "WmpHook.dll").delete()  // the player plug-in is no longer shipped
        writeState()
        // A restored session already has its helper running and won't say hello by itself: ask, so an
        // outdated helper is noticed (one too old to know "hello" answers "unknown").
        helloSeq = "%08d".format(seq)
        helloSeen = false
        synchronized(held) { held.clear(); running = false }  // this session's commands start when it is up
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
        if (settings.profile?.helper != false) add("2577" to autorunDir.path)
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

    /**
     * Commands wait here until Windows Mobile is running (booted, not paused). Writing them onto
     * the card earlier leaves a mailbox the helper is not reading, which the next open treats as
     * a dead helper.
     */
    private val held = ArrayDeque<String>()
    private var running = false

    /** The guest is up. Queued commands go out on the sideband, or on TCP. */
    fun setRunning(on: Boolean) {
        val queued = ArrayList<String>()
        synchronized(held) {
            if (running == on) return
            running = on
            if (!on) return
            queued.addAll(held)
            held.clear()
        }
        queued.forEach { send(it) }
    }

    fun stopNet() { net.stop() }

    /** A blob that arrived on the TCP link, ahead of the line that names it. */
    fun stash(name: String, data: ByteArray) {
        synchronized(stashes) { stashes.getOrPut(name) { ArrayDeque() }.addLast(data) }
    }

    fun takeStash(name: String): ByteArray? = synchronized(stashes) {
        val q = stashes[name] ?: return null
        if (q.isEmpty()) null else q.removeFirst()
    }

    /** Large bytes, then a line, both on the TCP link so the line cannot overtake the bytes. */
    fun sendBlob(name: String, data: ByteArray) = net.sendBlob(name, data)
    fun sendOnNet(line: String) = net.sendLine(line)

    fun send(command: String) {
        if (settings.profile?.helper == false) return
        synchronized(held) {
            if (!running) { held.addLast(command); return }
        }
        // "update" is the one line the helper already in \Windows still reads from the card,
        // and only when the sideband did not take it.
        if (command == "update") {
            if (!deliver(command)) writeCmd(command)
            return
        }
        if (!deliver(command)) net.sendLine(command)
    }

    /** Sideband when the line is short and the page is up, otherwise a live TCP write. */
    private fun deliver(line: String): Boolean {
        if (line.length <= 220) {
            val pipe = if (line.startsWith("status ")) sideband else command
            if (pipe != null && pipe(line)) return true
        }
        return net.trySendLine(line)
    }

    /**
     * Command pipe (clip watch, ime watch, replies). False when the page is not answering;
     * the line then goes over TCP, not the storage card.
     */
    var command: ((String) -> Boolean)? = null

    /** Status pipe (the phone's radio and Wi-Fi line). A burst here cannot block [command]. */
    var sideband: ((String) -> Boolean)? = null

    /** Contact and calendar bytes, 1792 at a time. False when the bulk window is not up. */
    var bulk: ((ByteArray) -> Boolean)? = null

    fun pushBulk(data: ByteArray): Boolean = bulk?.invoke(data) == true

    /** One command on the command pipe, or on TCP when the page is not answering. */
    fun order(line: String) {
        if (!deliver(line)) net.sendLine(line)
    }

    /** One status line on the status pipe, or on TCP when the page is not answering. */
    fun tell(line: String) {
        if (!deliver(line)) net.sendLine(line)
    }

    /** Lines the helper wrote on the link. Same text as a mailbox message, no file. */
    fun ingest(text: String) {
        text.lineSequence().forEach { handle(it.trim()) }
    }

    /** The helper already running from \Windows still reads this one command off the card. */
    private fun writeCmd(command: String) {
        val name = synchronized(held) { "%08d".format(seq++) }
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
        order("tz ${-offsetMin} 0 ${tz.getDisplayName(tz.inDaylightTime(java.util.Date()), TimeZone.SHORT)}")
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

    /**
     * How long the oldest command the helper must answer has been waiting, or -1 when none is.
     * Status and the clipboard/keyboard watch are host chatter. An idle helper sends no pulse, so
     * silence since the last message is not a dead helper; only a command left unread is.
     */
    fun pendingWaitMs(): Long {
        val files = inDir.listFiles { it.name.endsWith(".cmd") } ?: return -1
        var oldest = Long.MAX_VALUE
        for (f in files) {
            val t = runCatching { f.readText() }.getOrNull() ?: continue
            if (t.startsWith("status ") || t.startsWith("clip watch ") || t.startsWith("ime watch ")) continue
            val modified = f.lastModified()
            if (modified < oldest) oldest = modified
        }
        if (oldest == Long.MAX_VALUE) return -1
        return System.currentTimeMillis() - oldest
    }

    /** A current helper said hello this session (cleared by [prepare]). */
    private var helloSeen = false

    /** The helper runs and reads its mailbox: it said hello, and no command has waited 10 s. */
    fun helperResponsive(): Boolean {
        if (!helloSeen) return false
        val oldest = inDir.listFiles { f -> f.name.endsWith(".cmd") }?.minOfOrNull { it.lastModified() } ?: return true
        return System.currentTimeMillis() - oldest < 10_000
    }

    /** "40 70 55" media, ringtone and notification percents from AudioClass. */
    private fun parseVol(body: String) {
        val n = body.split(' ')
        if (n.size < 3) return
        val media = n[0].toIntOrNull() ?: return
        val ring = n[1].toIntOrNull() ?: return
        val notify = n[2].toIntOrNull() ?: return
        listener.onGuestVolume(media, ring, notify)
    }

    /** "playing 1234 180000 title\tartist\talbum" from AudioClass.ReadMeta. */
    private fun parseMeta(body: String) {
        val parts = body.split(' ', limit = 4)
        if (parts.size < 4) return
        val pos = parts[1].toLongOrNull() ?: return
        val dur = parts[2].toLongOrNull() ?: return
        val fields = parts[3].split('\t', limit = 3)
        listener.onGuestMeta(parts[0], pos, dur, fields.getOrElse(0) { "" }, fields.getOrElse(1) { "" }, fields.getOrElse(2) { "" })
    }

    private fun handle(line: String) {
        if (line.isEmpty()) return
        lastHeard = android.os.SystemClock.uptimeMillis()
        if (line == "alive") return  // watchdog pulse while a command is slow; not a command result
        if (line.startsWith("result ")) return  // command-pipe reply; the helper already did the work
        if (line == "sync") { listener.onGuestSync(); return }
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
            line.startsWith("audio ") -> {
                val rest = line.removePrefix("audio ").trim()
                if (rest.startsWith("meta ")) parseMeta(rest.removePrefix("meta "))
                else if (rest.startsWith("title ")) listener.onGuestTrack(rest.removePrefix("title ").trim())
                else if (rest == "title") listener.onGuestTrack("")
                else if (rest.startsWith("vol ")) parseVol(rest.removePrefix("vol ").trim())
                else listener.onGuestAudio(rest)
            }
            line.startsWith("share ") -> listener.onGuestShare(line.removePrefix("share ").trim())
            line == "radio refresh" -> listener.onGuestRadioRefresh()  // virtual modem (core/src/board/modem.h)
            line.startsWith("call ") -> listener.onGuestCall(line.removePrefix("call ").trim())  // CallBridge
            line.startsWith("pim ") -> listener.onGuestPim(line.removePrefix("pim ").trim())  // PimSync
            line.startsWith("irda ") -> listener.onGuestIrda(line.removePrefix("irda ").trim())  // InfraredBridge
            line == "lobby" -> listener.onGuestLobby(false)  // InfraLobby is open (guest/InfraLobby)
            line == "lobby stop" -> listener.onGuestLobby(true)
            line == "lobby today" -> listener.onGuestLobbyToday(false)  // Today item (guest/Today)
            line == "lobby today stop" -> listener.onGuestLobbyToday(true)
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
