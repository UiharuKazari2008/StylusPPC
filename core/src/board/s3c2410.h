// Samsung S3C2410 on-chip peripherals as emulated by Microsoft Device Emulator.
#pragma once
#include "bus.h"
#include <cstdint>
#include <deque>
#include <functional>
#include <string>

namespace wm {

class StateIO;

class Cpu;

enum IrqSource {
    IRQ_EINT0 = 0, IRQ_EINT1, IRQ_EINT2, IRQ_EINT3, IRQ_EINT4_7, IRQ_EINT8_23, IRQ_RSV6, IRQ_BATT_FLT,
    IRQ_TICK, IRQ_WDT, IRQ_TIMER0, IRQ_TIMER1, IRQ_TIMER2, IRQ_TIMER3, IRQ_TIMER4, IRQ_UART2,
    IRQ_LCD, IRQ_DMA0, IRQ_DMA1, IRQ_DMA2, IRQ_DMA3, IRQ_SDI, IRQ_SPI0, IRQ_UART1,
    IRQ_RSV24, IRQ_USBD, IRQ_USBH, IRQ_IIC, IRQ_UART0, IRQ_SPI1, IRQ_RTC, IRQ_ADC,
};
enum SubIrq {
    SUB_RXD0 = 0, SUB_TXD0, SUB_ERR0, SUB_RXD1, SUB_TXD1, SUB_ERR1, SUB_RXD2, SUB_TXD2, SUB_ERR2,
    SUB_TC, SUB_ADC,
};

class IntController : public Device {
public:
    void serialize(StateIO &s);
    explicit IntController(Cpu &cpu) : cpu(cpu) {}
    const char *name() const override { return "intc"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void raise(int src);          // edge: latch into SRCPND
    void raiseSub(int sub);
    void setLevel(int src, bool level);  // level sources re-latch while asserted
    void setSubLevel(int sub, bool level);  // same for SUBSRCPND sources (UART RX with data waiting)
    void update();

    uint32_t srcpnd = 0, intmod = 0, intmsk = 0xFFFFFFFF, priority = 0x7F, intpnd = 0, intoffset = 0;
    uint32_t subsrcpnd = 0, intsubmsk = 0x7FF, subLevels = 0;
    uint32_t levels = 0;

private:
    Cpu &cpu;
};

class Gpio : public Device {
public:
    void serialize(StateIO &s);
    explicit Gpio(IntController &ic) : ic(ic) {}
    const char *name() const override { return "gpio"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void raiseExt(int eint);   // external interrupt 0..23
    void setExtLevel(int eint, bool level);  // level-triggered EINT4..23 (re-latches while asserted)
    /** Press the power button (GPF0 / EINT0): the guest suspends or wakes. */
    void powerButton();
    bool powerPressed = false;
    uint32_t powerPresses = 0;  // presses so far (wake from sleep, Machine::asleep); not saved
    int gpfReads = 0;
    uint32_t extLevels = 0;
    uint32_t regs[0x100 / 4] = {};
    uint32_t eintmask = 0x00FFFFF0, eintpend = 0;
    uint32_t gpfdatIn = 0xFF, gpgdatIn = 0xFFFF;
    uint32_t gstatus2 = 0;
    void updateExt();

private:
    IntController &ic;
};

// PWM timers 0..4. Time is driven in nanoseconds by the machine.
class PwmTimers : public Device {
public:
    uint64_t fired = 0;  // timer interrupts raised (diagnostics)
    void serialize(StateIO &s);
    explicit PwmTimers(IntController &ic) : ic(ic) {}
    const char *name() const override { return "pwm"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void advance(uint64_t nowNs);
    uint64_t nextEventNs() const;   // absolute time of the next timer underflow (or ~0)

    uint64_t pclkHz = 50750000;
    uint32_t tcfg0 = 0, tcfg1 = 0, tcon = 0, tcntb[5] = {}, tcmpb[5] = {};

private:
    IntController &ic;
    struct T {
        bool running = false;
        uint64_t startNs = 0;   // time the current count started at `startCount`
        uint32_t startCount = 0;
    } t[5];
    uint64_t now = 0;
    uint64_t tickNs(int n) const;  // ns per count
    static constexpr uint64_t kMaxOwedNs = 250000000;  // catch up at most this far behind
    uint32_t count(int n) const;
    void startTimer(int n, uint32_t from);
};

class Uart : public Device {
public:
    void serialize(StateIO &s);
    Uart(IntController &ic, int idx) : ic(ic), idx(idx) {}
    const char *name() const override { return "uart"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void feed(uint8_t c) {
        // An app that is not reading must not grow this without bound (a phone sleep that
        // stalls the guest, with NMEA still arriving, otherwise wakes into an interrupt storm).
        if (rx.size() >= 2048) return;
        rx.push_back(c);
        updateRx();
    }
    /** The guest has receive interrupts on, i.e. a driver has the port open. */
    bool rxEnabled() const { return rxIntEnabled(); }
    /**
     * A program has this port open. The serial driver turns RX interrupt mode on when it loads;
     * opening a handle is what enables the FIFO and unmasks this port's interrupt. A mask of zero
     * (every source unmasked, as after a bad wake) or a queue nobody is draining is not an open port.
     */
    bool appOpen(uint32_t intmsk, uint32_t intsubmsk, int irq, int sub) const {
        if (intmsk == 0 || intsubmsk == 0 || (ufcon & 1) == 0 || rx.size() > 1024) return false;
        return rxIntEnabled() && !(intsubmsk & (1u << sub)) && !(intmsk & (1u << irq));
    }
    /** Bytes waiting in the receive queue (the IrDA bridge paces its frames by it, irda.h). */
    size_t rxQueued() const { return rx.size(); }
    /**
     * TX interrupt as the hardware's level type (UCON bit 9): asserted for as long as TX interrupts
     * are on, since the TX FIFO here is always empty. The serial driver fills the 16-byte FIFO, then
     * waits for that interrupt; the per-write edge loses it for longer writes, which then crawl at
     * the driver's IST timeout (~256 ms per 16 bytes). Set for UART2, the infrared port (irda.h).
     */
    bool txLevel = false;
    void updateTx() { if (txLevel) ic.setSubLevel(idx * 3 + 1, ((ucon >> 2) & 3) == 1 && (ucon & 0x200)); }
    std::function<void(int idx, uint8_t c)> onTx;
    /** A device on the other end of the line (UART0: the virtual GSM modem, modem.h). */
    std::function<void(uint8_t c)> peer;
    std::string lineBuf;
    bool traceConfig = false;
    int traceReads = 0;
    uint32_t umstat = 1;   // bit0: CTS asserted

private:
    bool rxIntEnabled() const { return (ucon & 3) != 0; }
    // RX is level-triggered, as on DE: the sub-interrupt re-latches while bytes are waiting.
    void updateRx() { ic.setSubLevel(idx * 3, rxIntEnabled() && !rx.empty()); }
    IntController &ic;
    int idx;
    uint32_t ulcon = 0, ucon = 0, ufcon = 0, umcon = 0, ubrdiv = 0;
    std::deque<uint8_t> rx;
};

class ClockPower : public Device {
public:
    void serialize(StateIO &s);
    explicit ClockPower(Cpu &cpu) : cpu(cpu) {}
    const char *name() const override { return "clkpwr"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    uint32_t locktime = 0x00FFFFFF, mpllcon = 0x0005C080, upllcon = 0x00028080, clkcon = 0x7FFF0,
             clkslow = 4, clkdivn = 3;

private:
    Cpu &cpu;
};

class Rtc : public Device {
public:
    void serialize(StateIO &s);
    explicit Rtc(IntController &ic) : ic(ic) {}
    const char *name() const override { return "rtc"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;
    void advance(uint64_t nowNs);
    int64_t offsetSec = 0;    // guest time = host local time + offset
    uint32_t rtccon = 0, ticnt = 0, rtcalm = 0, alm[6] = {};
    uint64_t lastTickNs = 0;
    const uint64_t *virtNs = nullptr;  // deterministic clock, if set

private:
    IntController &ic;
    int64_t localNow() const;
    uint32_t latched[7] = {};
    uint32_t pending[7] = {}, pendingMask = 0;
    void latch();
    void applyPending();
};

// Registers that only need to hold their values (memory controller, watchdog, misc).
class RegFile : public Device {
public:
    void serialize(StateIO &s);
    RegFile(const char *n, uint32_t size) : nm(n), regs(size / 4, 0) {}
    const char *name() const override { return nm; }
    uint32_t read(uint32_t pa, int) override { return regs[((pa & 0xFFFFF) / 4) % regs.size()]; }
    void write(uint32_t pa, uint32_t v, int) override { regs[((pa & 0xFFFFF) / 4) % regs.size()] = v; }
    const char *nm;
    std::vector<uint32_t> regs;
};

}  // namespace wm
