#include "devices.h"
#include "../util/log.h"
#include <cstring>

namespace wm {

// ---------------------------------------------------------------------------------------------
// NOR flash

Flash::Flash(Bus &b, uint32_t base, std::vector<uint8_t> &&data)
    : mem(std::move(data)), bus(b), base(base) {}

void Flash::setMode(State s) {
    bool wasQuery = st == AUTOSEL;
    st = s;
    bool isQuery = s == AUTOSEL;
    // Only the ID words at the start of the bank change in autoselect mode; the rest of flash
    // (including XIP kernel code) stays readable. Route just the first 1 MB through read().
    if (wasQuery != isQuery) bus.setDirectRead(base, 0x100000, !isQuery);
}

uint32_t Flash::sectorBase(uint32_t off, uint32_t &size) const {
    // AM29LV800BB bottom-boot layout (16K, 8K, 8K, 32K), then uniform 64K sectors.
    if (off < 0x4000) { size = 0x4000; return 0; }
    if (off < 0x6000) { size = 0x2000; return 0x4000; }
    if (off < 0x8000) { size = 0x2000; return 0x6000; }
    if (off < 0x10000) { size = 0x8000; return 0x8000; }
    size = 0x10000;
    return off & ~0xFFFFu;
}

uint32_t Flash::read(uint32_t pa, int size) {
    uint32_t off = pa - base;
    if (st == AUTOSEL && off < 0x10) {
        // Halfword 0: manufacturer (AMD), halfword 1: device id; DE reads them as one word.
        auto id = [](uint32_t idx) -> uint32_t { return idx == 0 ? 0x0001 : idx == 1 ? 0x225B : 0; };
        uint32_t idx = (off >> 1) & 0xFF;
        uint32_t v = id(idx) | (size == 4 ? id(idx + 1) << 16 : 0);
        return v;
    }
    if (off + size > mem.size()) return 0;
    uint32_t v = 0;
    memcpy(&v, &mem[off], size);
    return v;
}

void Flash::write(uint32_t pa, uint32_t v, int size) {
    uint32_t off = pa - base;
    uint32_t cmd = v & 0xFF;
    if (logCommands) LOGD("flash: w%d off=%08x v=%08x st=%d", size * 8, off, v, st);
    switch (st) {
        case ARRAY:
        case AUTOSEL:
            if (cmd == 0xF0) { setMode(ARRAY); return; }
            if (cmd == 0xAA) { setMode(UNLOCK1); return; }
            return;
        case UNLOCK1:
            st = (cmd == 0x55) ? UNLOCK2 : ARRAY;
            return;
        case UNLOCK2:
            switch (cmd) {
                case 0x90: setMode(AUTOSEL); return;
                case 0xA0: st = PROGRAM; return;
                case 0x80: st = ERASE1; return;
                default: st = ARRAY; return;
            }
        case PROGRAM:
            if (off + size <= mem.size()) {
                for (int i = 0; i < size; i++) mem[off + i] &= (uint8_t)(v >> (8 * i));
                dirty = true;
            }
            st = ARRAY;
            return;
        case ERASE1:
            st = (cmd == 0xAA) ? ERASE2 : ARRAY;
            return;
        case ERASE2:
            st = (cmd == 0x55) ? ERASE3 : ARRAY;
            return;
        case ERASE3:
            if (cmd == 0x30) {
                uint32_t sz, s0 = sectorBase(off, sz);
                if (s0 + sz <= mem.size()) memset(&mem[s0], 0xFF, sz);
                dirty = true;
                LOGD("flash: erase sector %08x (+%x)", s0, sz);
            } else if (cmd == 0x10) {
                memset(mem.data(), 0xFF, mem.size());
                dirty = true;
            }
            st = ARRAY;
            return;
        default:
            st = ARRAY;
            return;
    }
}

// ---------------------------------------------------------------------------------------------
// LCD

int Lcd::bpp() const {
    // DE: BPPMODE 12 = RGB565, 13 = 24 bpp, 14 = 32 bpp; anything else is treated as 16.
    switch ((lcdcon[0] >> 1) & 0xF) {
        case 0xD: return 24;
        case 0xE: return 32;
        default: return 16;
    }
}

uint32_t Lcd::strideBytes() const {
    // DE copies the frame buffer linearly; PAGEWIDTH/OFFSIZE are ignored.
    return (uint32_t)width() * (uint32_t)(bpp() / 8);
}

void Lcd::vsync() {
    if (!enabled()) return;
    srcpnd |= 2;
    intpnd |= 2 & ~intmsk;
    if (intpnd) ic.raise(IRQ_LCD);
}

uint32_t Lcd::read(uint32_t pa, int) {
    uint32_t off = pa & 0xFFFFF;
    if (off >= 0x400 && off < 0x800) return palette[(off - 0x400) / 4];
    switch (off) {
        case 0x00: return lcdcon[0];
        case 0x04: return lcdcon[1];
        case 0x08: return lcdcon[2];
        case 0x0C: return lcdcon[3];
        case 0x10: return lcdcon[4];
        case 0x14: return saddr1;
        case 0x18: return saddr2;
        case 0x1C: return saddr3;
        case 0x20: return redlut;
        case 0x24: return greenlut;
        case 0x28: return bluelut;
        case 0x4C: return dithmode;
        case 0x50: return tpal;
        case 0x54: return intpnd;
        case 0x58: return srcpnd;
        case 0x5C: return intmsk;
        case 0x60: return lpcsel;
    }
    return 0;
}

void Lcd::write(uint32_t pa, uint32_t v, int) {
    uint32_t off = pa & 0xFFFFF;
    if (off >= 0x400 && off < 0x800) { palette[(off - 0x400) / 4] = v; return; }
    switch (off) {
        case 0x00:
            if ((v ^ lcdcon[0]) & 1) LOGI("lcd: %s", (v & 1) ? "enabled" : "disabled");
            lcdcon[0] = v;
            break;
        case 0x04: lcdcon[1] = v; break;
        case 0x08: lcdcon[2] = v; break;
        case 0x0C: lcdcon[3] = v; break;
        case 0x10: lcdcon[4] = v; break;
        case 0x14: saddr1 = v; LOGI("lcd: framebuffer pa=%08x", framebufferPa()); break;
        case 0x18: saddr2 = v; break;
        case 0x1C: saddr3 = v; break;
        case 0x20: redlut = v; break;
        case 0x24: greenlut = v; break;
        case 0x28: bluelut = v; break;
        case 0x4C: dithmode = v; break;
        case 0x50: tpal = v; break;
        case 0x54: intpnd &= ~v; break;
        case 0x58: srcpnd &= ~v; break;
        case 0x5C: intmsk = v; break;
        case 0x60: lpcsel = v; break;
    }
}

// ---------------------------------------------------------------------------------------------
// ADC / touch

void Touch::setPen(bool down, int sampleX, int sampleY) {
    penDown = down;
    rawX = sampleX;
    rawY = sampleY;
    uint32_t flags = (down ? 0 : (1u << 15)) | ((adctsc & 3) << 12);
    dat0 = (dat0 & 0x3FF) | flags;
    dat1 = (dat1 & 0x3FF) | flags;
    ic.raiseSub(SUB_TC);  // pen up/down; the driver then samples on its Timer3 tick
}

void Touch::convert() {
    uint32_t flags = (penDown ? 0 : (1u << 15)) | ((adctsc & 3) << 12) | ((adctsc & 4) << 12);
    if (penDown && (adctsc & 7)) {
        // DE swaps the axes: XPDATA carries the Y sample and YPDATA the X sample.
        dat0 = flags | (uint32_t)(rawY & 0x3FF);
        dat1 = flags | (uint32_t)(rawX & 0x3FF);
    } else {
        dat0 = flags | (dat0 & 0x3FF);
        dat1 = flags | (dat1 & 0x3FF);
    }
    adccon |= 1u << 15;  // conversions finish instantly; no ADC interrupt
}

uint32_t Touch::read(uint32_t pa, int) {
    switch (pa & 0xFF) {
        case 0x00: return adccon;
        case 0x04: return adctsc;
        case 0x08: return adcdly;
        case 0x0C: {
            uint32_t v = dat0;
            if (adccon & 2) convert();
            return v;
        }
        case 0x10: return dat1;
    }
    return 0;
}

void Touch::write(uint32_t pa, uint32_t v, int) {
    switch (pa & 0xFF) {
        case 0x00:
            adccon = (v & ~(1u << 15)) & ~1u;
            if (v & 1) convert();
            break;
        case 0x04: adctsc = v; break;
        case 0x08: adcdly = v; break;
    }
}

}  // namespace wm
