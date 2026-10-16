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
 * The phone's GPS as an NMEA receiver on the guest's COM1, where Windows Mobile's GPS
 * Intermediate Driver reads it. Location is only requested while the guest has the port open.
 * Sentences are built from Android's fix and satellite status ($GP talker, GGA/GSA/GSV/RMC once a
 * second), since GPSID only understands GPS-talker sentences.
 */
class GpsBridge(private val ctx: Context, private val handle: () -> Long) {
    private val lm = ctx.getSystemService(LocationManager::class.java)
    private val ui = Handler(Looper.getMainLooper())
    private var running = false
    private var fix: Location? = null
    private var sats: List<Sat> = emptyList()

    private data class Sat(val prn: Int, val elev: Int, val azim: Int, val snr: Int, val used: Boolean)

    fun hasPermission() = ctx.checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED

    private val listener = LocationListener { fix = it }

    private val gnss = object : GnssStatus.Callback() {
        override fun onSatelliteStatusChanged(s: GnssStatus) {
            val list = ArrayList<Sat>()
            for (i in 0 until s.satelliteCount) {
                // GPS keeps its PRN; other systems go to 33-99 so old parsers accept them.
                val prn = when (s.getConstellationType(i)) {
                    GnssStatus.CONSTELLATION_GPS -> s.getSvid(i)
                    GnssStatus.CONSTELLATION_SBAS -> s.getSvid(i).let { if (it in 33..64) it else 0 }
                    GnssStatus.CONSTELLATION_GLONASS -> 64 + s.getSvid(i).coerceIn(1, 24)
                    else -> 0
                }
                if (prn !in 1..99) continue
                list += Sat(prn, s.getElevationDegrees(i).toInt(), s.getAzimuthDegrees(i).toInt(),
                    s.getCn0DbHz(i).toInt().coerceIn(0, 99), s.usedInFix(i))
            }
            sats = list.sortedByDescending { it.snr }.take(16)
        }
    }

    private val tick = object : Runnable {
        override fun run() {
            val h = handle()
            if (running && h != 0L) NativeBridge.serialWrite(h, sentences().toByteArray(Charsets.US_ASCII))
            if (running) ui.postDelayed(this, 1000)
        }
    }

    /** Starts or stops the receiver to follow whether the guest has the port open. */
    fun setActive(on: Boolean) {
        if (on == running) return
        android.util.Log.i("WMDroid", "gps: guest port ${if (on) "opened" else "closed"}")
        if (on) {
            if (!hasPermission()) return
            running = true
            runCatching {
                lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 0f, listener, Looper.getMainLooper())
                lm.registerGnssStatusCallback(gnss, ui)
            }
            fix = runCatching { lm.getLastKnownLocation(LocationManager.GPS_PROVIDER) }.getOrNull()
            ui.post(tick)
        } else {
            running = false
            ui.removeCallbacks(tick)
            runCatching { lm.removeUpdates(listener) }
            runCatching { lm.unregisterGnssStatusCallback(gnss) }
            fix = null
            sats = emptyList()
        }
    }

    private fun sentences(): String {
        val f = fix?.takeIf { System.currentTimeMillis() - it.time < 10_000 }
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
