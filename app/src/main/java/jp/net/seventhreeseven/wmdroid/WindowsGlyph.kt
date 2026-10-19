package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import java.io.DataInputStream
import java.io.DataOutputStream
import java.io.File
import java.io.RandomAccessFile
import kotlin.concurrent.thread
import kotlin.math.atan2
import kotlin.math.max
import kotlin.math.min

/**
 * The Windows key's mark. Four squares until the installed ROM's own Windows flag
 * (`\Windows\wm_flag.bmp` in the IMGFS) has been read and traced into a path. The traced
 * logo is not shipped with the app; it comes from the image the user installed.
 */
object WindowsGlyph {
    /** Fired on the caller of [prepare], or on the extractor thread, when a logo is ready. */
    var onReady: (() -> Unit)? = null

    private val gate = Any()
    private var key: String? = null
    /** Contours in bitmap-pixel space (y down). Published whole; drawn on the UI thread. */
    @Volatile private var glyph: Glyph? = null
    private var drawn: Glyph? = null
    private val drawnPath = Path()
    private val scaled = Path()
    private val bounds = RectF()
    private val matrix = android.graphics.Matrix()

    /** Starts a read of [image] unless this exact file was already handled. */
    fun prepare(ctx: Context, image: File) {
        if (!image.isFile || image.length() <= 0L) return
        val k = "${image.absolutePath}|${image.length()}|${image.lastModified()}"
        synchronized(gate) {
            if (key == k) return
            key = k
            glyph = null
        }
        readCache(ctx, k)?.let { ready ->
            publish(k, ready)
            return
        }
        val app = ctx.applicationContext
        thread(name = "windows-glyph", isDaemon = true) {
            val found = runCatching { extract(image) }.getOrNull() ?: return@thread
            writeCache(app, k, found)
            publish(k, found)
        }
    }

    private fun publish(k: String, found: Glyph) {
        synchronized(gate) { if (key == k) glyph = found }
        onReady?.invoke()
    }

    /**
     * The mark, centred on ([cx], [cy]) and fitted in a square of side [size]. Uses [paint] as
     * [glyph][SkinLayout] left it, so the backlight's colour and glow apply.
     */
    fun draw(c: Canvas, paint: Paint, cx: Float, cy: Float, size: Float) {
        val g = glyph
        if (g == null) {
            drawSquares(c, paint, cx, cy, size)
            return
        }
        if (drawn !== g) {
            drawn = g
            drawnPath.set(g.toPath())
        }
        drawnPath.computeBounds(bounds, true)
        val span = max(bounds.width(), bounds.height())
        if (span < 1f) {
            drawSquares(c, paint, cx, cy, size)
            return
        }
        val s = size / span
        matrix.setScale(s, s)
        matrix.postTranslate(cx - bounds.centerX() * s, cy - bounds.centerY() * s)
        drawnPath.transform(matrix, scaled)
        paint.style = Paint.Style.FILL
        c.drawPath(scaled, paint)
    }

    /** Four panes, the mark shown until a ROM flag has been traced. */
    private fun drawSquares(c: Canvas, paint: Paint, cx: Float, cy: Float, size: Float) {
        val gap = size * 0.14f
        val pane = (size - gap) / 2f
        val left = cx - size / 2f
        val top = cy - size / 2f
        paint.style = Paint.Style.FILL
        for (row in 0..1) {
            for (col in 0..1) {
                val x = left + col * (pane + gap)
                val y = top + row * (pane + gap)
                c.drawRect(x, y, x + pane, y + pane, paint)
            }
        }
    }

    private class Glyph(val contours: List<FloatArray>) {
        fun toPath(): Path {
            val p = Path()
            for (pts in contours) {
                if (pts.size < 6) continue
                p.moveTo(pts[0], pts[1])
                var i = 2
                while (i + 1 < pts.size) {
                    p.lineTo(pts[i], pts[i + 1])
                    i += 2
                }
                p.close()
            }
            return p
        }
    }

    // ---- cache: the traced contours, so the next launch does not read the ROM again ----

    private fun cacheFile(ctx: Context, k: String) =
        File(File(ctx.filesDir, "windows-glyph").apply { mkdirs() }, k.hashCode().toUInt().toString(16))

