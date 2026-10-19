// Windows Marketplace stand-in. See market.h and docs/research-marketplace.md.
#include "market.h"
#include "../util/log.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <thread>
#include <limits.h>
#include <mutex>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace wm {

namespace {

std::string lower(std::string s) {
    for (char &ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

std::string trim(const std::string &s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) b--;
    return s.substr(a, b - a);
}

std::string xmlEscape(const std::string &s) {
    std::string o;
    o.reserve(s.size());
    for (char ch : s) {
        if (ch == '&') o += "&amp;";
        else if (ch == '<') o += "&lt;";
        else if (ch == '>') o += "&gt;";
        else if (ch == '"') o += "&quot;";
        else if ((unsigned char)ch >= 32 || ch == '\n' || ch == '\t') o += ch;
    }
    return o;
}

std::string htmlEscape(const std::string &s) { return xmlEscape(s); }

std::string urlEncode(const std::string &s) {
    static const char *hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.') o += (char)c;
        else {
            o += '%';
            o += hex[c >> 4];
            o += hex[c & 15];
        }
    }
    return o;
}

std::string urlDecode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            auto nyb = [](char c) {
                c = (char)tolower((unsigned char)c);
                return c <= '9' ? c - '0' : c - 'a' + 10;
            };
            o += (char)((nyb(s[i + 1]) << 4) | nyb(s[i + 2]));
            i += 2;
        } else if (s[i] == '+') {
            o += ' ';
        } else {
            o += s[i];
        }
    }
    return o;
}

bool endsWith(const std::string &s, const char *suf) {
    size_t n = strlen(suf);
    return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}

bool sendAll(int fd, const char *p, size_t n) {
    size_t done = 0;
    while (done < n) {
        ssize_t w = send(fd, p + done, n - done, MSG_NOSIGNAL);
        if (w > 0) {
            done += (size_t)w;
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EINTR)) {
            pollfd pol{fd, POLLOUT, 0};
            if (poll(&pol, 1, 10000) <= 0) return false;
            continue;
        }
        return false;
    }
    return true;
}

bool sendAll(int fd, const std::string &s) { return sendAll(fd, s.data(), s.size()); }

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

std::string headerValue(const std::string &head, const char *name) {
    std::string want = lower(name);
    size_t i = 0;
    while (i < head.size()) {
        size_t end = head.find("\r\n", i);
        if (end == std::string::npos) end = head.size();
        std::string line = head.substr(i, end - i);
        auto c = line.find(':');
        if (c != std::string::npos && lower(trim(line.substr(0, c))) == want) {
            return trim(line.substr(c + 1));
        }
        if (end == head.size()) break;
        i = end + 2;
    }
    return {};
}

size_t contentLength(const std::string &head) {
    std::string v = headerValue(head, "content-length");
    if (v.empty()) return 0;
    return (size_t)strtoul(v.c_str(), nullptr, 10);
}

std::string hostOnly(std::string h) {
    h = trim(h);
    auto colon = h.rfind(':');
    if (colon != std::string::npos && h.find(']') == std::string::npos) h.resize(colon);
    while (!h.empty() && h.back() == '.') h.pop_back();
    return lower(h);
}

std::string queryParam(const std::string &query, const char *key) {
    std::string want = key;
    size_t i = 0;
    while (i < query.size()) {
        size_t amp = query.find('&', i);
        if (amp == std::string::npos) amp = query.size();
        std::string part = query.substr(i, amp - i);
        auto eq = part.find('=');
        std::string k = urlDecode(eq == std::string::npos ? part : part.substr(0, eq));
        if (lower(k) == want) return urlDecode(eq == std::string::npos ? std::string() : part.substr(eq + 1));
        i = amp + 1;
    }
    return {};
}

// A version the client reported (digits and dots only), else 0.0.0.0 so a comparison finds no update.
std::string clientVersion(const std::string &s) {
    for (size_t i = 0; i < s.size(); i++) {
        if (!isdigit((unsigned char)s[i])) continue;
        size_t j = i;
        bool dot = false;
        while (j < s.size() && j - i < 32 && (isdigit((unsigned char)s[j]) || s[j] == '.')) {
            if (s[j] == '.') dot = true;
            j++;
        }
        if (dot && j > i + 2 && isdigit((unsigned char)s[j - 1]) && (j == s.size() || !isdigit((unsigned char)s[j])))
            return s.substr(i, j - i);
        i = j;
    }
    return "0.0.0.0";
}

bool isGuid(const std::string &s) {
    if (s.size() != 36) return false;
    for (size_t i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return false;
        } else if (!isxdigit((unsigned char)s[i])) {
            return false;
        }
    }
    return true;
}

std::string findGuid(const std::string &path) {
    for (size_t i = 0; i + 36 <= path.size(); i++) {
        std::string g = path.substr(i, 36);
        if (isGuid(g)) return lower(g);
    }
    return {};
}

std::string appId(const std::string &rel) {
    auto mix = [](const std::string &s, uint64_t h) {
        for (unsigned char c : s) {
            h ^= c;
            h *= 0x100000001B3ull;
        }
        return h;
    };
    uint64_t a = mix(rel, 0xcbf29ce484222325ull);
    uint64_t b = mix(rel, 0x84222325cbf29ce4ull);
    char out[40];
    snprintf(out, sizeof out, "%08x-%04x-%04x-%04x-%012llx", (unsigned)(a >> 32), (unsigned)((a >> 16) & 0xffff),
             (unsigned)(a & 0xffff), (unsigned)((b >> 48) & 0xffff), (unsigned long long)(b & 0xffffffffffffull));
    return out;
}

bool pathInside(const std::string &root, const std::string &path, std::string *realOut) {
    char r[PATH_MAX], p[PATH_MAX];
    if (!realpath(root.c_str(), r) || !realpath(path.c_str(), p)) return false;
    size_t n = strlen(r);
    if (strncmp(p, r, n) != 0) return false;
    if (p[n] != 0 && p[n] != '/') return false;
    if (realOut) *realOut = p;
    return true;
}

struct App {
    std::string id, num, title, publisher, version, summary, fileName, rel, cabPath, iconPath, added;
    unsigned long long bytes = 0;
};

std::string isoTime(time_t t) {
    struct tm tm;
    if (t <= 0 || !gmtime_r(&t, &tm)) return "2012-01-01T00:00:00Z";
    char buf[40];
    if (!strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm)) return "2012-01-01T00:00:00Z";
    return buf;
}

using Ini = std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>;

Ini readIni(const std::string &path) {
    Ini out;
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return out;
    std::string cur = "*";
    std::vector<std::pair<std::string, std::string>> keys;
    auto flush = [&] {
        if (!keys.empty() || cur != "*") out.push_back({cur, keys});
        keys.clear();
    };
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        std::string s = trim(line);
        if (s.empty() || s[0] == ';' || s[0] == '#') continue;
        if (s.front() == '[' && s.back() == ']') {
            flush();
            cur = lower(trim(s.substr(1, s.size() - 2)));
            continue;
        }
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        keys.push_back({lower(trim(s.substr(0, eq))), trim(s.substr(eq + 1))});
    }
    flush();
    fclose(f);
    return out;
}

