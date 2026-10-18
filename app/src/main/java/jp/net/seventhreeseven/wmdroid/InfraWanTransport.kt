package jp.net.seventhreeseven.wmdroid

import android.util.Base64
import android.util.Log
import org.json.JSONObject
import java.io.DataInputStream
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.Socket
import java.net.URI
import java.security.MessageDigest
import java.security.SecureRandom
import java.util.concurrent.LinkedBlockingQueue
import java.util.concurrent.TimeUnit
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLSocket
import javax.net.ssl.SSLSocketFactory

/** A way out of the phone for Windows Mobile's infrared frames (InfraredBridge). */
interface IrTransport {
    /** Callbacks come on the transport's own threads. */
    interface Listener {
        /** A frame from another device ([node]: this transport's id for it). */
        fun onFrame(node: Int, frame: ByteArray)
        fun onPeerGone(node: Int)
        /** Connected or not, with a short description for the user ("InfraWAN: room lab, 2 others"). */
        fun onState(up: Boolean, text: String)
    }

    fun start()
    fun stop()
    /** Queues one frame for every device on this transport; never blocks. */
    fun send(frame: ByteArray)
}

/**
 * InfraWAN: the WebSocket relay in server/infrawan (protocol in its server.js). Every frame goes to
 * everyone in the room. A minimal RFC 6455 client over a plain or TLS socket, so the app needs no
 * extra library; it reconnects with a back-off while started.
 */
class InfraWanTransport(private val ir: InfraredSettings, private val listener: IrTransport.Listener) : IrTransport {
    @Volatile private var running = false
    @Volatile private var socket: Socket? = null
    private var worker: Thread? = null
    private val outbox = LinkedBlockingQueue<ByteArray>(512)
    private val rng = SecureRandom()
    private val peers = HashMap<Int, String>()
    @Volatile private var up = false
    @Volatile private var statusText = "Connecting…"

    fun isUp() = up
    fun statusText() = statusText

    /** Other devices in the room, in a stable order. The same display name twice keeps its id. */
    fun peerNames(): List<String> {
        val list = synchronized(peers) { peers.entries.map { it.value to it.key }.sortedBy { it.second } }
        val counts = HashMap<String, Int>()
        for ((name, _) in list) counts[name] = (counts[name] ?: 0) + 1
        return list.map { (name, id) -> if ((counts[name] ?: 0) > 1) "$name (#$id)" else name }
    }

    override fun start() {
        if (running) return
        running = true
        worker = Thread({ loop() }, "infrawan").apply { isDaemon = true; start() }
    }

    override fun stop() {
        running = false
        runCatching { socket?.close() }
        worker?.interrupt()
        worker = null
    }

    override fun send(frame: ByteArray) {
        if (socket != null) outbox.offer(frame)  // a full queue drops the frame, as a busy IR link would
    }

    private fun loop() {
        var backoff = 2000L
        while (running) {
            val err = runCatching { session() }.exceptionOrNull()
            lastError = err?.message
            if (!running) break
            report(false, "InfraWAN: " + (err?.message ?: "disconnected") + "; retrying")
            Log.i(TAG, "session ended: ${err?.message}")
            try { Thread.sleep(backoff) } catch (e: InterruptedException) { break }
            backoff = (backoff * 2).coerceAtMost(30000)
        }
        report(false, "InfraWAN: " + (lastError ?: "off"))
    }

    @Volatile private var lastError: String? = null

