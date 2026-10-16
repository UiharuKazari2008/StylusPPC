// Windows Live Mobile gateway (prototype). See mwsgate.h and docs/research-escargot.md §8.
#include "mwsgate.h"
#include "revival.h"
#include "mwspdu.h"
#include "../util/log.h"

#if WM_HAVE_TLS

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <openssl/base64.h>
#include <openssl/md5.h>
#include <openssl/rand.h>

namespace wm {

namespace {

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (char &ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

bool sendAll(int fd, const std::string &s) {
    size_t done = 0;
    while (done < s.size()) {
        ssize_t w = send(fd, s.data() + done, s.size() - done, MSG_NOSIGNAL);
        if (w > 0) { done += (size_t)w; continue; }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            pollfd p{fd, POLLOUT, 0};
            if (poll(&p, 1, 10000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

// Appends what arrives within `timeoutMs`: 1 data, 0 timeout, -1 end of stream or error.
int recvWait(int fd, std::string &buf, int timeoutMs) {
    pollfd p{fd, POLLIN, 0};
    int r = poll(&p, 1, timeoutMs);
    if (r == 0) return 0;
    if (r < 0) return errno == EINTR ? 0 : -1;
    char tmp[16384];
    ssize_t n = recv(fd, tmp, sizeof tmp, 0);
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return 0;
    if (n <= 0) return -1;
    buf.append(tmp, (size_t)n);
    return 1;
}

// The same, false for both timeout and end of stream.
bool recvSome(int fd, std::string &buf, int timeoutMs) { return recvWait(fd, buf, timeoutMs) > 0; }

int connectTcp(const std::string &host, uint16_t port, int timeoutMs) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return -1;
    int fd = -1;
    for (addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        fd = socket(a->ai_family, SOCK_STREAM, 0);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        if (connect(fd, a->ai_addr, a->ai_addrlen) != 0 && errno != EINPROGRESS) { close(fd); fd = -1; continue; }
        pollfd p{fd, POLLOUT, 0};
        int err = 0;
        socklen_t el = sizeof err;
        if (poll(&p, 1, timeoutMs) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) { close(fd); fd = -1; continue; }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);
    }
    freeaddrinfo(res);
    return fd;
}

std::string xmlEscape(const std::string &s) {
    std::string o;
    for (char ch : s) {
        if (ch == '<') o += "&lt;";
        else if (ch == '>') o += "&gt;";
        else if (ch == '&') o += "&amp;";
        else if (ch == '"') o += "&quot;";
        else if (ch == '\'') o += "&apos;";
        else o += ch;
    }
    return o;
}

std::string xmlUnescape(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] != '&') { o += s[i]; continue; }
        size_t e = s.find(';', i);
        if (e == std::string::npos || e - i > 10) { o += s[i]; continue; }
        std::string ent = s.substr(i + 1, e - i - 1);
        if (ent == "lt") o += '<';
        else if (ent == "gt") o += '>';
        else if (ent == "amp") o += '&';
        else if (ent == "quot") o += '"';
        else if (ent == "apos") o += '\'';
        else if (!ent.empty() && ent[0] == '#') {
            unsigned long v = ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent.c_str() + 2, nullptr, 16) : strtoul(ent.c_str() + 1, nullptr, 10);
            if (v < 0x80) o += (char)v;
            else if (v < 0x800) { o += (char)(0xC0 | (v >> 6)); o += (char)(0x80 | (v & 63)); }
            else { o += (char)(0xE0 | (v >> 12)); o += (char)(0x80 | ((v >> 6) & 63)); o += (char)(0x80 | (v & 63)); }
        } else { o += s.substr(i, e - i + 1); }
        i = e;
    }
    return o;
}

// MSNP and Passport 1.4 URL encoding: everything but letters and digits as %XX.
std::string urlEncode(const std::string &s) {
    std::string o;
    char h[4];
    for (unsigned char ch : s) {
        if (isalnum(ch)) o += (char)ch;
        else { snprintf(h, sizeof h, "%%%02X", ch); o += h; }
    }
    return o;
}

std::string urlDecode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else o += s[i];
    }
    return o;
}

// Every element named `local` (any prefix): the text between its start and end tags.
std::vector<std::string> elements(const std::string &xml, const std::string &local) {
    std::vector<std::string> v;
    size_t i = 0;
    while ((i = xml.find('<', i)) != std::string::npos) {
        size_t nameEnd = xml.find_first_of(" \t\r\n/>", i + 1);
        if (nameEnd == std::string::npos) break;
        std::string name = xml.substr(i + 1, nameEnd - i - 1);
        size_t colon = name.find(':');
        std::string ln = colon == std::string::npos ? name : name.substr(colon + 1);
        size_t gt = xml.find('>', i);
        if (gt == std::string::npos) break;
        if (ln == local && name[0] != '/' && xml[gt - 1] != '/') {
            std::string close = "</" + name + ">";
            size_t e = xml.find(close, gt);
            if (e == std::string::npos) break;
            v.push_back(xml.substr(gt + 1, e - gt - 1));
            i = e + close.size();
            continue;
        }
        i = gt + 1;
    }
    return v;
}

std::string element(const std::string &xml, const std::string &local) {
    std::vector<std::string> v = elements(xml, local);
    return v.empty() ? std::string() : v[0];
}

std::string isoTime(time_t t) {
    struct tm g {};
    gmtime_r(&t, &g);
    char b[32];
    strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%SZ", &g);
    return b;
}

std::string randomBytes(size_t n) {
    std::string s(n, '\0');
    RAND_bytes((uint8_t *)&s[0], n);
    return s;
}

std::string base64(const std::string &b) {
    size_t n = 0;
    EVP_EncodedLength(&n, b.size());
    std::string o(n, '\0');
    size_t w = EVP_EncodeBlock((uint8_t *)&o[0], (const uint8_t *)b.data(), b.size());
    o.resize(w);
    return o;
}

std::string hex(const std::string &b) {
    static const char *d = "0123456789abcdef";
    std::string o;
    for (unsigned char ch : b) { o += d[ch >> 4]; o += d[ch & 15]; }
    return o;
}

std::string md5hex(const std::string &s) {
    uint8_t md[MD5_DIGEST_LENGTH];
    MD5((const uint8_t *)s.data(), s.size(), md);
    return hex(std::string((const char *)md, sizeof md));
}

// ---- Redaction: what may go into a log ----

void redactBetween(std::string &s, const std::string &open, const std::string &close) {
    size_t i = 0;
    while ((i = s.find(open, i)) != std::string::npos) {
        size_t a = i + open.size();
        size_t e = s.find(close, a);
        if (e == std::string::npos) e = s.size();
        if (e > a) s.replace(a, e - a, "(redacted)");
        i = a + 10;
    }
}

// Element contents with this local name (any prefix) are secrets: each start tag's text up to the
// next tag becomes "(redacted)".
void redactElement(std::string &s, const std::string &local) {
    size_t i = 0;
    while ((i = s.find('<', i)) != std::string::npos) {
        size_t nameEnd = s.find_first_of(" \t\r\n/>", i + 1);
        if (nameEnd == std::string::npos) break;
        std::string name = s.substr(i + 1, nameEnd - i - 1);
        size_t colon = name.find(':');
        std::string ln = colon == std::string::npos ? name : name.substr(colon + 1);
        size_t gt = s.find('>', i);
        if (gt == std::string::npos) break;
        if (ln == local && !name.empty() && name[0] != '/' && s[gt - 1] != '/') {
            size_t e = s.find('<', gt);
            if (e == std::string::npos) e = s.size();
            if (e > gt + 1) s.replace(gt + 1, e - gt - 1, "(redacted)");
        }
        i = gt + 1;
    }
}

