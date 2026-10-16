// WMDroid camera bridge: see camera.h for the guest interface.
#include "camera.h"
#include "../arm/cpu.h"
#include "../arm/jit.h"
#include "../util/log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace wm {

namespace {

uint64_t hostNs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr uint32_t FOURCC_YV12 = 0x32315659;   // 'YV12'
constexpr uint32_t RESULT_FAULT = 0x80000000u;
constexpr uint32_t RESULT_UNSUPPORTED = 0xFFFFFFFFu;
constexpr uint64_t WANT_AFTER_FILL_NS = 2000000000ull;   // keep the host camera this long after a frame
constexpr uint64_t WANT_AFTER_OPEN_NS = 5000000000ull;   // ...and this long after the driver opened
constexpr uint64_t TARGET_ACTIVE_NS = 1500000000ull;     // sizes asked for recently are pre-rendered

inline uint8_t clamp8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint16_t rd16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

// Filter taps for resampling n samples to m: an area average when shrinking, linear interpolation
// when enlarging. Weights are in 1/4096 and sum to 4096 for each output sample.
struct Taps {
    std::vector<int> offs, count, idx, w;
};

void makeTaps(int n, int m, Taps &t) {
    t.offs.resize(m);
    t.count.resize(m);
    t.idx.clear();
    t.w.clear();
    for (int i = 0; i < m; i++) {
        t.offs[i] = (int)t.idx.size();
        if (n > m) {
            int64_t a = (int64_t)i * n, b = a + n;   // [a, b) in units of 1/m input sample
            int sum = 0;
            for (int k = (int)(a / m); (int64_t)k * m < b && k < n; k++) {
                int64_t lo = std::max(a, (int64_t)k * m), hi = std::min(b, (int64_t)(k + 1) * m);
                int wt = (int)((hi - lo) * 4096 / n);
                t.idx.push_back(k);
                t.w.push_back(wt);
                sum += wt;
            }
            t.w[t.offs[i]] += 4096 - sum;
        } else {
            int64_t pos = ((2 * (int64_t)i + 1) * n * 4096) / (2 * (int64_t)m) - 2048;   // 1/4096 units
            int64_t k0 = pos >= 0 ? pos >> 12 : -((-pos + 4095) >> 12);
            int f = (int)(pos - k0 * 4096);
            t.idx.push_back(std::clamp((int)k0, 0, n - 1));
            t.w.push_back(4096 - f);
            t.idx.push_back(std::clamp((int)k0 + 1, 0, n - 1));
            t.w.push_back(f);
        }
        t.count[i] = (int)t.idx.size() - t.offs[i];
    }
}

// Scales an RGB888 rectangle (rows `inStride` bytes apart) to ow x oh. Integer maths; the vertical
// pass runs row by row.
void scaleRgb(const uint8_t *in, int inStride, int iw, int ih, uint8_t *out, int ow, int oh) {
    thread_local Taps tx, ty;
    thread_local std::vector<uint8_t> tmp;
    thread_local std::vector<int> acc;
    makeTaps(iw, ow, tx);
    makeTaps(ih, oh, ty);
    tmp.resize((size_t)ow * ih * 3);
    for (int y = 0; y < ih; y++) {
        const uint8_t *row = in + (size_t)y * inStride;
        uint8_t *o = &tmp[(size_t)y * ow * 3];
        for (int i = 0; i < ow; i++, o += 3) {
            int r = 2048, g = 2048, b = 2048;
            for (int k = tx.offs[i], e = k + tx.count[i]; k < e; k++) {
                const uint8_t *p = row + tx.idx[k] * 3;
                r += tx.w[k] * p[0];
                g += tx.w[k] * p[1];
                b += tx.w[k] * p[2];
            }
            o[0] = (uint8_t)(r >> 12);
            o[1] = (uint8_t)(g >> 12);
            o[2] = (uint8_t)(b >> 12);
        }
    }
    acc.resize((size_t)ow * 3);
    for (int y = 0; y < oh; y++) {
        std::fill(acc.begin(), acc.end(), 2048);
        for (int k = ty.offs[y], e = k + ty.count[y]; k < e; k++) {
            const uint8_t *row = &tmp[(size_t)ty.idx[k] * ow * 3];
            int wt = ty.w[k];
            for (int i = 0; i < ow * 3; i++) acc[i] += wt * row[i];
        }
        uint8_t *o = out + (size_t)y * ow * 3;
        for (int i = 0; i < ow * 3; i++) o[i] = (uint8_t)(acc[i] >> 12);
    }
}

// xorshift64*: cheap per-frame noise.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint32_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return (uint32_t)((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    // Roughly normal, unit variance (sum of four uniforms).
    float gauss() {
        uint32_t a = next(), b = next();
        int sum = (int)(a & 0xFFFF) + (int)(a >> 16) + (int)(b & 0xFFFF) + (int)(b >> 16) - 2 * 65536;
        return sum * (1.0f / 65536.0f) * 0.866f;  // 4 uniforms on [-.5,.5): variance 1/3
    }
};

