// Device Emulator board additions: SPI1 keyboard controller and the DE-only devices that share
// the UART page at 0x500F0000 (DMA transport, folder sharing, EmulServ, battery, LEDs, speakerphone).
#pragma once
#include "bus.h"
#include "nled.h"  // notification LED / vibrator decoder
#include "s3c2410.h"
#include <cstdint>
#include <deque>
#include <functional>

namespace wm {

class StateIO;

// SMDK2410 keyboard micro on SPI1 (registers at 0x59000020). Codes are Samsung matrix
// scancodes, | 0x80 on release; each queued code raises EINT1.
class SpiKeyboard : public Device {
public:
    void serialize(StateIO &s);
    explicit SpiKeyboard(IntController &ic) : ic(ic) {}
    const char *name() const override { return "spi1-kbd"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void push(uint8_t code);
    bool enabled() const { return ((spcon >> 5) & 3) == 1; }
    void updateLevel();
    uint32_t control() const { return spcon; }
    size_t queued() const { return q.size(); }

private:
    IntController &ic;
    std::deque<uint8_t> q;
    uint32_t spcon = 0, sppin = 2, sppre = 0, sptdat = 0;
};

// Notification interrupts from the emulator to the guest (EINT11).
class EmulServ {
public:
    void serialize(StateIO &s);
    explicit EmulServ(Gpio &g) : gpio(g) {}
    uint32_t read(uint32_t off);
    void write(uint32_t off, uint32_t v);
    void raise(uint32_t bits);
    static constexpr uint32_t FOLDER_SHARING_CHANGED = 0x40000000;
    bool trace = false;  // log mask writes, pending reads and raises (CLI)

private:
    Gpio &gpio;
    uint32_t mask = 0, pending = 0;
};

// Routes the 0x50000000 page: UART0-2 and the DE devices above them.
class DePage : public Device {
public:
    void serialize(StateIO &s);
    DePage(Uart *uarts[3], EmulServ &es);
    const char *name() const override { return "uart/de"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;

    // Folder sharing (\Storage Card). Disabled until the storage bridge provides a handler:
    // handler(code, paramBlockPa) returns the Result register value.
    std::function<uint32_t(uint32_t code, uint32_t pbPa)> folderShare;

    // DE 2.0/3.0 byte-register devices (protocols still being learned; accesses are logged).
    uint8_t battery[4] = {0x00, 0x00, 0x18, 0x01};
    // The battery driver polls twice a second while Windows Mobile runs: a heartbeat for the host.
    uint64_t batteryReads = 0;
    uint8_t leds[7] = {};
    NotifyLeds nled{leds};  // host view of `leds` (nled.h); fed by write()
    uint8_t speaker[2] = {};
    int logBudget = 24;
    bool traceDma = false;
    int dmaLog = 0;

private:
    Uart *uarts[3];
    EmulServ &emulServ;
    uint32_t fsPb = 0, fsResult = 0;
    uint32_t dmaRegs[0x120 / 4] = {};
    uint8_t *byteDev(uint32_t off, const char *&nm, uint32_t &len);
};

}  // namespace wm