const std::vector<std::pair<std::string, std::string>> *iniSection(const Ini &ini, const std::string &name) {
    std::string n = lower(name);
    for (const auto &sec : ini)
        if (sec.first == n) return &sec.second;
    return nullptr;
}

std::string iniGet(const std::vector<std::pair<std::string, std::string>> *sec, const char *key) {
    if (!sec) return {};
    for (const auto &kv : *sec)
        if (kv.first == key) return kv.second;
    return {};
}

void applySection(App &a, const std::vector<std::pair<std::string, std::string>> *sec) {
    if (!sec) return;
    if (std::string v = iniGet(sec, "title"); !v.empty()) a.title = v;
    if (std::string v = iniGet(sec, "publisher"); !v.empty()) a.publisher = v;
    if (std::string v = iniGet(sec, "version"); !v.empty()) a.version = v;
    if (std::string v = iniGet(sec, "summary"); !v.empty()) a.summary = v;
}

std::string stemOf(const std::string &fileName) {
    std::string s = fileName;
    if (endsWith(lower(s), ".cab") && s.size() > 4) s.resize(s.size() - 4);
    for (char &ch : s)
        if (ch == '_' || ch == '-') ch = ' ';
    return s;
}

bool isDir(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::vector<App> scanDir(const std::string &root) {
    std::vector<App> apps;
    if (root.empty() || !isDir(root)) return apps;
    Ini catalog = readIni(root + "/catalog.ini");
    std::vector<std::pair<std::string, std::string>> stack{{root, ""}};
    while (!stack.empty() && apps.size() < 500) {
        auto cur = stack.back();
        stack.pop_back();
        if (std::count(cur.second.begin(), cur.second.end(), '/') > 5) continue;
        DIR *d = opendir(cur.first.c_str());
        if (!d) continue;
        while (dirent *e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            std::string name = e->d_name;
            std::string path = cur.first + "/" + name;
            std::string rel = cur.second.empty() ? name : cur.second + "/" + name;
            if (isDir(path)) {
                stack.push_back({path, rel});
                continue;
            }
            if (!endsWith(lower(name), ".cab")) continue;
            std::string real;
            if (!pathInside(root, path, &real)) continue;
            App a;
            a.rel = rel;
            a.fileName = name;
            a.cabPath = real;
            a.id = appId(lower(rel));
            a.title = stemOf(name);
            struct stat st;
            if (stat(real.c_str(), &st) == 0) {
                a.bytes = (unsigned long long)st.st_size;
                a.added = isoTime(st.st_mtime);
            }
            a.publisher = "On this phone";
            a.version = "1.0";
            std::string stem = name.size() > 4 ? name.substr(0, name.size() - 4) : name;
            std::string icon1 = cur.first + "/" + stem + ".png";
            std::string icon2 = path + ".png";
            if (!pathInside(root, icon1, &a.iconPath) && !pathInside(root, icon2, &a.iconPath)) a.iconPath.clear();
            applySection(a, iniSection(catalog, rel));
            applySection(a, iniSection(catalog, name));
            applySection(a, iniSection(catalog, stem));
            // Name.cab.ini beside the cabinet. Keys may sit at the top or under one section.
            Ini side = readIni(path + ".ini");
            if (const auto *sec = iniSection(side, "*")) applySection(a, sec);
            else if (!side.empty()) applySection(a, &side[0].second);
            if (a.title.empty()) a.title = stemOf(name);
            apps.push_back(std::move(a));
        }
        closedir(d);
    }
    std::stable_sort(apps.begin(), apps.end(), [](const App &x, const App &y) { return lower(x.title) < lower(y.title); });
    for (size_t i = 0; i < apps.size(); i++) apps[i].num = std::to_string(i + 1);
    return apps;
}

const App *findApp(const std::vector<App> &apps, const std::string &id) {
    for (const App &a : apps)
        if (a.id == id) return &a;
    return nullptr;
}

const App *findAny(const std::vector<App> &apps, const std::string &id) {
    if (id.empty()) return nullptr;
    std::string want = lower(id);
    for (const App &a : apps)
        if (a.id == want || a.num == id || lower(a.fileName) == want || lower(a.rel) == want) return &a;
    return nullptr;
}

std::string packageUrl(const App &a) {
    return "https://files.marketplace.windowsmobile.com/" + a.id + "/Package/" + urlEncode(a.fileName);
}

std::string bootstrapXml(const std::string &version, bool soap) {
    std::string inner =
        "<BootstrapResponse xmlns=\"http://marketplace.windowsmobile.com/2009/02\">"
        "<ClientUpdate available=\"false\"/>"
        "<LatestClientVersion>" + xmlEscape(version) + "</LatestClientVersion>"
        "<Catalog>https://client.marketplace.windowsmobile.com/Services/CatalogService/CatalogService.svc</Catalog>"
        "<Files>https://files.marketplace.windowsmobile.com/</Files>"
        "<Store>https://marketplace.windowsmobile.com/</Store>"
        "</BootstrapResponse>";
    if (!soap) return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + inner;
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>" +
           inner + "</s:Body></s:Envelope>";
}

std::string catalogXml(const std::vector<App> &apps, bool soap) {
    std::string inner = "<CatalogResponse xmlns=\"http://marketplace.windowsmobile.com/2009/02\">";
    for (const App &a : apps) {
        inner += "<Application id=\"" + xmlEscape(a.id) + "\">";
        inner += "<Title>" + xmlEscape(a.title) + "</Title>";
        inner += "<Publisher>" + xmlEscape(a.publisher) + "</Publisher>";
        inner += "<Version>" + xmlEscape(a.version) + "</Version>";
        inner += "<Summary>" + xmlEscape(a.summary) + "</Summary>";
        inner += "<Price>0</Price><Currency>USD</Currency>";
        inner += "<Package>" + xmlEscape(packageUrl(a)) + "</Package>";
        if (!a.iconPath.empty())
            inner += "<Icon>https://files.marketplace.windowsmobile.com/" + a.id + "/LargeWebIcon.png</Icon>";
        inner += "</Application>";
    }
    inner += "</CatalogResponse>";
    if (!soap) return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n" + inner;
    return "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
           "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>" +
           inner + "</s:Body></s:Envelope>";
}

std::string pageHtml(const std::vector<App> &apps, const std::string &query, const App *one) {
    std::string h =
        "<!DOCTYPE html><html><head><title>Windows Marketplace</title></head><body>"
        "<h1>Windows Marketplace</h1>"
        "<p>Microsoft's Marketplace service ended in 2012. This phone offers the cabinets in "
        "\\Storage Card\\Marketplace. Install opens the cabinet. The Marketplace program lists the "
        "same cabinets, and it also needs Secure Web Proxy.</p>";
    if (!one) {
        h += "<form action=\"/\" method=\"get\"><input type=\"text\" name=\"q\" value=\"" + htmlEscape(query) +
             "\"><input type=\"submit\" value=\"Search\"></form>";
        if (apps.empty())
            h += query.empty() ? "<p>No cabinets yet. Copy a .cab into that folder.</p>"
                                : "<p>No cabinets match.</p>";
    }
    h += "<table>";
    for (const App &a : apps) {
        h += "<tr><td><b>" + htmlEscape(a.title) + "</b><br>" + htmlEscape(a.publisher);
        if (!a.version.empty()) h += " " + htmlEscape(a.version);
        if (!a.summary.empty()) h += "<br>" + htmlEscape(a.summary);
        h += "<br><a href=\"/pkg/" + a.id + "\">Install</a>";
        if (!one) h += " <a href=\"/app/" + a.id + "\">Details</a>";
        h += "</td></tr>";
    }
    h += "</table></body></html>";
    return h;
}

std::string loginHtml() {
    return "<!DOCTYPE html><html><head><title>Windows Marketplace</title></head><body>"
           "<h1>Signed in on this phone</h1>"
           "<p>This Marketplace does not use a Microsoft account. "
           "<a href=\"https://marketplace.windowsmobile.com/\">Browse installers</a>.</p>"
           "</body></html>";
}

struct Req {
    std::string method, path, query, host, action, contentType, body;
    bool soap = false;
};

struct Reply {
    int code = 200;
    const char *reason = "OK";
    std::string type = "text/html; charset=utf-8";
    std::string body;
    std::string file;
    std::string downloadName;
    std::string extra;
};

bool parseReq(const std::string &raw, Req &req) {
    auto he = raw.find("\r\n\r\n");
    if (he == std::string::npos) return false;
    std::string head = raw.substr(0, he);
    req.body = raw.substr(he + 4);
    auto nl = head.find("\r\n");
    std::string first = nl == std::string::npos ? head : head.substr(0, nl);
    auto sp1 = first.find(' ');
    auto sp2 = sp1 == std::string::npos ? std::string::npos : first.find(' ', sp1 + 1);
    if (sp1 == std::string::npos) return false;
    req.method = first.substr(0, sp1);
    std::string target = sp2 == std::string::npos ? first.substr(sp1 + 1) : first.substr(sp1 + 1, sp2 - sp1 - 1);
    auto q = target.find('?');
    req.path = urlDecode(q == std::string::npos ? target : target.substr(0, q));
    req.query = q == std::string::npos ? std::string() : target.substr(q + 1);
    if (req.path.empty() || req.path[0] != '/') req.path = "/" + req.path;
    if (req.path.find("..") != std::string::npos) return false;
    req.host = hostOnly(headerValue(head, "host"));
    req.action = headerValue(head, "soapaction");
    req.contentType = headerValue(head, "content-type");
    if (!req.action.empty() && req.action.front() == '"') {
        req.action.erase(req.action.begin());
        if (!req.action.empty() && req.action.back() == '"') req.action.pop_back();
    }
    std::string type = lower(headerValue(head, "content-type"));
    std::string bodyL = lower(req.body);
    req.soap = !req.action.empty() || type.find("soap") != std::string::npos || bodyL.find("envelope") != std::string::npos;
    return true;
}

std::vector<App> matching(const std::vector<App> &all, const std::string &q) {
    if (trim(q).empty()) return all;
    std::string want = lower(q);
    std::vector<App> out;
    for (const App &a : all) {
        std::string hay = lower(a.title + " " + a.publisher + " " + a.summary + " " + a.fileName);
        if (hay.find(want) != std::string::npos) out.push_back(a);
    }
    return out;
}

// The built-in Marketplace program (WMMarketplaceFullClient.exe 1.2.1422) calls
// api.marketplace.windowsmobile.com. Element names are the ones that program compares.
// docs/research-marketplace.md
const char *kMarketNs = "Microsoft.WindowsMobile.Service.Marketplace";

std::string xel(const char *name, const std::string &value) {
    return std::string("<") + name + ">" + xmlEscape(value) + "</" + name + ">";
}

std::string xnil(const char *name) { return std::string("<") + name + " i:nil=\"true\"/>"; }

std::string xbool(const char *name, bool v) {
    return std::string("<") + name + ">" + (v ? "true" : "false") + "</" + name + ">";
}

std::string xdoc(const char *root, const std::string &inner) {
    return std::string("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<") + root + " xmlns=\"" + kMarketNs +
           "\" xmlns:i=\"http://www.w3.org/2001/XMLSchema-instance\">" + inner + "</" + root + ">";
}

std::string xok() { return xel("ResponseCode", "Success") + xnil("ResponseMessage"); }

std::vector<std::string> pathSegs(const std::string &path) {
    std::vector<std::string> s;
    size_t i = 0;
    while (i < path.size()) {
        if (path[i] == '/') {
            i++;
            continue;
        }
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        s.push_back(path.substr(i, j - i));
        i = j;
    }
    return s;
}

unsigned queryUInt(const std::string &query, const char *key, unsigned fallback) {
    std::string v = queryParam(query, key);
    if (v.empty()) return fallback;
    return (unsigned)strtoul(v.c_str(), nullptr, 10);
}

std::string iconUrl(const App &a) {
    if (a.iconPath.empty()) return {};
    return "https://files.marketplace.windowsmobile.com/" + a.id + "/LargeWebIcon.png";
}

std::string appSummaryXml(const App &a) {
    std::string icon = iconUrl(a);
    return std::string("<AppSummary>") + xel("CompanyName", a.publisher) + xel("Id", a.num) + xel("InstallName", a.title) +
           xbool("IsFeatured", a.num == "1") + xel("Name", a.title) + xel("Price", "0") + xel("Rating", "0") +
           (icon.empty() ? xnil("SmallImageUrl") : xel("SmallImageUrl", icon)) + xbool("dotNet35", false) + "</AppSummary>";
}

std::string appListXml(const std::vector<App> &apps, unsigned before, unsigned chunk) {
    if (chunk == 0 || chunk > 100) chunk = 25;
    std::string inner;
    unsigned n = 0;
    for (const App &a : apps) {
        unsigned id = (unsigned)strtoul(a.num.c_str(), nullptr, 10);
        if (id <= before) continue;
        inner += appSummaryXml(a);
        if (++n >= chunk) break;
    }
    return xdoc("ListOfAppSummary", inner);
}

std::string categoryXml(const char *id, const char *name, size_t count) {
    return std::string("<Category>") + xbool("HasSubCategories", false) + xel("Id", id) + xnil("ImageUrl") + xel("Name", name) +
           xel("NumItems", std::to_string(count)) + "</Category>";
}

std::string appEntityXml(const App &a) {
    std::string icon = iconUrl(a);
    std::string features = a.summary.empty() ? std::string("<Features/>")
                                              : "<Features><string>" + xmlEscape(a.summary) + "</string></Features>";
    std::string shots = icon.empty() ? std::string("<Screenshots/>")
                                      : "<Screenshots><string>" + xmlEscape(icon) + "</string></Screenshots>";
    const char *hw[] = {"Accelerometer", "Audio", "Bluetooth", "Dpad", "FmRadio", "GPS", "Infrared", "Keyboard",
                        "MemoryExtension", "Photo", "TouchScreen", "TvOut", "Video", "Wifi"};
    std::string flags;
    for (const char *name : hw) flags += xbool(name, false);
    return xok() + "<Entity>" + xel("ApplicationId", a.id) + xel("CompanyName", a.publisher) +
           xel("DateAdded", a.added.empty() ? "2012-01-01T00:00:00Z" : a.added) + xnil("DotNetVersion") + features +
           xel("Id", a.num) + xel("InstallName", a.title) + xel("LongDescription", a.summary.empty() ? a.title : a.summary) +
           (icon.empty() ? xnil("MediumImageUrl") : xel("MediumImageUrl", icon)) + xel("Name", a.title) +
           xel("NumOfReviews", "0") + xbool("OverAirDownloadAllowed", true) + xel("Price", "0") + xel("PriceWithTax", "0") +
           xel("Rating", "0") + shots + xel("Size", std::to_string(a.bytes)) + xnil("SupportUrl") +
           xbool("SupportsCCBilling", false) + xbool("SupportsMOBilling", false) + xnil("TermsOfUseURL") +
           xel("Version", a.version) + xbool("dotNet35", false) + flags + "</Entity>";
}

// True when this request is the catalog the built-in program calls. Fills r and returns true.
bool marketplaceApi(const Req &req, const std::vector<App> &all, Reply &r) {
    std::string pathL = lower(req.path);
    bool apiHost = req.host.find("api.marketplace.windowsmobile.com") != std::string::npos;
    bool apiPath = pathL.find("/appcatalog/") != std::string::npos || pathL.find("/devices/") != std::string::npos ||
                   pathL.find("/deviceinstance/") != std::string::npos || pathL.find("updateproducts") != std::string::npos ||
                   pathL.find("/update/") != std::string::npos || pathL.find("/commerce/") != std::string::npos;
    if (!apiHost && !apiPath) return false;
    // Sign-in is a page, not a catalog call. login.live.com itself is not answered here.
    if (pathL.find("login") != std::string::npos || pathL.find("passport") != std::string::npos ||
        pathL.find("expirecookie") != std::string::npos)
        return false;
    r.type = "text/xml; charset=utf-8";
    auto segs = pathSegs(pathL);
    int at = -1;
    bool instance = false;
    for (int i = 0; i < (int)segs.size(); i++) {
        if (segs[i] == "devices" || segs[i] == "deviceinstance") {
            at = i;
            instance = segs[i] == "deviceinstance";
            break;
        }
    }
    auto fail = [&](const char *root, const std::string &msg) {
        r.code = 404;
        r.reason = "Not Found";
        r.body = xdoc(root, xel("ResponseCode", "NotFound") + xel("ResponseMessage", msg));
    };
    if (pathL.find("updateproducts") != std::string::npos) {
        // No client update and no owned-app list: the program stays on the version it already has.
        r.body = xdoc("UpdateResponse", xok() + "<RemoveProducts/>" + "<AvailableUpdates/>" + "<AvailableProducts/>" +
                                         "<RevokeLicenses/>");
        return true;
    }
    if (at < 0 || at + 1 >= (int)segs.size()) {
        fail("ResponseOfApplication", "Unknown catalog request");
        return true;
    }
    std::vector<std::string> rest(segs.begin() + at + 2, segs.end());
    std::string head = rest.empty() ? std::string() : rest[0];
    std::string id = rest.size() > 1 ? rest[1] : std::string();
    unsigned before = queryUInt(req.query, "beforemarker", 0);
    unsigned chunk = queryUInt(req.query, "chunksize", 25);
    std::string q = queryParam(req.query, "q");
    if (q.empty()) q = queryParam(req.query, "applicationid");
    auto listPage = [&](const std::vector<App> &src) {
        r.body = appListXml(src, before, chunk);
    };
    if (!instance && head == "markets") {
        r.body = xdoc("ListResponseOfGeoMarket",
                      xok() + "<List><GeoMarket>" + xel("Code", "US") + xel("DisplayName", "United States") +
                                  xel("GeoFilter", "US") + xbool("IsHome", true) + "</GeoMarket></List>");
        return true;
    }
    if (!instance && head == "categories" && rest.size() <= 2) {
        std::string cats = categoryXml("1", "This phone", all.size());
        if (!id.empty() && id != "1") cats.clear();
        r.body = xdoc("ListResponseOfCategory", xok() + cats);
        return true;
    }
    if (!instance && head == "charts" && rest.size() <= 1) {
        std::string cats = categoryXml("1", "What's new", all.size()) + categoryXml("2", "Top free", all.size()) +
                           categoryXml("3", "Top paid", all.size());
        r.body = xdoc("ListResponseOfCategory", xok() + cats);
        return true;
    }
    if (!instance && head == "customizations") {
        r.body = xdoc("ListResponseOfMoCustomizationProperty", xok() + "<List/>");
        return true;
    }
    if (!instance && (head == "account" || head == "charge")) {
        const char *root = head == "charge" ? "OneTimePaymentResponse" : "ResponseOfAccount";
        r.body = xdoc(root, xok() + xel("AccountId", "local"));
        return true;
    }
    if (!instance && (head == "search" || (head == "categories" && rest.size() >= 3 && rest[2] == "apps") ||
                      (head == "charts" && rest.size() >= 3 && rest[2] == "apps"))) {
        listPage(head == "search" ? matching(all, q) : all);
        return true;
    }
    if (head == "apps" && rest.size() >= 3 && rest[2] == "reviews") {
        r.body = xdoc("ListOfReview", "");
        return true;
    }
    bool details = !instance && head == "apps" && !id.empty() && (rest.size() == 2);
    bool byApp = !instance && head == "catalogs" && !id.empty();
    bool download = instance && head == "apps" && rest.size() >= 3 && rest[2] == "urlandlicense";
    const App *app = nullptr;
    if (details || byApp || download) app = findAny(all, id);
    if (!app && !q.empty() && (details || byApp)) app = findAny(all, q);
    if ((details || byApp) && app) {
        r.body = xdoc("ResponseOfApplication", appEntityXml(*app));
        return true;
    }
    if (download && app) {
        std::string url = packageUrl(*app);
        r.body = xdoc("ResponseOfDownloadUrl", xok() + "<License>" + xel("Id", app->id) + xel("Url", url) + "</License>" +
                                                   xel("Url", url));
        return true;
    }
    if (details || byApp || download) {
        fail(download ? "ResponseOfDownloadUrl" : "ResponseOfApplication", "No installer with that id");
        return true;
    }
    fail("ResponseOfApplication", "Unknown catalog request");
    return true;
}

Reply respond(const Req &req, const std::vector<App> &all, const std::string &root) {
    Reply r;
    if (marketplaceApi(req, all, r)) return r;
    std::string pathL = lower(req.path);
    std::string actionL = lower(req.action);
    std::string guid = findGuid(pathL);
    if (guid.empty()) guid = findGuid(lower(req.body));
    const App *app = guid.empty() ? nullptr : findApp(all, guid);
    auto fileReply = [&](const std::string &path, const std::string &type, const std::string &name) {
        std::string real;
        if (!pathInside(root, path, &real)) {
            r.code = 404;
            r.reason = "Not Found";
            r.body = pageHtml({}, "", nullptr);
            return;
        }
        r.code = 200;
        r.type = type;
        r.file = real;
        r.downloadName = name;
    };
    bool wantIcon = pathL.find("largewebicon") != std::string::npos || pathL.find("screenshot") != std::string::npos ||
                    endsWith(pathL, ".png");
    bool wantCab = pathL.find("/pkg/") != std::string::npos || pathL.find("/download/") != std::string::npos ||
                   pathL.find("/package/") != std::string::npos || endsWith(pathL, ".cab");
    if (app && wantIcon && !wantCab) {
        if (app->iconPath.empty()) {
            r.code = 404;
            r.reason = "Not Found";
            r.type = "text/plain; charset=utf-8";
            r.body = "No icon\n";
        } else {
            fileReply(app->iconPath, "image/png", "");
        }
        return r;
    }
    if (app && wantCab) {
        fileReply(app->cabPath, "application/vnd.ms-cab-compressed", app->fileName);
        return r;
    }
    bool bootstrap = pathL.find("bootstrap") != std::string::npos || actionL.find("bootstrap") != std::string::npos ||
                     actionL.find("federation") != std::string::npos || actionL.find("clientconfig") != std::string::npos ||
                     actionL.find("clientupdate") != std::string::npos;
    bool catalog = pathL.find("catalog") != std::string::npos || actionL.find("catalog") != std::string::npos ||
                   actionL.find("search") != std::string::npos || actionL.find("application") != std::string::npos;
    bool login = pathL.find("login") != std::string::npos || pathL.find("passport") != std::string::npos;
    if (pathL.rfind("/app/", 0) == 0 && app) {
        r.body = pageHtml({*app}, "", app);
        return r;
    }
    if (bootstrap && !catalog) {
        r.type = "text/xml; charset=utf-8";
        r.body = bootstrapXml(clientVersion(req.body + " " + req.query), req.soap);
        return r;
    }
    if (login) {
        if (req.soap) {
            r.type = "text/xml; charset=utf-8";
            r.body = "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                     "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                     "<LoginResponse xmlns=\"http://marketplace.windowsmobile.com/2009/02\">"
                     "<Authenticated>true</Authenticated></LoginResponse></s:Body></s:Envelope>";
        } else {
            r.body = loginHtml();
        }
        return r;
    }
    std::string q = queryParam(req.query, "q");
    if (q.empty()) q = queryParam(req.query, "search");
    bool storefront = pathL == "/" || pathL == "/index.html" || pathL == "/default.aspx" || pathL == "/search";
    if (catalog || (req.soap && !storefront)) {
        const std::vector<App> *list = &all;
        std::vector<App> one, found;
        if (app && (actionL.find("application") != std::string::npos || actionL.find("download") != std::string::npos ||
                    actionL.find("package") != std::string::npos)) {
            one.push_back(*app);
            list = &one;
        } else if (!q.empty()) {
            found = matching(all, q);
            list = &found;
        }
        r.type = "text/xml; charset=utf-8";
        r.body = catalogXml(*list, req.soap);
        return r;
    }
    if (storefront || pathL == "/apps") {
        r.body = pageHtml(matching(all, q), q, nullptr);
        return r;
    }
    r.code = 404;
    r.reason = "Not Found";
    r.body = pageHtml({}, "", nullptr);
    return r;
}

std::string statusReason(int code) {
    if (code == 200) return "OK";
    if (code == 302) return "Found";
    if (code == 404) return "Not Found";
    if (code == 502) return "Bad Gateway";
    if (code == 413) return "Payload Too Large";
    if (code == 400) return "Bad Request";
    return "Error";
}

std::string safeFileName(const std::string &name) {
    std::string o;
    for (unsigned char c : name) {
        if (c < 32 || c == '"' || c == '\\' || c == '/') o += '_';
        else o += (char)c;
    }
    return o.empty() ? "installer.cab" : o;
}

std::string responseHead(int code, const std::string &type, size_t len, const std::string &downloadName,
                          const std::string &extra = {}) {
    std::string h = "HTTP/1.1 " + std::to_string(code) + " " + statusReason(code) + "\r\nContent-Type: " + type +
                    "\r\nContent-Length: " + std::to_string(len) + "\r\nCache-Control: no-cache\r\nConnection: close\r\n";
    if (!downloadName.empty()) h += "Content-Disposition: attachment; filename=\"" + safeFileName(downloadName) + "\"\r\n";
    if (!extra.empty()) h += extra;
    h += "\r\n";
    return h;
}

struct Upstream {
    std::string host;
    uint16_t port = 443;
    bool tls = true;
};

bool parseUpstream(std::string s, Upstream &u) {
    s = trim(s);
    if (s.empty()) return false;
    bool scheme = false;
    u.tls = true;
    u.port = 443;
    std::string low = lower(s);
    if (low.rfind("https://", 0) == 0) {
        scheme = true;
        s = s.substr(8);
    } else if (low.rfind("http://", 0) == 0) {
        scheme = true;
        u.tls = false;
        u.port = 80;
        s = s.substr(7);
    }
    auto slash = s.find('/');
    if (slash != std::string::npos) s.resize(slash);
    while (!s.empty() && s.back() == '.') s.pop_back();
    auto colon = s.rfind(':');
    if (colon != std::string::npos && s.find(']') == std::string::npos) {
        int p = atoi(s.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        u.port = (uint16_t)p;
        s.resize(colon);
    }
    if (!scheme) u.tls = u.port == 443;
    u.host = lower(trim(s));
    return !u.host.empty() && u.host.find(' ') == std::string::npos;
}

int connectHost(const std::string &host, uint16_t port, int timeoutMs) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res) return -1;
    int fd = -1;
    for (addrinfo *a = res; a && fd < 0; a = a->ai_next) {
        int s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s < 0) continue;
        int flags = fcntl(s, F_GETFL, 0);
        fcntl(s, F_SETFL, flags | O_NONBLOCK);
        int c = connect(s, a->ai_addr, a->ai_addrlen);
        if (c < 0 && errno != EINPROGRESS) {
            close(s);
            continue;
        }
        pollfd pol{s, POLLOUT, 0};
        if (c != 0 && poll(&pol, 1, timeoutMs) <= 0) {
            close(s);
            continue;
        }
        int err = 0;
        socklen_t len = sizeof err;
        if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
            close(s);
            continue;
        }
        fcntl(s, F_SETFL, flags);
        fd = s;
    }
    freeaddrinfo(res);
    return fd;
}