// ---- The 2009 phone camera ----
// A 2-3 MP CMOS phone of 2008-2009 as its pictures really looked: little real detail (tiny sensor,
// cheap fixed-focus optics, heavy noise reduction), strong noise with a fixed pattern, low dynamic
// range (crushed blacks, highlights clipping early and hard), a cool/green cast with muted, slightly
// wrong colour, purple fringes, JPEG blocking, heavy vignetting and a slow, smeary sensor.
// Integer maths on the small frame; scratch buffers are reused.

// Roughly normal noise with a standard deviation of ~148 (four 8-bit uniforms).
inline int gauss148(Rng &r) {
    uint32_t a = r.next();
    return (int)(a & 255) + (int)((a >> 8) & 255) + (int)((a >> 16) & 255) + (int)(a >> 24) - 510;
}

inline uint32_t hash2(uint32_t x, uint32_t y) {
    uint32_t h = x * 0x8DA6B343u ^ y * 0xD8163841u;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    return h ^ (h >> 12);
}

// Stage 1, at the low "real detail" size: sensor noise and the phone's ISP (tone, colour, fringes).
void eraSensor(uint8_t *img, int w, int h, uint64_t salt, int lv) {
    // Blacks crushed below ~22, a short range, everything from ~205 up clips to white.
    static const struct Lut {
        uint8_t v[256];
        Lut() { for (int i = 0; i < 256; i++) v[i] = clamp8((i - 22) * 255 / 183); }
    } lut;
    Rng rng(salt);
    thread_local std::vector<uint8_t> lum;
    lum.resize((size_t)w * h);
    for (int i = 0; i < w * h; i++) {
        uint8_t *p = img + (size_t)i * 3;
        int r = p[0], g = p[1], b = p[2];
        const int r0 = r, g0 = g, b0 = b;
        int Y = (77 * r + 150 * g + 29 * b) >> 8;
        // Muted and slightly wrong colour: reds lean orange, blues lean cyan.
        int cr = r - Y, cg = g - Y, cb = b - Y;
        r = Y + ((cr * 170) >> 8);
        g = Y + ((cg * 180 + cr * 40) >> 8);
        b = Y + ((cb * 160 + cg * 30) >> 8);
        // Weak auto white balance: cool and green.
        r -= 8;
        g += 6;
        b += 4;
        // Noise: luma much stronger in the shadows, plus chroma noise (blotchy once upscaled).
        int sigma = 6 + ((255 - Y) * 16 >> 8);
        int n = gauss148(rng) * sigma / 148;
        int n1 = gauss148(rng) * 11 / 148, n2 = gauss148(rng) * 11 / 148;
        // Strength: part way from the clean pixel to the degraded one.
        p[0] = clamp8(r0 + (lut.v[clamp8(r + n + n1)] - r0) * lv / 100);
        p[1] = clamp8(g0 + (lut.v[clamp8(g + n - ((n1 + n2) >> 1))] - g0) * lv / 100);
        p[2] = clamp8(b0 + (lut.v[clamp8(b + n + n2)] - b0) * lv / 100);
        lum[i] = (uint8_t)((77 * p[0] + 150 * p[1] + 29 * p[2]) >> 8);
    }
    // Purple fringing: the dark side of a hard bright edge picks up magenta.
    for (int y = 1; y < h - 1; y++)
        for (int x = 1; x < w - 1; x++) {
            int i = y * w + x, Y = lum[i];
            int m = std::max(std::max(lum[i - 1], lum[i + 1]), std::max(lum[i - w], lum[i + w]));
            if (m < 190 || m - Y < 70) continue;
            int k = std::min((m - Y - 70) >> 1, 50) * lv / 100;
            uint8_t *p = img + (size_t)i * 3;
            p[0] = clamp8(p[0] + (k * 3 >> 2));
            p[1] = clamp8(p[1] - (k >> 2));
            p[2] = clamp8(p[2] + k);
        }
}

