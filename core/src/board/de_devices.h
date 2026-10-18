// Device Emulator board additions: SPI1 keyboard controller and the DE-only devices that share
// the UART page at 0x500F0000 (DMA transport, folder sharing, EmulServ, battery, LEDs, speakerphone).
#pragma once
#include "bus.h"
#include "nled.h"  // notification LED / vibrator decoder
#include "s3c2410.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

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

// One ACKless pipe. Command and status each have one, 0x400 apart, inside Sideband's page.
// A newer line per verb replaces an unread one. The line the guest has already latched stays
// put until it consumes it.
//   +0x00 ID
//   +0x04 RXLEN  read: latch the oldest host→guest line and return its length (0 = none)
//                write 0: consume that line
//   +0x08 TXDATA write: one guest→host byte, '\n' ends a line
//   +0x0C TXLEN  bytes waiting for the host
//   +0x20        the latched line, 240 bytes
class HostLink {
public:
    explicit HostLink(uint32_t id) : id(id) {}
    void send(const std::string &line);
    std::string take();
    uint32_t read(uint32_t off);
    void write(uint32_t off, uint32_t v, int size);

private:
    static std::string verbOf(const std::string &line);
    uint32_t id;
    std::mutex mu;
    std::deque<std::string> rx;
    bool rxLatched = false;
    std::string tx;
};

// One chunk of the contact/calendar transfer. Half duplex in each direction, queued so a dump
// larger than one window does not have to wait for the other side between chunks.
class BulkPipe {
public:
    static constexpr uint32_t ID = 0x314B4C42u;  // 'BLK1'
    static constexpr size_t CAP = 1792;
    bool put(const uint8_t *p, size_t n, bool last);
    bool take(std::vector<uint8_t> &out, bool &last);
    uint32_t read(uint32_t off);
    void write(uint32_t off, uint32_t v, int size);

private:
    struct Chunk {
        std::vector<uint8_t> data;
        bool last = false;
    };
    std::mutex mu;
    std::deque<Chunk> rx;  // host → guest
    bool rxLatched = false;
    std::deque<Chunk> tx;  // guest → host
    std::vector<uint8_t> stage;
    uint32_t commitAcc = 0;  // the length word, assembled from its bytes
};

// Host↔guest pipes on their own page, next to the camera and the code patcher. Not COM1 or COM4,
// and not the UART page the kernel owns.
//   +0x000 command/response, ID 0x31444D43 ('CMD1')
//   +0x400 status,           ID 0x31415453 ('STA1')
//   +0x800 bulk,             ID 0x314B4C42 ('BLK1'): contact/calendar bytes, 1792 per chunk
class Sideband : public Device {
public:
    static constexpr uint32_t BASE = 0x5F300000;
    const char *name() const override { return "sideband"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void cmdSend(const std::string &line) { cmd.send(line); }
    std::string cmdTake() { return cmd.take(); }
    void statSend(const std::string &line) { sta.send(line); }
    std::string statTake() { return sta.take(); }
    bool bulkPut(const uint8_t *p, size_t n, bool last) { return bulk.put(p, n, last); }
    bool bulkTake(std::vector<uint8_t> &out, bool &last) { return bulk.take(out, last); }

private:
    HostLink cmd{0x31444D43u};  // 'CMD1'
    HostLink sta{0x31415453u};  // 'STA1'
    BulkPipe bulk;              // 'BLK1'
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
