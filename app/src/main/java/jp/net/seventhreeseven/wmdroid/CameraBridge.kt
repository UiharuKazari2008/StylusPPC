package jp.net.seventhreeseven.wmdroid

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.OutputConfiguration
import android.hardware.camera2.params.SessionConfiguration
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.util.Size
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executor
import java.util.concurrent.TimeUnit

/**
 * The phone's camera as Windows Mobile's camera (core/src/board/camera.h: the guest's NULLCAM
 * driver gets its frames from WMDroid's wmdcam.dll, installed by the helper).
 *
 * Battery first: Camera2 is opened only while the guest has its camera open (polled every
 * [POLL_MS]; the core reports it from the driver loading the DLL and from frame requests) or an
 * app is holding the flash LED, and is closed as soon as both of those stop, or the app leaves
 * the screen.
 * The phone delivers about 640x480 YUV at up to 15 fps; the core turns it upright, crops and scales
 * it to each size the guest asks for, and adds the optional 2009 look ([Settings.cameraEraEffective]).
 * [Settings.cameraFront] picks the front camera (mirrored, like a mirror).
 */
class CameraBridge(private val activity: Activity, private val settings: Settings, private val handle: () -> Long) {
    /**
     * How far the guest's panel is turned clockwise on the phone's screen, in degrees (90 in the
     * open-fold layout, where the renderer draws it a quarter turn clockwise; NativeBridge.setRotation).
     * Windows Mobile's own screen rotation does not matter: its camera driver's frames go onto the
     * panel pre-rotated (BI_SRCPREROTATE), so the picture must match the panel, as on a real phone.
     */
    @Volatile var panelRotation = 0

    private val bg by lazy { Handler(bridgeThread.looper) }
    @Volatile private var running = false

    // Camera state, touched only on the bridge thread.
    private var device: CameraDevice? = null
    private var session: CameraCaptureSession? = null
    private var reader: ImageReader? = null
    private var opening = false
    private var openFront = false
    private var sensorOrientation = 90
    private var mirror = false
    private var askedPermission = false
    private var frames = 0L
    private var retryAt = 0L   // after an error (e.g. another app has the camera), wait before reopening
    private var hasFlash = false
    private var wantTorch = false
    private var torchOn = false
    private var sceneDark = false
    private var fpsRange: Range<Int>? = null

    private val manager get() = activity.getSystemService(CameraManager::class.java)

    private fun granted() = activity.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    /** Watches the guest while the emulator is on screen. */
    fun start() {
        if (running) return
        running = true
        bg.post(poll)
    }

    /**
     * Closes the camera; no frame reaches the core after this returns. A camera still opening
     * is closed by its own callback (the bridge thread lives on for that).
     */
    fun stop() {
        if (!running) return
        running = false
        bg.removeCallbacks(poll)
        val done = CountDownLatch(1)
        bg.post { close(); done.countDown() }
        runCatching { done.await(2, TimeUnit.SECONDS) }
    }

    private val poll = object : Runnable {
        override fun run() {
            if (!running) return
            runCatching { update() }.onFailure { Log.w(TAG, "camera update", it) }
            bg.postDelayed(this, POLL_MS)
        }
    }

