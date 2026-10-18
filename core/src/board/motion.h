// WMDroid motion sensor (a WMDroid-only device; DE has no accelerometer).
//
// HTC's G-sensor apps (Teeter, the compass/level toys, auto-rotate) talk to HTC's sensor stream
// driver SEN0: (HTCSensorService.dll on the HD2). PocketStylus ships its own SEN0: driver
// (guest/Sensor/htcsensor.c, installed as \Windows\HTCSensorService.dll); it maps this device with
// VirtualCopy and answers the tilt IOCTL from it (docs/research-hd2-port.md, "Teeter sensor").
//
// Registers (physical page Motion::BASE):
//   +0x00 ID      r   'MOTN'
//   +0x04 OPEN    w   the driver's open-handle count (a hint: the host sensor starts at once)
//   +0x08 SEQ     r   samples received from the host so far
//   +0x0C X       r   gravity in HTC's units and frame (1000 = 1 g; the panel held portrait:
//   +0x10 Y       r     x to the right, y toward the bottom, z out of the screen; a phone lying
//   +0x14 Z       r     face up reads 0, 0, -1000; left edge down gives x < 0, top edge down y < 0)
//   +0x18 FLAGS   r   bit 0: a host sensor feeds the values (else they stay at face up)
//   +0x1C IOCTL   w   an IOCTL code the driver doesn't know (logged once per code, for research)
//
// Reading X, Y or Z counts as use: the host sensor runs while the guest read one in the last two
// seconds, or while OPEN is non-zero (cleared by a guest reset). No snapshot state.
#pragma once
#include "bus.h"
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>

namespace wm {

class Motion : public Device {
public:
    static constexpr uint32_t BASE = 0x5F200000;   // own 1 MB region after the code patcher's
    static constexpr uint32_t ID = 0x4E544F4D;     // 'MOTN'

    const char *name() const override { return "motion"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;

    // ---- host side (thread-safe) ----

    /** The guest uses the sensor: the host accelerometer should run. */
    bool wanted() const;
    /**
     * One accelerometer sample as Android reports it (m/s^2, the force that holds the phone up:
     * +9.8 on z lying face up), already turned into the guest panel's portrait frame.
     */
    void setAccel(float ax, float ay, float az);
    /** No host sensor any more: back to the face-up default. */
    void clearAccel();
    void onGuestReset() { open = 0; }

    uint32_t reads() const { return readCount; }   // diagnostics

private:
    std::atomic<int32_t> x{0}, y{0}, z{-1000};
    std::atomic<uint32_t> seq{0}, open{0}, readCount{0};
    std::atomic<bool> host{false};
    std::atomic<uint64_t> lastReadNs{0};
    std::mutex mu;
    std::set<uint32_t> unknownCodes;   // logged already (mu)
};

}  // namespace wm
