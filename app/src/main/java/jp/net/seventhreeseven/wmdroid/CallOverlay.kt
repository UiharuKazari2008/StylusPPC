package jp.net.seventhreeseven.wmdroid

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Color
import android.graphics.Outline
import android.graphics.PixelFormat
import android.graphics.drawable.GradientDrawable
import android.provider.Settings as AndroidSettings
import android.util.Log
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewOutlineProvider
import android.view.WindowManager
import android.widget.FrameLayout

/**
 * Windows Mobile's screen as a floating card over other apps while a call rings and PocketStylus
 * is in the background: the emulator draws into the card's surface (NativeBridge.setSurface), and
 * taps go to Windows Mobile, so its own incoming-call screen (Answer / Ignore) is the UI. When the
 * call is answered the card goes and the full emulator comes to the front (CallBridge).
 * Needs "Display over other apps" (Settings.canDrawOverlays); without it the phone's dialer keeps
 * the call as before.
 */
class CallOverlay(private val ctx: Context) {
    private var root: FrameLayout? = null
    private var handle = 0L
    private var guestW = 0
    private var guestH = 0
    private var rotated = false

    val showing get() = root != null

    @SuppressLint("ClickableViewAccessibility")
    fun show(handle: Long, guestW: Int, guestH: Int, rotated: Boolean): Boolean {
        if (root != null) return true
        if (handle == 0L || !AndroidSettings.canDrawOverlays(ctx)) return false
        this.handle = handle; this.guestW = guestW; this.guestH = guestH; this.rotated = rotated
        val wm = ctx.getSystemService(WindowManager::class.java)
        val dm = ctx.resources.displayMetrics
        // The frame as the guest shows it (turned a quarter in fold mode), 88 % of the screen's width.
        val (fw, fh) = if (rotated) guestH to guestW else guestW to guestH
        var w = (dm.widthPixels * 0.88f).toInt()
        var h = w * fh / fw
        if (h > dm.heightPixels * 0.85f) { h = (dm.heightPixels * 0.85f).toInt(); w = h * fw / fh }
        val pad = (8 * dm.density).toInt()
        val corner = 18 * dm.density
        val card = FrameLayout(ctx).apply {
            background = GradientDrawable().apply { setColor(Color.BLACK); cornerRadius = corner }
            setPadding(pad, pad, pad, pad)
            elevation = 24 * dm.density
            outlineProvider = object : ViewOutlineProvider() {
                override fun getOutline(view: View, outline: Outline) = outline.setRoundRect(0, 0, view.width, view.height, corner)
            }
            clipToOutline = true
        }
        val surface = SurfaceView(ctx)
        surface.holder.addCallback(object : SurfaceHolder.Callback {
            override fun surfaceCreated(holder: SurfaceHolder) { NativeBridge.setSurface(this@CallOverlay.handle, holder.surface) }
            override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}
            override fun surfaceDestroyed(holder: SurfaceHolder) { NativeBridge.setSurface(this@CallOverlay.handle, null) }
        })
        surface.setOnTouchListener { v, e -> touch(v, e) }
        card.addView(surface, FrameLayout.LayoutParams(w, h))
        val lp = WindowManager.LayoutParams(w + 2 * pad, h + 2 * pad, WindowManager.LayoutParams.TYPE_APPLICATION_OVERLAY,
            WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN or
                WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON, PixelFormat.OPAQUE).apply { gravity = Gravity.CENTER }
        return try {
            wm.addView(card, lp)
            root = card
            Log.i(TAG, "incoming call: Windows Mobile shown over other apps")
            true
        } catch (e: RuntimeException) {
            Log.w(TAG, "overlay refused", e)
            false
        }
    }

    /** Removes the card at once (its surface goes before the activity's takes over). */
    fun hide() {
        val r = root ?: return
        root = null
        runCatching { ctx.getSystemService(WindowManager::class.java).removeViewImmediate(r) }
        NativeBridge.setSurface(handle, null)
    }

    private fun touch(v: View, e: MotionEvent): Boolean {
        val u = (e.x / v.width).coerceIn(0f, 1f)
        val w = (e.y / v.height).coerceIn(0f, 1f)
        val (fx, fy) = if (rotated) w to 1f - u else u to w
        val x = (fx * guestW).toInt().coerceIn(0, guestW - 1)
        val y = (fy * guestH).toInt().coerceIn(0, guestH - 1)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> NativeBridge.touch(handle, true, x, y)
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> NativeBridge.touch(handle, false, x, y)
        }
        return true
    }

    companion object {
        private const val TAG = "WMDroidCall"

        /** "Ring Windows Mobile while PocketStylus is in the background" (Settings > Phone calls). */
        fun enabled(ctx: Context) = Settings.prefs(ctx).getBoolean("calls.background", true) && AndroidSettings.canDrawOverlays(ctx)
        fun setEnabled(ctx: Context, on: Boolean) = Settings.prefs(ctx).edit().putBoolean("calls.background", on).apply()
    }
}
