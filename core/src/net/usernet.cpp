#include "usernet.h"
#include "localsvc.h"
#include "../util/log.h"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <random>
#include <sys/socket.h>
#include <unistd.h>

namespace wm {

namespace {

const uint8_t kGwMac[6] = {0x52, 0x54, 0x00, 0x12, 0x35, 0x02};
constexpr size_t kBufCap = 256 * 1024;  // per-direction buffering of a TCP connection

uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
uint32_t ip4(const uint8_t *a) { return get32(a); }

constexpr uint32_t kGuest = 0x0A00020F, kGateway = 0x0A000202, kDns = 0x0A000203;
constexpr uint8_t FIN = 1, SYN = 2, RST = 4, PSH = 8, ACK = 16;

// Milliseconds from t to now, 0 if t is later (timestamps taken mid-loop can be).
uint64_t since(uint64_t now, uint64_t t) { return now > t ? now - t : 0; }

uint64_t tcpKey(uint16_t gport, uint32_t dip, uint16_t dport) { return (uint64_t)gport << 48 | (uint64_t)dip << 16 | dport; }

void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

// Where a guest destination really is: the gateway address is the phone itself.
sockaddr_in hostAddr(uint32_t dip, uint16_t dport) {
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(dport);
    a.sin_addr.s_addr = htonl(dip == kGateway ? 0x7F000001 : dip);
    return a;
}

}  // namespace

constexpr uint8_t UserNet::kGuestIp[4];
constexpr uint8_t UserNet::kGatewayIp[4];
constexpr uint8_t UserNet::kDnsIp[4];

struct UserNet::Tcp {
    int fd = -1;
    uint32_t dip = 0;
    uint16_t gport = 0, dport = 0;
    enum { CONNECTING, SYNACK_SENT, OPEN } st = CONNECTING;
    uint32_t rcvNxt = 0;                  // next sequence number expected from the guest
    uint32_t iss = 0, sndUna = 0;         // our side: oldest unacknowledged byte
    size_t sentOff = 0;                   // bytes of `out` already sent
    uint32_t gWnd = 0;
    uint16_t mss = 1460;
    std::vector<uint8_t> toHost;          // guest -> socket, not yet written
    std::deque<uint8_t> out;              // socket -> guest, from sndUna on
    bool hostEof = false, finSent = false, finAcked = false, guestFin = false, shutWr = false;
    uint64_t lastTx = 0, lastActive = 0;
    uint32_t rto = 300;
    int synTries = 0;
    uint32_t seqNow() const { return sndUna + (uint32_t)sentOff + (finSent && !finAcked ? 1 : 0); }
};

struct UserNet::Udp {
    int fd = -1;
    uint32_t dip = 0;
    uint16_t gport = 0, dport = 0;
    uint64_t last = 0;
};

UserNet::UserNet() {}
UserNet::~UserNet() { stop(); }

uint64_t UserNet::nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void UserNet::start() {
    if (running) return;
    int p[2];
    if (pipe(p) != 0) return;
    wakeRd = p[0];
    wakeWr = p[1];
    nonblock(wakeRd);
    nonblock(wakeWr);
    {
        std::lock_guard<std::mutex> lk(dnsBox->mu);
        dnsBox->wakeWr = wakeWr;
        dnsBox->alive = true;
    }
    running = true;
    thread = std::thread([this] { loop(); });
}

void UserNet::stop() {
    if (!running) return;
    running = false;
    char c = 0;
    (void)!write(wakeWr, &c, 1);
    thread.join();
    if (namesDirty) saveNames();
    {
        std::lock_guard<std::mutex> lk(dnsBox->mu);
        dnsBox->alive = false;
        dnsBox->wakeWr = -1;
    }
    for (auto &t : tcps) if (t.second->fd >= 0) close(t.second->fd);
    for (auto &u : udps) if (u.second->fd >= 0) close(u.second->fd);
    tcps.clear();
    udps.clear();
    close(wakeRd);
    close(wakeWr);
    wakeRd = wakeWr = -1;
}

void UserNet::fromGuest(const uint8_t *frame, size_t len) {
    {
        std::lock_guard<std::mutex> lk(inMu);
        if (inbox.size() > 4096) return;
        inbox.emplace_back(frame, frame + len);
    }
    char c = 0;
    (void)!write(wakeWr, &c, 1);
}

bool UserNet::nextForGuest(std::vector<uint8_t> &frame) {
    std::lock_guard<std::mutex> lk(outMu);
    if (outbox.empty()) return false;
    frame = outbox.front();
    return true;
}

void UserNet::popForGuest() {
    std::lock_guard<std::mutex> lk(outMu);
    if (!outbox.empty()) outbox.pop_front();
}

void UserNet::setTlsProxy(std::shared_ptr<TlsProxy> proxy, std::vector<uint16_t> ports) {
    std::lock_guard<std::mutex> lk(cfgMu);
    if (!!tls != !!proxy) LOGI("net: secure web proxy %s", proxy ? "on" : "off");
    if (proxy && !proxy->dir().empty()) namesPath = proxy->dir() + "/names.txt";
    tls = std::move(proxy);
    tlsPorts = std::move(ports);
}

void UserNet::setHostRules(std::vector<HostRule> rules) {
    for (HostRule &r : rules) {
        for (char &ch : r.pattern) ch = (char)tolower((unsigned char)ch);
        LOGI("net: DNS maps %s to %s", r.pattern.c_str(), r.target.c_str());
    }
    std::lock_guard<std::mutex> lk(cfgMu);
    hostRules = std::make_shared<const std::vector<HostRule>>(std::move(rules));
}

// Kept for 6 hours: longer than the guest caches an answer (60 s TTL) or keeps a connection.
static constexpr uint64_t kNameKeepMs = 6 * 3600 * 1000ull;

void UserNet::rememberName(const std::string &name, const std::vector<uint32_t> &addrs) {
    // Only names that can go into a certificate (the guest could ask for anything).
    if (name.empty() || name.size() > 253) return;
    for (char ch : name)
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '.' || ch == '_')) return;
    uint64_t now = nowMs();
    for (uint32_t a : addrs) {
        auto &v = namesByIp[a];
        v.erase(std::remove_if(v.begin(), v.end(), [&](const SeenName &s) { return s.name == name || since(now, s.at) > kNameKeepMs; }), v.end());
        v.insert(v.begin(), {name, now});
        if (v.size() > 16) v.resize(16);
    }
    if (namesByIp.size() > 4096) namesByIp.clear();  // a guest resolving without end: start over
    namesDirty = true;
}