// JPEG-like 8x8 quantisation of one plane (quality ~25-30): blocking and ringing.
void jpegPlane(int *plane, int w, int h, bool chroma, int lv) {
    static const uint8_t kLuma[64] = {16, 11, 10, 16, 24, 40, 51, 61, 12, 12, 14, 19, 26, 58, 60, 55,
                                      14, 13, 16, 24, 40, 57, 69, 56, 14, 17, 22, 29, 51, 87, 80, 62,
                                      18, 22, 37, 56, 68, 109, 103, 77, 24, 35, 55, 64, 81, 104, 113, 92,
                                      49, 64, 78, 87, 103, 121, 120, 101, 72, 92, 95, 98, 112, 100, 103, 99};
    static const uint8_t kChroma[64] = {17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99, 99, 99, 99,
                                        24, 26, 56, 99, 99, 99, 99, 99, 47, 66, 99, 99, 99, 99, 99, 99,
                                        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
                                        99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99};
    static const struct Dct {
        int c[8][8];   // orthonormal DCT-II basis, x4096
        Dct() {
            for (int u = 0; u < 8; u++)
                for (int x = 0; x < 8; x++)
                    c[u][x] = (int)std::lround(4096.0 * (u ? 0.5 : std::sqrt(0.125)) *
                                               std::cos((2 * x + 1) * u * 3.14159265358979 / 16));
        }
    } dct;
    const uint8_t *qt = chroma ? kChroma : kLuma;
    // Percent of the standard tables: quality ~20 at full strength, ~70 at the lightest.
    const int full = chroma ? 320 : 260;
    const int scale = 30 + (full - 30) * lv / 100;
    int blk[64], tmp[64];
    for (int by = 0; by < h; by += 8)
        for (int bx = 0; bx < w; bx += 8) {
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 8; x++)
                    blk[y * 8 + x] = plane[std::min(by + y, h - 1) * w + std::min(bx + x, w - 1)] - 128;
            for (int y = 0; y < 8; y++)
                for (int u = 0; u < 8; u++) {
                    int a = 0;
                    for (int x = 0; x < 8; x++) a += dct.c[u][x] * blk[y * 8 + x];
                    tmp[y * 8 + u] = (a + 2048) >> 12;
                }
            for (int v = 0; v < 8; v++)
                for (int u = 0; u < 8; u++) {
                    int a = 0;
                    for (int y = 0; y < 8; y++) a += dct.c[v][y] * tmp[y * 8 + u];
                    int f = (a + 2048) >> 12, q = std::max(1, qt[v * 8 + u] * scale / 100);
                    blk[v * 8 + u] = (f >= 0 ? (f + q / 2) / q : -((q / 2 - f) / q)) * q;
                }
            for (int y = 0; y < 8; y++)
                for (int u = 0; u < 8; u++) {
                    int a = 0;
                    for (int v = 0; v < 8; v++) a += dct.c[v][y] * blk[v * 8 + u];
                    tmp[y * 8 + u] = (a + 2048) >> 12;
                }
            for (int y = 0; y < 8 && by + y < h; y++)
                for (int x = 0; x < 8 && bx + x < w; x++) {
                    int a = 0;
                    for (int u = 0; u < 8; u++) a += dct.c[u][x] * tmp[y * 8 + u];
                    plane[(by + y) * w + bx + x] = std::clamp(((a + 2048) >> 12) + 128, 0, 255);
                }
        }
}

// Stage 2, at the guest's size after a plain bilinear upscale: fixed-pattern noise, heavy
// vignetting with soft corners, then JPEG compression (4:2:0 chroma, 8x8 blocks).
void eraFinish(uint8_t *img, int w, int h, uint64_t salt, int lv) {
    Rng rng(salt ^ 0x5BD1E995u);
    int cx = w / 2, cy = h / 2;
    int64_t inv = ((int64_t)256 << 24) / std::max(1, cx * cx + cy * cy);
    thread_local std::vector<int> Y, Cb, Cr;
    int cw = (w + 1) / 2, ch = (h + 1) / 2;
    Y.resize((size_t)w * h);
    Cb.assign((size_t)cw * ch, 0);
    Cr.assign((size_t)cw * ch, 0);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = img + ((size_t)y * w + x) * 3;
            int dx = x - cx, dy = y - cy, r2 = (int)(((int64_t)(dx * dx + dy * dy) * inv) >> 24);   // 0..256
            int f = 256 - ((r2 * 130 >> 8) + (r2 * r2 * 70 >> 16)) * lv / 100;                 // corners ~22 % at full
            int fpn = (int)(hash2((uint32_t)x, (uint32_t)y) & 15) - 8 + (int)(hash2((uint32_t)x, 0x9E37u) & 7) - 4;
            int n = (gauss148(rng) * 2 / 148 + (fpn >> 1)) * lv / 100;                         // pixels + columns
            int r = (p[0] * f >> 8) + n, g = (p[1] * f >> 8) + n, b = (p[2] * f >> 8) + n;
            Y[(size_t)y * w + x] = std::clamp((77 * r + 150 * g + 29 * b) >> 8, 0, 255);
            size_t ci = (size_t)(y >> 1) * cw + (x >> 1);
            Cb[ci] += (-43 * r - 85 * g + 128 * b) >> 8;
            Cr[ci] += (128 * r - 107 * g - 21 * b) >> 8;
        }
    for (size_t i = 0; i < Cb.size(); i++) {   // 2x2 averages
        Cb[i] = std::clamp(Cb[i] / 4 + 128, 0, 255);
        Cr[i] = std::clamp(Cr[i] / 4 + 128, 0, 255);
    }
    jpegPlane(Y.data(), w, h, false, lv);
    jpegPlane(Cb.data(), cw, ch, true, lv);
    jpegPlane(Cr.data(), cw, ch, true, lv);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = img + ((size_t)y * w + x) * 3;
            size_t ci = (size_t)(y >> 1) * cw + (x >> 1);
            int L = Y[(size_t)y * w + x], u = Cb[ci] - 128, v = Cr[ci] - 128;
            p[0] = clamp8(L + ((359 * v) >> 8));
            p[1] = clamp8(L - ((88 * u + 183 * v) >> 8));
            p[2] = clamp8(L + ((454 * u) >> 8));
        }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Guest side

