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
 * [POLL_MS]; the core reports it from the driver loading the DLL and from frame requests) and is
 * closed as soon as the guest closes it, stops asking for frames, or the app leaves the screen.
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
        if (!wanted || !enabled) { close(); return }
        if (!ok) {
            if (!askedPermission) {
                askedPermission = true
                activity.runOnUiThread { activity.requestPermissions(arrayOf(Manifest.permission.CAMERA), REQ_CAMERA) }
            }
            return  // the guest sees WMDroid's test pattern meanwhile
        }
        val front = settings.cameraFront
        if ((device != null || opening) && front != openFront) close()  // switched in settings
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
                    val req = d.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
                        addTarget(r.surface)
                        set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO)
                        set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                        if (fps != null) set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, fps)
                    }.build()
                    runCatching { s.setRepeatingRequest(req, null, bg) }.onFailure { failed("request", it) }
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

    private fun onImage(r: ImageReader) {
        val img = runCatching { r.acquireLatestImage() }.getOrNull() ?: return
        try {
            val h = handle()
            if (h == 0L || !running || reader !== r) return
            val p = img.planes
            // Clockwise turn that maps the scene onto the guest's panel the way the phone shows it:
            // the usual preview rotation for the display, less the panel's own turn on screen (the
            // front camera is mirrored afterwards, which flips the direction of that turn).
            val display = displayDegrees()
            val rot = if (mirror) (sensorOrientation + display + panelRotation) % 360
            else (sensorOrientation - display - panelRotation + 720) % 360
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
        if (was) {
            Log.i(TAG, "camera closed after $frames frames")
            val h = handle()
            if (h != 0L) NativeBridge.cameraClear(h)
        }
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
        const val REQ_CAMERA = 11

        /** One idle thread for all bridges: camera callbacks and frame conversion run here. */
        private val bridgeThread by lazy { HandlerThread("wm-camera").apply { start() } }
    }
}