    private fun readCache(ctx: Context, k: String): Glyph? {
        val f = cacheFile(ctx, k)
        if (!f.isFile) return null
        return runCatching {
            DataInputStream(f.inputStream().buffered()).use { inn ->
                if (inn.readInt() != MAGIC || inn.readInt() != 1 || inn.readUTF() != k) return null
                val n = inn.readInt()
                if (n !in 1..8) return null
                val contours = ArrayList<FloatArray>(n)
                for (c in 0 until n) {
                    val m = inn.readInt()
                    if (m !in 3..20000) return null
                    val pts = FloatArray(m * 2)
                    for (i in pts.indices) pts[i] = inn.readFloat()
                    contours.add(pts)
                }
                Glyph(contours)
            }
        }.getOrNull()
    }

    private fun writeCache(ctx: Context, k: String, g: Glyph) {
        runCatching {
            val f = cacheFile(ctx, k)
            val tmp = File(f.path + ".part")
            DataOutputStream(tmp.outputStream().buffered()).use { out ->
                out.writeInt(MAGIC)
                out.writeInt(1)
                out.writeUTF(k)
                out.writeInt(g.contours.size)
                for (pts in g.contours) {
                    out.writeInt(pts.size / 2)
                    for (v in pts) out.writeFloat(v)
                }
            }
            if (!tmp.renameTo(f)) tmp.copyTo(f, overwrite = true)
            tmp.delete()
        }
    }

    // ---- ROM: IMGFS inside a B000FF or NB0 image, file wm_flag.bmp ----

    private val guid = byteArrayOf(
        0xF8.toByte(), 0xAC.toByte(), 0x2C, 0x9D.toByte(), 0xE3.toByte(), 0xD4.toByte(), 0x2B, 0x4D,
        0xBD.toByte(), 0x30, 0x91.toByte(), 0x6E, 0xD8.toByte(), 0x4F, 0x31, 0xDC.toByte(),
    )

    private fun extract(image: File): Glyph? {
        RandomAccessFile(image, "r").use { raf ->
            for (base in findGuids(raf)) {
                val bmp = readFlag(raf, base) ?: continue
                trace(bmp)?.let { return it }
            }
        }
        return null
    }

    private fun findGuids(raf: RandomAccessFile): List<Long> {
        val buf = ByteArray(1 shl 20)
        val hits = ArrayList<Long>()
        var pos = 0L
        var carry = ByteArray(0)
        while (pos < raf.length()) {
            val n = raf.read(buf)
            if (n <= 0) break
            val window = ByteArray(carry.size + n)
            carry.copyInto(window)
            buf.copyInto(window, carry.size, 0, n)
            var i = 0
            val last = window.size - guid.size
            while (i <= last) {
                if (window.regionMatches(i, guid)) hits.add(pos - carry.size + i)
                i++
            }
            carry = window.copyOfRange(max(0, window.size - (guid.size - 1)), window.size)
            pos += n
        }
        return hits
    }

    private fun ByteArray.regionMatches(at: Int, needle: ByteArray): Boolean {
        for (i in needle.indices) if (this[at + i] != needle[i]) return false
        return true
    }

    /** wm_flag.bmp bytes, or null if [base] is not the IMGFS that holds it. */
    private fun readFlag(raf: RandomAccessFile, base: Long): ByteArray? {
        val boot = readAt(raf, base, 0x30) ?: return null
        if (!boot.regionMatches(0, guid)) return null
        val bph = u32(boot, 0x1C)
        val sect = u32(boot, 0x24)
        if (bph != 0x34L || sect !in 0x200..0x10000) return null
        if (boot[0x2C] != 'X'.code.toByte() || boot[0x2D] != 'P'.code.toByte() || boot[0x2E] != 'R'.code.toByte()) return null
        for (fal in booleanArrayOf(true, false)) {
            if (u32at(raf, base, fal, sect) != SIG_HDR) continue
            readFlagFrom(raf, base, fal, sect, bph.toInt())?.let { return it }
        }
        return null
    }