std::string redact(std::string s) {
    for (const char *el : {"Password", "BinarySecret", "BinarySecurityToken", "CipherValue", "SignatureValue", "Nonce", "Identifier"})
        redactElement(s, el);
    for (const char *h : {"Authorization:", "authorization:", "Authentication-Info:", "authentication-info:", "Cookie:", "Set-Cookie:"})
        redactBetween(s, h, "\r\n");
    redactBetween(s, "pwd=", ",");
    redactBetween(s, "from-PP='", "'");
    redactBetween(s, "TWN S t=", "\r\n");
    return s;
}

// Printable text for a log: CR LF kept, other control bytes as \xHH.
std::string printable(const std::string &s) {
    std::string o;
    char h[8];
    for (unsigned char ch : s) {
        if (ch == '\n' || ch == '\r' || ch == '\t' || (ch >= 0x20 && ch < 0x7F)) o += (char)ch;
        else { snprintf(h, sizeof h, "\\x%02X", ch); o += h; }
    }
    return o;
}

std::string hexDump(const std::string &s) {
    std::string o;
    char line[128];
    for (size_t i = 0; i < s.size(); i += 16) {
        int n = snprintf(line, sizeof line, "%06zx ", i);
        std::string asc;
        for (size_t k = 0; k < 16; k++) {
            if (i + k < s.size()) {
                unsigned char ch = (unsigned char)s[i + k];
                n += snprintf(line + n, sizeof line - n, " %02x", ch);
                asc += ch >= 0x20 && ch < 0x7F ? (char)ch : '.';
            } else n += snprintf(line + n, sizeof line - n, "   ");
        }
        o += line;
        o += "  " + asc + "\n";
    }
    return o;
}

// ---- HTTP (both sides) ----

struct HttpMsg {
    std::string first;  // request or status line
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    std::string header(const std::string &name) const {
        for (auto &h : headers)
            if (strcasecmp(h.first.c_str(), name.c_str()) == 0) return h.second;
        return "";
    }
};

// Parses one HTTP message from the start of `buf` when complete (removing it). `more` reads more.
// Returns false at end of stream before a complete message.
template <class More>
bool readHttp(std::string &buf, HttpMsg &m, More more, bool response = false) {
    for (;;) {
        size_t he = buf.find("\r\n\r\n");
        if (he != std::string::npos) {
            m = HttpMsg();
            size_t i = 0;
            bool firstLine = true;
            while (i < he) {
                size_t e = buf.find("\r\n", i);
                if (e == std::string::npos || e > he) e = he;
                std::string line = buf.substr(i, e - i);
                i = e + 2;
                if (firstLine) { m.first = line; firstLine = false; continue; }
                size_t c = line.find(':');
                if (c == std::string::npos) continue;
                std::string v = line.substr(c + 1);
                v.erase(0, v.find_first_not_of(" \t"));
                m.headers.push_back({line.substr(0, c), v});
            }
            size_t bodyAt = he + 4;
            std::string te = lower(m.header("Transfer-Encoding"));
            std::string cl = m.header("Content-Length");
            if (te.find("chunked") != std::string::npos) {
                // Chunked: wait for the whole thing.
                for (;;) {
                    std::string body;
                    size_t p = bodyAt;
                    bool done = false;
                    while (true) {
                        size_t le = buf.find("\r\n", p);
                        if (le == std::string::npos) break;
                        size_t n = strtoul(buf.substr(p, le - p).c_str(), nullptr, 16);
                        if (n == 0) {
                            size_t end = buf.find("\r\n\r\n", le);
                            if (end == std::string::npos && buf.size() >= le + 4 && buf.compare(le, 4, "\r\n\r\n") == 0) end = le;
                            if (end == std::string::npos) break;
                            m.body = body;
                            buf.erase(0, end + 4);
                            done = true;
                            break;
                        }
                        if (buf.size() < le + 2 + n + 2) break;
                        body.append(buf, le + 2, n);
                        p = le + 2 + n + 2;
                    }
                    if (done) return true;
                    if (!more()) return false;
                }
            }
            size_t len = cl.empty() ? 0 : (size_t)strtoul(cl.c_str(), nullptr, 10);
            if (cl.empty() && response) {
                // A response without a length runs to the end of the stream.
                while (more()) {}
                m.body = buf.substr(bodyAt);
                buf.clear();
                return true;
            }
            while (buf.size() < bodyAt + len)
                if (!more()) return false;
            m.body = buf.substr(bodyAt, len);
            buf.erase(0, bodyAt + len);
            return true;
        }
        if (buf.size() > (1u << 20)) return false;
        if (!more()) return false;
    }
}

struct Url {
    bool https = true;
    std::string host, path = "/";
    uint16_t port = 443;
};

Url parseUrl(std::string u) {
    Url r;
    if (strncasecmp(u.c_str(), "https://", 8) == 0) u = u.substr(8);
    else if (strncasecmp(u.c_str(), "http://", 7) == 0) { u = u.substr(7); r.https = false; r.port = 80; }
    size_t sl = u.find('/');
    std::string hp = sl == std::string::npos ? u : u.substr(0, sl);
    if (sl != std::string::npos) r.path = u.substr(sl);
    size_t c = hp.find(':');
    if (c != std::string::npos) { r.port = (uint16_t)atoi(hp.c_str() + c + 1); hp.resize(c); }
    r.host = lower(hp);
    return r;
}

// One GET to `url`; the response head and body. False with `why` when no response came.
bool httpGet(const std::string &url, const std::string &extraHeaders, HttpMsg &resp, std::string *why) {
    Url u = parseUrl(url);
    std::string req = "GET " + u.path + " HTTP/1.1\r\nHost: " + u.host + "\r\nUser-Agent: MSMSGS\r\nConnection: close\r\nCache-Control: no-cache\r\n" +
                      extraHeaders + "\r\n";
    std::string raw;
    if (u.https) {
        raw = httpsExchange(u.host, u.port, req, why);
        if (raw.empty()) return false;
    } else {
        int fd = connectTcp(u.host, u.port, 15000);
        if (fd < 0) { if (why) *why = "cannot connect to " + u.host; return false; }
        sendAll(fd, req);
        while (recvSome(fd, raw, 20000)) {}
        close(fd);
    }
    std::string buf = raw;
    if (!readHttp(buf, resp, [] { return false; }, true)) {
        // readHttp wants a complete message; a body cut short still has a usable head.
        size_t he = raw.find("\r\n\r\n");
        if (he == std::string::npos) { if (why) *why = "no HTTP response"; return false; }
        std::string head = raw.substr(0, he + 4);
        readHttp(head, resp, [] { return false; }, true);
    }
    return true;
}

int statusCode(const HttpMsg &m) {
    size_t sp = m.first.find(' ');
    return sp == std::string::npos ? 0 : atoi(m.first.c_str() + sp + 1);
}

