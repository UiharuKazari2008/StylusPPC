// Infrared (IrDA SIR) transceiver on UART2, tunnelled to other devices (docs/research-irda.md).
//
// The Windows Mobile images ship the whole Microsoft IrDA stack: irdastk.dll (IrLAP, IrLMP, TinyTP,
// IAS, AF_IRDA sockets), the serial-IR NDIS miniport irsir.dll (Comm\Irsir1, Port=2 -> "COM2:"),
// IrCOMM (COM3:) and OBEX beaming. Only the port is missing: DE's Drivers\BuiltIn\IRDA2410 names a
// DLL that is not in the image. The helper points that key at the stock serial_smdk2410.dll on
// UART2 (DeviceArrayIndex=1, the BSP's IR UART), so irsir opens COM2 at boot and does the SIR
// framing in software: BOF 0xC0, escaped bytes (CE 0x7D, xor 0x20), CRC-16 FCS, EOF 0xC1.
//
// This class is the other end of that line. It decodes the guest's SIR frames into IrLAP frames
// (address, control, information; no FCS) for a transport (Bluetooth LE or the InfraWAN relay),
// and encodes frames from other devices back into SIR bytes for the guest's receiver, paced so the
// serial driver's buffers never overflow. The transport sees opaque frames; every device on it
// hears every frame, as in a room full of IR ports.
//
// Discovery: IrLAP finds devices with XID commands in time slots (25-85 ms each) and expects the
// answer in the slot the responder picks, far shorter than an Internet round trip. The bridge keeps
// the identity of every device it has heard (address, hints, nickname) and answers the guest's
// discovery for them at once; the remote device's own late answer is then dropped. The first
// discovery after a device appears goes over the network and fills the cache.
//
// Threads: fromGuest() and service() run on the emulation thread (UART2); the host side (transport
// threads, JNI, CLI) uses the other calls, which only touch the queues under the mutex.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace wm {

class Uart;

class Irda {
public:
    explicit Irda(Uart &u) : uart(u) {}

    // ---- Emulation thread ----
    /** A byte the guest transmitted on UART2. */
    void fromGuest(uint8_t c);
    /** Feeds queued frames to the guest's receiver; called often (each pass of the run loop). */
    void service(uint64_t nowNs);
    /** The guest restarted: partial frames and the discovery round are dropped. */
    void reset();

    // ---- Host (any thread) ----
    /**
     * A transport is connected: the guest's frames go out (takeOutgoing) and remote frames come in.
     * Unlinked, the guest's frames are dropped (its IrLAP just finds nobody), but still raise "start".
     */
    void setLinked(bool on);
    bool linked() const { return linkedFlag; }
    /** Next frame the guest sent, waiting up to timeoutMs (0: no wait). False when none. */
    bool takeOutgoing(std::vector<uint8_t> &frame, int timeoutMs);
    /** Wakes a takeOutgoing() waiter (transport shutting down). */
    void wake();
    /** A frame from another device (node: the transport's id for it, for forgetting it later). */
    void fromPeer(uint32_t node, const uint8_t *f, size_t n);
    /** The transport lost a device: its cached discovery answer goes. */
    void peerGone(uint32_t node);
    /**
     * Events for the app, one per line:
     *   "start"  the guest began an infrared operation (discovery or a connection) after being quiet;
     *   "idle"   no infrared traffic for kIdleNs.
     */
    std::string takeEvents();
    /** The guest has used infrared within kIdleNs. */
    bool active() const { return activeFlag; }
    /** Devices currently in the discovery cache ("addr nickname" per line), for diagnostics/UI. */
    std::string peers();

    bool trace = false;       // log every frame (CLI --irda-trace)
    std::atomic<uint64_t> framesOut{0}, framesIn{0}, badFrames{0}, spoofed{0}, dropped{0};

    static constexpr uint64_t kIdleNs = 60000000000ull;     // 60 s without IR traffic: "idle"
    static constexpr uint64_t kPeerTtlNs = 120000000000ull;  // forget a silent device's identity
    static constexpr size_t kMaxFrame = 4096;               // IrLAP data size <= 2048, plus headers

    /** IrLAP frame in words, e.g. "XID cmd slot 2/8 ff:0000a1b2>ffffffff" (traces, tests). */
    static std::string describe(const uint8_t *f, size_t n);
    /** SIR framing: BOF, escaped frame + FCS, EOF. */
    static void encodeSir(const uint8_t *f, size_t n, std::vector<uint8_t> &out);
    static uint16_t fcs(const uint8_t *p, size_t n, uint16_t crc = 0xFFFF);

private:
    Uart &uart;

    // Emulation thread: receive decoder for the guest's transmit side
    std::vector<uint8_t> rxFrame;
    bool inFrame = false, escaped = false;
    uint64_t now = 0;
    uint64_t lastTrafficNs = 0;
    uint64_t bofNs = 0;        // when the guest started its current frame (trace)
    uint64_t feedStartNs = 0;  // first byte of the current batch for the guest (receive latency trace)
    bool quiet = true;        // no IR traffic since the last "idle"
    std::deque<uint8_t> toGuest;  // SIR bytes waiting for the guest's receiver
    void frameFromGuest(const std::vector<uint8_t> &f);
    void deliver(const uint8_t *f, size_t n);  // to the guest (emulation thread)

    // Discovery state (emulation thread; peer cache under mu)
    struct Peer {
        uint32_t node = 0;
        std::vector<uint8_t> info;  // discovery info: hints, charset, nickname
        uint8_t version = 0;
        uint64_t seenNs = 0;
    };
    std::map<uint32_t, Peer> peerCache;  // by IrLAP device address
    uint32_t guestAddr = 0;
    struct Round {
        bool active = false;
        uint8_t flags = 0, slot = 0, slots = 1;
        uint32_t src = 0;
        uint64_t lastNs = 0;
        std::set<uint32_t> answered;
    } round;
    void answerFromCache();
    // Repeated connection requests (high-latency links, see inboundFrame)
    std::vector<uint8_t> lastSnrm;
    uint64_t lastSnrmNs = 0;
    bool uaSinceSnrm = false;
    static constexpr uint64_t kSnrmRepeatNs = 5000000000ull;
    void inboundFrame(uint32_t node, const std::vector<uint8_t> &f);

    // Host side, under mu
    std::mutex mu;
    std::condition_variable outCv;
    std::deque<std::vector<uint8_t>> outbox;              // guest -> transport
    std::deque<std::pair<uint32_t, std::vector<uint8_t>>> inbox;  // transport -> guest
    std::vector<uint32_t> gone;
    std::vector<std::string> events;
    std::atomic<bool> inboxReady{false};
    std::atomic<bool> linkedFlag{false}, activeFlag{false};
    bool woken = false;
    void event(const char *e);
};

}  // namespace wm
