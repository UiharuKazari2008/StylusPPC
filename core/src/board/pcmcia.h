// PCMCIA slot of the Device Emulator board: a Cirrus PD6710 controller with an NE2000-compatible
// (DP8390) network card, the only network hardware the WM images have drivers for
// (pcc_smdk2410.dll + ne2000.dll). See docs/research-gps-network.md section B1.
//
//   0x10000000 + a   card attribute/common memory (CIS at even bytes, FCSR at 0x3FA, RAM 0x4000-0x7FFF)
//   0x11000000 + p   card I/O port p, through the controller's two I/O windows
//   0x110003E0/E1    controller index / data
//
// Card interrupts are EINT8, level-triggered while (ISR & IMR) != 0; card insertion raises EINT3.
#pragma once
#include "bus.h"
#include "s3c2410.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace wm {

class StateIO;

class Pcmcia : public Device {
public:
    Pcmcia(IntController &ic, Gpio &gpio) : ic(ic), gpio(gpio) { resetNic(); }
    const char *name() const override { return "pcmcia"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;

    /** Plug the card in or out (raises the card-detect interrupt the guest is waiting for). */
    void setInserted(bool on);
    bool inserted() const { return cardIn; }
    void setMac(const uint8_t mac[6]);
    /**
     * A frame from the network for the card (emulation thread only). False when the receive ring
     * is full: keep the frame and offer it again later (the network side never drops for that).
     */
    bool receive(const uint8_t *frame, size_t len);
    /** True while the card is up and receiving (the guest driver started it). */
    bool receiving() const { return cardIn && powered() && (cr & 0x01) == 0 && (cr & 0x02); }
    void serialize(StateIO &s);
    /** Frames the card transmits. */
    std::function<void(const uint8_t *frame, size_t len)> onTransmit;

private:
    IntController &ic;
    Gpio &gpio;

    // PD6710
    bool cardIn = false;
    uint8_t index = 0, power = 0, intCtl = 0, cscChange = 0, cscConfig = 0, winEnable = 0, ioCtl = 0;
    uint8_t chipInfo = 0xC0;
    uint16_t ioStart[2] = {}, ioEnd[2] = {};
    bool powered() const { return (power & 0x90) == 0x90; }
    uint8_t controllerRead(uint8_t reg);
    void controllerWrite(uint8_t reg, uint8_t v);
    bool mapIo(uint32_t port, uint32_t &cardPort) const;

    // Card attribute memory
    uint8_t fcsr = 0;
    uint32_t memRead(uint32_t a, int size);
    void memWrite(uint32_t a, uint32_t v, int size);

    // DP8390
    uint8_t cr = 0x21, pstart = 0, pstop = 0, bnry = 0, tsr = 0, tpsr = 0, isr = 0, imr = 0;
    uint8_t rcr = 0, tcr = 0, dcr = 0, rsr = 0, curr = 0;
    uint16_t tbcr = 0, rsar = 0, rbcr = 0, dmaAddr = 0, dmaCount = 0;
    uint8_t par[6] = {}, mar[8] = {};
    uint8_t prom[32] = {};
    uint8_t ram[0x4000] = {};  // NIC addresses 0x4000-0x7FFF
    void resetNic();
    uint32_t nicRead(uint32_t port, int size);
    void nicWrite(uint32_t port, uint32_t v, int size);
    uint8_t dmaByte(bool write, uint8_t v);
    void raise(uint8_t bits);
    void updateIrq();
    bool accept(const uint8_t *frame, size_t len) const;
};

}  // namespace wm