std::string httpResponse(int code, const char *reason, const std::string &type, const std::string &body, bool close) {
    return "HTTP/1.1 " + std::to_string(code) + " " + reason + "\r\nContent-Type: " + type + "\r\nContent-Length: " +
           std::to_string(body.size()) + "\r\nCache-Control: no-cache\r\n" + (close ? "Connection: close\r\n" : "") + "\r\n" + body;
}

// ---- Capture log ----

class Capture {
public:
    explicit Capture(const std::string &dir) { setDir(dir); }
    // Files of one gateway run start with its start time, so a later run never appends to them.
    void setDir(const std::string &d) {
        std::lock_guard<std::mutex> lk(mu);
        if (d == dir) return;
        dir = d;
        if (!dir.empty()) mkdir(dir.c_str(), 0700);
    }
    bool on() {
        std::lock_guard<std::mutex> lk(mu);
        return !dir.empty();
    }
    // A new numbered file for one connection; returns its path ("" when off).
    std::string open(const std::string &what) {
        std::lock_guard<std::mutex> lk(mu);
        if (dir.empty() || count >= 500) return "";  // a capture is for a few sign-ins, not for ever
        int n = ++count;
        char name[64];
        snprintf(name, sizeof name, "/%s-%03d-", runTag.c_str(), n);
        std::string safe;
        for (char ch : what) safe += isalnum((unsigned char)ch) || ch == '.' || ch == '-' ? ch : '_';
        return dir + name + safe + ".txt";
    }
    // `bytes`: the size of what went over the wire (the text may be a hex dump or redacted).
    static void write(const std::string &path, const std::string &title, const std::string &text, size_t bytes) {
        if (path.empty()) return;
        FILE *f = fopen(path.c_str(), "a");
        if (!f) return;
        fprintf(f, "\n===== %s (%zu bytes) =====\n%s\n", title.c_str(), bytes, text.c_str());
        fclose(f);
    }

private:
    std::mutex mu;
    std::string dir;
    int count = 0;
    std::string runTag = [] {
        time_t t = time(nullptr);
        struct tm g {};
        localtime_r(&t, &g);
        char b[32];
        strftime(b, sizeof b, "%Y%m%d-%H%M%S", &g);
        return std::string(b);
    }();
};

// ---- MSNP (the gateway is the Messenger client here) ----

constexpr const char *kMsnpClientId = "msmsgs@msnmsgr.com";  // MSN Messenger 6.x challenge identity
constexpr const char *kMsnpClientKey = "Q1P7W2E4J9R8U3S5";

class Msnp {
public:
    ~Msnp() { if (fd >= 0) close(fd); }
    bool open(const std::string &host, uint16_t port, std::string *why) {
        if (fd >= 0) close(fd);
        buf.clear();
        fd = connectTcp(host, port, 15000);
        if (fd < 0) { if (why) *why = "cannot connect to " + host + ":" + std::to_string(port); return false; }
        return true;
    }
    int send(const std::string &cmd, const std::string &args, const std::string &payload = std::string(), bool hasPayload = false) {
        int t = ++trid;
        std::string line = cmd + " " + std::to_string(t) + (args.empty() ? "" : " " + args) + "\r\n";
        LOGD("mws: msnp >> %s", redact(line.substr(0, line.size() - 2)).c_str());
        if (!sendAll(fd, line + payload)) return -1;
        (void)hasPayload;
        return t;
    }
    // The next command line (without CRLF); payload commands also fill `payload`. False on end/timeout.
    bool next(std::string &line, std::string *payload, int timeoutMs) {
        uint64_t end = nowMs() + (uint64_t)timeoutMs;
        for (;;) {
            size_t e = buf.find("\r\n");
            if (e != std::string::npos) {
                line = buf.substr(0, e);
                size_t need = 0;
                std::string c = line.substr(0, 3);
                if (c == "MSG" || c == "NOT" || c == "IPG" || c == "GCF" || c == "UBX" || c == "UBN" || c == "NFY" || c == "SDG") {
                    size_t sp = line.rfind(' ');
                    if (sp != std::string::npos) need = (size_t)strtoul(line.c_str() + sp + 1, nullptr, 10);
                }
                if (buf.size() >= e + 2 + need) {
                    if (payload) *payload = buf.substr(e + 2, need);
                    buf.erase(0, e + 2 + need);
                    LOGD("mws: msnp << %s", redact(line).c_str());
                    return true;
                }
            }
            uint64_t now = nowMs();
            if (now >= end) return false;
            if (recvWait(fd, buf, (int)std::min<uint64_t>(end - now, 1000)) < 0) { closed = true; return false; }
        }
    }
    int fd = -1;
    bool closed = false;
    int trid = 0;
    std::string buf;
};

std::vector<std::string> words(const std::string &s) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find(' ', i);
        if (e == std::string::npos) e = s.size();
        if (e > i) v.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    return v;
}

}  // namespace

// ---------------------------------------------------------------------------------------------

struct MwsGateway::Impl : std::enable_shared_from_this<MwsGateway::Impl> {
    Config cfg;
    Capture cap;
    explicit Impl(Config c) : cfg(std::move(c)), cap(cfg.captureDir) {}

    // One signed-in user: the MSNP connection the gateway keeps for them.
    struct Session {
        std::string user, token, proofKey;
        std::string nick;
        std::unique_ptr<Msnp> ns;
        std::atomic<bool> stop{false};
        std::atomic<uint64_t> lastUse{0};
        int contacts = 0, groups = 0, online = 0;
        ~Session() {
            stop = true;
        }
    };
    std::mutex mu;
    std::map<std::string, std::shared_ptr<Session>> sessions;  // by token

    // ---- Addresses that signed in (no secrets): the sign-in page's domain list offers only
    // hotmail.com / live.com / msn.com, so "name@hotmail.com" after "name@escargot.chat" signed in
    // means the same account. Kept in memory and in <stateDir>/mws-addresses.txt.
    std::vector<std::string> addresses;  // newest first
    std::string addressFile;

    void setStateDir(const std::string &dir) {
        std::lock_guard<std::mutex> lk(mu);
        std::string f = dir.empty() ? std::string() : dir + "/mws-addresses.txt";
        if (f == addressFile) return;
        addressFile = f;
        if (f.empty()) return;
        FILE *in = fopen(f.c_str(), "r");
        if (!in) return;
        char line[320];
        while (fgets(line, sizeof line, in)) {
            std::string a = line;
            while (!a.empty() && (a.back() == '\n' || a.back() == '\r' || a.back() == ' ')) a.pop_back();
            if (a.find('@') != std::string::npos && std::find(addresses.begin(), addresses.end(), a) == addresses.end())
                addresses.push_back(a);
        }
        fclose(in);
    }

    std::string mapAddress(const std::string &user) {
        size_t at = user.find('@');
        if (at == std::string::npos) return user;
        std::string dom = lower(user.substr(at + 1)), local = lower(user.substr(0, at));
        if (dom != "hotmail.com" && dom != "live.com" && dom != "msn.com") return user;
        std::lock_guard<std::mutex> lk(mu);
        for (const std::string &a : addresses) {
            size_t aa = a.find('@');
            if (lower(a.substr(0, aa)) == local && lower(a.substr(aa + 1)) != dom) {
                LOGI("mws: %s taken as the address that signed in before (domain %s)", dom.c_str(), a.substr(aa + 1).c_str());
                return a;
            }
        }
        return user;
    }