// names.txt: "a.b.c.d name unix-seconds" per line, newest first per address.
void UserNet::loadNames(const std::string &path) {
    FILE *f = fopen(path.c_str(), "r");
    if (!f) return;
    uint64_t now = nowMs();
    long long wall = (long long)time(nullptr);
    char line[512], name[300];
    unsigned a, b, c, d;
    long long t;
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%u.%u.%u.%u %299s %lld", &a, &b, &c, &d, name, &t) != 6 || a > 255 || b > 255 || c > 255 || d > 255) continue;
        long long ageS = wall - t;
        if (ageS < 0 || (uint64_t)ageS * 1000 > kNameKeepMs) continue;
        auto &v = namesByIp[a << 24 | b << 16 | c << 8 | d];
        if (v.size() >= 16 || std::any_of(v.begin(), v.end(), [&](const SeenName &s) { return s.name == name; })) continue;
        v.push_back({name, now > (uint64_t)ageS * 1000 ? now - (uint64_t)ageS * 1000 : 0});
        n++;
    }
    fclose(f);
    if (n) LOGI("net: %d remembered DNS names", n);
}

void UserNet::saveNames() {
    if (namesLoadedPath.empty()) return;
    std::string tmp = namesLoadedPath + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    FILE *f = fd >= 0 ? fdopen(fd, "w") : nullptr;
    if (!f) { if (fd >= 0) close(fd); return; }
    uint64_t now = nowMs();
    long long wall = (long long)time(nullptr);
    for (auto &kv : namesByIp)
        for (auto &s : kv.second)
            if (since(now, s.at) <= kNameKeepMs)
                fprintf(f, "%u.%u.%u.%u %s %lld\n", kv.first >> 24, (kv.first >> 16) & 255, (kv.first >> 8) & 255, kv.first & 255,
                        s.name.c_str(), wall - (long long)(since(now, s.at) / 1000));
    fclose(f);
    rename(tmp.c_str(), namesLoadedPath.c_str());
    namesDirty = false;
    namesSavedAt = now;
}

std::vector<std::string> UserNet::namesFor(uint32_t ip) {
    std::vector<std::string> r;
    auto it = namesByIp.find(ip);
    if (it == namesByIp.end()) return r;
    uint64_t now = nowMs();
    for (auto &s : it->second)
        if (since(now, s.at) <= kNameKeepMs) r.push_back(s.name);
    return r;
}

