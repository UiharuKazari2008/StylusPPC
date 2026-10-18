package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.widget.Toast
import java.io.File

/**
 * Windows Mobile's infrared port, carried off the phone (docs/research-irda.md).
 *
 * The emulator core is the guest's IR transceiver (core/src/board/irda.cpp): it turns the IrLAP
 * frames Windows Mobile sends into byte arrays and back. This class moves them between the core and
 * a transport: [BleIrTransport] (nearby phones) or [InfraWanTransport] (a relay on the Internet).
 *
 * When a program starts using infrared the core reports "start"; with the "ask" setting the helper
 * shows Windows Mobile's own question page (guest/Helper/Irda.cs) and answers "irda use ble|wan|cancel
 * remember=0|1". The transport stays up while infrared is in use and [LINGER_MS] after, or for the
 * whole session with "Stay reachable".
 *
 * InfraLobby (guest/InfraLobby) asks who is in the room: "lobby" while its window is open,
 * "lobby stop" when it closes. The Today item (guest/Today) does the same with "lobby today".
 * This writes .wmdroid\lobby.txt. If infrared is already on InfraWAN, that connection's list is
 * used. Otherwise a second connection only watches the room, and it leaves a few seconds after
 * both the app and the Today item have gone quiet.
 *
 * All native calls happen on the main thread (the machine is created and destroyed there).
 */