    void rememberAddress(const std::string &user) {
        std::lock_guard<std::mutex> lk(mu);
        addresses.erase(std::remove(addresses.begin(), addresses.end(), user), addresses.end());
        addresses.insert(addresses.begin(), user);
        if (addresses.size() > 8) addresses.resize(8);
        if (addressFile.empty()) return;
        std::string tmp = addressFile + ".tmp";
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        FILE *out = fd >= 0 ? fdopen(fd, "w") : nullptr;
        if (!out) { if (fd >= 0) close(fd); return; }
        for (const std::string &a : addresses) fprintf(out, "%s\n", a.c_str());
        fclose(out);
        rename(tmp.c_str(), addressFile.c_str());
    }

    // ---- MSNP sign-in (Tweener, MSNP8) ----

    // Passport 1.4 login: the ticket for `challenge` ("lc=...,id=...,..."), or "" with `why`.
    std::string tweener(const std::string &user, const std::string &password, const std::string &challenge, std::string *why, bool *badPassword) {
        std::string nexus = cfg.nexusUrl.empty() ? std::string("https://") + kEscargotServer + "/rdr/pprdr.asp" : cfg.nexusUrl;
        HttpMsg r;
        if (!httpGet(nexus, "", r, why)) return "";
        std::string urls = r.header("PassportURLs");
        size_t d = urls.find("DALogin=");
        if (d == std::string::npos) { if (why) *why = "nexus gave no DALogin (HTTP " + std::to_string(statusCode(r)) + ")"; return ""; }
        std::string da = urls.substr(d + 8);
        da = da.substr(0, da.find(','));
        if (da.find("://") == std::string::npos) da = "https://" + da;
        LOGI("mws: Passport login at %s", da.c_str());
        std::string auth = "Authorization: Passport1.4 OrgVerb=GET,OrgURL=http%3A%2F%2Fmessenger%2Emsn%2Ecom,sign-in=" + urlEncode(user) +
                           ",pwd=" + urlEncode(password) + "," + challenge + "\r\n";
        for (int hop = 0; hop < 3; hop++) {
            HttpMsg lr;
            if (!httpGet(da, auth, lr, why)) return "";
            int code = statusCode(lr);
            if (code == 302) {
                std::string loc = lr.header("Location");
                if (loc.empty()) break;
                da = loc;
                continue;
            }
            std::string info = lr.header("Authentication-Info");
            size_t f = info.find("from-PP='");
            if (code == 200 && f != std::string::npos) {
                size_t e = info.find('\'', f + 9);
                return info.substr(f + 9, e == std::string::npos ? std::string::npos : e - f - 9);
            }
            if (code == 401) {
                if (badPassword) *badPassword = true;
                if (why) *why = "the Passport login refused the e-mail address or password";
                return "";
            }
            if (why) *why = "Passport login answered HTTP " + std::to_string(code);
            return "";
        }
        if (why) *why = "Passport login redirected too often";
        return "";
    }

    // Signs `user` in; on success the session's NS connection is open and online.
    bool msnpLogin(Session &s, const std::string &password, std::string *why, bool *badPassword) {
        std::string host = cfg.msnpHost.empty() ? kEscargotServer : cfg.msnpHost;
        uint16_t port = cfg.msnpPort ? cfg.msnpPort : 1863;
        s.ns.reset(new Msnp);
        Msnp &ns = *s.ns;
        std::string line;
        for (int redirect = 0; redirect < 3; redirect++) {
            LOGI("mws: MSNP to %s:%u for the sign-in", host.c_str(), port);
            if (!ns.open(host, port, why)) return false;
            ns.send("VER", "MSNP8 CVR0");
            if (!ns.next(line, nullptr, 15000) || line.compare(0, 3, "VER") != 0 || line.find("MSNP8") == std::string::npos) {
                if (why) *why = "the server does not speak MSNP8 (" + line + ")";
                return false;
            }
            ns.send("CVR", "0x0409 winnt 5.1 i386 MSNMSGR 6.2.0137 MSMSGS " + s.user);
            if (!ns.next(line, nullptr, 15000) || line.compare(0, 3, "CVR") != 0) { if (why) *why = "CVR refused: " + line; return false; }
            ns.send("USR", "TWN I " + s.user);
            if (!ns.next(line, nullptr, 15000)) { if (why) *why = "no answer to USR"; return false; }
            std::vector<std::string> w = words(line);
            if (w.size() >= 4 && w[0] == "XFR" && w[2] == "NS") {
                std::string hp = w[3];
                size_t c = hp.find(':');
                host = hp.substr(0, c);
                port = c == std::string::npos ? 1863 : (uint16_t)atoi(hp.c_str() + c + 1);
                LOGI("mws: MSNP redirected to %s:%u", host.c_str(), port);
                continue;
            }
            if (w.size() >= 5 && w[0] == "USR" && w[2] == "TWN" && w[3] == "S") {
                LOGI("mws: MSNP asks for a Passport ticket (Tweener)");
                std::string ticket = tweener(s.user, password, w[4], why, badPassword);
                if (ticket.empty()) return false;
                ns.send("USR", "TWN S " + ticket);
                if (!ns.next(line, nullptr, 20000)) { if (why) *why = "no answer to the ticket"; return false; }
                w = words(line);
                if (w.size() >= 4 && w[0] == "USR" && w[2] == "OK") {
                    s.nick = w.size() >= 5 ? urlDecode(w[4]) : s.user;
                    LOGI("mws: MSNP signed in");
                    return true;
                }
                if (badPassword && w.size() && w[0] == "911") *badPassword = true;
                if (why) *why = "the server refused the ticket (" + (w.empty() ? std::string("?") : w[0]) + ")";
                return false;
            }
            if (badPassword && !w.empty() && w[0] == "911") *badPassword = true;
            if (why) *why = "unexpected answer to USR: " + (w.empty() ? std::string("?") : w[0]);
            return false;
        }
        if (why) *why = "too many redirects";
        return false;
    }

    // After the sign-in: contact list, online, then keep the connection alive (pings, challenges).
    void startPump(std::shared_ptr<Session> s) {
        Msnp &ns = *s->ns;
        std::string line, payload;
        ns.send("SYN", "0");
        uint64_t end = nowMs() + 20000;
        int listed = -1;
        while (nowMs() < end && ns.next(line, &payload, 5000)) {
            std::vector<std::string> w = words(line);
            if (w.empty()) continue;
            if (w[0] == "SYN" && w.size() >= 5) { s->contacts = atoi(w[3].c_str()); s->groups = atoi(w[4].c_str()); listed = 0; }
            else if (w[0] == "LST") listed++;
            if (listed >= s->contacts && listed >= 0) break;
        }
        LOGI("mws: contact list: %d contacts, %d groups", s->contacts, s->groups);
        ns.send("CHG", "NLN 0");
        std::thread([s] {
            Msnp &ns = *s->ns;
            uint64_t lastPing = nowMs();
            std::string line, payload;
            while (!s->stop) {
                if (nowMs() - lastPing > 45000) { ns.send("PNG", ""); lastPing = nowMs(); }
                if (!ns.next(line, &payload, 1000)) {
                    if (ns.closed) {
                        LOGW("mws: MSNP connection closed by the server");
                        break;
                    }
                    if (nowMs() - s->lastUse > 15 * 60000) { LOGI("mws: no client activity for 15 min: signing out"); ns.send("OUT", ""); break; }
                    continue;
                }
                std::vector<std::string> w = words(line);
                if (w.empty()) continue;
                if (w[0] == "CHL" && w.size() >= 3) {
                    int t = ++ns.trid;
                    std::string qry = "QRY " + std::to_string(t) + " " + kMsnpClientId + " 32\r\n" + md5hex(w[2] + kMsnpClientKey);
                    sendAll(ns.fd, qry);
                } else if (w[0] == "ILN" || w[0] == "NLN") {
                    s->online++;
                } else if (w[0] == "CHG") {
                    LOGI("mws: MSNP presence is now %s", w.size() > 2 ? w[2].c_str() : "?");
                } else if (w[0] == "RNG") {
                    LOGI("mws: MSNP: a contact opened a conversation (not relayed to the phone yet)");
                } else if (w[0] == "OUT") {
                    LOGI("mws: MSNP: server signed us out (%s)", w.size() > 1 ? w[1].c_str() : "");
                    break;
                }
            }
        }).detach();
    }

