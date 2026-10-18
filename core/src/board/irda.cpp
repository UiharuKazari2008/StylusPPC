// Infrared SIR transceiver on UART2 and its frame tunnel (irda.h, docs/research-irda.md).
#include "irda.h"
#include "s3c2410.h"
#include "../util/log.h"
#include <algorithm>
#include <chrono>
#include <cstdio>

namespace wm {

namespace {

const uint8_t BOF = 0xC0, EOF_ = 0xC1, CE = 0x7D;
const uint64_t kSecond = 1000000000ull;
const size_t kMaxQueuedBytes = 256 * 1024;  // SIR bytes for the guest; beyond that, frames are dropped
const size_t kMaxOutbox = 512;              // guest frames waiting for a slow transport
const size_t kFeedLevel = 2048;             // one max I-frame of SIR bytes; the driver's buffer is 4 KB

// IrLAP control field values (P/F bit 0x10 masked off).
const uint8_t U_XID_CMD = 0x2F, U_XID_RSP = 0xAF, U_SNRM = 0x83, U_UA = 0x63, U_DISC = 0x43, U_DM = 0x0F,
              U_UI = 0x03, U_TEST = 0xE3, U_FRMR = 0x87;

uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

// XID frame: A C FI(0x01) src[4] dst[4] flags slot version [discovery info]
struct Xid {
    bool ok = false, command = false;
    uint32_t src = 0, dst = 0;
    uint8_t flags = 0, slot = 0, version = 0;
    const uint8_t *info = nullptr;
    size_t infoLen = 0;
};

Xid parseXid(const uint8_t *f, size_t n) {
    Xid x;
    if (n < 14 || f[2] != 0x01) return x;
    uint8_t c = f[1] & ~0x10;
    if (c != U_XID_CMD && c != U_XID_RSP) return x;
    x.ok = true;
    x.command = c == U_XID_CMD;
    x.src = le32(f + 3);
    x.dst = le32(f + 7);
    x.flags = f[11];
    x.slot = f[12];
    x.version = f[13];
    x.info = f + 14;
    x.infoLen = n - 14;
    return x;
}

int slotCount(uint8_t flags) {
    static const int n[4] = {1, 6, 8, 16};
    return n[flags & 3];
}

// Nickname from discovery info: hint bytes (bit 7 = another follows), charset, then the name.
std::string nickname(const uint8_t *p, size_t n) {
    size_t i = 0;
    while (i < n && (p[i] & 0x80)) i++;
    i += 2;  // last hint byte, charset
    std::string s;
    for (; i < n && p[i] && s.size() < 32; i++) s += (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';
    return s;
}

}  // namespace

uint16_t Irda::fcs(const uint8_t *p, size_t n, uint16_t crc) {
    // CRC-16-CCITT, reflected (0x8408), as PPP and IrLAP use.
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
    }
    return crc;
}

void Irda::encodeSir(const uint8_t *f, size_t n, std::vector<uint8_t> &out) {
    uint16_t c = (uint16_t)~fcs(f, n);
    auto put = [&](uint8_t b) {
        if (b == BOF || b == EOF_ || b == CE) { out.push_back(CE); out.push_back(b ^ 0x20); }
        else out.push_back(b);
    };
    out.push_back(BOF);
    for (size_t i = 0; i < n; i++) put(f[i]);
    put((uint8_t)c);
    put((uint8_t)(c >> 8));
    out.push_back(EOF_);
}

std::string Irda::describe(const uint8_t *f, size_t n) {
    char b[160];
    if (n < 2) return "short frame";
    uint8_t a = f[0], c = f[1];
    bool pf = c & 0x10;
    int ca = a >> 1;
    const char *cr = (a & 1) ? "cmd" : "rsp";
    if ((c & 1) == 0) {
        snprintf(b, sizeof b, "I %02x ns=%d nr=%d%s len=%zu", ca, (c >> 1) & 7, c >> 5, pf ? " P/F" : "", n - 2);
        return b;
    }
    if ((c & 3) == 1) {
        static const char *s[4] = {"RR", "RNR", "REJ", "SREJ"};
        snprintf(b, sizeof b, "%s %s %02x nr=%d%s", s[(c >> 2) & 3], cr, ca, c >> 5, pf ? " P/F" : "");
        return b;
    }
    uint8_t u = c & ~0x10;
    const char *name = u == U_XID_CMD ? "XID" : u == U_XID_RSP ? "XID" : u == U_SNRM ? "SNRM" : u == U_UA ? "UA"
                     : u == U_DISC ? "DISC" : u == U_DM ? "DM" : u == U_UI ? "UI" : u == U_TEST ? "TEST"
                     : u == U_FRMR ? "FRMR" : nullptr;
    Xid x = parseXid(f, n);
    if (x.ok) {
        int len = snprintf(b, sizeof b, "XID %s slot %d/%d %08x>%08x", x.command ? "cmd" : "rsp", x.slot,
                           slotCount(x.flags), x.src, x.dst);
        if (x.infoLen) snprintf(b + len, sizeof b - len, " \"%s\"", nickname(x.info, x.infoLen).c_str());
        return b;
    }
    if (name) snprintf(b, sizeof b, "%s %s %02x%s len=%zu", name, cr, ca, pf ? " P/F" : "", n - 2);
    else snprintf(b, sizeof b, "U?%02x %s %02x len=%zu", c, cr, ca, n - 2);
    return b;
}

// ---- Emulation thread ----

void Irda::fromGuest(uint8_t c) {
    if (c == BOF) { inFrame = true; escaped = false; rxFrame.clear(); bofNs = now; return; }
    if (!inFrame) return;  // extra BOFs (0xFF) and line noise between frames
    if (c == EOF_) {
        inFrame = false;
        if (rxFrame.size() >= 4 && fcs(rxFrame.data(), rxFrame.size()) == 0xF0B8) {
            rxFrame.resize(rxFrame.size() - 2);
            frameFromGuest(rxFrame);
        } else {
            badFrames++;
            if (trace) LOGI("irda: bad frame from guest (%zu bytes)", rxFrame.size());
        }
        return;
    }
    if (c == CE) { escaped = true; return; }
    if (rxFrame.size() > kMaxFrame + 2) { inFrame = false; badFrames++; return; }
    rxFrame.push_back(escaped ? c ^ 0x20 : c);
    escaped = false;
}

void Irda::frameFromGuest(const std::vector<uint8_t> &f) {
    framesOut++;
    if (quiet) { quiet = false; activeFlag = true; event("start"); }
    lastTrafficNs = now;
    if (trace) LOGI("irda %.3f >> %s (%.1f ms on the line)", now / 1e9, describe(f.data(), f.size()).c_str(), (now - bofNs) / 1e6);
    if (f.size() >= 2 && (f[1] & ~0x10) == U_UA && !(f[0] & 1)) uaSinceSnrm = true;  // the guest accepted a connection
    Xid x = parseXid(f.data(), f.size());
    if (x.ok) {
        guestAddr = x.src;
        if (x.command) {
            if (x.slot == 0xFF) {
                round.active = false;  // the final frame: discovery is over
            } else {
                if (!round.active || x.slot == 0 || x.slot < round.slot || x.src != round.src || x.flags != round.flags) {
                    round = Round();
                    round.active = true;
                    round.src = x.src;
                    round.flags = x.flags;
                    round.slots = (uint8_t)slotCount(x.flags);
                }
                round.slot = x.slot;
                round.lastNs = now;
                answerFromCache();
            }
        }
    }
    if (!linkedFlag) { dropped++; return; }
    std::lock_guard<std::mutex> l(mu);
    if (outbox.size() >= kMaxOutbox) { dropped++; return; }
    outbox.push_back(f);
    outCv.notify_one();
}

// Answers the guest's discovery slot for devices it has not heard from this round. The remaining
// devices are spread over the remaining slots, as if each had picked its own.
void Irda::answerFromCache() {
    std::vector<std::vector<uint8_t>> answers;
    {
        std::lock_guard<std::mutex> l(mu);
        std::vector<std::pair<uint32_t, const Peer *>> todo;
        for (auto &p : peerCache)
            if (p.first != guestAddr && !round.answered.count(p.first) && now - p.second.seenNs < kPeerTtlNs)
                todo.push_back({p.first, &p.second});
        if (todo.empty()) return;
        int left = std::max(1, (int)round.slots - (int)round.slot);
        size_t n = (todo.size() + left - 1) / left;
        for (size_t i = 0; i < n && i < todo.size(); i++) {
            const Peer &p = *todo[i].second;
            std::vector<uint8_t> r(14);
            r[0] = 0xFE;                  // broadcast connection address, response
            r[1] = U_XID_RSP | 0x10;      // XID response, F
            r[2] = 0x01;                  // format identifier: discovery
            put32(&r[3], todo[i].first);  // the remote device's address
            put32(&r[7], guestAddr);
            r[11] = round.flags;
            r[12] = round.slot;
            r[13] = p.version;
            r.insert(r.end(), p.info.begin(), p.info.end());
            round.answered.insert(todo[i].first);
            answers.push_back(std::move(r));
        }
    }
    for (auto &r : answers) {
        spoofed++;
        if (trace) LOGI("irda %.3f << %s (cached)", now / 1e9, describe(r.data(), r.size()).c_str());
        deliver(r.data(), r.size());
    }
}

void Irda::deliver(const uint8_t *f, size_t n) {
    if (toGuest.size() > kMaxQueuedBytes) { dropped++; return; }
    std::vector<uint8_t> sir;
    encodeSir(f, n, sir);
    toGuest.insert(toGuest.end(), sir.begin(), sir.end());
}

void Irda::inboundFrame(uint32_t node, const std::vector<uint8_t> &f) {
    framesIn++;
    lastTrafficNs = now;
    // A connection request the guest already accepted, sent again because our UA took longer than
    // the sender's F-timer (500 ms) to get back: on a real IR link the guest would never see both,
    // and answering the copy (DM: "not in that state") tears the new link down. Drop the copy.
    if (f.size() >= 2 && f[0] == 0xFF && (f[1] & ~0x10) == U_SNRM) {
        if (f == lastSnrm && now - lastSnrmNs < kSnrmRepeatNs && uaSinceSnrm) {
            if (trace) LOGI("irda %.3f <- %s (repeat, dropped)", now / 1e9, describe(f.data(), f.size()).c_str());
            return;
        }
        lastSnrm = f;
        lastSnrmNs = now;
        uaSinceSnrm = false;
    }
    Xid x = parseXid(f.data(), f.size());
    if (x.ok) {
        // Remember who answers or announces itself (final discovery command), for later rounds.
        bool learn = !x.command || (x.slot == 0xFF && x.infoLen > 0);
        if (learn && x.src != guestAddr && x.src != 0 && x.src != 0xFFFFFFFF) {
            std::lock_guard<std::mutex> l(mu);
            Peer &p = peerCache[x.src];
            p.node = node;
            p.version = x.version;
            if (x.infoLen) p.info.assign(x.info, x.info + x.infoLen);
            p.seenNs = now;
        }
        if (!x.command) {
            // A late answer to the guest's discovery: deliver it in the current slot, once per round;
            // answers to someone else's discovery, or after the round ended, are dropped.
            bool fresh = round.active && now - round.lastNs < kSecond && x.dst == guestAddr &&
                         !round.answered.count(x.src);
            if (!fresh) {
                if (trace) LOGI("irda %.3f <- %s (dropped)", now / 1e9, describe(f.data(), f.size()).c_str());
                return;
            }
            round.answered.insert(x.src);
            std::vector<uint8_t> r = f;
            r[12] = round.slot;
            if (trace) LOGI("irda %.3f << %s", now / 1e9, describe(r.data(), r.size()).c_str());
            deliver(r.data(), r.size());
            return;
        }
    }
    if (trace) LOGI("irda %.3f << %s", now / 1e9, describe(f.data(), f.size()).c_str());
    deliver(f.data(), f.size());
}

void Irda::service(uint64_t nowNs) {
    now = nowNs;
    if (inboxReady.load(std::memory_order_relaxed)) {
        std::deque<std::pair<uint32_t, std::vector<uint8_t>>> in;
        std::vector<uint32_t> lost;
        {
            std::lock_guard<std::mutex> l(mu);
            in.swap(inbox);
            lost.swap(gone);
            inboxReady = false;
            for (uint32_t node : lost)
                for (auto it = peerCache.begin(); it != peerCache.end();)
                    it = it->second.node == node ? peerCache.erase(it) : std::next(it);
        }
        for (auto &e : in) inboundFrame(e.first, e.second);
    }
    if (!toGuest.empty()) {
        if (!uart.rxEnabled()) {  // nobody has the port open
            if (trace) LOGI("irda %.3f port closed: %zu bytes dropped", now / 1e9, toGuest.size());
            toGuest.clear();
        }
        if (!feedStartNs) feedStartNs = now;
        while (!toGuest.empty() && uart.rxQueued() < kFeedLevel) {
            uart.feed(toGuest.front());
            toGuest.pop_front();
        }
    } else if (feedStartNs && uart.rxQueued() == 0) {
        if (trace) LOGI("irda %.3f guest read it all in %.1f ms", now / 1e9, (now - feedStartNs) / 1e6);
        feedStartNs = 0;
    }
    if (!quiet && now - lastTrafficNs > kIdleNs) {
        quiet = true;
        activeFlag = false;
        event("idle");
    }
}

void Irda::reset() {
    inFrame = escaped = false;
    lastSnrm.clear();
    uaSinceSnrm = false;
    rxFrame.clear();
    toGuest.clear();
    round = Round();
    guestAddr = 0;
}

// ---- Host ----

void Irda::setLinked(bool on) {
    linkedFlag = on;
    if (!on) {
        std::lock_guard<std::mutex> l(mu);
        outbox.clear();
    }
}

bool Irda::takeOutgoing(std::vector<uint8_t> &frame, int timeoutMs) {
    std::unique_lock<std::mutex> l(mu);
    if (outbox.empty() && timeoutMs > 0)
        outCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return !outbox.empty() || woken; });
    woken = false;
    if (outbox.empty()) return false;
    frame.swap(outbox.front());
    outbox.pop_front();
    return true;
}

