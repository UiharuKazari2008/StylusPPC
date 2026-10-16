// Machine snapshots: every device's state, RAM (zero pages skipped) and the flash pages that differ
// from the ROM image. Used for quick boot and suspend/resume.
#include "machine.h"
#include "pcmcia.h"
#include "modem.h"
#include "audio.h"
#include "de_devices.h"
#include "devices.h"
#include "folder_share.h"
#include "state.h"
#include "../arm/jit.h"
#include "../util/log.h"
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <unistd.h>

namespace wm {

// ---------------------------------------------------------------------------------------------
// Devices

void Cpu::serialize(StateIO &s) {
    s.tag("CPU0");
    s.io(r);
    s.io(fN); s.io(fZ); s.io(fC); s.io(fV); s.io(fQ);
    s.io(mode); s.io(fI); s.io(fF); s.io(fT); s.io(spsr);
    s.io(bankUsr); s.io(bankFiq); s.io(bankSP); s.io(bankLR); s.io(bankSPSR);
    s.io(cp15_ctrl); s.io(cp15_ttb); s.io(cp15_dacr); s.io(cp15_fsr); s.io(cp15_far);
    s.io(cp15_ifsr); s.io(cp15_pid); s.io(cp15_tid);
    s.io(halted); s.io(insnCount);
    if (!s.writing) {
        prefetchLeft = 0;
        flushTlb();
        if (jit) jitFlush(jit);
        if (!scanned.empty()) std::fill(scanned.begin(), scanned.end(), 0);  // host code fixes again
    }
}

void IntController::serialize(StateIO &s) {
    s.tag("INTC");
    s.io(srcpnd); s.io(intmod); s.io(intmsk); s.io(priority); s.io(intpnd); s.io(intoffset);
    s.io(subsrcpnd); s.io(intsubmsk); s.io(levels);
    if (!s.writing) update();
}

void Gpio::serialize(StateIO &s) {
    s.tag("GPIO");
    s.io(regs); s.io(eintmask); s.io(eintpend); s.io(gpfdatIn); s.io(gpgdatIn); s.io(gstatus2);
}

void PwmTimers::serialize(StateIO &s) {
    s.tag("PWMT");
    s.io(pclkHz); s.io(tcfg0); s.io(tcfg1); s.io(tcon); s.io(tcntb); s.io(tcmpb);
    for (T &x : t) { s.io(x.running); s.time(x.startNs); s.io(x.startCount); }
    s.time(now);
}

void Uart::serialize(StateIO &s) {
    s.tag("UART");
    s.io(ulcon); s.io(ucon); s.io(ufcon); s.io(umcon); s.io(ubrdiv); s.io(rx); s.io(umstat);
}

void ClockPower::serialize(StateIO &s) {
    s.tag("CLKP");
    s.io(locktime); s.io(mpllcon); s.io(upllcon); s.io(clkcon); s.io(clkslow); s.io(clkdivn);
}

void Rtc::serialize(StateIO &s) {
    s.tag("RTCC");
    s.io(offsetSec); s.io(rtccon); s.io(ticnt); s.io(rtcalm); s.io(alm); s.io(pending); s.io(pendingMask);
    s.time(lastTickNs);
}

void RegFile::serialize(StateIO &s) {
    s.tag("REGF");
    s.bytes(regs.data(), regs.size() * 4);
}

void Flash::serialize(StateIO &s) {
    s.tag("FLSH");
    uint32_t mode = st;
    s.io(mode);
    if (!s.writing) {
        st = ARRAY;
        bus.setDirectRead(base, 0x100000, true);
        setMode((State)mode);
    }
}

void Lcd::serialize(StateIO &s) {
    s.tag("LCDC");
    s.io(lcdcon); s.io(saddr1); s.io(saddr2); s.io(saddr3); s.io(redlut); s.io(greenlut); s.io(bluelut);
    s.io(dithmode); s.io(tpal); s.io(intpnd); s.io(srcpnd); s.io(intmsk); s.io(lpcsel); s.io(palette);
}

void Touch::serialize(StateIO &s) {
    s.tag("ADCT");
    s.io(adccon); s.io(adctsc); s.io(adcdly); s.io(dat0); s.io(dat1); s.io(penDown); s.io(rawX); s.io(rawY);
}

void SpiKeyboard::serialize(StateIO &s) {
    s.tag("SPI1");
    s.io(q); s.io(spcon); s.io(sppin); s.io(sppre); s.io(sptdat);
}

void EmulServ::serialize(StateIO &s) {
    s.tag("EMSV");
    s.io(mask); s.io(pending);
}

void DePage::serialize(StateIO &s) {
    s.tag("DEPG");
    s.io(fsPb); s.io(fsResult); s.io(dmaRegs); s.io(battery); s.io(leds); s.io(speaker);
}

void AudioIis::serialize(StateIO &s) {
    s.tag("IIS0");
    s.io(regs);
}

void AudioDma::serialize(StateIO &s) {
    s.tag("DMA0");
    for (Chan &c : ch) {
        s.io(c.r); s.io(c.running); s.time(c.startNs); s.time(c.doneNs); s.io(c.bytes); s.io(c.queued); s.io(c.queuedAddr);
    }
    s.time(now);
}

void FolderShare::serialize(StateIO &s) {
    s.tag("FSHR");
    std::lock_guard<std::mutex> lk(mu);
    // Open files are reopened by path; directory enumerations are not kept (the guest restarts them).
    for (Handle &h : handles) {
        s.io(h.used); s.io(h.mode); s.io(h.host); s.io(h.guest);
        if (!s.writing) {
            h.fd = -1;
            if (h.used) {
                uint32_t a = h.mode & 0xF;
                h.fd = open(h.host.c_str(), a == 0 ? O_RDONLY : a == 1 ? O_WRONLY : O_RDWR);
                if (h.fd < 0) h.used = false;
            }
        }
    }
    if (!s.writing) for (Find &f : finds) f = Find();
}

// ---------------------------------------------------------------------------------------------
// Machine

namespace {

const uint32_t kMagic = 0x53444D57;  // "WMDS"
const uint32_t kVersion = 1;

}  // namespace

uint32_t snapshotCrc(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return ~c;
}

namespace {

bool allZero(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        if (v) return false;
    }
    return true;
}

}  // namespace

