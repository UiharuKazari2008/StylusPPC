// Notification LED and vibrator of Device Emulator 2.0/3.0 (IONotificationLEDs, PA 0x500FFFD0),
// decoded for the host. See docs/research-notification-led.md.
//
// The guest driver is the image's own nleddrvr.dll (de_nleddrvr), loaded from
// HKLM\Drivers\BuiltIn\NLed. It declares two "LEDs" and maps 6 bytes, 3 per LED:
//   +0 OffOnBlink   0 off, 1 on, 2 blink (the driver turns a blink request for LED 1 into "on")
//   +1 on time      NLED_SETTINGS_INFO.OnTime  / 100, clamped to 255 (units of 100 us)
//   +2 off time     NLED_SETTINGS_INFO.OffTime / 100, clamped to 255
// LED 0 (bytes 0..2) is the notification light; LED 1 (bytes 3..5) is the vibrator (the driver
// reports lCycleAdjust = -1 for it). The driver only ever writes these bytes (byte stores, from
// NLedSetDevice); it never reads them back and does nothing on PowerDown/PowerUp.
#pragma once
#include <atomic>
#include <cstdint>

namespace wm {

class NotifyLeds {
public:
    // Bits of state(), the value NativeBridge.ledState() returns.
    static constexpr uint32_t LIGHT = 1u << 0;    // notification light on (steady or blinking)
    static constexpr uint32_t BLINK = 1u << 1;    // ...and it is blinking
    static constexpr uint32_t VIBRATE = 1u << 2;  // vibrator on now, or switched on since the last state()
    static constexpr int ON_TIME_SHIFT = 8;       // bits 8..15: blink on time in 50 ms units, 0 = unknown
    static constexpr int PULSES_SHIFT = 16;       // bits 16..23: vibrator off->on count, mod 256

    // regs: the device's register bytes (DePage::leds), owned by the caller.
    explicit NotifyLeds(const uint8_t *regs) : regs(regs) {}

    // Called on the emulation thread after every guest write to the device (offset = byte written).
    // The driver stores on time, off time, then the mode byte, so a mode byte completes an update.
    void written(unsigned offset);

    // Current state as a bitmask (thread safe). consume=true clears the vibrate latch, so a pulse
    // shorter than the host's polling interval (touch feedback is ~20-50 ms) is reported once.
    uint32_t state(bool consume = true);

    // Turns everything off, as a board reset does (the driver does not clear the bytes at Init).
    void reset(uint8_t *mutableRegs);

private:
    uint32_t decode() const;
    const uint8_t *regs;
    std::atomic<bool> vibLatch{false};
    std::atomic<uint32_t> pulses{0};
    bool vibWasOn = false;
    uint32_t lastLogged = 0xFFFFFFFF;
};

}  // namespace wm