    private fun update() {
        val h = handle()
        if (h == 0L) { close(); return }
        val ok = granted()
        // Camera off: the guest gets WMDroid's test pattern and the phone camera stays closed.
        val enabled = settings.cameraMode != "off"
        NativeBridge.cameraConfig(h, ok && enabled, settings.cameraEraEffective)
        val wanted = NativeBridge.cameraWanted(h)
        val flash = NativeBridge.cameraFlash(h)
        val preview = wanted && enabled
        // Bit 8: HTC Flashlight (or another app) is holding the LED. Low byte: the camera app's
        // own mode, which only matters while that app is open. Auto follows the scene brightness.
        val hold = (flash and FLASH_HOLD) != 0
        val mode = flash and 0xff
        val torch = hold || (preview && mode == FLASH_ON) || (preview && mode == FLASH_AUTO && sceneDark)
        wantTorch = torch
        if (!preview && !torch) { close(); return }
        if (!ok) {
            if (!askedPermission) {
                askedPermission = true
                activity.runOnUiThread { activity.requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA) }
            }
            return  // the guest sees WMDroid's test pattern meanwhile
        }
        // A flashlight with the camera app closed uses the back camera, which has the LED.
        val front = preview && settings.cameraFront
        if ((device != null || opening) && front != openFront) close()
        if (device != null && session != null && torch != torchOn) pushRequest(torch)
        if (device == null && !opening && SystemClock.uptimeMillis() >= retryAt) open(front)
    }

    private fun failed(what: String, e: Any?) {
        Log.w(TAG, "camera $what: $e")
        retryAt = SystemClock.uptimeMillis() + RETRY_MS
        close()
    }

    @SuppressLint("MissingPermission")  // update() checks the permission first
    private fun open(front: Boolean) {
        val cm = manager
        val want = if (front) CameraCharacteristics.LENS_FACING_FRONT else CameraCharacteristics.LENS_FACING_BACK
        val ids = cm.cameraIdList
        val id = ids.firstOrNull { cm.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == want }
            ?: ids.firstOrNull() ?: return
        val ch = cm.getCameraCharacteristics(id)
        sensorOrientation = ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        mirror = ch.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        hasFlash = ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
        val sizes = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
            ?.getOutputSizes(ImageFormat.YUV_420_888)?.toList().orEmpty()
        val size = pickSize(sizes) ?: return
        val fps = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES)
            ?.filter { it.upper >= 15 }?.minWithOrNull(compareBy<Range<Int>> { it.upper }.thenBy { it.lower })
        val r = ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, 2)
        r.setOnImageAvailableListener({ onImage(it) }, bg)
        reader = r
        opening = true
        openFront = front
        frames = 0
        Log.i(TAG, "opening camera $id (${if (front) "front" else "back"}) ${size.width}x${size.height} fps $fps")
        val callback = object : CameraDevice.StateCallback() {
            override fun onOpened(d: CameraDevice) {
                if (!running || reader !== r) { d.close(); return }
                opening = false
                device = d
                startSession(d, r, fps)
            }
            override fun onDisconnected(d: CameraDevice) {
                d.close()
                if (device === d || reader === r) failed("disconnected", id)
            }
            override fun onError(d: CameraDevice, error: Int) {
                d.close()
                if (device === d || reader === r) failed("error", error)
            }
        }
        runCatching { cm.openCamera(id, callback, bg) }.onFailure { failed("open", it) }
    }

    private fun startSession(d: CameraDevice, r: ImageReader, fps: Range<Int>?) {
        val stateCallback =
            object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (device !== d) { s.close(); return }
                    session = s
                    fpsRange = fps
                    pushRequest(wantTorch)
                }
                override fun onConfigureFailed(s: CameraCaptureSession) { if (device === d) failed("session", "configure failed") }
            }
        runCatching {
            if (android.os.Build.VERSION.SDK_INT >= 28) {
                val exec = Executor { bg.post(it) }
                d.createCaptureSession(SessionConfiguration(SessionConfiguration.SESSION_REGULAR,
                    listOf(OutputConfiguration(r.surface)), exec, stateCallback))
            } else @Suppress("DEPRECATION") d.createCaptureSession(listOf(r.surface), stateCallback, bg)  // SessionConfiguration is API 28
        }.onFailure { failed("session", it) }
    }

    /** Repeating preview, with the LED on when the guest asked for flash and this camera has one. */
    private fun pushRequest(torch: Boolean) {
        val d = device ?: return
        val s = session ?: return
        val r = reader ?: return
        val use = torch && hasFlash
        val req = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
            addTarget(r.surface)
            set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
            set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
            if (fpsRange != null) set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fpsRange)
            if (hasFlash) set(CaptureRequest.FLASH_MODE,
                if (use) CaptureRequest.FLASH_MODE_TORCH else CaptureRequest.FLASH_MODE_OFF)
        }.build()
        runCatching { s.setRepeatingRequest(req, null, bg) }
            .onSuccess { torchOn = torch }
            .onFailure { failed("request", it) }
    }

    private fun onImage(r: ImageReader) {
        val img = runCatching { r.acquireLatestImage() }.getOrNull() ?: return
        try {
            val h = handle()
            if (h == 0L || !running || reader !== r) return
            val p = img.planes
            // Skip this while the LED is on: its own light would look like daylight and turn auto off.
            if ((frames and 7L) == 0L && !torchOn) sceneDark = darkScene(p[0].buffer, p[0].rowStride, img.width, img.height)
            // Clockwise turn that maps the scene onto the guest's panel the way the phone shows it:
            // the usual preview rotation for the display, less the panel's own turn on screen (the
            // front camera is mirrored afterwards, which flips the direction of that turn).
            val display = displayDegrees()
            val rot = if (mirror) (sensorOrientation + display + panelRotation) % 360
            else (sensorOrientation - display - panelRotation + 720) % 360
            // A flashlight with the camera app closed still needs a stream, but the guest is not asking for frames.
            if (NativeBridge.cameraWanted(h) && settings.cameraMode != "off")
                NativeBridge.cameraFrame(h, p[0].buffer, p[0].rowStride, p[1].buffer, p[2].buffer, p[1].rowStride,
                    p[1].pixelStride, img.width, img.height, rot, mirror)
            if (frames++ == 0L) Log.i(TAG, "first camera frame ${img.width}x${img.height}, turned $rot")
        } finally {
            img.close()
        }
    }

    private fun close() {
        val was = device != null || opening || reader != null
        opening = false
        runCatching { session?.close() }
        session = null
        runCatching { device?.close() }
        device = null
        runCatching { reader?.close() }
        reader = null
        torchOn = false
        hasFlash = false
        sceneDark = false
        if (was) {
            Log.i(TAG, "camera closed after $frames frames")
            val h = handle()
            if (h != 0L) NativeBridge.cameraClear(h)
        }
    }

    /** True while the sampled picture stays dim. The two thresholds keep auto-flash from flickering. */
    private fun darkScene(y: java.nio.ByteBuffer, stride: Int, w: Int, h: Int): Boolean {
        if (w < 32 || h < 32 || stride < w) return sceneDark
        return try { sampleDark(y, stride, w, h) } catch (e: Exception) { sceneDark }
    }

    private fun sampleDark(y: java.nio.ByteBuffer, stride: Int, w: Int, h: Int): Boolean {
        var sum = 0
        var n = 0
        var row = 0
        while (row < h) {
            val base = row * stride
            var col = 0
            while (col < w) {
                sum += y.get(base + col).toInt() and 0xff
                n++
                col += 32
            }
            row += 32
        }
        if (n == 0) return sceneDark
        val avg = sum / n
        return if (sceneDark) avg < 90 else avg < 45
    }

    @Suppress("DEPRECATION")
    private fun displayDegrees(): Int {
        val r = (if (android.os.Build.VERSION.SDK_INT >= 30) activity.display?.rotation else null)
            ?: activity.windowManager.defaultDisplay.rotation
        return r * 90   // Surface.ROTATION_0..270
    }

    /** About VGA, 4:3 if the camera has it: the guest never asks for more than 640x480. */
    private fun pickSize(sizes: List<Size>): Size? {
        val big = sizes.filter { it.width >= 640 && it.height >= 480 }
        val fourThree = big.filter { it.width * 3 == it.height * 4 }
        return (fourThree.ifEmpty { big }).minByOrNull { it.width * it.height } ?: sizes.maxByOrNull { it.width * it.height }
    }

    companion object {
        private const val TAG = "WMDroid"
        private const val POLL_MS = 250L
        private const val RETRY_MS = 3000L
        // Pictures & Videos menu order (shellres 0xA050..0xA052): 0 auto, 1 on, 2 off.
        private const val FLASH_AUTO = 0
        private const val FLASH_ON = 1
        private const val FLASH_HOLD = 0x100
        const val REQ_CAMERA = 11

        /** One idle thread for all bridges: camera callbacks and frame conversion run here. */
        private val bridgeThread by lazy { HandlerThread("wm-camera").apply { start() } }
    }
}
