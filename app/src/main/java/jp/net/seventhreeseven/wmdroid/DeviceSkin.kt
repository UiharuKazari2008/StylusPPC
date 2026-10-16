package jp.net.seventhreeseven.wmdroid

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.Surface
import android.view.RoundedCorner
import android.view.View
import android.view.WindowInsets
import android.widget.FrameLayout
import kotlin.random.Random

/**
 * Lays the guest screen out edge to edge and roughly centred: a top bar with the carrier's name,
 * the display, a row of four thin Vista-style glass keys (Talk, soft key 1, soft key 2, End) and
 * optionally a D-pad row, all on a backdrop. Tilting the phone moves a highlight across the keys.
 */
class SkinLayout(ctx: Context, private val screen: View, private val onKey: (vk: Int, down: Boolean) -> Unit) :
    FrameLayout(ctx) {

    /** Show the carrier bar and keys; off = the screen alone, fitted. */
    var keysOn = true
        set(v) { field = v; requestLayout(); invalidate(); keysView.invalidate() }
    var backdrop = BACKDROP_BLACK
        set(v) { field = v; texture = null; invalidate(); keysView.invalidate() }
    var carrier = ""
        set(v) { if (field != v) { field = v; invalidate() } }

    /** Draw the phone's wallpaper under the backdrop. */
    var wallpaperOn = false
        set(v) { field = v; invalidate(); keysView.invalidate() }
    /** Blurred home-screen wallpaper (null with [wallpaperOn]: the window shows the real one). */
    var wallpaper: Bitmap? = null
        set(v) { field = v; invalidate() }
    /** 0..1: how opaque the backdrop is over the wallpaper. */
    var backdropOpacity = 0.4f
        set(v) { field = v; invalidate(); keysView.invalidate() }

    private var guestW = 480
    private var guestH = 854

    // ---- Open foldable (see FoldSkin): set [foldCrease] to the fold, null when closed ----
    val foldSkin = FoldSkin()
    val foldKeyboard = FoldKeyboard(ctx, onKey).apply {
        visibility = GONE
        painter = { c, k -> drawKey(c, k, raised = true) }
        onTouchDown = { wakeBacklight() }
    }
    /** The fold in this view's coordinates while the phone is open (landscape), else null. */
    var foldCrease: Rect? = null
        set(v) {
            if (field == v) return
            field = v
            foldKeyboard.visibility = if (v != null) VISIBLE else GONE
            if (v == null) foldKeyboard.releaseAll()
            updateHinge()
            requestLayout(); invalidate(); keysView.invalidate()
        }
    private val screenRect = Rect()
    private val topBar = RectF()
    /** A drawn hardware key. */
    class Key(var vk: Int, val kind: Int) {
        val rect = RectF()
        var pressed = false
        /** Text instead of a drawn mark (fold keys and the keyboard); [alt]: small second character. */
        var label: String? = null
        var alt: String? = null
        /** Stable identity for the backlight's per-key unevenness (0: taken from vk, kind and label). */
        var id = 0
    }

    private val keys = listOf(
        Key(Keymap.VK_TTALK, KIND_SOFT), Key(Keymap.VK_TSOFT1, KIND_SOFT),
        Key(Keymap.VK_TSOFT2, KIND_SOFT), Key(Keymap.VK_TEND, KIND_SOFT),
        Key(0x25, KIND_LEFT), Key(Keymap.VK_UP, KIND_UP), Key(Keymap.VK_RETURN, KIND_ACTION),
        Key(Keymap.VK_DOWN, KIND_DOWN), Key(0x27, KIND_RIGHT),
    )
    private val softKeys get() = keys.subList(0, 4)
    private val dpadKeys get() = keys.subList(4, 9)
    private val dpad = RectF()
    private val dpadCentre = RectF()
    private val pointerKey = HashMap<Int, Key>()
    /** Lock button at the top left, partly off the window's edge; [lockTouch] is its touch area. */
    private val lockKey = Key(0, KIND_LOCK)
    private val lockTouch = RectF()

    /** Tapping the lock button. */
    var onLock: (() -> Unit)? = null
    /** Holding the lock button (like a phone's power key): the power menu. */
    var onLockLong: (() -> Unit)? = null
    private var lockLongFired = false
    private val lockHold = Runnable {
        lockLongFired = true
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        onLockLong?.invoke()
    }

    /** Right edge of the lock button (0 without it), for overlays sharing the top-left corner. */
    val lockRight: Float get() = if (lockKey.rect.isEmpty) 0f else lockKey.rect.right
    /** Where the lock button shows (for anchoring the power menu under it). */
    val lockRect: RectF get() = lockKey.rect

    /**
     * Windows Mobile's notification light, drawn as a dot in the top corner opposite the lock
     * button: 0 off, [NativeBridge.LED_LIGHT] on, with [NativeBridge.LED_BLINK] flashing (WM's
     * pattern is a short flash about every two seconds).
     */
    var alertLight = 0
        set(v) { if (field != v) { field = v; keysView.invalidate() } }

    private fun drawAlertLight(c: Canvas) {
        if (lockKey.rect.isEmpty) return
        val d = resources.displayMetrics.density
        val r = 4.5f * d
        var cx = width - maxOf(lockKey.rect.left, 0f) - r
        var cy = (maxOf(lockKey.rect.top, 0f) + lockKey.rect.bottom) / 2
        if (foldCrease != null) {
            // Open fold: the display runs edge to edge across the top, so the light sits just
            // below it at the right edge (where the lock's mirror image would be on the display).
            val dr = displayRect()
            cx = width - r * 3f
            cy = dr.bottom + r * 2.6f
        }
        val on = alertLight and NativeBridge.LED_LIGHT != 0
        val blink = alertLight and NativeBridge.LED_BLINK != 0
        val lit = on && (!blink || android.os.SystemClock.uptimeMillis() % 2000 < 180)
        // Unlit: a dark lens with a faint rim, so the light's place is visible.
        paint.shader = null
        paint.style = Paint.Style.FILL
        paint.color = if (light) 0x40000000 else 0xFF15171A.toInt()
        c.drawCircle(cx, cy, r, paint)
        if (lit) {
            paint.shader = RadialGradient(cx, cy, r * 3.2f, intArrayOf(0xC040FF70.toInt(), 0x4040FF70, 0x0040FF70),
                floatArrayOf(0f, 0.35f, 1f), Shader.TileMode.CLAMP)
            c.drawCircle(cx, cy, r * 3.2f, paint)
            paint.shader = RadialGradient(cx, cy - r * 0.3f, r, 0xFFE8FFEE.toInt(), 0xFF2EE05A.toInt(), Shader.TileMode.CLAMP)
            c.drawCircle(cx, cy, r, paint)
            paint.shader = null
        }
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = maxOf(1f, 0.8f * d)
        paint.color = if (light) 0x60000000 else 0x50FFFFFF
        c.drawCircle(cx, cy, r, paint)
        paint.style = Paint.Style.FILL
        // Blinking: redraw at the next flash edge only.
        if (on && blink) {
            val t = android.os.SystemClock.uptimeMillis() % 2000
            keysView.postInvalidateDelayed(if (t < 180) 180 - t else 2000 - t)
        }
    }

    // ---- Dock: a row of Android apps at the top or bottom of the display (LauncherBridge) ----
    // The band is taken from the display's own area and the display shrinks (same shape, centred)
    // into the rest, so the dock never lies over Windows Mobile's screen: touches there never reach
    // the guest, and the guest's resolution (and its saved states) stay as they are.

    /** Settings.DOCK_OFF / DOCK_TOP / DOCK_BOTTOM. */
    var dockPos = Settings.DOCK_OFF
        set(v) { if (field != v) { field = v; requestLayout(); keysView.invalidate() } }
    /** Slots in the row (4-8); slots past [dockIcons] are empty. */
    var dockSlots = 5
        set(v) { if (field != v) { field = v; requestLayout(); keysView.invalidate() } }
    /** Each slot's icon, null = empty. */
    var dockIcons: List<Bitmap?> = emptyList()
        set(v) { field = v; keysView.invalidate() }
    /** A slot tapped / held. */
    var onDockTap: ((Int) -> Unit)? = null
    var onDockLong: ((Int) -> Unit)? = null
    private val dockRect = RectF()
    private var dockIcon = 0f
    private var dockPressed = -1
    private var dockDown = false
    private var dockLongFired = false
    private val dockHold = Runnable {
        dockLongFired = true
        performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
        val slot = dockPressed
        dockPressed = -1
        keysView.invalidate()
        onDockLong?.invoke(slot)
    }

    /** Takes the dock's band from the display's area (after the layout placed the display). */
    private fun carveDock() {
        dockRect.setEmpty()
        if (dockPos == Settings.DOCK_OFF || screenRect.isEmpty) return
        val r = Rect(screenRect)
        val d = resources.displayMetrics.density
        val slot = r.width().toFloat() / dockSlots
        val icon = minOf(slot * 0.62f, 52 * d, r.height() * 0.09f)
        val h = icon + 2 * minOf(9 * d, icon * 0.3f)
        val nh = r.height() - h
        val nw = nh * r.width() / r.height()
        val left = r.left + (r.width() - nw) / 2
        if (dockPos == Settings.DOCK_TOP) {
            dockRect.set(r.left.toFloat(), r.top.toFloat(), r.right.toFloat(), r.top + h)
            screenRect.set(left.toInt(), Math.ceil((r.top + h).toDouble()).toInt(), (left + nw).toInt(), r.bottom)
        } else {
            dockRect.set(r.left.toFloat(), r.bottom - h, r.right.toFloat(), r.bottom.toFloat())
            screenRect.set(left.toInt(), r.top, (left + nw).toInt(), (r.bottom - h).toInt())
        }
        dockIcon = icon
    }

    private fun drawDock(c: Canvas) {
        if (dockRect.isEmpty) return
        val d = resources.displayMetrics.density
        val p = paint
        val r = dockRect
        val inset = 3 * d
        val bar = RectF(r.left + inset, r.top + inset, r.right - inset, r.bottom - inset)
        val rad = minOf(14 * d, bar.height() / 3)
        // Dark glass, like the keys: see-through with a light rim.
        p.shader = null
        p.style = Paint.Style.FILL
        p.color = if (light) 0x26000000 else 0x66000000
        c.drawRoundRect(bar, rad, rad, p)
        p.shader = LinearGradient(0f, bar.top, 0f, bar.bottom, 0x22FFFFFF, 0x00FFFFFF, Shader.TileMode.CLAMP)
        c.drawRoundRect(bar, rad, rad, p)
        p.shader = null
        p.style = Paint.Style.STROKE
        p.strokeWidth = maxOf(1f, 0.8f * d)
        p.color = if (light) 0x40000000 else 0x50FFFFFF
        c.drawRoundRect(bar, rad, rad, p)
        val slot = r.width() / dockSlots
        val s = dockIcon
        for (i in 0 until dockSlots) {
            val cx = r.left + slot * (i + 0.5f)
            val cy = r.centerY()
            val bmp = dockIcons.getOrNull(i)
            if (bmp != null) {
                p.style = Paint.Style.FILL
                p.color = Color.BLACK
                p.alpha = if (i == dockPressed) 140 else 255
                c.drawBitmap(bmp, null, RectF(cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2), p)
                p.alpha = 255
            } else {
                // An empty slot: a faint ring with a plus (tapping it picks the dock's apps).
                p.style = Paint.Style.STROKE
                p.strokeWidth = maxOf(1f, 1.4f * d)
                p.color = if (light) 0x50000000 else if (i == dockPressed) 0xA0FFFFFF.toInt() else 0x50FFFFFF
                c.drawCircle(cx, cy, s * 0.34f, p)
                c.drawLine(cx - s * 0.14f, cy, cx + s * 0.14f, cy, p)
                c.drawLine(cx, cy - s * 0.14f, cx, cy + s * 0.14f, p)
            }
        }
        p.style = Paint.Style.FILL
    }

    private fun dockSlotAt(x: Float, y: Float): Int =
        if (!dockRect.contains(x, y)) -1 else ((x - dockRect.left) / (dockRect.width() / dockSlots)).toInt().coerceIn(0, dockSlots - 1)

    /** Touches that start on the dock are the dock's: a tap launches, a hold opens the picker. */
    private fun dockTouch(e: MotionEvent): Boolean {
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                val slot = dockSlotAt(e.x, e.y)
                if (slot < 0) return false
                dockDown = true
                dockPressed = slot
                dockLongFired = false
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                postDelayed(dockHold, 550)
                keysView.invalidate()
                return true
            }
            MotionEvent.ACTION_UP -> {
                if (!dockDown) return false
                dockDown = false
                removeCallbacks(dockHold)
                val slot = dockPressed
                dockPressed = -1
                keysView.invalidate()
                if (!dockLongFired && slot >= 0 && dockSlotAt(e.x, e.y) == slot) onDockTap?.invoke(slot)
                return true
            }
            MotionEvent.ACTION_CANCEL -> {
                if (!dockDown) return false
                dockDown = false
                removeCallbacks(dockHold)
                dockPressed = -1
                keysView.invalidate()
                return true
            }
            else -> return dockDown
        }
    }

    /** Show the square D-pad under the keys (its space is kept either way). */
    var dpadOn = false
        set(v) { field = v; requestLayout(); invalidate(); keysView.invalidate() }

    /** What the four keys send, left to right (Windows virtual keys). */
    fun setSoftKeys(vks: IntArray) {
        for (i in 0..3) softKeys[i].vk = vks.getOrElse(i) { softKeys[i].vk }
        vks.copyInto(softVks, 0, 0, minOf(4, vks.size))
        requestLayout()
        keysView.invalidate()
    }

    // ---- JSON skin (SkinSpec): when it has a layout for the current mode, it places everything ----

    /** The skin's layouts; null = the built-in arrangement below. */
    var spec: SkinSpec? = null
        set(v) { field = v; requestLayout(); invalidate(); keysView.invalidate() }
    private val softVks = intArrayOf(Keymap.VK_TTALK, Keymap.VK_TSOFT1, Keymap.VK_TSOFT2, Keymap.VK_TEND)
    private val specKeys = ArrayList<Key>()
    private var specActive = false
    private val keyIndex = Regex("k([1-4])")

    /** The skin layout for the current mode, if the skin has one. */
    private fun specLayout(): SkinSpec.Layout? {
        val s = spec ?: return null
        return when {
            foldCrease != null -> if (foldKeyboard.useIme) s.foldableIme ?: s.foldable else s.foldable
            !keysOn -> null
            dpadOn -> s.frontDpad ?: s.front
            else -> s.front
        }
    }

    /** Places the display, keys, D-pad, lock button and keyboard from a skin layout. */
    private fun layoutSpec(l: SkinSpec.Layout, pw: Int, ph: Int) {
        val fold = foldCrease != null
        specKeys.clear()
        keys.forEach { it.rect.setEmpty() }
        topBar.setEmpty()
        dpad.setEmpty()
        lockKey.rect.setEmpty()
        lockTouch.setEmpty()
        foldSkin.keyboard.setEmpty()
        // Display height over width as shown (landscape when the fold turns it).
        val ratio = if (fold) guestW.toFloat() / guestH else guestH.toFloat() / guestW
        // The cover layout without the D-pad keeps the D-pad's space but leaves it out.
        val skipDpad = !fold && !dpadOn
        for (it in l.items) {
            val r = RectF(it.x * pw, it.y * ph, (it.x + it.w) * pw, (it.y + it.h) * ph)
            when (it.type) {
                "screen" -> {
                    var w = r.width()
                    var h = w * ratio
                    if (h > r.height() * 1.02f) { h = r.height(); w = h / ratio }
                    val left = r.left + (r.width() - w) / 2
                    screenRect.set(left.toInt(), r.top.toInt(), (left + w).toInt(), (r.top + h).toInt())
                }
                "lock" -> {
                    lockKey.rect.set(r)
                    val p = r.height()
                    lockTouch.set(r.left - p, 0f, r.right + p, r.bottom + p)
                }
                "dpad" -> if (!skipDpad) {
                    val s = minOf(r.width(), r.height())
                    dpad.set(r.centerX() - s / 2, r.centerY() - s / 2, r.centerX() + s / 2, r.centerY() + s / 2)
                    val c = s * DPAD_CENTRE / 2
                    dpadCentre.set(dpad.centerX() - c, dpad.centerY() - c, dpad.centerX() + c, dpad.centerY() + c)
                    for (k in dpadKeys) k.rect.set(dpad)
                }
                "keyboard" -> foldSkin.keyboard.set(r.left.toInt(), r.top.toInt(), r.right.toInt(), r.bottom.toInt())
                else -> {
                    // k1..k4 on the cover screen follow the key assignment in Settings.
                    val idx = keyIndex.matchEntire(it.id)?.groupValues?.get(1)?.toInt()
                    val vk = if (idx != null && !fold) softVks[idx - 1] else SkinSpec.actionVk(it.action)
                    specKeys += Key(vk, KIND_SOFT).apply {
                        rect.set(r)
                        label = it.label ?: if (idx != null) null else SkinSpec.actionLabel(it.action)
                    }
                }
            }
        }
        if (fold) fitKeyboard()
    }

    /**
     * The skin keyboard's number row: the keyboard's area grows upward by a row
     * ([FoldKeyboard.heightFor]) so the keys keep their size, but stays below the display and any
     * key or D-pad above it.
     */
    private fun fitKeyboard() {
        val k = foldSkin.keyboard
        if (k.isEmpty) return
        val want = k.bottom - foldKeyboard.heightFor(k.height())
        if (want >= k.top) return
        var limit = screenRect.bottom.toFloat()
        if (!dpad.isEmpty && dpad.bottom <= k.top) limit = maxOf(limit, dpad.bottom)
        for (s in specKeys) if (s.rect.bottom <= k.top) limit = maxOf(limit, s.rect.bottom)
        limit += 6 * resources.displayMetrics.density
        k.top = minOf(k.top, maxOf(want, Math.ceil(limit.toDouble()).toInt()))
    }

    // Phone screen shape: the camera cut-outs at the top (and their bottom edge), the bottom
    // corners' radius and the top-left corner's (for the lock button).
    private var cutouts: List<Rect> = emptyList()
    private var cutoutBottom = 0
    private var cornerRadius = 0
    private var topCorner = 0

    override fun onApplyWindowInsets(insets: WindowInsets): WindowInsets {
        val rects = insets.displayCutout?.boundingRects?.filter { it.top <= 0 || it.top < height / 4 } ?: emptyList()
        val cut = rects.maxOfOrNull { it.bottom } ?: 0
        var corner = 0
        var top = 0
        if (android.os.Build.VERSION.SDK_INT >= 31) {
            for (pos in intArrayOf(RoundedCorner.POSITION_BOTTOM_LEFT, RoundedCorner.POSITION_BOTTOM_RIGHT))
                corner = maxOf(corner, insets.getRoundedCorner(pos)?.radius ?: 0)
            top = insets.getRoundedCorner(RoundedCorner.POSITION_TOP_LEFT)?.radius ?: 0
        }
        if (cut != cutoutBottom || corner != cornerRadius || top != topCorner || rects != cutouts) {
            cutouts = rects
            cutoutBottom = cut
            cornerRadius = corner
            topCorner = top
            requestLayout()
            invalidate()
        }
        return super.onApplyWindowInsets(insets)
    }

    /**
     * Shine: a few virtual lights fixed in the room (placed above and behind the viewer when the
     * keys first appear). Each frame their positions are taken into the phone's frame from its
     * orientation, and each light's mirror reflection in the key glass, as seen from an eye in
     * front of the screen, gives a highlight point; so highlights move like real reflections.
     * Orientation comes from the game rotation vector (gyroscope + accelerometer: no location or
     * compass) at 50 Hz and is damped per display frame; drawing stops once it settles.
     */
    private val sensors = ctx.getSystemService(SensorManager::class.java)
    private val rot = FloatArray(9)          // device -> world, latest
    private var haveRot = false
    private var worldLights: Array<FloatArray>? = null
    private val lightDev = Array(LIGHTS.size) { FloatArray(3) }       // damped, phone frame (cm)
    private val lightTarget = Array(LIGHTS.size) { FloatArray(3) }
    private var lastFrameNs = 0L
    private val pxPerCm = ctx.resources.displayMetrics.ydpi / 2.54f
    /** Gyro key shine: brightness and 3D width of the reflections, 0..100 (50 = default). */
    var shineBrightness = 50
        set(v) { field = v; keysView.invalidate() }
    var shineWidth = 50
        set(v) { field = v; keysView.invalidate() }

    /** Key glass opacity 0..1 (1 = the stock look). */
    var keyOpacity = 1f
        set(v) { field = v; keysView.invalidate() }
    /** Key colour (0xRRGGBB), or null for the skin's own glass; glyphs pick dark or light to match. */
    var keyColor: Int? = null
        set(v) { field = v; keysView.invalidate() }
    /** Frosted keys: the wallpaper, blurred, laid out like the backdrop (null: no frosting). */
    var keyFrost: Bitmap? = null
        set(v) { field = v; frostLayer = null; keysView.invalidate() }
    /** [keyFrost] rendered once at the view's size (rebuilt on a new picture or size), so each frame
     *  only copies pixels: no per-frame scaling, which flickered while the keys animate. */
    private var frostLayer: Bitmap? = null

    private fun frostLayer(): Bitmap? {
        val src = keyFrost ?: return null
        val have = frostLayer
        if (have != null && have.width == width && have.height == height) return have
        if (width <= 0 || height <= 0) return null
        val out = Bitmap.createBitmap(width, height, Bitmap.Config.ARGB_8888)
        val k = maxOf(width.toFloat() / src.width, height.toFloat() / src.height)
        val m = Matrix().apply {
            setScale(k, k)
            postTranslate((width - src.width * k) / 2, (height - src.height * k) / 2)
        }
        Canvas(out).drawBitmap(src, m, Paint(Paint.FILTER_BITMAP_FLAG))
        frostLayer = out
        return out
    }
    private val keyClipPath = Path()

    private val tiltListener = object : SensorEventListener {
        override fun onSensorChanged(e: SensorEvent) {
            SensorManager.getRotationMatrixFromVector(rot, e.values)
            if (worldLights == null) {
                // Fix the lights in the room relative to how the phone is held right now.
                worldLights = Array(LIGHTS.size) { i -> mul(rot, LIGHTS[i]) }
            }
            haveRot = true
            keysView.postInvalidateOnAnimation()
        }
        override fun onAccuracyChanged(s: Sensor?, a: Int) {}
    }

    /** Follow the phone's orientation for the keys' shine (only while the activity is visible). */
    fun startTilt() {
        val g = sensors?.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR)
            ?: sensors?.getDefaultSensor(Sensor.TYPE_ROTATION_VECTOR) ?: return
        // 50 Hz is plenty: frames between samples are interpolated by the damping.
        sensors.registerListener(tiltListener, g, SensorManager.SENSOR_DELAY_GAME)
    }

    fun stopTilt() {
        sensors?.unregisterListener(tiltListener)
        worldLights = null
        haveRot = false
    }

    /** Advances the damped light positions; true while still moving. */
    private fun stepLights(): Boolean {
        val now = System.nanoTime()
        val dt = if (lastFrameNs == 0L) 0.016f else ((now - lastFrameNs) / 1e9f).coerceIn(0.001f, 0.1f)
        lastFrameNs = now
        val wl = worldLights
        for (i in LIGHTS.indices) {
            val t = lightTarget[i]
            if (wl != null && haveRot) {
                // world -> device is the transpose of the rotation matrix.
                val w = wl[i]
                t[0] = rot[0] * w[0] + rot[3] * w[1] + rot[6] * w[2]
                t[1] = rot[1] * w[0] + rot[4] * w[1] + rot[7] * w[2]
                t[2] = rot[2] * w[0] + rot[5] * w[1] + rot[8] * w[2]
            } else {
                LIGHTS[i].copyInto(t)
            }
        }
        // Critically damped-ish follow: time constant ~45 ms keeps it responsive but steady.
        val k = 1f - Math.exp((-dt / 0.045f).toDouble()).toFloat()
        var moving = false
        for (i in LIGHTS.indices) for (j in 0..2) {
            val d = lightTarget[i][j] - lightDev[i][j]
            if (Math.abs(d) > 0.05f) moving = true
            lightDev[i][j] += d * k
        }
        return moving
    }

    private fun mul(m: FloatArray, v: FloatArray) = floatArrayOf(
        m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
        m[3] * v[0] + m[4] * v[1] + m[5] * v[2],
        m[6] * v[0] + m[7] * v[1] + m[8] * v[2],
    )

    /** The keys are drawn in their own layer above the display so the shine can animate cheaply. */
    private val keysView = object : View(ctx) {
        override fun onDraw(c: Canvas) {
            drawAlertLight(c)
            drawDock(c)
            if (specActive) {
                val moving = stepLights()
                for (k in specKeys) drawKey(c, k, raised = true)
                if (!dpad.isEmpty) drawDpad(c)
                if (!lockKey.rect.isEmpty) drawKey(c, lockKey)
                drawShine(c)
                if (moving) postInvalidateOnAnimation()
                return
            }
            if (foldCrease != null) {
                for (k in foldSkin.keys) drawKey(c, k, raised = true)
                drawDpad(c)
                if (!lockKey.rect.isEmpty) drawKey(c, lockKey)
                return
            }
            if (!keysOn) { if (!lockKey.rect.isEmpty) drawKey(c, lockKey); return }
            val moving = stepLights()
            for (k in softKeys) if (!k.rect.isEmpty) drawKey(c, k, raised = true)
            if (!lockKey.rect.isEmpty) drawKey(c, lockKey)
            if (dpadOn && !dpad.isEmpty) drawDpad(c)
            drawShine(c)
            if (moving) postInvalidateOnAnimation()
        }
    }

    // ---- Fold hinge: a band across the window at the fold, over the display, keys and keyboard ----

    /** Draw the hinge while the phone is open (see [foldCrease]). */
    var hingeOn = false
        set(v) { if (field != v) { field = v; updateHinge() } }
    /** One of the HINGE_* looks. */
    var hingeStyle = HINGE_CREASE
        set(v) { if (field != v) { field = v; hingeView.invalidate() } }
    /** 0..100: the band's size. */
    var hingeThickness = 30
        set(v) { if (field != v) { field = v; hingeView.invalidate() } }
    /** -50..50: offset from the fold, up to [HINGE_SHIFT] of the window's height either way. */
    var hingeLocation = 0
        set(v) { if (field != v) { field = v; hingeView.invalidate() } }
    /** 0..100: the band's opacity. */
    var hingeOpacity = 70
        set(v) { if (field != v) { field = v; hingeView.invalidate() } }

    private val hingePaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val hingeRect = RectF()       // the band (classic: the bar)
    private val hingeShadeRect = RectF()  // classic: the shadow under the bar
    private var hingeShader: Shader? = null
    private var hingeShade: Shader? = null
    // What the shaders were built for: rebuilt only when the geometry or style changes.
    private var hingeBuiltY = Float.NaN
    private var hingeBuiltS = 0f
    private var hingeBuiltW = 0
    private var hingeBuiltStyle = ""

    /** Its own layer on top, so nothing else redraws when the hinge changes. Not touchable. */
    private val hingeView = object : View(ctx) {
        override fun onDraw(c: Canvas) = drawHinge(c)
    }.apply { visibility = GONE }

    private fun updateHinge() {
        hingeView.visibility = if (hingeOn && foldCrease != null) VISIBLE else GONE
        hingeView.invalidate()
    }

    private fun buildHinge(y: Float, s: Float, w: Int, style: String) {
        hingeBuiltY = y; hingeBuiltS = s; hingeBuiltW = w; hingeBuiltStyle = style
        hingeShade = null
        when (style) {
            HINGE_SHADOW -> {
                // The top half lies over the bottom: a shadow falling downward from the line.
                hingeRect.set(0f, y, w.toFloat(), y + s * 2.5f)
                hingeShader = LinearGradient(0f, hingeRect.top, 0f, hingeRect.bottom,
                    intArrayOf(0xF0000000.toInt(), 0x90000000.toInt(), 0x38000000, 0x00000000),
                    floatArrayOf(0f, 0.18f, 0.5f, 1f), Shader.TileMode.CLAMP)
            }
            HINGE_CLASSIC -> {
                // A rounded plastic barrel: lit top edge, shaded body, dark underside, soft shadow below.
                val h = s * 1.5f
                val inset = h * 0.35f
                hingeRect.set(inset, y - h / 2, w - inset, y + h / 2)
                hingeShader = LinearGradient(0f, hingeRect.top, 0f, hingeRect.bottom,
                    intArrayOf(0xFF2A2A2A.toInt(), 0xFFC4C4C4.toInt(), 0xFF8E8E8E.toInt(), 0xFF575757.toInt(),
                        0xFF2E2E2E.toInt(), 0xFF101010.toInt()),
                    floatArrayOf(0f, 0.07f, 0.2f, 0.5f, 0.85f, 1f), Shader.TileMode.CLAMP)
                hingeShadeRect.set(inset + h / 2, hingeRect.bottom - h * 0.25f, w - inset - h / 2, hingeRect.bottom + h * 0.55f)
                hingeShade = LinearGradient(0f, hingeShadeRect.top, 0f, hingeShadeRect.bottom,
                    intArrayOf(0x90000000.toInt(), 0x40000000, 0x00000000), floatArrayOf(0f, 0.35f, 1f), Shader.TileMode.CLAMP)
            }
            HINGE_VOID -> {
                hingeRect.set(0f, y - s / 2, w.toFloat(), y + s / 2)
                hingeShader = null
            }
            else -> {
                // Crease: a soft shadow on each side of the line, a faint glint along the fold itself.
                hingeRect.set(0f, y - s * 1.5f, w.toFloat(), y + s * 1.5f)
                hingeShader = LinearGradient(0f, hingeRect.top, 0f, hingeRect.bottom,
                    intArrayOf(0x00000000, 0x50000000, 0xC0000000.toInt(), 0x50FFFFFF, 0xC0000000.toInt(), 0x50000000, 0x00000000),
                    floatArrayOf(0f, 0.3f, 0.47f, 0.5f, 0.53f, 0.7f, 1f), Shader.TileMode.CLAMP)
            }
        }
    }

    private fun drawHinge(c: Canvas) {
        val crease = foldCrease ?: return
        if (!hingeOn) return
        val d = resources.displayMetrics.density
        val y = (crease.top + crease.bottom) / 2f + hingeLocation / 50f * HINGE_SHIFT * height
        val s = (1f + hingeThickness * 0.4f) * d
        val style = hingeStyle
        if (y != hingeBuiltY || s != hingeBuiltS || width != hingeBuiltW || style != hingeBuiltStyle) buildHinge(y, s, width, style)
        val a = hingeOpacity * 255 / 100
        val p = hingePaint
        p.style = Paint.Style.FILL
        p.shader = null
        p.color = Color.BLACK
        when (style) {
            HINGE_VOID -> { p.alpha = a; c.drawRect(hingeRect, p) }
            HINGE_CLASSIC -> {
                val r = hingeRect.height() / 2
                p.shader = hingeShade; p.alpha = a
                c.drawRect(hingeShadeRect, p)
                p.shader = hingeShader; p.alpha = a
                c.drawRoundRect(hingeRect, r, r, p)
                // Knuckle splits: a dark groove with a lit edge, a fifth of the way in from each end.
                p.shader = null
                val gw = maxOf(1f, 1.2f * d)
                val top = hingeRect.top + r * 0.12f
                val bottom = hingeRect.bottom - r * 0.12f
                for (f in HINGE_KNUCKLES) {
                    val x = hingeRect.left + hingeRect.width() * f
                    p.color = Color.BLACK; p.alpha = a * 0xA0 / 255
                    c.drawRect(x - gw, top, x, bottom, p)
                    p.color = Color.WHITE; p.alpha = a * 0x50 / 255
                    c.drawRect(x, top, x + gw * 0.8f, bottom, p)
                }
            }
            else -> { p.shader = hingeShader; p.alpha = a; c.drawRect(hingeRect, p) }
        }
        p.shader = null
    }

    private val labelPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textAlign = Paint.Align.CENTER
        typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
    }
    private var texture: Bitmap? = null
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)

    init {
        setWillNotDraw(false)
        addView(screen, LayoutParams(1, 1))
        addView(keysView, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(foldKeyboard, LayoutParams(1, 1))
        addView(hingeView, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        LIGHTS.forEachIndexed { i, l -> l.copyInto(lightDev[i]) }
    }

    /** Where the display sits in this view (which fills the window). */
    fun displayRect() = Rect(screenRect)

    fun setGuestSize(w: Int, h: Int) {
        if (w == guestW && h == guestH) return
        guestW = w
        guestH = h
        requestLayout()
    }

    /** Height / width of the area the display gets in a window of this size. */
    fun displayAspect(winW: Int, winH: Int): Double {
        val l = spec?.let { s -> if (!keysOn) null else if (dpadOn) s.frontDpad ?: s.front else s.front }
        l?.items?.firstOrNull { it.type == "screen" }?.let { return (it.h * winH).toDouble() / (it.w * winW) }
        return if (!keysOn) (winH - minTop(winW)).toDouble() / winW
        else (winH - minTop(winW) - bottomArea(winW)).toDouble() / winW
    }

    /** The display starts just below the camera cut-out and the lock button, with a little padding. */
    private fun minTop(w: Int) = maxOf(w * 0.04f, cutoutBottom + w * 0.03f, lockTop(w) + w * (LOCK_H + 0.012f))

    /** The lock button sits on the top edge like a hardware power key, partly off the window. */
    private fun lockTop(w: Int): Float = -w * LOCK_H * LOCK_HIDDEN

    /** Left end of the lock button: clear of the rounded top corner and any cut-out. */
    private fun lockLeft(w: Int): Float {
        var left = maxOf(w * 0.07f, topCorner * 0.9f)
        for (r in cutouts) if (r.top < w * LOCK_H && r.right > left && r.left < left + w * LOCK_W) left = r.right + w * 0.03f
        return left
    }

    /** Key row plus the D-pad's space, which stays reserved when the D-pad is hidden. */
    private fun bottomArea(w: Int) = w * (KEY_H + DPAD_H + 3 * KEY_MARGIN) + cornerRadius * 0.35f

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        computeLayout(MeasureSpec.getSize(widthMeasureSpec), MeasureSpec.getSize(heightMeasureSpec))
        // (computeLayout keeps the old rects for an empty size: carve only freshly placed ones)
        if (MeasureSpec.getSize(widthMeasureSpec) > 0 && MeasureSpec.getSize(heightMeasureSpec) > 0) carveDock()
        // The display's place goes in its layout params, so the frame layout puts it there in one
        // pass (moving a SurfaceView twice per layout costs a compositor transaction each time).
        (screen.layoutParams as? LayoutParams)?.let { lp ->
            lp.width = screenRect.width()
            lp.height = screenRect.height()
            lp.leftMargin = screenRect.left
            lp.topMargin = screenRect.top
            lp.gravity = android.view.Gravity.TOP or android.view.Gravity.START
        }
        (foldKeyboard.layoutParams as? LayoutParams)?.let { lp ->
            val k = foldSkin.keyboard
            lp.width = maxOf(1, k.width())
            lp.height = maxOf(1, k.height())
            lp.leftMargin = k.left
            lp.topMargin = k.top
            lp.gravity = android.view.Gravity.TOP or android.view.Gravity.START
        }
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        super.onSizeChanged(w, h, oldw, oldh)
        texture = null
    }

    /** Lock button: on the top edge, a long key partly off the window like a phone's power key. */
    private fun placeLock(pw: Int) {
        val lt = lockTop(pw)
        val ll = lockLeft(pw)
        lockKey.rect.set(ll, lt, ll + pw * LOCK_W, lt + pw * LOCK_H)
        lockTouch.set(ll - pw * 0.02f, 0f, lockKey.rect.right + pw * 0.02f, lockKey.rect.bottom + pw * 0.03f)
    }

    private fun computeLayout(pw: Int, ph: Int) {
        if (pw == 0 || ph == 0) return
        val sl = specLayout()
        specActive = sl != null
        if (sl != null) { layoutSpec(sl, pw, ph); return }
        val crease = foldCrease
        if (crease != null) {
            // Open foldable: the display is turned to landscape in the slider's top half.
            // The lock button keeps its cover-screen size (sized from a phone-width, not the wide window).
            placeLock((ph * 0.4f).toInt())
            foldSkin.layout(pw, ph, crease.top, crease.bottom, guestH.toFloat() / guestW, lockKey.rect.bottom)
            screenRect.set(foldSkin.screen)
            topBar.setEmpty()
            keys.forEach { it.rect.setEmpty() }
            dpad.set(foldSkin.pad)
            dpadCentre.set(foldSkin.padCentre)
            fitKeyboard()
            return
        }
        val aspect = guestH.toFloat() / guestW
        if (!keysOn) {
            // No keys, but the lock button stays: it is also the way to the power menu and settings.
            val top = minTop(pw)
            val room = ph - top
            val w = minOf(pw.toFloat(), room / aspect)
            val h = w * aspect
            val y = top + (room - h) / 2
            screenRect.set(((pw - w) / 2).toInt(), y.toInt(), ((pw + w) / 2).toInt(), (y + h).toInt())
            topBar.setEmpty()
            keys.forEach { it.rect.setEmpty() }
            dpad.setEmpty()
            placeLock(pw)
            return
        }
        // Full width when it fits; otherwise (wide windows) full height between the bars.
        val bottom = bottomArea(pw)
        var w = pw.toFloat()
        var h = w * aspect
        val room = ph - minTop(pw) - bottom
        if (h > room) { h = room; w = h / aspect }
        // Display at the top (below the cut-out); keys and D-pad underneath.
        val top = minTop(pw).coerceAtMost(ph - h - bottom)
        val left = (pw - w) / 2
        screenRect.set(left.toInt(), top.toInt(), (left + w).toInt(), (top + h).toInt())
        topBar.set(0f, cutoutBottom.toFloat(), pw.toFloat(), screenRect.top.toFloat())
        // Keys stay clear of the display's rounded corners at the sides.
        val m = maxOf(pw * KEY_MARGIN, cornerRadius * 0.3f)
        val kh = pw * KEY_H
        val gap = pw * 0.02f
        val rowW = minOf(pw - 2 * m, w)
        val rowL = (pw - rowW) / 2
        val kw = (rowW - 3 * gap) / 4
        val ky = screenRect.bottom + pw * KEY_MARGIN
        for (i in 0..3) {
            val l = rowL + i * (kw + gap)
            softKeys[i].rect.set(l, ky, l + kw, ky + kh)
        }
        // D-pad: a square under the middle of the key row with a square action button in its
        // centre, so all four sides (and their arrows) have the same depth.
        val side = pw * DPAD_H
        val dy = ky + kh + pw * KEY_MARGIN
        dpad.set(pw / 2f - side / 2, dy, pw / 2f + side / 2, dy + side)
        val c = side * DPAD_CENTRE / 2
        dpadCentre.set(dpad.centerX() - c, dpad.centerY() - c, dpad.centerX() + c, dpad.centerY() + c)
        for (k in dpadKeys) if (dpadOn) k.rect.set(dpad) else k.rect.setEmpty()
        placeLock(pw)
    }

    /**
     * The D-pad region under a point: centre, else the side whose diagonal sector it falls in
     * (the pad and its centre are concentric squares, so the sectors meet the drawn diagonals).
     */
    private fun dpadKeyAt(x: Float, y: Float): Key? {
        if (dpad.isEmpty || (!dpadOn && foldCrease == null && !specActive) || !dpad.contains(x, y)) return null
        if (dpadCentre.contains(x, y)) return dpadKeys[2]
        val nx = x - dpad.centerX()
        val ny = y - dpad.centerY()
        return if (Math.abs(nx) > Math.abs(ny)) (if (nx < 0) dpadKeys[0] else dpadKeys[4])
        else (if (ny < 0) dpadKeys[1] else dpadKeys[3])
    }

    /** Light backdrop (dark key glyphs): silver or white, unless mostly see-through to the wallpaper. */
    private val light get() = (backdrop == BACKDROP_SILVER || backdrop == BACKDROP_WHITE) &&
        (!wallpaperOn || backdropOpacity >= 0.5f)

    override fun onDraw(canvas: Canvas) {
        drawBackdrop(canvas)
    }

    private fun drawBackdrop(c: Canvas) {
        val w = width.toFloat()
        val h = height.toFloat()
        paint.shader = null
        paint.style = Paint.Style.FILL
        var alpha = 255
        if (wallpaperOn) {
            val wp = wallpaper
            if (wp != null) {
                c.drawColor(Color.BLACK)
                // Centre-crop the (already blurred) wallpaper.
                val k = maxOf(w / wp.width, h / wp.height)
                val mtx = Matrix().apply {
                    setScale(k, k)
                    postTranslate((w - wp.width * k) / 2, (h - wp.height * k) / 2)
                }
                c.drawBitmap(wp, mtx, paint)
            }
            // else: the window shows the live wallpaper underneath.
            alpha = (backdropOpacity * 255).toInt().coerceIn(0, 255)
        }
        // The backdrop itself, over the wallpaper at [backdropOpacity].
        paint.alpha = alpha
        when (backdrop) {
            BACKDROP_GRAY -> {
                paint.shader = LinearGradient(0f, 0f, 0f, h, 0xFF26262A.toInt(), 0xFF141416.toInt(), Shader.TileMode.CLAMP)
                c.drawRect(0f, 0f, w, h, paint)
            }
            BACKDROP_SILVER, BACKDROP_WHITE -> {
                val tex = texture ?: makeTexture(width, height, backdrop == BACKDROP_SILVER).also { texture = it }
                c.drawBitmap(tex, 0f, 0f, paint)
            }
            else -> c.drawColor(Color.argb(alpha, 0, 0, 0))
        }
        paint.alpha = 255
        paint.shader = null
    }

    /**
     * Silver: satin metal, a soft vertical sheen with very fine grain (no brushing).
     * White: glossy plastic, bright with a broad specular band.
     */
    private fun makeTexture(w: Int, h: Int, metal: Boolean): Bitmap {
        val bmp = Bitmap.createBitmap(maxOf(1, w), maxOf(1, h), Bitmap.Config.ARGB_8888)
        val c = Canvas(bmp)
        val p = Paint(Paint.ANTI_ALIAS_FLAG)
        if (metal) {
            p.shader = LinearGradient(0f, 0f, w * 0.35f, h.toFloat(),
                intArrayOf(0xFFD9DCE0.toInt(), 0xFFB4B8BE.toInt(), 0xFFE6E8EB.toInt(), 0xFFA9ADB3.toInt(), 0xFFC9CCD1.toInt()),
                floatArrayOf(0f, 0.3f, 0.5f, 0.75f, 1f), Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, w.toFloat(), h.toFloat(), p)
            // Fine, isotropic grain: a small noise tile at low contrast.
            val n = 128
            val noise = Bitmap.createBitmap(n, n, Bitmap.Config.ARGB_8888)
            val rnd = Random(7)
            val px = IntArray(n * n) { val v = rnd.nextInt(256); Color.argb(14, v, v, v) }
            noise.setPixels(px, 0, n, 0, 0, n, n)
            p.shader = BitmapShader(noise, Shader.TileMode.REPEAT, Shader.TileMode.REPEAT)
            c.drawRect(0f, 0f, w.toFloat(), h.toFloat(), p)
        } else {
            p.shader = LinearGradient(0f, 0f, 0f, h.toFloat(), 0xFFFDFDFD.toInt(), 0xFFE4E5E8.toInt(), Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, w.toFloat(), h.toFloat(), p)
            p.shader = LinearGradient(0f, 0f, w.toFloat(), h * 0.45f,
                intArrayOf(0x00FFFFFF, 0x80FFFFFF.toInt(), 0x00FFFFFF), floatArrayOf(0.2f, 0.45f, 0.7f), Shader.TileMode.CLAMP)
            c.drawRect(0f, 0f, w.toFloat(), h.toFloat(), p)
        }
        // Soft vignette towards the edges for depth.
        p.shader = RadialGradient(w / 2f, h / 2f, maxOf(w, h) * 0.75f,
            intArrayOf(0x00000000, if (metal) 0x28000000 else 0x14000000), null, Shader.TileMode.CLAMP)
        c.drawRect(0f, 0f, w.toFloat(), h.toFloat(), p)
        return bmp
    }

    private fun drawCarrier(c: Canvas) {
        if (topBar.height() < 8) return
        val w = width.toFloat()
        val p = paint
        val name = carrier.ifBlank { "Windows Mobile" }
        p.style = Paint.Style.FILL
        p.typeface = Typeface.create("sans-serif-medium", Typeface.BOLD_ITALIC)
        p.textAlign = Paint.Align.CENTER
        p.textSize = minOf(w * 0.065f, topBar.height() * 0.4f)
        val tw = p.measureText(name)
        if (tw > w * 0.6f) p.textSize *= w * 0.6f / tw
        // Sit in the lower part of the bar, clear of the phone's camera cut-out.
        val ty = topBar.centerY() + p.textSize * 0.35f
        if (light) {
            p.setShadowLayer(w * 0.004f, 0f, w * 0.002f, 0x60FFFFFF)
            p.shader = LinearGradient(0f, ty - p.textSize, 0f, ty, 0xFF55585E.toInt(), 0xFF2A2C30.toInt(), Shader.TileMode.CLAMP)
        } else {
            p.setShadowLayer(w * 0.01f, 0f, w * 0.004f, 0xC0000000.toInt())
            p.shader = LinearGradient(0f, ty - p.textSize, 0f, ty,
                intArrayOf(0xFFFFFFFF.toInt(), 0xFFC8C8CE.toInt(), 0xFF8C8C94.toInt()), floatArrayOf(0f, 0.55f, 1f),
                Shader.TileMode.CLAMP)
        }
        c.drawText(name, w / 2, ty, p)
        p.clearShadowLayer()
        p.shader = null
    }

    private val inner = RectF()
    private val arc = RectF()
    private val mark = Path()
    private val keysClip = Path()
    private val keyClip = Path()
    private val shineKeys = ArrayList<RectF>()
    private val shineMatrix = Matrix()

    /**
     * A Windows Vista glass button: a dark, mostly see-through pane with barely rounded corners,
     * a thin light outer border, a fine dark line just inside it, and a faint gloss over the upper
     * half that ends in a crisp line. Talk and End are the same glass tinted green and red; a
     * pressed key lights up from inside. Reflections come only from the gyro shine ([drawShine]).
     */
    private fun drawKey(c: Canvas, key: Key, raised: Boolean = false) {
        val k = key.rect
        val p = paint
        val d = resources.displayMetrics.density
        val r = 2.5f * d
        val custom = keyColor
        val tint = when (key.vk) {
            Keymap.VK_TTALK -> 0x1E9C3A
            Keymap.VK_TEND -> 0xC0281E
            else -> custom ?: if (light) 0xFFFFFF else 0x0A0C10
        }
        val tinted = key.vk == Keymap.VK_TTALK || key.vk == Keymap.VK_TEND
        p.style = Paint.Style.FILL
        // Frosted glass: the blurred wallpaper behind the key (Key Blur).
        val frost = if (key.kind != KIND_LOCK) frostLayer() else null
        if (frost != null) {
            c.save()
            keyClipPath.reset()
            keyClipPath.addRoundRect(k, r, r, Path.Direction.CW)
            c.clipPath(keyClipPath)
            p.alpha = (255 * keyOpacity.coerceAtLeast(0.35f)).toInt()
            c.drawBitmap(frost, 0f, 0f, p)
            p.alpha = 255
            c.restore()
        }
        // Glass body; a picked colour is a fuller tint. Key Opacity scales it.
        val a1 = ((if (tinted) 0x90 else if (custom != null) 0xC0 else if (light) 0x60 else 0x58) * keyOpacity).toInt()
        val a2 = ((if (tinted) 0x60 else if (custom != null) 0x90 else if (light) 0x30 else 0x30) * keyOpacity).toInt()
        p.shader = LinearGradient(0f, k.top, 0f, k.bottom, (a1 shl 24) or tint, (a2 shl 24) or tint, Shader.TileMode.CLAMP)
        c.drawRoundRect(k, r, r, p)
        // Gloss over the upper half, ending in a crisp line.
        val mid = k.top + k.height() * 0.48f
        p.shader = LinearGradient(0f, k.top, 0f, mid, 0x2EFFFFFF, 0x10FFFFFF, Shader.TileMode.CLAMP)
        inner.set(k.left, k.top, k.right, mid)
        c.save()
        c.clipRect(inner)
        c.drawRoundRect(k, r, r, p)
        c.restore()
        // Pressed: light from inside (Vista's hot glow), strongest along the bottom.
        if (key.pressed) {
            val glow = if (tinted) tint or 0x404040 else 0x9CC8FF
            p.shader = RadialGradient(k.centerX(), k.bottom, k.width() * 0.6f,
                (0xA0 shl 24) or glow, glow, Shader.TileMode.CLAMP)
            c.drawRoundRect(k, r, r, p)
        }
        p.shader = null
        // Borders: light outside, fine dark line inside.
        p.style = Paint.Style.STROKE
        p.strokeWidth = maxOf(1f, d)
        p.color = if (light) 0xB0707880.toInt() else 0xA8D8DCE2.toInt()
        val half = p.strokeWidth / 2
        inner.set(k.left + half, k.top + half, k.right - half, k.bottom - half)
        c.drawRoundRect(inner, r, r, p)
        p.color = if (light) 0x90FFFFFF.toInt() else 0x90000000.toInt()
        inner.inset(p.strokeWidth, p.strokeWidth)
        c.drawRoundRect(inner, r * 0.6f, r * 0.6f, p)
        p.style = Paint.Style.FILL
        if (key.kind == KIND_LOCK) {
            // Darker towards the window's edge, as if the glass curved away round the phone's side.
            val edge = maxOf(k.left, 0f)
            val shade = edge + (k.right - edge) * 0.4f
            p.shader = LinearGradient(edge, 0f, shade, 0f, if (light) 0x38000000 else 0x60000000, 0, Shader.TileMode.CLAMP)
            c.drawRect(edge, k.top, shade, k.bottom, p)
            p.shader = null
        }
        // Mark (with the key backlight: drawn dimmer, then lit over it; see glyph()).
        val markColor = when {
            tinted -> 0xF0FFFFFF.toInt()
            // A picked key colour: dark glyphs on light keys, light glyphs on dark ones.
            custom != null -> if (Color.luminance(0xFF000000.toInt() or custom) > 0.45f) 0xE01A1C20.toInt() else 0xF0F2F3F5.toInt()
            light -> 0xD0303236.toInt()
            else -> 0xE8E6E8EE.toInt()
        }
        p.color = markColor
        val s = k.height() * 0.17f
        // The lock button's mark sits in the part that shows.
        val x = k.centerX()
        val y = if (key.kind == KIND_LOCK) (maxOf(k.top, 0f) + k.bottom) / 2 + k.height() * 0.02f else k.centerY()
        val lbl = key.label
        // The lock button is a side key: no backlight.
        val backlit = backlightOn && key.kind != KIND_LOCK
        val seed = keySeed(key)
        val glow = if (backlit) keyGlow(seed, x, c.width) else 0f
        if (glow > 0f && backlightDirt > 0f) drawBleed(c, k, r, glow, seed)
        if (lbl == "⊞") {
            val fs = k.height() * 0.42f
            glyph(c, markColor, glow, backlit, x, y, fs * 0.6f) { drawWindowsFlag(c, x, y, fs) }
            return
        }
        if (lbl != null) {
            if (lbl.isBlank()) return
            val alt = key.alt
            val lx = if (alt != null) k.left + k.width() * 0.42f else x
            glyph(c, markColor, glow, backlit, x, y, k.height() * 0.3f) {
                labelPaint.textSize = k.height() * (if (lbl.length > 2) 0.26f else 0.36f)
                c.drawText(lbl, lx, y + labelPaint.textSize * 0.36f, labelPaint)
                if (alt != null) {
                    if (!glowPass) labelPaint.color = dimmed(if (light) 0xFF1F5FC0.toInt() else 0xFF8CC4FF.toInt(), backlit)
                    labelPaint.textSize = k.height() * 0.24f
                    c.drawText(alt, k.right - k.width() * 0.24f, k.top + k.height() * 0.38f, labelPaint)
                }
            }
            return
        }
        glyph(c, markColor, glow, backlit, x, y, s * 1.6f) { when (key.kind) {
            KIND_UP, KIND_DOWN, KIND_LEFT, KIND_RIGHT -> {
                mark.reset()
                when (key.kind) {
                    KIND_UP -> { mark.moveTo(x, y - s); mark.lineTo(x + s * 1.2f, y + s * 0.7f); mark.lineTo(x - s * 1.2f, y + s * 0.7f) }
                    KIND_DOWN -> { mark.moveTo(x, y + s); mark.lineTo(x + s * 1.2f, y - s * 0.7f); mark.lineTo(x - s * 1.2f, y - s * 0.7f) }
                    KIND_LEFT -> { mark.moveTo(x - s, y); mark.lineTo(x + s * 0.7f, y - s * 1.2f); mark.lineTo(x + s * 0.7f, y + s * 1.2f) }
                    else -> { mark.moveTo(x + s, y); mark.lineTo(x - s * 0.7f, y - s * 1.2f); mark.lineTo(x - s * 0.7f, y + s * 1.2f) }
                }
                mark.close()
                c.drawPath(mark, p)
            }
            KIND_ACTION -> c.drawCircle(x, y, s * 0.8f, p)
            KIND_LOCK -> {
                // Padlock: a body with a shackle arching over it, small within the visible part.
                val h = k.height() * 0.6f
                val bodyTop = y - h * 0.02f
                inner.set(x - h * 0.17f, bodyTop, x + h * 0.17f, y + h * 0.24f)
                c.drawRoundRect(inner, h * 0.04f, h * 0.04f, p)
                val sr = h * 0.105f
                val sw = h * 0.06f
                val sy = y - h * 0.12f
                arc.set(x - sr, sy - sr, x + sr, sy + sr)
                mark.reset()
                mark.moveTo(x - sr, bodyTop + sw / 2)
                mark.lineTo(x - sr, sy)
                mark.arcTo(arc, 180f, 180f, false)
                mark.lineTo(x + sr, bodyTop + sw / 2)
                p.style = Paint.Style.STROKE
                p.strokeWidth = sw
                c.drawPath(mark, p)
                p.style = Paint.Style.FILL
            }
            else -> when (key.vk) {
                // App Launch keys: one dot, two dots.
                Keymap.VK_APP1 -> c.drawCircle(x, y, maxOf(2f, 2.2f * d), p)
                Keymap.VK_APP2 -> {
                    val gap = maxOf(4f, 4.5f * d)
                    c.drawCircle(x - gap, y, maxOf(2f, 2.2f * d), p)
                    c.drawCircle(x + gap, y, maxOf(2f, 2.2f * d), p)
                }
                else -> {
                    val lw = k.width() * 0.2f
                    val lh = maxOf(2f, 1.5f * d)
                    c.drawRect(x - lw / 2, y - lh / 2, x + lw / 2, y + lh / 2, p)
                }
            }
        } }
    }

    /** The Start key's mark: the waving Windows flag (the user's outline, a 2000 x 1860 box), in the mark colour. */
    private fun drawWindowsFlag(c: Canvas, cx: Float, cy: Float, size: Float) {
        // Scaled into a reused path rather than by the canvas, so the backlight's glow shader and
        // halo (glyph()) stay in key coordinates.
        val k = size / 1860f
        flagMatrix.setScale(k, k)
        flagMatrix.postTranslate(cx - 1000f * k, cy - 930f * k)
        windowsLogo.transform(flagMatrix, flagPath)
        paint.style = Paint.Style.FILL
        c.drawPath(flagPath, paint)
    }

    private val flagPath = Path()
    private val flagMatrix = Matrix()

    private val windowsLogo = Path().apply {
        // Top left pane.
        moveTo(415f, 85f)
        cubicTo(640f, -30f, 900f, -60f, 1157f, 130f)
        lineTo(965f, 787f)
        cubicTo(830f, 700f, 640f, 640f, 600f, 655f)
        cubicTo(470f, 665f, 340f, 700f, 224f, 746f)
        close()
        // Top right pane.
        moveTo(1258f, 200f)
        cubicTo(1400f, 300f, 1520f, 330f, 1625f, 330f)
        cubicTo(1760f, 330f, 1880f, 290f, 1999f, 242f)
        lineTo(1809f, 900f)
        cubicTo(1680f, 960f, 1560f, 990f, 1420f, 993f)
        cubicTo(1290f, 995f, 1150f, 930f, 1067f, 852f)
        close()
        // Bottom left pane.
        moveTo(190f, 862f)
        cubicTo(300f, 810f, 430f, 775f, 550f, 775f)
        cubicTo(680f, 775f, 820f, 830f, 933f, 907f)
        lineTo(740f, 1570f)
        cubicTo(620f, 1490f, 480f, 1435f, 380f, 1443f)
        cubicTo(250f, 1450f, 100f, 1470f, 0f, 1525f)
        close()
        // Bottom right pane.
        moveTo(1034f, 971f)
        cubicTo(1150f, 1050f, 1280f, 1100f, 1370f, 1100f)
        cubicTo(1520f, 1100f, 1640f, 1070f, 1775f, 1016f)
        lineTo(1585f, 1785f)
        cubicTo(1460f, 1840f, 1330f, 1860f, 1210f, 1860f)
        cubicTo(1030f, 1860f, 910f, 1740f, 844f, 1628f)
        close()
    }

    /** The D-pad: one glass square, a square centre button, equal arrows, and the pressed region lit. */
    private fun drawDpad(c: Canvas) {
        val body = Key(0, KIND_SOFT).apply { rect.set(dpad); label = "" }
        drawKey(c, body)
        val centre = Key(0, KIND_SOFT).apply { rect.set(dpadCentre); pressed = dpadKeys[2].pressed; label = "" }  // plain, no mark
        drawKey(c, centre)
        val p = paint
        val d = resources.displayMetrics.density
        // Faint diagonals marking the four sides.
        p.style = Paint.Style.STROKE
        p.strokeWidth = maxOf(1f, d * 0.8f)
        p.color = if (light) 0x40000000 else 0x30FFFFFF
        c.drawLine(dpad.left, dpad.top, dpadCentre.left, dpadCentre.top, p)
        c.drawLine(dpad.right, dpad.top, dpadCentre.right, dpadCentre.top, p)
        c.drawLine(dpad.left, dpad.bottom, dpadCentre.left, dpadCentre.bottom, p)
        c.drawLine(dpad.right, dpad.bottom, dpadCentre.right, dpadCentre.bottom, p)
        p.style = Paint.Style.FILL
        val s = dpad.height() * 0.07f
        fun arrow(k: Key, x: Float, y: Float) {
            val color = if (k.pressed) 0xFFFFFFFF.toInt() else if (light) 0xD0303236.toInt() else 0xE0E6E8EE.toInt()
            mark.reset()
            when (k.kind) {
                KIND_UP -> { mark.moveTo(x, y - s); mark.lineTo(x + s * 1.3f, y + s * 0.7f); mark.lineTo(x - s * 1.3f, y + s * 0.7f) }
                KIND_DOWN -> { mark.moveTo(x, y + s); mark.lineTo(x + s * 1.3f, y - s * 0.7f); mark.lineTo(x - s * 1.3f, y - s * 0.7f) }
                KIND_LEFT -> { mark.moveTo(x - s, y); mark.lineTo(x + s * 0.7f, y - s * 1.3f); mark.lineTo(x + s * 0.7f, y + s * 1.3f) }
                else -> { mark.moveTo(x + s, y); mark.lineTo(x - s * 0.7f, y - s * 1.3f); mark.lineTo(x - s * 0.7f, y + s * 1.3f) }
            }
            mark.close()
            val glow = if (backlightOn) keyGlow(keySeed(k), x, c.width) else 0f
            glyph(c, color, glow, backlightOn, x, y, s * 1.6f) { c.drawPath(mark, p) }
        }
        arrow(dpadKeys[1], dpad.centerX(), (dpad.top + dpadCentre.top) / 2)
        arrow(dpadKeys[3], dpad.centerX(), (dpad.bottom + dpadCentre.bottom) / 2)
        arrow(dpadKeys[0], (dpad.left + dpadCentre.left) / 2, dpad.centerY())
        arrow(dpadKeys[4], (dpad.right + dpadCentre.right) / 2, dpad.centerY())
    }

    /**
     * The lights' reflections, per key: each key is a curved glass cap, so a light shows on it as a
     * specular spot where the cap's surface normal halves the angle between the light and the eye
     * (so it slides across the key as the phone turns), plus a glint along the rim facing the light.
     */
    private fun drawShine(c: Canvas) {
        shineKeys.clear()
        for (k in if (specActive) specKeys else softKeys) if (!k.rect.isEmpty) shineKeys.add(k.rect)
        if (!lockKey.rect.isEmpty) shineKeys.add(lockKey.rect)
        if ((dpadOn || specActive) && !dpad.isEmpty) shineKeys.add(dpad)
        if (shineKeys.isEmpty()) return
        // Light positions are in the phone's natural frame; turn them into this window's frame.
        @Suppress("DEPRECATION")
        val turn = (display?.rotation ?: 0)
        val cx = width / 2f
        val cy = height / 2f
        val d = resources.displayMetrics.density
        val r = 2.5f * d
        val wide = 0.3f + shineWidth / 50f * 0.7f
        val b = shineBrightness / 50f
        fun white(a: Float) = (minOf(255f, a * b).toInt().coerceAtLeast(0) shl 24) or 0xFFFFFF
        for (k in shineKeys) {
            c.save()
            keyClip.reset()
            keyClip.addRoundRect(k, r, r, Path.Direction.CW)
            c.clipPath(keyClip)
            val kx = (k.centerX() - cx) / pxPerCm
            val ky = -(k.centerY() - cy) / pxPerCm
            val h = k.height()
            for ((i, l) in lightDev.withIndex()) {
                if (l[2] <= 1f) continue  // behind the glass
                val (lx, ly) = when (turn) {
                    Surface.ROTATION_90 -> -l[1] to l[0]
                    Surface.ROTATION_180 -> -l[0] to -l[1]
                    Surface.ROTATION_270 -> l[1] to -l[0]
                    else -> l[0] to l[1]
                }
                // Unit vectors key -> light and key -> eye, and their half vector.
                var ax = lx - kx; var ay = ly - ky; var az = l[2]
                var n = Math.sqrt((ax * ax + ay * ay + az * az).toDouble()).toFloat()
                ax /= n; ay /= n; az /= n
                var vx = -kx; var vy = -ky; var vz = EYE_CM
                n = Math.sqrt((vx * vx + vy * vy + vz * vz).toDouble()).toFloat()
                vx /= n; vy /= n; vz /= n
                var hx = ax + vx; var hy = ay + vy; var hz = az + vz
                n = Math.sqrt((hx * hx + hy * hy + hz * hz).toDouble()).toFloat()
                hx /= n; hy /= n; hz /= n
                if (hz <= 0.05f) continue
                // Where on the cap the normal equals the half vector: the cap curves gently along
                // the key and strongly across it, so the spot travels far sideways, less up/down.
                val ox = hx / hz * h * 1.6f
                val oy = -hy / hz * h * 0.9f
                val limX = k.width() / 2 - h * 0.2f
                val limY = h / 2 - h * 0.12f
                val over = maxOf(Math.abs(ox) / maxOf(limX, 1f), Math.abs(oy) / maxOf(limY, 1f))
                val fade = if (over <= 1f) 1f else maxOf(0f, 1f - (over - 1f) * 1.5f)
                val strength = LIGHT_GAIN[i] * fade
                if (strength > 0.01f) {
                    val sx = k.centerX() + ox.coerceIn(-limX, limX)
                    val sy = k.centerY() + oy.coerceIn(-limY, limY)
                    val rx = h * 0.75f * wide
                    shineMatrix.setScale(1f, 0.38f, sx, sy)
                    paint.shader = RadialGradient(sx, sy, rx,
                        intArrayOf(white((if (light) 150f else 120f) * strength), white((if (light) 50f else 38f) * strength), 0x00FFFFFF),
                        floatArrayOf(0f, 0.3f, 1f), Shader.TileMode.CLAMP).apply { setLocalMatrix(shineMatrix) }
                    c.drawRect(k, paint)
                }
                // Rim glint: the bevel facing the light catches it, the far side stays dark.
                val dx = ax; val dy = -ay
                val dl = Math.sqrt((dx * dx + dy * dy).toDouble()).toFloat()
                if (dl > 0.02f) {
                    val ux = dx / dl; val uy = dy / dl
                    val reach = Math.abs(ux) * k.width() / 2 + Math.abs(uy) * h / 2
                    val rim = LIGHT_GAIN[i] * minOf(1f, dl * 2.5f)
                    paint.shader = LinearGradient(k.centerX() - ux * reach, k.centerY() - uy * reach,
                        k.centerX() + ux * reach, k.centerY() + uy * reach,
                        intArrayOf(0x00FFFFFF, 0x00FFFFFF, white((if (light) 200f else 150f) * rim)),
                        floatArrayOf(0f, 0.55f, 1f), Shader.TileMode.CLAMP)
                    paint.style = Paint.Style.STROKE
                    paint.strokeWidth = maxOf(1f, 1.2f * d * wide)
                    val half = paint.strokeWidth / 2
                    inner.set(k.left + half, k.top + half, k.right - half, k.bottom - half)
                    c.drawRoundRect(inner, r, r, paint)
                    paint.style = Paint.Style.FILL
                }
            }
            c.restore()
        }
        paint.shader = null
    }

    // ---- Key backlight: the glyphs light up on a key press or screen touch, then fade out ----

    private var backlightOn = false
    private var backlightIntensity = 70
    /** How long it stays lit after the last touch, ms; 0 = always on. */
    private var backlightTimeoutMs = 10_000L
    private var backlightRgb = 0xF4F8FF
    /** 0..1: how much it looks like a cheap membrane keypad lit from the side (see [keyGlow]). */
    private var backlightDirt = 0.25f
    private var blState = BL_OFF
    private var blFadeFrom = 0L
    /** The glyphs' inner glow: a unit radial gradient placed per glyph by [glowMatrix]; rebuilt on a colour change. */
    private var glowShader: RadialGradient? = null
    private val glowMatrix = Matrix()
    // Hotspot offset towards the LEDs for the glyph being drawn, in glyph radii (set by keyGlow).
    private var glowHotX = 0f
    private var glowHotY = 0f
    /** True while [glyph] draws the lit pass (the glyph's own colours are left alone then). */
    private var glowPass = false

    /**
     * Key backlight settings: [timeoutSec] 0 = always on; [color] one of Settings.backlightColor's
     * names ("theme": the generated Windows Mobile theme's accent); [dirt] 0..100.
     */
    fun setBacklight(on: Boolean, intensity: Int, timeoutSec: Int, color: String, dirt: Int) {
        val rgb = when (color) {
            "blue" -> 0x3C8CFF
            "green" -> 0x40FF78
            "amber" -> 0xFFA028
            "red" -> 0xFF3C2C
            "theme" -> runCatching { ThemeGenerator(context).palette().primaryHi and 0xFFFFFF }.getOrDefault(0xF4F8FF)
            else -> 0xF4F8FF
        }
        val dirtF = dirt.coerceIn(0, 100) / 100f
        if (rgb != backlightRgb || dirtF != backlightDirt) glowShader = null
        backlightRgb = rgb
        backlightDirt = dirtF
        backlightIntensity = intensity.coerceIn(0, 100)
        backlightTimeoutMs = timeoutSec.coerceAtLeast(0) * 1000L
        backlightOn = on
        removeCallbacks(blTimeout)
        removeCallbacks(blFade)
        // Light up so a change shows at once (it times out as usual).
        if (on) { blState = BL_OFF; wakeBacklight() } else { blState = BL_OFF; redrawKeys() }
    }

    /** A key or the screen was touched: light the keys and restart the timeout. */
    fun wakeBacklight() {
        if (!backlightOn) return
        removeCallbacks(blTimeout)
        removeCallbacks(blFade)
        if (backlightTimeoutMs > 0) postDelayed(blTimeout, backlightTimeoutMs)
        if (blState != BL_LIT) { blState = BL_LIT; redrawKeys() }
    }

    private fun redrawKeys() {
        keysView.invalidate()
        foldKeyboard.invalidate()
    }

    private val blTimeout = Runnable {
        blState = BL_FADING
        blFadeFrom = android.os.SystemClock.uptimeMillis()
        blFade.run()
    }

    /** Redraws each frame while fading out (only then). */
    private val blFade = object : Runnable {
        override fun run() {
            if (blState == BL_FADING && android.os.SystemClock.uptimeMillis() - blFadeFrom >= BL_FADE_MS) blState = BL_OFF
            redrawKeys()
            if (blState == BL_FADING) postOnAnimation(this)
        }
    }

    private fun backlightLevel(): Float = when (blState) {
        BL_LIT -> 1f
        BL_FADING -> {
            val t = ((android.os.SystemClock.uptimeMillis() - blFadeFrom) / BL_FADE_MS.toFloat()).coerceIn(0f, 1f)
            1f - t * t * (3f - 2f * t)
        }
        else -> 0f
    }

    private fun keySeed(key: Key) = if (key.id != 0) key.id else key.vk * 31 + key.kind * 1009 + (key.label?.hashCode() ?: 0) + 7

    /** A stable pseudo-random 0..1 for key [seed] (value [n] of it). */
    private fun rnd(seed: Int, n: Int): Float {
        var h = seed * -0x61c88647 + n * 0x27d4eb2d
        h = h xor (h ushr 15)
        h *= 0x2c1b3c6d
        h = h xor (h ushr 12)
        h *= 0x297a2d39
        h = h xor (h ushr 15)
        return (h ushr 8) / 16777216f
    }

    /**
     * How lit a glyph at [x] (in a canvas [width] wide) is now, 0..1. With dirt it is a rubber
     * keypad with an indirect light: each key a little different (stable per key), some barely
     * lit, the light falling off away from LEDs at the left, and each glyph's hotspot pulled
     * towards them (lower left; set in [glowHotX]/[glowHotY] for [glyph]).
     */
    private fun keyGlow(seed: Int, x: Float, width: Int): Float {
        val level = backlightLevel()
        if (level <= 0f || backlightIntensity <= 0) return 0f
        val dirt = backlightDirt
        val r1 = rnd(seed, 1)
        val r2 = rnd(seed, 2)
        val r3 = rnd(seed, 3)
        var f = 1f - dirt * (0.15f + 0.5f * r1)
        if (r2 < dirt * 0.3f) f *= 0.3f + 0.4f * r3
        f *= 1f - dirt * 0.5f * (x / maxOf(1, width)).coerceIn(0f, 1f)
        f *= 1f - dirt * 0.25f
        glowHotX = dirt * (-0.7f + (r3 - 0.5f) * 0.8f)
        glowHotY = dirt * (0.45f + (r1 - 0.5f) * 0.8f)
        return (level * backlightIntensity / 100f * f).coerceIn(0f, 1f)
    }

    /** With the backlight on, unlit glyphs are a little see-through so lit ones stand out. */
    private fun dimmed(color: Int, backlit: Boolean) =
        if (!backlit) color else (((color ushr 24) * BL_DIM).toInt() shl 24) or (color and 0xFFFFFF)

    private fun mix(a: Int, b: Int, t: Float) = Color.rgb(
        ((a shr 16 and 255) * (1 - t) + (b shr 16 and 255) * t).toInt(),
        ((a shr 8 and 255) * (1 - t) + (b shr 8 and 255) * t).toInt(),
        ((a and 255) * (1 - t) + (b and 255) * t).toInt())

    private fun buildGlow(): RadialGradient {
        val c = backlightRgb
        // A hot core fading to the backlight's colour, darker at the rim the dirtier it is.
        val core = mix(c, 0xFFFFFF, 0.6f)
        val rim = mix(c, 0, 0.15f + 0.45f * backlightDirt)
        return RadialGradient(0f, 0f, 1f, intArrayOf(core, Color.rgb(c shr 16 and 255, c shr 8 and 255, c and 255), rim),
            floatArrayOf(0f, 0.55f, 1f), Shader.TileMode.CLAMP).also { glowShader = it }
    }

    /**
     * Draws a glyph with [draw] (using [paint] and [labelPaint] as set here): in [base] (dimmed
     * when [backlit]), then, when [glow] > 0, again lit: the inner-glow shader centred on
     * ([gx], [gy]) with radius [gr] and a faint halo in the backlight's colour.
     */
    private inline fun glyph(c: Canvas, base: Int, glow: Float, backlit: Boolean, gx: Float, gy: Float, gr: Float, draw: () -> Unit) {
        val dim = dimmed(base, backlit)
        paint.color = dim
        labelPaint.color = dim
        draw()
        if (glow > 0.004f) {
            val shader = glowShader ?: buildGlow()
            glowMatrix.setScale(gr, gr)
            glowMatrix.postTranslate(gx + glowHotX * gr, gy + glowHotY * gr)
            shader.setLocalMatrix(glowMatrix)
            val a = (glow * 255).toInt().coerceIn(0, 255)
            // Outer glow: first a wide, soft spill of light around the glyph (drawn with the glyph
            // itself nearly clear), then the lit glyph with a tighter, brighter halo.
            val wide = ((glow * 0xB0).toInt().coerceIn(0, 255) shl 24) or backlightRgb
            paint.color = ((glow * 0x30).toInt().coerceIn(0, 255) shl 24) or backlightRgb
            labelPaint.color = paint.color
            paint.setShadowLayer(maxOf(1f, gr * 0.9f), 0f, 0f, wide)
            labelPaint.setShadowLayer(maxOf(1f, gr * 0.9f), 0f, 0f, wide)
            glowPass = true
            draw()
            glowPass = false
            val halo = ((glow * 0xD8).toInt().coerceIn(0, 255) shl 24) or backlightRgb
            val hr = maxOf(1f, gr * 0.45f)
            paint.shader = shader
            paint.color = Color.WHITE
            paint.alpha = a
            paint.setShadowLayer(hr, 0f, 0f, halo)
            labelPaint.shader = shader
            labelPaint.color = Color.WHITE
            labelPaint.alpha = a
            labelPaint.setShadowLayer(hr, 0f, 0f, halo)
            glowPass = true
            draw()
            glowPass = false
            paint.shader = null
            paint.clearShadowLayer()
            labelPaint.shader = null
            labelPaint.clearShadowLayer()
        }
        paint.color = base
    }

    /** Dirt: light leaking round a key's edges, strongest along the sides towards the LEDs (left, bottom). */
    private fun drawBleed(c: Canvas, k: RectF, r: Float, glow: Float, seed: Int) {
        val a = (glow * backlightDirt * 150).toInt().coerceIn(0, 255)
        if (a < 3) return
        val p = paint
        val d = resources.displayMetrics.density
        p.shader = null
        p.style = Paint.Style.STROKE
        p.strokeWidth = maxOf(1f, 1.2f * d)
        val col = (a shl 24) or backlightRgb
        p.setShadowLayer(3f * d, 0f, 0f, col)
        p.color = ((a * 2 / 5) shl 24) or backlightRgb
        c.drawRoundRect(k, r, r, p)
        p.color = col
        val reach = 0.35f + 0.65f * rnd(seed, 4)
        c.drawLine(k.left, k.bottom - r, k.left, k.bottom - (k.height() - r) * reach, p)
        c.drawLine(k.left + r, k.bottom, k.left + r + (k.width() - 2 * r) * reach, k.bottom, p)
        p.clearShadowLayer()
        p.style = Paint.Style.FILL
    }

    override fun onTouchEvent(e: MotionEvent): Boolean {
        if (dockTouch(e)) return true
        val idx = e.actionIndex
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN -> {
                val x = e.getX(idx)
                val y = e.getY(idx)
                val key = (if (lockTouch.contains(x, y)) lockKey else null)
                    ?: (if (specActive) specKeys.firstOrNull { it.rect.contains(x, y) } else null)
                    ?: (if (foldCrease != null && !specActive) foldSkin.keys.firstOrNull { it.rect.contains(x, y) } else null)
                    ?: softKeys.firstOrNull { !it.rect.isEmpty && it.rect.contains(x, y) } ?: dpadKeyAt(x, y)
                if (key == null) return e.actionMasked == MotionEvent.ACTION_POINTER_DOWN || pointerKey.isNotEmpty()
                pointerKey[e.getPointerId(idx)] = key
                wakeBacklight()
                if (!key.pressed) {
                    key.pressed = true
                    performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
                    if (key !== lockKey) onKey(key.vk, true)
                    else { lockLongFired = false; postDelayed(lockHold, 550) }
                }
                // The keys are drawn in their own layer; the backdrop below needs no redraw.
                keysView.invalidate()
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP ->
                release(e.getPointerId(idx), lockTouch.contains(e.getX(idx), e.getY(idx)))
            MotionEvent.ACTION_CANCEL -> pointerKey.keys.toList().forEach { release(it, false) }
        }
        return true
    }

    /** [tapped]: for the lock button, whether the finger lifted on it (otherwise nothing happens). */
    private fun release(pointer: Int, tapped: Boolean) {
        val key = pointerKey.remove(pointer) ?: return
        if (pointerKey.containsValue(key)) return
        key.pressed = false
        performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY_RELEASE)
        if (key === lockKey) {
            removeCallbacks(lockHold)
            if (tapped && !lockLongFired) onLock?.invoke()
        } else onKey(key.vk, false)
        keysView.invalidate()
    }

    /** Releases every key held on screen (e.g. when the app loses focus mid-press). */
    fun releaseAll() = pointerKey.keys.toList().forEach { release(it, false) }

    companion object {
        const val BACKDROP_BLACK = "black"
        const val BACKDROP_GRAY = "gray"
        const val BACKDROP_SILVER = "silver"
        const val BACKDROP_WHITE = "white"

        // Fold hinge styles (Settings.hingeStyle).
        const val HINGE_SHADOW = "shadow"
        const val HINGE_CLASSIC = "classic"
        const val HINGE_CREASE = "crease"
        const val HINGE_VOID = "void"

        private const val KIND_SOFT = 0
        private const val KIND_TALK = 1
        private const val KIND_END = 2
        private const val KIND_LEFT = 3
        private const val KIND_UP = 4
        private const val KIND_ACTION = 5
        private const val KIND_DOWN = 6
        private const val KIND_RIGHT = 7
        private const val KIND_LOCK = 8

        /** Eye distance in front of the screen, cm. */
        private const val EYE_CM = 32f

        /**
         * Virtual lights in the phone's frame when the keys first show (cm; x right, y up the
         * screen, z towards the viewer): two ceiling lights above and behind the viewer, and a
         * broad window light off to one side.
         */
        private val LIGHTS = arrayOf(
            floatArrayOf(-35f, 70f, 140f),
            floatArrayOf(55f, 40f, 180f),
            floatArrayOf(-120f, -20f, 90f),
        )
        /** Highlight radius per light, cm. */
        private val LIGHT_SIZE = floatArrayOf(1.6f, 2.2f, 4.5f)
        /** How strongly each light shows on a key (the broad window light is soft). */
        private val LIGHT_GAIN = floatArrayOf(1f, 0.8f, 0.45f)

        // Key row proportions, in window widths.
        private const val KEY_H = 0.064f
        private const val KEY_MARGIN = 0.035f
        private const val DPAD_H = 0.26f
        /** D-pad action button's side, as a share of the pad's. */
        private const val DPAD_CENTRE = 0.36f
        // Lock button: height, full width and the part of the width beyond the window's left edge.
        private const val LOCK_H = KEY_H * 1.45f
        private const val LOCK_W = 0.22f        // as long as a row key
        private const val LOCK_HIDDEN = 0.45f   // share of its height above the top edge
        /** Fold hinge: how far Location moves it at either end, in window heights. */
        private const val HINGE_SHIFT = 0.1f
        /** Classic hinge: knuckle splits along the bar, as shares of its length. */
        private val HINGE_KNUCKLES = floatArrayOf(0.2f, 0.8f)

        // Key backlight: off, lit, fading out (over BL_FADE_MS); unlit glyphs' opacity with it on.
        private const val BL_OFF = 0
        private const val BL_LIT = 1
        private const val BL_FADING = 2
        private const val BL_FADE_MS = 300L
        private const val BL_DIM = 0.45f
    }
}
