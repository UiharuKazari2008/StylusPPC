// Host-side fixes to Microsoft driver code in guest RAM (a WMDroid-only device; DE has none).
//
// A few of the images' own drivers need a word or two changed to behave on a phone:
//   - battdrvr.dll (WM 6.5.3, 6.1.4): BatteryFlag LOW below 65 % and CRITICAL below 20 % made the shell
//     warn "Main battery low" at 64 %; now LOW below 20 % and CRITICAL below 10 %. CHARGING was only
//     set on the poll where the level rose; now whenever AC is online (docs/research-battery.md).
//   - nleddrvr.dll (WM 6.5.3, 6.1.4): rejected ossvcs' blink request (MetaCycleOff != 0, error 87),
//     so the notification light never came on (docs/research-notification-led.md).
//   - fakeril.dll (WM 6.5.3): the dial thread "answers" every call after 2 s. The gate patch keeps
//     the call dialling until the helper says the real (Android) call is active
//     (docs/research-phone-calls-pim.md, section E). Only while the helper arms it.
//
// The host sees all of physical memory, so each fix is found by a byte signature of the code around
// it (any load address, any copy) rather than at a fixed VA, and the guest needs no privileges.
// The CPU calls scanPage() for a physical page just before code there first runs (JIT compile or
// interpreter fetch) and again after the page has been written, so a module page the kernel drops
// and pages back in (possibly elsewhere) is fixed again before it runs. Translated blocks of a page
// are discarded whenever a word in it changes. Every word is checked to hold its original (or
// already patched) value first, so other images and builds are left alone.
//
// Registers (physical page CodePatcher::BASE), used by the guest helper (guest/Helper/Calls.cs):
//   +0x00 ID      r   'PTCH'
//   +0x04 STATUS  r   ST_* bits: which fixes are in place now (checked when read)
//   +0x08 GATE    w   any value: the dialling FakeRIL call connects now (the real call is active)
//                 r   the gate counter
//   +0x0C ARM     w   1: calls are app-driven, hold FakeRIL calls at the gate; 0: FakeRIL connects
//                     calls by itself again (patch removed)
//                 r   1 while armed
//
// No snapshot state: the fixes live in guest RAM. A gate found patched (a snapshot, or an older
// helper that patched from inside the guest) counts as armed. A guest restart disarms the gate until
// the helper runs again.
#pragma once
#include "bus.h"
#include <cstdint>
#include <vector>

namespace wm {

class Cpu;

class CodePatcher : public Device {
public:
    static constexpr uint32_t BASE = 0x5F100000;   // own 1 MB region next to the camera's
    static constexpr uint32_t ID = 0x48435450;     // 'PTCH'
    static constexpr uint32_t ST_BATTERY = 1u << 0;    // battdrvr thresholds / charging
    static constexpr uint32_t ST_NLED = 1u << 1;       // nleddrvr accepts notification blinks
    static constexpr uint32_t ST_GATE_ARMED = 1u << 2; // the helper armed the FakeRIL gate
    static constexpr uint32_t ST_GATE = 1u << 3;       // ...and it is in FakeRIL's code now

    CodePatcher(Cpu &cpu, Bus &bus) : cpu(cpu), bus(bus) {}
    const char *name() const override { return "codepatch"; }
    uint32_t read(uint32_t pa, int size) override;
    void write(uint32_t pa, uint32_t v, int size) override;

    // Cpu::codeScan (emulation thread): code is about to run in the 4 KB physical page at pa.
    void scanPage(uint32_t pa);

    void onGuestReset();   // the guest restarts: the helper arms the gate again once it runs
    void setGateArmed(bool on);
    bool openGate();       // the dialling call connects; false if the gate is not in place
    uint32_t status();     // ST_* bits

    uint64_t pagesScanned = 0;  // diagnostics

private:
    struct Site {
        int patch;                      // index into the patch table
        uint32_t pa;                    // physical address of the signature's anchor word
        std::vector<uint32_t> original; // the words the patch replaced (for removing it)
    };
    enum Match { NONE, ORIGINAL, PATCHED, MIXED };
    Match match(int patch, const uint8_t *page, int off) const;
    bool apply(Site &s, uint8_t *page, int off, bool log);
    void unapply(Site &s);
    uint8_t *pageOf(uint32_t pa) const;
    Site *site(int patch, uint32_t pa, bool &seen);   // finds or adds
    bool verified(const Site &s, Match want) const;
    void invalidate(uint32_t pa);

    Cpu &cpu;
    Bus &bus;
    std::vector<Site> sites;   // places where a signature was seen (pruned when they stop matching)
    bool gateArmed = false;
    uint32_t gateCounter = 0;  // last value written to the gate word
};

}  // namespace wm