    // ---- What the guest's client gets ----

    std::string policyXml() {
        // The literal prefixes matter: WLMService queries the document with XSL patterns written with
        // these prefixes (wsp:, sp:, wst:, wsa:, mws:).
        std::string base = "https://http.mws.mobile.live.com";
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
               "<wsp:Policy xmlns:wsp=\"http://schemas.xmlsoap.org/ws/2004/09/policy\""
               " xmlns:sp=\"http://schemas.xmlsoap.org/ws/2005/07/securitypolicy\""
               " xmlns:wst=\"http://schemas.xmlsoap.org/ws/2005/02/trust\""
               " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\""
               " xmlns:mws=\"http://schemas.live.com/mws/2006/10/core\">"
               "<mws:metadatavalidity><mws:Expiry>" + isoTime(time(nullptr) + 30 * 86400) + "</mws:Expiry></mws:metadatavalidity>"
               "<mws:Dictionaries></mws:Dictionaries>"
               "<sp:SymmetricBinding><wsp:Policy><sp:ProtectionToken><wsp:Policy><wsp:ExactlyOne>"
               "<sp:IssuedToken>"
               "<sp:Issuer><wsa:Address>" + base + "/RST.srf</wsa:Address>"
               "<wsa:ReferenceProperties>"
               "<mws:LiveIdSignUpUrl>https://account.escargot.chat/register/</mws:LiveIdSignUpUrl>"
               "<mws:Privacy>https://escargot.chat/legal/privacy/</mws:Privacy>"
               "<mws:TOU>https://escargot.chat/legal/rules/</mws:TOU>"
               "<mws:RPSCluster>mobile.live.com</mws:RPSCluster>"
               "</wsa:ReferenceProperties></sp:Issuer>"
               "<sp:RequestSecurityTokenTemplate><wst:TokenType>urn:passport:compact</wst:TokenType>"
               "<wsp:PolicyReference URI=\"MBI_SSL\"/></sp:RequestSecurityTokenTemplate>"
               "</sp:IssuedToken>"
               "</wsp:ExactlyOne></wsp:Policy></sp:ProtectionToken></wsp:Policy></sp:SymmetricBinding>"
               "</wsp:Policy>\r\n";
    }

    std::string rstFault(const std::string &code, const std::string &text) {
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
               "<S:Envelope xmlns:S=\"http://www.w3.org/2003/05/soap-envelope\""
               " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\""
               " xmlns:psf=\"http://schemas.microsoft.com/Passport/SoapServices/SOAPFault\">"
               "<S:Header><psf:pp><psf:serverVersion>1</psf:serverVersion><psf:authstate>0x80048800</psf:authstate>"
               "<psf:reqstatus>" + code + "</psf:reqstatus><psf:serverInfo>PocketStylus</psf:serverInfo></psf:pp></S:Header>"
               "<S:Body><S:Fault><S:Code><S:Value>S:Sender</S:Value><S:Subcode><S:Value>wsse:FailedAuthentication</S:Value></S:Subcode></S:Code>"
               "<S:Reason><S:Text xml:lang=\"en-US\">" + xmlEscape(text) + "</S:Text></S:Reason>"
               "<S:Detail><psf:error><psf:value>" + code + "</psf:value><psf:internalerror><psf:code>" + code +
               "</psf:code><psf:text>" + xmlEscape(text) + "</psf:text></psf:internalerror></psf:error></S:Detail>"
               "</S:Fault></S:Body></S:Envelope>";
    }

    std::string rstResponse(const std::string &requestBody, const Session &s) {
        time_t now = time(nullptr);
        std::string rstrs;
        int n = 0;
        for (const std::string &rst : elements(requestBody, "RequestSecurityToken")) {
            std::string addr = xmlUnescape(element(rst, "Address"));
            n++;
            rstrs += "<wst:RequestSecurityTokenResponse>"
                     "<wst:TokenType>urn:passport:compact</wst:TokenType>"
                     "<wsp:AppliesTo><wsa:EndpointReference><wsa:Address>" + xmlEscape(addr) + "</wsa:Address></wsa:EndpointReference></wsp:AppliesTo>"
                     "<wst:LifeTime><wsu:Created>" + isoTime(now) + "</wsu:Created><wsu:Expires>" + isoTime(now + 8 * 3600) + "</wsu:Expires></wst:LifeTime>"
                     "<wst:RequestedSecurityToken><wsse:BinarySecurityToken Id=\"Compact" + std::to_string(n) + "\">" + xmlEscape(s.token) +
                     "</wsse:BinarySecurityToken></wst:RequestedSecurityToken>"
                     "<wst:RequestedTokenReference><wsse:KeyIdentifier ValueType=\"urn:passport:compact\"/>"
                     "<wsse:Reference URI=\"#Compact" + std::to_string(n) + "\"/></wst:RequestedTokenReference>"
                     "<wst:RequestedProofToken><wst:BinarySecret>" + s.proofKey + "</wst:BinarySecret></wst:RequestedProofToken>"
                     "</wst:RequestSecurityTokenResponse>";
        }
        return "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
               "<S:Envelope xmlns:S=\"http://www.w3.org/2003/05/soap-envelope\">"
               "<S:Header><psf:pp xmlns:psf=\"http://schemas.microsoft.com/Passport/SoapServices/SOAPFault\">"
               "<psf:serverVersion>1</psf:serverVersion><psf:PUID>" + hex(randomBytes(8)) + "</psf:PUID>"
               "<psf:authstate>0x48803</psf:authstate><psf:reqstatus>0x0</psf:reqstatus>"
               "<psf:serverInfo>PocketStylus</psf:serverInfo></psf:pp></S:Header>"
               "<S:Body><wst:RequestSecurityTokenResponseCollection"
               " xmlns:wst=\"http://schemas.xmlsoap.org/ws/2005/02/trust\""
               " xmlns:wsse=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\""
               " xmlns:wsu=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\""
               " xmlns:wsp=\"http://schemas.xmlsoap.org/ws/2004/09/policy\""
               " xmlns:wsa=\"http://www.w3.org/2005/08/addressing\">" +
               rstrs + "</wst:RequestSecurityTokenResponseCollection></S:Body></S:Envelope>";
    }

