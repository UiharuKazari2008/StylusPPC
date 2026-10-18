#include "machine.h"
#include "pcmcia.h"
#include "camera.h"  // WMDroid camera bridge
#include "modem.h"
#include "irda.h"
#include "codepatch.h"  // host fixes to driver code in guest RAM
#include "motion.h"  // accelerometer for the SEN0: driver
#include "audio.h"
#include "de_devices.h"
#include "folder_share.h"
#include "state.h"
#include "devices.h"
#include "../arm/jit.h"
#include "../util/log.h"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <thread>

namespace wm {

// Watchdog (0x53000000): DE turns "enable + reset" into a soft reset (the OAL's reboot path).
class Watchdog : public Device {
public:
    explicit Watchdog(Machine &m) : m(m) {}
    const char *name() const override { return "wdt"; }
    uint32_t read(uint32_t pa, int) override { return regs[(pa & 0xF) / 4]; }
    void write(uint32_t pa, uint32_t v, int) override {
        regs[(pa & 0xF) / 4] = v;
        if ((pa & 0xF) == 0 && (v & 0x21) == 0x21) {
            LOGI("watchdog: guest requested reset");
            m.resetRequested = true;  // handled between slices: see Machine::softReset
        }
    }
    void serialize(StateIO &s) {
        s.tag("WDT0");
        s.io(regs);
    }
    Machine &m;
    uint32_t regs[4] = {0x8021, 0x8000, 0x8000, 0};
};

namespace {

// Logs the first accesses to a block we have not modelled, then behaves like plain registers.
class Probe : public RegFile {
public:
    Probe(const char *n, uint32_t size) : RegFile(n, size) {}
    uint32_t read(uint32_t pa, int size) override {
        uint32_t v = RegFile::read(pa, size);
        if (count++ < 32) LOGW("%s: read%d %08x -> %08x", nm, size * 8, pa, v);
        return v;
    }
    void write(uint32_t pa, uint32_t v, int size) override {
        if (count++ < 32) LOGW("%s: write%d %08x <- %08x", nm, size * 8, pa, v);
        RegFile::write(pa, v, size);
    }
    int count = 0;
};

uint64_t hostNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

template <typename T>
void put(std::vector<uint8_t> &ram, uint32_t pa, T v) {
    memcpy(&ram[pa - 0x30000000u], &v, sizeof v);
}

// Windows virtual key -> SMDK2410 keyboard matrix code (the BSP driver's table, inverted).
uint8_t vkCode(int vk) {
    static const struct { uint8_t vk, code; } kMap[] = {
        {0x08, 0x69}, {0x09, 0x0B}, {0x0D, 0x5A}, {0x10, 0x12}, {0x11, 0x19}, {0x12, 0x01}, {0x14, 0x2C},
        {0x1B, 0x29}, {0x20, 0x6E}, {0x25, 0x6D}, {0x26, 0x6C}, {0x27, 0x6F}, {0x28, 0x6A}, {0x2E, 0x2A},
        {'0', 0x49}, {'1', 0x31}, {'2', 0x39}, {'3', 0x2F}, {'4', 0x37}, {'5', 0x3F}, {'6', 0x47},
        {'7', 0x4F}, {'8', 0x57}, {'9', 0x41},
        {'A', 0x0D}, {'B', 0x3E}, {'C', 0x2E}, {'D', 0x35}, {'E', 0x3B}, {'F', 0x3D}, {'G', 0x45},
        {'H', 0x4D}, {'I', 0x52}, {'J', 0x55}, {'K', 0x44}, {'L', 0x4C}, {'M', 0x4E}, {'N', 0x46},
        {'O', 0x4B}, {'P', 0x53}, {'Q', 0x2B}, {'R', 0x43}, {'S', 0x2D}, {'T', 0x3A}, {'U', 0x4A},
        {'V', 0x36}, {'W', 0x33}, {'X', 0x0E}, {'Y', 0x42}, {'Z', 0x0C}, {0x5B, 0x60}, {0x5C, 0x5F},
        {0xA0, 0x12}, {0xA1, 0x62}, {0xA2, 0x19}, {0xA3, 0x19}, {0xA4, 0x01}, {0xA5, 0x01},
        {0xBA, 0x54}, {0xBB, 0x59}, {0xBC, 0x56}, {0xBD, 0x51}, {0xBE, 0x5E}, {0xBF, 0x5D},
        {0xC0, 0x63}, {0xC1, 0x64}, {0xC2, 0x65}, {0xC3, 0x66}, {0xC4, 0x67}, {0xC5, 0x68},
        {0xDB, 0x5B}, {0xDC, 0x0A}, {0xDD, 0x6B}, {0xDE, 0x5C},
    };
    for (auto &k : kMap)
        if (k.vk == vk) return k.code;
    return 0;
}

// Keys the matrix lacks are typed as Fn (Machine::fnCode) + another key: F1-F10 = Fn+1..0
// (WM: F1/F2 soft keys, F3 Talk, F4 End, F6/F7 volume up/down), Home/PgUp/PgDn/End = Fn+arrows.
int fnVk(int vk) {
    if (vk >= 0x70 && vk <= 0x78) return '1' + (vk - 0x70);
    if (vk == 0x79) return '0';
    switch (vk) {
    case 0x24: return 0x25;
    case 0x21: return 0x26;
    case 0x22: return 0x28;
    case 0x23: return 0x27;
    case 0x2D: return 0x50;
    default: return 0;
    }
}

}  // namespace

Machine::Machine(const MachineConfig &c)
    : cfg(c), cpu(bus), intc(cpu), gpio(intc), pwm(intc), uart0(intc, 0), uart1(intc, 1), uart2(intc, 2),
      clk(cpu), rtc(intc), memctl("memctl", 0x100) {
    cpu.core = cfg.core;
    if (cfg.jit) {
        cpu.jit = jitCreate(cpu);
        cpu.jitEnabled = cpu.jit != nullptr;
        if (!cpu.jit) LOGW("JIT unavailable, using the interpreter");
    }
    if (cfg.deterministic) rtc.virtNs = &virtNs;
    bus.onMapChange = [this] { cpu.flushTlb(); };
    bus.pcNow = [this] { return cpu.pc() - (cpu.getCpsr() & 0x20 ? 4 : 8); };
    wdt.reset(new Watchdog(*this));
    lcd.reset(new Lcd(bus, intc));
    adc.reset(new Touch(intc));
    kbd.reset(new SpiKeyboard(intc));
    emulServ.reset(new EmulServ(gpio));
    Uart *u[3] = {&uart0, &uart1, &uart2};
    dePage.reset(new DePage(u, *emulServ));
    folderShare.reset(new FolderShare(bus, *emulServ));
    pcmcia.reset(new Pcmcia(intc, gpio));
    camera.reset(new Camera(cpu, bus));  // WMDroid camera bridge (mapped in load())
    camera->panelPortrait = cfg.screenW <= cfg.screenH;  // the sensor is fixed to the panel
    dePage->folderShare = [this](uint32_t code, uint32_t pb) { return folderShare->call(code, pb); };
    iis.reset(new AudioIis());
    dma.reset(new AudioDma(bus, intc));
    modem.reset(new Modem());
    modem->toGuest = [this](const std::string &s) { for (char c : s) uart0.feed((uint8_t)c); };
    uart0.peer = [this](uint8_t c) { modem->fromGuest(c); };
    irda.reset(new Irda(uart2));  // IR port: UART2 as COM2: for irsir.dll (docs/research-irda.md)
    uart2.peer = [this](uint8_t c) { irda->fromGuest(c); };
    uart2.txLevel = true;  // the IR driver writes whole frames: level TX interrupt (s3c2410.h)
    codePatch.reset(new CodePatcher(cpu, bus));
    motion.reset(new Motion());
    sideband.reset(new Sideband());  // command, status and bulk pipes (mapped in load())
    cpu.codeScanCtx = codePatch.get();
    cpu.codeScan = [](void *ctx, uint32_t pa) { static_cast<CodePatcher *>(ctx)->scanPage(pa); };
}

Machine::~Machine() {
    if (cpu.jit) jitDestroy(cpu.jit);
}

uint64_t Machine::nowNs() const { return cfg.deterministic ? virtNs : hostNs() - startHostNs; }

bool Machine::load(std::string &err) {
    std::ifstream f(cfg.imagePath, std::ios::binary);
    if (!f) { err = "cannot open " + cfg.imagePath; return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    imageSize = (uint32_t)data.size();
    imageCrc = snapshotCrc(data.data(), data.size() < (1u << 20) ? data.size() : (1u << 20));

    if (cfg.memMB < 64) cfg.memMB = 64;
    if (cfg.memMB > 256) cfg.memMB = 256;
    ram.assign((size_t)cfg.memMB << 20, 0);
    bus.mapMemory(0x30000000, (uint32_t)ram.size(), ram.data(), true);
    codePatch->setRam(ram.data(), (uint32_t)ram.size());

    // NOR flash bank 0: 128 MB on DE 2.0/3.0, erased beyond the image.
    std::vector<uint8_t> nor(128u << 20, 0xFF);

    if (data.size() > 15 && memcmp(data.data(), "B000FF\n", 7) == 0) {
        struct Rec { uint32_t va, len; size_t off; };
        std::vector<Rec> recs;
        uint32_t entryVa = 0;
        size_t off = 15;
        while (off + 12 <= data.size()) {
            uint32_t va, len;
            memcpy(&va, &data[off], 4);
            memcpy(&len, &data[off + 4], 4);
            off += 12;
            if (va == 0) { entryVa = len; break; }
            if (off + len > data.size()) { err = "truncated B000FF record"; return false; }
            recs.push_back({va, len, off});
            off += len;
        }
        // Pick DE's VA->PA rule from ROMHDR.ulRAMStart (found via the ECEC record).
        auto readVa = [&](uint32_t va, uint32_t &out) {
            for (auto &r : recs)
                if (va >= r.va && va + 4 <= r.va + r.len) { memcpy(&out, &data[r.off + (va - r.va)], 4); return true; }
            return false;
        };
        uint32_t ramStart = 0;
        for (auto &r : recs) {
            if (r.len == 8 && memcmp(&data[r.off], "ECEC", 4) == 0) {
                uint32_t toc;
                memcpy(&toc, &data[r.off + 4], 4);
                readVa(toc + 0x14, ramStart);
                break;
            }
        }
        uint32_t delta;
        if (ramStart >= 0x92000000u) delta = 0x92000000u;
        else if (ramStart >= 0x8C000000u) { delta = 0x4E000000u; ce42Layout = true; }
        else delta = 0x50000000u;
        for (auto &r : recs) {
            uint32_t pa = r.va - delta;
            if (pa >= 0x30000000u && pa - 0x30000000u + r.len <= ram.size()) memcpy(&ram[pa - 0x30000000u], &data[r.off], r.len);
            else if (pa + r.len <= nor.size()) memcpy(&nor[pa], &data[r.off], r.len);
            else { err = "B000FF record outside memory"; return false; }
        }
        entryPa = entryVa - delta;
        flashFromRam = true;
        LOGI("B000FF image: %zu records, RAMStart=%08x, entry va=%08x pa=%08x%s", recs.size(), ramStart, entryVa,
             entryPa, ce42Layout ? " (CE 4.2 layout)" : "");
    } else {
        uint32_t w0;
        memcpy(&w0, data.data(), 4);
        if ((w0 & 0xEA000000u) != 0xEA000000u) { err = "not a B000FF or NB0 image"; return false; }
        if (data.size() > nor.size()) { err = "image larger than 128 MB flash"; return false; }
        memcpy(nor.data(), data.data(), data.size());
        entryPa = 0;
        LOGI("NB0 image: %zu MB in NOR flash", data.size() >> 20);
    }
    data.clear();
    data.shrink_to_fit();
    flash.reset(new Flash(bus, 0, std::move(nor)));
    bus.mapMemory(0, (uint32_t)flash->mem.size(), flash->mem.data(), false, flash.get());

    auto own = [this](Device *d) { owned.emplace_back(d); return d; };
    own(new Probe("pcmcia", 0x100));  // unmapped: keeps the snapshot layout of states saved before the card existed
    bus.mapDevice(0x10000000, 0x2000000, pcmcia.get());
    bus.mapDevice(0x18000000, 0x2000000, own(new Probe("cs8900", 0x200)));
    bus.mapDevice(0x48000000, 0x100000, &memctl);
    bus.mapDevice(0x49000000, 0x100000, own(new Probe("usbhost", 0x100)));
    bus.mapDevice(0x4A000000, 0x100000, &intc);
    bus.mapDevice(0x4B000000, 0x100000, dma.get());
    bus.mapDevice(0x4C000000, 0x100000, &clk);
    bus.mapDevice(0x4D000000, 0x100000, lcd.get());
    bus.mapDevice(0x4E000000, 0x100000, own(new Probe("nand", 0x100)));
    bus.mapDevice(0x50000000, 0x100000, dePage.get());
    bus.mapDevice(0x51000000, 0x100000, &pwm);
    bus.mapDevice(0x52000000, 0x100000, own(new Probe("usbdev", 0x400)));
    bus.mapDevice(0x53000000, 0x100000, wdt.get());
    bus.mapDevice(0x54000000, 0x100000, own(new Probe("iic", 0x100)));
    bus.mapDevice(0x55000000, 0x100000, iis.get());
    bus.mapDevice(0x56000000, 0x100000, &gpio);
    bus.mapDevice(0x57000000, 0x100000, &rtc);
    bus.mapDevice(0x58000000, 0x100000, adc.get());
    bus.mapDevice(0x59000000, 0x100000, kbd.get());
    bus.mapDevice(0x5A000000, 0x100000, own(new Probe("sdi", 0x100)));
    bus.mapDevice(Camera::BASE, 0x100000, camera.get());  // WMDroid camera bridge (no snapshot state)
    bus.mapDevice(CodePatcher::BASE, 0x100000, codePatch.get());  // host code fixes (no snapshot state)
    bus.mapDevice(Motion::BASE, 0x100000, motion.get());  // accelerometer (no snapshot state)
    bus.mapDevice(Sideband::BASE, 0x100000, sideband.get());  // command/status/bulk pipes (no snapshot state)

    hardReset();
    return true;
}

void Machine::writeBootArgs(bool soft) {
    // BSP_ARGS screen block, read by the CE 5+ OAL to program the LCD.
    put<uint32_t>(ram, 0x30020044, 0xDE12DE34);
    put<uint16_t>(ram, 0x30020048, (uint16_t)cfg.screenW);
    put<uint16_t>(ram, 0x3002004A, (uint16_t)cfg.screenH);
    put<uint16_t>(ram, 0x3002004C, (uint16_t)cfg.screenDepth);
    put<uint16_t>(ram, 0x3002004E, soft ? 1 : 0);
    put<uint8_t>(ram, 0x30020050, (uint8_t)(cfg.rotation & 3));
    if (ce42Layout) {  // CE 4.2 BOOT_ARGS
        put<uint32_t>(ram, 0x301FF004, 0x544F4F42);
        put<uint16_t>(ram, 0x301FF072, (uint16_t)cfg.screenW);
        put<uint16_t>(ram, 0x301FF074, (uint16_t)cfg.screenH);
        put<uint16_t>(ram, 0x301FF078, (uint16_t)cfg.screenDepth);
        put<uint8_t>(ram, 0x301FF080, soft ? 1 : 0);
    }
}

void Machine::hardReset() {
    modem->reset();
    irda->reset();
    codePatch->onGuestReset();
    motion->onGuestReset();
    cpu.reset();
    gpio.gstatus2 = 0;
    writeBootArgs(false);
    cpu.setPc(entryPa);
    startHostNs = hostNs();
    resetRequested = false;
}

// Host-initiated restart (the app's Restart / Reset buttons). DE's EINT2 "reset button" only works
// while the guest's interrupt handler is healthy, and WM 6.5 ignores it anyway (it logged a fresh
// "boot complete" without restarting). This does what the OAL's own reboot does instead: the core
// comes out of reset (SVC, interrupts off, MMU off) at the image entry with RAM kept and a soft
// reset flagged in the boot arguments, so the kernel boots again and StartUp runs the helper.
void Machine::softReset(bool warm) {
    LOGI("%s reset (reboot from pc=%08x)", warm ? "soft" : "cold", cpu.pc());
    folderShare->onGuestReset();
    dePage->nled.reset(dePage->leds);  // notification LED / vibrator off (nled.h)
    modem->reset();
    irda->reset();
    codePatch->onGuestReset();
    motion->onGuestReset();
    clk.clkcon &= ~8u;     // not asleep
    sleepNoticed = false;
    gpio.gstatus2 &= ~2u;  // not a resume
    lcd->intmsk = 3;
    lcd->lpcsel = 4;
    adc->adccon = 0x3FC4;
    adc->adctsc = 0x58;
    adc->adcdly = 0xFF;
    intc.intmsk = 0xFFFFFFFF;  // what the reset state is; the boot code unmasks what it uses
    intc.intsubmsk = 0x7FF;
    intc.srcpnd = intc.intpnd = intc.subsrcpnd = 0;
    intc.intoffset = 0;
    writeBootArgs(warm);
    cpu.reset();
    cpu.setPc(entryPa);
    intc.update();
    bootComplete = false;  // detect the home screen again after the guest restarts
    idleSeconds = 0;
    oneTierScans = kOneTierScans;
    resetRequested = false;
}

bool Machine::runFor(uint64_t ns) {
    uint64_t end = nowNs() + ns;
    // While the guest is halted, its 1 ms tick does not wake the host. The sleep runs to the end
    // of this slice (the owed ticks are delivered in a burst once the guest runs again) and never
    // past the next audio DMA event, so playback keeps its sample clock. A guest that is actually
    // running is not halted and is not delayed.
    bool coalesce = idleCoalescing && !cfg.deterministic;
    modem->service(nowNs());  // host updates and unsolicited codes, once per slice batch
    while (!stopRequested) {
        uint64_t now = nowNs();
        pwm.advance(now);
        rtc.advance(now);
        dma->advance(now);
        irda->service(now);  // frames from other devices into UART2's receiver
        if (resetRequested) softReset();
        if (cpu.mmuDisabled) {
            // The OAL's reboot turns the MMU off and jumps to the boot code: a warm restart that
            // never touches the watchdog. Forget guest-side state the way a reset would.
            cpu.mmuDisabled = false;
            LOGI("guest restarting (MMU off)");
            folderShare->onGuestReset();
            dePage->nled.reset(dePage->leds);  // notification LED / vibrator off (nled.h)
            modem->reset();
            irda->reset();
            bootComplete = false;
            idleSeconds = 0;
            oneTierScans = kOneTierScans;
        }
        updateBootState(now);
        if (now >= end) break;
        if (asleep(now, end)) continue;  // suspended (CLKCON POWER_OFF) until the power button
        if (cpu.halted && !cpu.irqPending()) {
            uint64_t pwmNext = pwm.nextEventNs();
            if (coalesce && pwmNext > now) pwmNext = std::max(pwmNext, end);
            uint64_t next = std::min(pwmNext, dma->nextEventNs());
            if (next > end) next = end;
            uint64_t wait = next > now ? next - now : 0;
            if (cfg.deterministic) {
                virtNs += wait ? wait : 1000;
                idleNs += wait ? wait : 1000;
                continue;
            }
            if (wait > 20000000) wait = 20000000;
            if (wait) std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
            idleWakes++;
            idleNs += nowNs() - now;
            continue;
        }
        bus.lastPc = cpu.pc();
        uint64_t slice = sliceInsns;
        if (cfg.deterministic && (end - now) / 5 < slice) slice = (end - now) / 5 + 1;
        uint64_t ran = cpu.run(slice);
        if (cfg.deterministic) virtNs += ran * 5;
    }
    return !stopRequested;
}

// Suspend: WM's OEMPowerOff saves its state in RAM (resume address, TTB, DACR, control at PA
// 0x30020800, checksum in GSTATUS3), then runs the code at flash 0x1004 whose third store sets
// CLKCON POWER_OFF and the chip sleeps (s3c2410.cpp). Nothing runs while asleep. A power button
// press is the wake-up: the core comes out of reset (SVC, MMU off, at the image entry) with RAM
// and the peripherals intact and GSTATUS2 OFFRST set, so the boot code at 0x10B0 verifies the
// saved block and jumps back into the OAL. Without this the CPU ran on past the sleep store into
// the cold-boot code with the MMU on and faulted forever at the abort vector.
bool Machine::asleep(uint64_t now, uint64_t end) {
    if (!(clk.clkcon & 8)) { sleepNoticed = false; return false; }
    if (!sleepNoticed) {  // only presses after falling asleep wake it (also after a state restore)
        sleepNoticed = true;
        sleepPresses = gpio.powerPresses;
        LOGI("asleep at pc=%08x", cpu.pc());
    }
    if (gpio.powerPresses != sleepPresses) {
        LOGI("wake from sleep (power button)");
        clk.clkcon &= ~8u;
        sleepNoticed = false;
        cpu.reset();
        cpu.setPc(entryPa);
        gpio.gstatus2 |= 2;  // OFFRST: this reset is a wake-up from power-off
        intc.update();       // drive the interrupt lines again (the boot code masks them)
        return false;
    }
    uint64_t wait = end - now;
    if (cfg.deterministic) virtNs += wait;
    else std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<uint64_t>(wait, 20000000)));
    idleWakes++;
    idleNs += nowNs() - now;
    return true;
}