uint32_t Camera::read(uint32_t pa, int) {
    switch (pa & 0xFFF) {
        case 0x00: return ID;
        case 0x04: return attached ? 1 : 0;
        case 0x08: return buf;
        case 0x0C: return vih;
        case 0x14: return result;
        case 0x18: return panelPortrait ? 1 : 2;
    }
    return 0;
}

void Camera::write(uint32_t pa, uint32_t v, int) {
    switch (pa & 0xFFF) {
        case 0x04: noteOpen(v != 0); break;
        case 0x08: buf = v; break;
        case 0x0C: vih = v; break;
        case 0x10: result = fill(); break;
        case 0x18:
            if (v == 0x200) registryInStep = true;   // the helper: pimg's registry sizes match
            else {                                    // wmdcam.dll, inside device.exe as the camera opens
                driverScanTried = false;
                orientFormats();
            }
            break;
    }
}

// Host address of a guest VA in any process slot: a plain walk of the (single, CE 5) page table
// that ignores domains and access bits. RAM only.
uint8_t *Camera::hostVa(uint32_t va) {
    if (!(cpu.cp15_ctrl & 1)) return nullptr;
    uint32_t l1 = bus.read((cpu.cp15_ttb & ~0x3FFFu) + ((va >> 20) << 2), 4), pa;
    switch (l1 & 3) {
        case 2: pa = (l1 & 0xFFF00000u) | (va & 0xFFFFF); break;
        case 1:
        case 3: {
            uint32_t l2 = (l1 & 3) == 1 ? bus.read((l1 & 0xFFFFFC00u) + (((va >> 12) & 0xFF) << 2), 4)
                                        : bus.read((l1 & 0xFFFFF000u) + (((va >> 10) & 0x3FF) << 2), 4);
            switch (l2 & 3) {
                case 1: pa = (l2 & 0xFFFF0000u) | (va & 0xFFFF); break;
                case 2: pa = (l2 & 0xFFFFF000u) | (va & 0xFFF); break;
                case 3: pa = (l2 & 0xFFFFFC00u) | (va & 0x3FF); break;
                default: return nullptr;
            }
            break;
        }
        default: return nullptr;
    }
    bool wr;
    uint8_t *h = bus.hostPage(pa & ~0xFFFu, wr);
    return h && wr ? h + (pa & 0xFFF) : nullptr;
}

// NULLCAM's CS_DATARANGE_VIDEO (0x538 bytes): FormatSize, Flags, SampleSize, Reserved, then the
// MEDIATYPE_Video / subtype / FORMAT_VideoInfo GUIDs at +0x10/+0x20/+0x30; size pairs of the
// config caps at +0x64..+0xA0; the BITMAPINFOHEADER at +0x100 (biWidth +0x104, biHeight +0x108).
// Runs inside device.exe (CE 5 fills page-table entries lazily, so another process may not reach
// the table), once per camera open; the table keeps its orientation from then on.
void Camera::orientFormats() {
    if (driverScanTried || !registryInStep) return;
    bool portrait = panelPortrait;
    static const uint8_t kVids[4] = {'v', 'i', 'd', 's'};
    static const uint8_t kFormatVideoInfo[16] = {0x80, 0x9f, 0x58, 0x05, 0x56, 0xc3, 0xce, 0x11,
                                                 0xbf, 0x01, 0x00, 0xaa, 0x00, 0x55, 0x59, 0x5a};
    auto isRange = [&](uint32_t va) {
        uint8_t *p = hostVa(va), *bmi = hostVa(va + 0x100);
        if (!p || !bmi || (va & 0xFFF) > 0xFC0) return false;
        if (rd32(p) != 0x538 || memcmp(p + 0x10, kVids, 4) || memcmp(p + 0x30, kFormatVideoInfo, 16)) return false;
        return rd32(bmi) == 40;
    };
    bool fresh = true;
    for (uint32_t va : ranges)
        if (!isRange(va)) fresh = false;
    if (ranges.empty() || !fresh) {
        // device.exe holds the only copy: search the top half of its slot (the current one).
        ranges.clear();
        driverScanTried = true;   // once per camera open
        uint32_t slot = cpu.cp15_pid >> 25;
        if (!slot) return;
        {
            std::vector<uint32_t> found;
            for (uint32_t va = slot << 25 | 0x01000000; va < (slot + 1) << 25; va += 0x1000) {
                uint8_t *pg = hostVa(va);
                if (!pg) continue;
                for (uint32_t o = 0; o <= 0xFC0; o += 4)
                    if (rd32(pg + o) == 0x538 && !memcmp(pg + o + 0x10, kVids, 4) && isRange(va + o)) found.push_back(va + o);
            }
            if (found.size() >= 4) ranges = found;
        }
        if (ranges.empty()) return;
        LOGI("camera: NULLCAM format table: %zu ranges", ranges.size());
    }
    int turned = 0;
    for (uint32_t va : ranges) {
        auto field = [&](uint32_t off) { return (uint32_t *)(void *)hostVa(va + off); };
        uint32_t *w = field(0x104), *h = field(0x108);
        if (!w || !h) continue;
        int32_t hw = (int32_t)*w, hh = (int32_t)*h, ah = hh < 0 ? -hh : hh;
        if ((hw < ah) == portrait || hw == ah) continue;
        *w = (uint32_t)ah;
        *h = (uint32_t)(hh < 0 ? -hw : hw);
        for (uint32_t off = 0x64; off < 0xA4; off += 8) {
            if (off == 0x7C || off == 0x84) continue;   // granularities and alignments, not sizes
            uint32_t *a = field(off), *b = field(off + 4);
            if (a && b) std::swap(*a, *b);
        }
        turned++;
    }
    if (turned) LOGI("camera: sensor formats turned %s (%d)", portrait ? "portrait" : "landscape", turned);
}

