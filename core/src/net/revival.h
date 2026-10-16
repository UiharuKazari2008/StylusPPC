// Revived services: DNS remap rule sets for servers that replaced Microsoft's dead ones, and the small
// HTTP fix-ups the secure web proxy applies to them (HostRule::http). Header-only; used by
// tlsproxy.cpp, the CLI (--escargot) and the app (NativeBridge.setEscargot).
//
// Escargot (https://escargot.chat/) runs MSN Messenger's MSNP servers again. Classic clients reach it
// once the names they have built in answer with Escargot's servers. Its HTTPS front ends serve only
// requests whose Host header is one of their own names (a request for nexus.passport.com gets 404) and
// spell some headers differently from Microsoft ("Passporturls"), so the proxy rewrites the Host header,
// keeps one request per connection and gives the response headers their original spelling.
// Research, measurements and what the Windows Mobile clients need: docs/research-escargot.md.
#pragma once
#include "tlsproxy.h"
#include "../util/log.h"
#include <cstring>
#include <string>
#include <strings.h>
#include <vector>

namespace wm {

/** Escargot's MSNP / Passport / config front ends (escargot.chat, checked 2026-10-16). */
constexpr const char *kEscargotServer = "msnmsgr.escargot.chat";
constexpr const char *kEscargotConfig = "conf.escargot.chat";

/**
 * Microsoft's Messenger and Passport names -> Escargot. The list follows WebOne's escargot.conf
 * (github.com/atauenis/webone) minus the mobile service names (*.mobile.msn.com, *.mobile.live.com):
 * Escargot has no handler for those, so the stock Windows Mobile clients gain nothing from a remap
 * and their requests are better not sent there. MSNP itself (TCP 1863) needs only the DNS answer.
 */
inline std::vector<HostRule> escargotHostRules() {
    static const char *const kServer[] = {
        "messenger.hotmail.com",           // notification server, MSNP on TCP 1863
        "gateway.messenger.hotmail.com",   // HTTP polling gateway (plain HTTP on port 80: see the doc)
        "nexus.passport.com",              // Passport 1.4 (Tweener): /rdr/pprdr.asp -> PassportURLs
        "login.passport.com",              // Tweener DALogin (/login2.srf)
        "loginnet.passport.com",           // RST (MSNP13/14 era)
        "login.live.com",                  // RST.srf / RST2.srf (MSNP15+ single sign-on), login2.srf
        ".contacts.msn.com",               // address book (ABCH) SOAP
        ".storage.msn.com",                // display pictures / profile (storage service)
        "ows.messenger.msn.com",           // offline messages
        "rsi.hotmail.com",                 // offline messages (retrieve)
        "sup.live.com",                    // sharing service
    };
    std::vector<HostRule> v;
    for (const char *n : kServer) {
        HostRule r{n, kEscargotServer};
        r.http = true;
        v.push_back(r);
    }
    HostRule conf{"config.messenger.msn.com", kEscargotConfig};
    conf.http = true;
    v.push_back(conf);
    // Escargot's own names (nexus hands out https://ds.escargot.nina.chat/login as DALogin): same
    // server, but their responses get the header spelling fix too.
    for (const char *n : {"ds.escargot.nina.chat", "msnmsgr.escargot.chat", "m1.escargot.chat"}) {
        HostRule self{n, n};
        self.http = true;
        v.push_back(self);
    }
    return v;
}

namespace revival_detail {

inline bool startsNoCase(const std::string &s, size_t at, const char *p) {
    size_t n = strlen(p);
    return s.size() - at >= n && strncasecmp(s.c_str() + at, p, n) == 0;
}

/** Hard-coded Tweener login URLs that Escargot serves under another path (WebOne's mapping). */
inline void fixPath(std::string &requestLine, const std::string &origHost) {
    static const char *const kLoginHosts[] = {"login.live.com", "login.passport.com", "loginnet.passport.com"};
    bool login = false;
    for (const char *h : kLoginHosts) login |= origHost == h;
    if (!login) return;
    size_t sp = requestLine.find(' ');
    if (sp == std::string::npos) return;
    if (startsNoCase(requestLine, sp + 1, "/login2.srf")) {
        char next = requestLine.size() > sp + 12 ? requestLine[sp + 12] : ' ';
        if (next == ' ' || next == '?') requestLine.replace(sp + 1, 11, "/login");
    }
}

}  // namespace revival_detail

/**
 * A request the guest sent for `origHost`, about to go to `server` instead (the head may be
 * followed by body bytes, which stay as they are): Host becomes `server`, Connection becomes close
 * (later requests on the connection would carry the old Host again), Tweener login paths are mapped.
 */
inline void revivalRequest(std::string &data, const std::string &origHost, const std::string &server) {
    using namespace revival_detail;
    size_t end = data.find("\r\n\r\n");
    if (end == std::string::npos) return;  // not an HTTP head: leave it alone
    std::string out;
    size_t i = 0;
    bool first = true;
    while (i < end) {
        size_t e = data.find("\r\n", i);
        if (e == std::string::npos || e > end) e = end;
        std::string line = data.substr(i, e - i);
        i = e + 2;
        if (first) {
            fixPath(line, origHost);
            first = false;
        } else if (startsNoCase(line, 0, "host:")) {
            line = "Host: " + server;
        } else if (startsNoCase(line, 0, "connection:") || startsNoCase(line, 0, "keep-alive:") ||
                   startsNoCase(line, 0, "proxy-connection:")) {
            continue;
        }
        out += line;
        out += "\r\n";
    }
    out += "Connection: close\r\n";
    LOGD("tls: revival: \"%s\" for %s sent with Host %s", out.substr(0, out.find("\r\n")).c_str(), origHost.c_str(), server.c_str());
    data.replace(0, end + 2, out);
}

/**
 * A piece of the server's response (the start of it, or more of its head): header names get the
 * spelling Microsoft's servers used, which old clients compare case-sensitively. True once the end of
 * the head was in this piece.
 */
inline bool revivalResponse(std::string &data) {
    using namespace revival_detail;
    static const char *const kNames[] = {"PassportURLs", "Authentication-Info", "WWW-Authenticate", "X-MSN-Messenger"};
    size_t i = 0;
    while (i < data.size()) {
        size_t e = data.find("\r\n", i);
        if (e == i) return true;  // the blank line: end of the head
        size_t c = data.find(':', i);
        if (c != std::string::npos && (e == std::string::npos || c < e)) {
            for (const char *n : kNames)
                if (c - i == strlen(n) && startsNoCase(data, i, n) && data.compare(i, c - i, n) != 0) {
                    LOGD("tls: revival: response header %s spelled as %s", data.substr(i, c - i).c_str(), n);
                    data.replace(i, c - i, n);
                }
        }
        if (e == std::string::npos) return false;
        i = e + 2;
    }
    return false;
}

}  // namespace wm
