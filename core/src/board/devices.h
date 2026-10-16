// Board devices beyond the plain S3C2410 register blocks: NOR flash, LCD controller, ADC/touch.
#pragma once
#include "bus.h"
#include "s3c2410.h"
#include <cstdint>
#include <vector>

namespace wm {

class StateIO;

class Cpu;

// NOR flash bank 0 as DE models it: AMD AM29LV800BB-style 16-bit command cycles
// (unlock 0xAA @0xAAAA, 0x55 @0x5554), autoselect id 0x225B0001.
class Flash : public Device {
public:
    void serialize(StateIO &s);
    Flash(Bus &bus, uint32_t base, std::vector<uint8_t> &&data);
    const char *name() const override { return "flash"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    std::vector<uint8_t> mem;
    bool dirty = false;
    bool logCommands = true;

private:
    enum State { ARRAY, UNLOCK1, UNLOCK2, AUTOSEL, PROGRAM, ERASE1, ERASE2, ERASE3 };
    Bus &bus;
    uint32_t base;
    State st = ARRAY;
    void setMode(State s);
    uint32_t sectorBase(uint32_t off, uint32_t &size) const;
};

// S3C2410 LCD controller (0x4D000000).
class Lcd : public Device {
public:
    void serialize(StateIO &s);
    Lcd(Bus &bus, IntController &ic) : bus(bus), ic(ic) {}
    const char *name() const override { return "lcd"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    bool enabled() const { return lcdcon[0] & 1; }
    int width() const { return (int)((lcdcon[2] >> 8) & 0x7FF) + 1; }
    int height() const { return (int)((lcdcon[1] >> 14) & 0x3FF) + 1; }
    int bpp() const;
    uint32_t framebufferPa() const { return ((saddr1 >> 21) << 22) | ((saddr1 & 0x1FFFFF) << 1); }
    uint32_t strideBytes() const;
    void vsync();   // raise frame interrupt if enabled
    uint32_t lcdcon[5] = {}, saddr1 = 0, saddr2 = 0, saddr3 = 0, redlut = 0, greenlut = 0, bluelut = 0,
             dithmode = 0, tpal = 0, intpnd = 0, srcpnd = 0, intmsk = 3, lpcsel = 0;
    uint32_t palette[256] = {};

private:
    Bus &bus;
    IntController &ic;
};

// S3C2410 ADC + touch screen interface (0x58000000).
class Touch : public Device {
public:
    void serialize(StateIO &s);
    explicit Touch(IntController &ic) : ic(ic) {}
    const char *name() const override { return "adc"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void setPen(bool down, int sampleX, int sampleY);  // 10-bit ADC samples

    uint32_t adccon = 0x3FC4, adctsc = 0x58, adcdly = 0xFF, dat0 = 0, dat1 = 0;
    bool penDown = false;
    int rawX = 0, rawY = 0;

private:
    IntController &ic;
    void convert();
};

}  // namespace wm
