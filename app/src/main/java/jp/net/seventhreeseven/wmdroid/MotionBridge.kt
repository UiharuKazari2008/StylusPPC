package jp.net.seventhreeseven.wmdroid

import android.app.Activity
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.Looper

/**
 * The phone's accelerometer for Windows Mobile's G-sensor apps (HTC's Teeter and friends). The guest
 * driver SEN0: (guest/Sensor/htcsensor.c) reads the core's motion device (core/src/board/motion.h),
 * which reports [NativeBridge.motionWanted] while a guest app uses it; only then does the sensor run,
 * at game rate. Samples are turned into the guest panel's portrait frame: the window's own rotation
 * plus the panel's quarter turn in the foldable layout ([panelRotation]).
 *
 * [onUse] tells the activity when a guest app starts or stops reading (from the watch loop only, not
 * from [stop]), so Windows Mobile's automatic screen rotation can stay out of the way of a tilt game;
 * [inUse] is the current state.
 */
class MotionBridge(
    private val activity: Activity,
    private val handle: () -> Long,
    private val onUse: (Boolean) -> Unit,
) : SensorEventListener {
    /** Clockwise degrees the guest panel is drawn turned inside the window (90 in the fold layout). */
    @Volatile var panelRotation = 0

    private val sensors = activity.getSystemService(SensorManager::class.java)
    private val main = Handler(Looper.getMainLooper())
    private var running = false
    private var listening = false

    /** A guest app reads the G-sensor and the phone's sensor feeds it. */
    val inUse get() = listening

    /** Watches the guest while the emulator is on screen. */
    fun start() {
        if (running) return
        running = true
        main.post(poll)
    }

    fun stop() {
        running = false
        main.removeCallbacks(poll)
        listen(false, notify = false)
    }

    private val poll = object : Runnable {
        override fun run() {
            if (!running) return
            val h = handle()
            listen(h != 0L && NativeBridge.motionWanted(h), notify = true)
            main.postDelayed(this, POLL_MS)
        }
    }

    private fun listen(on: Boolean, notify: Boolean) {
        if (on == listening) return
        if (on) {
            val accel = sensors?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) ?: return
            listening = sensors.registerListener(this, accel, SensorManager.SENSOR_DELAY_GAME)
            if (!listening) return
        } else {
            sensors?.unregisterListener(this)
            listening = false
            val h = handle()
            if (h != 0L) NativeBridge.motionClear(h)
        }
        if (notify) onUse(listening)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    override fun onSensorChanged(e: SensorEvent) {
        val h = handle()
        if (h == 0L) return
        val ax = e.values[0]
        val ay = e.values[1]
        // Device frame -> window -> guest panel: each clockwise quarter turn maps (x, y) to (-y, x).
        when (((displayDegrees() + panelRotation) % 360) / 90) {
            1 -> NativeBridge.motionAccel(h, -ay, ax, e.values[2])
            2 -> NativeBridge.motionAccel(h, -ax, -ay, e.values[2])
            3 -> NativeBridge.motionAccel(h, ay, -ax, e.values[2])
            else -> NativeBridge.motionAccel(h, ax, ay, e.values[2])
        }
    }

    private fun displayDegrees(): Int {
        val r = (if (android.os.Build.VERSION.SDK_INT >= 30) activity.display?.rotation else null)
            ?: @Suppress("DEPRECATION") activity.windowManager.defaultDisplay.rotation
        return r * 90   // Surface.ROTATION_0..270
    }

    companion object {
        private const val POLL_MS = 500L
    }
}
