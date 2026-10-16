// Infrared transports for the headless runner (irda_link.h).
#include "irda_link.h"
#include "../src/board/irda.h"
#include "../src/util/log.h"
#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <cstring>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <random>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace wm {

namespace {

// ---- UDP pipe: one datagram per frame, peer node 1 ----
class UdpLink : public IrdaLink {
public:
    UdpLink(int l, int r, int d) : localPort(l), remotePort(r), delayMs(d) {}
    ~UdpLink() override { stop(); }
    bool start(Irda &ir, std::string &err) override {
        irda = &ir;
        fd = socket(AF_INET, SOCK_DGRAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)localPort);
        if (fd < 0 || bind(fd, (sockaddr *)&a, sizeof a) < 0) { err = "udp bind failed"; return false; }
        timeval tv{0, 200000};
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        running = true;
        irda->setLinked(true);
        rx = std::thread([this] {
            uint8_t buf[8192];
            while (running) {
                ssize_t n = recv(fd, buf, sizeof buf, 0);
                if (n > 0) irda->fromPeer(1, buf, (size_t)n);
            }
        });
        tx = std::thread([this] {
            sockaddr_in to{};
            to.sin_family = AF_INET;
            to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            to.sin_port = htons((uint16_t)remotePort);
            std::vector<uint8_t> f;
            // --irda-delay: each frame leaves delayMs late (a slow network), order kept.
            std::deque<std::pair<std::chrono::steady_clock::time_point, std::vector<uint8_t>>> late;
            while (running) {
                bool got = irda->takeOutgoing(f, late.empty() ? 200 : 1);
                auto now = std::chrono::steady_clock::now();
                if (got) {
                    if (delayMs <= 0) sendto(fd, f.data(), f.size(), 0, (sockaddr *)&to, sizeof to);
                    else late.push_back({now + std::chrono::milliseconds(delayMs), f});
                }
                while (!late.empty() && late.front().first <= now) {
                    sendto(fd, late.front().second.data(), late.front().second.size(), 0, (sockaddr *)&to, sizeof to);
                    late.pop_front();
                }
            }
        });
        LOGI("irda: udp link %d -> %d%s", localPort, remotePort, delayMs > 0 ? " (delayed)" : "");
        return true;
    }
    void stop() override {
        if (!running.exchange(false)) return;
        irda->setLinked(false);
        irda->wake();
        if (rx.joinable()) rx.join();
        if (tx.joinable()) tx.join();
        close(fd);
    }

private:
    int localPort, remotePort, delayMs, fd = -1;
    Irda *irda = nullptr;
    std::atomic<bool> running{false};
    std::thread rx, tx;
};

// ---- InfraWAN: WebSocket client (RFC 6455, client frames masked) ----
std::string base64(const uint8_t *p, size_t n) {
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = p[i] << 16 | (i + 1 < n ? p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        s += t[v >> 18 & 63];
        s += t[v >> 12 & 63];
        s += i + 1 < n ? t[v >> 6 & 63] : '=';
        s += i + 2 < n ? t[v & 63] : '=';
    }
    return s;
}

std::string jsonEscape(const std::string &v) {
    std::string s;
    for (char c : v) {
        if (c == '"' || c == '\\') s += '\\';
        if ((unsigned char)c >= 0x20) s += c;
    }
    return s;
}