void Camera::noteOpen(bool open) {
    if (open) {
        driverScanTried = false;
        orientFormats();
        attachNs = hostNs();
        if (!attached.exchange(true)) LOGI("camera: guest driver opened the camera");
    } else {
        lastFillNs = 0;
        attachNs = 0;
        if (attached.exchange(false)) LOGI("camera: guest driver closed the camera (%llu frames)",
                                           (unsigned long long)framesServed.load());
        loggedFormat = false;
    }
}

bool Camera::wanted() const {
    uint64_t now = hostNs(), lf = lastFillNs, an = attachNs;
    if (lf && now - lf < WANT_AFTER_FILL_NS) return true;
    return attached && an && now - an < WANT_AFTER_OPEN_NS;
}

// Translates a VA in the current process the way the guest's own access would.
bool Camera::translate(uint32_t va, bool write, uint32_t &pa) {
    bool user = (cpu.getCpsr() & 0x1F) == 0x10;
    uint32_t mva = (va < 0x02000000 && (cpu.cp15_ctrl & 1)) ? (va | cpu.cp15_pid) : va;
    uint32_t fsr;
    return cpu.translate(mva, write, user, pa, fsr);
}

bool Camera::readGuest(uint32_t va, void *out, uint32_t n) {
    uint8_t *o = (uint8_t *)out;
    while (n) {
        uint32_t chunk = std::min<uint32_t>(n, 0x400 - (va & 0x3FF));
        uint32_t pa;
        if (!translate(va, false, pa)) return false;
        bool wr;
        if (uint8_t *h = bus.hostPage(pa, wr)) memcpy(o, h, chunk);
        else for (uint32_t i = 0; i < chunk; i++) o[i] = (uint8_t)bus.read(pa + i, 1);
        o += chunk;
        va += chunk;
        n -= chunk;
    }
    return true;
}

