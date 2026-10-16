package jp.net.seventhreeseven.wmdroid

import java.util.Calendar
import java.util.TimeZone

/**
 * SMS PDUs (3GPP TS 23.040 / 23.038) for the virtual modem: SMS-DELIVER for messages coming into
 * Windows Mobile, SMS-SUBMIT for the ones it sends. Both carry a leading SMSC field (PDU mode, 27.005).
 */
object SmsPdu {
    /** A message Windows Mobile sent: one part of it, with its concatenation reference if split. */
    class Submit(val to: String, val text: String, val ref: Int, val parts: Int, val part: Int)

    // GSM 7-bit default alphabet (23.038 6.2.1) and its extension table (after ESC 0x1B).
    private const val BASIC = "@£\$¥èéùìòÇ\nØø\rÅåΔ_ΦΓΛΩΠΨΣΘΞ\u001BÆæßÉ !\"#¤%&'()*+,-./0123456789:;<=>?" +
        "¡ABCDEFGHIJKLMNOPQRSTUVWXYZÄÖÑÜ§¿abcdefghijklmnopqrstuvwxyzäöñüà"
    private val EXT = mapOf('\u000C' to 0x0A, '^' to 0x14, '{' to 0x28, '}' to 0x29, '\\' to 0x2F, '[' to 0x3C,
        '~' to 0x3D, ']' to 0x3E, '|' to 0x40, '€' to 0x65)
    private val EXT_BACK = EXT.entries.associate { (k, v) -> v to k }

    private fun septets(text: String): List<Int>? {
        val out = ArrayList<Int>()
        for (c in text) {
            val i = BASIC.indexOf(c)
            if (i >= 0 && c != '\u001B') out += i
            else out += listOf(0x1B, EXT[c] ?: return null)
        }
        return out
    }

    // ---- SMS-DELIVER (into Windows Mobile) ----

    /** The PDUs for one incoming message: one, or several with a concatenation header. */
    fun deliver(from: String, text: String, timeMs: Long, ref: Int): List<String> {
        val gsm = septets(text)
        val chunks: List<Pair<Boolean, List<Int>>> = if (gsm != null) {
            if (gsm.size <= 160) listOf(true to gsm) else splitSeptets(gsm, 153).map { true to it }
        } else {
            val units = text.map { it.code }
            if (units.size <= 70) listOf(false to units) else units.chunked(67).map { false to it }
        }
        val many = chunks.size > 1
        return chunks.mapIndexed { i, (seven, data) ->
            val sb = StringBuilder("00")  // no SMSC
            sb.append(hex(if (many) 0x44 else 0x04))  // SMS-DELIVER, no more messages, UDHI when split
            sb.append(address(from))
            sb.append("00")  // PID
            sb.append(if (seven) "00" else "08")  // DCS: GSM 7-bit / UCS2
            sb.append(timestamp(timeMs))
            val udh = if (many) intArrayOf(0x05, 0x00, 0x03, ref and 0xFF, chunks.size, i + 1) else IntArray(0)
            if (seven) {
                // The header takes whole septets: fill bits up to the next septet boundary.
                val headerSeptets = (udh.size * 8 + 6) / 7
                sb.append(hex(headerSeptets + data.size))
                sb.append(pack7(udh, data))
            } else {
                val bytes = ArrayList<Int>()
                udh.forEach { bytes += it }
                data.forEach { bytes += it shr 8; bytes += it and 0xFF }
                sb.append(hex(bytes.size))
                bytes.forEach { sb.append(hex(it)) }
            }
            sb.toString()
        }
    }

    /** Splits septets into parts, never between an escape and its character. */
    private fun splitSeptets(s: List<Int>, max: Int): List<List<Int>> {
        val out = ArrayList<List<Int>>()
        var i = 0
        while (i < s.size) {
            var end = minOf(i + max, s.size)
            if (end < s.size && s[end - 1] == 0x1B) end--
            out += s.subList(i, end)
            i = end
        }
        return out
    }

    private fun pack7(udh: IntArray, septets: List<Int>): String {
        val bytes = ArrayList<Int>()
        udh.forEach { bytes += it }
        val fill = (7 - (udh.size * 8) % 7) % 7
        var acc = 0L
        var bits = 0
        // Fill bits first (zeros), then the septets, low bits first.
        bits += fill
        for (s in septets) {
            acc = acc or (s.toLong() shl bits)
            bits += 7
            while (bits >= 8) { bytes += (acc and 0xFF).toInt(); acc = acc shr 8; bits -= 8 }
        }
        if (bits > 0) bytes += (acc and 0xFF).toInt()
        return bytes.joinToString("") { hex(it) }
    }

    private fun address(a: String): String {
        val digits = a.filter { it.isDigit() || it == '*' || it == '#' }
        if (a.any { it.isLetter() } || digits.isEmpty()) {
            // Alphanumeric sender ("Google"): packed GSM 7-bit, length in semi-octets.
            val s = septets(a.take(11)) ?: septets(a.filter { it.code < 128 }.take(11)) ?: emptyList()
            val packed = pack7(IntArray(0), s)
            return hex(packed.length) + "D0" + packed
        }
        val type = if (a.trim().startsWith("+")) "91" else "81"
        return hex(digits.length) + type + semi(digits)
    }