void Machine::updateBootState(uint64_t now) {
    // Boot is done once the guest has been at least 80% idle in each of 10 consecutive seconds.
    // Boot itself contains shorter idle stretches (driver timeouts), so a single window is not enough.
    if (oneTierScans > 0 && now >= oneTierNext) {  // Standard images (oneTierPolicy)
        oneTierNext = now + 1000000000ull;
        oneTierScans--;
        oneTierPolicy();
    }
    if (bootComplete) return;
    uint64_t span = now - windowStart;
    if (span < 1000000000ull) return;
    uint64_t idle = idleNs - windowIdle;
    if (lcd->enabled() && idle * 100 >= span * 80) idleSeconds++;
    else idleSeconds = 0;
    windowStart = now;
    windowIdle = idleNs;
    if (idleSeconds >= 10) {
        bootComplete = true;
        LOGI("boot complete after %.1f s", now / 1e9);
    }
}

// Security policy 4123 (0x101B, Privileged Applications) = 1: every program that runs is privileged
// ("one-tier"), as on the Professional images. The Standard (Smartphone) images ship two-tier
// (0x101B = 0), so the unsigned guest helper runs with normal trust and every write to a protected
// registry key or \Windows fails (docs/research-standard.md). The value lives in the registry hive
// in guest RAM: its cell is u16 type (REG_DWORD 4), u16 data size (4), u8 name length (8), u8 flags,
// the UTF-16 name "0000101b", then the DWORD. Rewritten in place once a second during boot (the
// hive and its object-store copies appear ~15 s in; the helper starts after the home screen).
void Machine::oneTierPolicy() {
    static const uint8_t kName[] = {'0', 0, '0', 0, '0', 0, '0', 0, '1', 0, '0', 0, '1', 0, 'b', 0};
    static const uint8_t kHead[] = {4, 0, 4, 0, 8};
    uint8_t *begin = ram.data(), *end = ram.data() + ram.size();
    int patched = 0;
    // memchr for the rare last letter is far faster than a byte-wise search through mostly-zero RAM.
    for (uint8_t *b = begin + 6 + 14; b < end - 6;) {
        b = (uint8_t *)memchr(b, 'b', end - 6 - b);
        if (!b) break;
        uint8_t *p = b - 14;
        b++;
        if (memcmp(p, kName, sizeof kName) || memcmp(p - 6, kHead, sizeof kHead)) continue;
        uint8_t *v = p + sizeof kName;
        uint32_t old;
        memcpy(&old, v, 4);
        if (old != 0) continue;
        const uint32_t one = 1;
        memcpy(v, &one, 4);
        patched++;
    }
    if (patched) LOGI("security policy: Privileged Applications set to one-tier (%d hive cells)", patched);
}

