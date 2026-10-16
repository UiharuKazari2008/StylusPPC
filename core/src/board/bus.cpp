#include "bus.h"
#include "../util/log.h"
#include <cstring>

namespace wm {

Bus::Bus() {}

void Bus::mapMemory(uint32_t base, uint32_t size, uint8_t *host, bool writable, Device *writeDev) {
    for (uint32_t off = 0; off < size; off += 0x100000) {
        Region &r = map[(base + off) >> 20];
        r.host = host;
        r.base = base;
        r.writable = writable;
        r.directRead = true;
        r.dev = writeDev;
    }
    if (onMapChange) onMapChange();
}

void Bus::mapDevice(uint32_t base, uint32_t size, Device *dev) {
    if (size < 0x100000) size = 0x100000;
    for (uint32_t off = 0; off < size; off += 0x100000) {
        Region &r = map[(base + off) >> 20];
        r.host = nullptr;
        r.dev = dev;
    }
    if (onMapChange) onMapChange();
}

void Bus::setDirectRead(uint32_t base, uint32_t size, bool direct) {
    for (uint32_t off = 0; off < size; off += 0x100000) map[(base + off) >> 20].directRead = direct;
    if (onMapChange) onMapChange();
}

uint32_t Bus::read(uint32_t pa, int size) {
    Region &r = map[pa >> 20];
    if (r.host && r.directRead) {
        uint8_t *p = r.host + (pa - r.base);
        if (size == 4) { uint32_t v; memcpy(&v, p, 4); return v; }
        if (size == 2) { uint16_t v; memcpy(&v, p, 2); return v; }
        return *p;
    }
    if (r.dev) return r.dev->read(pa, size);
    if (logUnmapped && unmappedCount++ < 200) LOGW("bus: unmapped read%d pa=%08x pc=%08x", size * 8, pa, pcNow ? pcNow() : lastPc);
    return 0;
}

void Bus::write(uint32_t pa, uint32_t value, int size) {
    Region &r = map[pa >> 20];
    if (r.dev) { r.dev->write(pa, value, size); return; }
    if (r.host && r.writable) {
        uint8_t *p = r.host + (pa - r.base);
        if (size == 4) memcpy(p, &value, 4);
        else if (size == 2) { uint16_t v = (uint16_t)value; memcpy(p, &v, 2); }
        else *p = (uint8_t)value;
        return;
    }
    if (logUnmapped && unmappedCount++ < 200) LOGW("bus: unmapped write%d pa=%08x v=%08x pc=%08x", size * 8, pa, value, pcNow ? pcNow() : lastPc);
}

}  // namespace wm
