// WMDroid motion sensor: see motion.h for the guest interface.
#include "motion.h"
#include "../util/log.h"
#include <chrono>
#include <cmath>

namespace wm {

namespace {

constexpr uint64_t WANT_AFTER_READ_NS = 2000000000ull;
constexpr float G = 9.80665f;

uint64_t hostNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

int32_t htc(float a) {   // Android's reaction force -> HTC's gravity, 1000 per g
    float v = -a / G * 1000.0f;
    if (!std::isfinite(v)) return 0;
    return (int32_t)std::lround(std::fmax(-4000.0f, std::fmin(4000.0f, v)));
}

}  // namespace

uint32_t Motion::read(uint32_t pa, int) {
    switch (pa & 0xFFF) {
    case 0x00: return ID;
    case 0x04: return open;
    case 0x08: return seq;
    case 0x0C: case 0x10: case 0x14:
        lastReadNs = hostNs();
        readCount++;
        return (uint32_t)((pa & 0xFFF) == 0x0C ? x : (pa & 0xFFF) == 0x10 ? y : z);
    case 0x18: return host ? 1u : 0u;
    }
    return 0;
}

void Motion::write(uint32_t pa, uint32_t v, int) {
    if ((pa & 0xFFF) == 0x04) open = v;
    if ((pa & 0xFFF) == 0x1C) {
        std::lock_guard<std::mutex> l(mu);
        if (unknownCodes.insert(v).second) LOGI("motion: SEN0: IOCTL %08x not handled", v);
    }
}

bool Motion::wanted() const {
    if (open) return true;
    uint64_t last = lastReadNs;
    return last && hostNs() - last < WANT_AFTER_READ_NS;
}

void Motion::setAccel(float ax, float ay, float az) {
    x = htc(ax);
    y = htc(ay);
    z = htc(az);
    host = true;
    seq++;
}

void Motion::clearAccel() {
    host = false;
    x = 0;
    y = 0;
    z = -1000;
}

}  // namespace wm
