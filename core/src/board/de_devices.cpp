#include "de_devices.h"
#include "../util/log.h"

namespace wm {

// ---------------------------------------------------------------------------------------------
// SPI1 keyboard

// Codes are queued even while the driver has the port out of interrupt mode (it switches modes
// while servicing a key); dropping them there loses key releases and leaves keys stuck down.
//
// EINT1 is level-triggered while codes are waiting: the driver reads one code per interrupt and then
// clears SRCPND, so an edge raised during the read would be wiped and the rest of a multi-code
// sequence (Fn + key + Fn up) would sit in the queue until the next key event.
void SpiKeyboard::push(uint8_t code) {
    if (q.size() >= 64) return;
    q.push_back(code);
    updateLevel();
}

void SpiKeyboard::updateLevel() { ic.setLevel(IRQ_EINT1, enabled() && !q.empty()); }

uint32_t SpiKeyboard::read(uint32_t pa, int) {
    switch (pa & 0xFF) {
        case 0x20: return spcon;
        case 0x24: return 1;  // SPSTA: ready
        case 0x28: return sppin;
        case 0x2C: return sppre;
        case 0x30: return sptdat;
        case 0x34: {
            if (q.empty()) return 0;
            uint8_t c = q.front();
            q.pop_front();
            updateLevel();
            return c;
        }
    }
    return 0;
}

void SpiKeyboard::write(uint32_t pa, uint32_t v, int) {
    switch (pa & 0xFF) {
        case 0x20: {
            spcon = v;
            updateLevel();
            break;
        }
        case 0x28: sppin = v; break;
        case 0x2C: sppre = v; break;
        case 0x30: sptdat = v; break;
    }
}

// ---------------------------------------------------------------------------------------------
// EmulServ (0x500F5000)

void EmulServ::raise(uint32_t bits) {
    if (trace) LOGI("emulserv: raise %x (mask %x, pending %x)", bits, mask, pending);
    pending |= bits;
    if (pending & mask) gpio.raiseExt(11);
}

uint32_t EmulServ::read(uint32_t off) {
    if (off == 0) return mask;
    if (off == 4) {
        uint32_t v = pending;
        if (trace) LOGI("emulserv: pending read %x", v);
        pending = 0;
        return v;
    }
    return 0;
}

void EmulServ::write(uint32_t off, uint32_t v) {
    if (off == 0) {
        if (trace) LOGI("emulserv: mask %x", v);
        mask = v;
        if (pending & mask) gpio.raiseExt(11);
    }
}

// ---------------------------------------------------------------------------------------------
// 0x50000000 page

DePage::DePage(Uart *u[3], EmulServ &es) : emulServ(es) {
    for (int i = 0; i < 3; i++) uarts[i] = u[i];
    dmaRegs[0x18 / 4] = 0xFFFFFFFF;  // InputStatus: nothing queued
    dmaRegs[0x24 / 4] = 0xFFFFFFFF;  // OutputStatus: closed
}

uint8_t *DePage::byteDev(uint32_t off, const char *&nm, uint32_t &len) {
    if (off >= 0xFFFF0 && off < 0xFFFF4) { nm = "battery"; len = 4; return battery + (off - 0xFFFF0); }
    if (off >= 0xFFFD0 && off < 0xFFFD7) { nm = "leds"; len = 7; return leds + (off - 0xFFFD0); }
    if (off >= 0xFFFC0 && off < 0xFFFC2) { nm = "speaker"; len = 2; return speaker + (off - 0xFFFC0); }
    return nullptr;
}

uint32_t DePage::read(uint32_t pa, int size) {
    uint32_t off = pa & 0xFFFFF;
    if (off < 0xC000) return uarts[off >> 14]->read(pa, size);
    if (off >= 0xF0000 && off < 0xF4000) {  // DMA transport: present but no host channels yet
        uint32_t r = off - 0xF0000;
        if (traceDma && dmaLog++ < 400) LOGI("dmatrans: read%d +%04x", size * 8, r);
        if (r >= 0x2000 && r < 0x2120) return dmaRegs[(r - 0x2000) / 4];
        return 0;
    }
    if (off >= 0xF4000 && off < 0xF4010) {
        switch (off & 0xF) {
            case 0x0: return fsPb;
            case 0x8: return 0;  // IOPending
            case 0xC: return fsResult;
        }
        return 0;
    }
    if (off >= 0xF5000 && off < 0xF5008) return emulServ.read(off & 7);
    const char *nm;
    uint32_t len;
    if (uint8_t *p = byteDev(off, nm, len)) {
        uint32_t v = 0;
        for (int i = 0; i < size && (off & 0xF) + i < 16; i++) v |= (uint32_t)p[i] << (8 * i);
        if (p == battery) batteryReads++;
        if (logBudget-- > 0) LOGI("%s: read%d +%u -> %x", nm, size * 8, (unsigned)(off & 0xF), v);
        return v;
    }
    if (logBudget-- > 0) LOGW("de page: read%d %08x", size * 8, pa);
    return 0;
}

void DePage::write(uint32_t pa, uint32_t v, int size) {
    uint32_t off = pa & 0xFFFFF;
    if (off < 0xC000) { uarts[off >> 14]->write(pa, v, size); return; }
    if (off >= 0xF0000 && off < 0xF4000) {
        uint32_t r = off - 0xF0000;
        if (traceDma && dmaLog++ < 400) LOGI("dmatrans: write%d +%04x <- %08x", size * 8, r, v);
        if (r >= 0x2000 && r < 0x2120) {
            uint32_t i = (r - 0x2000) / 4;
            if (i == 0x18 / 4 || i == 0x24 / 4) return;  // status registers are read-only here
            dmaRegs[i] = v;
        }
        return;
    }
    if (off >= 0xF4000 && off < 0xF4010) {
        switch (off & 0xF) {
            case 0x0: fsPb = v; break;
            case 0x4:
                // Operations complete synchronously (IOPending always reads 0), so vcefsd never polls
                // after an operation. Code 0 (PollCompletion) comes from emulserv's change handler,
                // which decides on Result alone: 1 = no share (DeactivateDevice, the card goes),
                // anything else = share present (ActivateDevice if the card is gone). DE answers
                // 1 / 0 here; leaving the last operation's Result in place made a card that came
                // back look removed (a failed call returns 1) and WM never mounted it again.
                fsResult = folderShare ? folderShare(v, fsPb) : 1;  // 1: sharing disabled
                break;
        }
        return;
    }
    if (off >= 0xF5000 && off < 0xF5008) { emulServ.write(off & 7, v); return; }
    const char *nm;
    uint32_t len;
    if (uint8_t *p = byteDev(off, nm, len)) {
        for (int i = 0; i < size; i++) p[i] = (uint8_t)(v >> (8 * i));
        if (logBudget-- > 0) LOGI("%s: write%d +%u <- %x", nm, size * 8, (unsigned)(off & 0xF), v);
        if (p >= leds && p < leds + 7) nled.written((unsigned)(p - leds));  // notification LED / vibrator (nled.h)
        return;
    }
    if (logBudget-- > 0) LOGW("de page: write%d %08x <- %08x", size * 8, pa, v);
}

}  // namespace wm