void UserNet::pushToGuest(std::vector<uint8_t> f) {
    std::lock_guard<std::mutex> lk(outMu);
    if (outbox.size() < 4096) outbox.push_back(std::move(f));
}

uint16_t UserNet::checksum(const uint8_t *p, size_t n, uint32_t sum) {
    for (size_t i = 0; i + 1 < n; i += 2) sum += (uint32_t)(p[i] << 8 | p[i + 1]);
    if (n & 1) sum += (uint32_t)p[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (uint16_t)~sum;
}

// ---------------------------------------------------------------------------------------------
// Frames to the guest

void UserNet::sendIp(uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *payload, size_t n) {
    if (!haveGuestMac) return;
    std::vector<uint8_t> f(14 + 20 + n);
    memcpy(&f[0], guestMac, 6);
    memcpy(&f[6], kGwMac, 6);
    put16(&f[12], 0x0800);
    uint8_t *ip = &f[14];
    ip[0] = 0x45;
    put16(ip + 2, (uint16_t)(20 + n));
    put16(ip + 4, ipId++);
    put16(ip + 6, 0x4000);  // don't fragment
    ip[8] = 64;
    ip[9] = proto;
    put32(ip + 12, src);
    put32(ip + 16, dst);
    put16(ip + 10, checksum(ip, 20));
    memcpy(ip + 20, payload, n);
    pushToGuest(std::move(f));
}

static uint32_t pseudoSum(uint32_t src, uint32_t dst, uint8_t proto, size_t len) {
    return (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF) + proto + (uint32_t)len;
}

void UserNet::sendUdp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *p, size_t n) {
    std::vector<uint8_t> u(8 + n);
    put16(&u[0], sport);
    put16(&u[2], dport);
    put16(&u[4], (uint16_t)(8 + n));
    memcpy(&u[8], p, n);
    uint16_t c = checksum(u.data(), u.size(), pseudoSum(src, dst, 17, u.size()));
    put16(&u[6], c ? c : 0xFFFF);
    sendIp(17, src, dst, u.data(), u.size());
}

void UserNet::sendTcp(Tcp &c, uint8_t flags, uint32_t seq, const uint8_t *data, size_t n, bool mssOption) {
    size_t hl = mssOption ? 24 : 20;
    std::vector<uint8_t> t(hl + n);
    put16(&t[0], c.dport);
    put16(&t[2], c.gport);
    put32(&t[4], seq);
    put32(&t[8], (flags & ACK) ? c.rcvNxt : 0);
    t[12] = (uint8_t)((hl / 4) << 4);
    t[13] = flags;
    size_t room = kBufCap > c.toHost.size() ? kBufCap - c.toHost.size() : 0;
    put16(&t[14], (uint16_t)(room > 65535 ? 65535 : room));
    if (mssOption) { t[20] = 2; t[21] = 4; put16(&t[22], 1460); }
    if (n) memcpy(&t[hl], data, n);
    put16(&t[16], checksum(t.data(), t.size(), pseudoSum(c.dip, kGuest, 6, t.size())));
    sendIp(6, c.dip, kGuest, t.data(), t.size());
}

void UserNet::sendRst(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, uint32_t seq, uint32_t ack) {
    uint8_t t[20] = {};
    put16(t, sport);
    put16(t + 2, dport);
    put32(t + 4, seq);
    put32(t + 8, ack);
    t[12] = 5 << 4;
    t[13] = RST | ACK;
    put16(t + 16, checksum(t, 20, pseudoSum(src, dst, 6, 20)));
    sendIp(6, src, dst, t, 20);
}

// ---------------------------------------------------------------------------------------------
// Frames from the guest

void UserNet::handleFrame(const std::vector<uint8_t> &f) {
    if (f.size() < 14) return;
    memcpy(guestMac, &f[6], 6);
    haveGuestMac = true;
    uint16_t type = get16(&f[12]);
    if (type == 0x0806) handleArp(&f[14], f.size() - 14);
    else if (type == 0x0800) handleIp(&f[14], f.size() - 14);
}

