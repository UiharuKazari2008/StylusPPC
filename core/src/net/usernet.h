// User-mode network for the guest's network card: the guest sees a small LAN (it is 10.0.2.15 by
// DHCP, the gateway is 10.0.2.2, DNS 10.0.2.3, like QEMU's user networking) and everything it sends
// is re-originated from ordinary sockets on the phone, so no root or VPN is needed.
//
// Handled: ARP, DHCP, DNS (resolved with the phone's resolver), UDP and TCP to any address, ping to
// the gateway. 10.0.2.2 also stands for the phone itself (127.0.0.1), for app-side services.
//
// Threads: the emulation thread hands frames in with fromGuest() and takes frames out with
// nextForGuest()/popForGuest(); a private thread does all socket work.
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace wm {

class UserNet {
public:
    UserNet();
    ~UserNet();
    void start();
    void stop();

    /** An Ethernet frame the guest's card transmitted. */
    void fromGuest(const uint8_t *frame, size_t len);
    /** The next frame for the guest, if any (stays queued until popForGuest). */
    bool nextForGuest(std::vector<uint8_t> &frame);
    void popForGuest();

    static constexpr uint8_t kGuestIp[4] = {10, 0, 2, 15};
    static constexpr uint8_t kGatewayIp[4] = {10, 0, 2, 2};
    static constexpr uint8_t kDnsIp[4] = {10, 0, 2, 3};

private:
    struct Tcp;
    struct Udp;
    struct DnsDone { uint16_t guestPort; std::vector<uint8_t> reply; };

    void loop();
    void handleFrame(const std::vector<uint8_t> &f);
    void handleArp(const uint8_t *p, size_t n);
    void handleIp(const uint8_t *p, size_t n);
    void handleUdp(const uint8_t *ip, const uint8_t *p, size_t n);
    void handleTcp(const uint8_t *ip, const uint8_t *p, size_t n);
    void handleIcmp(const uint8_t *ip, const uint8_t *p, size_t n);
    void dhcp(const uint8_t *p, size_t n);
    void dns(uint16_t guestPort, const uint8_t *p, size_t n);

    void sendIp(uint8_t proto, uint32_t src, uint32_t dst, const uint8_t *payload, size_t n);
    void sendUdp(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, const uint8_t *p, size_t n);
    void sendTcp(Tcp &c, uint8_t flags, uint32_t seq, const uint8_t *data, size_t n, bool mssOption = false);
    void sendRst(uint32_t src, uint16_t sport, uint32_t dst, uint16_t dport, uint32_t seq, uint32_t ack);
    void pushToGuest(std::vector<uint8_t> f);

    void tcpPump(Tcp &c);
    void tcpTimers(uint64_t now);
    void closeTcp(uint64_t key);

    static uint16_t checksum(const uint8_t *p, size_t n, uint32_t sum = 0);
    static uint64_t nowMs();

    std::thread thread;
    std::atomic<bool> running{false};
    int wakeRd = -1, wakeWr = -1;

    std::mutex inMu, outMu;
    std::deque<std::vector<uint8_t>> inbox, outbox;
    // Resolver threads outlive nothing they don't own: they post into this shared box.
    struct DnsBox {
        std::mutex mu;
        std::vector<DnsDone> done;
        int wakeWr = -1;
        bool alive = true;
    };
    std::shared_ptr<DnsBox> dnsBox = std::make_shared<DnsBox>();

    uint8_t guestMac[6] = {};
    bool haveGuestMac = false;
    uint16_t ipId = 1;
    std::map<uint64_t, std::unique_ptr<Tcp>> tcps;
    std::map<uint64_t, std::unique_ptr<Udp>> udps;
};

}  // namespace wm