    private fun readFlagFrom(raf: RandomAccessFile, base: Long, fal: Boolean, sect: Long, bph: Int): ByteArray? {
        val entries = HashMap<Long, ByteArray>()
        val files = ArrayList<ByteArray>()
        var off = sect
        var blocks = 0
        while (off != 0L && blocks < 4000) {
            blocks++
            val blk = readLogical(raf, base, fal, off, sect.toInt()) ?: return null
            if (u32(blk, 0) != SIG_HDR) return null
            var p = 8
            while (p + bph <= sect) {
                val e = blk.copyOfRange(p, p + bph)
                when (u32(e, 0)) {
                    T_NAME, T_SECT -> entries[off + p] = e
                    T_FILE, T_PLAIN -> files.add(e)
                }
                p += bph
            }
            off = u32(blk, 4)
        }
        if (files.size < 10) return null
        for (e in files) {
            val name = entryName(raf, base, fal, entries, e, 0x0C) ?: continue
            if (!name.equals("wm_flag.bmp", ignoreCase = true)) continue
            val size = u32(e, 0x18)
            val tbl = u32(e, 0x2C)
            val tsz = u32(e, 0x30)
            if (size == 0L || size > 1L shl 20 || tsz > 1L shl 20) return null
            val table = readLogical(raf, base, fal, tbl, tsz.toInt()) ?: return null
            val out = ByteArray(size.toInt())
            var filled = 0
            var t = 0
            while (t + 8 <= table.size && filled < out.size) {
                val cs = u16(table, t)
                val us = u16(table, t + 2)
                val at = u32(table, t + 4)
                t += 8
                if (cs == 0 && us == 0) continue
                val raw = readLogical(raf, base, fal, at, cs) ?: return null
                val part = if (cs == us) raw else xpr(raw, us) ?: return null
                val n = min(part.size, out.size - filled)
                part.copyInto(out, filled, 0, n)
                filled += n
            }
            if (filled != out.size || out.size < 54 || out[0] != 'B'.code.toByte() || out[1] != 'M'.code.toByte()) return null
            return out
        }
        return null
    }

    private fun entryName(raf: RandomAccessFile, base: Long, fal: Boolean, entries: Map<Long, ByteArray>, e: ByteArray, at: Int): String? {
        val cch = u16(e, at)
        if (cch == 0 || cch > 260) return null
        if (cch <= 4) return utf16(e, at + 4, cch * 2)
        var noff = u32(e, at + 8)
        val head = readLogical(raf, base, fal, noff, 4) ?: return null
        if (u32(head, 0) == T_NAME) noff += 4
        val raw = readLogical(raf, base, fal, noff, cch * 2) ?: return null
        return utf16(raw, 0, raw.size)
    }

    /** [MS-XCA] plain LZ77 (IMGFS "XPR"). Extended lengths share one byte, low nibble then high. */
    private fun xpr(src: ByteArray, usize: Int): ByteArray? {
        val out = ByteArray(usize)
        var o = 0
        var i = 0
        var flags = 0L
        var nflags = 0
        var half = -1
        while (o < usize) {
            if (nflags == 0) {
                if (i + 4 > src.size) return null
                flags = u32(src, i)
                i += 4
                nflags = 32
            }
            nflags--
            if ((flags and (1L shl nflags)) == 0L) {
                if (i >= src.size) return null
                out[o++] = src[i++]
                continue
            }
            if (i >= src.size) break
            if (i + 2 > src.size) return null
            val mb = u16(src, i)
            i += 2
            var length = mb and 7
            val disp = (mb shr 3) + 1
            if (length == 7) {
                length = if (half < 0) {
                    if (i >= src.size) return null
                    val b = src[i].toInt() and 0xFF
                    half = i
                    i++
                    b and 15
                } else {
                    val b = src[half].toInt() and 0xFF
                    half = -1
                    b shr 4
                }
                if (length == 15) {
                    if (i >= src.size) return null
                    length = src[i].toInt() and 0xFF
                    i++
                    if (length == 255) {
                        if (i + 2 > src.size) return null
                        length = u16(src, i)
                        i += 2
                        if (length == 0) {
                            if (i + 4 > src.size) return null
                            length = u32(src, i).toInt()
                            i += 4
                        }
                        if (length < 22) return null
                        length -= 22
                    }
                    length += 15
                }
                length += 7
            }
            length += 3
            if (disp <= 0 || disp > o) return null
            val from = o - disp
            for (k in 0 until length) {
                if (o >= usize) break
                out[o] = out[from + k]
                o++
            }
        }
        return if (o == usize) out else null
    }

