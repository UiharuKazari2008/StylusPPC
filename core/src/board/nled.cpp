#include "nled.h"
#include "../util/log.h"

namespace wm {

static inline uint8_t rd(const uint8_t *p) { return __atomic_load_n(p, __ATOMIC_RELAXED); }

uint32_t NotifyLeds::decode() const {
    uint8_t led = rd(regs + 0), on = rd(regs + 1), vib = rd(regs + 3);
    uint32_t m = 0;
    if (led) m |= LIGHT;
    if (led == 2) {
        m |= BLINK;
        // The byte is OnTime/100 us, clamped at 255 (25.5 ms). A clamped value says nothing, so
        // 0 = unknown; anything shorter is a short flash and reads as one 50 ms unit at least.
        if (on > 0 && on < 255) {
            uint32_t u = (on * 100u + 25000u) / 50000u;
            m |= (u ? u : 1u) << ON_TIME_SHIFT;
        }
    }
    if (vib) m |= VIBRATE;
    return m;
}

void NotifyLeds::written(unsigned offset) {
    bool vib = rd(regs + 3) != 0;
    if (vib && !vibWasOn) {
        vibLatch.store(true);
        pulses.fetch_add(1);
    }
    vibWasOn = vib;
    if (offset != 0 && offset != 3) return;  // half-way through an update: log the result only
    uint32_t m = decode();
    if (m != lastLogged) {
        lastLogged = m;
        LOGI("nled: light %s, vibrate %s (regs %u/%u/%u %u/%u/%u, state %#x)",
             (m & BLINK) ? "blink" : (m & LIGHT) ? "on" : "off", (m & VIBRATE) ? "on" : "off", rd(regs + 0),
             rd(regs + 1), rd(regs + 2), rd(regs + 3), rd(regs + 4), rd(regs + 5), (unsigned)state(false));
    }
}

uint32_t NotifyLeds::state(bool consume) {
    uint32_t m = decode();
    bool latched = consume ? vibLatch.exchange(false) : vibLatch.load();
    if (latched) m |= VIBRATE;
    m |= (pulses.load() & 0xFF) << PULSES_SHIFT;
    return m;
}

void NotifyLeds::reset(uint8_t *mutableRegs) {
    for (int i = 0; i < 6; i++) __atomic_store_n(mutableRegs + i, (uint8_t)0, __ATOMIC_RELAXED);
    vibWasOn = false;
    written(0);
}

}  // namespace wm