bool readHttpRaw(int fd, std::string &raw, size_t maxBytes, int timeoutMs) {
    while (raw.size() < maxBytes) {
        int w = recvWait(fd, raw, timeoutMs);
        if (w < 0) return !raw.empty();
        if (w == 0) {
            auto he = raw.find("\r\n\r\n");
            if (he == std::string::npos) return false;
            if (raw.size() >= he + 4 + contentLength(raw.substr(0, he))) return true;
        }
    }
    return raw.find("\r\n\r\n") != std::string::npos;
}

// Forwards one Marketplace request to the cabinet server. False only when that server does not answer.
bool proxyUpstream(const std::string &spec, const Req &req, Reply &r, std::string *err) {
    Upstream u;
    if (!parseUpstream(spec, u)) {
        if (err) *err = "bad host";
        return false;
    }
    std::string target = req.path;
    if (!req.query.empty()) target += "?" + req.query;
    std::string hostHeader = u.host;
    if ((u.tls && u.port != 443) || (!u.tls && u.port != 80)) hostHeader += ":" + std::to_string(u.port);
    std::string msg = req.method + " " + target + " HTTP/1.1\r\nHost: " + hostHeader + "\r\nConnection: close\r\n";
    if (!req.contentType.empty()) msg += "Content-Type: " + req.contentType + "\r\n";
    if (!req.action.empty()) msg += "SOAPAction: \"" + req.action + "\"\r\n";
    if (!req.body.empty() || req.method == "POST" || req.method == "PUT")
        msg += "Content-Length: " + std::to_string(req.body.size()) + "\r\n";
    msg += "\r\n";
    msg += req.body;
    const size_t maxBytes = 48u << 20;
    std::string raw;
    if (u.tls) {
#if WM_HAVE_TLS
        raw = httpsExchange(u.host, u.port, msg, err, 20000, maxBytes);
#else
        if (err) *err = "built without TLS support";
        return false;
#endif
    } else {
        int fd = connectHost(u.host, u.port, 10000);
        if (fd < 0) {
            if (err) *err = "cannot connect";
            return false;
        }
        bool ok = sendAll(fd, msg) && readHttpRaw(fd, raw, maxBytes, 20000);
        close(fd);
        if (!ok || raw.empty()) {
            if (err) *err = "no answer";
            return false;
        }
    }
    if (raw.empty()) return false;
    auto he = raw.find("\r\n\r\n");
    if (he == std::string::npos) {
        if (err) *err = "no HTTP response";
        return false;
    }
    std::string head = raw.substr(0, he);
    std::string body = raw.substr(he + 4);
    size_t need = contentLength(head);
    if (need && body.size() > need) body.resize(need);
    if (need && body.size() < need) {
        if (err) *err = "short response";
        return false;
    }
    auto nl = head.find("\r\n");
    std::string first = nl == std::string::npos ? head : head.substr(0, nl);
    auto sp = first.find(' ');
    r.code = sp == std::string::npos ? 200 : atoi(first.c_str() + sp + 1);
    if (r.code < 100 || r.code > 599) r.code = 502;
    r.reason = "";
    std::string type = headerValue(head, "content-type");
    r.type = type.empty() ? "application/octet-stream" : type;
    r.body = std::move(body);
    r.file.clear();
    auto disp = headerValue(head, "content-disposition");
    auto nameAt = lower(disp).find("filename=");
    if (nameAt != std::string::npos) {
        std::string name = trim(disp.substr(nameAt + 9));
        if (!name.empty() && name.front() == '"') {
            name.erase(name.begin());
            auto q = name.find('"');
            if (q != std::string::npos) name.resize(q);
        }
        r.downloadName = name;
    }
    return true;
}

