package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.widget.Toast

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
        close()
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

    private val tick = object : Runnable {
        override fun run() {
            if (!running) return
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
        Toast.makeText(act, text, Toast.LENGTH_SHORT).show()
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
            }
        }
    }
}