void Machine::touch(bool down, int x, int y) {
    int w = cfg.screenW, h = cfg.screenH;
    if (lcd->width() > 1 && lcd->height() > 1) { w = lcd->width(); h = lcd->height(); }
    // DE's sample scaling, which the images' default calibration expects.
    int sx = 90 + x * 875 / w;
    int sy = 920 - y * 870 / h;
    adc->setPen(down, sx, sy);
}

void Machine::key(int vk, bool down) {
    uint8_t up = down ? 0 : 0x80;
    // Application buttons (VK_APP1-5) are Win + their code; the driver wants Win down at both
    // edges. Win is held from the press to the release: a separate Win tap around the release
    // reads to the shell as a lone Win press and opens the Start menu.
    // VK_APP1 (0xC1) has no key: kbdmouse.dll swallows VK 0xC1 as its Fn key whichever matrix code
    // (0x21, 0x64) gives it, so the app sends Button 1 as its action's key (Keymap.HwButtons).
    if (vk == 0xC1) {
        LOGD("key: VK_APP1 is the keyboard driver's Fn key; not sent");
        return;
    }
    if (vk >= 0xC2 && vk <= 0xC5) {
        if (down) {
            kbd->push(0x60);
            kbd->push(vkCode(vk));
        } else {
            kbd->push(vkCode(vk) | 0x80);
            kbd->push(0x60 | 0x80);
        }
        return;
    }
    if (uint8_t c = vkCode(vk)) {
        kbd->push(c | up);
        return;
    }
    // Fn keys: Fn is pressed and released around both the press and the release of the key.
    if (int base = fnVk(vk)) {
        kbd->push(fnCode);
        kbd->push(vkCode(base) | up);
        kbd->push(fnCode | 0x80);
        return;
    }
    LOGD("key: no mapping for vk %02x", vk);
}

