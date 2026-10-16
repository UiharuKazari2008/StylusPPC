#include "folder_share.h"
#include "bus.h"
#include "de_devices.h"
#include "../util/log.h"
#include <cerrno>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace wm {

namespace {

// ServerPB field offsets.
enum : uint32_t {
    PB_SIZE = 0x000, PB_TID = 0x004, PB_INDEX = 0x008, PB_HANDLE = 0x00A, PB_TIME = 0x00C, PB_FSIZE = 0x010,
    PB_POS = 0x014, PB_DTA = 0x018, PB_ATTR = 0x01C, PB_MODE = 0x01E, PB_NAMELEN = 0x022, PB_NAME = 0x024,
    PB_NAME2LEN = 0x224, PB_NAME2 = 0x226, PB_CTIME = 0x428, PB_BYTES = 0x42C,
};
enum : uint32_t {
    E_OK = 0, E_FUNC = 1, E_NOFILE = 2, E_NOPATH = 3, E_TOOMANY = 4, E_ACCESS = 5, E_HANDLE = 6,
    E_NOMORE = 0x12, E_WRITEFAULT = 0x1D, E_READFAULT = 0x1E, E_GENERAL = 0x1F, E_SHARING = 0x20,
    E_EXISTS = 0x50, E_NAME = 0x7B,
};
enum : uint16_t { A_RO = 0x01, A_HIDDEN = 0x02, A_DIR = 0x10, A_ARCHIVE = 0x20 };
const uint32_t kMaxIo = 0x10000;

uint16_t get16(const uint8_t *p, uint32_t o) { uint16_t v; memcpy(&v, p + o, 2); return v; }
uint32_t get32(const uint8_t *p, uint32_t o) { uint32_t v; memcpy(&v, p + o, 4); return v; }
void put16(uint8_t *p, uint32_t o, uint16_t v) { memcpy(p + o, &v, 2); }
void put32(uint8_t *p, uint32_t o, uint32_t v) { memcpy(p + o, &v, 4); }

std::u16string pbName(const uint8_t *pb, uint32_t lenOff, uint32_t nameOff) {
    uint32_t n = get16(pb, lenOff) / 2;
    if (n > 256) n = 256;
    std::u16string s;
    for (uint32_t i = 0; i < n; i++) {
        char16_t c = (char16_t)get16(pb, nameOff + 2 * i);
        if (!c) break;
        s.push_back(c);
    }
    return s;
}

std::string toUtf8(const std::u16string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
            c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
        if (c < 0x80) out += (char)c;
        else if (c < 0x800) { out += (char)(0xC0 | (c >> 6)); out += (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += (char)(0xE0 | (c >> 12)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
        else { out += (char)(0xF0 | (c >> 18)); out += (char)(0x80 | ((c >> 12) & 0x3F)); out += (char)(0x80 | ((c >> 6) & 0x3F)); out += (char)(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::u16string toUtf16(const std::string &s) {
    std::u16string out;
    for (size_t i = 0; i < s.size();) {
        uint8_t b = (uint8_t)s[i];
        uint32_t c;
        int len = b < 0x80 ? 1 : (b >> 5) == 6 ? 2 : (b >> 4) == 14 ? 3 : 4;
        c = len == 1 ? b : len == 2 ? (b & 0x1F) : len == 3 ? (b & 0x0F) : (b & 0x07);
        for (int k = 1; k < len && i + k < s.size(); k++) c = (c << 6) | ((uint8_t)s[i + k] & 0x3F);
        i += len;
        if (c >= 0x10000) {
            c -= 0x10000;
            out.push_back((char16_t)(0xD800 + (c >> 10)));
            out.push_back((char16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            out.push_back((char16_t)c);
        }
    }
    return out;
}

void putName(uint8_t *pb, const std::string &name) {
    std::u16string w = toUtf16(name);
    if (w.size() > 255) w.resize(255);
    for (size_t i = 0; i < w.size(); i++) put16(pb, PB_NAME + 2 * (uint32_t)i, w[i]);
    put16(pb, PB_NAME + 2 * (uint32_t)w.size(), 0);
    put16(pb, PB_NAMELEN, (uint16_t)(w.size() * 2));
}

uint32_t toDos(time_t t) {
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year < 80 || tm.tm_year > 207) return 0;
    uint32_t date = ((uint32_t)(tm.tm_year - 80) << 9) | ((uint32_t)(tm.tm_mon + 1) << 5) | (uint32_t)tm.tm_mday;
    uint32_t time = ((uint32_t)tm.tm_hour << 11) | ((uint32_t)tm.tm_min << 5) | (uint32_t)(tm.tm_sec / 2);
    return (date << 16) | time;
}

bool fromDos(uint32_t v, time_t &out) {
    struct tm tm = {};
    uint32_t date = v >> 16, t = v & 0xFFFF;
    tm.tm_year = (int)(date >> 9) + 80;
    tm.tm_mon = (int)((date >> 5) & 0xF) - 1;
    tm.tm_mday = (int)(date & 0x1F);
    tm.tm_hour = (int)(t >> 11);
    tm.tm_min = (int)((t >> 5) & 0x3F);
    tm.tm_sec = (int)(t & 0x1F) * 2;
    tm.tm_isdst = -1;
    if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_hour > 23 || tm.tm_min > 59 || tm.tm_sec > 59)
        return false;
    out = mktime(&tm);
    return out != (time_t)-1;
}

// Windows-style wildcard match, case-insensitive. "*.*" matches every name.
bool wildMatch(const char *p, const char *s) {
    if (!strcmp(p, "*.*")) return true;
    const char *star = nullptr, *ss = nullptr;
    while (*s) {
        if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) { p++; s++; }
        else if (*p == '*') { star = p++; ss = s; }
        else if (star) { p = star + 1; s = ++ss; }
        else return false;
    }
    while (*p == '*') p++;
    return !*p;
}

bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string parentOf(const std::string &p) {
    size_t i = p.find_last_of('/');
    return i == std::string::npos ? std::string() : p.substr(0, i ? i : 1);
}

}  // namespace

FolderShare::~FolderShare() { closeAll(); }

void FolderShare::closeAll() {
    for (Handle &h : handles) {
        if (h.used && h.fd >= 0) close(h.fd);
        h = Handle();
    }
    for (Find &f : finds) f = Find();
}

void FolderShare::setMounts(const std::vector<Mount> &m, bool announce) {
    std::lock_guard<std::mutex> lk(mu);
    bool wasEnabled = !mounts.empty();
    if (traceCalls) {  // what the guest still has open on the card
        for (Handle &h : handles)
            if (h.used) LOGI("folder share: open handle %s (mode %x)", h.guest.c_str(), h.mode);
        for (int i = 0; i < 40; i++)
            if (finds[i].used) LOGI("folder share: open search %d (%zu entries, next %zu)", i, finds[i].entries.size(), finds[i].next);
    }
    closeAll();
    mounts.clear();
    for (const Mount &mt : m) {
        struct stat st;
        if (stat(mt.path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) mounts.push_back(mt);
        else LOGW("folder share: %s is not a directory, skipped", mt.path.c_str());
    }
    LOGI("folder share: %zu mount(s)%s", mounts.size(), mounts.empty() ? " (disabled)" : "");
    if (announce && (wasEnabled || !mounts.empty())) emulServ.raise(EmulServ::FOLDER_SHARING_CHANGED);
}

void FolderShare::onGuestReset() {
    std::lock_guard<std::mutex> lk(mu);
    closeAll();
    if (!mounts.empty()) emulServ.raise(EmulServ::FOLDER_SHARING_CHANGED);
}

uint8_t *FolderShare::ram(uint32_t pa, uint32_t len) {
    bool wr;
    uint8_t *p = bus.hostPage(pa, wr);
    if (!p || !wr) return nullptr;
    if (len > 1) {
        uint8_t *q = bus.hostPage(pa + len - 1, wr);
        if (q != p + len - 1) return nullptr;
    }
    return p;
}

std::string FolderShare::matchCase(const std::string &dir, const std::string &name) {
    if (exists(dir + "/" + name)) return name;
    DIR *d = opendir(dir.c_str());
    if (!d) return name;
    std::string found = name;
    while (struct dirent *e = readdir(d)) {
        if (!strcasecmp(e->d_name, name.c_str())) { found = e->d_name; break; }
    }
    closedir(d);
    return found;
}

uint32_t FolderShare::resolve(const std::u16string &guest, std::string &host, bool &virtualRoot, bool allowWild) {
    virtualRoot = false;
    if (guest.empty() || (guest[0] != u'\\' && guest[0] != u'/')) return E_NAME;
    std::vector<std::string> comps;
    std::string cur;
    std::string s = toUtf8(guest);
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '\\' || s[i] == '/') {
            if (cur == "..") {
                if (comps.empty()) return E_NAME;
                comps.pop_back();
            } else if (!cur.empty() && cur != ".") {
                comps.push_back(cur);
            }
            cur.clear();
        } else {
            cur += s[i];
        }
    }
    for (size_t k = 0; k < comps.size(); k++) {
        const std::string &c = comps[k];
        for (unsigned char ch : c) {
            if (ch < 0x20 || strchr("<>:\"|", ch)) return E_NAME;
            if ((ch == '*' || ch == '?') && !(allowWild && k + 1 == comps.size())) return E_NAME;
        }
        if (c.back() == '.' || c.back() == ' ') return E_NAME;
    }
    size_t first = 0;
    std::string base;
    for (const Mount &o : overlays)
        if (!comps.empty() && !strcasecmp(o.name.c_str(), comps[0].c_str())) { base = o.path; first = 1; break; }
    if (!base.empty()) {
        // overlay folder (served by the app on every share)
    } else if (mounts.size() == 1 && mounts[0].name.empty()) {
        base = mounts[0].path;
    } else {
        if (comps.empty()) { virtualRoot = true; host.clear(); return E_OK; }
        for (const Mount &m : mounts)
            if (!strcasecmp(m.name.c_str(), comps[0].c_str())) { base = m.path; break; }
        if (base.empty()) return comps.size() > 1 ? E_NOPATH : E_NOFILE;
        first = 1;
    }
    host = base;
    for (size_t k = first; k < comps.size(); k++) host += "/" + matchCase(host, comps[k]);
    if (host.size() > 1024) return E_NAME;
    return E_OK;
}

uint32_t FolderShare::errnoToCode(int err, bool parentKnown) {
    switch (err) {
        case ENOENT: return parentKnown ? E_NOFILE : E_NOPATH;
        case ENOTDIR: return E_NOPATH;
        case EACCES:
        case EPERM:
        case EISDIR: return E_ACCESS;
        case EROFS: return 0x13;
        case EBUSY:
        case ETXTBSY: return E_SHARING;
        case EEXIST: return E_EXISTS;
        case ENAMETOOLONG: return E_NAME;
        default: return E_GENERAL;
    }
}

void FolderShare::fillInfo(uint8_t *pb, const std::string &host, bool isVirtualDir) {
    struct stat st;
    if (isVirtualDir || stat(host.c_str(), &st) != 0) {
        put16(pb, PB_ATTR, A_DIR);
        put32(pb, PB_FSIZE, 0);
        put32(pb, PB_TIME, 0);
        put32(pb, PB_CTIME, 0);
        return;
    }
    uint16_t attr = S_ISDIR(st.st_mode) ? A_DIR : A_ARCHIVE;
    if (!(st.st_mode & S_IWUSR)) attr |= A_RO;
    // The app's own folders (".wmdroid", the "2577" autorun folder) stay out of the user's way;
    // overlaid files meant for the user (device_wallpaper.bmp) are visible.
    for (const Mount &o : overlays)
        if (host == o.path && (o.name[0] == '.' || o.name == "2577")) attr |= A_HIDDEN;
    put16(pb, PB_ATTR, attr);
    put32(pb, PB_FSIZE, S_ISDIR(st.st_mode) ? 0 : (st.st_size >= 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)st.st_size));
    put32(pb, PB_TIME, toDos(st.st_mtime));
    put32(pb, PB_CTIME, toDos(st.st_ctime));
}

void FolderShare::listDir(const std::string &host, bool virtualRoot, const std::string &pattern, std::vector<Entry> &out) {
    out.clear();
    bool isRoot = virtualRoot || (mounts.size() == 1 && mounts[0].name.empty() && host == mounts[0].path);
    auto shadowed = [&](const char *name) {
        if (!isRoot) return false;
        for (const Mount &o : overlays)
            if (!strcasecmp(o.name.c_str(), name)) return true;
        return false;
    };
    if (isRoot)
        for (const Mount &o : overlays)
            if (wildMatch(pattern.c_str(), o.name.c_str())) out.push_back({o.name, o.path});
    if (virtualRoot) {
        for (const Mount &m : mounts)
            if (!shadowed(m.name.c_str()) && wildMatch(pattern.c_str(), m.name.c_str())) out.push_back({m.name, m.path});
        return;
    }
    DIR *d = opendir(host.c_str());
    if (!d) return;
    while (struct dirent *e = readdir(d)) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || shadowed(e->d_name)) continue;
        if (wildMatch(pattern.c_str(), e->d_name)) out.push_back({e->d_name, host + "/" + e->d_name});
    }
    closedir(d);
}

void FolderShare::setOverlays(const std::vector<Mount> &o) {
    std::lock_guard<std::mutex> lk(mu);
    overlays = o;
}

uint32_t FolderShare::call(uint32_t code, uint32_t pbPa) {
    std::lock_guard<std::mutex> lk(mu);
    if (traceCalls && (code == 0 || code == 4 || mounts.empty())) LOGI("folder share: op %02x (%s)", code, mounts.empty() ? "disabled" : "mounted");
    if (mounts.empty()) return E_FUNC;
    if (code == 0) return E_OK;          // PollCompletion: nothing pending (share present, see DePage)
    if (code == 4) return E_OK;          // GetDriveConfig: mounted
    if (code == 0x15) return kMaxIo;     // GetMaxIOSize
    uint8_t *pb = ram(pbPa, PB_BYTES);
    if (!pb || get16(pb, PB_SIZE) != PB_BYTES) return E_GENERAL;
    uint32_t r = dispatch(code, pb);
    if (traceCalls) LOGI("folder share: op %02x %s -> %x", code, toUtf8(pbName(pb, PB_NAMELEN, PB_NAME)).c_str(), r);
    return r;
}

uint32_t FolderShare::dispatch(uint32_t code, uint8_t *pb) {
    switch (code) {
        case 0x05: return opCreate(pb);
        case 0x06: return opOpen(pb);
        case 0x07: return opRead(pb);
        case 0x08: return opWrite(pb);
        case 0x09: return opSetEof(pb);
        case 0x0A: return opClose(pb);
        case 0x0B: return opGetSpace(pb);
        case 0x0C: return opMkDir(pb);
        case 0x0D: return opRmDir(pb);
        case 0x0E: return opSetAttributes(pb);
        case 0x0F: return opRename(pb);
        case 0x10: return opDelete(pb);
        case 0x11: return opGetInfo(pb);
        case 0x13: return opGetFcbInfo(pb);
        default: return E_FUNC;
    }
}

uint32_t FolderShare::opCreate(uint8_t *pb) {
    std::string host;
    bool vroot;
    if (uint32_t r = resolve(pbName(pb, PB_NAMELEN, PB_NAME), host, vroot)) return r;
    if (vroot) return E_ACCESS;
    int fd = open(host.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0666);
    if (fd < 0) return errnoToCode(errno, exists(parentOf(host)));
    close(fd);
    return E_OK;
}

uint32_t FolderShare::opOpen(uint8_t *pb) {
    int slot = -1;
    for (int i = 0; i < 40; i++) if (!handles[i].used) { slot = i; break; }
    if (slot < 0) return E_TOOMANY;
    std::u16string guest = pbName(pb, PB_NAMELEN, PB_NAME);
    std::string host;
    bool vroot;
    if (uint32_t r = resolve(guest, host, vroot)) return r;
    if (vroot) return E_ACCESS;
    uint32_t mode = get16(pb, PB_MODE), access = mode & 0xF, share = (mode >> 4) & 0xF;
    if (access > 2 || share < 1 || share > 4) return E_FUNC;
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return errnoToCode(errno, exists(parentOf(host)));
    if (S_ISDIR(st.st_mode)) return E_ACCESS;
    // Share modes between guest handles: 1 deny read+write, 2 deny write, 3 deny read, 4 none.
    auto wants = [](uint32_t a, bool write) { return write ? a != 0 : a != 1; };
    auto denies = [](uint32_t s, bool write) { return s == 1 || (write ? s == 2 : s == 3); };
    for (const Handle &h : handles) {
        if (!h.used || h.host != host) continue;
        uint32_t ha = h.mode & 0xF, hs = (h.mode >> 4) & 0xF;
        for (bool w : {false, true})
            if ((wants(access, w) && denies(hs, w)) || (wants(ha, w) && denies(share, w))) return E_SHARING;
    }
    int flags = access == 0 ? O_RDONLY : access == 1 ? O_WRONLY : O_RDWR;
    int fd = open(host.c_str(), flags);
    if (fd < 0) return errnoToCode(errno);
    Handle &h = handles[slot];
    h.used = true;
    h.fd = fd;
    h.mode = mode;
    h.host = host;
    h.guest = toUtf8(guest);
    put16(pb, PB_HANDLE, (uint16_t)slot);
    return E_OK;
}

uint32_t FolderShare::opRead(uint8_t *pb) {
    uint32_t idx = get16(pb, PB_HANDLE);
    if (idx >= 40 || !handles[idx].used) return E_HANDLE;
    Handle &h = handles[idx];
    if ((h.mode & 0xF) == 1) return E_ACCESS;
    uint32_t size = get32(pb, PB_FSIZE), pos = get32(pb, PB_POS);
    if (size > kMaxIo) size = kMaxIo;
    uint8_t *dst = size ? ram(get32(pb, PB_DTA), size) : nullptr;
    if (size && !dst) return E_READFAULT;
    ssize_t n = size ? pread(h.fd, dst, size, (off_t)pos) : 0;
    if (n < 0) return errnoToCode(errno);
    lseek(h.fd, (off_t)pos + n, SEEK_SET);
    put32(pb, PB_FSIZE, (uint32_t)n);
    return E_OK;
}

uint32_t FolderShare::opWrite(uint8_t *pb) {
    uint32_t idx = get16(pb, PB_HANDLE);
    if (idx >= 40 || !handles[idx].used) return E_HANDLE;
    Handle &h = handles[idx];
    if ((h.mode & 0xF) == 0) return E_ACCESS;
    uint32_t size = get32(pb, PB_FSIZE), pos = get32(pb, PB_POS);
    if (size > kMaxIo) size = kMaxIo;
    uint8_t *src = size ? ram(get32(pb, PB_DTA), size) : nullptr;
    if (size && !src) return E_WRITEFAULT;
    ssize_t n = size ? pwrite(h.fd, src, size, (off_t)pos) : 0;
    if (n < 0) return errno == ENOSPC ? E_GENERAL : errnoToCode(errno);
    lseek(h.fd, (off_t)pos + n, SEEK_SET);
    put32(pb, PB_FSIZE, (uint32_t)n);
    return E_OK;
}

uint32_t FolderShare::opSetEof(uint8_t *pb) {
    uint32_t idx = get16(pb, PB_HANDLE);
    if (idx >= 40 || !handles[idx].used) return E_HANDLE;
    Handle &h = handles[idx];
    if ((h.mode & 0xF) == 0) return E_ACCESS;
    uint32_t pos = get32(pb, PB_POS);
    if (ftruncate(h.fd, (off_t)pos) != 0) return errnoToCode(errno);
    lseek(h.fd, (off_t)pos, SEEK_SET);
    return E_OK;
}

uint32_t FolderShare::opClose(uint8_t *pb) {
    uint32_t idx = get16(pb, PB_HANDLE);
    if (idx >= 40 || !handles[idx].used) return E_HANDLE;
    close(handles[idx].fd);
    handles[idx] = Handle();
    return E_OK;
}

uint32_t FolderShare::opGetSpace(uint8_t *pb) {
    struct statvfs vs;
    if (statvfs(mounts[0].path.c_str(), &vs) != 0) return errnoToCode(errno);
    // The phone's own storage figures, in 32 KB units. DE clamps the byte counts to 4 GiB first;
    // we don't, so a modern phone shows its real size (units fit 32 bits up to 128 TiB).
    uint64_t freeB = (uint64_t)vs.f_bavail * vs.f_frsize, total = (uint64_t)vs.f_blocks * vs.f_frsize;
    uint64_t freeU = freeB / 32768, totalU = total / 32768;
    put32(pb, PB_FSIZE, (uint32_t)(freeU > 0xFFFFFFFFull ? 0xFFFFFFFFull : freeU));
    put32(pb, PB_POS, (uint32_t)(totalU > 0xFFFFFFFFull ? 0xFFFFFFFFull : totalU));
    return E_OK;
}

uint32_t FolderShare::opMkDir(uint8_t *pb) {
    std::string host;
    bool vroot;
    if (uint32_t r = resolve(pbName(pb, PB_NAMELEN, PB_NAME), host, vroot)) return r;
    if (vroot) return E_GENERAL;
    if (mkdir(host.c_str(), 0777) != 0) return errno == EEXIST ? E_GENERAL : errnoToCode(errno, false);
    return E_OK;
}

uint32_t FolderShare::opRmDir(uint8_t *pb) {
    std::string host;
    bool vroot;
    if (uint32_t r = resolve(pbName(pb, PB_NAMELEN, PB_NAME), host, vroot)) return r;
    if (vroot) return E_ACCESS;
    if (rmdir(host.c_str()) != 0) {
        if (errno == ENOTEMPTY || errno == EEXIST) return E_GENERAL;
        if (errno == ENOTDIR) return E_ACCESS;
        return errnoToCode(errno, exists(parentOf(host)));
    }
    return E_OK;
}

uint32_t FolderShare::opSetAttributes(uint8_t *pb) {
    std::string host;
    bool vroot;
    if (uint32_t r = resolve(pbName(pb, PB_NAMELEN, PB_NAME), host, vroot)) return r;
    if (vroot) return E_OK;
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return errnoToCode(errno, exists(parentOf(host)));
    uint16_t attr = get16(pb, PB_ATTR);
    if (!S_ISDIR(st.st_mode)) {
        mode_t m = st.st_mode & 0777;
        m = (attr & A_RO) ? (m & ~0222u) : (m | 0200u);
        chmod(host.c_str(), m);  // may be unsupported on shared storage; not an error for the guest
    }
    uint32_t t = get32(pb, PB_TIME);
    if (t) {
        time_t when;
        if (!fromDos(t, when)) return E_GENERAL;
        struct timespec ts[2];
        ts[0].tv_nsec = UTIME_OMIT;
        ts[0].tv_sec = 0;
        ts[1].tv_sec = when;
        ts[1].tv_nsec = 0;
        if (utimensat(AT_FDCWD, host.c_str(), ts, 0) != 0) return errnoToCode(errno);
    }
    return E_OK;
}

uint32_t FolderShare::opRename(uint8_t *pb) {
    std::string from, to;
    bool v1, v2;
    if (uint32_t r = resolve(pbName(pb, PB_NAMELEN, PB_NAME), from, v1)) return r;
    if (uint32_t r = resolve(pbName(pb, PB_NAME2LEN, PB_NAME2), to, v2)) return r;
    if (v1 || v2) return E_ACCESS;
    if (!exists(from)) return exists(parentOf(from)) ? E_NOFILE : E_NOPATH;
    if (exists(to) && strcasecmp(from.c_str(), to.c_str())) return E_GENERAL;
    for (const Handle &h : handles)
        if (h.used && (h.host == from || h.host.compare(0, from.size() + 1, from + "/") == 0)) return E_SHARING;
    if (rename(from.c_str(), to.c_str()) != 0) return errno == EXDEV ? E_GENERAL : errnoToCode(errno, false);
    return E_OK;
}

uint32_t FolderShare::opDelete(uint8_t *pb) {
    std::string host;
    bool vroot;
    if (uint32_t r = resolve(pbName(pb, PB_NAMELEN, PB_NAME), host, vroot)) return r;
    if (vroot) return E_ACCESS;
    struct stat st;
    if (stat(host.c_str(), &st) != 0) return errnoToCode(errno, exists(parentOf(host)));
    if (S_ISDIR(st.st_mode)) return E_ACCESS;
    for (const Handle &h : handles)
        if (h.used && h.host == host) return E_SHARING;
    if (unlink(host.c_str()) != 0) return errnoToCode(errno);
    return E_OK;
}

uint32_t FolderShare::opGetInfo(uint8_t *pb) {
    int16_t index = (int16_t)get16(pb, PB_INDEX);
    uint32_t tid = get32(pb, PB_TID);
    if (index == -1) {  // existence check / stat
        if (tid != 0xFFFFFFFFu) return E_FUNC;
        std::string host;
        bool vroot;
        if (uint32_t r = resolve(pbName(pb, PB_NAMELEN, PB_NAME), host, vroot)) return r;
        if (!vroot && !exists(host)) return exists(parentOf(host)) ? E_NOFILE : E_NOPATH;
        fillInfo(pb, host, vroot);
        return E_OK;
    }
    if (index == 0) {  // FindFirst
        if (tid != 0xFFFFFFFFu && tid >= 40) return E_GENERAL;
        std::u16string g = pbName(pb, PB_NAMELEN, PB_NAME);
        size_t sep = g.find_last_of(u"\\/");
        if (sep == std::u16string::npos) return E_NAME;
        std::u16string dir = g.substr(0, sep + 1);
        std::string pattern = toUtf8(g.substr(sep + 1));
        if (pattern.empty()) pattern = "*";
        std::string host;
        bool vroot;
        if (uint32_t r = resolve(dir, host, vroot)) return r;
        struct stat st;
        if (!vroot && (stat(host.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) return E_NOPATH;
        std::vector<Entry> all, matches;
        listDir(host, vroot, "*", all);
        listDir(host, vroot, pattern, matches);
        if (matches.empty()) return all.empty() ? E_NOMORE : E_NOFILE;
        fillInfo(pb, matches[0].host, false);
        if (tid != 0xFFFFFFFFu) {
            putName(pb, matches[0].name);
            Find &f = finds[tid];
            f.used = true;
            f.entries = std::move(matches);
            f.next = 1;
        }
        return E_OK;
    }
    // FindNext
    if (tid >= 40 || !finds[tid].used) return E_GENERAL;
    Find &f = finds[tid];
    if (f.next >= f.entries.size()) {
        f = Find();
        return E_NOMORE;
    }
    const Entry &e = f.entries[f.next++];
    fillInfo(pb, e.host, false);
    putName(pb, e.name);
    return E_OK;
}

uint32_t FolderShare::opGetFcbInfo(uint8_t *pb) {
    uint32_t idx = get16(pb, PB_HANDLE);
    if (idx >= 40 || !handles[idx].used) return E_HANDLE;
    Handle &h = handles[idx];
    struct stat st;
    if (fstat(h.fd, &st) != 0) return errnoToCode(errno);
    off_t pos = lseek(h.fd, 0, SEEK_CUR);
    put32(pb, PB_POS, pos >= 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)pos);
    put32(pb, PB_FSIZE, st.st_size >= 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)st.st_size);
    uint16_t attr = A_ARCHIVE;
    if (!(st.st_mode & S_IWUSR)) attr |= A_RO;
    put16(pb, PB_ATTR, attr);
    put16(pb, PB_MODE, (uint16_t)h.mode);
    putName(pb, h.guest);
    return E_OK;
}

}  // namespace wm