bool bodyHasPassword(const std::string &body) {
    std::string l = lower(body);
    return l.find("password") != std::string::npos || l.find("passwd") != std::string::npos;
}

}  // namespace

struct Marketplace::Impl {
    std::mutex mu;
    std::string catalogDir;
    std::string captureDir;
    std::string upstream;
    int captureCount = 0;

    std::string dir() {
        std::lock_guard<std::mutex> lk(mu);
        return catalogDir;
    }

    std::string host() {
        std::lock_guard<std::mutex> lk(mu);
        return upstream;
    }

    void capture(const std::string &what, const std::string &text) {
        std::string folder;
        int n;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (captureDir.empty() || captureCount >= 200) return;
            if (captureCount == 0) mkdir(captureDir.c_str(), 0700);
            n = ++captureCount;
            folder = captureDir;
        }
        std::string safe;
        for (char ch : what) safe += isalnum((unsigned char)ch) || ch == '.' || ch == '-' ? ch : '_';
        FILE *f = fopen((folder + "/" + std::to_string(n) + "-" + safe + ".txt").c_str(), "wb");
        if (!f) return;
        fputs(text.c_str(), f);
        fclose(f);
    }

    Reply replyFor(const Req &req) {
        std::string root = dir();
        std::string up = host();
        if (!up.empty()) {
            Reply proxied;
            std::string err;
            if (proxyUpstream(up, req, proxied, &err)) return proxied;
            LOGW("market: %s did not answer (%s); cabinets in %s", up.c_str(), err.c_str(), root.c_str());
        }
        return respond(req, scanDir(root), root);
    }

    std::string answer(const std::string &raw) {
        Req req;
        if (!parseReq(raw, req))
            return responseHead(400, "text/plain; charset=utf-8", 12, "") + "Bad request\n";
        Reply rep = replyFor(req);
        std::string payload = rep.body;
        if (!rep.file.empty()) {
            FILE *f = fopen(rep.file.c_str(), "rb");
            if (!f) return responseHead(404, "text/plain; charset=utf-8", 10, "") + "Not found\n";
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, f)) > 0) payload.append(buf, n);
            fclose(f);
        }
        return responseHead(rep.code, rep.type, payload.size(), rep.downloadName, rep.extra) + payload;
    }

    void serveConn(int fd, const std::string &fallbackHost) {
        std::string buf;
        size_t need = 0;
        bool haveHead = false;
        while (buf.size() < 1024 * 1024) {
            if (!haveHead) {
                auto he = buf.find("\r\n\r\n");
                if (he != std::string::npos) {
                    haveHead = true;
                    need = he + 4 + contentLength(buf.substr(0, he));
                    if (need > 256 * 1024) {
                        std::string msg = "Request too large\n";
                        sendAll(fd, responseHead(413, "text/plain; charset=utf-8", msg.size(), "") + msg);
                        close(fd);
                        return;
                    }
                }
            }
            if (haveHead && buf.size() >= need) break;
            int w = recvWait(fd, buf, haveHead ? 5000 : 15000);
            if (w < 0) break;
            if (w == 0) {
                if (haveHead && contentLength(buf.substr(0, buf.find("\r\n\r\n"))) == 0) break;
                if (!haveHead) break;
            }
        }
        Req req;
        if (!parseReq(buf, req)) {
            std::string msg = "Bad request\n";
            sendAll(fd, responseHead(400, "text/plain; charset=utf-8", msg.size(), "") + msg);
            close(fd);
            return;
        }
        if (req.host.empty()) req.host = hostOnly(fallbackHost);
        std::string shown = req.body;
        if (bodyHasPassword(shown) || req.path.find("login") != std::string::npos || req.path.find("passport") != std::string::npos)
            shown = "(omitted: a password or a sign-in)\n";
        Reply rep = replyFor(req);
        LOGI("market: %s %s %s -> %d", req.method.c_str(), req.host.c_str(), req.path.c_str(), rep.code);
        capture(req.method + "-" + req.path, req.method + " " + req.path + " host " + req.host + "\nSOAPAction: " + req.action +
                                                  "\n\n" + shown + "\n\n-> " + std::to_string(rep.code));
        if (!rep.file.empty()) {
            FILE *f = fopen(rep.file.c_str(), "rb");
            if (!f) {
                std::string msg = "Not found\n";
                sendAll(fd, responseHead(404, "text/plain; charset=utf-8", msg.size(), "") + msg);
                close(fd);
                return;
            }
            if (fseek(f, 0, SEEK_END) != 0) {
                fclose(f);
                close(fd);
                return;
            }
            long sz = ftell(f);
            if (sz < 0) sz = 0;
            rewind(f);
            if (!sendAll(fd, responseHead(rep.code, rep.type, (size_t)sz, rep.downloadName, rep.extra))) {
                fclose(f);
                close(fd);
                return;
            }
            char block[16384];
            size_t n;
            while ((n = fread(block, 1, sizeof block, f)) > 0) {
                if (!sendAll(fd, block, n)) break;
            }
            fclose(f);
        } else {
            sendAll(fd, responseHead(rep.code, rep.type, rep.body.size(), "", rep.extra) + rep.body);
        }
        close(fd);
    }
};