    // The client's Live ID sign-in: sign in to MSNP with the same e-mail address and password.
    std::string handleRst(const HttpMsg &req) {
        std::string user = xmlUnescape(element(req.body, "Username"));
        std::string password = xmlUnescape(element(req.body, "Password"));
        // The sign-in page joins the name field and a domain list (hotmail.com, live.com, msn.com). An
        // Escargot address at another domain typed whole into the name field may come as
        // "name@gmail.com@hotmail.com": keep the address the user typed.
        size_t firstAt = user.find('@');
        if (firstAt != std::string::npos && user.find('@', firstAt + 1) != std::string::npos) user.resize(user.rfind('@'));
        user = mapAddress(user);
        if (user.empty()) return httpResponse(500, "Internal Server Error", "application/soap+xml; charset=utf-8",
                                              rstFault("0x80048821", "No user name in the request."), false);
        auto s = std::make_shared<Session>();
        s->user = user;
        s->lastUse = nowMs();
        LOGI("mws: Live ID sign-in from the phone's Windows Live client; signing in to MSNP");
        std::string why;
        bool bad = false;
        bool ok = msnpLogin(*s, password, &why, &bad);
        password.assign(password.size(), '\0');
        if (!ok) {
            LOGW("mws: MSNP sign-in failed: %s", why.c_str());
            // 0x80048821: wrong name or password; 0x80048823: the service could not be reached (see the client's list).
            return httpResponse(500, "Internal Server Error", "application/soap+xml; charset=utf-8",
                                rstFault(bad ? "0x80048821" : "0x80048823", why), false);
        }
        s->token = "t=" + base64(randomBytes(48)) + "&p=";
        s->proofKey = base64(randomBytes(24));
        rememberAddress(user);
        startPump(s);
        std::vector<std::shared_ptr<Session>> old;
        {
            std::lock_guard<std::mutex> lk(mu);
            // The client signs in again after trouble: the user's earlier MSNP sessions go.
            for (auto it = sessions.begin(); it != sessions.end();)
                if (it->second->user == user) { old.push_back(it->second); it = sessions.erase(it); }
                else ++it;
            sessions[s->token] = s;
        }
        for (auto &o : old) { if (o->ns) o->ns->send("OUT", ""); o->stop = true; }
        return httpResponse(200, "OK", "application/soap+xml; charset=utf-8", rstResponse(req.body, *s), false);
    }

    void touchSessions(const std::string &body) {
        std::lock_guard<std::mutex> lk(mu);
        for (auto &kv : sessions)
            if (body.find(xmlEscape(kv.first)) != std::string::npos || body.find(kv.first) != std::string::npos) kv.second->lastUse = nowMs();
    }

    // One HTTP connection (decrypted by the secure web proxy, or plain).
    void serveHttp(int fd, const std::string &host, uint16_t port, std::string buf, const std::string &capPath) {
        auto more = [&] { return recvSome(fd, buf, 120000); };
        HttpMsg req;
        while (readHttp(buf, req, more)) {
            std::string raw = req.first + "\r\n";
            for (auto &h : req.headers) raw += h.first + ": " + h.second + "\r\n";
            raw += "\r\n" + req.body;
            Capture::write(capPath, "guest -> gateway", printable(redact(raw)), raw.size());
            std::vector<std::string> rl = words(req.first);
            std::string method = rl.size() > 0 ? rl[0] : "", path = rl.size() > 1 ? rl[1] : "";
            bool close = lower(req.header("Connection")) == "close";
            std::string soapAction = req.header("SOAPAction");
            if (soapAction.empty()) {
                std::string ct = req.header("Content-Type");
                size_t a = ct.find("action=");
                if (a != std::string::npos) soapAction = ct.substr(a + 7);
            }
            LOGI("mws: %s %s%s %s%s%s", method.c_str(), host.c_str(), path.c_str(), req.body.empty() ? "" : "(",
                 req.body.empty() ? "" : (std::to_string(req.body.size()) + " bytes" + (soapAction.empty() ? "" : ", " + soapAction) + ")").c_str(), "");
            std::string resp;
            std::string lp = lower(path);
            if (lp.find("/securitypolicy/") != std::string::npos) {
                resp = httpResponse(200, "OK", "text/xml; charset=utf-8", policyXml(), close);
            } else if (method == "POST" && req.body.find("RequestSecurityToken") != std::string::npos) {
                resp = handleRst(req);
            } else {
                touchSessions(req.body);
                // Not implemented yet: answer like an unavailable service, so the client says so.
                LOGW("mws: not implemented yet: %s %s (captured)", method.c_str(), path.c_str());
                resp = httpResponse(503, "Service Unavailable", "text/plain", "PocketStylus: this Windows Live service is not available yet.\r\n", close);
            }
            Capture::write(capPath, "gateway -> guest", printable(redact(resp)), resp.size());
            if (!sendAll(fd, resp) || close) break;
        }
    }

    // A connection that isn't HTTP (the push channel on tcp.mws.mobile.live.com:50001): capture only.
    void serveRaw(int fd, const std::string &host, uint16_t port, std::string buf, const std::string &capPath) {
        size_t logged = 0;
        uint64_t start = nowMs();
        do {
            if (buf.size() > logged) {
                std::string chunk = buf.substr(logged);
                LOGI("mws: %s:%u sent %zu bytes: %s", host.c_str(), port, chunk.size(), hex(chunk.substr(0, 32)).c_str());
                Capture::write(capPath, "guest -> gateway (raw)", hexDump(chunk), chunk.size());
                logged = buf.size();
            }
            if (buf.size() > (1u << 20)) break;
        } while (recvSome(fd, buf, 300000) && nowMs() - start < 1800000);
        LOGI("mws: %s:%u closed after %zu bytes", host.c_str(), port, buf.size());
    }

    // ---- The push channel (tcp.mws.mobile.live.com:50001): framing in mws_pdu below ----
    void servePush(int fd, std::string buf, const std::string &capPath);

    void serveConn(int fd, uint16_t port, std::string host, bool tls) {
        std::string buf;
        recvSome(fd, buf, 60000);
        bool http = buf.compare(0, 4, "GET ") == 0 || buf.compare(0, 5, "POST ") == 0 || buf.compare(0, 5, "HEAD ") == 0 ||
                    buf.compare(0, 4, "PUT ") == 0 || buf.compare(0, 8, "OPTIONS ") == 0;
        std::string capPath = cap.open((host.empty() ? std::string("10.0.2.4") : host) + "-" + std::to_string(port) + (tls ? "-tls" : ""));
        LOGI("mws: connection to %s port %u%s%s", host.empty() ? "10.0.2.4" : host.c_str(), port, tls ? " (TLS, decrypted)" : "",
             http ? "" : buf.empty() ? " (silent)" : " (not HTTP)");
        if (http) serveHttp(fd, host, port, std::move(buf), capPath);
        else if (!buf.empty() && (uint8_t)buf[0] == 0x30) servePush(fd, std::move(buf), capPath);
        else serveRaw(fd, host, port, std::move(buf), capPath);
        close(fd);
    }
};

