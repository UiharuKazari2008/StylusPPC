package jp.net.seventhreeseven.wmdroid

import android.view.Surface

/** Thin JNI surface over the C++ emulator core (app/src/main/cpp/jni.cpp). */
object NativeBridge {
    init {
        System.loadLibrary("wmdroid")
    }

    external fun create(imagePath: String, memMB: Int, armv5: Boolean, width: Int, height: Int): Long
    external fun start(handle: Long)
    external fun pause(handle: Long)
    external fun destroy(handle: Long)
    external fun setSurface(handle: Long, surface: Surface?)
    /** Upscaling filter: 0 smooth, 1 sharp pixels, 2 enhanced (bicubic). */
    external fun setFilter(handle: Long, filter: Int)
    /** Realism filters (replace the scaling filter while any is on): [Settings.realismValues]. */
    external fun setRealism(handle: Long, values: IntArray)
    /**
     * What the realism filters react to: the viewer's position from the resting pose (-1..1, + =
     * from the right / from above), room light in lux (0 = unknown), the phone's temperature in C
     * and its screen brightness 0..1.
     */
    external fun setScreenSensors(handle: Long, tiltX: Float, tiltY: Float, lux: Float, tempC: Float, brightness: Float)
    /** The screen's surface is extended-range (HDR), so its brightness can be lifted above the UI's. */
    external fun hdrActive(handle: Long): Boolean
    external fun setRotation(handle: Long, quarterTurns: Int)
    /** Ask for the fastest CPU for [ms] (a rotation or layout change is about to redraw everything). */
    external fun boost(handle: Long, ms: Int)
    /** Windows Mobile has crashed: its battery driver stopped polling while the CPU stays busy. */
    external fun guestHung(handle: Long): Boolean
    /** Android audio usage for the guest's sound: 1 media, 5 notification, 6 ringtone (AAudio values). */
    external fun setAudioUsage(handle: Long, usage: Int)
    /** Guest audio reached the speaker within the last [ms] (jni.cpp, lastPlayNs). */
    external fun playedWithin(handle: Long, ms: Int): Boolean
    external fun touch(handle: Long, down: Boolean, x: Int, y: Int)
    /** A Windows virtual key (VK_*) pressed or released. */
    external fun key(handle: Long, vk: Int, down: Boolean)
    /** Bytes received on the guest's COM1 (UART0), e.g. NMEA from the phone's GPS. */
    external fun serialWrite(handle: Long, data: ByteArray)
    /** True while Windows Mobile has its display on (LCD controller enabled). */
    external fun displayOn(handle: Long): Boolean
    /** Press the guest's power button (suspend / wake). */
    /** Windows Mobile is in standby (its own auto-off): only [powerButton] wakes it. */
    external fun guestAsleep(handle: Long): Boolean
    external fun powerButton(handle: Long)
    /** Soft reset (the board's reset button): the guest restarts, keeping its RAM. */
    external fun softReset(handle: Long)
    /** Host CPU share for the guest while busy, 1..100 % (background "stepping"). */
    external fun setCpuLimit(handle: Long, percent: Int)
    /** Guest speed limit in MIPS (guest instructions per microsecond), 0 = unlimited (Machine::mipsCap). */
    external fun setSpeedLimit(handle: Long, mips: Int)
    /** Output gain applied to the guest's sound (1 = as is). */
    external fun setAudioGain(handle: Long, gain: Float)
    /** True while a program has the GPS COM port open (FIFO on, that port's interrupt unmasked, and it is reading). */
    external fun serialOpen(handle: Long): Boolean
    /** Emulation-loop turns finished. Stops advancing while a frame swap is blocked. */
    external fun guestSlices(handle: Long): Long
    /** False while the phone's screen is off, so a swap cannot freeze the guest for the whole sleep. */
    external fun setPresenting(handle: Long, on: Boolean)
    /** Command pipe. Latest line per verb. Not COM1 or COM4. */
    external fun hostCmdSend(handle: Long, line: String)
    /** Lines the helper wrote on the command pipe, separated by '\n'. Empty if none. */
    external fun hostCmdTake(handle: Long): String
    /** Status pipe. Phone status in, pulses out. Does not share a queue with commands. */
    external fun hostStatSend(handle: Long, line: String)
    /** Lines the helper wrote on the status pipe, separated by '\n'. Empty if none. */
    external fun hostStatTake(handle: Long): String
    /** One chunk of a contact/calendar transfer, at most 1792 bytes. False when 64 chunks are already waiting. */
    external fun hostBulkPut(handle: Long, data: ByteArray, last: Boolean): Boolean
    /** One chunk from the helper. First byte is 1 on the last chunk. Null when none is waiting. */
    external fun hostBulkTake(handle: Long): ByteArray?
    external fun screenSize(handle: Long): IntArray
    external fun status(handle: Long): String
    /** Battery: plugged in, charge 0-100, temperature in tenths of a degree C. */
    external fun setBattery(handle: Long, acOnline: Boolean, percent: Int, tenthsC: Int)
    external fun setMicEnabled(handle: Long, on: Boolean)
    /** True while the guest is recording audio, so the phone's mic only opens when needed. */
    external fun micWanted(handle: Long): Boolean
    /** Snapshots; only while paused. After a failed loadState the handle must be destroyed. */
    external fun saveState(handle: Long, path: String): Boolean
    external fun loadState(handle: Long, path: String): Boolean
    /**
     * A snapshot of the same image saved with another screen or memory size: Windows Mobile boots again
     * at this session's size with its internal storage kept (GuestStates). [cold]: WM 6.x, the memory
     * may differ; else a warm restart (WM 5), same memory only. False: destroy the handle.
     */
    external fun adoptState(handle: Long, path: String, cold: Boolean): Boolean
    /** True once the guest has finished booting (sustained idle at the home screen). */
    external fun bootComplete(handle: Long): Boolean
    /** Folders shown to the guest as \Storage Card; empty arrays turn sharing off. */
    external fun setShareMounts(handle: Long, names: Array<String>, paths: Array<String>)
    /** App folders added to every \Storage Card (the WMDroid helper and its mailbox). */
    external fun setShareOverlays(handle: Long, names: Array<String>, paths: Array<String>)
    /**
     * Notification LED / vibrator (docs/research-notification-led.md). Bitmask: [LED_LIGHT] light on,
     * [LED_BLINK] blinking, [LED_VIBRATE] vibrator on now or pulsed since the previous call (each call
     * consumes that latch: poll from one place), bits 8..15 blink on time in 50 ms units (0 = unknown;
     * WM's own pattern is a short flash every 2 s), bits 16..23 vibrator start count mod 256.
     */
    external fun ledState(handle: Long): Int
    const val LED_LIGHT = 1
    const val LED_BLINK = 2
    const val LED_VIBRATE = 4