void Machine::serializeDevices(StateIO &s) {
    cpu.serialize(s);
    intc.serialize(s);
    gpio.serialize(s);
    pwm.serialize(s);
    uart0.serialize(s);
    uart1.serialize(s);
    uart2.serialize(s);
    clk.serialize(s);
    rtc.serialize(s);
    memctl.serialize(s);
    serializeWatchdog(s);
    flash->serialize(s);
    lcd->serialize(s);
    adc->serialize(s);
    kbd->serialize(s);
    emulServ->serialize(s);
    dePage->serialize(s);
    iis->serialize(s);
    dma->serialize(s);
    folderShare->serialize(s);
    for (auto &d : owned)
        if (auto *rf = dynamic_cast<RegFile *>(d.get())) rf->serialize(s);
    s.tag("MACH");
    bool boot = bootComplete;
    s.io(boot);
    s.io(resetRequested);
    if (!s.writing) {
        bootComplete = boot;
        idleSeconds = 0;
    }
}

bool Machine::saveState(const std::string &path, std::string &err) {
    StateIO s((int64_t)nowNs());
    uint32_t magic = kMagic, version = kVersion;
    s.io(magic);
    s.io(version);
    s.io(imageSize);
    s.io(imageCrc);
    s.io(cfg.memMB);
    s.io(cfg.screenW);
    s.io(cfg.screenH);
    s.io(virtNs);
    serializeDevices(s);

    // RAM: one byte per 4 KB page (0 = all zero), then the non-zero pages.
    s.tag("RAM0");
    size_t pages = ram.size() / 4096;
    std::vector<uint8_t> present(pages);
    for (size_t i = 0; i < pages; i++) present[i] = !allZero(&ram[i * 4096], 4096);
    s.bytes(present.data(), pages);
    for (size_t i = 0; i < pages; i++)
        if (present[i]) s.bytes(&ram[i * 4096], 4096);

    // Flash: pages that differ from the ROM image (registry, persistent storage).
    s.tag("NOR0");
    std::ifstream img(cfg.imagePath, std::ios::binary);
    std::vector<uint8_t> orig(4096);
    for (size_t off = 0; off < flash->mem.size(); off += 4096) {
        std::fill(orig.begin(), orig.end(), 0xFF);
        if (!flashFromRam && img) {
            img.seekg((std::streamoff)off);
            img.read((char *)orig.data(), 4096);
            img.clear();
        }
        if (memcmp(orig.data(), &flash->mem[off], 4096) != 0) {
            uint32_t page = (uint32_t)(off / 4096);
            s.io(page);
            s.bytes(&flash->mem[off], 4096);
        }
    }
    uint32_t end = 0xFFFFFFFFu;
    s.io(end);
    // Optional trailing sections (older snapshots end here).
    pcmcia->serialize(s);
    modem->serialize(s);

    std::string tmp = path + ".tmp";
    FILE *f = fopen(tmp.c_str(), "wb");
    if (!f) { err = "cannot write " + tmp; return false; }
    bool ok = fwrite(s.buf.data(), 1, s.buf.size(), f) == s.buf.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        err = "write failed";
        return false;
    }
    LOGI("snapshot saved: %s (%.1f MB)", path.c_str(), s.buf.size() / 1048576.0);
    return true;
}

