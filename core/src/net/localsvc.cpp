// LocalDispatch (localsvc.h): one installed local service that hands the connection to the stand-in
// whose host pattern matches.
#include "localsvc.h"
#include "../util/log.h"

#include <cctype>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace wm {
namespace {

std::string lowerHost(std::string h) {
    while (!h.empty() && (h.back() == '.' || h.back() == ' ')) h.pop_back();
    auto colon = h.rfind(':');
    if (colon != std::string::npos && h.find(']') == std::string::npos) h.resize(colon);
    for (char &ch : h) ch = (char)tolower((unsigned char)ch);
    return h;
}

bool hostMatches(const std::string &pattern, const std::string &host) {
    if (pattern.empty() || host.empty()) return false;
    if (pattern[0] == '.') {
        if (host == pattern.substr(1)) return true;
        return host.size() > pattern.size() &&
               host.compare(host.size() - pattern.size(), pattern.size(), pattern) == 0;
    }
    return host == pattern;
}

std::string headerValue(const std::string &head, const char *name) {
    std::string want = name;
    for (char &ch : want) ch = (char)tolower((unsigned char)ch);
    size_t i = 0;
    while (i < head.size()) {
        size_t end = head.find("\r\n", i);
        if (end == std::string::npos) end = head.size();
        std::string line = head.substr(i, end - i);
        auto c = line.find(':');
        if (c != std::string::npos) {
            std::string key = lowerHost(line.substr(0, c));
            if (key == want) {
                size_t v = c + 1;
                while (v < line.size() && line[v] == ' ') v++;
                return lowerHost(line.substr(v));
            }
        }
        if (end == head.size()) break;
        i = end + 2;
    }
    return {};
}

// Host from a request already sitting in the socket, or soon to arrive. Does not consume bytes.
std::string peekHost(int fd) {
    std::string buf;
    for (int spins = 0; spins < 20 && buf.find("\r\n\r\n") == std::string::npos && buf.size() < 8192; spins++) {
        char tmp[8192];
        ssize_t n = recv(fd, tmp, sizeof tmp, MSG_PEEK | MSG_DONTWAIT);
        if (n > 0) {
            buf.assign(tmp, (size_t)n);
            if (buf.find("\r\n\r\n") != std::string::npos) break;
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            break;
        }
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, 50) <= 0 && n <= 0) break;
    }
    if (buf.size() < 5 || !(buf[0] == 'G' || buf[0] == 'P' || buf[0] == 'H' || buf[0] == 'D')) return {};
    return headerValue(buf, "host");
}

}  // namespace

void LocalDispatch::setRoutes(std::vector<Route> r) {
    for (Route &route : r)
        for (char &ch : route.pattern) ch = (char)tolower((unsigned char)ch);
    std::lock_guard<std::mutex> lk(mu);
    routes = std::move(r);
}

void LocalDispatch::serve(int fd, uint16_t port, const std::string &host, bool tls) {
    std::vector<Route> routes;
    {
        std::lock_guard<std::mutex> lk(mu);
        routes = this->routes;
    }
    auto pick = [&](const std::string &h) -> std::shared_ptr<LocalService> {
        for (const Route &r : routes)
            if (hostMatches(r.pattern, h)) return r.service;
        return nullptr;
    };
    std::string h = lowerHost(host);
    std::shared_ptr<LocalService> svc = pick(h);
    // Plain HTTP only: HTTPS already carries the Host the proxy read. The push channel (not port 80)
    // is binary and must not wait on a header.
    if (!svc && !tls && (port == 80 || port == 8080)) svc = pick(peekHost(fd));
    if (!svc && routes.size() == 1) svc = routes[0].service;
    if (!svc) {
        LOGW("local: no service for %s port %u", h.c_str(), port);
        ::close(fd);
        return;
    }
    svc->serve(fd, port, h, tls);
}

}  // namespace wm
