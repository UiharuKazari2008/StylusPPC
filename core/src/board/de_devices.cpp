#include "de_devices.h"
#include "../util/log.h"

namespace wm {

// ---------------------------------------------------------------------------------------------
// Host link: ACKless lines on Sideband's page. See HostLink in de_devices.h.

std::string HostLink::verbOf(const std::string &line) {
    auto sp = line.find(' ');
    return sp == std::string::npos ? line : line.substr(0, sp);
}

void HostLink::send(const std::string &line) {
    std::string clean;
    clean.reserve(line.size());
    for (unsigned char c : line) {
        if (c == '\n' || c == '\r') continue;
        clean.push_back((char)c);
    }
    if (clean.size() > 240) {
        clean.resize(240);
        while (!clean.empty() && ((unsigned char)clean.back() & 0xC0) == 0x80) clean.pop_back();
    }
    if (clean.empty()) return;
    std::lock_guard<std::mutex> lk(mu);
    const std::string verb = verbOf(clean);
    // The latched front line is the one the guest is copying. Leave it alone.
    const size_t start = (rxLatched && !rx.empty()) ? 1 : 0;
    for (size_t i = start; i < rx.size(); i++) {
        if (verbOf(rx[i]) == verb) { rx[i] = clean; return; }
    }
    if (rx.size() >= 16) {
        if (start < rx.size()) rx.erase(rx.begin() + (long)start);
        else return;
    }
    rx.push_back(clean);
}

std::string HostLink::take() {
    std::lock_guard<std::mutex> lk(mu);
    auto nl = tx.rfind('\n');
    if (nl == std::string::npos) return {};
    std::string out = tx.substr(0, nl);
    tx.erase(0, nl + 1);
    return out;
}

uint32_t HostLink::read(uint32_t off) {
    std::lock_guard<std::mutex> lk(mu);
    // Marshal.ReadInt32 on CF is four byte loads, or'd back together. A byte load is handed
    // the whole register shifted down to that byte, so the four pieces or back to the word.
    uint32_t shift = (off & 3) * 8;
    off &= ~3u;
    uint32_t w = 0;
    if (off == 0x00) w = id;
    else if (off == 0x04) {
        if (!rx.empty()) {
            rxLatched = true;
            w = (uint32_t)rx.front().size();
        }
    } else if (off == 0x0C) w = (uint32_t)tx.size();
    else if (off >= 0x20 && off < 0x20 + 240 && !rx.empty()) {
        const std::string &s = rx.front();
        uint32_t i = off - 0x20;
        for (int b = 0; b < 4; b++) if (i + b < s.size()) w |= (uint32_t)(uint8_t)s[i + b] << (8 * b);
    }
    return w >> shift;
}

void HostLink::write(uint32_t off, uint32_t v, int size) {
    if (size < 1) size = 1;
    if (size > 4) size = 4;
    std::lock_guard<std::mutex> lk(mu);
    for (int b = 0; b < size; b++) {
        uint32_t o = off + (uint32_t)b;
        uint8_t byte = (uint8_t)(v >> (8 * b));
        if (o == 0x04) {
            if (byte == 0 && rxLatched && !rx.empty()) rx.pop_front();
            rxLatched = false;
            continue;
        }
        if (o == 0x08) {
            if (tx.size() > 8192) {
                auto nl = tx.find('\n');
                if (nl == std::string::npos) tx.clear();
                else tx.erase(0, nl + 1);
            }
            tx.push_back((char)byte);
        }
    }
}

bool BulkPipe::put(const uint8_t *p, size_t n, bool last) {
    std::lock_guard<std::mutex> lk(mu);
    if (rx.size() >= 64 || n > CAP) return false;
    Chunk c;
    c.last = last;
    c.data.assign(p, p + n);
    rx.push_back(std::move(c));
    return true;
}

bool BulkPipe::take(std::vector<uint8_t> &out, bool &last) {
    std::lock_guard<std::mutex> lk(mu);
    if (tx.empty()) return false;
    out = std::move(tx.front().data);
    last = tx.front().last;
    tx.pop_front();
    return true;
}

uint32_t BulkPipe::read(uint32_t off) {
    std::lock_guard<std::mutex> lk(mu);
    uint32_t shift = (off & 3) * 8;
    off &= ~3u;
    uint32_t w = 0;
    if (off == 0x00) w = ID;
    else if (off == 0x04) {
        if (!rx.empty()) {
            rxLatched = true;
            w = 1u | (rx.front().last ? 2u : 0u);
        }
    } else if (off == 0x08) w = rx.empty() ? 0u : (uint32_t)rx.front().data.size();
    else if (off == 0x10) w = (uint32_t)tx.size();
    else if (off >= 0x20 && off < 0x20 + CAP && !rx.empty()) {
        const std::vector<uint8_t> &d = rx.front().data;
        uint32_t i = off - 0x20;
        for (int b = 0; b < 4; b++) if (i + b < d.size()) w |= (uint32_t)d[i + b] << (8 * b);
    }
    return w >> shift;
}

void BulkPipe::write(uint32_t off, uint32_t v, int size) {
    if (size < 1) size = 1;
    if (size > 4) size = 4;
    std::lock_guard<std::mutex> lk(mu);
    for (int b = 0; b < size; b++) {
        uint32_t o = off + (uint32_t)b;
        uint8_t byte = (uint8_t)(v >> (8 * b));
        if (o == 0x04) {
            if (byte == 0 && rxLatched && !rx.empty()) rx.pop_front();
            rxLatched = false;
            continue;
        }
        if (o >= 0x0C && o < 0x10) {
            unsigned sh = (o & 3) * 8;
            commitAcc = (commitAcc & ~(0xFFu << sh)) | ((uint32_t)byte << sh);
            if ((o & 3) == 3) {
                if (tx.size() < 64) {
                    size_t n = commitAcc & 0xFFFF;
                    if (n > stage.size()) n = stage.size();
                    if (n > CAP) n = CAP;
                    Chunk c;
                    c.last = (commitAcc & 0x80000000u) != 0;
                    c.data.assign(stage.begin(), stage.begin() + (std::ptrdiff_t)n);
                    tx.push_back(std::move(c));
                    stage.clear();
                }
                commitAcc = 0;
            }
            continue;
        }
        if (o >= 0x20 && o < 0x20 + CAP) {
            if (stage.size() < CAP) stage.resize(CAP);
            stage[o - 0x20] = byte;
        }
    }
}

uint32_t Sideband::read(uint32_t pa, int) {
    uint32_t off = pa & 0xFFF;
    if (off < 0x400) return cmd.read(off);
    if (off < 0x800) return sta.read(off - 0x400);
    return bulk.read(off - 0x800);
}

void Sideband::write(uint32_t pa, uint32_t v, int size) {
    uint32_t off = pa & 0xFFF;
    if (off < 0x400) cmd.write(off, v, size);
    else if (off < 0x800) sta.write(off - 0x400, v, size);
    else bulk.write(off - 0x800, v, size);
}

void SpiKeyboard::push(uint8_t code) {
    // Codes are queued even while the driver has the port out of interrupt mode (it switches modes
    // while servicing a key); dropping them there loses key releases and leaves keys stuck down.
    //
    // EINT1 is level-triggered while codes are waiting: the driver reads one code per interrupt and then
    // clears SRCPND, so an edge raised during the read would be wiped and the rest of a multi-code
    // sequence (Fn + key + Fn up) would sit in the queue until the next key event.
    if (q.size() >= 64) return;
    q.push_back(code);
    updateLevel();
}

void SpiKeyboard::updateLevel() { ic.setLevel(IRQ_EINT1, enabled() && !q.empty()); }

uint32_t SpiKeyboard::read(uint32_t pa, int) {
    switch (pa & 0xFF) {
        case 0x20: return spcon;
        case 0x24: return 1;  // SPSTA: ready
        case 0x28: return sppin;
        case 0x2C: return sppre;
        case 0x30: return sptdat;
        case 0x34: {
            if (q.empty()) return 0;
            uint8_t c = q.front();
            q.pop_front();
            updateLevel();
            return c;
        }
    }
    return 0;
}

void SpiKeyboard::write(uint32_t pa, uint32_t v, int) {
    switch (pa & 0xFF) {
        case 0x20: {
            spcon = v;
            updateLevel();
            break;
        }
        case 0x28: sppin = v; break;
        case 0x2C: sppre = v; break;
        case 0x30: sptdat = v; break;
    }
}

// ---------------------------------------------------------------------------------------------
// EmulServ (0x500F5000)

void EmulServ::raise(uint32_t bits) {
    if (trace) LOGI("emulserv: raise %x (mask %x, pending %x)", bits, mask, pending);
    pending |= bits;
    if (pending & mask) gpio.raiseExt(11);
}

uint32_t EmulServ::read(uint32_t off) {
    if (off == 0) return mask;
    if (off == 4) {
        uint32_t v = pending;
        if (trace) LOGI("emulserv: pending read %x", v);
        pending = 0;
        return v;
    }
    return 0;
}

void EmulServ::write(uint32_t off, uint32_t v) {
    if (off == 0) {
        if (trace) LOGI("emulserv: mask %x", v);
        mask = v;
        if (pending & mask) gpio.raiseExt(11);
    }
}

// ---------------------------------------------------------------------------------------------
// 0x50000000 page

DePage::DePage(Uart *u[3], EmulServ &es) : emulServ(es) {
    for (int i = 0; i < 3; i++) uarts[i] = u[i];
    dmaRegs[0x18 / 4] = 0xFFFFFFFF;  // InputStatus: nothing queued
    dmaRegs[0x24 / 4] = 0xFFFFFFFF;  // OutputStatus: closed
}

uint8_t *DePage::byteDev(uint32_t off, const char *&nm, uint32_t &len) {
    if (off >= 0xFFFF0 && off < 0xFFFF4) { nm = "battery"; len = 4; return battery + (off - 0xFFFF0); }
    if (off >= 0xFFFD0 && off < 0xFFFD7) { nm = "leds"; len = 7; return leds + (off - 0xFFFD0); }
    if (off >= 0xFFFC0 && off < 0xFFFC2) { nm = "speaker"; len = 2; return speaker + (off - 0xFFFC0); }
    return nullptr;
}

uint32_t DePage::read(uint32_t pa, int size) {
    uint32_t off = pa & 0xFFFFF;
    if (off < 0xC000) return uarts[off >> 14]->read(pa, size);
    if (off >= 0xF0000 && off < 0xF4000) {  // DMA transport: present but no host channels yet
        uint32_t r = off - 0xF0000;
        if (traceDma && dmaLog++ < 400) LOGI("dmatrans: read%d +%04x", size * 8, r);
        if (r >= 0x2000 && r < 0x2120) return dmaRegs[(r - 0x2000) / 4];
        return 0;
    }
    if (off >= 0xF4000 && off < 0xF4010) {
        switch (off & 0xF) {
            case 0x0: return fsPb;
            case 0x8: return 0;  // IOPending
            case 0xC: return fsResult;
        }
        return 0;
    }
    if (off >= 0xF5000 && off < 0xF5008) return emulServ.read(off & 7);
    const char *nm;
    uint32_t len;
    if (uint8_t *p = byteDev(off, nm, len)) {
        uint32_t v = 0;
        for (int i = 0; i < size && (off & 0xF) + i < 16; i++) v |= (uint32_t)p[i] << (8 * i);
        if (p == battery) batteryReads++;
        if (logBudget-- > 0) LOGI("%s: read%d +%u -> %x", nm, size * 8, (unsigned)(off & 0xF), v);
        return v;
    }
    if (logBudget-- > 0) LOGW("de page: read%d %08x", size * 8, pa);
    return 0;
}

void DePage::write(uint32_t pa, uint32_t v, int size) {
    uint32_t off = pa & 0xFFFFF;
    if (off < 0xC000) { uarts[off >> 14]->write(pa, v, size); return; }
    if (off >= 0xF0000 && off < 0xF4000) {
        uint32_t r = off - 0xF0000;
        if (traceDma && dmaLog++ < 400) LOGI("dmatrans: write%d +%04x <- %08x", size * 8, r, v);
        if (r >= 0x2000 && r < 0x2120) {
            uint32_t i = (r - 0x2000) / 4;
            if (i == 0x18 / 4 || i == 0x24 / 4) return;  // status registers are read-only here
            dmaRegs[i] = v;
        }
        return;
    }
    if (off >= 0xF4000 && off < 0xF4010) {
        switch (off & 0xF) {
            case 0x0: fsPb = v; break;
            case 0x4:
                // Operations complete synchronously (IOPending always reads 0), so vcefsd never polls
                // after an operation. Code 0 (PollCompletion) comes from emulserv's change handler,
                // which decides on Result alone: 1 = no share (DeactivateDevice, the card goes),
                // anything else = share present (ActivateDevice if the card is gone). DE answers
                // 1 / 0 here; leaving the last operation's Result in place made a card that came
                // back look removed (a failed call returns 1) and WM never mounted it again.
                fsResult = folderShare ? folderShare(v, fsPb) : 1;  // 1: sharing disabled
                break;
        }
        return;
    }
    if (off >= 0xF5000 && off < 0xF5008) { emulServ.write(off & 7, v); return; }
    const char *nm;
    uint32_t len;
    if (uint8_t *p = byteDev(off, nm, len)) {
        for (int i = 0; i < size; i++) p[i] = (uint8_t)(v >> (8 * i));
        if (logBudget-- > 0) LOGI("%s: write%d +%u <- %x", nm, size * 8, (unsigned)(off & 0xF), v);
        if (p >= leds && p < leds + 7) nled.written((unsigned)(p - leds));  // notification LED / vibrator (nled.h)
        return;
    }
    if (logBudget-- > 0) LOGW("de page: write%d %08x <- %08x", size * 8, pa, v);
}

}  // namespace wm