// One frame for NULLCAM: called with the driver's buffer and its pin's CS_VIDEOINFOHEADER.
uint32_t Camera::fill() {
    lastFillNs = hostNs();
    orientFormats();   // a DLL that did not say ORIENT (no-op after the first try per open)
    // CS_VIDEOINFOHEADER: rcSource, rcTarget, dwBitRate, dwBitErrorRate, AvgTimePerFrame, then the
    // BITMAPINFOHEADER at +48.
    uint8_t v[72];
    if (!readGuest(vih, v, sizeof v)) return RESULT_UNSUPPORTED;
    int32_t w = (int32_t)rd32(v + 52), h = (int32_t)rd32(v + 56);
    uint16_t bpp = rd16(v + 62);
    uint32_t comp = rd32(v + 64) & ~0x8000u;   // minus BI_SRCPREROTATE
    uint32_t sizeImage = rd32(v + 68);
    Target t;
    t.w = w;
    t.h = h < 0 ? -h : h;
    if ((comp == 3 || comp == 0) && bpp == 16) {
        t.fmt = FMT_RGB565;
        t.bottomUp = h > 0;   // a positive height is a bottom-up DIB
    } else if (comp == FOURCC_YV12 && bpp == 12) {
        t.fmt = FMT_YV12;
    }
    size_t need = t.bytes();
    if (!t.fmt || t.w <= 0 || t.h <= 0 || t.w > 4096 || t.h > 4096 || (t.w & 1) || (t.h & 1) ||
        (sizeImage && sizeImage < need)) {
        if (!loggedFormat) {
            loggedFormat = true;
            LOGW("camera: unsupported format %dx%d bpp %u compression %08x size %u", w, h, bpp, comp, sizeImage);
        }
        return RESULT_UNSUPPORTED;
    }
    // Every page must be committed and writable first: CE commits buffers lazily, so the DLL
    // touches the page we name and asks again.
    uint32_t pa;
    for (uint32_t off = 0; off < need;) {
        if (!translate(buf + off, true, pa)) return RESULT_FAULT | off;
        off += 0x400 - ((buf + off) & 0x3FF);
    }
    auto frame = frameFor(t);
    const uint8_t *src = frame->data();
    uint32_t va = buf;
    for (size_t left = need; left;) {
        uint32_t chunk = (uint32_t)std::min<size_t>(left, 0x400 - (va & 0x3FF));
        if (!translate(va, true, pa)) return RESULT_FAULT | (va - buf);
        if (!cpu.codePage.empty() && cpu.codePage[pa >> 12] && cpu.jit) jitCodeWrite(cpu.jit, pa, chunk);
        bool wr;
        uint8_t *hp = bus.hostPage(pa, wr);
        if (hp && wr) memcpy(hp, src, chunk);
        else for (uint32_t i = 0; i < chunk; i++) bus.write(pa + i, src[i], 1);
        src += chunk;
        va += chunk;
        left -= chunk;
    }
    framesServed++;
    return sizeImage ? sizeImage : (uint32_t)need;
}

// ---------------------------------------------------------------------------------------------
// Frames

std::shared_ptr<const std::vector<uint8_t>> Camera::frameFor(const Target &t) {
    uint64_t now = hostNs();
    std::shared_ptr<const Image> s;
    uint64_t sq;
    {
        std::lock_guard<std::mutex> lk(mu);
        Packed *hit = nullptr;
        for (auto &p : cache)
            if (p.t == t) hit = &p;
        if (!hit) {
            LOGI("camera: guest asks for %dx%d %s%s", t.w, t.h, t.fmt == FMT_RGB565 ? "RGB565" : "YV12",
                 t.bottomUp ? " bottom-up" : "");
            if (cache.size() >= 4) {
                auto old = std::min_element(cache.begin(), cache.end(),
                                            [](const Packed &a, const Packed &b) { return a.lastAsked < b.lastAsked; });
                cache.erase(old);
            }
            cache.push_back(Packed{t, 0, now, nullptr});
            hit = &cache.back();
        }
        hit->lastAsked = now;
        // A frame one camera frame old is fine: the camera thread is already rendering the next.
        if (src && hit->data) return hit->data;
        s = src;
        sq = seq;
    }
    if (!s) {
        auto img = std::make_shared<Image>();
        if (hostCamera) {
            img->w = 4;
            img->h = 3;
            img->rgb.assign(4 * 3 * 3, 0);   // the phone camera is starting: black
        } else {
            makePattern(*img, now);
        }
        return render(*img, t, renderSalt++);
    }
    auto data = render(*s, t, renderSalt++);
    std::lock_guard<std::mutex> lk(mu);
    for (auto &p : cache)
        if (p.t == t && sq >= p.seq) { p.seq = sq; p.data = data; }
    return data;
}

