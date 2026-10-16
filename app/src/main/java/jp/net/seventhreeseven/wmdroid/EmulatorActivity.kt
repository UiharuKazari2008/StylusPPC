package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ActivityInfo
import android.content.pm.PackageManager
import android.graphics.Color
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.util.Log
import android.view.Gravity
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.WindowManager
import android.widget.FrameLayout
import android.widget.ImageButton
import android.widget.TextView
import android.widget.Toast
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import java.io.File
import kotlin.concurrent.thread

/** Runs the selected image full screen. */
class EmulatorActivity : AppCompatActivity(), SurfaceHolder.Callback, GuestBridge.Listener {
    private var handle = 0L
    private lateinit var surface: SurfaceView
    private lateinit var skin: SkinLayout
    private lateinit var rotation: GuestRotation
    private lateinit var stats: TextView
    private lateinit var settings: Settings
    private var guestW = 240
    private var guestH = 320
    private var booting = true
    private var askedMic = false
    private var micOn = false
    private var snapshotDir: File? = null
    private var saveThread: Thread? = null
    private val ui = Handler(Looper.getMainLooper())
    private val status by lazy { DeviceStatusBridge(this) { handle } }
    private lateinit var guest: GuestBridge
    private lateinit var bluetooth: BluetoothBridge
    private lateinit var calls: CallBridge  // phone calls from Windows Mobile's Phone app (CallBridge.kt)
    private lateinit var clipboard: ClipboardBridge  // "Sync Clipboard" (ClipboardBridge.kt)
    private lateinit var ime: ImeBridge  // "Use Android IME over Device IME" (ImeBridge.kt)
    private val phone by lazy { PhoneStatus(this) }
    private val gps by lazy { GpsBridge(this) { handle } }
    private val camera by lazy { CameraBridge(this, settings) { handle } }  // camera bridge
    private var askedLocation = false
    private var lastStatus = ""
    private var statusTicks = 0
    private var askedPhone = false
    private var askedNumber = false
    private var myNumber = ""  // PhoneNumber.own, read on resume and after permission answers
    private var guestReady = false
    private var wallpaperChecked = false
    private var bootCompleteAt = 0L