    // One connection: handshake, hello, then frames both ways until it breaks.
    private fun session() {
        val s = connect(ir.server)
        socket = s
        try {
            val out = s.getOutputStream()
            val inp = DataInputStream(s.getInputStream().buffered())
            sendFrame(out, 1, hello(ir).toByteArray())
            outbox.clear()
            val writer = Thread({
                try {
                    var lastPing = System.currentTimeMillis()
                    while (running && !s.isClosed) {
                        val f = outbox.poll(1, TimeUnit.SECONDS)
                        if (f != null) sendFrame(out, 2, byteArrayOf(1) + f)
                        if (System.currentTimeMillis() - lastPing > 20000) { sendFrame(out, 9, ByteArray(0)); lastPing = System.currentTimeMillis() }
                    }
                } catch (e: Exception) { runCatching { s.close() } }
            }, "infrawan-tx").apply { isDaemon = true; start() }
            try {
                while (running) {
                    val (op, payload) = readMessage(inp, out)
                    when (op) {
                        1 -> onText(String(payload, Charsets.UTF_8))
                        2 -> if (payload.size > 5 && payload[0].toInt() == 1) {
                            val src = (payload[1].toInt() and 255) or ((payload[2].toInt() and 255) shl 8) or
                                ((payload[3].toInt() and 255) shl 16) or ((payload[4].toInt() and 255) shl 24)
                            listener.onFrame(src, payload.copyOfRange(5, payload.size))
                        }
                        8 -> throw java.io.IOException("closed by the relay")
                    }
                }
            } finally {
                writer.interrupt()
            }
        } finally {
            socket = null
            runCatching { s.close() }
            synchronized(peers) { for (id in peers.keys) listener.onPeerGone(id); peers.clear() }
        }
    }

    private fun onText(text: String) {
        val m = runCatching { JSONObject(text) }.getOrNull() ?: return
        when (m.optString("type")) {
            "welcome" -> {
                val list = m.optJSONArray("peers")
                synchronized(peers) {
                    peers.clear()
                    for (i in 0 until (list?.length() ?: 0)) list!!.getJSONObject(i).let { peers[it.optInt("id")] = it.optString("name") }
                }
                announce()
            }
            "join" -> { synchronized(peers) { peers[m.optInt("id")] = m.optString("name") }; announce() }
            "leave" -> {
                val id = m.optInt("id")
                synchronized(peers) { peers.remove(id) }
                listener.onPeerGone(id)
                announce()
            }
            "error" -> {
                // A wrong account or protocol won't fix itself by retrying.
                if (m.optString("code") in listOf("auth", "proto")) running = false
                throw java.io.IOException(m.optString("message", m.optString("code")))
            }
        }
    }

    private fun report(isUp: Boolean, text: String) {
        up = isUp
        statusText = text
        listener.onState(isUp, text)
    }

    private fun announce() {
        val names = synchronized(peers) { peers.values.toList() }
        report(true, "InfraWAN: room ${ir.room}, " + when (names.size) {
            0 -> "nobody else here yet"
            1 -> "with ${names[0]}"
            else -> "${names.size} others (${names.take(3).joinToString(", ")}${if (names.size > 3) ", …" else ""})"
        })
    }

    private fun sendFrame(out: OutputStream, op: Int, p: ByteArray) = Companion.sendFrame(out, op, p, rng)