std::string urlDecode(const std::string &v) {
    std::string s;
    for (size_t i = 0; i < v.size(); i++) {
        if (v[i] == '%' && i + 2 < v.size()) { s += (char)strtol(v.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
        else s += v[i] == '+' ? ' ' : v[i];
    }
    return s;
}

// Number after "key": in a flat JSON object (enough for the relay's own messages).
long jsonNumber(const std::string &j, const char *key) {
    std::string k = std::string("\"") + key + "\":";
    size_t p = j.find(k);
    return p == std::string::npos ? -1 : strtol(j.c_str() + p + k.size(), nullptr, 10);
}

class WanLink : public IrdaLink {
public:
    explicit WanLink(const std::string &u) : url(u) {}
    ~WanLink() override { stop(); }

    bool start(Irda &ir, std::string &err) override {
        irda = &ir;
        // ws://host:port/path?room=..&user=..&token=..&name=..
        if (url.compare(0, 5, "ws://") != 0) { err = "only ws:// URLs (use a TLS proxy for wss)"; return false; }
        std::string rest = url.substr(5), query;
        size_t q = rest.find('?');
        if (q != std::string::npos) { query = rest.substr(q + 1); rest = rest.substr(0, q); }
        size_t slash = rest.find('/');
        std::string hostport = rest.substr(0, slash), path = slash == std::string::npos ? "/" : rest.substr(slash);
        size_t colon = hostport.rfind(':');
        host = hostport.substr(0, colon);
        std::string port = colon == std::string::npos ? "80" : hostport.substr(colon + 1);
        for (size_t p = 0; p < query.size();) {
            size_t e = query.find('&', p);
            std::string kv = query.substr(p, e == std::string::npos ? std::string::npos : e - p);
            p = e == std::string::npos ? query.size() : e + 1;
            size_t eq = kv.find('=');
            if (eq == std::string::npos) continue;
            std::string k = kv.substr(0, eq), v = urlDecode(kv.substr(eq + 1));
            if (k == "room") room = v;
            else if (k == "user") user = v;
            else if (k == "token") token = v;
            else if (k == "name") name = v;
        }
        addrinfo hints{}, *res = nullptr;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) { err = "cannot resolve " + host; return false; }
        fd = socket(res->ai_family, SOCK_STREAM, 0);
        int ok = fd >= 0 ? connect(fd, res->ai_addr, res->ai_addrlen) : -1;
        freeaddrinfo(res);
        if (ok < 0) { err = "cannot connect to " + hostport; return false; }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        uint8_t key[16];
        std::random_device rd;
        for (auto &b : key) b = (uint8_t)rd();
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + hostport +
                          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + base64(key, 16) +
                          "\r\nSec-WebSocket-Version: 13\r\n\r\n";
        if (!sendAll(req.data(), req.size())) { err = "send failed"; return false; }
        std::string head;
        char c;
        while (head.find("\r\n\r\n") == std::string::npos && head.size() < 4096) {
            if (recv(fd, &c, 1, 0) != 1) { err = "connection closed during upgrade"; return false; }
            head += c;
        }
        if (head.find(" 101 ") == std::string::npos) { err = "upgrade refused: " + head.substr(0, head.find('\r')); return false; }
        std::string hello = "{\"type\":\"hello\",\"version\":1,\"room\":\"" + jsonEscape(room) + "\",\"user\":\"" +
                            jsonEscape(user) + "\",\"token\":\"" + jsonEscape(token) + "\",\"name\":\"" + jsonEscape(name) + "\"}";
        sendFrame(1, (const uint8_t *)hello.data(), hello.size());
        running = started = true;
        rx = std::thread([this] { readLoop(); });
        tx = std::thread([this] {
            std::vector<uint8_t> f, m;
            while (running) {
                if (!irda->takeOutgoing(f, 200)) continue;
                m.assign(1, 0x01);
                m.insert(m.end(), f.begin(), f.end());
                if (!sendFrame(2, m.data(), m.size())) break;
            }
        });
        LOGI("irda: InfraWAN %s room \"%s\" as %s", hostport.c_str(), room.c_str(), user.c_str());
        return true;
    }

    void stop() override {
        if (!started) return;
        started = false;
        running = false;
        irda->setLinked(false);
        irda->wake();
        shutdown(fd, SHUT_RDWR);
        if (rx.joinable()) rx.join();
        if (tx.joinable()) tx.join();
        close(fd);
    }

private:
    std::string url, host, room, user, token, name = "wmcore-cli";
    int fd = -1;
    Irda *irda = nullptr;
    std::atomic<bool> running{false};
    bool started = false;
    std::thread rx, tx;
    std::mutex wmu;
    std::mt19937 rng{std::random_device{}()};

    bool sendAll(const void *p, size_t n) {
        const char *c = (const char *)p;
        while (n) {
            ssize_t w = send(fd, c, n, MSG_NOSIGNAL);
            if (w <= 0) return false;
            c += w;
            n -= (size_t)w;
        }
        return true;
    }
    bool recvAll(void *p, size_t n) {
        char *c = (char *)p;
        while (n) {
            ssize_t r = recv(fd, c, n, 0);
            if (r <= 0) return false;
            c += r;
            n -= (size_t)r;
        }
        return true;
    }
    bool sendFrame(int op, const uint8_t *p, size_t n) {
        std::lock_guard<std::mutex> l(wmu);
        std::vector<uint8_t> h;
        h.push_back((uint8_t)(0x80 | op));
        if (n < 126) h.push_back((uint8_t)(0x80 | n));
        else if (n < 65536) { h.push_back(0x80 | 126); h.push_back((uint8_t)(n >> 8)); h.push_back((uint8_t)n); }
        else { h.push_back(0x80 | 127); for (int i = 7; i >= 0; i--) h.push_back((uint8_t)((uint64_t)n >> (8 * i))); }
        uint8_t mask[4];
        for (auto &b : mask) b = (uint8_t)rng();
        h.insert(h.end(), mask, mask + 4);
        h.reserve(h.size() + n);
        for (size_t i = 0; i < n; i++) h.push_back(p[i] ^ mask[i & 3]);
        return sendAll(h.data(), h.size());
    }
    void readLoop() {
        std::vector<uint8_t> msg;
        int msgOp = 0;
        while (running) {
            uint8_t h[2];
            if (!recvAll(h, 2)) break;
            int op = h[0] & 15;
            bool fin = h[0] & 0x80;
            uint64_t n = h[1] & 127;
            if (n == 126) { uint8_t e[2]; if (!recvAll(e, 2)) break; n = e[0] << 8 | e[1]; }
            else if (n == 127) { uint8_t e[8]; if (!recvAll(e, 8)) break; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | e[i]; }
            if (n > (1u << 20)) break;
            std::vector<uint8_t> p(n);
            if (n && !recvAll(p.data(), n)) break;
            if (op == 9) { sendFrame(10, p.data(), p.size()); continue; }  // ping -> pong
            if (op == 10) continue;
            if (op == 8) { LOGI("irda: InfraWAN closed by the relay"); break; }
            if (op != 0) { msgOp = op; msg.clear(); }
            msg.insert(msg.end(), p.begin(), p.end());
            if (!fin) continue;
            if (msgOp == 2 && msg.size() > 5 && msg[0] == 0x01) {
                uint32_t src = msg[1] | msg[2] << 8 | msg[3] << 16 | (uint32_t)msg[4] << 24;
                irda->fromPeer(src, msg.data() + 5, msg.size() - 5);
            } else if (msgOp == 1) {
                std::string j(msg.begin(), msg.end());
                LOGI("irda: relay says %s", j.c_str());
                if (j.find("\"type\":\"welcome\"") != std::string::npos) irda->setLinked(true);
                if (j.find("\"type\":\"leave\"") != std::string::npos) irda->peerGone((uint32_t)jsonNumber(j, "id"));
            }
        }
        irda->setLinked(false);
        running = false;
        irda->wake();
    }
};

}  // namespace

std::unique_ptr<IrdaLink> irdaUdpLink(int localPort, int remotePort, int delayMs) {
    return std::unique_ptr<IrdaLink>(new UdpLink(localPort, remotePort, delayMs));
}

std::unique_ptr<IrdaLink> irdaWanLink(const std::string &url) {
    return std::unique_ptr<IrdaLink>(new WanLink(url));
}

}  // namespace wm
