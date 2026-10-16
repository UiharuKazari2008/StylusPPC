#include "tlsproxy.h"
#include "localsvc.h"
#include "revival.h"
#include "../util/log.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace wm {

bool HostRule::matches(const std::string &host) const {
    if (pattern.empty()) return false;
    if (pattern[0] == '.') {
        if (host == pattern.substr(1)) return true;
        return host.size() > pattern.size() && host.compare(host.size() - pattern.size(), pattern.size(), pattern) == 0;
    }
    return host == pattern;
}

const HostRule *findHostRule(const std::vector<HostRule> *rules, const std::string &host) {
    if (!rules) return nullptr;
    for (const HostRule &r : *rules)
        if (r.matches(host)) return &r;
    return nullptr;
}

}  // namespace wm

#if WM_HAVE_TLS

#include <arpa/inet.h>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <set>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

namespace wm {

namespace {

std::mutex g_trustMu;
std::string g_trustDir;  // empty = Android's own store

uint64_t nowMs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

void nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

bool readFile(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

// Written whole or not at all, readable by this user only (the CA key must not leak to other apps).
bool writePrivate(const std::string &path, const void *data, size_t n) {
    std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return false;
    const uint8_t *p = (const uint8_t *)data;
    size_t done = 0;
    while (done < n) {
        ssize_t w = write(fd, p + done, n - done);
        if (w <= 0) { close(fd); unlink(tmp.c_str()); return false; }
        done += (size_t)w;
    }
    fsync(fd);
    close(fd);
    return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string keyPem(EVP_PKEY *key) {
    bssl::UniquePtr<BIO> bio(BIO_new(BIO_s_mem()));
    if (!PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr)) return "";
    const uint8_t *p;
    size_t n;
    BIO_mem_contents(bio.get(), &p, &n);
    return std::string((const char *)p, n);
}

bssl::UniquePtr<EVP_PKEY> keyFromPem(const std::string &pem) {
    bssl::UniquePtr<BIO> bio(BIO_new_mem_buf(pem.data(), (ossl_ssize_t)pem.size()));
    return bssl::UniquePtr<EVP_PKEY>(PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr));
}

std::vector<uint8_t> der(X509 *x) {
    int n = i2d_X509(x, nullptr);
    if (n <= 0) return {};
    std::vector<uint8_t> v((size_t)n);
    uint8_t *p = v.data();
    i2d_X509(x, &p);
    return v;
}

std::string ipString(uint32_t ip) {
    char b[16];
    snprintf(b, sizeof b, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
    return b;
}

bool isIpLiteral(const std::string &s) {
    in_addr a;
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

// Host names that may go into a certificate or an SNI: letters, digits, '-', '_' and dots.
bool plausibleHost(const std::string &s) {
    if (s.empty() || s.size() > 253 || s[0] == '.' || s[0] == '-') return false;
    for (char ch : s)
        if (!(isalnum((unsigned char)ch) || ch == '-' || ch == '.' || ch == '_')) return false;
    return true;
}

std::string lower(std::string s) {
    for (char &ch : s) ch = (char)tolower((unsigned char)ch);
    return s;
}

// Random positive 128-bit serial. Its randomness is also what keeps SHA-1 signatures (needed by
// WM 5's certificate checks) safe from chosen-prefix collisions: nobody can predict the signed bytes.
bool randomSerial(X509 *x) {
    uint8_t b[16];
    RAND_bytes(b, sizeof b);
    b[0] = (uint8_t)((b[0] & 0x3F) | 0x40);
    bssl::UniquePtr<BIGNUM> bn(BN_bin2bn(b, sizeof b, nullptr));
    bssl::UniquePtr<ASN1_INTEGER> i(BN_to_ASN1_INTEGER(bn.get(), nullptr));
    return i && X509_set_serialNumber(x, i.get());
}

bool setValidity(X509 *x, time_t notAfter) {
    // From 2000: the guest's clock may be off (a fresh boot before time sync) and the CA is private,
    // so an early start costs nothing. UTCTime (before 2050) is what 2009-era parsers expect.
    bssl::UniquePtr<ASN1_TIME> from(ASN1_TIME_new()), to(ASN1_TIME_new());
    if (!ASN1_UTCTIME_set_string(from.get(), "000101000000Z")) return false;
    if (!ASN1_TIME_set(to.get(), notAfter)) return false;
    return X509_set1_notBefore(x, from.get()) && X509_set1_notAfter(x, to.get());
}

bool addExt(X509 *cert, X509 *issuer, int nid, const std::string &value) {
    X509V3_CTX ctx;
    X509V3_set_ctx(&ctx, issuer, cert, nullptr, nullptr, 0);
    X509_EXTENSION *ex = X509V3_EXT_nconf_nid(nullptr, &ctx, nid, value.c_str());
    if (!ex) return false;
    bool ok = X509_add_ext(cert, ex, -1) == 1;
    X509_EXTENSION_free(ex);
    return ok;
}

bool addName(X509_NAME *n, const char *field, const std::string &v) {
    return X509_NAME_add_entry_by_txt(n, field, MBSTRING_ASC, (const uint8_t *)v.data(), (ossl_ssize_t)v.size(), -1, 0) == 1;
}

// 2049-12-31: the last date UTCTime can say.
constexpr time_t kCaNotAfter = 2524607999;

bssl::UniquePtr<X509> makeCa(EVP_PKEY *key) {
    bssl::UniquePtr<X509> x(X509_new());
    uint8_t tag[2];
    RAND_bytes(tag, sizeof tag);
    char cn[80];
    snprintf(cn, sizeof cn, "PocketStylus Secure Web Proxy CA %02X%02X", tag[0], tag[1]);
    X509_NAME *n = X509_get_subject_name(x.get());
    if (!X509_set_version(x.get(), X509_VERSION_3) || !randomSerial(x.get()) ||
        !addName(n, "CN", cn) || !addName(n, "O", "PocketStylus (private to this phone)") ||
        !X509_set_issuer_name(x.get(), n) || !setValidity(x.get(), kCaNotAfter) ||
        !X509_set_pubkey(x.get(), key) ||
        !addExt(x.get(), x.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
        !addExt(x.get(), x.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !addExt(x.get(), x.get(), NID_subject_key_identifier, "hash") ||
        !X509_sign(x.get(), key, EVP_sha1()))
        return nullptr;
    return x;
}

int loadTrustDir(X509_STORE *st, const std::string &dir) {
    DIR *d = opendir(dir.c_str());
    if (!d) return 0;
    int n = 0;
    while (dirent *e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        bssl::UniquePtr<BIO> bio(BIO_new_file((dir + "/" + e->d_name).c_str(), "r"));
        if (!bio) continue;
        bssl::UniquePtr<X509> x(PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr));
        if (x && X509_STORE_add_cert(st, x.get())) n++;
    }
    closedir(d);
    ERR_clear_error();
    return n;
}

std::string htmlEscape(const std::string &s) {
    std::string o;
    for (char ch : s) {
        if (ch == '<') o += "&lt;";
        else if (ch == '>') o += "&gt;";
        else if (ch == '&') o += "&amp;";
        else if (ch == '"') o += "&quot;";
        else o += ch;
    }
    return o;
}

// Waits until `fd` can do what the last SSL call wanted. False on timeout.
bool waitSsl(int fd, int sslErr, int ms) {
    pollfd p{fd, (short)(sslErr == SSL_ERROR_WANT_WRITE ? POLLOUT : POLLIN), 0};
    return poll(&p, 1, ms) > 0;
}

bool handshake(SSL *s, int fd, int timeoutMs) {
    uint64_t end = nowMs() + (uint64_t)timeoutMs;
    for (;;) {
        int r = SSL_do_handshake(s);
        if (r == 1) return true;
        int e = SSL_get_error(s, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
        uint64_t now = nowMs();
        if (now >= end || !waitSsl(fd, e, (int)(end - now))) return false;
    }
}

bool writeAll(SSL *s, int fd, const char *p, size_t n, int timeoutMs) {
    uint64_t end = nowMs() + (uint64_t)timeoutMs;
    while (n) {
        int r = SSL_write(s, p, (int)std::min<size_t>(n, 16384));
        if (r > 0) { p += r; n -= (size_t)r; continue; }
        int e = SSL_get_error(s, r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) return false;
        uint64_t now = nowMs();
        if (now >= end || !waitSsl(fd, e, (int)(end - now))) return false;
    }
    return true;
}

int connectTimeout(const sockaddr *a, socklen_t len, int timeoutMs) {
    int fd = socket(a->sa_family, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    nonblock(fd);
    if (connect(fd, a, len) != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    pollfd p{fd, POLLOUT, 0};
    int err = 0;
    socklen_t el = sizeof err;
    if (poll(&p, 1, timeoutMs) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el) != 0 || err) {
        close(fd);
        return -1;
    }
    return fd;
}

// The Host header of an HTTP request head (lower case, port dropped), or "".
std::string hostHeader(const std::string &head) {
    size_t i = 0;
    while (i < head.size()) {
        size_t e = head.find("\r\n", i);
        if (e == std::string::npos) e = head.size();
        if (e - i > 5 && strncasecmp(head.c_str() + i, "host:", 5) == 0) {
            std::string v = head.substr(i + 5, e - i - 5);
            v.erase(0, v.find_first_not_of(" \t"));
            v.erase(v.find_last_not_of(" \t") + 1);
            size_t c = v.find(':');
            if (c != std::string::npos) v.resize(c);
            return lower(v);
        }
        if (e == i) break;  // end of the head
        i = e + 2;
    }
    return "";
}

// ---- What a failed certificate check tells the user ----

// Text from a certificate, safe for the dialog and the mailbox command: printable ASCII, short.
std::string printable(const std::string &s, size_t max = 64) {
    std::string o;
    for (char ch : s) {
        if (o.size() >= max) { o += "..."; break; }
        o += (ch >= 0x20 && ch < 0x7F) ? ch : '?';
    }
    return o;
}

std::string hexString(const uint8_t *p, size_t n) {
    std::string o;
    char h[3];
    for (size_t i = 0; i < n; i++) { snprintf(h, sizeof h, "%02X", p[i]); o += h; }
    return o;
}

// "12 Apr 2015" (UTC): the same everywhere, unlike 4/12/2015.
std::string dateString(const ASN1_TIME *t) {
    int64_t p = 0;
    if (!t || !ASN1_TIME_to_posix(t, &p)) return "an unknown date";
    static const char *kMon[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t tt = (time_t)p;
    struct tm g {};
    gmtime_r(&tt, &g);
    char b[32];
    snprintf(b, sizeof b, "%d %s %d", g.tm_mday, kMon[g.tm_mon % 12], g.tm_year + 1900);
    return b;
}

// The names a certificate is for: its subjectAltName DNS/IP entries, else the subject CN.
std::vector<std::string> certNames(X509 *x) {
    std::vector<std::string> v;
    auto *gens = (GENERAL_NAMES *)X509_get_ext_d2i(x, NID_subject_alt_name, nullptr, nullptr);
    if (gens) {
        for (size_t i = 0; i < sk_GENERAL_NAME_num(gens); i++) {
            const GENERAL_NAME *g = sk_GENERAL_NAME_value(gens, i);
            if (g->type == GEN_DNS)
                v.push_back(printable(std::string((const char *)ASN1_STRING_get0_data(g->d.dNSName), (size_t)ASN1_STRING_length(g->d.dNSName))));
            else if (g->type == GEN_IPADD && ASN1_STRING_length(g->d.iPAddress) == 4) {
                const uint8_t *a = ASN1_STRING_get0_data(g->d.iPAddress);
                v.push_back(ipString((uint32_t)a[0] << 24 | (uint32_t)a[1] << 16 | (uint32_t)a[2] << 8 | a[3]));
            }
        }
        GENERAL_NAMES_free(gens);
    }
    char cn[256];
    if (v.empty() && X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, cn, sizeof cn) > 0) v.push_back(printable(cn));
    return v;
}

// A certificate name's most telling part: CN, else O.
std::string nameString(X509_NAME *n) {
    char b[256];
    if (n && (X509_NAME_get_text_by_NID(n, NID_commonName, b, sizeof b) > 0 || X509_NAME_get_text_by_NID(n, NID_organizationName, b, sizeof b) > 0))
        return printable(b);
    return "";
}

std::string nameList(const std::vector<std::string> &names) {
    std::string o;
    size_t shown = std::min<size_t>(names.size(), 4);
    for (size_t i = 0; i < shown; i++) o += (i ? ", " : "") + names[i];
    if (names.size() > shown) o += " and " + std::to_string(names.size() - shown) + " more";
    return o.empty() ? "no name" : o;
}

// One line for the dialog per problem, worded like Internet Explorer's certificate warnings.
std::string describeProblem(int err, int depth, X509 *cur, const std::vector<std::string> &leafNames) {
    switch (err) {
        case X509_V_ERR_CERT_HAS_EXPIRED:
            if (depth == 0) return "The security certificate expired on " + dateString(cur ? X509_get0_notAfter(cur) : nullptr) + ".";
            return "A certificate it depends on (" + nameString(cur ? X509_get_subject_name(cur) : nullptr) + ") expired on " +
                   dateString(cur ? X509_get0_notAfter(cur) : nullptr) + ".";
        case X509_V_ERR_CERT_NOT_YET_VALID:
            return "The security certificate is not valid until " + dateString(cur ? X509_get0_notBefore(cur) : nullptr) + ".";
        case X509_V_ERR_HOSTNAME_MISMATCH:
        case X509_V_ERR_IP_ADDRESS_MISMATCH:
            return "The name on the security certificate does not match the site. It was issued for " + nameList(leafNames) + ".";
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
            return "The security certificate is self-signed: no trusted authority vouches for it.";
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: {
            std::string who = nameString(!cur ? nullptr : err == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN ? X509_get_subject_name(cur) : X509_get_issuer_name(cur));
            return "The security certificate was issued by a company that is not trusted" + (who.empty() ? std::string(".") : " (" + who + ").");
        }
        default:
            return std::string("The security certificate is not valid: ") + X509_verify_cert_error_string(err) + ".";
    }
}

// The guest closed its side of a connection (IE gave up on the request).
bool peerGone(int fd) {
    pollfd p{fd, (short)(POLLIN | POLLRDHUP), 0};
    return poll(&p, 1, 0) > 0 && (p.revents & (POLLRDHUP | POLLHUP | POLLERR));
}

// Where an upstream SSL keeps its certificate check (verifyCb).
int checkIndex() {
    static int idx = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return idx;
}

}  // namespace

struct TlsProxy::Impl : std::enable_shared_from_this<TlsProxy::Impl> {
    std::string dir;
    bssl::UniquePtr<EVP_PKEY> caKey, leafKey;
    bssl::UniquePtr<X509> ca;
    std::vector<uint8_t> caDer;
    std::string thumb;
    bssl::UniquePtr<SSL_CTX> server, client;
    std::mutex mu;
    std::map<std::string, bssl::UniquePtr<X509>> leafs;
    int trusted = 0;

    // The leaf for these names (newest first) and the address, minted once and cached.
    bssl::UniquePtr<X509> leafFor(const std::vector<std::string> &names, uint32_t ip) {
        std::string key = ipString(ip);
        for (auto &n : names) key += "," + n;
        std::lock_guard<std::mutex> lk(mu);
        auto it = leafs.find(key);
        if (it != leafs.end()) {
            X509_up_ref(it->second.get());
            return bssl::UniquePtr<X509>(it->second.get());
        }
        bssl::UniquePtr<X509> x(X509_new());
        std::string cn = names.empty() ? ipString(ip) : names[0];
        std::string san;
        for (auto &n : names) san += (san.empty() ? "" : ",") + std::string(isIpLiteral(n) ? "IP:" : "DNS:") + n;
        if (std::find(names.begin(), names.end(), ipString(ip)) == names.end())
            san += (san.empty() ? "IP:" : ",IP:") + ipString(ip);
        time_t notAfter = std::min<time_t>(time(nullptr) + 397 * 86400, kCaNotAfter);
        X509_NAME *n = X509_get_subject_name(x.get());
        if (!X509_set_version(x.get(), X509_VERSION_3) || !randomSerial(x.get()) ||
            !addName(n, "CN", cn) || !addName(n, "O", "PocketStylus Secure Web Proxy") ||
            !X509_set_issuer_name(x.get(), X509_get_subject_name(ca.get())) ||
            !setValidity(x.get(), notAfter) || !X509_set_pubkey(x.get(), leafKey.get()) ||
            !addExt(x.get(), ca.get(), NID_basic_constraints, "CA:FALSE") ||
            !addExt(x.get(), ca.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
            !addExt(x.get(), ca.get(), NID_ext_key_usage, "serverAuth") ||
            !addExt(x.get(), ca.get(), NID_subject_alt_name, san) ||
            !addExt(x.get(), ca.get(), NID_authority_key_identifier, "keyid:always") ||
            !X509_sign(x.get(), caKey.get(), EVP_sha1())) {
            LOGW("tls: could not mint a certificate for %s", cn.c_str());
            return nullptr;
        }
        if (leafs.size() >= 256) leafs.clear();
        X509_up_ref(x.get());
        leafs.emplace(key, bssl::UniquePtr<X509>(x.get()));
        return x;
    }

    struct ConnCtx {
        Impl *im;
        const Conn *c;
        std::string sni;
    };

    static int certCb(SSL *s, void *arg) {
        auto *cx = (ConnCtx *)arg;
        std::vector<std::string> names = cx->c->names;
        const char *sni = SSL_get_servername(s, TLSEXT_NAMETYPE_host_name);
        if (sni) {
            cx->sni = lower(sni);
            if (plausibleHost(cx->sni)) {
                names.erase(std::remove(names.begin(), names.end(), cx->sni), names.end());
                names.insert(names.begin(), cx->sni);
            }
        }
        if (names.size() > 16) names.resize(16);
        bssl::UniquePtr<X509> leaf = cx->im->leafFor(names, cx->c->ip);
        if (!leaf) return 0;
        return SSL_use_certificate(s, leaf.get()) && SSL_use_PrivateKey(s, cx->im->leafKey.get()) ? 1 : 0;
    }

    // ---- Certificate check of one upstream connection attempt (verifyCb) ----
    struct Check {
        Impl *im = nullptr;
        std::string host;                   // the name the certificate must carry
        std::string fp;                     // SHA-256 of the server's certificate, hex
        std::vector<std::string> names;     // what that certificate is for
        std::vector<int> codes;             // the X509_V_ERR_* found
        std::vector<std::string> problems;  // one sentence per problem (describeProblem)
        bool allowed = false;               // the user chose to continue with this very certificate
        bool failed() const { return !codes.empty() && !allowed; }
        std::string reason() const {
            std::string r;
            for (auto &p : problems) r += (r.empty() ? "" : " ") + p;
            return r;
        }
    };

    // Called by BoringSSL for every certificate of the chain. It records each problem and lets the
    // handshake finish, so the question lists all of them; run() then sends nothing upstream unless
    // the certificate was clean or is one the user accepted for this host.
    static int verifyCb(int ok, X509_STORE_CTX *st) {
        auto *s = (SSL *)X509_STORE_CTX_get_ex_data(st, SSL_get_ex_data_X509_STORE_CTX_idx());
        auto *ck = s ? (Check *)SSL_get_ex_data(s, checkIndex()) : nullptr;
        if (!ck) return ok;
        X509 *leaf = X509_STORE_CTX_get0_cert(st);
        if (ck->fp.empty() && leaf) {
            uint8_t md[EVP_MAX_MD_SIZE];
            unsigned n = 0;
            X509_digest(leaf, EVP_sha256(), md, &n);
            ck->fp = hexString(md, n);
            ck->names = certNames(leaf);
            ck->allowed = ck->im->isAllowed(ck->host, ck->fp);
        }
        if (ok) return 1;
        int err = X509_STORE_CTX_get_error(st);
        if (std::find(ck->codes.begin(), ck->codes.end(), err) == ck->codes.end()) {
            ck->codes.push_back(err);
            ck->problems.push_back(describeProblem(err, X509_STORE_CTX_get_error_depth(st), X509_STORE_CTX_get_current_cert(st), ck->names));
        }
        return 1;
    }

    // ---- Asking the user (TlsProxy::takePrompts / answerPrompt) ----
    struct Ask {
        uint32_t id = 0;
        std::string key, host, fp, text;
        uint64_t asked = 0, deadline = 0;
        int answer = 0;  // 0 waiting, 1 continue, 2 don't, 3 no answer in time
    };
    enum AskResult { kContinue, kDeclined, kNoAnswer, kGuestGone, kNobody };
    std::mutex askMu;
    std::condition_variable askCv;
    std::map<std::string, std::shared_ptr<Ask>> asking;          // the open question per host
    std::map<uint32_t, std::shared_ptr<Ask>> asks;               // recent questions by id (late answers)
    std::vector<std::shared_ptr<Ask>> fresh;                     // not yet handed out
    std::map<std::string, std::set<std::string>> allowedCerts;   // host -> certificates (SHA-256) accepted
    std::map<std::string, uint64_t> declined;                    // host|fp -> no new question before then
    uint64_t lastPoll = 0;                                       // someone polls: questions can be answered
    uint32_t nextAsk = 1;
    int waitSec = TlsProxy::kPromptWaitSec;

    bool isAllowed(const std::string &host, const std::string &fp) {
        std::lock_guard<std::mutex> lk(askMu);
        auto it = allowedCerts.find(host);
        return it != allowedCerts.end() && it->second.count(fp);
    }

    static std::string promptText(const std::string &host, const Check &ck) {
        std::string t = "There is a problem with the security certificate of " + host + ".\n";
        for (auto &p : ck.problems) t += "\n" + p + "\n";
        t += "\nThe connection stays encrypted, but the site may not be the one it claims to be.\n"
             "\nDo you want to continue to this site anyway?";
        return t;
    }

    // Holds this connection until the user answers the question about `ck` (shared with any other
    // connection to the same host that is already waiting), the time is up, or the guest gives up.
    AskResult ask(const std::string &host, const Check &ck, int gfd, const std::string &where) {
        std::unique_lock<std::mutex> lk(askMu);
        uint64_t now = nowMs();
        auto al = allowedCerts.find(ck.host);
        if (al != allowedCerts.end() && al->second.count(ck.fp)) return kContinue;  // accepted meanwhile
        if (!lastPoll || now - lastPoll > 5000) return kNobody;  // no app / helper to show the question
        auto d = declined.find(ck.host + "|" + ck.fp);
        if (d != declined.end() && now < d->second) return kDeclined;  // just said no: no question storm
        std::shared_ptr<Ask> a;
        auto it = asking.find(ck.host);
        if (it != asking.end() && it->second->answer == 0) {
            a = it->second;
            LOGI("tls: %s %s: waiting for the open question #%u", where.c_str(), host.c_str(), a->id);
        } else {
            a = std::make_shared<Ask>();
            a->id = nextAsk++;
            a->key = ck.host;
            a->host = host;
            a->fp = ck.fp;
            a->text = promptText(host, ck);
            a->asked = now;
            a->deadline = now + (uint64_t)waitSec * 1000;
            asking[ck.host] = a;
            asks[a->id] = a;
            fresh.push_back(a);
            for (auto i = asks.begin(); i != asks.end();)  // late answers count for 10 minutes
                i = now - i->second->asked > 600000 ? asks.erase(i) : std::next(i);
            LOGI("tls: %s %s: asking whether to continue (#%u, up to %d s)", where.c_str(), host.c_str(), a->id, waitSec);
        }
        while (a->answer == 0) {
            now = nowMs();
            if (now >= a->deadline) {
                a->answer = 3;
                auto o = asking.find(a->key);
                if (o != asking.end() && o->second == a) asking.erase(o);
                LOGI("tls: %s: no answer to #%u within %d s", host.c_str(), a->id, waitSec);
                askCv.notify_all();
                break;
            }
            askCv.wait_for(lk, std::chrono::milliseconds(std::min<uint64_t>(1000, a->deadline - now)));
            if (a->answer == 0 && peerGone(gfd)) {
                LOGI("tls: %s %s: the guest gave up after %.1f s waiting for #%u", where.c_str(), host.c_str(),
                     (nowMs() - a->asked) / 1000.0, a->id);
                return kGuestGone;
            }
        }
        return a->answer == 1 ? kContinue : a->answer == 2 ? kDeclined : kNoAnswer;
    }

    std::vector<CertPrompt> takePrompts() {
        std::lock_guard<std::mutex> lk(askMu);
        uint64_t now = nowMs();
        lastPoll = now;
        std::vector<CertPrompt> v;
        for (auto &a : fresh)
            if (a->answer == 0 && a->deadline > now)
                v.push_back({a->id, (int)((a->deadline - now + 999) / 1000), a->host, a->text});
        fresh.clear();
        return v;
    }

    void answer(uint32_t id, bool allow) {
        std::lock_guard<std::mutex> lk(askMu);
        auto it = asks.find(id);
        if (it == asks.end()) { LOGW("tls: answer to unknown question #%u", id); return; }
        std::shared_ptr<Ask> a = it->second;
        if (allow) allowedCerts[a->key].insert(a->fp);
        else declined[a->key + "|" + a->fp] = nowMs() + 3000;
        LOGI("tls: %s: the user chose %s (#%u%s)", a->host.c_str(), allow ? "to continue" : "not to continue", id,
             a->answer == 0 ? "" : ", after the request stopped waiting");
        if (a->answer == 0) a->answer = allow ? 1 : 2;
        auto o = asking.find(a->key);
        if (o != asking.end() && o->second == a) asking.erase(o);
        askCv.notify_all();
    }

    void errorPage(SSL *g, int fd, const std::string &host, const std::string &reason, const std::string &note = "") {
        std::string body =
            "<html><head><title>Secure connection blocked</title></head><body>"
            "<h3>PocketStylus could not connect securely to " + htmlEscape(host) + "</h3>"
            "<p><b>" + htmlEscape(reason) + "</b></p>"
            "<p>The secure web proxy checks every site with Android's own certificate store. "
            "This site did not pass, so the connection was stopped to protect you. "
            "Nothing was sent to it.</p>" +
            (note.empty() ? std::string() : "<p>" + htmlEscape(note) + "</p>") +
            "<p><small>PocketStylus secure web proxy</small></p></body></html>";
        // IE replaces short error pages with its own "friendly" one (IE Mobile 6 on WM 6.5.3 did):
        // keep the page well past its 512-byte threshold.
        if (body.size() < 1500) body += "\n<!--" + std::string(1500 - body.size(), ' ') + "-->";
        std::string resp = "HTTP/1.1 502 Bad Gateway\r\nContent-Type: text/html; charset=utf-8\r\n"
                           "Cache-Control: no-store\r\nConnection: close\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body;
        writeAll(g, fd, resp.data(), resp.size(), 10000);
    }

    void run(Conn c);

    // Relays the guest's decrypted stream (starting with `pending`) to a plain socket and back, until
    // either side ends. For local services (localsvc.h).
    static void relayPlain(SSL *g, int gfd, int pfd, std::string pending) {
        char buf[16384];
        std::string p2g;
        bool gEof = false, pEof = false;
        int gWantOut = 0;
        uint64_t lastMove = nowMs();
        for (;;) {
            bool moved = false;
            if (pending.empty() && !gEof) {  // guest -> service
                int r = SSL_read(g, buf, sizeof buf);
                if (r > 0) { pending.assign(buf, (size_t)r); moved = true; }
                else {
                    int e = SSL_get_error(g, r);
                    if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { gEof = true; moved = true; shutdown(pfd, SHUT_WR); }
                }
            }
            if (!pending.empty()) {
                ssize_t w = send(pfd, pending.data(), pending.size(), MSG_NOSIGNAL);
                if (w > 0) { pending.erase(0, (size_t)w); moved = true; }
                else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK) break;
            }
            if (p2g.empty() && !pEof) {  // service -> guest
                ssize_t r = recv(pfd, buf, sizeof buf, 0);
                if (r > 0) { p2g.assign(buf, (size_t)r); moved = true; }
                else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) { pEof = true; moved = true; }
            }
            if (!p2g.empty()) {
                int r = SSL_write(g, p2g.data(), (int)p2g.size());
                if (r > 0) { p2g.erase(0, (size_t)r); moved = true; gWantOut = 0; }
                else {
                    int e = SSL_get_error(g, r);
                    if (e == SSL_ERROR_WANT_WRITE) gWantOut = 1;
                    else if (e != SSL_ERROR_WANT_READ) break;
                }
            }
            if (pEof && p2g.empty()) break;               // the service is done and everything went out
            if (gEof && pending.empty() && pEof) break;
            if (moved) { lastMove = nowMs(); continue; }
            if (nowMs() - lastMove > 1800000) break;      // idle 30 min (push channels stay quiet long)
            pollfd p[2] = {{gfd, 0, 0}, {pfd, 0, 0}};
            if (pending.empty() && !gEof) p[0].events |= POLLIN;
            if (gWantOut) p[0].events |= POLLOUT;
            if (!p2g.empty() && !gWantOut) p[0].events |= POLLIN;
            if (p2g.empty() && !pEof) p[1].events |= POLLIN;
            if (!pending.empty()) p[1].events |= POLLOUT;
            poll(p, 2, 5000);
        }
        ERR_clear_error();
    }
};

void TlsProxy::Impl::run(Conn c) {
    int gfd = c.fd, ufd = -1;
    nonblock(gfd);
    ConnCtx cx{this, &c, ""};
    bssl::UniquePtr<SSL> g(SSL_new(server.get()));
    bssl::UniquePtr<SSL> u;
    std::string where = ipString(c.ip) + ":" + std::to_string(c.port);
    auto finish = [&] {
        if (u) SSL_shutdown(u.get());
        if (g) SSL_shutdown(g.get());
        u.reset();
        g.reset();
        if (ufd >= 0) close(ufd);
        close(gfd);
    };
    if (!g) { close(gfd); return; }
    SSL_set_fd(g.get(), gfd);
    SSL_set_accept_state(g.get());
    SSL_set_cert_cb(g.get(), certCb, &cx);
    if (!handshake(g.get(), gfd, 30000)) {
        // Usually the guest refusing our certificate (root not installed yet) or giving up.
        uint32_t e = ERR_peek_last_error();
        LOGW("tls: %s guest handshake failed (%s)", where.c_str(), e ? ERR_reason_error_string(e) : "closed");
        ERR_clear_error();
        finish();
        return;
    }
    std::string guestTls = std::string(SSL_get_version(g.get())) + " " + SSL_CIPHER_get_name(SSL_get_current_cipher(g.get()));

    // The request head names the host the guest means (WM's TLS sends no SNI). Wait for it; a
    // connection opened ahead of use may stay quiet for a while.
    std::string head;
    char buf[16384];
    uint64_t lastData = nowMs(), opened = lastData;
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < sizeof buf) {
        int r = SSL_read(g.get(), buf, sizeof buf);
        if (r > 0) { head.append(buf, (size_t)r); lastData = nowMs(); continue; }
        int e = SSL_get_error(g.get(), r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { ERR_clear_error(); finish(); return; }
        uint64_t now = nowMs();
        if (!head.empty() && now - lastData > 3000) break;  // not HTTP: go by the other names
        if (head.empty() && now - opened > 300000) { finish(); return; }
        waitSsl(gfd, e, 1000);
    }
    std::string host = hostHeader(head);
    if (!plausibleHost(host)) host = plausibleHost(cx.sni) ? cx.sni : !c.names.empty() ? c.names[0] : ipString(c.ip);

    // Where to really connect, and which name the server's certificate must carry.
    const HostRule *rule = findHostRule(c.rules.get(), host);
    std::string target = rule ? lower(rule->target) : host;
    if (target == kLocalServiceAddr || (!rule && c.ip == kLocalServiceIp)) {
        // A service inside PocketStylus (localsvc.h): it gets the decrypted stream, nothing leaves the phone.
        std::shared_ptr<LocalService> svc = localService();
        int sv[2];
        if (!svc || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
            errorPage(g.get(), gfd, host, "The server could not be reached.");
            finish();
            return;
        }
        LOGI("tls: %s %s to the local service, guest %s", where.c_str(), host.c_str(), guestTls.c_str());
        nonblock(sv[0]);
        svc->serve(sv[1], c.port, host, true);
        ufd = sv[0];
        relayPlain(g.get(), gfd, ufd, head);
        finish();
        return;
    }
    std::string verifyName = rule && isIpLiteral(target) ? host : target;
    bool sameAsGuest = !rule && (std::find(c.names.begin(), c.names.end(), host) != c.names.end() || host == ipString(c.ip));
    auto connectUpstream = [&]() -> int {
        int fd = -1;
        if (sameAsGuest) {
            sockaddr_in a{};
            a.sin_family = AF_INET;
            a.sin_port = htons(c.port);
            a.sin_addr.s_addr = htonl(c.ip);
            fd = connectTimeout((sockaddr *)&a, sizeof a, 15000);
        } else {
            addrinfo hints{}, *res = nullptr;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(target.c_str(), std::to_string(c.port).c_str(), &hints, &res) == 0) {
                int tries = 0;
                for (addrinfo *a = res; a && fd < 0 && tries < 4; a = a->ai_next, tries++)
                    fd = connectTimeout(a->ai_addr, a->ai_addrlen, 15000);
                freeaddrinfo(res);
            }
        }
        return fd;
    };
    // A certificate that fails the check gets one question per connection; after "continue" the
    // proxy connects again and goes on only if the server shows the very certificate accepted.
    Check ck;
    bool asked = false;
    for (;;) {
        ufd = connectUpstream();
        if (ufd < 0) {
            LOGW("tls: %s %s: could not connect", where.c_str(), host.c_str());
            errorPage(g.get(), gfd, host, "The server could not be reached.");
            finish();
            return;
        }
        ck = Check();
        ck.im = this;
        ck.host = verifyName;
        u.reset(SSL_new(client.get()));
        SSL_set_ex_data(u.get(), checkIndex(), &ck);
        SSL_set_fd(u.get(), ufd);
        SSL_set_connect_state(u.get());
        if (isIpLiteral(verifyName)) {
            X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(u.get()), verifyName.c_str());
        } else {
            SSL_set_tlsext_host_name(u.get(), verifyName.c_str());
            SSL_set1_host(u.get(), verifyName.c_str());
        }
        static const uint8_t kAlpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};  // the guest speaks HTTP/1.x
        SSL_set_alpn_protos(u.get(), kAlpn, sizeof kAlpn);
        if (!handshake(u.get(), ufd, 20000)) {
            // verifyCb lets certificate problems through to the end of the handshake, so this is
            // the TLS itself (no common protocol or suite, a reset): nothing to ask about.
            uint32_t e = ERR_peek_last_error();
            std::string reason = std::string("The secure connection to the server failed (") + (e ? ERR_reason_error_string(e) : "no answer") + ").";
            ERR_clear_error();
            LOGW("tls: %s %s: upstream rejected: %s", where.c_str(), host.c_str(), reason.c_str());
            errorPage(g.get(), gfd, host, reason);
            finish();
            return;
        }
        if (!ck.failed()) break;  // valid, or accepted by the user
        // The certificate failed the check: drop this connection before anything is sent on it.
        std::string reason = ck.reason();
        LOGW("tls: %s %s: upstream rejected: %s", where.c_str(), host.c_str(), reason.c_str());
        SSL_shutdown(u.get());
        u.reset();
        close(ufd);
        ufd = -1;
        if (asked) {  // accepted, but the new connection showed another certificate
            errorPage(g.get(), gfd, host, reason, "The site showed a different certificate from the one you accepted. "
                      "Reload the page to be asked again.");
            finish();
            return;
        }
        asked = true;
        AskResult r = ask(host, ck, gfd, where);
        if (r == kContinue) continue;
        const char *note =
            r == kDeclined ? "You chose not to continue to this site. Reload the page to be asked again." :
            r == kNoAnswer ? "The question whether to continue to this site anyway got no answer in time. "
                             "Reload the page to be asked again." :
            r == kNobody   ? "PocketStylus could not ask whether to continue anyway: its helper is not running in "
                             "Windows Mobile. Reload the page once it runs to be asked." : "";
        if (r != kGuestGone) errorPage(g.get(), gfd, host, reason, note);
        finish();
        return;
    }
    LOGI("tls: %s %s%s%s, guest %s, server %s %s%s", where.c_str(), host.c_str(), rule ? " via " : "", rule ? target.c_str() : "",
         guestTls.c_str(), SSL_get_version(u.get()), SSL_CIPHER_get_name(SSL_get_current_cipher(u.get())),
         ck.codes.empty() ? "" : ", certificate problem accepted by the user");

    // Relay. Each direction holds at most one buffer; a side is read only while its buffer is
    // empty, so a slow reader slows the writer down instead of filling memory.
    std::string g2u = head, u2g;
    // A revived service (HostRule::http, revival.h) knows itself only by its own name.
    bool fixResponse = rule && rule->http && !isIpLiteral(target);
    if (fixResponse) revivalRequest(g2u, host, target);
    bool gEof = false, uEof = false;
    int gWantOut = 0, uWantOut = 0;  // a write to that side last wanted POLLOUT
    uint64_t lastMove = nowMs();
    auto pump = [&](SSL *from, SSL *to, std::string &bufStr, bool &eof, int &toWantOut) -> int {
        int moved = 0;
        if (bufStr.empty() && !eof) {
            int r = SSL_read(from, buf, sizeof buf);
            if (r > 0) { bufStr.assign(buf, (size_t)r); moved = 1; }
            else {
                int e = SSL_get_error(from, r);
                if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) { eof = true; moved = 1; }
            }
        }
        if (!bufStr.empty()) {
            int r = SSL_write(to, bufStr.data(), (int)bufStr.size());
            if (r > 0) { bufStr.erase(0, (size_t)r); moved = 1; toWantOut = 0; }
            else {
                int e = SSL_get_error(to, r);
                if (e == SSL_ERROR_WANT_WRITE) toWantOut = 1;
                else if (e != SSL_ERROR_WANT_READ) return -1;
            }
        }
        return moved;
    };
    for (;;) {
        int a = pump(g.get(), u.get(), g2u, gEof, uWantOut);
        int b;
        if (fixResponse && u2g.empty() && !uEof) {  // revival: the response head, as the old clients spell it
            int r = SSL_read(u.get(), buf, sizeof buf), e = r > 0 ? 0 : SSL_get_error(u.get(), r);
            b = r > 0 || (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE);
            if (r > 0) { u2g.assign(buf, (size_t)r); fixResponse = !revivalResponse(u2g); }
            else if (b) uEof = true;
        } else {
            b = pump(u.get(), g.get(), u2g, uEof, gWantOut);
        }
        if (a < 0 || b < 0) break;
        if ((gEof && g2u.empty()) || (uEof && u2g.empty())) break;  // one side is done and drained
        if (a || b) { lastMove = nowMs(); continue; }
        if (nowMs() - lastMove > 600000) break;  // idle 10 min
        pollfd p[2] = {{gfd, 0, 0}, {ufd, 0, 0}};
        if (g2u.empty() && !gEof) p[0].events |= POLLIN;
        if (u2g.empty() && !uEof) p[1].events |= POLLIN;
        if (gWantOut) p[0].events |= POLLOUT;
        if (uWantOut) p[1].events |= POLLOUT;
        if (!g2u.empty() && !uWantOut) p[1].events |= POLLIN;  // a write that wanted a read
        if (!u2g.empty() && !gWantOut) p[0].events |= POLLIN;
        poll(p, 2, 5000);
    }
    ERR_clear_error();
    finish();
}

TlsProxy::TlsProxy() : impl(new Impl) {}
TlsProxy::~TlsProxy() {}

void TlsProxy::setTrustDir(const std::string &dir) {
    std::lock_guard<std::mutex> lk(g_trustMu);
    g_trustDir = dir;
}

std::shared_ptr<TlsProxy> TlsProxy::open(const std::string &dir, std::string *err) {
    auto fail = [&](const char *why) -> std::shared_ptr<TlsProxy> {
        if (err) *err = why;
        LOGW("tls: %s (%s)", why, dir.c_str());
        return nullptr;
    };
    // BoringSSL writes to its sockets with write(): a peer that went away must not kill the process
    // (Android apps already ignore SIGPIPE; the CLI doesn't).
    signal(SIGPIPE, SIG_IGN);
    mkdir(dir.c_str(), 0700);
    std::shared_ptr<TlsProxy> p(new TlsProxy);
    Impl &im = *p->impl;
    im.dir = dir;
    std::string keyPemText, caDerText, leafPemText;
    if (readFile(dir + "/ca.key", keyPemText) && readFile(dir + "/ca.cer", caDerText) && readFile(dir + "/leaf.key", leafPemText)) {
        im.caKey = keyFromPem(keyPemText);
        im.leafKey = keyFromPem(leafPemText);
        const uint8_t *q = (const uint8_t *)caDerText.data();
        im.ca.reset(d2i_X509(nullptr, &q, (long)caDerText.size()));
        if (!im.caKey || !im.leafKey || !im.ca || !X509_check_private_key(im.ca.get(), im.caKey.get())) {
            LOGW("tls: stored CA unreadable; making a new one");
            im.caKey.reset();
        }
        ERR_clear_error();
    }
    if (!im.caKey) {
        // First use: a fresh 2048-bit root and leaf key (RSA: WM's TLS only does RSA key exchange).
        im.caKey.reset(EVP_RSA_gen(2048));
        im.leafKey.reset(EVP_RSA_gen(2048));
        if (!im.caKey || !im.leafKey) return fail("could not generate keys");
        im.ca = makeCa(im.caKey.get());
        if (!im.ca) return fail("could not make the CA certificate");
        std::string kp = keyPem(im.caKey.get()), lp = keyPem(im.leafKey.get());
        std::vector<uint8_t> d = der(im.ca.get());
        if (kp.empty() || lp.empty() || !writePrivate(dir + "/ca.key", kp.data(), kp.size()) ||
            !writePrivate(dir + "/leaf.key", lp.data(), lp.size()) || !writePrivate(dir + "/ca.cer", d.data(), d.size()))
            return fail("could not store the CA");
        LOGI("tls: made a new private root CA in %s", dir.c_str());
    }
    im.caDer = der(im.ca.get());
    uint8_t md[EVP_MAX_MD_SIZE];
    unsigned mdLen = 0;
    X509_digest(im.ca.get(), EVP_sha1(), md, &mdLen);
    for (unsigned i = 0; i < mdLen; i++) {
        char h[3];
        snprintf(h, sizeof h, "%02X", md[i]);
        im.thumb += h;
    }

    // Guest side: what 2009 Windows Mobile can speak. TLS 1.0 with RSA key exchange: AES-128/256-CBC
    // (WM 6.1.4, 6.5.3) or 3DES (WM 5.0, whose hello is SSL 2.0-framed). This leg never leaves the
    // process, so its weak crypto is not exposed to any network.
    im.server.reset(SSL_CTX_new(TLS_method()));
    SSL_CTX_set_min_proto_version(im.server.get(), TLS1_VERSION);
    SSL_CTX_set_max_proto_version(im.server.get(), TLS1_2_VERSION);
    if (!SSL_CTX_set_strict_cipher_list(im.server.get(),
            "ECDHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-SHA:ECDHE-RSA-AES256-SHA:"
            "AES128-SHA:AES256-SHA:DES-CBC3-SHA"))
        return fail("cipher list rejected");
    SSL_CTX_set_options(im.server.get(), SSL_OP_CIPHER_SERVER_PREFERENCE);
    SSL_CTX_set_mode(im.server.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    // Upstream: TLS 1.2 or 1.3 with BoringSSL's default (modern) suites, validated against Android's store.
    im.client.reset(SSL_CTX_new(TLS_method()));
    SSL_CTX_set_min_proto_version(im.client.get(), TLS1_2_VERSION);
    SSL_CTX_set_verify(im.client.get(), SSL_VERIFY_PEER, Impl::verifyCb);  // problems: run() decides (and may ask)
    SSL_CTX_set_mode(im.client.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    std::string trustDir;
    {
        std::lock_guard<std::mutex> lk(g_trustMu);
        trustDir = g_trustDir;
    }
    X509_STORE *st = SSL_CTX_get_cert_store(im.client.get());
    if (!trustDir.empty()) {
        im.trusted = loadTrustDir(st, trustDir);
    } else {
        // Android 14+ keeps the updatable store in the Conscrypt APEX; older releases in /system.
        im.trusted = loadTrustDir(st, "/apex/com.android.conscrypt/cacerts");
        if (!im.trusted) im.trusted = loadTrustDir(st, "/system/etc/security/cacerts");
    }
    LOGI("tls: proxy ready, root %s, %d trusted upstream roots%s%s", im.thumb.c_str(), im.trusted,
         trustDir.empty() ? "" : " from ", trustDir.c_str());
    return p;
}

const std::vector<uint8_t> &TlsProxy::caDer() const { return impl->caDer; }
std::string TlsProxy::caThumbprint() const { return impl->thumb; }
const std::string &TlsProxy::dir() const { return impl->dir; }

void TlsProxy::serve(Conn c) {
    std::shared_ptr<Impl> im = impl;
    std::thread([im, c]() mutable { im->run(std::move(c)); }).detach();
}

std::vector<TlsProxy::CertPrompt> TlsProxy::takePrompts() { return impl->takePrompts(); }
void TlsProxy::answerPrompt(uint32_t id, bool allow) { impl->answer(id, allow); }

void TlsProxy::clearExceptions() {
    std::lock_guard<std::mutex> lk(impl->askMu);
    impl->allowedCerts.clear();
    impl->declined.clear();
}

void TlsProxy::setPromptWait(int seconds) {
    std::lock_guard<std::mutex> lk(impl->askMu);
    impl->waitSec = std::max(1, seconds);
}

std::string httpsExchange(const std::string &host, uint16_t port, const std::string &request, std::string *err, int timeoutMs) {
    auto fail = [&](const std::string &why) { if (err) *err = why; ERR_clear_error(); return std::string(); };
    static std::mutex ctxMu;
    static bssl::UniquePtr<SSL_CTX> ctx;
    {
        std::lock_guard<std::mutex> lk(ctxMu);
        if (!ctx) {
            signal(SIGPIPE, SIG_IGN);
            ctx.reset(SSL_CTX_new(TLS_method()));
            SSL_CTX_set_min_proto_version(ctx.get(), TLS1_2_VERSION);
            SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, nullptr);  // any problem fails the handshake
            std::string dir;
            {
                std::lock_guard<std::mutex> tk(g_trustMu);
                dir = g_trustDir;
            }
            X509_STORE *st = SSL_CTX_get_cert_store(ctx.get());
            if (!dir.empty()) loadTrustDir(st, dir);
            else if (!loadTrustDir(st, "/apex/com.android.conscrypt/cacerts")) loadTrustDir(st, "/system/etc/security/cacerts");
        }
    }
    addrinfo hints{}, *res = nullptr;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return fail("cannot resolve " + host);
    int fd = -1;
    for (addrinfo *a = res; a && fd < 0; a = a->ai_next) fd = connectTimeout(a->ai_addr, a->ai_addrlen, 15000);
    freeaddrinfo(res);
    if (fd < 0) return fail("cannot connect to " + host);
    bssl::UniquePtr<SSL> s(SSL_new(ctx.get()));
    SSL_set_fd(s.get(), fd);
    SSL_set_connect_state(s.get());
    SSL_set_tlsext_host_name(s.get(), host.c_str());
    SSL_set1_host(s.get(), host.c_str());
    if (!handshake(s.get(), fd, timeoutMs)) {
        long v = SSL_get_verify_result(s.get());
        uint32_t e = ERR_peek_last_error();
        close(fd);
        return fail(v != X509_V_OK ? std::string("certificate: ") + X509_verify_cert_error_string(v)
                                   : std::string("TLS: ") + (e ? ERR_reason_error_string(e) : "no answer"));
    }
    if (!writeAll(s.get(), fd, request.data(), request.size(), timeoutMs)) { close(fd); return fail("send failed"); }
    std::string out;
    char buf[16384];
    uint64_t end = nowMs() + (uint64_t)timeoutMs;
    for (;;) {
        int r = SSL_read(s.get(), buf, sizeof buf);
        if (r > 0) { out.append(buf, (size_t)r); if (out.size() > (8u << 20)) break; continue; }
        int e = SSL_get_error(s.get(), r);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) break;
        uint64_t now = nowMs();
        if (now >= end || !waitSsl(fd, e, (int)(end - now))) break;
    }
    SSL_shutdown(s.get());
    close(fd);
    ERR_clear_error();
    if (out.empty()) return fail("no answer");
    return out;
}

}  // namespace wm

#else  // !WM_HAVE_TLS

#include <unistd.h>

namespace wm {

struct TlsProxy::Impl {
    std::vector<uint8_t> none;
    std::string dir;
};
TlsProxy::TlsProxy() : impl(new Impl) {}
TlsProxy::~TlsProxy() {}
std::shared_ptr<TlsProxy> TlsProxy::open(const std::string &, std::string *err) {
    if (err) *err = "built without TLS support";
    return nullptr;
}
void TlsProxy::setTrustDir(const std::string &) {}
const std::vector<uint8_t> &TlsProxy::caDer() const { return impl->none; }
std::string TlsProxy::caThumbprint() const { return ""; }
const std::string &TlsProxy::dir() const { return impl->dir; }
void TlsProxy::serve(Conn c) { if (c.fd >= 0) ::close(c.fd); }
std::vector<TlsProxy::CertPrompt> TlsProxy::takePrompts() { return {}; }
void TlsProxy::answerPrompt(uint32_t, bool) {}
void TlsProxy::clearExceptions() {}
void TlsProxy::setPromptWait(int) {}
std::string httpsExchange(const std::string &, uint16_t, const std::string &, std::string *err, int) {
    if (err) *err = "built without TLS support";
    return "";
}

}  // namespace wm

#endif