void UserNet::handleArp(const uint8_t *p, size_t n) {
    if (n < 28 || get16(p + 6) != 1) return;  // requests only
    uint32_t target = ip4(p + 24);
    if ((target & 0xFFFFFF00) != 0x0A000200 || target == kGuest) return;
    uint8_t r[28];
    put16(r, 1);
    put16(r + 2, 0x0800);
    r[4] = 6;
    r[5] = 4;
    put16(r + 6, 2);
    memcpy(r + 8, kGwMac, 6);
    memcpy(r + 14, p + 24, 4);
    memcpy(r + 18, p + 8, 10);  // sender MAC + IP become the target
    std::vector<uint8_t> f(14 + 28);
    memcpy(&f[0], p + 8, 6);
    memcpy(&f[6], kGwMac, 6);
    put16(&f[12], 0x0806);
    memcpy(&f[14], r, 28);
    pushToGuest(std::move(f));
}

void UserNet::handleIp(const uint8_t *p, size_t n) {
    if (n < 20 || (p[0] >> 4) != 4) return;
    size_t hl = (p[0] & 15) * 4, total = get16(p + 2);
    if (total > n || total < hl) return;
    if (get16(p + 6) & 0x3FFF) return;  // fragments are not supported
    const uint8_t *payload = p + hl;
    size_t len = total - hl;
    switch (p[9]) {
        case 1: handleIcmp(p, payload, len); break;
        case 6: handleTcp(p, payload, len); break;
        case 17: handleUdp(p, payload, len); break;
    }
}

void UserNet::handleIcmp(const uint8_t *ip, const uint8_t *p, size_t n) {
    uint32_t dst = ip4(ip + 16);
    if (n < 8 || p[0] != 8 || (dst != kGateway && dst != kDns)) return;  // echo to our own addresses
    std::vector<uint8_t> r(p, p + n);
    r[0] = 0;
    r[2] = r[3] = 0;
    put16(&r[2], checksum(r.data(), r.size()));
    sendIp(1, dst, ip4(ip + 12), r.data(), r.size());
}

void UserNet::handleUdp(const uint8_t *ip, const uint8_t *p, size_t n) {
    if (n < 8) return;
    uint16_t sport = get16(p), dport = get16(p + 2);
    uint32_t dst = ip4(ip + 16);
    const uint8_t *data = p + 8;
    size_t len = std::min<size_t>(get16(p + 4), n) - 8;
    if (dport == 67) { dhcp(data, len); return; }
    if (dst == kDns && dport == 53) { dns(sport, data, len); return; }
    if ((dst & 0xFF) == 0xFF || (dst >> 28) == 0xE || ((dst & 0xFFFFFF00) == 0x0A000200 && dst != kGateway)) return;
    uint64_t key = tcpKey(sport, dst, dport);
    auto it = udps.find(key);
    if (it == udps.end()) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return;
        nonblock(fd);
        sockaddr_in a = hostAddr(dst, dport);
        if (connect(fd, (sockaddr *)&a, sizeof a) != 0) { close(fd); return; }
        auto u = std::make_unique<Udp>();
        u->fd = fd;
        u->dip = dst;
        u->gport = sport;
        u->dport = dport;
        it = udps.emplace(key, std::move(u)).first;
    }
    it->second->last = nowMs();
    (void)!send(it->second->fd, data, len, 0);
}

void UserNet::dhcp(const uint8_t *p, size_t n) {
    if (n < 240 || p[0] != 1 || get32(p + 236) != 0x63825363) return;
    int type = 0;
    for (size_t i = 240; i < n && p[i] != 255;) {
        if (p[i] == 0) { i++; continue; }
        if (i + 1 >= n) break;
        if (p[i] == 53 && p[i + 1] >= 1) type = p[i + 2];
        i += 2 + p[i + 1];
    }
    int reply = type == 1 ? 2 : type == 3 ? 5 : 0;  // DISCOVER -> OFFER, REQUEST -> ACK
    if (!reply) return;
    std::vector<uint8_t> r(300, 0);
    r[0] = 2;
    r[1] = 1;
    r[2] = 6;
    memcpy(&r[4], p + 4, 4);    // xid
    memcpy(&r[10], p + 10, 2);  // flags
    memcpy(&r[16], kGuestIp, 4);
    memcpy(&r[20], kGatewayIp, 4);
    memcpy(&r[28], p + 28, 16);  // chaddr
    put32(&r[236], 0x63825363);
    size_t o = 240;
    auto opt = [&](uint8_t code, std::initializer_list<uint8_t> v) {
        r[o++] = code;
        r[o++] = (uint8_t)v.size();
        for (uint8_t b : v) r[o++] = b;
    };
    opt(53, {(uint8_t)reply});
    opt(54, {10, 0, 2, 2});
    opt(51, {0, 1, 0x51, 0x80});  // 1 day
    opt(1, {255, 255, 255, 0});
    opt(3, {10, 0, 2, 2});
    opt(6, {10, 0, 2, 3});
    opt(26, {0x05, 0xDC});        // MTU 1500
    r[o++] = 255;
    sendUdp(kGateway, 67, 0xFFFFFFFF, 68, r.data(), r.size());
}

