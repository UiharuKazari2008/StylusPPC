package jp.net.seventhreeseven.wmdroid

import android.os.Handler
import android.os.Looper
import android.util.Log
import java.io.InputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.charset.StandardCharsets

/**
 * The guest reaches this at 10.0.2.2:7378. That address is the phone itself (UserNet), so a large
 * transfer never goes through the storage card. Short lines still prefer the sideband.
 *
 * One connection. Frames are little-endian: u32 length, u8 type, payload.
 * Type 1 is a UTF-8 line. Type 2 is a blob: u16 name length, name, bytes.
 */
class GuestNet(
    private val onLine: (String) -> Unit,
    private val onBlob: (String, ByteArray) -> Unit,
) {
    private val ui = Handler(Looper.getMainLooper())
    private val outQ = ArrayDeque<ByteArray>()
    private var server: ServerSocket? = null
    private var sock: Socket? = null
    @Volatile private var running = false

    fun start() {
        if (running) return
        running = true
        Thread({
            while (running) {
                try {
                    val ss = ServerSocket(PORT, 1, InetAddress.getByName("127.0.0.1"))
                    server = ss
                    while (running) {
                        val s = ss.accept()
                        s.tcpNoDelay = true
                        attach(s)
                        read(s)
                    }
                } catch (e: Exception) {
                    if (running) Log.w(TAG, "guest net: ${e.message}")
                    try { Thread.sleep(1000) } catch (_: InterruptedException) { }
                }
            }
        }, "guest-net").apply { isDaemon = true }.start()
    }

    fun stop() {
        running = false
        synchronized(outQ) {
            try { sock?.close() } catch (_: Exception) { }
            sock = null
        }
        try { server?.close() } catch (_: Exception) { }
        server = null
    }

    /** True when the frame was written to the guest, not merely queued. */
    fun trySendLine(line: String): Boolean = write(frame(LINE, line.toByteArray(StandardCharsets.UTF_8)), queue = false)

    fun sendLine(line: String) { write(frame(LINE, line.toByteArray(StandardCharsets.UTF_8)), queue = true) }

    fun sendBlob(name: String, data: ByteArray) {
        val nb = name.toByteArray(StandardCharsets.UTF_8)
        val payload = ByteArray(2 + nb.size + data.size)
        payload[0] = nb.size.toByte()
        payload[1] = (nb.size shr 8).toByte()
        System.arraycopy(nb, 0, payload, 2, nb.size)
        System.arraycopy(data, 0, payload, 2 + nb.size, data.size)
        write(frame(BLOB, payload), queue = true)
    }

    private fun attach(s: Socket) {
        synchronized(outQ) {
            try { sock?.close() } catch (_: Exception) { }
            sock = s
            val outs = s.getOutputStream()
            while (outQ.isNotEmpty()) outs.write(outQ.removeFirst())
        }
    }

    private fun write(frame: ByteArray, queue: Boolean): Boolean {
        synchronized(outQ) {
            val s = sock
            if (s != null) {
                try {
                    s.getOutputStream().write(frame)
                    return true
                } catch (_: Exception) {
                    try { s.close() } catch (_: Exception) { }
                    sock = null
                }
            }
            if (queue && outQ.size < 64) outQ.addLast(frame)
            return false
        }
    }

    private fun read(s: Socket) {
        val input = s.getInputStream()
        val hdr = ByteArray(5)
        try {
            while (running && readFull(input, hdr)) {
                val n = ByteBuffer.wrap(hdr, 0, 4).order(ByteOrder.LITTLE_ENDIAN).int
                if (n < 0 || n > 8 * 1024 * 1024) break
                val payload = ByteArray(n)
                if (n > 0 && !readFull(input, payload)) break
                when (hdr[4].toInt() and 0xFF) {
                    LINE -> {
                        val text = String(payload, StandardCharsets.UTF_8)
                        ui.post { onLine(text) }
                    }
                    BLOB -> {
                        if (payload.size < 2) continue
                        val nameLen = (payload[0].toInt() and 0xFF) or ((payload[1].toInt() and 0xFF) shl 8)
                        if (nameLen < 0 || 2 + nameLen > payload.size) continue
                        val name = String(payload, 2, nameLen, StandardCharsets.UTF_8)
                        val data = payload.copyOfRange(2 + nameLen, payload.size)
                        ui.post { onBlob(name, data) }
                    }
                }
            }
        } catch (_: Exception) { }
        synchronized(outQ) { if (sock === s) sock = null }
        try { s.close() } catch (_: Exception) { }
    }

    private fun readFull(input: InputStream, buf: ByteArray): Boolean {
        var o = 0
        while (o < buf.size) {
            val r = input.read(buf, o, buf.size - o)
            if (r < 0) return false
            o += r
        }
        return true
    }

    private fun frame(type: Int, payload: ByteArray): ByteArray {
        val out = ByteArray(5 + payload.size)
        ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN).putInt(payload.size)
        out[4] = type.toByte()
        System.arraycopy(payload, 0, out, 5, payload.size)
        return out
    }

    companion object {
        const val PORT = 7378
        private const val LINE = 1
        private const val BLOB = 2
        private const val TAG = "WMDroid"
    }
}