    /** The flag's silhouette, smoothed, in pixel coordinates with y pointing down. */
    private fun trace(bmp: ByteArray): Glyph? {
        val mask = maskOf(bmp) ?: return null
        val h = mask.size
        val w = mask[0].size
        fun ink(x: Int, y: Int) = x in 0 until w && y in 0 until h && mask[y][x]
        val segs = ArrayList<Pair<Pt, Pt>>()
        for (y in 0 until h) {
            for (x in 0 until w) {
                if (!mask[y][x]) continue
                if (!ink(x, y - 1)) segs.add(Pt(x, y) to Pt(x + 1, y))
                if (!ink(x + 1, y)) segs.add(Pt(x + 1, y) to Pt(x + 1, y + 1))
                if (!ink(x, y + 1)) segs.add(Pt(x + 1, y + 1) to Pt(x, y + 1))
                if (!ink(x - 1, y)) segs.add(Pt(x, y + 1) to Pt(x, y))
            }
        }
        if (segs.isEmpty()) return null
        val next = HashMap<Pt, MutableList<Pt>>()
        for ((a, b) in segs) next.getOrPut(a) { ArrayList(1) }.add(b)
        val used = HashSet<Long>()
        fun id(a: Pt, b: Pt) = (a.x.toLong() shl 48) or (a.y.toLong() shl 32) or (b.x.toLong() shl 16) or b.y.toLong()
        val loops = ArrayList<FloatArray>()
        for ((a0, b0) in segs) {
            if (!used.add(id(a0, b0))) continue
            val loop = ArrayList<Pt>()
            loop.add(a0)
            var prev: Pt? = null
            var cur = a0
            var closed = false
            var firstStep = true
            var guard = 0
            while (guard++ <= segs.size) {
                val open = next[cur].orEmpty().filter { id(cur, it) !in used }
                val choices = if (firstStep) listOf(b0) else open
                if (choices.isEmpty()) break
                val d = if (choices.size == 1 || prev == null) choices[0] else {
                    val vx = cur.x - prev.x
                    val vy = cur.y - prev.y
                    choices.maxBy { atan2((vx * (it.y - cur.y) - vy * (it.x - cur.x)).toDouble(), (vx * (it.x - cur.x) + vy * (it.y - cur.y)).toDouble()) }
                }
                if (!firstStep) used.add(id(cur, d))
                firstStep = false
                if (d == a0 && loop.size > 2) {
                    closed = true
                    break
                }
                loop.add(d)
                prev = cur
                cur = d
            }
            if (closed && loop.size >= 3) loops.add(chaikin(loop))
        }
        if (loops.isEmpty()) return null
        return Glyph(loops)
    }

    /** Two smoothing passes. A closed ring; the first point is not repeated at the end. */
    private fun chaikin(loop: List<Pt>): FloatArray {
        var pts = loop
        repeat(2) {
            val nxt = ArrayList<Pt>(pts.size * 2)
            val m = pts.size
            for (i in 0 until m) {
                val p = pts[i]
                val q = pts[(i + 1) % m]
                nxt.add(Pt(p.x * 0.75f + q.x * 0.25f, p.y * 0.75f + q.y * 0.25f))
                nxt.add(Pt(p.x * 0.25f + q.x * 0.75f, p.y * 0.25f + q.y * 0.75f))
            }
            pts = nxt
        }
        val out = FloatArray(pts.size * 2)
        for (i in pts.indices) {
            out[i * 2] = pts[i].x
            out[i * 2 + 1] = pts[i].y
        }
        return out
    }

    private class Pt(val x: Float, val y: Float) {
        constructor(x: Int, y: Int) : this(x.toFloat(), y.toFloat())
        override fun equals(other: Any?) = other is Pt && other.x == x && other.y == y
        override fun hashCode() = x.hashCode() * 31 + y.hashCode()
    }

