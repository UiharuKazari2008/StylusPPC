// Device Emulator folder sharing (0x500F4000): exposes host folders to the guest as \Storage Card.
// Behaviour follows docs/folder-sharing-protocol.md; operations complete synchronously.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unordered_map>
#include <vector>

namespace wm {

class StateIO;

class Bus;
class EmulServ;

class FolderShare {
public:
    void serialize(StateIO &s);
    FolderShare(Bus &bus, EmulServ &emulServ) : bus(bus), emulServ(emulServ) {}
    ~FolderShare();

    // What the guest sees. One mount with an empty name maps the share root directly (e.g. all of
    // phone storage); otherwise each mount appears as a top-level folder of \Storage Card.
    struct Mount {
        std::string name;   // top-level folder name in the guest, or "" for the root itself
        std::string path;   // host directory
    };
    // empty = sharing off; notifies the guest unless !announce (tests: the guest keeps its view)
    void setMounts(const std::vector<Mount> &mounts, bool announce = true);
    bool enabled() const { return !mounts.empty(); }
    // Folders the app adds at the root of every share (e.g. "2577" with the WMDroid helper, and the
    // ".wmdroid" mailbox). They take precedence over same-named entries and are reported hidden.
    void setOverlays(const std::vector<Mount> &overlays);
    void onGuestReset();   // closes everything, re-announces the share

    // Register interface used by DePage.
    uint32_t call(uint32_t code, uint32_t pbPa);
    // Operations since the last take (the background CPU cap rises while the card is in use).
    uint32_t takeOps() { return ops.exchange(0, std::memory_order_relaxed); }
    bool traceCalls = false;

private:
    struct Handle {
        bool used = false;
        int fd = -1;
        uint32_t mode = 0;
        std::string host;   // resolved host path
        std::string guest;  // guest path as supplied
    };
    struct Entry {
        std::string name, host;
        bool have = false;     // stat taken while the directory was listed
        bool dir = false;
        uint64_t size = 0;
        time_t mtime = 0, ctime = 0;
        mode_t mode = 0;
    };
    struct Meta {
        std::string name;
        bool dir = false;
        uint64_t size = 0;
        time_t mtime = 0, ctime = 0;
        mode_t mode = 0;
    };
    struct Snap {
        std::chrono::steady_clock::time_point at{};
        bool valid = false;
        std::vector<Meta> items;
    };
    struct Find {
        bool used = false;
        std::vector<Entry> entries;
        size_t next = 0;
    };

    Bus &bus;
    EmulServ &emulServ;
    std::mutex mu;
    std::vector<Mount> mounts, overlays;
    Handle handles[40];
    Find finds[40];
    std::unordered_map<std::string, Snap> dirCache;
    std::chrono::steady_clock::time_point spaceAt{};
    uint64_t spaceFree = 0, spaceTotal = 0;
    bool spaceOk = false;
    std::atomic<uint32_t> ops{0};

    void closeAll();
    void dropCache(const std::string &path);
    const Snap &snapshot(const std::string &dir);
    uint32_t dispatch(uint32_t code, uint8_t *pb);
    uint8_t *ram(uint32_t pa, uint32_t len);
    // Path resolution: guest "\a\b" -> host path. Returns 0 or an error code (0x7B, 3).
    // `virtualRoot` is set when the path names the synthetic root of a multi-mount share.
    uint32_t resolve(const std::u16string &guest, std::string &host, bool &virtualRoot, bool allowWild = false);
    std::string matchCase(const std::string &dir, const std::string &name);
    uint32_t errnoToCode(int err, bool parentKnown = true);
    void fillInfo(uint8_t *pb, const std::string &host, bool isVirtualDir);
    void fillInfo(uint8_t *pb, const Entry &e, bool isVirtualDir);
    void listDir(const std::string &host, bool virtualRoot, const std::string &pattern, std::vector<Entry> &out);

    uint32_t opCreate(uint8_t *pb);
    uint32_t opOpen(uint8_t *pb);
    uint32_t opRead(uint8_t *pb);
    uint32_t opWrite(uint8_t *pb);
    uint32_t opSetEof(uint8_t *pb);
    uint32_t opClose(uint8_t *pb);
    uint32_t opGetSpace(uint8_t *pb);
    uint32_t opMkDir(uint8_t *pb);
    uint32_t opRmDir(uint8_t *pb);
    uint32_t opSetAttributes(uint8_t *pb);
    uint32_t opRename(uint8_t *pb);
    uint32_t opDelete(uint8_t *pb);
    uint32_t opGetInfo(uint8_t *pb);
    uint32_t opGetFcbInfo(uint8_t *pb);
};

}  // namespace wm