// The push channel. The client opens it right after its Live ID sign-in with a connect PDU (type 1)
// and waits for the answer before anything else; without one it gives up after ~15 s.
void MwsGateway::Impl::servePush(int fd, std::string buf, const std::string &capPath) {
    std::string session;  // the session id this connection was given (option 2)
    std::map<uint16_t, std::string> partial;  // data PDUs still missing fragments, by message id
    uint32_t sent = 0;
    auto send = [&](MwsPdu p) {
        p.seq = p.type == 0x10 ? ++sent : 0;
        std::string w = buildMwsPdu(p);
        LOGI("mws: push channel -> %s", describeMwsPdu(p).c_str());
        Capture::write(capPath, "gateway -> guest (push) " + describeMwsPdu(p), hexDump(w), w.size());
        return sendAll(fd, w);
    };
    uint64_t start = nowMs();
    for (;;) {
        MwsPdu p;
        long used = parseMwsPdu(buf, p);
        if (used < 0) {
            LOGW("mws: push channel: not a PDU (%s)", hex(buf.substr(0, 16)).c_str());
            Capture::write(capPath, "guest -> gateway (raw, not a PDU)", hexDump(buf), buf.size());
            return;
        }
        if (used == 0) {
            int r = recvWait(fd, buf, 60000);
            if (r < 0 || nowMs() - start > 6 * 3600000ull) break;
            continue;
        }
        std::string wire = buf.substr(0, (size_t)used);
        buf.erase(0, (size_t)used);
        LOGI("mws: push channel <- %s", describeMwsPdu(p).c_str());
        Capture::write(capPath, "guest -> gateway (push) " + describeMwsPdu(p), hexDump(wire), wire.size());
        switch (p.type) {
            case 0x01:    // connect: a new session
            case 0x02: {  // reconnect with the session (option 2) it was given
                const std::string *o2 = p.opt(2);
                if (p.type == 0x02 && o2 && o2->size() >= 4) session = o2->substr(4, (uint8_t)(*o2)[3]);
                if (session.empty()) session = hex(randomBytes(5));  // 10 characters keep the option 4-byte aligned
                MwsPdu a;
                a.type = 0x82;
                std::string o;
                o += '\x01';
                o += '\0';
                o += '\x05';  // sub-item 5: the session id
                o += (char)session.size();
                o += session;
                a.opts.push_back({2, o});
                {
                    std::lock_guard<std::mutex> lk(mu);
                    for (auto &kv : sessions) kv.second->lastUse = nowMs();
                }
                if (!send(a)) return;
                break;
            }
            case 0x04: {  // heartbeat: answer with the TTL it proposed (option 5, seconds)
                MwsPdu a;
                a.type = 0x84;
                const std::string *o5 = p.opt(5);
                a.opts.push_back({5, o5 && o5->size() == 2 ? *o5 : std::string("\x58\x02", 2)});  // 600 s
                {
                    std::lock_guard<std::mutex> lk(mu);
                    for (auto &kv : sessions) kv.second->lastUse = nowMs();
                }
                if (!send(a)) return;
                break;
            }
            case 0x03: {  // the client ends the session
                MwsPdu a;
                a.type = 0x83;
                send(a);
                return;
            }
            case 0x10: {  // data from the client: collect the fragments of each message
                std::string &m = partial[p.id];
                m += p.payload;
                if (!(p.flags & 1)) {
                    LOGI("mws: push channel: message %u from the client, %zu bytes (not served yet)", p.id, m.size());
                    bool text = std::count_if(m.begin(), m.end(), [](char ch) { return (uint8_t)ch >= 0x20 || ch == '\r' || ch == '\n' || ch == '\t'; }) > (long)m.size() * 9 / 10;
                    Capture::write(capPath, "message " + std::to_string(p.id) + " from the client", text ? printable(redact(m)) : hexDump(m), m.size());
                    partial.erase(p.id);
                }
                break;
            }
            default:  // 0x00 data acks and anything unknown: logged only
                break;
        }
    }
    LOGI("mws: push channel closed%s%s", session.empty() ? "" : ", session ", session.c_str());
}

MwsGateway::MwsGateway(Config cfg) : impl(std::make_shared<Impl>(std::move(cfg))) {
    LOGI("mws: Windows Live Mobile gateway ready (MSNP at %s:%u)%s%s", impl->cfg.msnpHost.empty() ? kEscargotServer : impl->cfg.msnpHost.c_str(),
         impl->cfg.msnpPort ? impl->cfg.msnpPort : 1863, impl->cap.on() ? ", capture to " : "", impl->cfg.captureDir.c_str());
}

MwsGateway::~MwsGateway() {}

void MwsGateway::setCaptureDir(const std::string &dir) { impl->cap.setDir(dir); }
void MwsGateway::setStateDir(const std::string &dir) { impl->setStateDir(dir); }

void MwsGateway::serve(int fd, uint16_t port, const std::string &host, bool tls) {
    std::shared_ptr<Impl> im = impl;
    std::thread([im, fd, port, host, tls] { im->serveConn(fd, port, host, tls); }).detach();
}

std::vector<HostRule> MwsGateway::hostRules() {
    // The gateway's own names. Pocket MSN's session servers (WM 5.0) come here too, only to be captured
    // (their protocol isn't served): with the setting on, a password typed into Pocket MSN stays on the
    // phone instead of going to Microsoft's still-resolving session.mobile.msn.com.
    return {HostRule{".mws.mobile.live.com", kLocalServiceAddr}, HostRule{"session.mobile.msn.com", kLocalServiceAddr},
            HostRule{"services.mobile.msn.com", kLocalServiceAddr}};
}

bool MwsGateway::testLogin(const std::string &user, const std::string &password, std::string *why) {
    auto s = std::make_shared<Impl::Session>();
    s->user = user;
    s->lastUse = nowMs();
    bool bad = false;
    if (!impl->msnpLogin(*s, password, why, &bad)) return false;
    impl->startPump(s);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    LOGI("mws: test sign-in: %d contacts, %d online so far", s->contacts, s->online);
    s->ns->send("OUT", "");
    return true;
}

bool MwsGateway::probe(std::string *report) {
    std::string host = impl->cfg.msnpHost.empty() ? kEscargotServer : impl->cfg.msnpHost;
    uint16_t port = impl->cfg.msnpPort ? impl->cfg.msnpPort : 1863;
    std::string out, why, line;
    Msnp ns;
    bool ok = ns.open(host, port, &why);
    if (ok) {
        ns.send("VER", "MSNP8 CVR0");
        ok = ns.next(line, nullptr, 15000);
        out += "VER -> " + (ok ? line : std::string("(no answer)")) + "\n";
        if (ok) {
            ns.send("CVR", "0x0409 winnt 5.1 i386 MSNMSGR 6.2.0137 MSMSGS probe@example.invalid");
            ok = ns.next(line, nullptr, 15000);
            out += "CVR -> " + (ok ? line : std::string("(no answer)")) + "\n";
        }
        ns.send("OUT", "");
    } else out += "MSNP: " + why + "\n";
    std::string nexus = impl->cfg.nexusUrl.empty() ? std::string("https://") + kEscargotServer + "/rdr/pprdr.asp" : impl->cfg.nexusUrl;
    HttpMsg r;
    if (httpGet(nexus, "", r, &why)) {
        std::string urls = r.header("PassportURLs");
        size_t d = urls.find("DALogin=");
        out += "nexus -> HTTP " + std::to_string(statusCode(r)) + ", DALogin " +
               (d == std::string::npos ? std::string("missing") : urls.substr(d + 8, urls.find(',', d) - d - 8)) + "\n";
        ok = ok && d != std::string::npos;
    } else {
        out += "nexus: " + why + "\n";
        ok = false;
    }
    if (report) *report = out;
    return ok;
}

