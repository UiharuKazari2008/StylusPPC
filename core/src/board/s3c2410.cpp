#include "s3c2410.h"
#include "../arm/cpu.h"
#include "../util/log.h"
#include <ctime>

namespace wm {

// ---------------------------------------------------------------------------------------------
// Interrupt controller (0x4A000000)

void IntController::raise(int src) {
    srcpnd |= 1u << src;
    update();
}

void IntController::raiseSub(int sub) {
    subsrcpnd |= 1u << sub;
    update();
}

void IntController::setLevel(int src, bool level) {
    if (level) levels |= 1u << src;
    else levels &= ~(1u << src);
    update();
}

void IntController::setSubLevel(int sub, bool level) {
    if (level) subLevels |= 1u << sub;
    else subLevels &= ~(1u << sub);
    update();
}

void IntController::update() {
    subsrcpnd |= subLevels;
    uint32_t sub = subsrcpnd & ~intsubmsk;
    if (sub & 0x007) srcpnd |= 1u << IRQ_UART0;
    if (sub & 0x038) srcpnd |= 1u << IRQ_UART1;
    if (sub & 0x1C0) srcpnd |= 1u << IRQ_UART2;
    if (sub & 0x600) srcpnd |= 1u << IRQ_ADC;
    srcpnd |= levels;
    uint32_t pend = srcpnd & ~intmsk;
    if (intpnd == 0 && (pend & ~intmod)) {
        int n = __builtin_ctz(pend & ~intmod);
        intpnd = 1u << n;
        intoffset = n;
    }
    cpu.setIrq(intpnd != 0);
    cpu.setFiq((pend & intmod) != 0);
}

uint32_t IntController::read(uint32_t pa, int) {
    switch (pa & 0xFF) {
        case 0x00: return srcpnd;
        case 0x04: return intmod;
        case 0x08: return intmsk;
        case 0x0C: return priority;
        case 0x10: return intpnd;
        case 0x14: return intoffset;
        case 0x18: return subsrcpnd;
        case 0x1C: return intsubmsk;
    }
    return 0;
}

void IntController::write(uint32_t pa, uint32_t v, int) {
    switch (pa & 0xFF) {
        case 0x00: srcpnd &= ~v; break;
        case 0x04: intmod = v; break;
        case 0x08: intmsk = v; break;
        case 0x0C: priority = v; break;
        case 0x10:
            intpnd &= ~v;
            if (!intpnd) intoffset = 0;
            break;
        case 0x18: subsrcpnd &= ~v; break;
        case 0x1C: intsubmsk = v & 0x7FF; break;
    }
    update();
}

// ---------------------------------------------------------------------------------------------
// GPIO (0x56000000)

void Gpio::raiseExt(int e) {
    if (e < 4) { ic.raise(e); return; }
    eintpend |= 1u << e;
    updateExt();
}

// As DE does: the button reads as pressed until GPFDAT has been read twice (the power-button driver
// samples it, sleeps, then samples again), then as released.
void Gpio::powerButton() {
    powerPresses++;
    powerPressed = true;
    gpfReads = 0;
    gpfdatIn &= ~1u;
    if ((regs[0x50 / 4] & 3) == 2) ic.raise(IRQ_EINT0);
}

void Gpio::setExtLevel(int e, bool level) {
    uint32_t bit = 1u << e;
    if (level == ((extLevels & bit) != 0)) return;
    if (level) extLevels |= bit;
    else extLevels &= ~bit;
    updateExt();
}

void Gpio::updateExt() {
    eintpend |= extLevels;
    uint32_t p = eintpend & ~eintmask;
    if (p & 0x000000F0) ic.raise(IRQ_EINT4_7);
    if (p & 0x00FFFF00) ic.raise(IRQ_EINT8_23);
}

uint32_t Gpio::read(uint32_t pa, int) {
    uint32_t off = pa & 0xFF;
    switch (off) {
        case 0x54: {  // GPFDAT: input pins read the external level
            if (powerPressed && ++gpfReads == 2) {
                powerPressed = false;
                gpfdatIn |= 1;
            }
            uint32_t con = regs[0x50 / 4], inMask = 0;
            for (int i = 0; i < 8; i++) if (((con >> (2 * i)) & 3) != 1) inMask |= 1u << i;
            return (regs[off / 4] & ~inMask) | (gpfdatIn & inMask);
        }
        case 0x64: {
            uint32_t con = regs[0x60 / 4], inMask = 0;
            for (int i = 0; i < 16; i++) if (((con >> (2 * i)) & 3) != 1) inMask |= 1u << i;
            return (regs[off / 4] & ~inMask) | (gpgdatIn & inMask);
        }
        case 0xA4: return eintmask;
        case 0xA8: return eintpend;
        case 0xB0: return 0x32410002;  // GSTATUS1 chip id
        case 0xB4: return gstatus2;
    }
    return regs[off / 4];
}

void Gpio::write(uint32_t pa, uint32_t v, int) {
    uint32_t off = pa & 0xFF;
    switch (off) {
        case 0xA4: eintmask = v; updateExt(); return;
        case 0xA8: eintpend &= ~v; updateExt(); return;
        case 0xB0: return;
        case 0xB4: gstatus2 &= ~v; return;
    }
    regs[off / 4] = v;
}

// ---------------------------------------------------------------------------------------------
// PWM timers (0x51000000)

static int tconStart(int n) { return n == 0 ? 0 : 4 * n + 4; }
static int tconAuto(int n) { return n == 0 ? 3 : n == 4 ? 22 : tconStart(n) + 3; }

uint64_t PwmTimers::tickNs(int n) const {
    // Device Emulator clocks the timers at PCLK = 203 MHz / 4 and uses the prescaler as-is
    // (0 counts as 1), not prescaler+1 as on silicon. The OAL's 1 ms tick depends on this.
    uint32_t pre = n < 2 ? (tcfg0 & 0xFF) : ((tcfg0 >> 8) & 0xFF);
    if (pre == 0) pre = 1;
    uint32_t mux = (tcfg1 >> (4 * n)) & 0xF;
    uint32_t div = mux < 4 ? (2u << mux) : 2;
    // ns per count, kept in 1/1024 ns fixed point to stay exact enough.
    return (uint64_t)((1000000000.0 * 1024.0 * pre * div) / (double)pclkHz);
}

uint32_t PwmTimers::count(int n) const {
    if (!t[n].running) return t[n].startCount;
    uint64_t elapsed = (now - t[n].startNs) * 1024 / tickNs(n);
    return elapsed >= t[n].startCount ? 0 : t[n].startCount - (uint32_t)elapsed;
}

void PwmTimers::startTimer(int n, uint32_t from) {
    t[n].running = true;
    t[n].startNs = now;
    t[n].startCount = from;
}

void PwmTimers::advance(uint64_t nowNs) {
    now = nowNs;
    for (int n = 0; n < 5; n++) {
        if (!t[n].running) continue;
        uint64_t period = (uint64_t)(t[n].startCount + 1) * tickNs(n) / 1024;
        uint64_t due = t[n].startNs + period;
        if (now < due) continue;
        // Ticks the host was late for are owed, not dropped: each is delivered once the guest has
        // taken the previous one, so the guest's tick count (its sense of time) stays exact even
        // when the host sleeps through several periods (see Machine::runFor's idle coalescing).
        if (ic.srcpnd & (1u << (IRQ_TIMER0 + n))) continue;
        ic.raise(IRQ_TIMER0 + n);
        fired++;
        if (tcon & (1u << tconAuto(n))) {
            uint64_t p2 = (uint64_t)(tcntb[n] + 1) * tickNs(n) / 1024;
            t[n].startCount = tcntb[n];
            t[n].startNs = due;
            if (p2 == 0 || now - due > kMaxOwedNs) t[n].startNs = now;  // stalled (paused): resync
        } else {
            t[n].running = false;
            t[n].startCount = 0;
        }
    }
}

uint64_t PwmTimers::nextEventNs() const {
    uint64_t best = ~0ull;
    for (int n = 0; n < 5; n++) {
        if (!t[n].running) continue;
        uint64_t due = t[n].startNs + (uint64_t)(t[n].startCount + 1) * tickNs(n) / 1024;
        if (due < best) best = due;
    }
    return best;
}

uint32_t PwmTimers::read(uint32_t pa, int) {
    uint32_t off = pa & 0xFF;
    switch (off) {
        case 0x00: return tcfg0;
        case 0x04: return tcfg1;
        case 0x08: return tcon;
        case 0x40: return count(4);
    }
    if (off >= 0x0C && off < 0x3C) {
        int n = (off - 0x0C) / 0xC, r = (off - 0x0C) % 0xC;
        if (r == 0) return tcntb[n];
        if (r == 4) return tcmpb[n];
        return count(n);
    }
    if (off == 0x3C) return tcntb[4];
    return 0;
}

void PwmTimers::write(uint32_t pa, uint32_t v, int) {
    uint32_t off = pa & 0xFF;
    switch (off) {
        case 0x00: tcfg0 = v; return;
        case 0x04: tcfg1 = v; return;
        case 0x08: {
            uint32_t old = tcon;
            tcon = v;
            for (int n = 0; n < 5; n++) {
                int sb = tconStart(n);
                if (v & (1u << (sb + 1))) {  // manual update
                    t[n].startCount = tcntb[n];
                    t[n].startNs = now;
                }
                bool was = old & (1u << sb), is = v & (1u << sb);
                if (is && !was) startTimer(n, (v & (1u << (sb + 1))) ? tcntb[n] : t[n].startCount);
                else if (!is && was) { t[n].startCount = count(n); t[n].running = false; }
            }
            return;
        }
        case 0x3C: tcntb[4] = v & 0xFFFF; return;
    }
    if (off >= 0x0C && off < 0x3C) {
        int n = (off - 0x0C) / 0xC, r = (off - 0x0C) % 0xC;
        if (r == 0) tcntb[n] = v & 0xFFFF;
        else if (r == 4) tcmpb[n] = v & 0xFFFF;
    }
}

// ---------------------------------------------------------------------------------------------
// UART (0x50000000 + n*0x4000)

uint32_t Uart::read(uint32_t pa, int) {
    if (traceConfig && traceReads++ < 60) LOGI("uart%d: read %02x", idx, pa & 0xFF);
    switch (pa & 0xFF) {
        case 0x00: return ulcon;
        case 0x04: return ucon;
        case 0x08: return ufcon;
        case 0x0C: return umcon;
        case 0x10: return 0x6 | (rx.empty() ? 0 : 1);
        case 0x14: return 0;
        case 0x18: return rx.size() >= 16 ? 0x100 | 15 : (uint32_t)rx.size();  // bit 8: RX FIFO full
        case 0x1C: return umstat;
        case 0x24: {
            if (rx.empty()) return 0;
            uint8_t c = rx.front();
            rx.pop_front();
            if (rx.empty()) updateRx();
            return c;
        }
        case 0x28: return ubrdiv;
    }
    return 0;
}

void Uart::write(uint32_t pa, uint32_t v, int) {
    if (traceConfig && (pa & 0xFF) != 0x20) LOGI("uart%d: reg %02x <- %08x", idx, pa & 0xFF, v);
    switch (pa & 0xFF) {
        case 0x00: ulcon = v; break;
        case 0x04: ucon = v; updateRx(); break;
        case 0x08: ufcon = v; break;
        case 0x0C: umcon = v; break;
        case 0x20: {
            uint8_t c = (uint8_t)v;
            if (peer) peer(c);
            if (onTx) onTx(idx, c);
            if (((ucon >> 2) & 3) == 1) ic.raiseSub(idx * 3 + 1);
            break;
        }
        case 0x28: ubrdiv = v; break;
    }
}

// ---------------------------------------------------------------------------------------------
// Clock & power (0x4C000000)

uint32_t ClockPower::read(uint32_t pa, int) {
    switch (pa & 0xFF) {
        case 0x00: return locktime;
        case 0x04: return mpllcon;
        case 0x08: return upllcon;
        case 0x0C: return clkcon;
        case 0x10: return clkslow;
        case 0x14: return clkdivn;
    }
    return 0;
}

void ClockPower::write(uint32_t pa, uint32_t v, int) {
    switch (pa & 0xFF) {
        case 0x00: locktime = v; break;
        case 0x04: mpllcon = v; break;
        case 0x08: upllcon = v; break;
        case 0x0C:
            clkcon = v & ~0x4u;
            if (v & 0x4) cpu.halted = true;  // IDLE: stop until an interrupt
            if (v & 0x8) {
                // POWER_OFF (sleep): the core loses power right after this store, whatever is
                // pending. Bit 3 stays set (and is saved with the snapshot) while asleep;
                // Machine::asleep() waits for the power button and then does the wake-up reset.
                cpu.halted = true;
                cpu.setIrq(false);
                cpu.setFiq(false);
                LOGI("clkpwr: sleep (POWER_OFF)");
            }
            break;
        case 0x10: clkslow = v; break;
        case 0x14: clkdivn = v; break;
    }
}

// ---------------------------------------------------------------------------------------------
// RTC (0x57000000)

static uint32_t toBcd(int v) { return (uint32_t)(((v / 10) << 4) | (v % 10)); }
static int fromBcd(uint32_t v) { return (int)(((v >> 4) & 0xF) * 10 + (v & 0xF)); }

static time_t hostLocalNow() {
    time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);
    return t + lt.tm_gmtoff;
}

