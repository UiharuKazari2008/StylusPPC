#include "audio.h"
#include "../util/log.h"
#include <cstring>
#include <vector>

namespace wm {

enum { DISRC, DISRCC, DIDST, DIDSTC, DCON, DSTAT, DCSRC, DCDST, DMASKTRIG };

uint32_t AudioIis::read(uint32_t pa, int) {
    uint32_t i = (pa & 0x1F) / 4;
    if (i == 0) return regs[0] | 0xC0;  // TX/RX FIFO always ready
    return i < 4 ? regs[i] : 0;
}

void AudioIis::write(uint32_t pa, uint32_t v, int) {
    uint32_t i = (pa & 0x1F) / 4;
    if (i < 4) regs[i] = v;
}

uint8_t *AudioDma::ramPtr(uint32_t pa, uint32_t len) {
    bool wr;
    uint8_t *p = bus.hostPage(pa, wr);
    uint8_t *end = len ? bus.hostPage(pa + len - 1, wr) : p;
    if (!p || !end || end != p + len - 1) return nullptr;  // must be one contiguous host block
    return p;
}

void AudioDma::start(int n, uint32_t addr) {
    Chan &c = ch[n];
    uint32_t tc = c.r[DCON] & 0xFFFFF;
    uint32_t unit = 1u << ((c.r[DCON] >> 20) & 3);  // DSZ: 0 byte, 1 halfword, 2 word
    c.bytes = tc * unit;
    c.running = true;
    c.startNs = now;
    uint64_t frames = c.bytes / 4;
    c.doneNs = now + frames * 1000000000ull / kRate;
    if (n == 2) {
        if (uint8_t *p = ramPtr(addr, c.bytes)) {
            if (onPlay) onPlay(reinterpret_cast<const int16_t *>(p), (uint32_t)frames);
        } else {
            LOGW("dma2: playback buffer %08x+%x not in RAM", addr, c.bytes);
        }
    } else if (n == 1) {
        if (uint8_t *p = ramPtr(addr, c.bytes)) {
            if (onCapture) onCapture(reinterpret_cast<int16_t *>(p), (uint32_t)frames);
            else memset(p, 0, c.bytes);
        }
    }
}

void AudioDma::finish(int n) {
    Chan &c = ch[n];
    ic.raise(n == 2 ? IRQ_DMA2 : n == 1 ? IRQ_DMA1 : n == 0 ? IRQ_DMA0 : IRQ_DMA3);
    // DCON.RELOAD (bit 22) clear = auto-reload: the channel carries on from the current
    // DISRC/DIDST whether or not the driver has queued a new buffer yet. Stopping instead left
    // playback dead for good whenever the guest was a moment late (WMP went silent mid-song).
    bool reload = !(c.r[DCON] & (1u << 22));
    if (c.queued || reload) {
        c.queued = false;
        uint64_t prevDone = c.doneNs;
        start(n, n == 1 ? c.r[DIDST] : c.r[DISRC]);
        if (now - prevDone < 100000000ull) {  // keep the sample clock steady unless far behind
            uint64_t dur = c.doneNs - c.startNs;
            c.startNs = prevDone;
            c.doneNs = prevDone + dur;
        }
    } else {
        LOGD("dma%d: transfer done, no reload (DCON %08x)", n, c.r[DCON]);
        c.running = false;
        c.r[DMASKTRIG] &= ~2u;
    }
}

void AudioDma::advance(uint64_t nowNs) {
    now = nowNs;
    for (int n = 0; n < 4; n++) {
        Chan &c = ch[n];
        if (c.running && now >= c.doneNs) finish(n);
    }
}

uint64_t AudioDma::nextEventNs() const {
    uint64_t best = ~0ull;
    for (const Chan &c : ch)
        if (c.running && c.doneNs < best) best = c.doneNs;
    return best;
}

uint32_t AudioDma::read(uint32_t pa, int) {
    uint32_t off = pa & 0xFF;
    int n = off / 0x40, i = (off % 0x40) / 4;
    if (n > 3 || i > 8) return 0;
    Chan &c = ch[n];
    if (i == DSTAT) {
        if (!c.running) return 0;
        uint32_t tc = c.r[DCON] & 0xFFFFF;
        uint64_t span = c.doneNs - c.startNs;
        uint64_t left = span && now < c.doneNs ? (c.doneNs - now) * tc / span : 0;
        return (1u << 20) | (uint32_t)left;  // busy + current count
    }
    if (i == DCSRC) return c.r[DISRC];
    if (i == DCDST) return c.r[DIDST];
    return c.r[i];
}

void AudioDma::write(uint32_t pa, uint32_t v, int) {
    uint32_t off = pa & 0xFF;
    int n = off / 0x40, i = (off % 0x40) / 4;
    if (n > 3 || i > 8) return;
    Chan &c = ch[n];
    if (i == DSTAT || i == DCSRC || i == DCDST) return;
    if ((i == DISRC && n == 2) || (i == DIDST && n == 1)) {
        c.r[i] = v;
        if (c.running) { c.queued = true; c.queuedAddr = v; }  // double buffering
        return;
    }
    if (i == DMASKTRIG) {
        bool was = c.r[DMASKTRIG] & 2;
        c.r[DMASKTRIG] = v & ~4u;
        if (v & 4) { c.running = false; c.queued = false; c.r[DMASKTRIG] &= ~2u; return; }  // STOP
        if ((v & 2) && !was && !c.running) start(n, n == 1 ? c.r[DIDST] : c.r[DISRC]);
        if (!(v & 2)) { c.running = false; c.queued = false; }
        return;
    }
    c.r[i] = v;
}

}  // namespace wm
