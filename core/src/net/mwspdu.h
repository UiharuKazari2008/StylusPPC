// Framing of Windows Live for Windows Mobile's push channel (tcp.mws.mobile.live.com:50001), as
// WLMService.dll 10.6 parses and builds it (facts from its code; docs/research-escargot.md §9). The
// layout is the MSP PDU of Pocket MSN's "MSPPDUGateway": a fixed header, a chain of options like IPv6
// extension headers, then the payload.
//
//   header (12 bytes)  [0] 0x30 version   [1] type   [2..3] message id (LE)   [4..7] sequence (LE)
//                      [8..9] length of everything after the header (LE, at most 0x4000)
//                      [10] flags (bit 0: more fragments follow)   [11] type of the first option (0: none)
//   option             [type of the next option, 0 = last][data length][data]
//
// Types: client 0x00 data ack (option 4), 0x01 connect, 0x02 reconnect (option 2 = the session),
// 0x03 disconnect/close, 0x04 heartbeat (option 5 = TTL in seconds), 0x10 data (option 0x10;
// fragments by message id). Server: 0x10 data, 0x81/0x82 connect answer (option 2 = session,
// optional 3 = device id, 6 = string, 8, 9), 0x83, 0x84 heartbeat answer (option 5 = next TTL),
// 0xff error (option 0xfe: u16 code).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace wm {

struct MwsPdu {
    uint8_t type = 0;
    uint16_t id = 0;
    uint32_t seq = 0;
    uint8_t flags = 0;
    std::vector<std::pair<uint8_t, std::string>> opts;  // in order
    std::string payload;

    const std::string *opt(uint8_t t) const {
        for (auto &o : opts)
            if (o.first == t) return &o.second;
        return nullptr;
    }
};

constexpr size_t kMwsPduHeader = 12;
constexpr size_t kMwsPduMaxBody = 0x4000;

/** Bytes of one PDU parsed from the start of `b`; 0 when more bytes are needed, -1 when it isn't one. */
inline long parseMwsPdu(const std::string &b, MwsPdu &p) {
    if (b.size() < kMwsPduHeader) return 0;
    const uint8_t *h = (const uint8_t *)b.data();
    if (h[0] != 0x30) return -1;
    size_t len = (size_t)h[8] | (size_t)h[9] << 8;
    if (len > kMwsPduMaxBody) return -1;
    if (b.size() < kMwsPduHeader + len) return 0;
    p = MwsPdu();
    p.type = h[1];
    p.id = (uint16_t)(h[2] | h[3] << 8);
    p.seq = (uint32_t)h[4] | (uint32_t)h[5] << 8 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 24;
    p.flags = h[10];
    uint8_t t = h[11];
    size_t i = kMwsPduHeader, end = kMwsPduHeader + len;
    while (t) {
        if (i + 2 > end) return -1;
        uint8_t next = h[i], n = h[i + 1];
        if (i + 2 + n > end) return -1;
        p.opts.push_back({t, b.substr(i + 2, n)});
        i += 2 + n;
        t = next;
    }
    p.payload = b.substr(i, end - i);
    return (long)end;
}

inline std::string buildMwsPdu(const MwsPdu &p) {
    std::string body;
    for (size_t k = 0; k < p.opts.size(); k++) {
        body += (char)(k + 1 < p.opts.size() ? p.opts[k + 1].first : 0);
        body += (char)p.opts[k].second.size();
        body += p.opts[k].second;
    }
    body += p.payload;
    std::string h(kMwsPduHeader, '\0');
    h[0] = 0x30;
    h[1] = (char)p.type;
    h[2] = (char)(p.id & 255);
    h[3] = (char)(p.id >> 8);
    for (int k = 0; k < 4; k++) h[4 + k] = (char)(p.seq >> (8 * k));
    h[8] = (char)(body.size() & 255);
    h[9] = (char)(body.size() >> 8);
    h[10] = (char)p.flags;
    h[11] = (char)(p.opts.empty() ? 0 : p.opts[0].first);
    return h + body;
}

/** One line for logs: type, id, seq, flags, options in hex (payload size only). */
inline std::string describeMwsPdu(const MwsPdu &p) {
    char b[96];
    snprintf(b, sizeof b, "type 0x%02x id %u seq %u flags %u", p.type, p.id, p.seq, p.flags);
    std::string s = b;
    for (auto &o : p.opts) {
        snprintf(b, sizeof b, " opt%u[", o.first);
        s += b;
        for (size_t k = 0; k < o.second.size() && k < 24; k++) {
            snprintf(b, sizeof b, "%s%02x", k ? " " : "", (uint8_t)o.second[k]);
            s += b;
        }
        if (o.second.size() > 24) s += " ...";
        s += "]";
    }
    if (!p.payload.empty()) s += " payload " + std::to_string(p.payload.size()) + " bytes";
    return s;
}

}  // namespace wm
