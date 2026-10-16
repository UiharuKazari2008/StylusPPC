// Windows Live Mobile gateway (prototype): a local service (localsvc.h) that plays Microsoft's dead
// "Mobile Web Services" (MWS) front end for the Windows Live for Windows Mobile client built into
// WM 6.1.4 / 6.5.3 (WLMMessenger.exe, WLMService.dll; version 10.6), and signs in to Escargot's MSNP
// server on the user's behalf. Research and protocol notes: docs/research-escargot.md §8.
//
// What the client needs, in order:
//   1. GET https://http.mws.mobile.live.com/.../SecurityPolicy/Default.aspx: a WS-Policy document
//      naming the Live ID issuer (RST endpoint), terms/privacy/sign-up links and a validity.  [done]
//   2. POST to that issuer: PPCRL RequestMultipleSecurityTokens with the Live ID and password. The
//      gateway signs in to Escargot with them (MSNP, Tweener) and answers with a compact token and a
//      proof key of its own.                                                                    [done]
//   3. The MWS services themselves (SOAP over HTTPS plus the push channel on tcp.mws.mobile.live.com:
//      50001): messaging RegisterRequest/InviteRequest/MessageRequest..., contacts ssync.  [captured only]
//
// Secrets never reach the capture log: passwords, tickets, tokens and proof keys are replaced by
// "(redacted)" before anything is written.
#pragma once
#include "localsvc.h"
#include "tlsproxy.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wm {

class MwsGateway : public LocalService {
public:
    struct Config {
        std::string captureDir;   // every exchange, secrets redacted, as numbered files here ("" = none)
        std::string msnpHost;     // MSNP notification server (default Escargot's)
        uint16_t msnpPort = 1863;
        std::string nexusUrl;     // Passport 1.4 nexus for the Tweener login (default Escargot's)
    };
    explicit MwsGateway(Config cfg);
    ~MwsGateway() override;
    void serve(int fd, uint16_t port, const std::string &host, bool tls) override;

    /** Where new connections are captured ("" = nowhere). Any thread. */
    void setCaptureDir(const std::string &dir);

    /** Where the addresses that signed in are remembered (mws-addresses.txt; no secrets). Any thread. */
    void setStateDir(const std::string &dir);

    /** DNS rules that send the mobile gateway's names to the local service (10.0.2.4). */
    static std::vector<HostRule> hostRules();

    /**
     * Self-test of the MSNP side without a guest: signs in as `user` / `password` to the configured
     * MSNP server and reports each step through the log. True when the server accepted the login.
     */
    bool testLogin(const std::string &user, const std::string &password, std::string *why = nullptr);

    /**
     * Reachability check that signs nobody in: MSNP VER/CVR with the configured server and the
     * Passport nexus lookup. `report` gets one line per step.
     */
    bool probe(std::string *report);

    /** Tests: sends one raw HTTP request as if the guest had (TLS already removed); the response. */
    std::string selfTestRequest(const std::string &request);

    struct Impl;

private:
    std::shared_ptr<Impl> impl;
};

/**
 * A fake MSNP8 notification server with a Tweener nexus and login (plain HTTP) on 127.0.0.1, for
 * tests of the gateway without any real account: accepts any password, and logs what it was sent
 * (password redacted). Returns the MSNP port and sets `nexusUrl`; 0 on failure. Runs until exit.
 */
uint16_t startFakeMsnpServer(std::string *nexusUrl);

}  // namespace wm