int64_t Rtc::localNow() const {
    if (virtNs) return 1767225600 + (int64_t)(*virtNs / 1000000000ull);  // 2026-01-01 + virtual time
    return hostLocalNow();
}

void Rtc::latch() {
    time_t g = localNow() + offsetSec;
    struct tm tm;
    gmtime_r(&g, &tm);
    latched[0] = toBcd(tm.tm_sec);
    latched[1] = toBcd(tm.tm_min);
    latched[2] = toBcd(tm.tm_hour);
    latched[3] = toBcd(tm.tm_mday);
    latched[4] = toBcd(tm.tm_wday + 1);
    latched[5] = toBcd(tm.tm_mon + 1);
    latched[6] = toBcd(tm.tm_year % 100);
}

void Rtc::applyPending() {
    time_t g = localNow() + offsetSec;
    struct tm tm;
    gmtime_r(&g, &tm);
    for (int i = 0; i < 7; i++) {
        if (!(pendingMask & (1u << i))) continue;
        int v = fromBcd(pending[i]);
        switch (i) {
            case 0: tm.tm_sec = v; break;
            case 1: tm.tm_min = v; break;
            case 2: tm.tm_hour = v; break;
            case 3: tm.tm_mday = v; break;
            case 4: break;  // day of week is derived
            case 5: tm.tm_mon = v - 1; break;
            case 6: tm.tm_year = 100 + v; break;
        }
    }
    pendingMask = 0;
    offsetSec = (int64_t)timegm(&tm) - localNow();
}