std::shared_ptr<const std::vector<uint8_t>> Camera::render(const Image &s, const Target &t, uint64_t salt) {
    // Centre crop to the guest's aspect ratio, then scale.
    int cw = s.w, ch = s.h;
    if ((int64_t)s.w * t.h > (int64_t)s.h * t.w) cw = std::max(1, (int)((int64_t)s.h * t.w / t.h));
    else ch = std::max(1, (int)((int64_t)s.w * t.h / t.w));
    int cx = (s.w - cw) / 2, cy = (s.h - ch) / 2;
    std::vector<uint8_t> img((size_t)t.w * t.h * 3);
    // Crop rectangle -> w x h (area average when shrinking, bilinear when enlarging).
    auto scale = scaleRgb;
    const uint8_t *crop = &s.rgb[((size_t)cy * s.w + cx) * 3];
    const int lv = era.load();
    if (lv <= 0) {
        scale(crop, s.w * 3, cw, ch, img.data(), t.w, t.h);
    } else {
        // Little real detail: down to half the size asked for at full strength (320x240 of
        // detail in a VGA photo).
        int dw = std::max(16, t.w * (200 - lv) / 200);
        int dh = std::max(12, dw * t.h / t.w);
        thread_local std::vector<uint8_t> small;
        small.resize((size_t)dw * dh * 3);
        scale(crop, s.w * 3, cw, ch, small.data(), dw, dh);
        eraSensor(small.data(), dw, dh, salt, lv);
        scale(small.data(), dw * 3, dw, dh, img.data(), t.w, t.h);
        // A slow sensor: a third of the previous frame stays (motion smear).
        {
            std::lock_guard<std::mutex> lk(smearMu);
            Smear *sm = nullptr;
            for (auto &e : smear)
                if (e.w == t.w && e.h == t.h) sm = &e;
            if (!sm) {
                if (smear.size() >= 4) smear.erase(smear.begin());
                smear.push_back(Smear{t.w, t.h, img});
                sm = &smear.back();
            }
            const int keep = 86 * lv / 100;   // a third of the previous frame at full strength
            for (size_t i = 0; i < img.size(); i++) img[i] = (uint8_t)((img[i] * (256 - keep) + sm->rgb[i] * keep) >> 8);
            sm->rgb = img;
        }
        eraFinish(img.data(), t.w, t.h, salt, lv);
    }

    auto out = std::make_shared<std::vector<uint8_t>>(t.bytes());
    uint8_t *o = out->data();
    if (t.fmt == FMT_RGB565) {
        for (int y = 0; y < t.h; y++) {
            const uint8_t *p = &img[(size_t)y * t.w * 3];
            uint8_t *row = o + (size_t)(t.bottomUp ? t.h - 1 - y : y) * t.w * 2;
            for (int x = 0; x < t.w; x++, p += 3) {
                uint16_t c = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
                row[x * 2] = (uint8_t)c;
                row[x * 2 + 1] = (uint8_t)(c >> 8);
            }
        }
    } else {
        // YV12 (BT.601 studio range): Y plane, then V, then U at half resolution; always top-down.
        uint8_t *Yp = o, *Vp = o + (size_t)t.w * t.h, *Up = Vp + (size_t)(t.w / 2) * (t.h / 2);
        for (int y = 0; y < t.h; y++)
            for (int x = 0; x < t.w; x++) {
                const uint8_t *p = &img[((size_t)y * t.w + x) * 3];
                Yp[(size_t)y * t.w + x] = clamp8(16 + ((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) >> 8));
            }
        for (int y = 0; y < t.h / 2; y++)
            for (int x = 0; x < t.w / 2; x++) {
                int r = 0, g = 0, b = 0;
                for (int k = 0; k < 4; k++) {
                    const uint8_t *p = &img[((size_t)(2 * y + (k >> 1)) * t.w + 2 * x + (k & 1)) * 3];
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
                r >>= 2;
                g >>= 2;
                b >>= 2;
                Up[(size_t)y * (t.w / 2) + x] = clamp8(128 + ((-38 * r - 74 * g + 112 * b + 128) >> 8));
                Vp[(size_t)y * (t.w / 2) + x] = clamp8(128 + ((112 * r - 94 * g - 18 * b + 128) >> 8));
            }
    }
    return out;
}

// SMPTE-like bars over a grey ramp, a white block sweeping across, and a red corner marker at the
// top left (to check orientation): what the guest sees with no phone camera behind it.
void Camera::makePattern(Image &img, uint64_t ns) {
    const int w = 320, h = 240;
    img.w = w;
    img.h = h;
    img.rgb.assign((size_t)w * h * 3, 0);
    static const uint8_t bars[7][3] = {{192, 192, 192}, {192, 192, 0}, {0, 192, 192}, {0, 192, 0},
                                       {192, 0, 192},   {192, 0, 0},   {0, 0, 192}};
    double t = (ns % 4000000000ull) / 4e9;
    int bx = (int)((t < 0.5 ? t * 2 : 2 - t * 2) * (w - 40));
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint8_t *p = &img.rgb[((size_t)y * w + x) * 3];
            if (y < h * 2 / 3) {
                const uint8_t *c = bars[x * 7 / w];
                p[0] = c[0];
                p[1] = c[1];
                p[2] = c[2];
            } else {
                p[0] = p[1] = p[2] = (uint8_t)(x * 255 / (w - 1));
            }
            if (y >= h * 2 / 3 - 30 && y < h * 2 / 3 + 10 && x >= bx && x < bx + 40) p[0] = p[1] = p[2] = 255;
            if (x < 24 && y < 24) { p[0] = 255; p[1] = 0; p[2] = 0; }
        }
}

// ---------------------------------------------------------------------------------------------
// Host side

void Camera::setHostCamera(bool present) { hostCamera = present; }

void Camera::clearFrames() {
    std::lock_guard<std::mutex> lk(mu);
    src.reset();
    for (auto &p : cache) { p.data.reset(); p.seq = 0; }
}

// Stores a new upright frame and pre-renders it for every size the guest asked for recently, so
// the emulation thread only copies.
void Camera::publish(std::shared_ptr<const Image> img) {
    uint64_t now = hostNs(), sq;
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lk(mu);
        src = img;
        sq = ++seq;
        for (auto &p : cache)
            if (now - p.lastAsked < TARGET_ACTIVE_NS) targets.push_back(p.t);
    }
    for (auto &t : targets) {
        auto data = render(*img, t, renderSalt++);
        std::lock_guard<std::mutex> lk(mu);
        for (auto &p : cache)
            if (p.t == t && sq >= p.seq) { p.seq = sq; p.data = data; }
    }
}