    // ---- Virtual GSM modem on COM1 (core/src/board/modem.h): the radio of the WM 6 / 6.1 / 6.1.4 images ----
    /** The phone's network for the guest's RIL: bars 0..4, operator name, MCC+MNC, WM data icon name, own number. */
    external fun modemNetwork(handle: Long, service: Boolean, signal: Int, roaming: Boolean, operator: String,
                              numeric: String, data: String, number: String)
    /** The real call's progress for a call Windows Mobile dialled: pending, dialing, active, held, ended. */
    external fun modemCallState(handle: Long, id: Int, state: String)
    /** An incoming call rings in Windows Mobile ("" or "-": withheld); answered by a ringing / noring event. */
    external fun modemRing(handle: Long, number: String)
    /** An incoming text message for Windows Mobile: an SMS-DELIVER PDU in hex, with a leading SMSC field. */
    external fun modemSms(handle: Long, pdu: String)
    /** The answer to the modem event "smssend <mr> <pdu>": the phone sent the message (or failed to). */
    external fun modemSmsResult(handle: Long, mr: Int, sent: Boolean)
    /** Call events from the modem, one per line, like the helper's "call ..." lines: dial, ringing, noring, answer, end, hold, mute. */
    external fun modemEvents(handle: Long): String
    /** Windows Mobile's radio driver talks to the modem (COM1 is the radio, not the GPS). */
    external fun modemActive(handle: Long): Boolean
    /** Announce the registration again (the guest's RIL then re-reads the operator name). */
    external fun modemRefresh(handle: Long)

    // ---- Secure web proxy (core/src/net/tlsproxy.h, SecureProxy.kt) ----
    /** The private root CA kept in [dir] (made on first use: slow, call off the UI thread), as DER; null on failure. */
    external fun tlsCa(dir: String): ByteArray?
    /** HTTPS from the guest through the proxy with the CA in [dir], or straight out when [dir] is null. */
    external fun setTlsProxy(handle: Long, dir: String?)
    /** Messenger / Passport names answer with Escargot's servers (core/src/net/revival.h, docs/research-escargot.md). */
    external fun setEscargot(handle: Long, on: Boolean)
    /** Certificate warnings waiting for the user, each "<id> <seconds> <host>\n<message>"; polling also tells the proxy someone answers. */
    external fun tlsPrompts(): Array<String>
    /** The user's answer to warning [id]: [allow] continues to that site with that certificate for this session. */
    external fun tlsAnswer(id: Int, allow: Boolean)
    /** Forgets the "continue anyway" choices (a new session). */
    external fun tlsForget()

    // ---- G-sensor (motion_jni.cpp, MotionBridge.kt) ----
    /** True while a Windows Mobile app reads the G-sensor (SEN0:), so the phone's accelerometer runs only then. */
    external fun motionWanted(handle: Long): Boolean
    /** One accelerometer sample (m/s^2, Android's sign), already turned into the guest panel's frame. */
    external fun motionAccel(handle: Long, x: Float, y: Float, z: Float)
    /** The phone sensor stopped: the guest sees a phone lying face up. */
    external fun motionClear(handle: Long)

    // ---- Camera bridge (camera_jni.cpp, CameraBridge.kt) ----
    /** True while Windows Mobile has its camera open, so the phone's camera runs only then. */
    external fun cameraWanted(handle: Long): Boolean
    /**
     * Flash the guest asked for. Low byte: 0 auto, 1 on, 2 off (Pictures & Videos menu order).
     * Bit 8: an app such as HTC Flashlight is holding the LED on.
     */
    external fun cameraFlash(handle: Long): Int
    /** hostCamera: the phone's camera feeds the guest (otherwise a test pattern); era: 2009 look. */
    external fun cameraConfig(handle: Long, hostCamera: Boolean, eraLevel: Int)
    /** The phone's camera closed: drop its last frame. */
    external fun cameraClear(handle: Long)
    /** A YUV_420_888 frame; rotation is the clockwise turn that makes it upright, mirror flips it after. */
    external fun cameraFrame(handle: Long, y: java.nio.ByteBuffer, yRowStride: Int, u: java.nio.ByteBuffer,
                             v: java.nio.ByteBuffer, uvRowStride: Int, uvPixelStride: Int, width: Int, height: Int,
                             rotation: Int, mirror: Boolean)
}