void Irda::wake() {
    std::lock_guard<std::mutex> l(mu);
    woken = true;
    outCv.notify_all();
}

void Irda::fromPeer(uint32_t node, const uint8_t *f, size_t n) {
    if (n < 2 || n > kMaxFrame || !linkedFlag) return;
    std::lock_guard<std::mutex> l(mu);
    if (inbox.size() > 1024) return;
    inbox.push_back({node, std::vector<uint8_t>(f, f + n)});
    inboxReady = true;
}

void Irda::peerGone(uint32_t node) {
    std::lock_guard<std::mutex> l(mu);
    gone.push_back(node);
    inboxReady = true;
}

void Irda::event(const char *e) {
    LOGI("irda: %s", e);
    std::lock_guard<std::mutex> l(mu);
    if (events.size() < 16) events.push_back(e);
}

std::string Irda::takeEvents() {
    std::lock_guard<std::mutex> l(mu);
    std::string s;
    for (auto &e : events) s += e + "\n";
    events.clear();
    return s;
}

std::string Irda::peers() {
    std::lock_guard<std::mutex> l(mu);
    std::string s;
    char b[16];
    for (auto &p : peerCache) {
        snprintf(b, sizeof b, "%08x ", p.first);
        s += b + nickname(p.second.info.data(), p.second.info.size()) + "\n";
    }
    return s;
}

}  // namespace wm