Marketplace::Marketplace(Config cfg) : impl(std::make_shared<Impl>()) {
    impl->catalogDir = std::move(cfg.catalogDir);
    impl->captureDir = std::move(cfg.captureDir);
    impl->upstream = std::move(cfg.upstream);
    LOGI("market: Windows Marketplace at 10.0.2.4, server %s, cabinets in %s",
         impl->upstream.empty() ? "(none)" : impl->upstream.c_str(), impl->catalogDir.c_str());
}

Marketplace::~Marketplace() = default;

void Marketplace::setCatalogDir(const std::string &dir) {
    std::lock_guard<std::mutex> lk(impl->mu);
    impl->catalogDir = dir;
}

void Marketplace::setUpstream(const std::string &host) {
    std::lock_guard<std::mutex> lk(impl->mu);
    impl->upstream = trim(host);
}

void Marketplace::setCaptureDir(const std::string &dir) {
    std::lock_guard<std::mutex> lk(impl->mu);
    if (dir == impl->captureDir) return;
    impl->captureDir = dir;
    impl->captureCount = 0;
}

void Marketplace::serve(int fd, uint16_t, const std::string &host, bool) {
    std::shared_ptr<Impl> im = impl;
    std::string h = host;
    std::thread([im, fd, h] { im->serveConn(fd, h); }).detach();
}