namespace {

// Maps output pixel (ox, oy) of the upright frame back to the source: ix = ax*ox + bx*oy + c.
struct Turn {
    int ow, oh;
    int ax, bx, cxx, ay, by, cyy;
};

Turn makeTurn(int w, int h, int rotation, bool mirror, int d) {
    Turn t{};
    rotation = ((rotation % 360) + 360) % 360;
    bool swap = rotation == 90 || rotation == 270;
    int OW = swap ? h : w, OH = swap ? w : h;   // full-resolution upright size
    t.ow = OW / d;
    t.oh = OH / d;
    // Full-resolution inverse maps (clockwise rotation of the source by `rotation`).
    int ax, bx, c0, ay, by, c1;
    switch (rotation) {
        case 90: ax = 0; bx = 1; c0 = 0; ay = -1; by = 0; c1 = h - 1; break;
        case 180: ax = -1; bx = 0; c0 = w - 1; ay = 0; by = -1; c1 = h - 1; break;
        case 270: ax = 0; bx = -1; c0 = w - 1; ay = 1; by = 0; c1 = 0; break;
        default: ax = 1; bx = 0; c0 = 0; ay = 0; by = 1; c1 = 0; break;
    }
    // Mirror: full ox = OW-1 - d*ox'; otherwise d*ox'.
    if (mirror) {
        c0 += ax * (OW - 1);
        c1 += ay * (OW - 1);
        ax = -ax;
        ay = -ay;
    }
    t.ax = ax * d; t.bx = bx * d; t.cxx = c0;
    t.ay = ay * d; t.by = by * d; t.cyy = c1;
    return t;
}

int decimation(int w, int h) {
    int d = 1;
    while (std::max(w, h) / d > 1280) d *= 2;
    return d;
}

}  // namespace

// The 2009 look runs at about 12 frames a second: a 15 fps camera keeps 4 frames in 5. Decided
// before any conversion, so dropped frames cost nothing.
bool Camera::takeFrame() {
    if (!era) return true;
    const uint64_t period = 1000000000ull / 12, now = hostNs();
    if (now < eraNextNs) return false;
    eraNextNs = std::max(eraNextNs + period, now);
    return true;
}

void Camera::pushYuv420(const uint8_t *y, int yRowStride, const uint8_t *u, const uint8_t *v, int uvRowStride,
                        int uvPixelStride, int w, int h, int rotation, bool mirror) {
    if (!y || !u || !v || w <= 0 || h <= 0 || !takeFrame()) return;
    Turn tr = makeTurn(w, h, rotation, mirror, decimation(w, h));
    auto img = std::make_shared<Image>();
    img->w = tr.ow;
    img->h = tr.oh;
    img->rgb.resize((size_t)tr.ow * tr.oh * 3);
    uint8_t *o = img->rgb.data();
    for (int oy = 0; oy < tr.oh; oy++) {
        int ix = tr.bx * oy + tr.cxx, iy = tr.by * oy + tr.cyy;
        for (int ox = 0; ox < tr.ow; ox++, ix += tr.ax, iy += tr.ay, o += 3) {
            int Y = y[(size_t)iy * yRowStride + ix];
            size_t ci = (size_t)(iy >> 1) * uvRowStride + (size_t)(ix >> 1) * uvPixelStride;
            int U = u[ci] - 128, V = v[ci] - 128;
            // BT.601 full range (JFIF), as Android cameras deliver.
            o[0] = clamp8(Y + ((359 * V) >> 8));
            o[1] = clamp8(Y - ((88 * U + 183 * V) >> 8));
            o[2] = clamp8(Y + ((454 * U) >> 8));
        }
    }
    publish(img);
}

void Camera::pushRgb(const uint8_t *rgb, int w, int h, int rotation, bool mirror) {
    if (!rgb || w <= 0 || h <= 0 || !takeFrame()) return;
    Turn tr = makeTurn(w, h, rotation, mirror, decimation(w, h));
    auto img = std::make_shared<Image>();
    img->w = tr.ow;
    img->h = tr.oh;
    img->rgb.resize((size_t)tr.ow * tr.oh * 3);
    uint8_t *o = img->rgb.data();
    for (int oy = 0; oy < tr.oh; oy++) {
        int ix = tr.bx * oy + tr.cxx, iy = tr.by * oy + tr.cyy;
        for (int ox = 0; ox < tr.ow; ox++, ix += tr.ax, iy += tr.ay, o += 3)
            memcpy(o, &rgb[((size_t)iy * w + ix) * 3], 3);
    }
    publish(img);
}

}  // namespace wm
