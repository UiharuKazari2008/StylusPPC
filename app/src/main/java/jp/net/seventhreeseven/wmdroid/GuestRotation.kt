package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.SystemClock
import android.provider.Settings
import kotlin.math.abs
import kotlin.math.atan2
import kotlin.math.sqrt

/**
 * The app stays portrait; the phone's physical orientation is passed to Windows Mobile instead,
 * which turns its own display. Reports the guest rotation in degrees counter-clockwise
 * (0, 90, 270; upside down is ignored like Android does), only while Android's auto-rotate is on.
 *
 * Deliberately sluggish, since every guest rotation redraws the whole screen: the phone must be
 * held upright enough (not lying flat), well inside the new orientation, and steady there for
 * [HOLD_MS] before it counts. Uses the accelerometer at its slowest rate, low-pass filtered.
 */
class GuestRotation(
    private val ctx: Context,
    /** How long a new orientation must hold, ms (setting). */
    private val holdMs: () -> Int = { HOLD_MS.toInt() },
    /** 0 firm turn .. 100 slight tilt (setting). */
    private val sensitivity: () -> Int = { 50 },
    private val send: (Int) -> Unit,
) : SensorEventListener {
    private val sensors = ctx.getSystemService(Context.SENSOR_SERVICE) as SensorManager
    private var running = false
    private var current = 0
    private var candidate = 0
    private var candidateSince = 0L
    private val g = FloatArray(3)
    private var primed = false

    private fun autoRotate() =
        Settings.System.getInt(ctx.contentResolver, Settings.System.ACCELEROMETER_ROTATION, 0) == 1

    fun start() {
        if (running) return
        val accel = sensors.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) ?: return
        running = true
        primed = false
        sensors.registerListener(this, accel, SensorManager.SENSOR_DELAY_NORMAL)
    }

    fun stop() {
        if (!running) return
        running = false
        sensors.unregisterListener(this)
    }

    /** The guest helper restarted: tell it again. */
    fun resend() {
        if (current != 0) send(current)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    override fun onSensorChanged(e: SensorEvent) {
        if (!primed) { e.values.copyInto(g, 0, 0, 3); primed = true }
        for (i in 0..2) g[i] += (e.values[i] - g[i]) * FILTER
        val now = SystemClock.uptimeMillis()
        val want = if (autoRotate()) quadrant() ?: return else 0
        if (want != candidate) {
            candidate = want
            candidateSince = now
            return
        }
        if (candidate != current && now - candidateSince >= holdMs()) {
            current = candidate
            send(current)
        }
    }

    /** The orientation the phone is clearly in, or null while flat, moving or between two. */
    private fun quadrant(): Int? {
        val (x, y, z) = g
        val mag = sqrt(x * x + y * y + z * z)
        if (abs(mag - SensorManager.GRAVITY_EARTH) > 2.5f) return null    // being moved about
        val planar = sqrt(x * x + y * y)
        if (planar < mag * MIN_TILT) return null                          // lying (nearly) flat
        // Same convention as OrientationEventListener: 90 = left side up, 270 = right side up.
        var deg = Math.toDegrees(atan2(y.toDouble(), -x.toDouble())).toInt()
        deg = ((90 - deg) % 360 + 360) % 360
        return when {
            near(deg, 0) -> 0
            near(deg, 90) -> 90
            near(deg, 270) -> 270
            near(deg, 180) -> current       // upside down: stay as we are
            else -> null                     // between orientations
        }
    }

    private fun near(deg: Int, target: Int): Boolean {
        val d = abs(((deg - target) % 360 + 540) % 360 - 180)
        // Leaving the current orientation takes a firmer turn than staying in it; sensitivity sets
        // how firm (35 degrees from the target at 0, 15 at 100).
        val enter = 35 - sensitivity().coerceIn(0, 100) / 5
        return d <= if (target == current) enter + 18 else enter
    }

    companion object {
        private const val HOLD_MS = 900L
        private const val FILTER = 0.25f
        private const val MIN_TILT = 0.5f    // sin(30°): at least ~30° up from flat
    }
}
