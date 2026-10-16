// The Device Emulator "SMDK2410" machine: CPU, memory, peripherals, image loading and the run loop.
#pragma once
#include "../arm/cpu.h"
#include "bus.h"
#include "s3c2410.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace wm {

class Flash;
class Lcd;
class Touch;
class SpiKeyboard;
class EmulServ;
class DePage;
class Watchdog;
class AudioIis;
class AudioDma;
class FolderShare;
class Pcmcia;
class Camera;  // camera.h
class Modem;   // modem.h
class CodePatcher;  // codepatch.h
class StateIO;
uint32_t snapshotCrc(const uint8_t *p, size_t n);

struct MachineConfig {
    std::string imagePath;
    uint32_t memMB = 128;       // 64..256; above 64 MB lands in the 0x34000000 extension
    CpuCore core = CORE_ARMV5;
    int screenW = 240, screenH = 320, screenDepth = 16, rotation = 0;
    bool jit = true;
    // Time advances with executed instructions (5 ns each) instead of the host clock, so two
    // runs of the same image behave identically (used to check the JIT against the interpreter).
    bool deterministic = false;
};

class Machine {
public:
    explicit Machine(const MachineConfig &cfg);
    ~Machine();
    bool load(std::string &err);
    void hardReset();
    void softReset();

    // Runs the guest for about `ns` nanoseconds of host time. Returns false if the guest stopped.
    bool runFor(uint64_t ns);
    uint64_t nowNs() const;

    // Host input. Touch coordinates are guest screen pixels; keys are PC set-1 scancodes.
    void touch(bool down, int x, int y);
    /** A Windows virtual key (VK_*) pressed or released on the board keyboard. */
    void key(int vk, bool down);
    // Matrix code of the Fn key. The WM images' driver (kbdmouse.dll) treats VK 0xC1 as Fn, which is
    // code 0x64 (DE runs them with /funckey 193); 0x63 maps to nothing there.
    uint8_t fnCode = 0x64;

    bool snapshotFramebuffer(std::vector<uint32_t> &out, int &w, int &h);

    // Snapshots (quick boot / suspend). After a failed loadState the machine is inconsistent and
    // must be recreated.
    bool saveState(const std::string &path, std::string &err);
    bool loadState(const std::string &path, std::string &err);

    MachineConfig cfg;
    Bus bus;
    Cpu cpu;
    IntController intc;
    Gpio gpio;
    PwmTimers pwm;
    Uart uart0, uart1, uart2;
    ClockPower clk;
    Rtc rtc;
    RegFile memctl;
    std::unique_ptr<Watchdog> wdt;
    std::unique_ptr<Flash> flash;
    std::unique_ptr<Lcd> lcd;
    std::unique_ptr<Touch> adc;
    std::unique_ptr<SpiKeyboard> kbd;
    std::unique_ptr<EmulServ> emulServ;
    std::unique_ptr<DePage> dePage;
    std::unique_ptr<AudioIis> iis;
    std::unique_ptr<AudioDma> dma;
    std::unique_ptr<FolderShare> folderShare;
    std::unique_ptr<Pcmcia> pcmcia;  // network card slot
    std::unique_ptr<Camera> camera;  // WMDroid camera bridge (camera.h), at Camera::BASE
    std::unique_ptr<Modem> modem;    // virtual GSM modem on UART0 for rilgsm images (modem.h)
    std::unique_ptr<CodePatcher> codePatch;  // host fixes to driver code in guest RAM (codepatch.h)
    std::vector<uint8_t> ram;
    std::atomic<bool> stopRequested{false};
    uint64_t startHostNs = 0;
    uint64_t sliceInsns = 20000;
    uint32_t entryPa = 0;
    bool ce42Layout = false;    // WM2003 SE: CE 4.2 memory layout and BOOT_ARGS
    bool resetRequested = false;
    std::atomic<bool> bootComplete{false};
    uint64_t idleNs = 0;            // host time spent with the guest idle
    uint64_t idleWakes = 0;         // host sleeps while idle (each one a wake-up)
    bool idleCoalescing = true;     // see runFor
    bool lastRunIdle = false;
    bool lastRunHalfIdle = false;   // >= 50% idle: enough to coalesce while audio streams
    uint64_t virtNs = 0;            // deterministic clock

private:
    void writeBootArgs(bool soft);
    void serializeDevices(StateIO &s);
    void serializeWatchdog(StateIO &s);
    uint32_t imageSize = 0, imageCrc = 0;
    bool flashFromRam = false;   // B000FF images: flash is not a copy of the image file
    void updateBootState(uint64_t now);
    bool asleep(uint64_t now, uint64_t end);  // S3C2410 sleep (CLKCON POWER_OFF) and the wake-up reset
    bool sleepNoticed = false;
    uint32_t sleepPresses = 0;
    uint64_t windowStart = 0, windowIdle = 0;
    int idleSeconds = 0;
    std::vector<std::unique_ptr<Device>> owned;
};

}  // namespace wm
