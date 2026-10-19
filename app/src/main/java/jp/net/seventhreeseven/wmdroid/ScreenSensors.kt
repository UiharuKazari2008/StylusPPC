package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.BatteryManager
import android.os.Handler
import android.os.Looper

/**
 * What the realism filters react to (NativeBridge.setScreenSensors): where the viewer is, from how
 * the phone is tilted away from the way it is usually held; the room's light; the phone's
 * temperature; and its screen brightness. Sensors listen only while an effect needs them and the
 * app is on screen; temperature and brightness are polled every two seconds.
 */
class ScreenSensors(private val ctx: Context, private val push: (Float, Float, Float, Float, Float) -> Unit) : SensorEventListener {
    private val sm = ctx.getSystemService(SensorManager::class.java)
    private val handler = Handler(Looper.getMainLooper())
    private var active = false
    private var tiltOn = false
    private var lightOn = false
    // The resting pose: the phone's angles as usually held, followed slowly so that a new way of
    // holding it becomes "straight on" after a few seconds. The viewer's head is assumed to stay
    // put, so turning the phone away from that pose turns the screen away from the eye.
    private var restX = 0f
    private var restY = 0f
    private var haveRest = false
    private var angX = 0f
    private var angY = 0f
    private var lastSampleNs = 0L
    private var lux = 0f
    private var tempC = 25f
    private var brightness = 1f
    private var lastPushNs = 0L
    private var pushedX = 9f
    private var pushedY = 9f
    private var pushedLux = -1f

    private val poll = object : Runnable {
        override fun run() {
            readSlow()
            send(force = true)
            if (active) handler.postDelayed(this, 2000)
        }
    }

    /** [on]: any realism effect is on and the app is in front; [tilt] / [light]: sensors needed. */
    fun update(on: Boolean, tilt: Boolean, light: Boolean) {
        val wantTilt = on && tilt
        val wantLight = on && light
        if (wantTilt != tiltOn) {
            tiltOn = wantTilt
            val g = sm?.getDefaultSensor(Sensor.TYPE_GRAVITY) ?: sm?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)
            if (wantTilt && g != null) sm?.registerListener(this, g, SensorManager.SENSOR_DELAY_GAME)
            else if (g != null) sm?.unregisterListener(this, g)
            if (!wantTilt) { angX = 0f; angY = 0f; haveRest = false }
        }
        if (wantLight != lightOn) {
            lightOn = wantLight
            val l = sm?.getDefaultSensor(Sensor.TYPE_LIGHT)
            if (wantLight && l != null) sm?.registerListener(this, l, SensorManager.SENSOR_DELAY_NORMAL)
            else if (l != null) sm?.unregisterListener(this, l)
            if (!wantLight) lux = 0f
        }
        if (on != active) {
            active = on
            handler.removeCallbacks(poll)
            if (on) handler.post(poll)
        }
    }

    fun stop() = update(false, false, false)

    private fun readSlow() {
        val battery = ctx.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        if (battery != null) tempC = battery.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 250) / 10f
        val raw = android.provider.Settings.System.getInt(ctx.contentResolver,
            android.provider.Settings.System.SCREEN_BRIGHTNESS, 128).coerceIn(0, 255)
        brightness = Math.sqrt(raw / 255.0).toFloat()  // perceived, like the backlight sync
    }

    override fun onSensorChanged(e: SensorEvent) {
        when (e.sensor.type) {
            Sensor.TYPE_LIGHT -> lux = e.values[0]
            else -> {
                // Angles of the "up" vector in the device's Y-Z and X-Z planes: the top edge tipping
                // away or towards the eye, the right edge tipping away or towards it.
                val (gx, gy, gz) = Triple(e.values[0], e.values[1], e.values[2])
                val ay = Math.atan2(gy.toDouble(), gz.toDouble()).toFloat()
                val ax = Math.atan2(gx.toDouble(), gz.toDouble()).toFloat()
                val now = e.timestamp
                val dt = if (lastSampleNs == 0L) 0.02f else ((now - lastSampleNs) / 1e9f).coerceIn(0.001f, 0.2f)
                lastSampleNs = now
                if (!haveRest) { restX = ax; restY = ay; angX = ax; angY = ay; haveRest = true }
                val k = 1f - Math.exp((-dt / 0.05f).toDouble()).toFloat()  // raw accelerometer jitter
                angX += (ax - angX) * k
                angY += (ay - angY) * k
                val r = 1f - Math.exp((-dt / 4f).toDouble()).toFloat()
                restX += (angX - restX) * r
                restY += (angY - restY) * r
            }
        }
        send(force = false)
    }

    override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}

    /** About 35 degrees away from the resting pose is the full effect. */
    private fun send(force: Boolean) {
        val tx = if (tiltOn) ((angX - restX) / 0.6f).coerceIn(-1.5f, 1.5f) else 0f
        val ty = if (tiltOn) ((angY - restY) / 0.6f).coerceIn(-1.5f, 1.5f) else 0f
        val now = System.nanoTime()
        val moved = Math.abs(tx - pushedX) > 0.005f || Math.abs(ty - pushedY) > 0.005f ||
            Math.abs(lux - pushedLux) > 0.1f * maxOf(pushedLux, 50f)
        if (!force && (!moved || now - lastPushNs < 33_000_000L)) return
        lastPushNs = now
        pushedX = tx
        pushedY = ty
        pushedLux = lux
        push(tx, ty, lux, tempC, brightness)
    }
}