uint32_t Rtc::read(uint32_t pa, int) {
    uint32_t off = pa & 0xFF;
    switch (off) {
        case 0x40: return rtccon;
        case 0x44: return ticnt;
        case 0x50: return rtcalm;
    }
    if (off >= 0x54 && off <= 0x68) return alm[(off - 0x54) / 4];
    if (off >= 0x70 && off <= 0x88) {
        if (off == 0x70) latch();
        else if (latched[0] == 0 && latched[6] == 0) latch();
        return latched[(off - 0x70) / 4];
    }
    return 0;
}

void Rtc::write(uint32_t pa, uint32_t v, int) {
    uint32_t off = pa & 0xFF;
    switch (off) {
        case 0x40:
            if (rtccon && !v && pendingMask) applyPending();
            rtccon = v;
            return;
        case 0x44: ticnt = v; return;
        case 0x50: rtcalm = v; return;
    }
    if (off >= 0x54 && off <= 0x68) { alm[(off - 0x54) / 4] = v; return; }
    if (off >= 0x70 && off <= 0x88) {
        int i = (off - 0x70) / 4;
        pending[i] = v;
        pendingMask |= 1u << i;
    }
}

void Rtc::advance(uint64_t nowNs) {
    if (!(ticnt & 0x80)) { lastTickNs = nowNs; return; }
    uint64_t period = (uint64_t)((ticnt & 0x7F) + 1) * 1000000000ull / 128;
    if (nowNs - lastTickNs >= period) {
        lastTickNs = nowNs;
        ic.raise(IRQ_TICK);
    }
}

}  // namespace wm
