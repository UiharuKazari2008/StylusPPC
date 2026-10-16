// Physical address space: RAM/flash backed by host memory, everything else routed to devices.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

namespace wm {

class Device {
public:
    virtual ~Device() = default;
    virtual uint32_t read(uint32_t pa, int size) = 0;
    virtual void write(uint32_t pa, uint32_t value, int size) = 0;
    virtual const char *name() const = 0;
};

class Bus {
public:
    Bus();
    // Map host memory at [base, base+size) (1 MB granularity). Writes go to `writeDev` if set.
    void mapMemory(uint32_t base, uint32_t size, uint8_t *host, bool writable, Device *writeDev = nullptr);
    void mapDevice(uint32_t base, uint32_t size, Device *dev);
    // Switch a memory region between direct reads and device reads (flash command modes).
    void setDirectRead(uint32_t base, uint32_t size, bool direct);

    // Returns the host pointer for a physical page, or null for device space.
    uint8_t *hostPage(uint32_t pa, bool &writable) const {
        const Region &r = map[pa >> 20];
        if (!r.host || !r.directRead) { writable = false; return nullptr; }
        writable = r.writable;
        return r.host + (pa - r.base);
    }

    uint32_t read(uint32_t pa, int size);
    void write(uint32_t pa, uint32_t value, int size);

    std::function<void()> onMapChange;   // CPU TLB flush
    bool logUnmapped = true;
    uint32_t unmappedCount = 0;
    uint32_t lastPc = 0;                 // updated by the machine for diagnostics
    std::function<uint32_t()> pcNow;     // current guest pc, for diagnostics

private:
    struct Region {
        uint8_t *host = nullptr;
        uint32_t base = 0;
        bool writable = false;
        bool directRead = true;
        Device *dev = nullptr;
    };
    Region map[4096];
};

}  // namespace wm
