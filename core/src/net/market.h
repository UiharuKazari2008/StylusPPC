// Windows Marketplace for Mobile, stood back up inside PocketStylus. Microsoft closed the service in
// 2012 (docs/research-marketplace.md). The names the Windows Mobile client and Internet Explorer
// still use (*.marketplace.windowsmobile.com) answer here, and the cabinets in a folder on the
// storage card are the catalogue. The built-in program speaks the 02/2009 catalog on
// api.marketplace.windowsmobile.com; Internet Explorer uses the HTML storefront.
//
// When an upstream host is set (Settings, default market.stylus.737.jp.net), those names are
// answered by forwarding to that host, which is the server in server/market. Cabinets on the
// storage card are used when the host is empty or does not answer.
// Nothing here is Microsoft's old store. There is no account and no payment.
#pragma once
#include "localsvc.h"
#include "tlsproxy.h"
#include <memory>
#include <string>
#include <vector>

namespace wm {

class Marketplace : public LocalService {
public:
    struct Config {
        std::string catalogDir;  // cabinets used when the upstream host is empty or does not answer
        std::string captureDir;  // redacted requests, "" = none
        std::string upstream;    // "host" (HTTPS) or "host:port" (plain HTTP unless the port is 443)
    };
    explicit Marketplace(Config cfg);
    ~Marketplace() override;
    void serve(int fd, uint16_t port, const std::string &host, bool tls) override;

    /** The folder scanned for cabinets. Any thread. */
    void setCatalogDir(const std::string &dir);
    /** Marketplace server hostname. "" uses only the cabinet folder. Any thread. */
    void setUpstream(const std::string &host);
    /** Where requests are captured, secrets left out ("" = nowhere). Any thread. */
    void setCaptureDir(const std::string &dir);

    /** DNS rules that send Marketplace's names to the local service (10.0.2.4). */
    static std::vector<HostRule> hostRules();

    /**
     * One raw HTTP request (headers and body already complete) answered as the guest would be.
     * A cabinet response carries the file bytes. For tests, without a socket.
     */
    std::string selfTestRequest(const std::string &request);

    struct Impl;

private:
    std::shared_ptr<Impl> impl;
};

/** Builds a tiny catalogue and checks the storefront, the catalog, a cabinet download and the upstream server. 0 = ok. */
int marketplaceSelfTest();

}  // namespace wm