bool Machine::snapshotFramebuffer(std::vector<uint32_t> &out, int &w, int &h) {
    w = lcd->width();
    h = lcd->height();
    if (w <= 1 || h <= 1) return false;
    uint32_t pa = lcd->framebufferPa();
    bool wr;
    int bpp = lcd->bpp();
    uint32_t stride = (uint32_t)w * (bpp == 16 ? 2 : bpp == 24 ? 3 : 4);
    out.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
        uint8_t *row = bus.hostPage(pa + (uint32_t)y * stride, wr);
        if (!row) return false;
        for (int x = 0; x < w; x++) {
            uint32_t c;
            if (bpp == 16) {
                uint16_t p;
                memcpy(&p, row + x * 2, 2);
                uint32_t r = (p >> 11) & 0x1F, g = (p >> 5) & 0x3F, b = p & 0x1F;
                c = 0xFF000000 | ((r * 255 / 31) << 16) | ((g * 255 / 63) << 8) | (b * 255 / 31);
            } else if (bpp == 24) {
                c = 0xFF000000 | (row[x * 3 + 2] << 16) | (row[x * 3 + 1] << 8) | row[x * 3];
            } else {
                memcpy(&c, row + x * 4, 4);
                c |= 0xFF000000;
            }
            out[(size_t)y * w + x] = c;
        }
    }
    return true;
}

}  // namespace wm

namespace wm {
void Machine::serializeWatchdog(StateIO &s) { wdt->serialize(s); }
}  // namespace wm
