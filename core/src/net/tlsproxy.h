// Secure web proxy: lets Windows Mobile's 2009-era TLS (Schannel: TLS 1.0 at best, RSA key exchange,
// AES-CBC / 3DES / RC4, a 2009 root store) reach today's HTTPS sites. UserNet hands it the guest's
// connections to TLS ports; it accepts the guest's handshake with a certificate it mints for the
// host on the fly (signed by a private root CA the guest helper installs into WM's root store),
// opens a modern TLS 1.2/1.3 connection to the real server, validates that server against Android's
// trust store and relays the bytes. An upstream that fails validation is never relayed unless the
// user, asked through a Windows Mobile dialog, chose to continue to that site with that certificate;
// otherwise the guest gets an error page. Design and security notes: docs/research-tls-proxy.md.
//
// Built with BoringSSL (third_party/boringssl, tools/build-boringssl.sh). Without it (WM_HAVE_TLS
// unset) open() fails and UserNet passes TLS ports through unchanged.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace wm {

/**
 * One DNS remap rule (UserNet::setHostRules): guest lookups of `pattern` answer as `target`.
 *   pattern  "host.example" (that name only) or ".example" (the domain and every name under it)
 *   target   an IPv4 literal (answered as is) or another host name (resolved instead)
 * The secure web proxy follows the same rules for the hosts it connects to, validating the upstream
 * certificate against the target name when the target is a name. Made for service revivals that
 * replace dead servers (e.g. Escargot for MSN/Windows Live Messenger, see docs/research-tls-proxy.md).
 */
struct HostRule {
    std::string pattern;
    std::string target;
    // A revived service that answers only to its own name (revival.h): through the proxy, requests get
    // Host = target and one request per connection, responses the original header spelling.
    bool http = false;
    bool matches(const std::string &host) const;
};

class TlsProxy {
public:
    struct Impl;

    /**
     * The proxy with its private CA kept in `dir` (created on first use: ca.key, ca.cer and leaf.key,
     * readable by this app/user only). Null with `err` set if the CA can't be loaded or created.
     */
    static std::shared_ptr<TlsProxy> open(const std::string &dir, std::string *err = nullptr);
    ~TlsProxy();

    /** The root certificate (DER), for the guest's root store. */
    const std::vector<uint8_t> &caDer() const;
    /** SHA-1 thumbprint of the root, upper-case hex (WM's store names certificates by it). */
    std::string caThumbprint() const;
    /** The folder given to open(); UserNet keeps its DNS name memory there (names.txt). */
    const std::string &dir() const;

    /**
     * Where upstream servers are validated from: a folder of PEM certificates (Android's
     * /apex/com.android.conscrypt/cacerts or /system/etc/security/cacerts, picked automatically).
     * For tests: a folder with no certificates makes every upstream fail validation.
     */
    static void setTrustDir(const std::string &dir);

    /** One intercepted guest connection; served on its own thread from serve(). */
    struct Conn {
        int fd = -1;                      // the guest's side: the bytes the guest's TLS sends and gets
        uint32_t ip = 0;                  // where the guest connected (host byte order)
        uint16_t port = 0;
        std::vector<std::string> names;   // host names the guest's DNS recently resolved to `ip`, newest first
        std::shared_ptr<const std::vector<HostRule>> rules;
    };
    /** Takes over `c.fd` (closes it when done). */
    void serve(Conn c);

    /**
     * Certificate warnings. When a server's certificate fails validation, the proxy holds the
     * guest's request and asks the user whether to continue (the app forwards the question to the
     * helper, which shows a Windows Mobile dialog; the CLI with --tls-ask does the same). One
     * question per host at a time: other connections to that host wait for the same answer. With
     * nobody polling takePrompts() for a few seconds the error page comes at once, as before.
     */
    struct CertPrompt {
        uint32_t id = 0;
        int seconds = 0;     // how long the request still waits for the answer
        std::string host;
        std::string text;    // the dialog's message, lines separated by '\n'
    };
    /** New questions since the last call (each handed out once). Polling also says "someone answers". */
    std::vector<CertPrompt> takePrompts();
    /** The user's answer. Yes allows that host with that certificate for as long as this proxy lives
     *  (or until clearExceptions), and the waiting requests go on. Late answers still count. */
    void answerPrompt(uint32_t id, bool allow);
    /** Forgets every "continue anyway" choice (a new emulator session). */
    void clearExceptions();
    /** How long a request waits for the answer (default kPromptWaitSec). */
    void setPromptWait(int seconds);
    static constexpr int kPromptWaitSec = 40;

private:
    TlsProxy();
    std::shared_ptr<Impl> impl;
};

/** The rule for `host` (lower case), or null. */
const HostRule *findHostRule(const std::vector<HostRule> *rules, const std::string &host);

/**
 * One request over HTTPS from PocketStylus itself (local services such as mwsgate.h): connects to
 * host:port, validates the server for `host` against the same store as the proxy's upstreams (no
 * exceptions), sends `request` and returns everything the server sent until it closed (or
 * `timeoutMs` passed). Empty with `err` set on failure. Without TLS support it always fails.
 */
std::string httpsExchange(const std::string &host, uint16_t port, const std::string &request, std::string *err,
                          int timeoutMs = 20000, size_t maxBytes = 8u << 20);

}  // namespace wm