    private fun semi(d: String): String {
        val s = if (d.length % 2 == 1) d + "F" else d
        val sb = StringBuilder()
        for (i in s.indices step 2) sb.append(s[i + 1]).append(s[i])
        return sb.toString().replace('*', 'A').replace('#', 'B')
    }

    private fun timestamp(ms: Long): String {
        val tz = TimeZone.getDefault()
        val c = Calendar.getInstance(tz).apply { timeInMillis = ms }
        fun two(v: Int) = "%02d".format(v % 100).reversed()
        val q = tz.getOffset(ms) / (15 * 60 * 1000)
        val tzs = "%02d".format(kotlin.math.abs(q)).reversed()
        val tzField = if (q < 0) tzs.substring(0, 1) + (tzs[1].digitToInt() or 8).toString(16) else tzs
        return two(c.get(Calendar.YEAR)) + two(c.get(Calendar.MONTH) + 1) + two(c.get(Calendar.DAY_OF_MONTH)) +
            two(c.get(Calendar.HOUR_OF_DAY)) + two(c.get(Calendar.MINUTE)) + two(c.get(Calendar.SECOND)) + tzField.uppercase()
    }

    // ---- SMS-SUBMIT (from Windows Mobile) ----

    fun submit(pduHex: String): Submit? = runCatching {
        val b = pduHex.chunked(2).map { it.toInt(16) }
        var p = 1 + b[0]  // skip the SMSC field
        val first = b[p++]
        if (first and 0x03 != 0x01) return null  // not SMS-SUBMIT
        val vpf = (first shr 3) and 0x03
        val udhi = first and 0x40 != 0
        p++  // MR
        val len = b[p++]
        val type = b[p++]
        val octets = (len + 1) / 2
        val raw = StringBuilder()
        for (i in 0 until octets) {
            val o = b[p + i]
            raw.append(digit(o and 0x0F)); raw.append(digit(o shr 4))
        }
        p += octets
        var to = raw.toString().take(len).replace("F", "")
        if (type and 0x70 == 0x10) to = "+$to"
        p++  // PID
        val dcs = b[p++]
        p += when (vpf) { 2 -> 1; 1, 3 -> 7; else -> 0 }
        val udl = b[p++]
        val ud = b.subList(p, b.size)
        val alphabet = if (dcs and 0xC0 == 0) (dcs shr 2) and 0x03 else if (dcs and 0xF0 == 0xF0) (dcs shr 2) and 0x01 else 0
        var headerLen = 0
        var ref = 0; var parts = 1; var part = 1
        if (udhi) {
            headerLen = ud[0] + 1
            var i = 1
            while (i < headerLen) {
                val iei = ud[i]; val l = ud[i + 1]
                if (iei == 0x00 && l == 3) { ref = ud[i + 2]; parts = ud[i + 3]; part = ud[i + 4] }
                if (iei == 0x08 && l == 4) { ref = (ud[i + 2] shl 8) or ud[i + 3]; parts = ud[i + 4]; part = ud[i + 5] }
                i += 2 + l
            }
        }
        val text = when (alphabet) {
            2 -> {
                val sb = StringBuilder()
                var i = headerLen
                while (i + 1 < minOf(udl, ud.size)) { sb.append(((ud[i] shl 8) or ud[i + 1]).toChar()); i += 2 }
                sb.toString()
            }
            1 -> ud.subList(headerLen, minOf(udl, ud.size)).map { it.toChar() }.joinToString("")
            else -> {
                val skip = if (udhi) (headerLen * 8 + 6) / 7 else 0
                unpack7(ud, udl).drop(skip).let { gsmText(it) }
            }
        }
        Submit(to, text, ref, parts, part)
    }.getOrNull()

    private fun digit(n: Int) = when (n) { 10 -> "*"; 11 -> "#"; 15 -> "F"; else -> n.toString() }

    private fun unpack7(ud: List<Int>, count: Int): List<Int> {
        val out = ArrayList<Int>()
        var acc = 0L
        var bits = 0
        var i = 0
        while (out.size < count) {
            if (bits < 7) {
                if (i >= ud.size) break
                acc = acc or (ud[i++].toLong() shl bits); bits += 8
            }
            out += (acc and 0x7F).toInt(); acc = acc shr 7; bits -= 7
        }
        return out
    }

    private fun gsmText(s: List<Int>): String {
        val sb = StringBuilder()
        var esc = false
        for (c in s) {
            if (esc) { sb.append(EXT_BACK[c] ?: ' '); esc = false }
            else if (c == 0x1B) esc = true
            else sb.append(BASIC[c])
        }
        return sb.toString()
    }

    private fun hex(v: Int) = "%02X".format(v and 0xFF)
}
