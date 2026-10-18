// HTC ROM kit (romkit.h). Ported from tools/hd2/*.py; formats in docs/research-hd2-port.md.
#include "htc/romkit.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>

namespace htc {

namespace {

uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t u32(const uint8_t *p) { return uint32_t(p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24); }
void put16(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void put32(uint8_t *p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

std::string lower(std::string s) {
    for (char &c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}

bool endsWith(const std::string &s, const char *tail) {
    size_t n = strlen(tail);
    return s.size() >= n && lower(s.substr(s.size() - n)) == tail;
}

/** UTF-16LE to UTF-8 (a leading BOM dropped). */
std::string utf8(const uint8_t *p, size_t bytes) {
    std::string out;
    size_t i = 0;
    if (bytes >= 2 && p[0] == 0xFF && p[1] == 0xFE) i = 2;
    for (; i + 1 < bytes; i += 2) {
        uint32_t c = u16(p + i);
        if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes) {
            uint32_t lo = u16(p + i + 2);
            if (lo >= 0xDC00 && lo < 0xE000) { c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        if (c < 0x80) out += char(c);
        else if (c < 0x800) { out += char(0xC0 | c >> 6); out += char(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { out += char(0xE0 | c >> 12); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
        else { out += char(0xF0 | c >> 18); out += char(0x80 | (c >> 12 & 0x3F)); out += char(0x80 | (c >> 6 & 0x3F)); out += char(0x80 | (c & 0x3F)); }
    }
    return out;
}

bool writeFile(const std::string &path, const std::vector<uint8_t> &d) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = d.empty() || fwrite(d.data(), 1, d.size(), f) == d.size();
    return fclose(f) == 0 && ok;
}

bool makeDir(const std::string &path) { return mkdir(path.c_str(), 0775) == 0 || errno == EEXIST; }

// [MS-XCA] LZ77+Huffman ("XPH"); the extended-length byte is read before the offset bits.
bool xph(const uint8_t *src, size_t n, uint8_t *out, size_t usize) {
    auto r16 = [&](size_t p) -> uint32_t { return p + 1 < n ? u16(src + p) : p < n ? src[p] : 0; };
    std::vector<uint16_t> sym(32768);
    std::vector<uint8_t> len(32768);
    size_t pos = 0, o = 0;
    while (o < usize) {
        if (pos + 256 > n) return false;
        uint8_t lens[512];
        for (int i = 0; i < 256; i++) { lens[2 * i] = src[pos + i] & 15; lens[2 * i + 1] = src[pos + i] >> 4; }
        pos += 256;
        size_t start = 0;  // canonical code, 15-bit lookup table
        for (int L = 1; L <= 15; L++)
            for (int s = 0; s < 512; s++)
                if (lens[s] == L) {
                    size_t span = size_t(1) << (15 - L);
                    if (start + span > 32768) return false;
                    std::fill(sym.begin() + start, sym.begin() + start + span, uint16_t(s));
                    std::fill(len.begin() + start, len.begin() + start + span, uint8_t(L));
                    start += span;
                }
        std::fill(len.begin() + start, len.end(), uint8_t(0));
        uint32_t nb = r16(pos) << 16 | r16(pos + 2);
        pos += 4;
        int extra = 16;
        size_t blockEnd = o + 65536;
        while (o < usize && o < blockEnd) {
            uint32_t idx = nb >> 17;
            int L = len[idx];
            if (!L) return false;
            uint32_t s = sym[idx];
            nb <<= L;
            extra -= L;
            if (extra < 0) { nb |= r16(pos) << -extra; extra += 16; pos += 2; }
            if (s < 256) { out[o++] = uint8_t(s); continue; }
            s -= 256;
            uint32_t length = s & 15, olog = s >> 4;
            if (length == 15) {
                if (pos >= n) return false;
                length = src[pos++];
                if (length == 255) {
                    length = r16(pos);
                    pos += 2;
                    if (length == 0) {
                        if (pos + 4 > n) return false;
                        length = u32(src + pos);
                        pos += 4;
                    }
                    length -= 15;
                }
                length += 15;
            }
            length += 3;
            uint32_t off = (nb >> 1 | 0x80000000u) >> (31 - olog);
            nb = olog ? nb << olog : nb;
            extra -= int(olog);
            if (extra < 0) { nb |= r16(pos) << -extra; extra += 16; pos += 2; }
            if (off > o) return false;
            for (uint32_t k = 0; k < length && o < usize; k++, o++) out[o] = out[o - off];
        }
    }
    return true;
}

}  // namespace

// ---- byte sources: file -> NBH container -> NAND sectors -> partition ----

class Source {
public:
    virtual ~Source() = default;
    virtual bool read(uint64_t off, void *p, size_t n) = 0;
    virtual uint64_t size() const = 0;
};

namespace {

class FdSource : public Source {
public:
    explicit FdSource(int fd) : fd_(fd) {
        struct stat st;
        size_ = fstat(fd, &st) == 0 ? uint64_t(st.st_size) : 0;
    }
    bool read(uint64_t off, void *p, size_t n) override {
        auto *b = static_cast<uint8_t *>(p);
        while (n) {
            ssize_t r = pread(fd_, b, n, off_t(off));
            if (r <= 0) return false;
            b += r; off += uint64_t(r); n -= size_t(r);
        }
        return true;
    }
    uint64_t size() const override { return size_; }

private:
    int fd_;
    uint64_t size_;
};

// R000FF NBH: 9-byte block headers (u32 len, u32 siglen, u8 flag) from 0x17, each followed by the
// data block and its signature; the data blocks joined are the inner HTCIMAGE.
class NbhSource : public Source {
public:
    explicit NbhSource(Source &f) : f_(f) {
        uint64_t at = 0x17, inner = 0;
        uint8_t h[9];
        while (at + 9 <= f.size() && f.read(at, h, 9)) {
            uint32_t len = u32(h), sig = u32(h + 4);
            if (!len) break;
            blocks_.push_back({inner, at + 9, len});
            inner += len;
            at += 9 + uint64_t(len) + sig;
        }
        size_ = inner;
    }
    bool read(uint64_t off, void *p, size_t n) override {
        auto *b = static_cast<uint8_t *>(p);
        auto it = std::upper_bound(blocks_.begin(), blocks_.end(), off, [](uint64_t v, const Block &x) { return v < x.inner; });
        if (it == blocks_.begin()) return false;
        --it;
        while (n) {
            if (it == blocks_.end()) return false;
            uint64_t in = off - it->inner;
            size_t take = size_t(std::min<uint64_t>(n, it->len - in));
            if (!f_.read(it->file + in, b, take)) return false;
            b += take; off += take; n -= take;
            ++it;
        }
        return true;
    }
    uint64_t size() const override { return size_; }

private:
    struct Block { uint64_t inner, file; uint32_t len; };
    Source &f_;
    std::vector<Block> blocks_;
    uint64_t size_;
};

// NAND: 0x800-byte sectors each followed by an 8-byte tag (u32 logical sector, u32 flags). A later
// copy of a logical sector wins, as in nbh2nb.py.
class NandSource : public Source {
public:
    NandSource(Source &s, uint64_t off, uint64_t len) : s_(s), off_(off) {
        uint64_t n = len / 0x808;
        map_.assign(size_t(n), UINT32_MAX);
        std::vector<uint8_t> buf(0x808 * 256);
        for (uint64_t i = 0; i < n; i += 256) {
            size_t cnt = size_t(std::min<uint64_t>(256, n - i));
            if (!s.read(off + i * 0x808, buf.data(), cnt * 0x808)) break;
            for (size_t k = 0; k < cnt; k++) {
                uint32_t lsn = u32(&buf[k * 0x808 + 0x800]);
                if (lsn < n) map_[lsn] = uint32_t(i + k);
            }
        }
    }
    bool read(uint64_t off, void *p, size_t n) override {
        auto *b = static_cast<uint8_t *>(p);
        while (n) {
            uint64_t sec = off / 0x800;
            size_t in = size_t(off % 0x800), take = std::min<size_t>(n, 0x800 - in);
            if (sec >= map_.size()) return false;
            if (map_[sec] == UINT32_MAX) memset(b, 0, take);
            else if (!s_.read(off_ + uint64_t(map_[sec]) * 0x808 + in, b, take)) return false;
            b += take; off += take; n -= take;
        }
        return true;
    }
    uint64_t size() const override { return uint64_t(map_.size()) * 0x800; }

private:
    Source &s_;
    uint64_t off_;
    std::vector<uint32_t> map_;
};

class SliceSource : public Source {
public:
    SliceSource(Source &s, uint64_t off, uint64_t len) : s_(s), off_(off), len_(len) {}
    bool read(uint64_t off, void *p, size_t n) override { return off + n <= len_ && s_.read(off_ + off, p, n); }
    uint64_t size() const override { return len_; }

private:
    Source &s_;
    uint64_t off_, len_;
};

constexpr uint32_t T_FILE = 0xFFFFFEFE, T_PLAIN = 0xFFFFF6FE, T_NAME = 0xFFFFFEFB, T_SECT = 0xFFFFF6FD;
constexpr uint32_t SIG_HDR = 0x2F5314CE;

}  // namespace

// ---- IMGFS ----

struct Stream { uint32_t size = 0, tbl = 0, tblsize = 0; };
struct FsFile {
    std::string name;
    Stream data;
    uint32_t attr = 0;
    std::vector<Stream> sects;  // a ROM module's sections (header stream is [data])
};

class Imgfs {
public:
    explicit Imgfs(Source &s) : s_(s) {}

    bool load(std::string *err) {
        uint8_t boot[0x30];
        if (!s_.read(0, boot, sizeof boot)) { *err = "can't read IMGFS"; return false; }
        bph_ = u32(boot + 0x1C);
        sect_ = u32(boot + 0x24);
        if (bph_ != 0x34 || sect_ < 0x200 || sect_ > 0x10000) { *err = "not an IMGFS partition"; return false; }
        std::vector<uint8_t> blk(sect_);
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> files;
        for (uint64_t off = sect_, guard = 0; off && guard < 1000000; guard++) {
            if (!s_.read(off, blk.data(), sect_) || u32(blk.data()) != SIG_HDR) { *err = "bad IMGFS header chain"; return false; }
            for (uint32_t p = 8; p + bph_ <= sect_; p += bph_) {
                std::vector<uint8_t> e(blk.begin() + p, blk.begin() + p + bph_);
                uint32_t t = u32(e.data());
                if (t == T_SECT || t == T_NAME) entries_[uint32_t(off + p)] = e;
                else if (t == T_FILE || t == T_PLAIN) files.push_back({uint32_t(off + p), e});
            }
            off = u32(blk.data() + 4);
        }
        for (auto &f : files) {
            FsFile ff;
            const uint8_t *e = f.second.data();
            ff.name = name(e + 0x0C);
            ff.data = {u32(e + 0x18), u32(e + 0x2C), u32(e + 0x30)};
            ff.attr = u32(e + 0x1C);
            for (uint32_t s = u32(e + 8), guard = 0; s && guard < 64; guard++) {
                auto it = entries_.find(s);
                if (it == entries_.end() || u32(it->second.data()) != T_SECT) break;
                const uint8_t *se = it->second.data();
                ff.sects.push_back({u32(se + 0x18), u32(se + 0x1C), u32(se + 0x20)});
                s = u32(se + 8);
            }
            byName_[lower(ff.name)] = files_.size();
            files_.push_back(std::move(ff));
        }
        return true;
    }

    const FsFile *find(const std::string &n) const {
        auto it = byName_.find(lower(n));
        return it == byName_.end() ? nullptr : &files_[it->second];
    }
    const std::vector<FsFile> &files() const { return files_; }

    bool stream(const Stream &st, std::vector<uint8_t> &out) {
        out.clear();
        if (st.tblsize > 0x1000000) return false;
        std::vector<uint8_t> tbl(st.tblsize);
        if (st.tblsize && !s_.read(st.tbl, tbl.data(), tbl.size())) return false;
        std::vector<uint8_t> raw, dec;
        for (size_t p = 0; out.size() < st.size && p + 8 <= tbl.size(); p += 8) {
            uint16_t cs = u16(&tbl[p]), us = u16(&tbl[p + 2]);
            uint32_t off = u32(&tbl[p + 4]);
            if (!cs && !us) continue;
            raw.resize(cs);
            if (!s_.read(off, raw.data(), cs)) return false;
            if (cs == us) { out.insert(out.end(), raw.begin(), raw.end()); continue; }
            dec.resize(us);
            if (!xph(raw.data(), raw.size(), dec.data(), us)) return false;
            out.insert(out.end(), dec.begin(), dec.end());
        }
        if (out.size() < st.size) return false;
        out.resize(st.size);
        return true;
    }

    /** A plain file's bytes, or a ROM module rebuilt into a PE file. */
    bool get(const FsFile &f, std::vector<uint8_t> &out, std::string *note);

private:
    // cch <= 4: inline UTF-16; else an offset to a name entry (+4) or straight to raw UTF-16.
    std::string name(const uint8_t *at) {
        uint16_t cch = u16(at);
        if (cch <= 4) return utf8(at + 4, cch * 2u);
        uint32_t noff = u32(at + 8);
        auto it = entries_.find(noff);
        if (it != entries_.end() && u32(it->second.data()) == T_NAME) noff += 4;
        std::vector<uint8_t> b(cch * 2u);
        if (!s_.read(noff, b.data(), b.size())) return std::string();
        return utf8(b.data(), b.size());
    }

    Source &s_;
    uint32_t bph_ = 0, sect_ = 0;
    std::map<uint32_t, std::vector<uint8_t>> entries_;
    std::vector<FsFile> files_;
    std::map<std::string, size_t> byName_;
};

// ---- ROM module -> PE (imgfs.py rebuild_pe / rebuild_relocs) ----

namespace {

constexpr int ROM_EXTRA = 9;
constexpr uint32_t RAM_DLL_BASE = 0x10000000;

struct Sec { uint32_t vsize, rva; std::vector<uint8_t> data; uint32_t real, flags; };

// romimage keeps relocations in its own compressed format and may move writable sections to
// o32_realaddr. Recover the fixups instead: literal-pool words (found via PC-relative LDRs) in code
// and aligned words in data that point into the image or a moved section; moved targets are pointed
// back at vbase+rva, then every fixup is rebased to [newbase]. Returns IMAGE_BASE_RELOCATION blocks.
std::vector<uint8_t> rebuildRelocs(std::vector<Sec> &sects, uint32_t vbase, uint32_t vsize, bool hasReloc, uint32_t relocRva,
                                   uint32_t newbase, size_t *nfix) {
    struct Moved { uint32_t real, vs, rva; };
    std::vector<Moved> moved;
    for (auto &s : sects)
        if (s.real && s.real != vbase + s.rva && !(hasReloc && s.rva == relocRva)) moved.push_back({s.real, s.vsize, s.rva});
    std::vector<uint32_t> sites;
    for (auto &s : sects) {
        if (hasReloc && s.rva == relocRva) continue;
        size_t n = s.data.size() & ~size_t(3);
        std::vector<size_t> offs;
        if (s.flags & 0x20) {  // code: literal-pool slots only
            std::set<size_t> slots;
            for (size_t o = 0; o < n; o += 4) {
                uint32_t w = u32(&s.data[o]);
                if ((w & 0x0F7F0000) == 0x051F0000 && (w >> 28) != 0xF) {  // LDR Rd, [pc, #+/-imm]
                    int64_t imm = w & 0xFFF, t = int64_t(o) + 8 + ((w & 0x00800000) ? imm : -imm);
                    if (t >= 0 && t <= int64_t(n) - 4 && t % 4 == 0) slots.insert(size_t(t));
                }
            }
            offs.assign(slots.begin(), slots.end());
        } else {
            for (size_t o = 0; o < n; o += 4) offs.push_back(o);
        }
        for (size_t o : offs) {
            uint32_t v = u32(&s.data[o]);
            bool hit = false;
            for (auto &m : moved)
                if (v >= m.real && v < m.real + std::max<uint32_t>(m.vs, 1)) {
                    put32(&s.data[o], vbase + m.rva + v - m.real);
                    sites.push_back(s.rva + uint32_t(o));
                    hit = true;
                    break;
                }
            if (!hit && v >= vbase && v < vbase + vsize) sites.push_back(s.rva + uint32_t(o));
        }
    }
    for (uint32_t r : sites)
        for (auto &s : sects)
            if (r >= s.rva && r < s.rva + s.data.size()) {
                uint8_t *p = &s.data[r - s.rva];
                put32(p, u32(p) + newbase - vbase);
                break;
            }
    std::sort(sites.begin(), sites.end());
    std::map<uint32_t, std::vector<uint16_t>> pages;
    for (uint32_t r : sites) pages[r & ~0xFFFu].push_back(uint16_t(0x3000 | (r & 0xFFF)));
    std::vector<uint8_t> out;
    for (auto &pg : pages) {
        auto &e = pg.second;
        if (e.size() & 1) e.push_back(0);
        size_t at = out.size();
        out.resize(at + 8 + 2 * e.size());
        put32(&out[at], pg.first);
        put32(&out[at + 4], uint32_t(8 + 2 * e.size()));
        for (size_t i = 0; i < e.size(); i++) put16(&out[at + 8 + 2 * i], e[i]);
    }
    *nfix = sites.size();
    return out;
}

}  // namespace

bool Imgfs::get(const FsFile &f, std::vector<uint8_t> &out, std::string *note) {
    if (f.sects.empty()) return stream(f.data, out);
    std::vector<uint8_t> hdr;
    if (!stream(f.data, hdr) || hdr.size() < 0x70) return false;
    uint16_t objcnt = u16(&hdr[0]);
    uint32_t entry = u32(&hdr[4]), vbase = u32(&hdr[8]);
    uint16_t smaj = u16(&hdr[12]), smin = u16(&hdr[14]);
    uint32_t stack = u32(&hdr[16]), vsize = u32(&hdr[20]), ts = u32(&hdr[32]);
    uint32_t units[ROM_EXTRA][2];
    for (int i = 0; i < ROM_EXTRA; i++) { units[i][0] = u32(&hdr[0x24 + i * 8]); units[i][1] = u32(&hdr[0x28 + i * 8]); }
    uint16_t subsys = u16(&hdr[0x6C]);
    if (hdr.size() < 0x70 + objcnt * 24u) return false;
    std::vector<Sec> sects;
    for (int i = 0; i < objcnt; i++) {
        const uint8_t *o = &hdr[0x70 + i * 24];
        Sec s{u32(o), u32(o + 4), {}, u32(o + 16), u32(o + 20) & ~0x2000u};
        if (size_t(i) < f.sects.size() && !stream(f.sects[i], s.data)) return false;
        sects.push_back(std::move(s));
    }
    bool hasReloc = units[5][1] != 0;
    uint32_t relocRva = units[5][0];
    // A RAM DLL preferring a slot-1 base (XIP DLL code) is loaded elsewhere but never relocated by the
    // WM 6.5 loader: rebase DLLs to an ordinary RAM-DLL base.
    uint32_t rombase = vbase;
    bool dll = endsWith(f.name, ".dll") || endsWith(f.name, ".dll1") || endsWith(f.name, ".cpl");
    if (dll) vbase = RAM_DLL_BASE;
    size_t nfix = 0;
    std::vector<uint8_t> relocs = rebuildRelocs(sects, rombase, vsize, hasReloc, relocRva, vbase, &nfix);
    Sec *rsec = nullptr;
    if (hasReloc)
        for (auto &s : sects) if (s.rva == relocRva) { rsec = &s; break; }
    if (rsec) {
        rsec->vsize = uint32_t(std::max<size_t>(relocs.size(), 4));
        rsec->data = relocs;
        rsec->flags = 0x42000040;  // romimage's bit 0x2 marks its compressed format: the loader would skip ours
    } else {  // stripped: append a new .reloc section
        relocRva = (vsize + 0xFFF) & ~0xFFFu;
        sects.push_back({uint32_t(std::max<size_t>(relocs.size(), 4)), relocRva, relocs, 0, 0x42000040});
        vsize = relocRva + ((uint32_t(relocs.size()) + 0xFFF) & ~0xFFFu);
    }
    units[5][0] = relocRva;
    units[5][1] = uint32_t(relocs.size());
    auto secname = [&](const Sec &s) -> const char * {
        static const struct { int idx; const char *nm; } named[] = {{2, ".rsrc"}, {3, ".pdata"}, {5, ".reloc"}};
        for (auto &n : named) if (units[n.idx][0] == s.rva && units[n.idx][1]) return n.nm;
        if (s.flags & 0x20) return ".text";
        if (s.flags & 0x80000000u) return ".data";
        return ".rdata";
    };
    const uint32_t fa = 0x200;
    size_t nsec = sects.size();
    uint32_t hsize = uint32_t((0x80 + 4 + 20 + 224 + 40 * nsec + fa - 1) / fa * fa);
    out.assign(hsize, 0);
    out[0] = 'M'; out[1] = 'Z';
    put32(&out[0x3C], 0x80);
    memcpy(&out[0x80], "PE\0\0", 4);
    uint16_t chars = 0x0102 | (dll ? 0x2000 : 0);
    if (!units[5][1]) chars |= 0x0001;  // relocations stripped
    uint8_t *fh = &out[0x84];
    put16(fh, 0x01C2); put16(fh + 2, uint16_t(nsec)); put32(fh + 4, ts); put16(fh + 16, 224); put16(fh + 18, chars);
    uint8_t *o = &out[0x98];
    uint32_t codeSize = 0;
    for (auto &s : sects) if (s.flags & 0x20) codeSize += uint32_t(s.data.size());
    put16(o, 0x10B); o[2] = 6; put32(o + 4, codeSize); put32(o + 16, entry); put32(o + 20, 0x1000);
    put32(o + 28, vbase); put32(o + 32, 0x1000); put32(o + 36, fa);
    put16(o + 40, 4); put16(o + 48, smaj); put16(o + 50, smin);
    // SizeOfImage must cover every section; romimage's vsize leaves out .reloc, and the loader then
    // ignores the relocations (the DLL runs unrelocated).
    for (auto &s : sects) vsize = std::max(vsize, (s.rva + std::max<uint32_t>(s.vsize, uint32_t(s.data.size())) + 0xFFF) & ~0xFFFu);
    put32(o + 56, vsize); put32(o + 60, hsize); put16(o + 68, subsys);
    put32(o + 72, stack); put32(o + 76, 0x1000); put32(o + 80, 0x100000); put32(o + 84, 0x1000); put32(o + 92, 16);
    for (int i = 0; i < ROM_EXTRA; i++) { put32(o + 96 + i * 8, units[i][0]); put32(o + 100 + i * 8, units[i][1]); }
    const size_t sh = 0x98 + 224;  // section headers
    for (size_t i = 0; i < nsec; i++) {
        Sec &s = sects[i];
        uint32_t raw = uint32_t((s.data.size() + fa - 1) / fa * fa);
        size_t at = out.size();
        uint8_t *h = &out[sh + i * 40];
        strncpy(reinterpret_cast<char *>(h), secname(s), 8);
        put32(h + 8, s.vsize); put32(h + 12, s.rva); put32(h + 16, raw); put32(h + 20, raw ? uint32_t(at) : 0); put32(h + 36, s.flags);
        out.resize(at + raw, 0);
        if (!s.data.empty()) memcpy(&out[at], s.data.data(), s.data.size());
    }
    if (note) {
        char b[96];
        snprintf(b, sizeof b, "module, base %08x, %zu fixups", rombase, nfix);
        *note = b;
    }
    return true;
}

// ---- PE helpers (stage.py) ----

namespace {

struct PeImport { size_t nameOff; std::string name; };

/** Import descriptor DLL names with their file offsets; empty if not a PE file. */
std::vector<PeImport> peImports(const std::vector<uint8_t> &d) {
    std::vector<PeImport> out;
    if (d.size() < 0x40 || d[0] != 'M' || d[1] != 'Z') return out;
    uint32_t pe = u32(&d[0x3C]);
    if (size_t(pe) + 24 + 224 > d.size() || memcmp(&d[pe], "PE\0\0", 4)) return out;
    uint32_t opt = pe + 24, osz = u16(&d[pe + 20]), nsec = u16(&d[pe + 6]);
    if (size_t(opt) + osz + nsec * 40u > d.size() || osz < 112) return out;
    auto off = [&](uint32_t r) -> int64_t {
        for (uint32_t i = 0; i < nsec; i++) {
            const uint8_t *s = &d[opt + osz + i * 40];
            uint32_t vs = u32(s + 8), va = u32(s + 12), rs = u32(s + 16), rp = u32(s + 20);
            if (r >= va && r < va + std::max(vs, rs)) return int64_t(rp) + r - va;
        }
        return -1;
    };
    uint32_t irva = u32(&d[opt + 104]);
    if (!irva) return out;
    for (int64_t o = off(irva); o >= 0 && size_t(o) + 20 <= d.size(); o += 20) {
        uint32_t nm = u32(&d[size_t(o) + 12]);
        if (!nm) break;
        int64_t no = off(nm);
        if (no < 0 || size_t(no) >= d.size()) break;
        size_t e = size_t(no);
        while (e < d.size() && d[e]) e++;
        out.push_back({size_t(no), std::string(d.begin() + no, d.begin() + e)});
    }
    return out;
}

/** Drops an Authenticode signature at the end of the file; true if there was one. */
bool stripSignature(std::vector<uint8_t> &d) {
    uint32_t pe = u32(&d[0x3C]), dir = pe + 24 + 96 + 4 * 8;
    if (size_t(dir) + 8 > d.size()) return false;
    uint32_t so = u32(&d[dir]), ss = u32(&d[dir + 4]);
    if (!so || !ss || size_t(so) + ss != d.size()) return false;
    d.resize(so);
    put32(&d[dir], 0);
    put32(&d[dir + 4], 0);
    return true;
}

// .dsm: u32 header size; +0x14 package-name length, +0x18 its offset; then 20-byte records
// (u32 name length, u32 flags 0x1000xxxx, u32 name offset, ...). Names UTF-16LE, no terminators.
bool parseDsm(const std::vector<uint8_t> &d, Package &p) {
    if (d.size() < 0x1C) return false;
    uint32_t hsz = u32(&d[0]), plen = u32(&d[0x14]), poff = u32(&d[0x18]);
    if (size_t(poff) + plen * 2u > d.size()) return false;
    p.name = utf8(&d[poff], plen * 2u);
    for (size_t q = hsz; q + 20 <= d.size(); q += 20) {
        uint32_t n = u32(&d[q]), flags = u32(&d[q + 4]), noff = u32(&d[q + 8]);
        if ((flags & 0xFFFF0000) != 0x10000000 || !noff || noff >= d.size() || size_t(noff) + n * 2u > d.size()) break;
        p.files.push_back(utf8(&d[noff], n * 2u));
    }
    return !p.name.empty();
}

/** "x" from Directory("...")/File("x","...") style arguments: the next quoted string at or after [at]. */
bool quoted(const std::string &s, size_t &at, std::string &out) {
    size_t a = s.find('"', at);
    if (a == std::string::npos) return false;
    size_t b = s.find('"', a + 1);
    if (b == std::string::npos) return false;
    out = s.substr(a + 1, b - a - 1);
    at = b + 1;
    return true;
}

std::string baseName(const std::string &p) {
    size_t s = p.find_last_of("\\/");
    return s == std::string::npos ? p : p.substr(s + 1);
}

}  // namespace

// ---- Rom ----

Rom::Rom() = default;
Rom::~Rom() = default;

bool Rom::open(int fd, std::string *err) {
    file_ = std::make_unique<FdSource>(fd);
    Source *fs = file_.get();
    uint8_t head[0x200] = {};
    if (!file_->read(0, head, std::min<uint64_t>(sizeof head, file_->size()))) { *err = "can't read the file"; return false; }
    Source *nb = nullptr;
    if (!memcmp(head, "R000FF", 6)) {  // HTC RUU .nbh
        nbh_ = std::make_unique<NbhSource>(*file_);
        uint8_t ih[0x1C0];
        if (nbh_->size() < sizeof ih || !nbh_->read(0, ih, sizeof ih)) { *err = "damaged NBH"; return false; }
        for (int i = 0; i < 32; i++) {
            uint32_t t = u32(ih + 0x40 + i * 4), o = u32(ih + 0xC0 + i * 4), l = u32(ih + 0x140 + i * 4);
            if (t == 0x400) { nand_ = std::make_unique<NandSource>(*nbh_, o, l); nb = nand_.get(); }
        }
        if (!nb) { *err = "the NBH has no OS section"; return false; }
    } else if (head[0x1FE] == 0x55 && head[0x1FF] == 0xAA) {
        nb = fs;  // os.nb (nbh2nb.py output)
    }
    Source *img = fs;
    if (nb) {
        uint8_t mbr[0x200];
        if (!nb->read(0, mbr, sizeof mbr)) { *err = "can't read the OS image"; return false; }
        img = nullptr;
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = mbr + 0x1BE + i * 16;
            if (e[4] == 0x25) {  // IMGFS
                part_ = std::make_unique<SliceSource>(*nb, uint64_t(u32(e + 8)) * 0x800, uint64_t(u32(e + 12)) * 0x800);
                img = part_.get();
            }
        }
        if (!img) { *err = "no IMGFS partition (is this a Windows Mobile 6.5 ROM?)"; return false; }
    }
    fs_ = std::make_unique<Imgfs>(*img);
    if (!fs_->load(err)) return false;
    packages_.clear();
    std::vector<uint8_t> d;
    for (auto &f : fs_->files()) {
        if (!endsWith(f.name, ".dsm") || !f.sects.empty() || !fs_->stream(f.data, d)) continue;
        Package p;
        if (parseDsm(d, p) && !p.files.empty()) packages_.push_back(std::move(p));
    }
    std::sort(packages_.begin(), packages_.end(), [](const Package &a, const Package &b) { return lower(a.name) < lower(b.name); });
    if (packages_.empty()) { *err = "no ROM packages found"; return false; }
    return true;
}

bool Rom::stage(const std::string &name, const std::string &outDir, const std::set<std::string> *have, std::string *log) {
    auto say = [&](const std::string &s) { if (log) *log += s + "\n"; };
    const Package *pkg = nullptr;
    int prefixHits = 0;
    for (auto &p : packages_) {
        if (p.name == name) { pkg = &p; prefixHits = 1; break; }
        if (lower(p.name).compare(0, name.size(), lower(name)) == 0) { pkg = &p; prefixHits++; }
    }
    if (!pkg || prefixHits != 1) { say("error: " + std::string(pkg ? "several packages match " : "no package ") + name); return false; }
    if (!makeDir(outDir) || !makeDir(outDir + "/deps")) { say("error: can't create " + outDir); return false; }

    std::string ini = "package=" + pkg->name + "\n";
    std::set<std::string> staged;   // lowercase names in the package
    std::vector<std::pair<std::string, std::vector<uint8_t>>> pes;  // binaries whose imports are followed
    std::vector<uint8_t> d;
    // Every staged EXE/DLL: DDRAW.dll -> HDRAW.dll (same length), then the signature goes.
    auto fixImports = [&](const std::string &fn, std::vector<uint8_t> &b) {
        bool changed = false;
        for (auto &im : peImports(b))
            if (lower(im.name) == "ddraw.dll") { memcpy(&b[im.nameOff], "HDRAW.dll", 9); changed = true; say("  " + fn + ": import " + im.name + " -> HDRAW.dll"); }
        if (changed && stripSignature(b)) say("  " + fn + ": signature removed");
    };
    for (auto &fn : pkg->files) {
        const FsFile *f = fs_->find(fn);
        if (!f) { say("  missing in ROM: " + fn); continue; }
        std::string note;
        if (!fs_->get(*f, d, &note)) { say("error: can't read " + fn); return false; }
        if (endsWith(fn, ".rgu")) {
            std::string reg = utf8(d.data(), d.size());
            std::string rn = pkg->name + ".reg";
            if (!writeFile(outDir + "/" + rn, std::vector<uint8_t>(reg.begin(), reg.end()))) { say("error: write " + rn); return false; }
            ini += "reg=" + rn + "\n";
            continue;
        }
        if (endsWith(fn, ".dsm")) continue;
        bool pe = endsWith(fn, ".exe") || endsWith(fn, ".dll") || endsWith(fn, ".cpl") || endsWith(fn, ".dll1");
        if (pe) fixImports(f->name, d);
        if (!writeFile(outDir + "/" + f->name, d)) { say("error: write " + f->name); return false; }
        if (!note.empty()) say("  " + f->name + ": " + note);
        ini += "file=" + f->name + "\n";
        staged.insert(lower(f->name));
        if (pe) pes.push_back({f->name, d});
    }
    // ROM DLLs the package needs that the guest lacks.
    std::set<std::string> seen, missing;
    size_t ndeps = 0;
    for (size_t i = 0; i < pes.size(); i++) {
        bool own = staged.count(lower(pes[i].first)) != 0;
        if (!own && !have) continue;  // without the guest's list, only the package's own imports
        for (auto &im : peImports(pes[i].second)) {
            std::string n = lower(im.name);
            if (staged.count(n) || seen.count(n) || (have && have->count(n))) continue;
            seen.insert(n);
            const FsFile *f = fs_->find(n);
            if (!f) { if (have) missing.insert(im.name); continue; }
            std::string note;
            if (!fs_->get(*f, d, &note)) { say("error: can't read " + f->name); return false; }
            fixImports(f->name, d);
            if (!writeFile(outDir + "/deps/" + f->name, d)) { say("error: write deps/" + f->name); return false; }
            say("  dep " + f->name + (note.empty() ? "" : ": " + note));
            ini += "dep=" + f->name + "\n";
            ndeps++;
            pes.push_back({f->name, d});
        }
    }
    for (auto &m : missing) { ini += "missing=" + m + "\n"; say("  not in the ROM or the guest: " + m); }
    // Start menu shortcuts: initflashfiles.dat lines Directory("<dir>"):-File("<name>","<source>").
    if (const FsFile *f = fs_->find("initflashfiles.dat"); f && fs_->get(*f, d, nullptr)) {
        std::string txt = utf8(d.data(), d.size());
        size_t a = 0;
        while (a < txt.size()) {
            size_t e = txt.find('\n', a);
            std::string line = txt.substr(a, e == std::string::npos ? std::string::npos : e - a);
            a = e == std::string::npos ? txt.size() : e + 1;
            size_t k = line.find_first_not_of(" \t");
            if (k == std::string::npos || line[k] == ';' || line.compare(k, 10, "Directory(") != 0) continue;
            size_t at = line.find(":-File(");
            std::string dir, nm, src;
            size_t q = k;
            if (at == std::string::npos || !quoted(line, q, dir) || q > at) continue;
            q = at;
            if (!quoted(line, q, nm) || !quoted(line, q, src)) continue;
            if (!staged.count(lower(baseName(src)))) continue;
            ini += "link=" + dir + "\\" + nm + "|" + src + "\n";
            say("  shortcut " + dir + "\\" + nm);
        }
    }
    // Last: the guest installs a folder only once install.ini is there.
    if (!writeFile(outDir + "/install.ini", std::vector<uint8_t>(ini.begin(), ini.end()))) { say("error: write install.ini"); return false; }
    say(pkg->name + ": " + std::to_string(staged.size()) + " files, " + std::to_string(ndeps) + " dependencies");
    return true;
}

}  // namespace htc
