package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.location.GnssStatus
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import java.util.Calendar
import java.util.Locale
import java.util.TimeZone

/**
 * The phone's GPS as an NMEA receiver. Windows Mobile 6.5.3 reads it on COM1. On 6 / 6.1 / 6.1.4
 * and the original 6.5, COM1 is the radio, so the same sentences go to COM4 (UART1), which GPS
 * programs open directly.
 * The radio is switched on only for a short while after the port opens. After that, sentences
 * keep going out from the last fix, so a port GPSID leaves open does not hold location on.
 * Sentences are $GP GGA/GSA/GSV/RMC once a second; GPSID only understands the GPS talker.
 */
class GpsBridge(private val ctx: Context, private val handle: () -> Long) {
    private val lm = ctx.getSystemService(LocationManager::class.java)
    private val ui = Handler(Looper.getMainLooper())
    private var running = false
    private var listening = false
    private var fix: Location? = null
    private var sats: List<Sat> = emptyList()

    private data class Sat(val prn: Int, val elev: Int, val azim: Int, val snr: Int, val used: Boolean)

    fun hasPermission() = ctx.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    private val listener = LocationListener { fix = it }

    private var loggedSats = -1

    private val gnss = object : GnssStatus.Callback() {
        override fun onSatelliteStatusChanged(s: GnssStatus) {
            // GPS keeps its PRN. Everything else is packed into 33-99: GPSID only accepts those,
            // and dropping Galileo / BeiDou left the sky view empty on phones that barely use GPS.
            class Raw(val elev: Int, val azim: Int, val snr: Int, val used: Boolean, val gps: Int)
            val raw = ArrayList<Raw>()
            for (i in 0 until s.satelliteCount) {
                val id = s.getSvid(i)
                val gps = if (s.getConstellationType(i) == GnssStatus.CONSTELLATION_GPS && id in 1..32) id else 0
                raw += Raw(s.getElevationDegrees(i).toInt(), s.getAzimuthDegrees(i).toInt(),
                    s.getCn0DbHz(i).toInt().coerceIn(0, 99), s.usedInFix(i), gps)
            }
            raw.sortByDescending { it.snr }
            val list = ArrayList<Sat>()
            var next = 33
            for (r in raw) {
                val prn = if (r.gps != 0) r.gps else if (next <= 99) next++ else continue
                if (prn !in 1..99) continue
                list += Sat(prn, r.elev, r.azim, r.snr, r.used)
                if (list.size == 16) break
            }
            if (list.size != loggedSats) {
                loggedSats = list.size
                android.util.Log.i("WMDroid", "gps: ${list.size} satellites in view")
            }
            sats = list
        }
    }

    private val tick = object : Runnable {
        override fun run() {
            val h = handle()
            if (h != 0L) NativeBridge.serialWrite(h, sentences().toByteArray(Charsets.US_ASCII))
            if (running) ui.postDelayed(this, 1000)
        }
    }

    /** Starts or stops the receiver to follow whether Windows Mobile has the GPS port open. */
    fun setActive(on: Boolean) {
        if (on == running) return
        android.util.Log.i("WMDroid", "gps: guest port ${if (on) "opened" else "closed"}")
        if (on) {
            if (!hasPermission()) return
            running = true
            fix = lastKnown()
            ui.post(tick)
            // One short listen when something opens the port, then the radio goes off.
            acquire()
        } else {
            running = false
            ui.removeCallbacks(tick)
            ui.removeCallbacks(stopListen)
            stopListening()
            fix = null
            sats = emptyList()
            loggedSats = -1
        }
    }

    /** Newest fix any provider already has. Does not turn the radio on. */
    private fun lastKnown(): Location? {
        val providers = buildList {
            add(LocationManager.GPS_PROVIDER)
            add(LocationManager.NETWORK_PROVIDER)
            if (Build.VERSION.SDK_INT >= 31) add(LocationManager.FUSED_PROVIDER)
        }
        return providers.mapNotNull { p -> runCatching { lm.getLastKnownLocation(p) }.getOrNull() }.maxByOrNull { it.time }
    }