std::vector<HostRule> Marketplace::hostRules() {
    return {HostRule{".marketplace.windowsmobile.com", kLocalServiceAddr}};
}

std::string Marketplace::selfTestRequest(const std::string &request) { return impl->answer(request); }

int marketplaceSelfTest() {
    char tmpl[] = "/tmp/wmdroid-market-XXXXXX";
    char *made = mkdtemp(tmpl);
    if (!made) {
        fprintf(stderr, "market self-test: cannot make a temp directory\n");
        return 1;
    }
    std::string dir = made;
    auto write = [&](const std::string &name, const std::string &bytes) {
        FILE *f = fopen((dir + "/" + name).c_str(), "wb");
        if (!f) return false;
        fwrite(bytes.data(), 1, bytes.size(), f);
        fclose(f);
        return true;
    };
    int failed = 0;
    auto expect = [&](bool ok, const char *what) {
        if (ok) return;
        fprintf(stderr, "market self-test: %s\n", what);
        failed++;
    };
    expect(write("Hello_App.cab", "MSCF-test-cabinet"), "write cab");
    expect(write("Hello_App.cab.ini", "title=Hello & Co\npublisher=Archive\nversion=2.0\nsummary=A note\n"), "write ini");
    expect(write("other.cab", "MSCF-other"), "write other");
    Marketplace market(Marketplace::Config{dir, ""});
    std::string page = market.selfTestRequest("GET / HTTP/1.1\r\nHost: marketplace.windowsmobile.com\r\n\r\n");
    expect(page.find("200 OK") != std::string::npos, "storefront status");
    expect(page.find("Hello &amp; Co") != std::string::npos, "title escaped");
    expect(page.find("Hello & Co") == std::string::npos, "title not raw");
    expect(page.find(">other<") != std::string::npos, "filename title");
    auto href = page.find("/pkg/");
    expect(href != std::string::npos, "install link");
    std::string id = href == std::string::npos ? std::string() : page.substr(href + 5, 36);
    expect(isGuid(id), "install id");
    std::string none = market.selfTestRequest("GET /?q=zzznomatch HTTP/1.1\r\nHost: marketplace.windowsmobile.com\r\n\r\n");
    expect(none.find("No cabinets match") != std::string::npos, "search miss");
    expect(none.find("Hello") == std::string::npos, "search hides the title");
    std::string boot = market.selfTestRequest("GET /bootstrap/02/2009 HTTP/1.1\r\nHost: client.marketplace.windowsmobile.com\r\n\r\n");
    expect(boot.find("available=\"false\"") != std::string::npos, "no client update");
    expect(boot.find("CatalogService.svc") != std::string::npos, "catalog endpoint");
    std::string soapBody = "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\"><s:Body>"
                           "<GetBootstrap><Version>1.2.1422.0001</Version></GetBootstrap></s:Body></s:Envelope>";
    std::string soap = "POST /Services/BootstrapService/BootstrapService.svc HTTP/1.1\r\n"
                       "Host: client.marketplace.windowsmobile.com\r\n"
                       "Content-Type: text/xml; charset=utf-8\r\n"
                       "SOAPAction: \"http://marketplace.windowsmobile.com/2009/02/GetBootstrap\"\r\n"
                       "Content-Length: " + std::to_string(soapBody.size()) + "\r\n\r\n" + soapBody;
    std::string soapResp = market.selfTestRequest(soap);
    expect(soapResp.find("s:Envelope") != std::string::npos, "soap envelope");
    expect(soapResp.find("available=\"false\"") != std::string::npos, "soap no update");
    expect(soapResp.find("1.2.1422.0001") != std::string::npos, "echoed client version");
    if (!id.empty()) {
        std::string cab = market.selfTestRequest("GET /pkg/" + id + " HTTP/1.1\r\nHost: files.marketplace.windowsmobile.com\r\n\r\n");
        expect(cab.find("MSCF-test-cabinet") != std::string::npos || cab.find("MSCF-other") != std::string::npos, "cabinet bytes");
        expect(cab.find("vnd.ms-cab-compressed") != std::string::npos, "cabinet type");
        std::string byPath = market.selfTestRequest("GET /" + id + "/Package/Hello_App.cab HTTP/1.1\r\nHost: files.marketplace.windowsmobile.com\r\n\r\n");
        expect(byPath.find("MSCF-") != std::string::npos, "package path");
    }
    std::string missing = market.selfTestRequest("GET /pkg/00000000-0000-0000-0000-000000000000 HTTP/1.1\r\nHost: files.marketplace.windowsmobile.com\r\n\r\n");
    expect(missing.find("404") != std::string::npos, "unknown id");
    auto apiGet = [&](const std::string &path) {
        return market.selfTestRequest("GET " + path + " HTTP/1.1\r\nHost: api.marketplace.windowsmobile.com\r\n\r\n");
    };
    std::string cats = apiGet("/appcatalog/02/2009/devices/phone/categories?loc=1033");
    expect(cats.find("ListResponseOfCategory") != std::string::npos, "category response");
    expect(cats.find("This phone") != std::string::npos, "category name");
    expect(cats.find("<html") == std::string::npos, "category is not the storefront");
    std::string listed = apiGet("/appcatalog/02/2009/devices/phone/categories/1/apps?beforeMarker=0&chunkSize=10");
    expect(listed.find("ListOfAppSummary") != std::string::npos, "app list");
    expect(listed.find("Hello &amp; Co") != std::string::npos, "app list title");
    expect(listed.find("<Id>1</Id>") != std::string::npos, "app list id");
    std::string one = apiGet("/appcatalog/02/2009/devices/phone/apps/1?loc=1033");
    expect(one.find("ResponseOfApplication") != std::string::npos, "app details");
    expect(one.find("Archive") != std::string::npos, "app publisher");
    expect(one.find(">2.0<") != std::string::npos, "app version");
    expect(one.find("OverAirDownloadAllowed>true") != std::string::npos, "over the air");
    std::string lic = apiGet("/appcatalog/02/2009/deviceInstance/phone/apps/1/urlandlicense?loc=1033");
    expect(lic.find("ResponseOfDownloadUrl") != std::string::npos, "download response");
    auto urlAt = lic.find("https://files.marketplace.windowsmobile.com/");
    expect(urlAt != std::string::npos, "download url");
    if (urlAt != std::string::npos) {
        std::string url = lic.substr(urlAt);
        auto end = url.find('<');
        if (end != std::string::npos) url.resize(end);
        auto slash = url.find(".com/");
        std::string pkg = slash == std::string::npos ? std::string() : url.substr(slash + 4);
        std::string got = market.selfTestRequest("GET " + pkg + " HTTP/1.1\r\nHost: files.marketplace.windowsmobile.com\r\n\r\n");
        expect(got.find("MSCF-test-cabinet") != std::string::npos, "download bytes");
    }
    std::string missed = apiGet("/appcatalog/02/2009/devices/phone/search?q=zzznomatch");
    expect(missed.find("ListOfAppSummary") != std::string::npos, "search response");
    expect(missed.find("Hello") == std::string::npos, "search hides the app");
    std::string markets = apiGet("/appcatalog/02/2009/devices/phone/markets?loc=1033");
    expect(markets.find("ListResponseOfGeoMarket") != std::string::npos, "markets");
    expect(markets.find("IsHome>true") != std::string::npos, "home market");
    std::string updBody = "<UpdateRequest xmlns=\"Microsoft.WindowsMobile.Service.Marketplace\">"
                          "<DeviceGuid>00000000-0000-0000-0000-000000000001</DeviceGuid>"
                          "<Products><Product><Name>Marketplace</Name><Version>1.2.1422.0000</Version></Product></Products>"
                          "</UpdateRequest>";
    std::string upd = market.selfTestRequest(
        "POST /update/02/2009/updateproducts HTTP/1.1\r\nHost: api.marketplace.windowsmobile.com\r\n"
        "Content-Type: text/xml; charset=utf-8\r\nContent-Length: " +
        std::to_string(updBody.size()) + "\r\n\r\n" + updBody);
    expect(upd.find("UpdateResponse") != std::string::npos, "update response");
    expect(upd.find("<Mandatory>true</Mandatory>") == std::string::npos, "no forced client update");
    std::string gone = apiGet("/appcatalog/02/2009/devices/phone/apps/999");
    expect(gone.find("404") != std::string::npos, "unknown app");
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bool listening = lfd >= 0 && bind(lfd, (sockaddr *)&addr, sizeof addr) == 0 && listen(lfd, 8) == 0;
    expect(listening, "loopback listen");
    if (listening) {
        socklen_t alen = sizeof addr;
        getsockname(lfd, (sockaddr *)&addr, &alen);
        unsigned port = ntohs(addr.sin_port);
        std::atomic<bool> stop{false};
        std::thread loop([&] {
            while (!stop.load()) {
                pollfd pol{lfd, POLLIN, 0};
                if (poll(&pol, 1, 200) <= 0) continue;
                int c = accept(lfd, nullptr, nullptr);
                if (c < 0) continue;
                std::string in;
                recvWait(c, in, 2000);
                bool cab = in.find("/pkg/") != std::string::npos;
                std::string body = cab ? "UPSTREAMCAB" : "upstream-ok";
                std::string type = cab ? "application/vnd.ms-cab-compressed" : "text/plain";
                std::string out = "HTTP/1.1 200 OK\r\nContent-Type: " + type + "\r\nContent-Length: " +
                                  std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                sendAll(c, out);
                close(c);
            }
        });
        market.setUpstream("127.0.0.1:" + std::to_string(port));
        std::string proxied = market.selfTestRequest("GET / HTTP/1.1\r\nHost: marketplace.windowsmobile.com\r\n\r\n");
        expect(proxied.find("upstream-ok") != std::string::npos, "proxied page");
        expect(proxied.find("Hello") == std::string::npos, "proxy skips the local title");
        std::string upCab = market.selfTestRequest("GET /pkg/x HTTP/1.1\r\nHost: files.marketplace.windowsmobile.com\r\n\r\n");
        expect(upCab.find("UPSTREAMCAB") != std::string::npos, "proxied cabinet");
        stop.store(true);
        loop.join();
        close(lfd);
        market.setUpstream("127.0.0.1:1");
        std::string fell = market.selfTestRequest("GET / HTTP/1.1\r\nHost: marketplace.windowsmobile.com\r\n\r\n");
        expect(fell.find("Hello") != std::string::npos, "local cabinets when the server is down");
        market.setUpstream("");
    } else if (lfd >= 0) {
        close(lfd);
    }
    bool rulesOk = Marketplace::hostRules().size() == 1 &&
                          Marketplace::hostRules()[0].pattern == ".marketplace.windowsmobile.com";
    expect(rulesOk, "host rule");
    remove((dir + "/Hello_App.cab").c_str());
    remove((dir + "/Hello_App.cab.ini").c_str());
    remove((dir + "/other.cab").c_str());
    rmdir(dir.c_str());
    if (failed) fprintf(stderr, "market self-test: %d failed\n", failed);
    else fprintf(stderr, "market self-test: ok\n");
    return failed ? 1 : 0;
}

}  // namespace wm
