// Local services: servers that run inside PocketStylus itself and stand in for dead Internet services
// the guest's built-in programs still call (first user: the Windows Live Mobile gateway in mwsgate.h,
// which answers Windows Live for Windows Mobile with Escargot's MSNP behind it).
//
// The guest reaches them at kLocalServiceIp (10.0.2.4), normally through a DNS rule (HostRule with that
// address as target). UserNet hands a plain TCP connection to that address straight to the service; a
// connection to a secure web proxy port goes through the proxy first, which terminates the guest's TLS
// (certificate for the original name, as for any site) and hands the service the decrypted stream.
// Header-only; a process has at most one local service (the latest set).
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace wm {

/** 10.0.2.4: the guest-side address of the local services. */
constexpr uint32_t kLocalServiceIp = 0x0A000204;
constexpr const char *kLocalServiceAddr = "10.0.2.4";

class LocalService {
public:
    virtual ~LocalService() = default;
    /**
     * One guest connection. `fd` is a connected stream socket carrying plaintext (the service owns it and
     * closes it). `port` is the port the guest connected to, `host` the name it looked up (the HTTP Host
     * for a TLS connection, else the newest DNS name for 10.0.2.4, or ""), `tls` whether the secure web
     * proxy decrypted it. Called on the network threads: return quickly (serve on a thread of your own).
     */
    virtual void serve(int fd, uint16_t port, const std::string &host, bool tls) = 0;
};

namespace localsvc_detail {
inline std::mutex &mu() { static std::mutex m; return m; }
inline std::shared_ptr<LocalService> &slot() { static std::shared_ptr<LocalService> s; return s; }
}  // namespace localsvc_detail

/** Installs (or with null removes) the local service. Any thread. */
inline void setLocalService(std::shared_ptr<LocalService> s) {
    std::lock_guard<std::mutex> lk(localsvc_detail::mu());
    localsvc_detail::slot() = std::move(s);
}

/** The local service, or null. Any thread. */
inline std::shared_ptr<LocalService> localService() {
    std::lock_guard<std::mutex> lk(localsvc_detail::mu());
    return localsvc_detail::slot();
}

}  // namespace wm