// DNS: A queries go through the phone's resolver (which knows the current network's servers and
// private DNS); other types get an empty answer so the guest falls back to A. Host rules
// (setHostRules) answer a name with a fixed address or another name's addresses: the hook for
// revived services such as Escargot (docs/research-tls-proxy.md). Answers are remembered per
// address for the secure web proxy (rememberName).
void UserNet::dns(uint16_t guestPort, const uint8_t *p, size_t n) {
    if (n < 17) return;
    std::vector<uint8_t> q(p, p + n);
    std::shared_ptr<DnsBox> box = dnsBox;
    std::shared_ptr<const std::vector<HostRule>> rules;
    {
        std::lock_guard<std::mutex> lk(cfgMu);
        rules = hostRules;
    }
    std::thread([box, guestPort, q, rules] {
        std::string name;
        size_t i = 12;
        while (i < q.size() && q[i]) {
            size_t l = q[i];
            if ((l & 0xC0) || i + 1 + l > q.size()) return;
            if (!name.empty()) name += '.';
            name.append((const char *)&q[i + 1], l);
            i += 1 + l;
        }
        if (i + 5 > q.size()) return;
        size_t qend = i + 5;
        uint16_t qtype = get16(&q[i + 1]);
        for (char &ch : name) ch = (char)tolower((unsigned char)ch);
        std::vector<uint32_t> addrs;
        bool nx = false;
        const HostRule *rule = findHostRule(rules.get(), name);
        in_addr fixed{};
        if (qtype == 1 && rule && inet_pton(AF_INET, rule->target.c_str(), &fixed) == 1) {
            addrs.push_back(ntohl(fixed.s_addr));
        } else if (qtype == 1) {
            addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            int rc = getaddrinfo((rule ? rule->target : name).c_str(), nullptr, &hints, &res);
            if (rc == 0) {
                for (addrinfo *a = res; a && addrs.size() < 8; a = a->ai_next) {
                    uint32_t v = ntohl(((sockaddr_in *)a->ai_addr)->sin_addr.s_addr);
                    if (std::find(addrs.begin(), addrs.end(), v) == addrs.end()) addrs.push_back(v);
                }
                freeaddrinfo(res);
            } else if (rc == EAI_NONAME) {
                nx = true;
            }
        }
        if (g_logLevel <= LOG_DEBUG && qtype == 1) {  // --log debug: which names the guest's programs look up
            std::string list;
            for (uint32_t a : addrs) list += " " + std::to_string(a >> 24) + "." + std::to_string(a >> 16 & 255) + "." + std::to_string(a >> 8 & 255) + "." + std::to_string(a & 255);
            LOGD("net: DNS %s ->%s%s%s", name.c_str(), list.empty() ? (nx ? " NXDOMAIN" : " (none)") : list.c_str(), rule ? " via rule " : "", rule ? rule->target.c_str() : "");
        }
        std::vector<uint8_t> r(q.begin(), q.begin() + qend);
        r[2] = 0x81 | (q[2] & 0x01);   // response, recursion desired as asked
        r[3] = (uint8_t)(0x80 | (nx ? 3 : 0));  // recursion available, NXDOMAIN if unknown
        put16(&r[4], 1);
        put16(&r[6], (uint16_t)addrs.size());
        put16(&r[8], 0);
        put16(&r[10], 0);
        for (uint32_t a : addrs) {
            uint8_t rr[16] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
            put32(rr + 12, a);
            r.insert(r.end(), rr, rr + 16);
        }
        std::lock_guard<std::mutex> lk(box->mu);
        if (!box->alive) return;
        box->done.push_back({guestPort, std::move(r), name, addrs});
        char c = 0;
        (void)!write(box->wakeWr, &c, 1);
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// TCP

void UserNet::handleTcp(const uint8_t *ip, const uint8_t *p, size_t n) {
    if (n < 20) return;
    uint16_t sport = get16(p), dport = get16(p + 2);
    uint32_t seq = get32(p + 4), ack = get32(p + 8);
    size_t hl = (p[12] >> 4) * 4;
    if (hl < 20 || hl > n) return;
    uint8_t flags = p[13];
    uint16_t win = get16(p + 14);
    const uint8_t *data = p + hl;
    size_t len = n - hl;
    uint32_t dst = ip4(ip + 16);
    uint64_t key = tcpKey(sport, dst, dport);
    auto it = tcps.find(key);

    if (flags & RST) {
        if (it != tcps.end()) closeTcp(key);
        return;
    }
    if ((flags & SYN) && !(flags & ACK)) {
        if (it != tcps.end()) {
            if (it->second->st == Tcp::SYNACK_SENT) sendTcp(*it->second, SYN | ACK, it->second->iss, nullptr, 0, true);
            return;
        }
        // 10.0.2.4: a service inside PocketStylus (localsvc.h), when one is installed.
        std::shared_ptr<LocalService> local = dst == kLocalServiceIp ? localService() : nullptr;
        if ((dst & 0xFFFFFF00) == 0x0A000200 && dst != kGateway && !local) {
            LOGD("net: TCP to 10.0.2.%u:%u refused (no such host)", dst & 255, dport);
            sendRst(dst, dport, kGuest, sport, 0, seq + 1);
            return;
        }
        int fd = -1;
        std::shared_ptr<TlsProxy> proxy;
        TlsProxy::Conn pc;
        {
            std::lock_guard<std::mutex> lk(cfgMu);
            if (tls && dst != kGateway && std::find(tlsPorts.begin(), tlsPorts.end(), dport) != tlsPorts.end()) {
                proxy = tls;
                pc.rules = hostRules;
            }
        }
        LOGD("net: TCP to %u.%u.%u.%u:%u%s", dst >> 24, dst >> 16 & 255, dst >> 8 & 255, dst & 255, dport,
             proxy ? " (secure web proxy)" : local ? " (local service)" : "");
        int sv[2];
        if (proxy && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            // The secure web proxy takes the other end; this connection relays to it like to a server.
            fd = sv[0];
            nonblock(fd);
            pc.fd = sv[1];
            pc.ip = dst;
            pc.port = dport;
            pc.names = namesFor(dst);
            proxy->serve(std::move(pc));
        } else if (local && socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            // Plain TCP to a local service: it takes the other end, like a server would.
            fd = sv[0];
            nonblock(fd);
            std::vector<std::string> names = namesFor(dst);
            local->serve(sv[1], dport, names.empty() ? std::string() : names[0], false);
        } else {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) return;
            nonblock(fd);
            sockaddr_in a = hostAddr(dst, dport);
            int rc = connect(fd, (sockaddr *)&a, sizeof a);
            if (rc != 0 && errno != EINPROGRESS) {
                LOGI("net: TCP connect error %d", errno);
                close(fd);
                sendRst(dst, dport, kGuest, sport, 0, seq + 1);
                return;
            }
        }
        auto c = std::make_unique<Tcp>();
        c->fd = fd;
        c->dip = dst;
        c->gport = sport;
        c->dport = dport;
        c->rcvNxt = seq + 1;
        static std::mt19937 rng{std::random_device{}()};
        c->iss = rng();
        c->sndUna = c->iss;
        c->gWnd = win;
        c->lastActive = nowMs();
        for (size_t i = 20; i + 1 < hl;) {  // MSS option
            uint8_t k = p[i];
            if (k == 0) break;
            if (k == 1) { i++; continue; }
            if (k == 2 && i + 3 < hl) c->mss = std::min<uint16_t>(1460, get16(p + i + 2));
            i += p[i + 1] ? p[i + 1] : 1;
        }
        tcps.emplace(key, std::move(c));
        return;  // the SYN-ACK goes out once the real connection is up
    }
    if (it == tcps.end()) {
        if (!(flags & RST)) sendRst(dst, dport, kGuest, sport, ack, seq + (uint32_t)len + ((flags & (SYN | FIN)) ? 1 : 0));
        return;
    }
    Tcp &c = *it->second;
    c.lastActive = nowMs();
    c.gWnd = win;
    if (c.st == Tcp::SYNACK_SENT && (flags & ACK) && ack == c.iss + 1) {
        c.st = Tcp::OPEN;
        c.sndUna = c.iss + 1;
        c.sentOff = 0;
        c.rto = 300;
    }
    if (c.st != Tcp::OPEN) return;
    if (flags & ACK) {
        int32_t acked = (int32_t)(ack - c.sndUna);
        int32_t limit = (int32_t)c.sentOff + (c.finSent && !c.finAcked ? 1 : 0);
        if (acked > 0 && acked <= limit) {
            size_t dataAcked = std::min<size_t>((size_t)acked, c.sentOff);
            c.out.erase(c.out.begin(), c.out.begin() + (long)dataAcked);
            c.sentOff -= dataAcked;
            c.sndUna += (uint32_t)dataAcked;
            if ((size_t)acked > dataAcked && c.finSent) { c.finAcked = true; c.sndUna += 1; }
            c.rto = 300;
            c.lastTx = nowMs();
        }
    }
    bool needAck = false;
    if (len) {
        if (seq == c.rcvNxt && c.toHost.size() + len <= kBufCap) {
            c.toHost.insert(c.toHost.end(), data, data + len);
            c.rcvNxt += (uint32_t)len;
        }
        needAck = true;  // in order: acknowledge; otherwise a duplicate ACK asks for the gap
    }
    if ((flags & FIN) && seq + (uint32_t)len == c.rcvNxt && !c.guestFin) {
        c.rcvNxt += 1;
        c.guestFin = true;
        needAck = true;
    }
    if (needAck) sendTcp(c, ACK, c.seqNow(), nullptr, 0);
    tcpPump(c);
    if (c.guestFin && c.finAcked && c.toHost.empty()) closeTcp(key);
}

void UserNet::tcpPump(Tcp &c) {
    if (c.st != Tcp::OPEN) return;
    uint32_t wnd = c.gWnd;
    while (c.sentOff < c.out.size() && c.sentOff < wnd) {
        size_t n = std::min<size_t>({(size_t)c.mss, c.out.size() - c.sentOff, wnd - c.sentOff});
        uint8_t buf[1460];
        std::copy(c.out.begin() + (long)c.sentOff, c.out.begin() + (long)(c.sentOff + n), buf);
        sendTcp(c, ACK | PSH, c.sndUna + (uint32_t)c.sentOff, buf, n);
        c.sentOff += n;
        c.lastTx = nowMs();
    }
    if (c.hostEof && !c.finSent && c.sentOff == c.out.size()) {
        sendTcp(c, FIN | ACK, c.sndUna + (uint32_t)c.sentOff, nullptr, 0);
        c.finSent = true;
        c.lastTx = nowMs();
    }
    if (c.guestFin && c.toHost.empty() && !c.shutWr) {
        shutdown(c.fd, SHUT_WR);
        c.shutWr = true;
    }
}

void UserNet::tcpTimers(uint64_t now) {
    std::vector<uint64_t> dead;
    for (auto &kv : tcps) {
        Tcp &c = *kv.second;
        if (c.st == Tcp::SYNACK_SENT && since(now, c.lastTx) > 1000) {
            if (++c.synTries > 5) { dead.push_back(kv.first); continue; }
            sendTcp(c, SYN | ACK, c.iss, nullptr, 0, true);
            c.lastTx = now;
        }
        if (c.st == Tcp::OPEN && (c.sentOff > 0 || (c.finSent && !c.finAcked)) && since(now, c.lastTx) > c.rto) {
            c.sentOff = 0;  // go back and resend everything unacknowledged
            c.finSent = c.finSent && c.finAcked;
            c.rto = std::min<uint32_t>(c.rto * 2, 8000);
            tcpPump(c);
        }
        bool idle = since(now, c.lastActive) > 600000;
        bool done = c.hostEof && c.finAcked && c.guestFin && c.toHost.empty();
        if (idle || done) dead.push_back(kv.first);
    }
    for (uint64_t k : dead) closeTcp(k);
}

void UserNet::closeTcp(uint64_t key) {
    auto it = tcps.find(key);
    if (it == tcps.end()) return;
    if (it->second->fd >= 0) close(it->second->fd);
    tcps.erase(it);
}

// ---------------------------------------------------------------------------------------------
// Socket loop

void UserNet::loop() {
    std::vector<pollfd> fds;
    std::vector<uint64_t> keys;   // tcp key per fd (0 for others)
    std::vector<char> kinds;      // 'w' wake, 't' tcp, 'u' udp
    uint8_t buf[65536];
    while (running) {
        fds.clear();
        keys.clear();
        kinds.clear();
        fds.push_back({wakeRd, POLLIN, 0});
        keys.push_back(0);
        kinds.push_back('w');
        for (auto &kv : tcps) {
            Tcp &c = *kv.second;
            short ev = 0;
            if (c.st == Tcp::CONNECTING) ev = POLLOUT;
            else {
                if (!c.hostEof && c.out.size() < kBufCap && c.st == Tcp::OPEN) ev |= POLLIN;
                if (!c.toHost.empty()) ev |= POLLOUT;
            }
            fds.push_back({c.fd, ev, 0});
            keys.push_back(kv.first);
            kinds.push_back('t');
        }
        for (auto &kv : udps) {
            fds.push_back({kv.second->fd, POLLIN, 0});
            keys.push_back(kv.first);
            kinds.push_back('u');
        }
        poll(fds.data(), fds.size(), 50);
        uint64_t now = nowMs();

        if (fds[0].revents) while (read(wakeRd, buf, sizeof buf) > 0) {}
        std::deque<std::vector<uint8_t>> in;
        {
            std::lock_guard<std::mutex> lk(inMu);
            in.swap(inbox);
        }
        for (auto &f : in) handleFrame(f);
        std::vector<DnsDone> done;
        {
            std::lock_guard<std::mutex> lk(dnsBox->mu);
            done.swap(dnsBox->done);
        }
        for (auto &d : done) {
            if (!d.addrs.empty()) rememberName(d.name, d.addrs);
            sendUdp(kDns, 53, kGuest, d.guestPort, d.reply.data(), d.reply.size());
        }

        for (size_t i = 1; i < fds.size(); i++) {
            if (!fds[i].revents) continue;
            if (kinds[i] == 'u') {
                auto it = udps.find(keys[i]);
                if (it == udps.end()) continue;
                ssize_t r = recv(it->second->fd, buf, sizeof buf, 0);
                if (r > 0) {
                    it->second->last = now;
                    sendUdp(it->second->dip, it->second->dport, kGuest, it->second->gport, buf, (size_t)r);
                }
                continue;
            }
            auto it = tcps.find(keys[i]);
            if (it == tcps.end()) continue;
            Tcp &c = *it->second;
            if (c.st == Tcp::CONNECTING) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(c.fd, SOL_SOCKET, SO_ERROR, &err, &el);
                if (err) {
                    sendRst(c.dip, c.dport, kGuest, c.gport, 0, c.rcvNxt);
                    closeTcp(keys[i]);
                    continue;
                }
                c.st = Tcp::SYNACK_SENT;
                sendTcp(c, SYN | ACK, c.iss, nullptr, 0, true);
                c.lastTx = now;
                continue;
            }
            if (fds[i].revents & POLLOUT && !c.toHost.empty()) {
                ssize_t w = send(c.fd, c.toHost.data(), c.toHost.size(), MSG_NOSIGNAL);
                if (w > 0) {
                    bool wasFull = c.toHost.size() >= kBufCap / 2;
                    c.toHost.erase(c.toHost.begin(), c.toHost.begin() + w);
                    if (wasFull) sendTcp(c, ACK, c.seqNow(), nullptr, 0);  // window update
                }
            }
            if (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) {
                size_t room = kBufCap - c.out.size();
                ssize_t r = recv(c.fd, buf, std::min(room, sizeof buf), 0);
                if (r > 0) c.out.insert(c.out.end(), buf, buf + r);
                else if (r == 0) c.hostEof = true;
                else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    sendRst(c.dip, c.dport, kGuest, c.gport, c.seqNow(), c.rcvNxt);
                    closeTcp(keys[i]);
                    continue;
                }
            }
            tcpPump(c);
        }
        // Timers run on a fresh clock: connections created above have later timestamps than `now`.
        now = nowMs();
        tcpTimers(now);
        std::string np;
        {
            std::lock_guard<std::mutex> lk(cfgMu);
            np = namesPath;
        }
        if (np != namesLoadedPath) {
            if (namesDirty) saveNames();
            namesLoadedPath = np;
            loadNames(np);
        }
        if (namesDirty && since(now, namesSavedAt) > 30000) saveNames();
        for (auto it = udps.begin(); it != udps.end();) {
            if (now > it->second->last && now - it->second->last > 120000) {
                close(it->second->fd);
                it = udps.erase(it);
            } else {
                ++it;
            }
        }
    }
}

}  // namespace wm
