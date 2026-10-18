package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.widget.Toast
import java.io.File
import java.io.IOException
import java.io.InputStream
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException
import kotlin.concurrent.thread

/**
 * Receives a Windows Mobile CAB from the PC drop window and writes it onto the storage card.
 * The drop window finds the phone with a UDP broadcast ("WMDCAB?") and then sends one CAB per
 * TCP connection: a "PUT name.cab" line, a 4-byte big-endian length, and the bytes.
 * Listens only while the emulator session exists (EmulatorActivity).
 */
object CardDrop {
    private const val TAG = "WMDroid"
    const val PORT = 7377
    private const val MAX = 64 * 1024 * 1024

    private val live = java.util.concurrent.atomic.AtomicBoolean(false)
    private var tcp: ServerSocket? = null
    private var udp: DatagramSocket? = null
    private var mlock: WifiManager.MulticastLock? = null

    fun start(ctx: Context) {
        if (!live.compareAndSet(false, true)) return
        val app = ctx.applicationContext
        try {
            val wifi = app.getSystemService(Context.WIFI_SERVICE) as WifiManager
            mlock = wifi.createMulticastLock("wmdroid-card").apply {
                setReferenceCounted(false)
                acquire()
            }
        } catch (e: RuntimeException) {
            Log.w(TAG, "card drop: no multicast lock", e)
        }
        Log.i(TAG, "card drop: listening on ${lanAddress() ?: "0.0.0.0"}:$PORT")
        thread(name = "card-drop", isDaemon = true) { tcpLoop(app) }
        thread(name = "card-drop-udp", isDaemon = true) { udpLoop() }
    }

    fun stop() {
        if (!live.compareAndSet(true, false)) return
        runCatching { tcp?.close() }
        runCatching { udp?.close() }
        runCatching { if (mlock?.isHeld == true) mlock?.release() }
        mlock = null
    }

    private fun tcpLoop(app: Context) {
        val server = ServerSocket()
        try {
            server.reuseAddress = true
            server.soTimeout = 1000
            server.bind(InetSocketAddress(PORT))
        } catch (e: IOException) {
            Log.w(TAG, "card drop: port $PORT is in use", e)
            runCatching { server.close() }
            stop()
            return
        }
        tcp = server
        while (live.get()) {
            val client = try {
                server.accept()
            } catch (_: SocketTimeoutException) {
                continue
            } catch (_: IOException) {
                break
            }
            try {
                client.soTimeout = 20_000
                take(app, client)
            } catch (e: Exception) {
                Log.w(TAG, "card drop: ${e.message}")
                runCatching { client.getOutputStream().write("ERR ${e.message}\n".toByteArray()) }
            } finally {
                runCatching { client.close() }
            }
        }
        runCatching { server.close() }
    }

    private fun take(app: Context, client: Socket) {
        val input = client.getInputStream()
        val line = readLine(input)
        if (!line.startsWith("PUT ")) throw IOException("expected PUT")
        val name = cabName(line.removePrefix("PUT ").trim())
        val len = readInt(input)
        if (len !in 1..MAX) throw IOException("refusing $len bytes")
        val body = readFully(input, len)
        if (body.size < 4 || body[0] != 'M'.code.toByte() || body[1] != 'S'.code.toByte() ||
            body[2] != 'C'.code.toByte() || body[3] != 'F'.code.toByte())
            throw IOException("not a cabinet")
        val dir = cardRoot(app)
        if (!dir.isDirectory || !dir.canWrite()) throw IOException("storage card is not writable")
        val dest = File(dir, name)
        val tmp = File(dir, "$name.part")
        tmp.outputStream().use { it.write(body) }
        if (dest.exists() && !dest.delete()) throw IOException("could not replace $name")
        if (!tmp.renameTo(dest)) {
            tmp.copyTo(dest, overwrite = true)
            tmp.delete()
        }
        Log.i(TAG, "card drop: ${dest.path} ($len bytes)")
        client.getOutputStream().write("OK $len\n".toByteArray())
        Handler(Looper.getMainLooper()).post {
            Toast.makeText(app, "$name is on the storage card", Toast.LENGTH_SHORT).show()
        }
    }

    private fun udpLoop() {
        val sock = DatagramSocket(null)
        try {
            sock.reuseAddress = true
            sock.broadcast = true
            sock.soTimeout = 1000
            sock.bind(InetSocketAddress(PORT))
        } catch (e: IOException) {
            Log.w(TAG, "card drop: discovery port is in use", e)
            return
        }
        udp = sock
        val buf = ByteArray(64)
        val reply = "WMDCAB $PORT".toByteArray()
        while (live.get()) {
            val pkt = DatagramPacket(buf, buf.size)
            try {
                sock.receive(pkt)
            } catch (_: SocketTimeoutException) {
                continue
            } catch (_: IOException) {
                break
            }
            val msg = String(pkt.data, 0, pkt.length, Charsets.US_ASCII).trim()
            if (msg == "WMDCAB?") {
                runCatching { sock.send(DatagramPacket(reply, reply.size, pkt.address, pkt.port)) }
            }
        }
        runCatching { sock.close() }
    }

    /** The folder Windows Mobile has mounted as \Storage Card. */
    private fun cardRoot(ctx: Context): File {
        val settings = Settings(ctx)
        val path = StorageBridge.mounts(ctx, settings).firstOrNull { it.first.isEmpty() }?.second
            ?: StorageBridge.privateDir(ctx).path
        return File(path).apply { mkdirs() }
    }

    private fun cabName(raw: String): String {
        val base = raw.replace('\\', '/').substringAfterLast('/')
        if (base != raw || !base.endsWith(".cab", ignoreCase = true) || base.length > 120 ||
            base.any { it.code < 32 || it in "<>:\"|?*" })
            throw IOException("bad name")
        return base
    }

    private fun readLine(input: InputStream): String {
        val out = StringBuilder()
        while (out.length < 200) {
            val b = input.read()
            if (b < 0) break
            if (b == '\n'.code) break
            if (b != '\r'.code) out.append(b.toChar())
        }
        return out.toString()
    }

    private fun readInt(input: InputStream): Int {
        val b = readFully(input, 4)
        return ((b[0].toInt() and 0xff) shl 24) or ((b[1].toInt() and 0xff) shl 16) or
            ((b[2].toInt() and 0xff) shl 8) or (b[3].toInt() and 0xff)
    }

    private fun readFully(input: InputStream, n: Int): ByteArray {
        val buf = ByteArray(n)
        var off = 0
        while (off < n) {
            val r = input.read(buf, off, n - off)
            if (r < 0) throw IOException("connection closed")
            off += r
        }
        return buf
    }

    private fun lanAddress(): String? {
        val ifs = NetworkInterface.getNetworkInterfaces() ?: return null
        for (nif in ifs) {
            if (!nif.isUp || nif.isLoopback) continue
            for (addr in nif.inetAddresses)
                if (addr is Inet4Address && !addr.isLoopbackAddress) return addr.hostAddress
        }
        return null
    }
}