class InfraredBridge(private val act: Activity, private val handle: () -> Long,
                     private val guest: () -> GuestBridge?) {
    companion object {
        private const val TAG = "WMDroidIr"
        private const val LINGER_MS = 5 * 60_000L     // transport kept after the guest went quiet
        private const val FAST_MS = 10L               // frame pump while infrared is in use
        private const val SLOW_MS = 250L              // events only
    }

    private val ui = Handler(Looper.getMainLooper())
    private val ir = InfraredSettings(act)
    private var transport: IrTransport? = null
    private var kind = ""
    private var linked = false
    private var running = false
    private var quietSince = 0L
    private var lastStatus = ""
    private var waitingForPermission = false
    private var lastHandle = 0L
    private var generation = 0  // callbacks from a transport that was since closed are ignored
    private var lobbyHeld = false
    private var lobbySeen = 0L
    private var todayHeld = false
    private var todaySeen = 0L
    private var watch: InfraWanTransport? = null
    private var watchGen = 0

    /** The session runs (after create/restore and on resume). */
    fun start() {
        if (running) return
        running = true
        quietSince = SystemClock.uptimeMillis()
        if (ir.listen && (ir.mode == "ble" || ir.mode == "wan")) open(ir.mode)
        ui.post(tick)
    }

    /** The session pauses or ends: the transport goes (other devices see this one leave). */
    fun stop() {
        running = false
        ui.removeCallbacks(tick)
        lobbyHeld = false
        todayHeld = false
        close()
        stopWatch()  // lobby presence leaves with the session
    }

    /** A line from the helper after "irda ": "use ble|wan|cancel remember=0|1". */
    fun onGuest(line: String) {
        val p = line.split(' ')
        if (p.getOrNull(0) != "use") return
        val choice = p.getOrNull(1) ?: return
        val remember = p.any { it == "remember=1" }
        if (choice != "ble" && choice != "wan") return
        ir.last = choice
        if (remember) ir.mode = choice
        open(choice)
    }

    /** InfraLobby is open ([stop] is false) or has closed. Runs on the main thread. */
    fun onLobby(stop: Boolean) {
        if (stop) {
            lobbyHeld = false
            lobbySeen = 0
            dropLobbyIfIdle()
            return
        }
        lobbyHeld = true
        lobbySeen = SystemClock.uptimeMillis()
        refreshLobby()
    }

    /** The Today screen item is showing ([stop] is false) or has been unloaded. */
    fun onLobbyToday(stop: Boolean) {
        if (stop) {
            todayHeld = false
            todaySeen = 0
            dropLobbyIfIdle()
            return
        }
        todayHeld = true
        todaySeen = SystemClock.uptimeMillis()
        refreshLobby()
    }

    private fun wantLobby() = lobbyHeld || todayHeld

    private fun dropLobbyIfIdle() {
        if (wantLobby()) return
        val watching = watch != null
        stopWatch()
        if (watching && kind != "wan") writeLobby(false, "Not in the lobby", emptyList())
    }

    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
            val now = SystemClock.uptimeMillis()
            var quiet = false
            if (lobbyHeld && now - lobbySeen > 8000) { lobbyHeld = false; lobbySeen = 0; quiet = true }
            if (todayHeld && now - todaySeen > 8000) { todayHeld = false; todaySeen = 0; quiet = true }
            if (quiet) dropLobbyIfIdle()
            val h = handle()
            if (h != lastHandle) {  // a new machine (restart, restore): tell it the link state
                lastHandle = h
                if (h != 0L && linked) IrdaNative.setLinked(h, true)
            }
            if (h != 0L) {
                for (e in IrdaNative.takeEvents(h).lines()) when (e) {
                    "start" -> onStart()
                    "idle" -> quietSince = SystemClock.uptimeMillis()
                }
                // Frames from Windows Mobile to the transport.
                val t = transport
                if (t != null && linked) {
                    while (true) t.send(IrdaNative.takeOutgoing(h) ?: break)
                }
                if (IrdaNative.active(h)) quietSince = SystemClock.uptimeMillis()
                else if (t != null && !ir.listen && SystemClock.uptimeMillis() - quietSince > LINGER_MS) {
                    Log.i(TAG, "infrared quiet for ${LINGER_MS / 60000} min: closing $kind")
                    close()
                }
            }
            ui.postDelayed(this, if (transport != null && h != 0L && IrdaNative.active(h)) FAST_MS else SLOW_MS)
        }
    }

    // A program started using infrared.
    private fun onStart() {
        quietSince = SystemClock.uptimeMillis()
        if (transport != null) return  // already connected: the guest's frames flow
        when (ir.mode) {
            "off" -> {}
            "ble", "wan" -> open(ir.mode)
            else -> guest()?.send("irda ask ${ir.last}")
        }
    }

    private fun open(which: String) {
        if (transport != null && kind == which) return
        close()
        if (which == "wan" && !ir.wanConfigured) {
            say("InfraWAN isn't set up: Settings > Infrared")
            return
        }
        if (which == "ble" && !InfraredSettings.hasBlePermissions(act)) {
            // Ask, then try again once the answer is in.
            if (!waitingForPermission) {
                waitingForPermission = true
                act.requestPermissions(InfraredSettings.blePermissions(), 41)
                ui.postDelayed({ waitingForPermission = false; if (running && InfraredSettings.hasBlePermissions(act)) open("ble") }, 8000)
            }
            return
        }
        if (which == "wan") stopWatch()  // one presence: the infrared connection is the lobby list
        kind = which
        val sink = Sink(++generation)
        val t = if (which == "ble") BleIrTransport(act, ir, sink) else InfraWanTransport(ir, sink)
        transport = t
        Log.i(TAG, "opening $which")
        t.start()
    }

    private fun close() {
        val t = transport ?: return
        transport = null
        kind = ""
        generation++
        setLinked(false)
        t.stop()
    }

    private fun setLinked(on: Boolean) {
        linked = on
        val h = handle()
        if (h != 0L) IrdaNative.setLinked(h, on)
    }

    private fun say(text: String) {
        if (text == lastStatus) return
        lastStatus = text
        Log.i(TAG, text)
        guest()?.send("irda status $text")  // shown on the question page while it is open
        // InfraWAN announces the room on every join and leave. That stays in the log and on the
        // question page; it is not a toast.
        if (!text.startsWith("InfraWAN")) Toast.makeText(act, text, Toast.LENGTH_SHORT).show()
    }

    // ---- Transport callbacks (their threads) -> main thread ----

    private inner class Sink(private val gen: Int) : IrTransport.Listener {
        private fun current() = gen == generation && transport != null

        override fun onFrame(node: Int, frame: ByteArray) {
            ui.post {
                val h = handle()
                if (current() && h != 0L && linked) IrdaNative.fromPeer(h, node, frame)
            }
        }

        override fun onPeerGone(node: Int) {
            ui.post { val h = handle(); if (current() && h != 0L) IrdaNative.peerGone(h, node) }
        }

        override fun onState(up: Boolean, text: String) {
            ui.post {
                if (!current()) return@post
                setLinked(up)
                say(text)
                if (wantLobby() && kind == "wan") (transport as? InfraWanTransport)?.let { writeFrom(it) }
            }
        }
    }

    // ---- InfraLobby: who is in the room (guest/InfraLobby) ----

    private fun refreshLobby() {
        if (!wantLobby()) return
        if (!ir.wanConfigured) {
            stopWatch()
            writeLobby(false, "Set the InfraWAN server and user in the phone's PocketStylus settings.", emptyList())
            return
        }
        val live = transport as? InfraWanTransport
        if (live != null && kind == "wan") {
            stopWatch()
            writeFrom(live)
            return
        }
        startWatch()
    }

    private fun startWatch() {
        val existing = watch
        if (existing != null) {
            writeFrom(existing)
            return
        }
        val w = InfraWanTransport(ir, WatchSink(++watchGen))
        watch = w
        writeLobby(false, "Connecting to ${ir.room}…", emptyList())
        w.start()
    }

    private fun stopWatch() {
        val w = watch ?: return
        watch = null
        watchGen++
        w.stop()
    }

    private fun writeFrom(t: InfraWanTransport) {
        val names = t.peerNames()
        val status = if (t.isUp()) {
            if (names.isEmpty()) "In ${ir.room}. Nobody else is here yet."
            else "In ${ir.room}."
        } else t.statusText()
        writeLobby(t.isUp(), status, names)
    }

    private fun writeLobby(connected: Boolean, status: String, names: List<String>) {
        val file = guest()?.boxFile("lobby.txt") ?: return
        val body = buildString {
            append("room=").appendLine(oneLine(ir.room.ifEmpty { "lobby" }))
            append("up=").appendLine(if (connected) "1" else "0")
            append("status=").appendLine(oneLine(status))
            append("you=").appendLine(oneLine(ir.name))
            for (n in names) append("peer=").appendLine(oneLine(n))
        }
        runCatching {
            if (file.exists() && file.readText() == body) return
            file.parentFile?.mkdirs()
            val tmp = File(file.parentFile, "lobby.txt.tmp")
            tmp.writeText(body)
            if (file.exists()) file.delete()
            if (!tmp.renameTo(file)) file.writeText(body)
        }
    }

    private fun oneLine(s: String) = s.replace('\n', ' ').replace('\r', ' ')

    private inner class WatchSink(private val gen: Int) : IrTransport.Listener {
        private fun current() = gen == watchGen && watch != null

        override fun onFrame(node: Int, frame: ByteArray) {
            // Presence only. Infrared frames stay on the real transport (open wan stops this watch).
        }

        override fun onPeerGone(node: Int) {}

        override fun onState(up: Boolean, text: String) {
            ui.post {
                if (!current()) return@post
                watch?.let { writeFrom(it) }
            }
        }
    }
}