    private fun acquire() {
        if (listening || !hasPermission()) return
        if (!runCatching { lm.isProviderEnabled(LocationManager.GPS_PROVIDER) }.getOrDefault(false)) return
        listening = true
        runCatching { lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 0f, listener, Looper.getMainLooper()) }
            .onFailure { android.util.Log.w("WMDroid", "gps: updates refused", it); listening = false; return }
        runCatching { lm.registerGnssStatusCallback(gnss, ui) }
            .onFailure { android.util.Log.w("WMDroid", "gps: satellite status refused", it) }
        ui.postDelayed(stopListen, 20_000L)
    }

    private val stopListen = Runnable { stopListening() }

    private fun stopListening() {
        if (!listening) return
        listening = false
        runCatching { lm.removeUpdates(listener) }
        runCatching { lm.unregisterGnssStatusCallback(gnss) }
    }

    private fun sentences(): String {
        val f = fix?.takeIf { System.currentTimeMillis() - it.time < 30 * 60_000 }
        val cal = Calendar.getInstance(TimeZone.getTimeZone("UTC")).apply { timeInMillis = f?.time ?: System.currentTimeMillis() }
        val time = String.format(Locale.US, "%02d%02d%02d.%02d", cal.get(Calendar.HOUR_OF_DAY), cal.get(Calendar.MINUTE),
            cal.get(Calendar.SECOND), cal.get(Calendar.MILLISECOND) / 10)
        val date = String.format(Locale.US, "%02d%02d%02d", cal.get(Calendar.DAY_OF_MONTH), cal.get(Calendar.MONTH) + 1,
            cal.get(Calendar.YEAR) % 100)
        val used = sats.filter { it.used }.map { it.prn }.take(12)
        val hdop = f?.let { (it.accuracy / 5f).coerceIn(0.5f, 50f) } ?: 99.9f
        val vdop = if (f != null && Build.VERSION.SDK_INT >= 26 && f.hasVerticalAccuracy())
            (f.verticalAccuracyMeters / 5f).coerceIn(0.5f, 50f) else hdop * 1.5f
        val pdop = Math.sqrt((hdop * hdop + vdop * vdop).toDouble()).toFloat()
        val lat = f?.let { coord(it.latitude, true) } ?: ",,"
        val lon = f?.let { coord(it.longitude, false) } ?: ",,"
        val sb = StringBuilder()
        // GGA: fix quality, satellites used, HDOP, altitude above sea level, geoid separation.
        val alt = f?.let {
            if (Build.VERSION.SDK_INT >= 34 && it.hasMslAltitude()) it.mslAltitudeMeters else it.altitude
        }
        sb.append(nmea("GPGGA,$time,$lat,$lon,${if (f != null) 1 else 0},${String.format(Locale.US, "%02d", used.size)}," +
            "${fmt1(hdop)},${alt?.let { fmt1(it.toFloat()) + ",M" } ?: ",M"},0.0,M,,"))
        // GSA: automatic mode, 3D/2D/no fix, used PRNs, DOPs.
        val mode = if (f == null) 1 else if (f.hasAltitude()) 3 else 2
        val prns = (used.map { it.toString() } + List(12 - used.size) { "" }).joinToString(",")
        sb.append(nmea("GPGSA,A,$mode,$prns,${fmt1(pdop)},${fmt1(hdop)},${fmt1(vdop)}"))
        // GSV: satellites in view, four per sentence.
        val groups = sats.chunked(4).ifEmpty { listOf(emptyList()) }
        groups.forEachIndexed { i, g ->
            val body = g.joinToString("") { ",${String.format(Locale.US, "%02d", it.prn)},${it.elev},${it.azim},${it.snr}" }
            sb.append(nmea("GPGSV,${groups.size},${i + 1},${String.format(Locale.US, "%02d", sats.size)}$body"))
        }
        // RMC: status, position, speed in knots, course, date.
        val speed = f?.takeIf { it.hasSpeed() }?.let { fmt1(it.speed * 1.943844f) } ?: ""
        val course = f?.takeIf { it.hasBearing() }?.let { fmt1(it.bearing) } ?: ""
        sb.append(nmea("GPRMC,$time,${if (f != null) "A" else "V"},$lat,$lon,$speed,$course,$date,,,${if (f != null) "A" else "N"}"))
        return sb.toString()
    }

    private fun coord(v: Double, lat: Boolean): String {
        val a = Math.abs(v)
        val deg = a.toInt()
        val min = (a - deg) * 60
        val hemi = if (lat) (if (v >= 0) "N" else "S") else (if (v >= 0) "E" else "W")
        return if (lat) String.format(Locale.US, "%02d%07.4f,%s", deg, min, hemi)
        else String.format(Locale.US, "%03d%07.4f,%s", deg, min, hemi)
    }

    private fun fmt1(v: Float) = String.format(Locale.US, "%.1f", v)

    private fun nmea(body: String): String {
        var sum = 0
        for (c in body) sum = sum xor c.code
        return "$" + body + "*" + String.format(Locale.US, "%02X", sum) + "\r\n"
    }
}