bool Machine::loadState(const std::string &path, std::string &err) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { err = "no snapshot"; return false; }
    std::vector<uint8_t> data;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    data.resize(n > 0 ? (size_t)n : 0);
    bool readOk = fread(data.data(), 1, data.size(), f) == data.size();
    fclose(f);
    if (!readOk) { err = "read failed"; return false; }

    StateIO s(std::move(data), (int64_t)nowNs());
    uint32_t magic = 0, version = 0, size = 0, crc = 0, mem = 0;
    int w = 0, h = 0;
    s.io(magic); s.io(version); s.io(size); s.io(crc); s.io(mem); s.io(w); s.io(h);
    uint64_t savedVirt = 0;
    s.io(savedVirt);
    if (magic != kMagic || version != kVersion) { err = "snapshot format differs"; return false; }
    if (size != imageSize || crc != imageCrc || mem != cfg.memMB || w != cfg.screenW || h != cfg.screenH) {
        err = "snapshot is for a different image or configuration";
        return false;
    }
    if (cfg.deterministic) {
        virtNs = savedVirt;  // the deterministic clock continues from the snapshot
        s.setNow((int64_t)virtNs);
    }
    serializeDevices(s);
    s.tag("RAM0");
    size_t pages = ram.size() / 4096;
    std::vector<uint8_t> present(pages);
    s.bytes(present.data(), pages);
    for (size_t i = 0; i < pages && s.ok; i++) {
        if (present[i]) s.bytes(&ram[i * 4096], 4096);
        else memset(&ram[i * 4096], 0, 4096);
    }
    s.tag("NOR0");
    for (;;) {
        uint32_t page = 0xFFFFFFFFu;
        s.io(page);
        if (!s.ok || page == 0xFFFFFFFFu) break;
        if ((size_t)page * 4096 + 4096 > flash->mem.size()) { s.ok = false; break; }
        s.bytes(&flash->mem[(size_t)page * 4096], 4096);
    }
    if (s.ok && !s.atEnd()) pcmcia->serialize(s);
    if (s.ok && !s.atEnd()) modem->serialize(s);
    if (!s.ok) {
        // The machine is now partly restored: the caller must discard it and boot a fresh one.
        err = "snapshot is truncated or corrupt";
        return false;
    }
    cpu.flushTlb();
    LOGI("snapshot restored: %s", path.c_str());
    return true;
}

}  // namespace wm
