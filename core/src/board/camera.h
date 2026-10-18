// WMDroid camera bridge (a WMDroid-only device; DE has no camera).
//
// The WM 6.x images ship Microsoft's sample camera driver NULLCAM.dll (CAM1:/PIN1:). It makes every
// frame by calling one generator function, which it loads per pin open from the registry
// (HKLM\Drivers\Capture\NullCam LibraryName/FunctionName; docs/research-camera.md §1.3). The guest
// helper installs \Windows\wmdcam.dll there (guest/Helper/wmdcam.s). That DLL maps this device with
// VirtualCopy and, for each frame, hands over the guest's buffer and CS_VIDEOINFOHEADER addresses;
// the device writes a frame of exactly the requested size and format straight into guest memory.
//
// Registers (physical page Camera::BASE):
//   +0x00 ID      r   'WCAM'
//   +0x04 CTRL    w   1 = the driver loaded the DLL (camera opening), 0 = unloaded (camera closed)
//   +0x08 BUF     rw  frame buffer VA (current process)
//   +0x0C VIH     rw  CS_VIDEOINFOHEADER VA
//   +0x10 GO      w   fill BUF as VIH describes
//   +0x14 RESULT  r   bytes written (DataUsed); 0x80000000|offset = that page of BUF is not
//                     committed yet (the DLL touches it and retries); 0xFFFFFFFF = unsupported
//   +0x18 ORIENT  r   the sensor's orientation: 1 = portrait, 2 = landscape (the panel's; the
//                     helper gives pimg's registry sizes the same orientation, which it requires)
//                 w   0x100 from the DLL, inside device.exe on each open: NULLCAM's format table is
//                     turned to that orientation now (its page-table entries are only reliably live
//                     there); 0x200 from the helper, every half second: the registry is in step.
//                     The table is only turned after 0x200 in this run: a mismatch crashes pimg.
//   +0x1C FLASH   rw  bits 0..7: Pictures & Videos flash mode (HKCU\...\Camera\User\Flash, written
//                     by the helper): 0 auto, 1 on, 2 off (the menu order). Bit 8: HTC Flashlight
//                     is holding the LED on (guest/Flash/flashlight.c). The host lights the phone's
//                     flash from these.
//
// Orientation: NULLCAM's formats are BI_SRCPREROTATE, so frames go onto the panel as they are,
// whatever Windows Mobile's own screen rotation. A real phone's sensor is fixed to its panel too:
// the sensor is portrait on a portrait panel, and the host turns each frame so it shows the world
// the right way round on the panel as the phone displays it (CameraBridge.kt).
//
// Host side: frames are pushed from any thread (the Android camera, or a still picture in the
// CLI), turned upright, then cropped and scaled to each size the guest asked for recently, with an
// optional 2009-era look. Without a host camera the guest gets an animated test pattern.
//
// No snapshot state: the open/close notion is rebuilt from the guest's frame requests.
#pragma once
#include "bus.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace wm {

class Cpu;

class Camera : public Device {
public:
    static constexpr uint32_t BASE = 0x5F000000;   // own 1 MB region, unused on the SMDK2410
    static constexpr uint32_t ID = 0x4D414357;     // 'WCAM'

    Camera(Cpu &cpu, Bus &bus) : cpu(cpu), bus(bus) {}
    const char *name() const override { return "camera"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;

    // ---- host side (thread-safe) ----

    /** The guest has the camera open: the host camera should run. Drops 2 s after the last frame. */
    bool wanted() const;
    /** A host camera is attached: show black (not the test pattern) until its first frame. */
    void setHostCamera(bool present);
    /** Forget the last host frame (call when the host camera closes). */
    void clearFrames();
    /** The 2009-era look: soft lens, sensor noise, warm cast, low dynamic range, vignetting. */
    /** Strength 0..100 (0 = off): every stage of the look scales with it. */
    void setEra(int level) { era = level < 0 ? 0 : level > 100 ? 100 : level; }
    bool eraOn() const { return era > 0; }
    /**
     * Flash request from the guest (register +0x1C). Low byte: 0 auto, 1 on, 2 off, the
     * Pictures & Videos menu order. Bit 8: an app such as HTC Flashlight is holding the LED on.
     */
    int flash() const { return flashMode.load(); }

    /**
     * A camera frame in YUV 420 (Android YUV_420_888: any pixel stride for the chroma planes).
     * `rotation` is the clockwise turn (0/90/180/270) that makes it upright; `mirror` flips it
     * horizontally afterwards (front camera).
     */
    void pushYuv420(const uint8_t *y, int yRowStride, const uint8_t *u, const uint8_t *v, int uvRowStride,
                    int uvPixelStride, int w, int h, int rotation, bool mirror);
    /** A frame as packed RGB888 rows. */
    void pushRgb(const uint8_t *rgb, int w, int h, int rotation, bool mirror);

    // Counters for logs/status.
    std::atomic<uint64_t> framesServed{0};

    /** The guest's LCD panel is portrait: so is the sensor (set by the machine from its screen). */
    bool panelPortrait = true;

private:
    enum Fmt { FMT_RGB565 = 1, FMT_YV12 = 2 };
    struct Target {
        int w = 0, h = 0, fmt = 0;
        bool bottomUp = false;
        bool operator==(const Target &o) const { return w == o.w && h == o.h && fmt == o.fmt && bottomUp == o.bottomUp; }
        size_t bytes() const { return fmt == FMT_RGB565 ? (size_t)w * h * 2 : (size_t)w * h * 3 / 2; }
    };
    struct Image {
        int w = 0, h = 0;
        std::vector<uint8_t> rgb;
    };
    struct Packed {
        Target t;
        uint64_t seq = 0;          // source frame it was made from
        uint64_t lastAsked = 0;    // host ns
        std::shared_ptr<const std::vector<uint8_t>> data;
    };

    uint32_t fill();
    bool translate(uint32_t va, bool write, uint32_t &pa);
    bool readGuest(uint32_t va, void *out, uint32_t n);
    uint8_t *hostVa(uint32_t va);   // any process slot, ignoring protection (page-table walk)
    std::vector<uint32_t> ranges;   // VAs of NULLCAM's data ranges in device.exe
    bool driverScanTried = false;   // searched device.exe during this camera open
    bool registryInStep = false;    // the helper said pimg's registry sizes match (this run)
    void orientFormats();
    std::shared_ptr<const std::vector<uint8_t>> frameFor(const Target &t);
    std::shared_ptr<const std::vector<uint8_t>> render(const Image &src, const Target &t, uint64_t salt);
    void publish(std::shared_ptr<const Image> img);
    bool takeFrame();
    void makePattern(Image &img, uint64_t ns);
    void noteOpen(bool open);

    Cpu &cpu;
    Bus &bus;
    uint32_t buf = 0, vih = 0, result = 0;
    bool loggedFormat = false;

    mutable std::mutex mu;                 // guards src, seq, cache
    std::shared_ptr<const Image> src;
    uint64_t seq = 0;
    std::vector<Packed> cache;
    std::atomic<bool> hostCamera{false};
    std::atomic<int> era{0};
    std::atomic<int> flashMode{0};
    std::atomic<bool> attached{false};
    std::atomic<uint64_t> attachNs{0}, lastFillNs{0};
    std::atomic<uint64_t> renderSalt{0};
    struct Smear {             // the 2009 look's previous frame per size (motion smear)
        int w, h;
        std::vector<uint8_t> rgb;
    };
    std::mutex smearMu;
    std::vector<Smear> smear;
    uint64_t eraNextNs = 0;    // the 2009 look takes about 12 frames a second
};

}  // namespace wm