    companion object {
        private const val TAG = "WMDroidIrWan"
        private const val GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

        fun hello(ir: InfraredSettings): String = JSONObject().apply {
            put("type", "hello"); put("version", 1); put("room", ir.room); put("user", ir.user)
            put("token", ir.token); put("name", ir.name)
        }.toString()

        /** TCP (+TLS for wss) and the WebSocket upgrade. */
        fun connect(url: String): Socket {
            val u = URI(url.trim())
            val tls = when (u.scheme?.lowercase()) { "wss" -> true; "ws" -> false; else -> throw IllegalArgumentException("server must start with wss:// or ws://") }
            val host = u.host ?: throw IllegalArgumentException("no host in the server address")
            val port = if (u.port > 0) u.port else if (tls) 443 else 80
            val raw = Socket()
            raw.connect(InetSocketAddress(host, port), 10000)
            raw.tcpNoDelay = true
            val s: Socket = if (tls) {
                val ssl = (SSLSocketFactory.getDefault() as SSLSocketFactory).createSocket(raw, host, port, true) as SSLSocket
                ssl.startHandshake()
                if (!HttpsURLConnection.getDefaultHostnameVerifier().verify(host, ssl.session)) {
                    ssl.close(); throw java.io.IOException("the server's certificate is not for $host")
                }
                ssl
            } else raw
            s.soTimeout = 60000  // the relay pings every 25 s
            val key = ByteArray(16).also { SecureRandom().nextBytes(it) }
            val k64 = Base64.encodeToString(key, Base64.NO_WRAP)
            val path = (u.rawPath?.ifEmpty { "/" } ?: "/") + (u.rawQuery?.let { "?$it" } ?: "")
            val req = "GET $path HTTP/1.1\r\nHost: $host${if (u.port > 0) ":$port" else ""}\r\nUpgrade: websocket\r\n" +
                "Connection: Upgrade\r\nSec-WebSocket-Key: $k64\r\nSec-WebSocket-Version: 13\r\n\r\n"
            s.getOutputStream().write(req.toByteArray(Charsets.US_ASCII))
            val head = readHead(s.getInputStream())
            if (!head.startsWith("HTTP/1.1 101")) { s.close(); throw java.io.IOException("relay refused: " + head.lineSequence().first()) }
            val want = Base64.encodeToString(MessageDigest.getInstance("SHA-1").digest((k64 + GUID).toByteArray()), Base64.NO_WRAP)
            if (!head.contains(want)) { s.close(); throw java.io.IOException("not a WebSocket server") }
            return s
        }

        private fun readHead(i: InputStream): String {
            val sb = StringBuilder()
            while (!sb.endsWith("\r\n\r\n")) {
                val c = i.read()
                if (c < 0 || sb.length > 8192) throw java.io.IOException("connection closed during the handshake")
                sb.append(c.toChar())
            }
            return sb.toString()
        }

        fun sendFrame(out: OutputStream, op: Int, p: ByteArray, rng: SecureRandom) {
            val n = p.size
            val head = java.io.ByteArrayOutputStream(14 + n)
            head.write(0x80 or op)
            when {
                n < 126 -> head.write(0x80 or n)
                n < 65536 -> { head.write(0x80 or 126); head.write(n shr 8); head.write(n and 255) }
                else -> { head.write(0x80 or 127); for (i in 7 downTo 0) head.write(((n.toLong() shr (8 * i)) and 255).toInt()) }
            }
            val mask = ByteArray(4).also { rng.nextBytes(it) }
            head.write(mask)
            for (i in 0 until n) head.write(p[i].toInt() xor mask[i and 3].toInt())
            synchronized(out) { out.write(head.toByteArray()); out.flush() }
        }

        /** One whole message (continuations joined); answers pings. */
        fun readMessage(inp: DataInputStream, out: OutputStream): Pair<Int, ByteArray> {
            var op = 0
            val msg = java.io.ByteArrayOutputStream()
            while (true) {
                val b0 = inp.readUnsignedByte()
                val b1 = inp.readUnsignedByte()
                var n = (b1 and 127).toLong()
                if (n == 126L) n = inp.readUnsignedShort().toLong() else if (n == 127L) n = inp.readLong()
                if (n > (1 shl 20)) throw java.io.IOException("message too large")
                val mask = if (b1 and 0x80 != 0) ByteArray(4).also { inp.readFully(it) } else null
                val p = ByteArray(n.toInt()).also { inp.readFully(it) }
                if (mask != null) for (i in p.indices) p[i] = (p[i].toInt() xor mask[i and 3].toInt()).toByte()
                val fop = b0 and 15
                when (fop) {
                    9 -> { sendFrame(out, 10, p, SecureRandom()); continue }
                    10 -> continue
                    8 -> return 8 to p
                }
                if (fop != 0) op = fop
                msg.write(p)
                if (b0 and 0x80 != 0) return op to msg.toByteArray()
            }
        }

        /** Settings > Test connection: connects, reads the welcome, and says who is in the room. */
        fun probe(ir: InfraredSettings): String = try {
            val s = connect(ir.server)
            s.use {
                val out = it.getOutputStream()
                val inp = DataInputStream(it.getInputStream().buffered())
                sendFrame(out, 1, hello(ir).toByteArray(), SecureRandom())
                val (op, p) = readMessage(inp, out)
                val m = if (op == 1) JSONObject(String(p, Charsets.UTF_8)) else JSONObject()
                when (m.optString("type")) {
                    "welcome" -> {
                        val n = m.optJSONArray("peers")?.length() ?: 0
                        "Connected to room ${ir.room}: " + if (n == 0) "nobody else is there now." else "$n other device(s) there now."
                    }
                    "error" -> "The relay said: " + m.optString("message")
                    else -> "Unexpected answer from the relay."
                }
            }
        } catch (e: Exception) {
            "Could not connect: ${e.message}"
        }
    }
}