    /** Ink mask, row 0 at the top. The border's most common colour is the field behind the flag. */
    private fun maskOf(bmp: ByteArray): Array<BooleanArray>? {
        if (bmp.size < 54 || u16(bmp, 0) != 0x4D42) return null
        val pixOff = u32(bmp, 10).toInt()
        val w = u32(bmp, 18).toInt()
        var h = u32(bmp, 22).toInt()
        val bpp = u16(bmp, 28)
        val comp = u32(bmp, 30)
        if (w !in 8..256 || comp != 0L || bpp !in intArrayOf(8, 24, 32)) return null
        val topDown = h < 0
        if (topDown) h = -h
        if (h !in 8..256 || pixOff !in 54 until bmp.size) return null
        val stride = ((w * bpp + 31) / 32) * 4
        if (pixOff + stride * h > bmp.size) return null
        fun color(x: Int, fileRow: Int): Int {
            val o = pixOff + fileRow * stride + when (bpp) {
                8 -> x
                24 -> x * 3
                else -> x * 4
            }
            return when (bpp) {
                8 -> bmp[o].toInt() and 0xFF
                24 -> (bmp[o].toInt() and 0xFF) or ((bmp[o + 1].toInt() and 0xFF) shl 8) or ((bmp[o + 2].toInt() and 0xFF) shl 16)
                else -> (bmp[o].toInt() and 0xFF) or ((bmp[o + 1].toInt() and 0xFF) shl 8) or ((bmp[o + 2].toInt() and 0xFF) shl 16)
            }
        }
        fun fileRow(top: Int) = if (topDown) top else h - 1 - top
        val count = HashMap<Int, Int>()
        for (x in 0 until w) {
            count[color(x, fileRow(0))] = (count[color(x, fileRow(0))] ?: 0) + 1
            count[color(x, fileRow(h - 1))] = (count[color(x, fileRow(h - 1))] ?: 0) + 1
        }
        for (y in 0 until h) {
            count[color(0, fileRow(y))] = (count[color(0, fileRow(y))] ?: 0) + 1
            count[color(w - 1, fileRow(y))] = (count[color(w - 1, fileRow(y))] ?: 0) + 1
        }
        val bg = count.maxBy { it.value }.key
        val mask = Array(h) { BooleanArray(w) }
        var ink = 0
        for (y in 0 until h) {
            val row = fileRow(y)
            for (x in 0 until w) if (color(x, row) != bg) {
                mask[y][x] = true
                ink++
            }
        }
        if (ink < 24 || ink > w * h * 9 / 10) return null
        return mask
    }

    private fun readAt(raf: RandomAccessFile, off: Long, n: Int): ByteArray? {
        if (n <= 0 || off < 0 || off + n > raf.length()) return null
        val b = ByteArray(n)
        raf.seek(off)
        var g = 0
        while (g < n) {
            val r = raf.read(b, g, n - g)
            if (r <= 0) return null
            g += r
        }
        return b
    }

    private fun readLogical(raf: RandomAccessFile, base: Long, fal: Boolean, logical: Long, n: Int): ByteArray? {
        if (n <= 0) return ByteArray(0)
        if (!fal) return readAt(raf, base + logical, n)
        val out = ByteArray(n)
        var left = n
        var L = logical
        var at = 0
        while (left > 0) {
            val inBlock = (L % 0xF000L).toInt()
            val chunk = min(left, 0xF000 - inBlock)
            val part = readAt(raf, base + (L / 0xF000L) * 0x10000L + inBlock, chunk) ?: return null
            part.copyInto(out, at)
            L += chunk
            at += chunk
            left -= chunk
        }
        return out
    }

    private fun u32at(raf: RandomAccessFile, base: Long, fal: Boolean, logical: Long): Long {
        val b = readLogical(raf, base, fal, logical, 4) ?: return -1
        return u32(b, 0)
    }

    private fun u16(b: ByteArray, o: Int) = (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8)
    private fun u32(b: ByteArray, o: Int): Long =
        (b[o].toLong() and 0xFF) or ((b[o + 1].toLong() and 0xFF) shl 8) or
            ((b[o + 2].toLong() and 0xFF) shl 16) or ((b[o + 3].toLong() and 0xFF) shl 24)

    private fun utf16(b: ByteArray, o: Int, n: Int): String {
        val chars = CharArray(n / 2)
        var c = 0
        var i = o
        while (i + 1 < o + n && i + 1 < b.size) {
            val v = (b[i].toInt() and 0xFF) or ((b[i + 1].toInt() and 0xFF) shl 8)
            if (v == 0) break
            chars[c++] = v.toChar()
            i += 2
        }
        return String(chars, 0, c)
    }

    private const val MAGIC = 0x474C4657
    private const val SIG_HDR = 0x2F5314CEL
    private const val T_FILE = 0xFFFFFEFEL
    private const val T_PLAIN = 0xFFFFF6FEL
    private const val T_NAME = 0xFFFFFEFBL
    private const val T_SECT = 0xFFFFF6FDL
}