    // Power and background state (see leave()).
    private lateinit var powerMenu: PowerMenu
    private lateinit var pausedCover: View
    private var displayOffTicks = 0
    private var displayGraceUntil = 0L
    private var foreground = false
    /** Paused from the power menu; only Resume runs it again. */
    private var userPaused = false
    /** Paused by the background setting (or while our own settings screen is open). */
    private var suspended = false
    /** The session was saved to resume.wms and released; restored on return. */
    private var hibernated = false
    /** resume.wms holds the current state (the guest has not run since it was written). */
    private var resumeSaved = false
    /** Our own settings screen is opening: not a real departure. */
    private var ownUi = false
    /** A lock went to the guest since the app was last on screen. */
    private var lockSent = false
    /** Running in the background under BackgroundService. */
    private var serviceOn = false
    private var askedNotify = false
    private var pendingLeave: Runnable? = null
    /** Elapsed-realtime deadline of the "after X minutes" setting, 0 if none. */
    private var bgDeadline = 0L

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        settings = Settings(this)
        settings.migrate()
        guest = GuestBridge(this, settings, this)
        bluetooth = BluetoothBridge(this, settings, guest, ui).apply { start() }
        calls = CallBridge(this, guest) { handle }
        WindowCompat.setDecorFitsSystemWindows(window, false)
        // The phone's keyboard (fold mode) overlays the keyboard half; nothing moves.
        window.setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING)
        WindowInsetsControllerCompat(window, window.decorView).apply {
            hide(WindowInsetsCompat.Type.systemBars())
            systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        }

        surface = SurfaceView(this)
        surface.holder.addCallback(this)
        stats = TextView(this).apply {
            setTextColor(Color.YELLOW)
            setShadowLayer(3f, 0f, 0f, Color.BLACK)
            textSize = 11f
            isSingleLine = true
        }
        pausedCover = PowerMenu.pausedCover(this) { setUserPaused(false) }
        val menuLayer = FrameLayout(this)
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        skin = SkinLayout(this, surface) { vk, down -> sendKey(vk, down) }.apply {
            keysOn = settings.keysOn
            dpadOn = settings.dpadOn
            setSoftKeys(settings.softKeyVks)
            shineBrightness = settings.shineBrightness
            shineWidth = settings.shineWidth
            hingeOn = settings.hingeOn
            hingeStyle = settings.hingeStyle
            hingeThickness = settings.hingeThickness
            hingeLocation = settings.hingeLocation
            hingeOpacity = settings.hingeOpacity
            setBacklight(settings.backlightOn, settings.backlightIntensity, settings.backlightTimeout, settings.backlightColor, settings.backlightDirt)
            spec =SkinSpec.load(this@EmulatorActivity, settings)
            backdrop = settings.backdrop
            wallpaperOn = settings.showWallpaper
            backdropOpacity = settings.backdropOpacity / 100f
            carrier = settings.skinCarrier
            onLock = { if (handle != 0L) guest.send("lock") }
            onLockLong = { showPowerMenu() }
            // Full width and one line: new text each second then needs no layout pass.
            addView(stats, FrameLayout.LayoutParams(-1, -2, Gravity.TOP or Gravity.START))
            addView(pausedCover, FrameLayout.LayoutParams(-1, -1))
            addView(menuLayer, FrameLayout.LayoutParams(-1, -1))
            // The performance overlay starts right of the lock button.
            // (only when the lock button is on the left; a skin may put it on the right)
            addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ ->
                stats.translationX = if (lockRect.centerX() < width / 2f) lockRight else 0f
            }
        }
        setContentView(skin)
        clipboard = ClipboardBridge(this, settings, guest)
        ime = ImeBridge(this, settings, guest, skin)  // adds its hidden editor view to the skin
        powerMenu = PowerMenu(this, menuLayer) { skin.lockRect }
        val menuBack = object : OnBackPressedCallback(false) {
            override fun handleOnBackPressed() = powerMenu.close()
        }
        powerMenu.onOpenChanged = { menuBack.isEnabled = it }
        onBackPressedDispatcher.addCallback(this, menuBack)
        BackgroundService.onSuspend = notificationSuspend
        val screenFilter = IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_OFF)
            addAction(Intent.ACTION_SCREEN_ON)
        }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(screenReceiver, screenFilter, Context.RECEIVER_NOT_EXPORTED)
        else registerReceiver(screenReceiver, screenFilter)
        if (settings.showWallpaper) loadWallpaper()
        rotation = GuestRotation(this, { settings.rotationDelayMs }, { settings.rotationSensitivity }) { boost(); guest.send("rotate $it") }
        surface.setOnTouchListener { v, e -> onGuestTouch(v, e) }
        boot()
    }

    /**
     * With "phone screen shape" on, picks the installed image of this release whose native screen
     * best matches the window's shape (WM only has layouts for fixed sizes, e.g. 480x854 tall or
     * 480x480 square), preferring higher resolutions. Otherwise the chosen image is used.
     */
    /** Height/width of the phone's display area, from chooseVariant (0 = not in phone-shape mode). */
    private var phoneAspect = 0.0

    private fun chooseVariant(profile: OsProfile, chosen: ImageVariant): ImageVariant {
        if (settings.screenMode != Settings.SCREEN_PHONE) return chosen
        val (ww, wh) = if (Build.VERSION.SDK_INT >= 30) {
            val b = windowManager.currentWindowMetrics.bounds
            b.width() to b.height()
        } else {
            resources.displayMetrics.widthPixels to resources.displayMetrics.heightPixels
        }
        // The cover screen decides the resolution: on a squarish (open foldable) window use the
        // aspect remembered from the cover screen, else measure it and remember it.
        val tall = wh.toFloat() / ww >= 1.6f
        val target = if (tall) skin.displayAspect(ww, wh).also { settings.coverAspect = it.toFloat() }
        else settings.coverAspect.takeIf { it > 0f }?.toDouble() ?: (854.0 / 480.0)
        phoneAspect = target
        val installed = ImageStore.available(this, profile)
        return installed.minWithOrNull(compareBy<ImageVariant>(
            { Math.abs(Math.log((it.height.toDouble() / it.width) / target)) },
            { -it.width * it.height },
        )) ?: chosen
    }

    private fun boot() {
        val profile = settings.profile
        val variant = settings.variant?.let { v -> profile?.let { chooseVariant(it, v) } ?: v }
        val file = if (profile != null && variant != null) ImageStore.find(this, profile, variant) else null
        if (profile == null || variant == null || file == null) {
            startActivity(Intent(this, SetupActivity::class.java))
            finish()
            return
        }
        val w = variant.width
        var h = variant.height
        // Phone shape, but this release has no image near it (WM 5: VGA/QVGA only): the board tells
        // the OS its screen size, so keep the image's width and make it as tall as the phone.
        // Every WM from 5 to 6.5 runs at the phone's height (checked in the CLI); within 2% the
        // image's own size stays (WM 6.5's 480x854 keeps its state).
        val aspect = phoneAspect
        if (aspect > 1.0 && variant.height >= variant.width &&
            Math.abs(Math.log((variant.height.toDouble() / variant.width) / aspect)) > 0.02) {
            h = (Math.round(w * aspect).toInt() / 2 * 2).coerceIn(w, w * 2)
            Log.i(TAG, "screen ${variant.width}x${variant.height} -> ${w}x$h for the phone's shape")
        }
        guestW = w
        guestH = h
        val mem = settings.effectiveMemMB()
        val dir = File(filesDir, "states/${profile.id}-${variant.id}-$mem-${w}x$h").apply { mkdirs() }
        snapshotDir = dir

        fun create(): Long {
            val hd = NativeBridge.create(file.path, mem, profile.armv5, w, h)
            if (hd != 0L) {
                val mounts = StorageBridge.mounts(this, settings)
                NativeBridge.setShareMounts(hd, mounts.map { it.first }.toTypedArray(), mounts.map { it.second }.toTypedArray())
                NativeBridge.setFilter(hd, settings.screenFilter)
                NativeBridge.setRotation(hd, if (foldGuest) 1 else 0)
                NativeBridge.setAudioGain(hd, Math.pow(10.0, settings.audioBoostDb / 20.0).toFloat())
                val overlays = guest.prepare()
                NativeBridge.setShareOverlays(hd, overlays.map { it.first }.toTypedArray(), overlays.map { it.second }.toTypedArray())
            }
            return hd
        }
        handle = create()
        if (handle == 0L) {
            Toast.makeText(this, "Could not load ${file.name}", Toast.LENGTH_LONG).show()
            return
        }
        // Quick boot: continue the last session, else start from the saved booted state.
        val candidates = buildList {
            if (settings.restoresSession) add(File(dir, RESUME))
            add(File(dir, BOOT))
        }
        for (snap in candidates) {
            if (!snap.exists()) continue
            if (NativeBridge.loadState(handle, snap.path)) {
                Log.i(TAG, "restored ${snap.name}")
                booting = !NativeBridge.bootComplete(handle)
                break
            }
            snap.delete()
            NativeBridge.destroy(handle)  // a failed restore leaves the machine half-written
            handle = create()
        }
        title = profile.title
        // Keep the phone awake while Windows Mobile boots; released once it reaches the home screen.
        if (booting) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        skin.setGuestSize(w, h)
    }

    private fun onGuestTouch(v: View, e: MotionEvent): Boolean {
        if (handle == 0L) return false
        if (e.actionMasked == MotionEvent.ACTION_DOWN) skin.wakeBacklight()
        val u = (e.x / v.width).coerceIn(0f, 1f)
        val w = (e.y / v.height).coerceIn(0f, 1f)
        // Fold mode shows the frame turned a quarter clockwise: the panel's x runs down the view,
        // its y from right to left (the touch panel reports unrotated coordinates).
        val (fx, fy) = if (foldGuest) w to 1f - u else u to w
        val x = (fx * guestW).toInt().coerceIn(0, guestW - 1)
        val y = (fy * guestH).toInt().coerceIn(0, guestH - 1)
        when (e.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_MOVE -> NativeBridge.touch(handle, true, x, y)
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> NativeBridge.touch(handle, false, x, y)
        }
        return true
    }

    private fun micGranted() = checkSelfPermission(Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED

    /** Runs the phone's GPS only while Windows Mobile has its GPS port open. */
    private fun updateGps() {
        val wanted = settings.gpsOn && handle != 0L && NativeBridge.serialOpen(handle)
        if (wanted && !gps.hasPermission()) {
            if (!askedLocation) {
                askedLocation = true
                requestPermissions(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION), REQ_LOCATION)
            }
            return
        }
        gps.setActive(wanted)
    }

    /** Opens the phone's microphone only while the guest records. */
    private fun updateMic() {
        val wanted = handle != 0L && NativeBridge.micWanted(handle)
        if (wanted && !micOn) {
            if (micGranted()) {
                NativeBridge.setMicEnabled(handle, true)
                micOn = true
            } else if (!askedMic) {
                askedMic = true
                requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_MIC)
            }
        } else if (!wanted && micOn) {
            NativeBridge.setMicEnabled(handle, false)
            micOn = false
        }
    }

    // ---- Notification light and vibrator ----

    private val vibrator by lazy {
        if (Build.VERSION.SDK_INT >= 31) getSystemService(android.os.VibratorManager::class.java)?.defaultVibrator
        else @Suppress("DEPRECATION") getSystemService(android.os.Vibrator::class.java)
    }

    /**
     * Polls Windows Mobile's LED device four times a second (also in the background, so a
     * notification still vibrates the phone): the light goes to the skin's dot, the vibrator
     * to the phone's, a quarter second at a time while it stays on.
     */
    private val ledPoll = object : Runnable {
        override fun run() {
            val h = handle
            // Stops while the guest is stopped (suspended, paused, hibernated); startGuest restarts it.
            if (h == 0L || suspended || userPaused || hibernated) return
            val s = NativeBridge.ledState(h)
            skin.alertLight = s and (NativeBridge.LED_LIGHT or NativeBridge.LED_BLINK)
            if (s and NativeBridge.LED_VIBRATE != 0) runCatching {
                vibrator?.vibrate(android.os.VibrationEffect.createOneShot(250, android.os.VibrationEffect.DEFAULT_AMPLITUDE))
            }
            ui.postDelayed(this, 250)
        }
    }

    private fun startLedPoll() {
        ui.removeCallbacks(ledPoll)
        ui.post(ledPoll)
    }

    /**
     * The home-screen wallpaper of the screen the app is on. A foldable keeps one per screen (the
     * cover's tall one, the inner's squarer one); the plain getter can hand back the other screen's,
     * which then looks zoomed in once cropped. Of all the system wallpapers on offer, the one whose
     * shape is closest to this screen's.
     */
    private var wallpaperThumbsSaved = false

    /**
     * The phone's wallpaper. [forW] x [forH] (either way round): the screen the picture is for. One UI
     * keeps one picture per screen sized like it, and while the phone folds or opens the "current"
     * picture switches before this window gets its new size, so a cut made then took the other
     * screen's picture; the size match picks the right one regardless.
     */
    private fun phoneWallpaper(forW: Int = 0, forH: Int = 0): android.graphics.drawable.Drawable? {
        val wm = android.app.WallpaperManager.getInstance(this)
        val b = if (Build.VERSION.SDK_INT >= 30) windowManager.currentWindowMetrics.bounds
        else android.graphics.Rect(0, 0, resources.displayMetrics.widthPixels, resources.displayMetrics.heightPixels)
        // Width over height, so a landscape window gets the picture One UI made for landscape (the
        // inner screen's square one) rather than a portrait one cut down to a strip.
        val screen = b.width().toFloat() / maxOf(1, b.height())
        val found = ArrayList<android.graphics.drawable.Drawable>()
        runCatching { wm.drawable }.getOrNull()?.let { found += it }
        // FLAG_SYSTEM with the display bits One UI uses for its per-screen wallpapers.
        if (Build.VERSION.SDK_INT >= 34) for (which in intArrayOf(1, 5, 9, 17, 33))
            runCatching { wm.getDrawable(which) }.getOrNull()?.let { found += it }
        fun shape(d: android.graphics.drawable.Drawable) =
            d.intrinsicWidth.coerceAtLeast(1).toFloat() / d.intrinsicHeight.coerceAtLeast(1)
        // The current screen's own picture (what the system draws there); the shape match is only a
        // fallback for when that one can't be read.
        fun fits(d: android.graphics.drawable.Drawable) = forW > 0 &&
            minOf(d.intrinsicWidth, d.intrinsicHeight) == minOf(forW, forH) && maxOf(d.intrinsicWidth, d.intrinsicHeight) == maxOf(forW, forH)
        val best = found.firstOrNull { fits(it) } ?: runCatching { wm.drawable }.getOrNull()
            ?: found.minByOrNull { Math.abs(Math.log((shape(it) / screen).toDouble())) }
        if (!wallpaperThumbsSaved) {
            // Diagnostics: small copies of every candidate (cache/wallpapers), to tell One UI's
            // per-screen pictures apart.
            wallpaperThumbsSaved = true
            runCatching {
                val dir = File(cacheDir, "wallpapers").apply { mkdirs() }
                found.forEachIndexed { i, d ->
                    val w = d.intrinsicWidth.coerceAtLeast(1)
                    val h = d.intrinsicHeight.coerceAtLeast(1)
                    val k = 320f / maxOf(w, h)
                    val bmp = android.graphics.Bitmap.createBitmap((w * k).toInt().coerceAtLeast(1), (h * k).toInt().coerceAtLeast(1),
                        android.graphics.Bitmap.Config.ARGB_8888)
                    d.setBounds(0, 0, bmp.width, bmp.height)
                    d.draw(android.graphics.Canvas(bmp))
                    File(dir, "$i-${w}x$h.png").outputStream().use { bmp.compress(android.graphics.Bitmap.CompressFormat.PNG, 90, it) }
                }
            }
        }
        Log.i(TAG, "wallpaper: ${found.joinToString { "${it.intrinsicWidth}x${it.intrinsicHeight}" }} for " +
            "${b.width()}x${b.height()} -> ${best?.intrinsicWidth}x${best?.intrinsicHeight}")
        return best
    }

    /** Layout switch or rotation: let the emulation thread run flat out while the guest redraws. */
    private fun boost() { if (handle != 0L) NativeBridge.boost(handle, 4000) }

    private var lastScreenDp = ""

    /**
     * The app handles screen changes itself (opening the phone, rotating). Fold changes arrive
     * before the window has its new size, so the phone wallpaper picked then was the old screen's:
     * pick it again once the size has really changed, and recut the guest's picture.
     */
    override fun onConfigurationChanged(newConfig: android.content.res.Configuration) {
        super.onConfigurationChanged(newConfig)
        val dp = "${newConfig.screenWidthDp}x${newConfig.screenHeightDp}"
        if (dp == lastScreenDp) return
        lastScreenDp = dp
        if (::settings.isInitialized && settings.showWallpaper) loadWallpaper()
        wallpaperChecked = false
    }

    // ---- Foldable: open = landscape slider on top, keyboard below ----

    private val layoutTracker by lazy {
        androidx.window.java.layout.WindowInfoTrackerCallbackAdapter(androidx.window.layout.WindowInfoTracker.getOrCreate(this))
    }
    private val layoutListener = androidx.core.util.Consumer<androidx.window.layout.WindowLayoutInfo> { onWindowLayout(it) }
    /** Windows Mobile is turned to landscape for the open-fold layout. */
    private var foldGuest = false

    private var lastLayoutInfo: androidx.window.layout.WindowLayoutInfo? = null

    private fun onWindowLayout(info: androidx.window.layout.WindowLayoutInfo) {
        lastLayoutInfo = info
        val fold = info.displayFeatures.filterIsInstance<androidx.window.layout.FoldingFeature>().firstOrNull()
        val open = settings.foldMode && fold != null
        if (!open) {
            skin.foldCrease = null
            requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            if (foldGuest) {
                foldGuest = false
                boost()
                if (handle != 0L) NativeBridge.setRotation(handle, 0)
                camera.panelRotation = 0  // the camera picture follows the panel, not WM's rotation
                guest.send("rotate 0")
                if (settings.showWallpaper) loadWallpaper()  // each screen can have its own wallpaper
                if (settings.rotateGuest && foreground) rotation.start()
                wallpaperChecked = false
            }
            return
        }
        // Fixed landscape (not sensor): the backdrop wallpaper and the guest's stay the right way up.
        requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
        if (!foldGuest) {
            foldGuest = true
            boost()
            rotation.stop()
            if (handle != 0L) NativeBridge.setRotation(handle, 1)
            camera.panelRotation = 90  // setRotation(1): the panel is drawn a quarter turn clockwise
            // Folding rewrote the landscape picture with the cover's (for turning WM on the cover):
            // put the open screen's picture back first, so WM turns straight onto it.
            runCatching {
                val open = guest.boxFile(OPEN_WALL)
                if (open.exists()) for (name in listOf("wall_landscape_90.jpg", "wall_landscape.jpg"))
                    open.copyTo(guest.boxFile(name), overwrite = true)
            }
            guest.send("rotate 90")
            if (settings.showWallpaper) loadWallpaper()
            wallpaperChecked = false
        }
        skin.foldKeyboard.useIme = settings.foldKeyboard == "ime"
        skin.requestLayout()  // the skin may have its own layout for the phone's keyboard
        skin.foldKeyboard.hotkeys = settings.foldHotkeys
        // Lay out once the window has turned (the fold then runs across it).
        if (fold!!.orientation == androidx.window.layout.FoldingFeature.Orientation.HORIZONTAL) {
            val loc = IntArray(2)
            skin.getLocationInWindow(loc)
            val crease = android.graphics.Rect(fold.bounds).apply { offset(-loc[0], -loc[1]) }
            if (crease != skin.foldCrease) {
                skin.foldCrease = crease
                wallpaperChecked = false  // re-cut the guest's picture for the new layout
            }
            if (settings.foldKeyboard == "ime") skin.post { skin.foldKeyboard.showIme() }
        }
    }

    /**
     * Mirrors Windows Mobile turning its display off by locking the phone (Android turns its screen
     * off). Unlocking brings the app back, which presses Windows Mobile's power button to wake it.
     */
    private fun updateDisplayOff() {
        val off = settings.mirrorDisplayOff && foreground && handle != 0L && !booting && !userPaused &&
            !NativeBridge.displayOn(handle)
        displayOffTicks = if (off) displayOffTicks + 1 else 0
        // Off for two ticks in a row, and not right after we woke it.
        if (displayOffTicks == 2 && android.os.SystemClock.uptimeMillis() > displayGraceUntil) LockAdmin.lock(this)
    }

    /**
     * The app is on screen again (reopened, back on top, or the phone unlocked) while Windows
     * Mobile's display is off: press its power button so it wakes with the phone.
     */
    private fun wakeGuestDisplay() {
        if (handle == 0L || booting || userPaused) return
        if (NativeBridge.guestAsleep(handle)) {
            // In standby nothing in Windows Mobile runs (the helper's commands wait): the power
            // button is the one thing that wakes it.
            NativeBridge.powerButton(handle)
            displayGraceUntil = android.os.SystemClock.uptimeMillis() + 15_000
            return
        }
        if (NativeBridge.displayOn(handle)) return
        // User activity (a Shift tap) turns a timed-out display back on. Not the power button:
        // when the display had only timed out, that press would suspend Windows Mobile instead.
        NativeBridge.key(handle, 0x10, true)
        NativeBridge.key(handle, 0x10, false)
        displayGraceUntil = android.os.SystemClock.uptimeMillis() + 15_000
    }

    private val tick = object : Runnable {
        override fun run() {
            if (handle != 0L) {
                val size = NativeBridge.screenSize(handle)
                if (size[0] > 1 && (size[0] != guestW || size[1] != guestH)) {
                    guestW = size[0]; guestH = size[1]; skin.setGuestSize(guestW, guestH)
                }
                stats.text = if (settings.showStats) NativeBridge.status(handle) else ""
                updateMic()
                updateGps()
                checkHung()
                guest.poll()  // first: what the helper said while we were away counts before checkHelper
                checkHelper()
                updateDisplayOff()
                // Cut the guest's picture only once the window has settled into the matching layout
                // (right after folding or unfolding it is still the other shape for a moment).
                val settled = if (foldGuest) skin.foldCrease != null && skin.width > skin.height
                else skin.foldCrease == null && skin.height > skin.width
                if (!wallpaperChecked && settled && !skin.displayRect().isEmpty) {
                    wallpaperChecked = true
                    writeDeviceWallpaper()
                }
                calls.pollModem()  // calls dialled through the virtual modem (WM 6 / 6.1 / 6.1.4)
                guest.syncTimeZone()
                // Mirror the phone's radio/Wi-Fi state into the guest every few seconds (on change).
                if (statusTicks++ % 3 == 0) {
                    val snap = phone.read()
                    if (settings.skinCarrier.isBlank()) skin.carrier = if (snap.service) snap.operator else ""
                    val line = snap.encode()
                    if (line != lastStatus) { lastStatus = line; guest.send(line) }
                    // WM 6 / 6.1 / 6.1.4: the same state for the RIL, through the virtual modem on COM1.
                    NativeBridge.modemNetwork(handle, snap.service, snap.signal, snap.roaming, snap.operator, snap.numeric, snap.data, myNumber)
                }
                if (booting && NativeBridge.bootComplete(handle)) {
                    booting = false
                    bootCompleteAt = System.nanoTime()
                    window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
                }
                // The saved boot state is the patched system: wait for the helper to report it,
                // or give up waiting on releases the helper cannot run on.
                val boot = File(snapshotDir, BOOT)
                if (!booting && !boot.exists() && (guestReady ||
                        (bootCompleteAt != 0L && System.nanoTime() - bootCompleteAt > 120_000_000_000L))) {
                    saveSnapshot(boot, resumeAfter = true)
                }
            }
            ui.postDelayed(this, 1000)
        }
    }

    /**
     * Pauses the guest, writes a snapshot in the background and optionally resumes (unless
     * something else now holds it paused); [then] runs on the UI thread with the result.
     */
    private fun saveSnapshot(file: File, resumeAfter: Boolean, then: ((Boolean) -> Unit)? = null) {
        val h = handle
        if (h == 0L) return
        saveThread?.join()
        NativeBridge.pause(h)
        saveThread = thread(name = "snapshot") {
            val ok = NativeBridge.saveState(h, file.path)
            Log.i(TAG, "snapshot ${file.name}: ${if (ok) "saved" else "failed"}")
            ui.post {
                if (resumeAfter && handle == h && shouldRun()) {
                    NativeBridge.start(h)
                    resumeSaved = false
                }
                then?.invoke(ok)
            }
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (requestCode == REQ_MIC) updateMic()
        if (requestCode == REQ_PHONE) { phone.stop(); phone.start(); lastStatus = "" }
        myNumber = PhoneNumber.own(this)
        calls.onPermissionsResult(requestCode)  // CALL_PHONE asked for during a WM dial
    }

    override fun onStart() {
        super.onStart()
        foreground = true
        ownUi = false
        lockSent = false
        suspended = false
        cancelLeave()
        cancelBackgroundTimer()
        releaseService()
        saveThread?.join()
        if (hibernated) wake()
        if (handle != 0L) NativeBridge.setCpuLimit(handle, 100)
        layoutTracker.addWindowLayoutInfoListener(this, mainExecutor, layoutListener)
        // The guest itself starts in onResume, after any settings result (which may recreate).
    }

    override fun onResume() {
        super.onResume()
        ownUi = false  // a Bluetooth share shown over this window (no onStop) is over
        startGuest()
        wakeGuestDisplay()
        status.start()
        calls.resume()  // call link, call-state resync, fast mailbox polling during calls
        clipboard.resume()  // clipboard sync while on screen
        ime.resume()  // the helper watches WM's SIP while on screen
        camera.start()  // the phone camera opens only while the guest uses its camera
        // Phone state, and notifications for running in the background, asked for together.
        val wanted = buildList {
            if (!phone.hasPhonePermission() && !askedPhone) {
                askedPhone = true
                add(Manifest.permission.READ_PHONE_STATE)
            }
            // The phone's own number for Windows Mobile's SIM: only asked for with calls on.
            if (CallBridge.enabled(this@EmulatorActivity) && !PhoneNumber.hasPermission(this@EmulatorActivity) && !askedNumber) {
                askedNumber = true
                add(PhoneNumber.PERMISSION)
            }
            if (Build.VERSION.SDK_INT >= 33 && settings.runsInBackground && !askedNotify &&
                checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                askedNotify = true
                add(Manifest.permission.POST_NOTIFICATIONS)
            }
        }
        if (wanted.isNotEmpty()) requestPermissions(wanted.toTypedArray(), REQ_PHONE)
        phone.start()
        myNumber = PhoneNumber.own(this)
        lastStatus = ""
        if (settings.rotateGuest && !foldGuest) rotation.start()
        if (settings.keyShine) skin.startTilt()
        wallpaperChecked = false
        contentResolver.registerContentObserver(
            android.provider.Settings.System.getUriFor(android.provider.Settings.System.SCREEN_BRIGHTNESS), false, brightnessObserver)
        sendBrightness()
        syncTheme()
        if (Build.VERSION.SDK_INT >= 27) runCatching {
            android.app.WallpaperManager.getInstance(this).addOnColorsChangedListener(wallpaperListener, ui)
        }
        ui.post(tick)
    }

    override fun onPause() {
        ui.removeCallbacks(tick)
        powerMenu.close()
        rotation.stop()
        gps.setActive(false)
        camera.stop()
        skin.stopTilt()
        skin.releaseAll()
        status.stop()
        calls.pause()
        clipboard.pause()
        ime.pause()
        phone.stop()
        contentResolver.unregisterContentObserver(brightnessObserver)
        if (Build.VERSION.SDK_INT >= 27) runCatching {
            android.app.WallpaperManager.getInstance(this).removeOnColorsChangedListener(wallpaperListener)
        }
        if (handle != 0L && micOn) {
            NativeBridge.setMicEnabled(handle, false)
            micOn = false
        }
        // The guest keeps running while merely paused (e.g. under a permission dialog); leaving
        // the screen is handled in onStop.
        super.onPause()
    }

    override fun onStop() {
        layoutTracker.removeWindowLayoutInfoListener(layoutListener)
        foreground = false
        calls.leave()  // WM can't show a call from the background: the phone's dialer takes it
        leave()
        super.onStop()
    }

    override fun onDestroy() {
        cancelLeave()
        cancelBackgroundTimer()
        releaseService()
        bluetooth.stop()
        calls.stop()
        unregisterReceiver(screenReceiver)
        if (BackgroundService.onSuspend === notificationSuspend) BackgroundService.onSuspend = null
        saveThread?.join()
        if (handle != 0L) {
            // Ended while still running in the background (e.g. swiped away): save it first.
            if (!suspended && settings.restoresSession && !booting) {
                NativeBridge.pause(handle)
                NativeBridge.saveState(handle, File(snapshotDir, RESUME).path)
            }
            NativeBridge.destroy(handle)
        }
        handle = 0L
        super.onDestroy()
    }

    // ---- Leaving the screen: lock, run on stepped down, suspend or hibernate ----

    /** Running is wanted: a session exists and nothing holds it paused. */
    private fun shouldRun() = handle != 0L && !userPaused && !suspended && !isFinishing

    /** Runs the guest unless something holds it paused. */
    private fun startGuest() {
        saveThread?.join()
        if (!shouldRun()) return
        NativeBridge.start(handle)
        guest.markAlive()  // the helper was stopped with the guest (or unheard in the background)
        startLedPoll()
        resumeSaved = false
    }

    /**
     * The app left the screen. Closing, recreating or opening our own settings suspends as
     * before. Otherwise, after an optional lock, the background setting decides: run on at a
     * stepped-down CPU share (for good, or for a while), suspend, or hibernate.
     */
    private fun leave() {
        if (handle == 0L) return
        if (isFinishing || isChangingConfigurations || ownUi) {
            // Closing: the lock waits in the mailbox and acts when the saved session resumes.
            if (isFinishing && settings.lockOnLeave && !userPaused && settings.restoresSession && !booting) sendLock()
            suspendSession()
            return
        }
        val screenOff = getSystemService(PowerManager::class.java)?.isInteractive == false
        if (!userPaused && (settings.lockOnLeave || (screenOff && settings.lockOnScreenOff))) sendLock()
        val mode = settings.backgroundMode
        if (!userPaused && settings.runsInBackground && runInBackground(screenOff)) return
        val hibernate = mode == Settings.BG_HIBERNATE || (mode == Settings.BG_HIBERNATE_AFTER && !userPaused)
        afterLock { if (hibernate) hibernate() else suspendSession() }
    }

    private fun sendLock() {
        if (lockSent || handle == 0L) return
        lockSent = true
        guest.send("lock")
    }

    /** Runs [action] now, or once the guest has had time to act on a lock just sent (it polls twice a second). */
    private fun afterLock(action: () -> Unit) {
        if (!lockSent) { action(); return }
        val r = Runnable { pendingLeave = null; action() }
        pendingLeave = r
        ui.postDelayed(r, LOCK_GRACE_MS)
    }

    private fun cancelLeave() {
        pendingLeave?.let { ui.removeCallbacks(it) }
        pendingLeave = null
    }

    /** Keeps running under the foreground service with the CPU share stepped down; false if refused. */
    private fun runInBackground(screenOff: Boolean): Boolean {
        if (!serviceOn) serviceOn = BackgroundService.start(this)
        if (!serviceOn) return false
        NativeBridge.setCpuLimit(handle, backgroundCpu(screenOff))
        if (settings.backgroundMode != Settings.BG_RUN) {
            val ms = settings.backgroundMinutes * 60_000L
            bgDeadline = SystemClock.elapsedRealtime() + ms
            ui.postDelayed(backgroundTimeout, ms)
        }
        return true
    }

    /**
     * CPU share in the background. Run in background (unrestricted) steps it down (less with the
     * screen off; sound playing is exempt in the core). Under a timer ("then suspend / hibernate")
     * it is not treated as background yet: full speed until the timer ends.
     */
    private fun backgroundCpu(screenOff: Boolean): Int =
        if (settings.backgroundMode != Settings.BG_RUN) 100 else if (screenOff) CPU_SCREEN_OFF else CPU_BACKGROUND

    /** "After X minutes": the handler's clock stops in deep sleep, so the screen turning on checks too. */
    private val backgroundTimeout = Runnable {
        bgDeadline = 0L
        if (!foreground) {
            if (settings.backgroundMode == Settings.BG_HIBERNATE_AFTER) hibernate() else suspendSession()
        }
    }

    private fun cancelBackgroundTimer() {
        ui.removeCallbacks(backgroundTimeout)
        bgDeadline = 0L
    }

    private fun releaseService() {
        if (!serviceOn) return
        serviceOn = false
        BackgroundService.stop()
    }

    /** The notification's Suspend (also used if the service could not go foreground). */
    private val notificationSuspend: () -> Unit = { if (!foreground) suspendSession() else releaseService() }

    /** Locks on screen-off when asked, and steps the background CPU share down further while it is off. */
    private val screenReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, intent: Intent) {
            val off = intent.action == Intent.ACTION_SCREEN_OFF
            if (off && settings.lockOnScreenOff && shouldRun()) sendLock()
            if (serviceOn && handle != 0L) NativeBridge.setCpuLimit(handle, backgroundCpu(off))
            if (!off && bgDeadline != 0L && SystemClock.elapsedRealtime() >= bgDeadline) {
                ui.removeCallbacks(backgroundTimeout)
                backgroundTimeout.run()
            }
        }
    }

    /**
     * Suspend: paused in memory. With resume on (or a hibernate mode) the session is also saved,
     * in case Android ends the app meanwhile; the service is let go once that is written.
     */
    private fun suspendSession() {
        cancelBackgroundTimer()
        if (handle == 0L) { releaseService(); return }
        suspended = true
        if (settings.restoresSession && !booting && !resumeSaved) saveResume { releaseService() }
        else {
            if (saveThread?.isAlive != true) NativeBridge.pause(handle)
            releaseService()
        }
    }

    /** Hibernate: save to resume.wms and release the native session and its memory. */
    private fun hibernate() {
        cancelBackgroundTimer()
        val h = handle
        if (h == 0L) { releaseService(); return }
        if (booting) { suspendSession(); return }  // a half-booted system is not worth keeping
        suspended = true
        val release = {
            if (handle == h && !foreground) {
                NativeBridge.destroy(h)
                handle = 0L
                hibernated = true
                Log.i(TAG, "hibernated")
            }
            releaseService()
        }
        if (resumeSaved) {
            saveThread?.join()
            NativeBridge.pause(h)
            release()
        } else saveResume { ok -> if (ok) release() else releaseService() }
    }

    private fun saveResume(then: (Boolean) -> Unit) {
        // Never keep a crashed Windows Mobile as the resume point: drop it and start clean next time.
        if (handle != 0L && NativeBridge.guestHung(handle)) {
            snapshotDir?.let { File(it, RESUME).delete() }
            then(false)
            return
        }
        resumeSaved = true
        saveSnapshot(File(snapshotDir, RESUME), resumeAfter = false) { ok ->
            if (!ok) resumeSaved = false
            then(ok)
        }
    }

    /** Back from hibernation: a new session from resume.wms (the surface is attached when it is created). */
    private fun wake() {
        hibernated = false
        booting = true
        bootCompleteAt = 0L
        micOn = false
        boot()
        if (handle != 0L && surface.holder.surface.isValid) NativeBridge.setSurface(handle, surface.holder.surface)
    }

    // ---- Power menu (long-press on the settings button) ----

    private fun showPowerMenu() {
        powerMenu.show(listOfNotNull(
            PowerMenu.Item("Savepoint", "Save the session now; PocketStylus resumes from here next time") {
                if (handle == 0L || booting) return@Item
                // Written as the resume point: the next start (or a hibernate) continues from here.
                saveSnapshot(File(snapshotDir, RESUME), resumeAfter = true) { ok ->
                    Toast.makeText(this, if (ok) "Savepoint written" else "Couldn't write the savepoint", Toast.LENGTH_SHORT).show()
                }
            },
            // WM 5's standby loses a kernel page on power-off and never wakes: no Suspend there.
            if (settings.profileId == "wm5ppcphone") null
            else PowerMenu.Item("Suspend", "Windows Mobile's power button (again to wake)") {
                if (userPaused) setUserPaused(false)
                if (handle != 0L) NativeBridge.powerButton(handle)
            },
            if (userPaused) PowerMenu.Item("Resume") { setUserPaused(false) }
            else PowerMenu.Item("Pause", "Stop the emulation until resumed") { setUserPaused(true) },
            PowerMenu.Item("Reset…", "Restart Windows Mobile (soft reset)",
                confirm = Triple("Reset Windows Mobile?", "Unsaved work in open programs is lost.", "Reset")) {
                if (userPaused) setUserPaused(false)
                if (handle != 0L) NativeBridge.softReset(handle)
                resumeSaved = false
            },
            PowerMenu.Item("Cold boot…", "Start Windows Mobile from scratch, like a new device", warn = true,
                confirm = Triple("Cold boot Windows Mobile?", "Everything stored inside Windows Mobile is erased and it " +
                    "boots like a new device (files on the storage card stay).", "Cold boot")) { coldBoot() },
            PowerMenu.Item("Power off…", "End the session without saving it", warn = true,
                confirm = Triple("Power off?", "The session is not saved; next time Windows Mobile starts " +
                    "from its freshly booted state.", "Power off")) { powerOff() },
            null,
            PowerMenu.Item("Settings") { openSettings() },
        ))
    }

    /** Pause from the power menu: the guest stops until Resume, under a dimmed cover. */
    private fun setUserPaused(paused: Boolean) {
        userPaused = paused
        pausedCover.visibility = if (paused) View.VISIBLE else View.GONE
        if (!paused) startGuest()
        else if (handle != 0L && saveThread?.isAlive != true) NativeBridge.pause(handle)  // a save in progress keeps it paused
    }

    /** Ends the session without saving it: the next start comes from the booted state (boot.wms). */
    // ---- The guest helper ----

    private var helperKickAt = 0L
    private var helperKicks = 0
    private var helperAsked = false

    /**
     * The helper stopped answering while commands wait for it (it died, e.g. during an update):
     * take the storage card out and put it back. Windows Mobile runs the card's autorun on
     * insertion, which starts the helper again. At most once a minute.
     */
    private fun checkHelper() {
        val h = handle
        if (h == 0L || booting || userPaused || !NativeBridge.bootComplete(h) || NativeBridge.guestHung(h)) return
        if (NativeBridge.guestAsleep(h)) { if (foreground) wakeGuestDisplay(); return }  // standby, not a dead helper
        val now = android.os.SystemClock.uptimeMillis()
        if (helperKicks > 0 && guest.lastHeard > helperKickAt) { helperKicks = 0; helperAsked = false }  // it came back
        if (guest.pendingCommands() == 0 || now - guest.lastHeard < 70_000 || now - helperKickAt < 60_000) return
        helperKickAt = now
        // (The helper restarts itself after 45 s stuck, so this is for a helper that is gone.)
        // Second time in a row: the card trick didn't bring it back; a Windows Mobile restart will
        // (the helper starts with the system). Ask first: open programs close.
        if (helperKicks++ >= 1) {
            if (helperAsked) return
            helperAsked = true
            com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
                .setTitle("PocketStylus helper isn't running")
                .setMessage("Windows Mobile's helper stopped, so the lock key, rotation, settings and sync can't reach " +
                    "it. Restart Windows Mobile to start it again? Open programs close; your files and settings stay.")
                .setPositiveButton("Restart") { _, _ ->
                    if (handle != 0L) NativeBridge.softReset(handle)
                    helperKicks = 0
                    helperAsked = false
                }
                .setNegativeButton("Later", null)
                .show()
            return
        }
        Log.w(TAG, "guest helper silent with ${guest.pendingCommands()} commands waiting: re-inserting the card")
        reinsertCard(h)
    }

    /**
     * Takes the storage card out and puts it back 2.5 s later: WM runs the card's autorun again
     * (starting a helper that is gone) and mounts it as \Storage Card again.
     */
    private fun reinsertCard(h: Long) {
        NativeBridge.setShareMounts(h, arrayOf(), arrayOf())
        ui.postDelayed({
            if (handle != h) return@postDelayed
            val mounts = StorageBridge.mounts(this, settings)
            NativeBridge.setShareMounts(h, mounts.map { it.first }.toTypedArray(), mounts.map { it.second }.toTypedArray())
            val o = guest.overlays()
            NativeBridge.setShareOverlays(h, o.map { it.first }.toTypedArray(), o.map { it.second }.toTypedArray())
        }, 2500)
    }

    /** Cold boot: no saved state at all; Windows Mobile boots from its ROM like a new device. */
    private fun coldBoot() {
        camera.stop()
        cancelLeave()
        cancelBackgroundTimer()
        saveThread?.join()
        val h = handle
        handle = 0L
        if (h != 0L) {
            NativeBridge.pause(h)
            NativeBridge.setSurface(h, null)
            NativeBridge.destroy(h)
        }
        clearSnapshots(this)
        recreate()
    }

    // ---- A crashed Windows Mobile ----

    private var hungAsked = false

    /** Windows Mobile stopped responding (see NativeBridge.guestHung): offer a clean restart. */
    private fun checkHung() {
        if (handle == 0L || hungAsked || !foreground || !NativeBridge.guestHung(handle)) return
        hungAsked = true
        com.google.android.material.dialog.MaterialAlertDialogBuilder(this)
            .setTitle("Windows Mobile stopped responding")
            .setMessage("It crashed and won't react to keys or the screen. Restart it from your last boot point? " +
                "This session can't be saved.")
            .setPositiveButton("Restart") { _, _ -> restartFromBoot() }
            .setNegativeButton("Wait") { _, _ -> ui.postDelayed({ hungAsked = false }, 30_000) }
            .show()
    }

    /** Ends the crashed session without saving it and starts again from boot.wms. */
    private fun restartFromBoot() {
        camera.stop()
        cancelLeave()
        cancelBackgroundTimer()
        saveThread?.join()
        val h = handle
        handle = 0L
        if (h != 0L) {
            NativeBridge.pause(h)
            NativeBridge.setSurface(h, null)
            NativeBridge.destroy(h)
        }
        snapshotDir?.let { File(it, RESUME).delete() }
        recreate()
    }

    private fun powerOff() {
        camera.stop()  // no camera frame may reach the session being destroyed
        cancelLeave()
        cancelBackgroundTimer()
        releaseService()
        saveThread?.join()
        val h = handle
        handle = 0L
        if (h != 0L) {
            NativeBridge.pause(h)
            NativeBridge.setSurface(h, null)
            NativeBridge.destroy(h)
        }
        snapshotDir?.let { File(it, RESUME).delete() }
        finish()
    }

    override fun onSettingsChanged(needsRestart: Boolean) {
        if (needsRestart) {
            Toast.makeText(this, "Restarting Windows Mobile to apply the change", Toast.LENGTH_SHORT).show()
            recreate()
        } else applyLiveSettings()
    }

    /** A setting changed from inside Windows Mobile: apply everything that can change live. */
    private fun applyLiveSettings() {
        skin.keysOn = settings.keysOn
        skin.dpadOn = settings.dpadOn
        skin.backdrop = settings.backdrop
        skin.wallpaperOn = settings.showWallpaper
        skin.backdropOpacity = settings.backdropOpacity / 100f
        if (settings.showWallpaper) loadWallpaper()
        if (foreground && settings.keyShine) skin.startTilt() else skin.stopTilt()
        skin.shineBrightness = settings.shineBrightness
        skin.shineWidth = settings.shineWidth
        skin.hingeOn = settings.hingeOn
        skin.hingeStyle = settings.hingeStyle
        skin.hingeThickness = settings.hingeThickness
        skin.hingeLocation = settings.hingeLocation
        skin.hingeOpacity = settings.hingeOpacity
        skin.setBacklight(settings.backlightOn, settings.backlightIntensity, settings.backlightTimeout, settings.backlightColor, settings.backlightDirt)
        if (settings.rotateGuest && foreground && !foldGuest) rotation.start() else rotation.stop()
        if (handle != 0L) {
            NativeBridge.setFilter(handle, settings.screenFilter)
            NativeBridge.setAudioGain(handle, Math.pow(10.0, settings.audioBoostDb / 20.0).toFloat())
            // WM's Buttons 1-4: choices WM must store (Keymap.HwButtons), only when they changed.
            val buttons = GuestSettings.buttonsCommand(settings)
            if (buttons != lastButtonsSent) { lastButtonsSent = buttons; guest.send(buttons) }
        }
        // A slider dragged in WM's settings app lands here many times a second: resend the
        // backlight, theme and wallpaper only when their own settings changed, so the helper's
        // mailbox doesn't fill with repeats.
        sendBrightness()
        syncTheme()
        val wallKey = "${settings.syncWallpaper}/${settings.showWallpaper}"
        if (wallKey != lastWallKey) {
            lastWallKey = wallKey
            wallpaperChecked = false
            wallpaperSent = false
        }
        lastLayoutInfo?.let { onWindowLayout(it) }  // only acts when the fold state changed
        clipboard.apply()  // Sync Clipboard
        ime.apply()  // Use Android IME over Device IME
    }

    override fun onOpenSettings() = openSettings()

    override fun onOpenAllFiles() {
        ownUi = true
        startActivity(StorageBridge.allFilesAccessIntent(this))
    }

    /** "Edit storage folders" in Windows Mobile's settings app: the folder mapping page. */
    override fun onOpenStorage() {
        ownUi = true
        @Suppress("DEPRECATION")
        startActivityForResult(Intent(this, StorageFoldersActivity::class.java), REQ_SETTINGS)
    }

    override fun onGuestPatched() {
        Log.i(TAG, "guest patched; waiting for it to restart")
        guestReady = false
        File(snapshotDir, BOOT).delete()
        booting = true
        bootCompleteAt = 0L
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    override fun onGuestReady() { guestReady = true }

    /** Ring tones and alerts play on Android's ringer / notification volume, media on media volume. */
    override fun onGuestAudio(kind: String) {
        if (handle == 0L) return
        NativeBridge.setAudioUsage(handle, when (kind) { "ring" -> 6; "alert" -> 5; else -> 1 })
    }

    /** Send via Bluetooth from Windows Mobile: the phone's Bluetooth picker, without locking the guest. */
    override fun onGuestCall(line: String) = calls.onGuest(line)

    override fun onGuestClip() = clipboard.fromGuest()

    override fun onGuestIme(what: String) = ime.onGuest(what)

    /** The helper runs, but on a card WM named \Storage Card2 (the name was taken): re-insert it. */
    override fun onGuestRemount() {
        val h = handle
        val now = android.os.SystemClock.uptimeMillis()
        if (h == 0L || now - helperKickAt < 60_000) return
        helperKickAt = now
        Log.i(TAG, "guest card mounted under another name: re-inserting it")
        reinsertCard(h)
    }

    /** Clipboard reads need window focus; the phone's keyboard shows only with it. */
    override fun onWindowFocusChanged(hasFocus: Boolean) {
        super.onWindowFocusChanged(hasFocus)
        clipboard.onFocus(hasFocus)
        ime.onFocus(hasFocus)
    }

    override fun onGuestRadioRefresh() { if (handle != 0L) NativeBridge.modemRefresh(handle) }

    override fun onGuestShare(name: String) {
        ownUi = true  // suspends without locking if the picker covers the app (reset on return)
        if (!bluetooth.share(this, name)) {
            ownUi = false
            Toast.makeText(this, "Could not send $name over Bluetooth", Toast.LENGTH_SHORT).show()
        }
    }

    override fun onGuestHello() {
        helperKicks = 0
        helperAsked = false
        lastStatus = ""
        if (foldGuest) guest.send("rotate 90") else rotation.resend()
        lastBacklight = -1
        sendBrightness()
        wallpaperSent = false
        wallpaperChecked = false
        syncTheme(force = true)
        lastButtonsSent = GuestSettings.buttonsCommand(settings)
        guest.send(lastButtonsSent!!)  // WM's Buttons 1-4: first setup / changes made while away
        bluetooth.scan()
        clipboard.resend()  // the helper forgot its clipboard / SIP watch
        ime.resend()
    }

    // ---- Phone wallpaper and brightness mirrored into the guest ----

    private var lastBacklight = -1
    /** The landscape picture was replaced by the fold layout's: rewrite the cover-screen set. */
    @Volatile private var portraitJpegsStale = false
    private var wallpaperSent = false
    private var lastButtonsSent: String? = null
    private var lastWallKey: String? = null

    private val brightnessObserver = object : android.database.ContentObserver(ui) {
        override fun onChange(selfChange: Boolean) = sendBrightness()
    }

    /** The phone's wallpaper changed: re-cut it for the guest and redraw the backdrop. */
    private val wallpaperListener = android.app.WallpaperManager.OnColorsChangedListener { _, which ->
        if (which and android.app.WallpaperManager.FLAG_SYSTEM == 0) return@OnColorsChangedListener
        wallpaperChecked = false
        if (settings.showWallpaper) loadWallpaper()
        syncTheme()
    }

    private var themeKeySent: String? = null

    /**
     * Themes Windows Mobile in the phone's current colours: regenerates the theme when the palette
     * changed (or [force], when the helper restarted; it skips a theme it already has) and sends
     * "theme". Turning the setting off sends "theme reset", which restores the previous theme.
     */
    private fun syncTheme(force: Boolean = false) {
        if (handle == 0L) return
        if (!settings.syncTheme) {
            if (settings.themeApplied) { guest.send("theme reset"); settings.themeApplied = false; themeKeySent = null }
            return
        }
        thread(name = "theme") { synchronized(ThemeGenerator::class.java) {
            val gen = ThemeGenerator(this)
            val pal = gen.palette()
            val key = gen.key(pal)
            if (!force && key == themeKeySent) return@synchronized
            gen.write(guest.boxFile("theme"), pal)
            ui.post {
                if (handle == 0L) return@post
                themeKeySent = key
                settings.themeApplied = true
                guest.send("theme")
            }
        } }
    }

    /** The phone's brightness as a slider position (0-100) -> "backlight N", when it changes. */
    private fun sendBrightness() {
        if (!settings.syncBrightness || handle == 0L) return
        val raw = android.provider.Settings.System.getInt(contentResolver,
            android.provider.Settings.System.SCREEN_BRIGHTNESS, 128).coerceIn(0, 255)
        // The setting is roughly linear in light; the slider (and WM's levels) are perceptual.
        val pct = Math.round(Math.sqrt(raw / 255.0) * 100).toInt()
        if (pct == lastBacklight) return
        lastBacklight = pct
        guest.send("backlight $pct")
    }

    /** Writes the home-screen pictures for the helper: portrait, and landscape for each turn. */
    private fun writeWallpaperJpegs(portrait: android.graphics.Bitmap) {
        fun save(name: String, b: android.graphics.Bitmap) {
            val f = guest.boxFile(name)
            f.parentFile?.mkdirs()
            val tmp = File(f.path + ".tmp")
            tmp.outputStream().use { b.compress(android.graphics.Bitmap.CompressFormat.JPEG, 92, it) }
            tmp.renameTo(f)
        }
        fun turned(deg: Float) = android.graphics.Bitmap.createBitmap(portrait, 0, 0, portrait.width, portrait.height,
            android.graphics.Matrix().apply { postRotate(deg) }, true)
        save("wall_portrait.jpg", portrait)
        // Turned so the picture stays put on the panel while Windows Mobile draws sideways:
        // left side up (guest 90) needs it a quarter turn clockwise, right side up the other way.
        val cw = turned(90f)
        save("wall_landscape_90.jpg", cw)
        save("wall_landscape.jpg", cw)
        save("wall_landscape_270.jpg", turned(-90f))
    }

    /**
     * Writes device_wallpaper.bmp to the storage card: the phone's wallpaper as it lies under the
     * display, at the guest's resolution, so it can be set as the Windows Mobile background and
     * line up with the backdrop. Rewritten only when it changes.
     */
    private fun writeDeviceWallpaper() {
        val rect = skin.displayRect()
        // The wallpaper fills the whole screen; the display rect is in the skin's coordinates.
        val screen = if (Build.VERSION.SDK_INT >= 30) windowManager.currentWindowMetrics.bounds
        else android.graphics.Rect(0, 0, skin.width, skin.height)
        val vw = screen.width()
        val vh = screen.height()
        val at = IntArray(2).also { skin.getLocationOnScreen(it) }
        if (rect.isEmpty || vw == 0 || vh == 0) return
        // Open foldable: Windows Mobile is landscape, so its picture is cut landscape too.
        val fold = foldGuest
        val gw = if (fold) guestH else guestW
        val gh = if (fold) guestW else guestH
        thread(name = "device-wallpaper") {
            val d = phoneWallpaper(vw, vh) ?: return@thread
            Log.i(TAG, "device wallpaper: ${if (fold) "landscape (open)" else "portrait (cover)"} cut from " +
                "${d.intrinsicWidth}x${d.intrinsicHeight} for screen ${vw}x$vh")
            val iw = d.intrinsicWidth.coerceAtLeast(1)
            val ih = d.intrinsicHeight.coerceAtLeast(1)
            // The system centre-crops the screen's picture to the whole screen; Windows Mobile gets
            // the piece under its display, so it lines up with the wallpaper around it.
            val k = maxOf(vw.toFloat() / iw, vh.toFloat() / ih)
            val ox = (vw - iw * k) / 2
            val oy = (vh - ih * k) / 2
            val out = android.graphics.Bitmap.createBitmap(gw, gh, android.graphics.Bitmap.Config.ARGB_8888)
            val c = android.graphics.Canvas(out)
            c.scale(gw.toFloat() / rect.width(), gh.toFloat() / rect.height())
            c.translate(-(rect.left + at[0] - screen.left).toFloat(), -(rect.top + at[1] - screen.top).toFloat())
            d.setBounds(ox.toInt(), oy.toInt(), (ox + iw * k).toInt(), (oy + ih * k).toInt())
            d.isFilterBitmap = true
            d.draw(c)
            if (fold) {
                // Shown by Windows Mobile turned to 90 degrees: exactly what sits under the display.
                for (name in listOf("wall_landscape_90.jpg", "wall_landscape.jpg", OPEN_WALL)) {
                    val jf = guest.boxFile(name)
                    jf.parentFile?.mkdirs()
                    val tmp = File(jf.path + ".tmp")
                    tmp.outputStream().use { out.compress(android.graphics.Bitmap.CompressFormat.JPEG, 92, it) }
                    tmp.renameTo(jf)
                }
                portraitJpegsStale = true
                ui.post { if (handle != 0L && settings.syncWallpaper) guest.send("wallpaper") }
                return@thread
            }
            val bytes = bmp24(out)
            val f = guest.deviceWallpaper
            val same = f.exists() && f.length() == bytes.size.toLong() && f.readBytes().contentEquals(bytes)
            val stale = portraitJpegsStale
            if (gw < gh && (!same || stale || !guest.boxFile("wall_portrait.jpg").exists())) {
                writeWallpaperJpegs(out)
                portraitJpegsStale = false
            }
            if (!same || stale || !wallpaperSent) ui.post {
                if (handle != 0L && settings.syncWallpaper) { wallpaperSent = true; guest.send("wallpaper") }
            }
            if (same) return@thread
            f.parentFile?.mkdirs()
            val tmp = File(f.path + ".tmp")
            tmp.writeBytes(bytes)
            tmp.renameTo(f)
            ui.post {
                if (handle == 0L) return@post
                val o = guest.overlays()
                NativeBridge.setShareOverlays(handle, o.map { it.first }.toTypedArray(), o.map { it.second }.toTypedArray())
            }
        }
    }

    /** A 24-bit bottom-up BMP, which every Windows Mobile picture viewer and background setting reads. */
    private fun bmp24(b: android.graphics.Bitmap): ByteArray {
        val w = b.width
        val h = b.height
        val row = (w * 3 + 3) and 3.inv()
        val size = 54 + row * h
        val buf = java.nio.ByteBuffer.allocate(size).order(java.nio.ByteOrder.LITTLE_ENDIAN)
        buf.put('B'.code.toByte()).put('M'.code.toByte()).putInt(size).putInt(0).putInt(54)
        buf.putInt(40).putInt(w).putInt(h).putShort(1).putShort(24).putInt(0).putInt(row * h)
            .putInt(2835).putInt(2835).putInt(0).putInt(0)
        val px = IntArray(w)
        for (y in h - 1 downTo 0) {
            b.getPixels(px, 0, w, 0, y, w, 1)
            for (x in 0 until w) {
                val p = px[x]
                buf.put((p and 0xFF).toByte()).put(((p shr 8) and 0xFF).toByte()).put(((p shr 16) and 0xFF).toByte())
            }
            repeat(row - w * 3) { buf.put(0) }
        }
        return buf.array()
    }

    /**
     * Home-screen wallpaper behind the display, blurred. Reading it needs all-files access on
     * Android 13+; without it the window shows the live wallpaper itself (blurred by the system
     * where supported).
     */
    private fun loadWallpaper() {
        val blur = settings.wallpaperBlur
        // The system's own wallpaper behind a see-through window: the very picture the home screen
        // shows on this screen (cover or inner), placed exactly as there. Blurred by the compositor
        // when it can blur behind windows; otherwise a blurred copy is drawn by the skin.
        val liveBlur = blur == 0 || (Build.VERSION.SDK_INT >= 31 && windowManager.isCrossWindowBlurEnabled)
        if (liveBlur) {
            skin.wallpaper = null
            window.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))
            window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER)
            if (Build.VERSION.SDK_INT >= 31) window.setBackgroundBlurRadius(blur * 6)
            skin.invalidate()
            return
        }
        thread(name = "wallpaper") {
            val bmp = runCatching {
                val d = phoneWallpaper() ?: return@runCatching null
                val w = d.intrinsicWidth.coerceAtLeast(1)
                val h = d.intrinsicHeight.coerceAtLeast(1)
                // Downscale hard (blur is cheap on a small picture), then box-blur.
                val k = if (blur > 0) 4f + blur else 2f
                val sw = (w / k).toInt().coerceAtLeast(8)
                val sh = (h / k).toInt().coerceAtLeast(8)
                val small = android.graphics.Bitmap.createBitmap(sw, sh, android.graphics.Bitmap.Config.ARGB_8888)
                d.setBounds(0, 0, sw, sh)
                d.draw(android.graphics.Canvas(small))
                if (blur > 0) boxBlur(small, 2, 3)
                small
            }.getOrNull()
            ui.post {
                if (isFinishing) return@post
                if (bmp != null) {
                    skin.wallpaper = bmp
                } else {
                    window.setBackgroundDrawable(android.graphics.drawable.ColorDrawable(Color.TRANSPARENT))
                    window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WALLPAPER)
                    if (Build.VERSION.SDK_INT >= 31 && blur > 0) window.setBackgroundBlurRadius(blur * 6)
                }
            }
        }
    }

    private fun boxBlur(b: android.graphics.Bitmap, radius: Int, passes: Int) {
        val w = b.width
        val h = b.height
        val src = IntArray(w * h)
        val tmp = IntArray(w * h)
        b.getPixels(src, 0, w, 0, 0, w, h)
        repeat(passes) {
            for (pass in 0..1) {
                val (from, to) = if (pass == 0) src to tmp else tmp to src
                val n = if (pass == 0) w else h
                val lines = if (pass == 0) h else w
                for (line in 0 until lines) {
                    for (i in 0 until n) {
                        var r = 0; var g = 0; var bl = 0; var c = 0
                        for (o in -radius..radius) {
                            val j = (i + o).coerceIn(0, n - 1)
                            val px = if (pass == 0) from[line * w + j] else from[j * w + line]
                            r += (px shr 16) and 0xFF; g += (px shr 8) and 0xFF; bl += px and 0xFF; c++
                        }
                        val v = (0xFF shl 24) or ((r / c) shl 16) or ((g / c) shl 8) or (bl / c)
                        if (pass == 0) to[line * w + i] = v else to[i * w + line] = v
                    }
                }
            }
        }
        b.setPixels(src, 0, w, 0, 0, w, h)
    }

    private fun sendKey(vk: Int, down: Boolean) {
        if (handle == 0L) return
        // Buttons 1-4 go out as the stock key for their Device Settings choice (Keymap.HwButtons),
        // so they need no guest helper; the release goes to the key the press went to.
        val n = Keymap.HwButtons.button(vk)
        if (n == 0) { NativeBridge.key(handle, vk, down); return }
        val key = if (down) Keymap.HwButtons.key(n, settings.hwButton(n), settings.hwButtonMap).also { heldButtons[n] = it }
            else if (heldButtons.containsKey(n)) heldButtons.remove(n) else return
        if (key != null) NativeBridge.key(handle, key, down)
    }

    /** The key each held button was sent as (null: "None"). */
    private val heldButtons = mutableMapOf<Int, Int?>()

    override fun onGuestButtons() {
        val buttons = GuestSettings.buttonsCommand(settings)  // a store that took effect, or WM's page changed
        if (buttons != lastButtonsSent) { lastButtonsSent = buttons; guest.send(buttons) }
    }

    private fun openSettings() {
        ownUi = true  // suspends without locking or running on in the background
        @Suppress("DEPRECATION")
        startActivityForResult(Intent(this, SettingsActivity::class.java), REQ_SETTINGS)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        // Settings: a restart when asked for, else the live changes are applied in place.
        if (requestCode == REQ_SETTINGS) { if (resultCode == RESULT_OK) recreate() else applyLiveSettings() }
    }

    /** Guest key for a phone key, or null to leave it to Android. Volume keys follow the settings. */
    private fun guestKey(keyCode: Int): Int? = when (keyCode) {
        KeyEvent.KEYCODE_VOLUME_UP -> Keymap.volumeKey(settings.volumeUp, up = true)
        KeyEvent.KEYCODE_VOLUME_DOWN -> Keymap.volumeKey(settings.volumeDown, up = false)
        else -> Keymap.vk(keyCode)
    }

    private fun isLockKey(keyCode: Int) =
        (keyCode == KeyEvent.KEYCODE_VOLUME_UP && settings.volumeUp == Keymap.KeyAction.LOCK) ||
            (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN && settings.volumeDown == Keymap.KeyAction.LOCK)

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (keyCode == KeyEvent.KEYCODE_MENU) { openSettings(); return true }
        if (isLockKey(keyCode)) {
            if (event.repeatCount == 0) guest.send("lock")
            return true
        }
        val vk = guestKey(keyCode) ?: return super.onKeyDown(keyCode, event)
        if (event.repeatCount == 0) sendKey(vk, true)
        return true
    }

    override fun onKeyUp(keyCode: Int, event: KeyEvent): Boolean {
        if (isLockKey(keyCode)) return true
        val vk = guestKey(keyCode) ?: return super.onKeyUp(keyCode, event)
        sendKey(vk, false)
        return true
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        if (handle != 0L) NativeBridge.setSurface(handle, holder.surface)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {}

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        if (handle != 0L) NativeBridge.setSurface(handle, null)
    }

    companion object {
        const val TAG = "WMDroid"
        const val REQ_SETTINGS = 7
        const val REQ_MIC = 8
        const val REQ_PHONE = 9
        const val REQ_LOCATION = 10
        /** The open (inner) screen's landscape wallpaper piece, kept across folds (mailbox file). */
        const val OPEN_WALL = "wall_open.jpg"
        const val BOOT = "boot.wms"      // state right after the first boot reached the home screen
        const val RESUME = "resume.wms"  // state when the app was last left
        /** Host CPU share (%) while running in the background, and with the screen off as well. */
        const val CPU_BACKGROUND = 25
        const val CPU_SCREEN_OFF = 15
        /** Time the guest gets to act on a lock before it is suspended or hibernated. */
        const val LOCK_GRACE_MS = 1500L

        /** Deletes saved states so the next start is a cold boot. */
        /** Deletes the saved states of the chosen OS only (each OS keeps its own, see [boot]). */
        fun clearSnapshots(ctx: Context) {
            val id = Settings(ctx).profileId ?: return
            File(ctx.filesDir, "states").listFiles()?.filter { it.name.startsWith("$id-") }?.forEach { it.deleteRecursively() }
        }
    }
}