std::string MwsGateway::selfTestRequest(const std::string &request) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return "";
    serve(sv[1], 443, "http.mws.mobile.live.com", true);
    sendAll(sv[0], request);
    std::string buf;
    HttpMsg m;
    std::string out;
    if (readHttp(buf, m, [&] { return recvSome(sv[0], buf, 60000); }, true)) {
        out = m.first + "\r\n";
        for (auto &h : m.headers) out += h.first + ": " + h.second + "\r\n";
        out += "\r\n" + m.body;
    }
    close(sv[0]);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Fake MSNP8 server + Passport nexus/login for tests (no real account, nothing leaves the phone).

namespace {

int listenLocal(uint16_t &port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(0x7F000001);
    a.sin_port = 0;
    if (fd < 0 || bind(fd, (sockaddr *)&a, sizeof a) != 0 || listen(fd, 8) != 0) { if (fd >= 0) close(fd); return -1; }
    socklen_t l = sizeof a;
    getsockname(fd, (sockaddr *)&a, &l);
    port = ntohs(a.sin_port);
    return fd;
}

constexpr const char *kFakeTicket = "t=fake.ticket&p=fake.profile";

void fakeNs(int c) {
    std::string buf, user;
    std::string chl = "15570131571988941333";
    for (;;) {
        size_t e;
        while ((e = buf.find("\r\n")) == std::string::npos)
            if (!recvSome(c, buf, 60000)) { close(c); return; }
        std::string line = buf.substr(0, e);
        buf.erase(0, e + 2);
        std::vector<std::string> w = words(line);
        if (w.size() < 2) continue;
        LOGI("fake-msnp: got %s", redact(line).c_str());
        const std::string &t = w[1];
        std::string out;
        if (w[0] == "VER") out = "VER " + t + " MSNP8 CVR0\r\n";
        else if (w[0] == "CVR") out = "CVR " + t + " 6.2.0137 6.2.0137 6.2.0137 http://fake.invalid http://fake.invalid\r\n";
        else if (w[0] == "USR" && w.size() >= 5 && w[3] == "I") {
            user = w[4];
            out = "USR " + t + " TWN S lc=1033,id=507,tw=40,fs=1,ru=http%3A%2F%2Fmessenger%2Emsn%2Ecom,ct=1,kpp=1,kv=5,ver=2.1.0173.1,tpf=0123456789abcdef\r\n";
        } else if (w[0] == "USR" && w.size() >= 5 && w[3] == "S") {
            out = w[4] == kFakeTicket ? "USR " + t + " OK " + user + " Fake%20User 1 0\r\n" : "911 " + t + "\r\n";
        } else if (w[0] == "SYN") {
            out = "SYN " + t + " 1 1 1\r\nGTC A\r\nBLP AL\r\nLSG 0 Other%20Contacts 0\r\nLST friend@fake.invalid Friend 11 0\r\n";
        } else if (w[0] == "CHG") {
            out = "CHG " + t + " " + (w.size() > 2 ? w[2] : "NLN") + " 0\r\nILN " + t + " NLN friend@fake.invalid Friend 0\r\nCHL 0 " + chl + "\r\n";
        } else if (w[0] == "QRY") {
            size_t need = 32;
            while (buf.size() < need) if (!recvSome(c, buf, 10000)) break;
            std::string got = buf.substr(0, need);
            buf.erase(0, std::min(need, buf.size()));
            bool ok = got == md5hex(chl + kMsnpClientKey);
            LOGI("fake-msnp: challenge answer %s", ok ? "correct" : "WRONG");
            out = ok ? "QRY " + t + "\r\n" : "540 " + t + "\r\n";
        } else if (w[0] == "PNG") out = "QNG 50\r\n";
        else if (w[0] == "OUT") { close(c); return; }
        if (!out.empty()) sendAll(c, out);
    }
}

void fakeHttp(int c, uint16_t httpPort) {
    std::string buf;
    HttpMsg req;
    if (!readHttp(buf, req, [&] { return recvSome(c, buf, 10000); })) { close(c); return; }
    std::vector<std::string> rl = words(req.first);
    std::string path = rl.size() > 1 ? rl[1] : "";
    std::string resp;
    if (path.find("/rdr/pprdr.asp") == 0) {
        // Escargot spells this header "Passporturls": the gateway must not care.
        resp = "HTTP/1.1 200 OK\r\nPassporturls: DALogin=http://127.0.0.1:" + std::to_string(httpPort) +
               "/login2.srf,DAReg=x,DARealm=Passport.Net\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else if (path.find("/login2.srf") == 0) {
        std::string a = req.header("Authorization");
        bool ok = a.find("Passport1.4") == 0 && a.find("sign-in=") != std::string::npos && a.find("pwd=") != std::string::npos &&
                  a.find("lc=1033") != std::string::npos;
        LOGI("fake-msnp: Passport login %s", ok ? "with name, password and challenge: accepted" : "malformed: refused");
        resp = ok ? std::string("HTTP/1.1 200 OK\r\nAuthentication-Info: Passport1.4 da-status=success,tname=MSPAuth,tname=MSPProf,tname=MSPSec,from-PP='") +
                        kFakeTicket + "',ru=http://messenger.msn.com\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
                  : "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Passport1.4 da-status=failed\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    } else {
        resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    }
    sendAll(c, resp);
    close(c);
}

}  // namespace

uint16_t startFakeMsnpServer(std::string *nexusUrl) {
    uint16_t nsPort = 0, httpPort = 0;
    int ns = listenLocal(nsPort), http = listenLocal(httpPort);
    if (ns < 0 || http < 0) return 0;
    std::thread([ns] { for (;;) { int c = accept(ns, nullptr, nullptr); if (c >= 0) std::thread(fakeNs, c).detach(); } }).detach();
    std::thread([http, httpPort] { for (;;) { int c = accept(http, nullptr, nullptr); if (c >= 0) std::thread(fakeHttp, c, httpPort).detach(); } }).detach();
    if (nexusUrl) *nexusUrl = "http://127.0.0.1:" + std::to_string(httpPort) + "/rdr/pprdr.asp";
    LOGI("fake-msnp: MSNP8 on 127.0.0.1:%u, nexus on port %u", nsPort, httpPort);
    return nsPort;
}

}  // namespace wm

#else  // !WM_HAVE_TLS: the gateway needs the secure web proxy (its client talks HTTPS)

#include <unistd.h>

namespace wm {

struct MwsGateway::Impl {};
MwsGateway::MwsGateway(Config) {}
MwsGateway::~MwsGateway() {}
void MwsGateway::setCaptureDir(const std::string &) {}
void MwsGateway::setStateDir(const std::string &) {}
void MwsGateway::serve(int fd, uint16_t, const std::string &, bool) { if (fd >= 0) ::close(fd); }
std::vector<HostRule> MwsGateway::hostRules() { return {}; }
bool MwsGateway::testLogin(const std::string &, const std::string &, std::string *why) {
    if (why) *why = "built without TLS support";
    return false;
}
bool MwsGateway::probe(std::string *report) {
    if (report) *report = "built without TLS support";
    return false;
}
std::string MwsGateway::selfTestRequest(const std::string &) { return ""; }
uint16_t startFakeMsnpServer(std::string *) { return 0; }

}  // namespace wm

#endif
