// Host-side fixes to Microsoft driver code in guest RAM: see codepatch.h.
#include "codepatch.h"
#include "../arm/cpu.h"
#include "../arm/jit.h"
#include "../util/log.h"
#include <cstring>

namespace wm {

namespace {

// A signature word, at a byte offset from the anchor (the first entry, always CTX):
//   CTX    must hold `value` under `mask` (masks drop pc-relative literal offsets and bl targets)
//   PATCH  holds `value` under `mask` (original) or exactly `patched`
//   DATA   a word the patch takes over as data (the gate counter): `value` until patched, then any
enum Kind : uint8_t { CTX, PATCH, DATA };
struct SigWord {
    int16_t off;
    Kind kind;
    uint32_t value, mask, patched;
};

// The tables were taken from RAM dumps of the booted images (CLI --dump-ram), disassembled with
// capstone; offsets in the comments are within the 4 KB page (6.5.3 VAs: battdrvr .text 0x7B63C000,
// nleddrvr 0x7B4A5000, fakeril page 0x7B4DC000).

// battdrvr.dll, WM 6.5.3 (unoptimised DE PDD), BatteryPDDGetStatus.
static const SigWord kBatt653[] = {
    {    0, CTX,  0xE1D330B0, 0xFFFFFFFF, 0x00000000},  // +3B0 ldrh r3, [r3]           (temperature)
    {  -84, CTX,  0xE59F3228, 0xFFFFF000, 0x00000000},  // +35C ldr r3, [pc, #0x228]
    {  -80, CTX,  0xE5D33000, 0xFFFFFFFF, 0x00000000},  // +360 ldrb r3, [r3]           (last level)
    {  -76, CTX,  0xE1520003, 0xFFFFFFFF, 0x00000000},  // +364 cmp r2, r3
    {  -72, PATCH, 0xDA000003, 0xFFFFFFFF, 0xE1A00000}, // +368 ble #0x37c  -> nop      (CHARGING on AC)
    {  -68, CTX,  0xE5DD3001, 0xFFFFFFFF, 0x00000000},  // +36C ldrb r3, [sp, #1]
    {  -64, CTX,  0xE3833008, 0xFFFFFFFF, 0x00000000},  // +370 orr r3, r3, #8          (CHARGING)
    {  -60, CTX,  0xE20330FF, 0xFFFFFFFF, 0x00000000},  // +374 and r3, r3, #0xff
    {  -56, CTX,  0xE5CD3001, 0xFFFFFFFF, 0x00000000},  // +378 strb r3, [sp, #1]
    {  -52, CTX,  0xE5DD2000, 0xFFFFFFFF, 0x00000000},  // +37C ldrb r2, [sp]
    {  -48, CTX,  0xE59F3204, 0xFFFFF000, 0x00000000},  // +380 ldr r3, [pc, #0x204]
    {  -44, CTX,  0xE5C32000, 0xFFFFFFFF, 0x00000000},  // +384 strb r2, [r3]
    {  -40, CTX,  0xEA000005, 0xFFFFFFFF, 0x00000000},  // +388 b #0x3a4
    {  -36, CTX,  0xE59D201C, 0xFFFFFFFF, 0x00000000},  // +38C ldr r2, [sp, #0x1c]
    {  -32, CTX,  0xE3A03000, 0xFFFFFFFF, 0x00000000},  // +390 mov r3, #0
    {  -28, CTX,  0xE5C23000, 0xFFFFFFFF, 0x00000000},  // +394 strb r3, [r2]
    {  -24, CTX,  0xE5DD2000, 0xFFFFFFFF, 0x00000000},  // +398 ldrb r2, [sp]
    {  -20, CTX,  0xE59F31E8, 0xFFFFF000, 0x00000000},  // +39C ldr r3, [pc, #0x1e8]
    {  -16, CTX,  0xE5C32000, 0xFFFFFFFF, 0x00000000},  // +3A0 strb r2, [r3]
    {  -12, CTX,  0xE59F31E4, 0xFFFFF000, 0x00000000},  // +3A4 ldr r3, [pc, #0x1e4]
    {   -8, CTX,  0xE5933000, 0xFFFFFFFF, 0x00000000},  // +3A8 ldr r3, [r3]
    {   -4, CTX,  0xE2833002, 0xFFFFFFFF, 0x00000000},  // +3AC add r3, r3, #2
    {    4, CTX,  0xE58D3004, 0xFFFFFFFF, 0x00000000},  // +3B4 str r3, [sp, #4]
    {    8, CTX,  0xE59F31CC, 0xFFFFF000, 0x00000000},  // +3B8 ldr r3, [pc, #0x1cc]
    {   12, CTX,  0xE5D33000, 0xFFFFFFFF, 0x00000000},  // +3BC ldrb r3, [r3]           (life)
    {   16, PATCH, 0xE3530041, 0xFFFFFFFF, 0xE3530014}, // +3C0 cmp r3, #65 -> #20      (HIGH from)
    {   20, CTX,  0xBA000004, 0xFFFFFFFF, 0x00000000},  // +3C4 blt #0x3dc
    {   24, CTX,  0xE5DD3001, 0xFFFFFFFF, 0x00000000},  // +3C8 ldrb r3, [sp, #1]
    {   28, CTX,  0xE3833001, 0xFFFFFFFF, 0x00000000},  // +3CC orr r3, r3, #1          (HIGH)
    {   32, CTX,  0xE20330FF, 0xFFFFFFFF, 0x00000000},  // +3D0 and r3, r3, #0xff
    {   36, CTX,  0xE5CD3001, 0xFFFFFFFF, 0x00000000},  // +3D4 strb r3, [sp, #1]
    {   40, CTX,  0xEA000010, 0xFFFFFFFF, 0x00000000},  // +3D8 b #0x420
    {   44, CTX,  0xE59F31A8, 0xFFFFF000, 0x00000000},  // +3DC ldr r3, [pc, #0x1a8]
    {   48, CTX,  0xE5D33000, 0xFFFFFFFF, 0x00000000},  // +3E0 ldrb r3, [r3]
    {   52, PATCH, 0xE3530041, 0xFFFFFFFF, 0xE3530014}, // +3E4 cmp r3, #65 -> #20
    {   56, CTX,  0xAA000008, 0xFFFFFFFF, 0x00000000},  // +3E8 bge #0x410
    {   60, CTX,  0xE59F3198, 0xFFFFF000, 0x00000000},  // +3EC ldr r3, [pc, #0x198]
    {   64, CTX,  0xE5D33000, 0xFFFFFFFF, 0x00000000},  // +3F0 ldrb r3, [r3]
    {   68, PATCH, 0xE3530014, 0xFFFFFFFF, 0xE353000A}, // +3F4 cmp r3, #20 -> #10      (CRITICAL below)
    {   72, CTX,  0xBA000004, 0xFFFFFFFF, 0x00000000},  // +3F8 blt #0x410
    {   76, CTX,  0xE5DD3001, 0xFFFFFFFF, 0x00000000},  // +3FC ldrb r3, [sp, #1]
};

// battdrvr.dll, WM 6.1.4 (optimised build of the same PDD): same thresholds, same CHARGING rule.
static const SigWord kBatt614[] = {
    {    0, CTX,  0x23843001, 0xFFFFFFFF, 0x00000000},  // +2B4 orrhs r3, r4, #1        (HIGH)
    {  -56, CTX,  0xE3510000, 0xFFFFFFFF, 0x00000000},  // +27C cmp r1, #0              (AC online?)
    {  -52, CTX,  0x0A000006, 0xFFFFFFFF, 0x00000000},  // +280 beq #0x2a0
    {  -48, CTX,  0xE3A03001, 0xFFFFFFFF, 0x00000000},  // +284 mov r3, #1
    {  -44, CTX,  0xE5C63000, 0xFFFFFFFF, 0x00000000},  // +288 strb r3, [r6]
    {  -40, CTX,  0xE5D53000, 0xFFFFFFFF, 0x00000000},  // +28C ldrb r3, [r5]           (last level)
    {  -36, CTX,  0xE1520003, 0xFFFFFFFF, 0x00000000},  // +290 cmp r2, r3
    {  -32, PATCH, 0x9A000002, 0xFFFFFFFF, 0xE1A00000}, // +294 bls #0x2a4  -> nop      (CHARGING on AC)
    {  -28, CTX,  0xE3A04008, 0xFFFFFFFF, 0x00000000},  // +298 mov r4, #8              (CHARGING)
    {  -24, CTX,  0xEA000000, 0xFFFFFFFF, 0x00000000},  // +29C b #0x2a4
    {  -20, CTX,  0xE5C6A000, 0xFFFFFFFF, 0x00000000},  // +2A0 strb sl, [r6]
    {  -16, CTX,  0xE5903000, 0xFFFFFFFF, 0x00000000},  // +2A4 ldr r3, [r0]
    {  -12, CTX,  0xE5C52000, 0xFFFFFFFF, 0x00000000},  // +2A8 strb r2, [r5]
    {   -8, PATCH, 0xE3520041, 0xFFFFFFFF, 0xE3520014}, // +2AC cmp r2, #65 -> #20      (HIGH from)
    {   -4, CTX,  0xE1D300B2, 0xFFFFFFFF, 0x00000000},  // +2B0 ldrh r0, [r3, #2]
    {    4, CTX,  0x2A000002, 0xFFFFFFFF, 0x00000000},  // +2B8 bhs #0x2c8
    {    8, PATCH, 0xE3520014, 0xFFFFFFFF, 0xE352000A}, // +2BC cmp r2, #20 -> #10      (CRITICAL below)
    {   12, CTX,  0x23843002, 0xFFFFFFFF, 0x00000000},  // +2C0 orrhs r3, r4, #2        (LOW)
    {   16, CTX,  0x33843004, 0xFFFFFFFF, 0x00000000},  // +2C4 orrlo r3, r4, #4        (CRITICAL)
};

// nleddrvr.dll, WM 6.5.3 (DE PDD), NLedSetDevice: `beq ok` taken only when MetaCycleOff == 0.
static const SigWord kNled653[] = {
    {    0, CTX,  0x500FFFD0, 0xFFFFFFFF, 0x00000000},  // +174 .word 0x500FFFD0     (the device's PA)
    {  308, CTX,  0xE3530000, 0xFFFFFFFF, 0x00000000},  // +2A8 cmp r3, #0
    {  312, CTX,  0x0A000002, 0xFFFFFFFF, 0x00000000},  // +2AC beq #0x2bc
    {  316, CTX,  0xE59D300C, 0xFFFFFFFF, 0x00000000},  // +2B0 ldr r3, [sp, #0xc]
    {  320, CTX,  0xE3530000, 0xFFFFFFFF, 0x00000000},  // +2B4 cmp r3, #0
    {  324, CTX,  0x0A000004, 0xFFFFFFFF, 0x00000000},  // +2B8 beq #0x2d0
    {  328, CTX,  0xE59D301C, 0xFFFFFFFF, 0x00000000},  // +2BC ldr r3, [sp, #0x1c]
    {  332, CTX,  0xE2833018, 0xFFFFFFFF, 0x00000000},  // +2C0 add r3, r3, #0x18       (MetaCycleOff)
    {  336, CTX,  0xE5933000, 0xFFFFFFFF, 0x00000000},  // +2C4 ldr r3, [r3]
    {  340, CTX,  0xE3530000, 0xFFFFFFFF, 0x00000000},  // +2C8 cmp r3, #0
    {  344, PATCH, 0x0A000004, 0xFFFFFFFF, 0xEA000004}, // +2CC beq #0x2e4  -> b #0x2e4
    {  348, CTX,  0xE3A00057, 0xFFFFFFFF, 0x00000000},  // +2D0 mov r0, #0x57           (ERROR_INVALID_PARAMETER)
    {  352, CTX,  0xEB0001D5, 0xFF000000, 0x00000000},  // +2D4 bl SetLastError
};

// nleddrvr.dll, WM 6.1.4 (optimised), NLedSetDevice: `bne fail` when MetaCycleOff != 0.
static const SigWord kNled614[] = {
    {    0, CTX,  0x500FFFD0, 0xFFFFFFFF, 0x00000000},  // +134 .word 0x500FFFD0     (the device's PA)
    {  200, CTX,  0xE5973014, 0xFFFFFFFF, 0x00000000},  // +1FC ldr r3, [r7, #0x14]
    {  204, CTX,  0xE3530000, 0xFFFFFFFF, 0x00000000},  // +200 cmp r3, #0
    {  208, CTX,  0x0A000001, 0xFFFFFFFF, 0x00000000},  // +204 beq #0x210
    {  212, CTX,  0xE3580000, 0xFFFFFFFF, 0x00000000},  // +208 cmp r8, #0
    {  216, CTX,  0x0A000026, 0xFFFFFFFF, 0x00000000},  // +20C beq #0x2ac
    {  220, CTX,  0xE5973018, 0xFFFFFFFF, 0x00000000},  // +210 ldr r3, [r7, #0x18]     (MetaCycleOff)
    {  224, CTX,  0xE3530000, 0xFFFFFFFF, 0x00000000},  // +214 cmp r3, #0
    {  228, PATCH, 0x1A000023, 0xFFFFFFFF, 0xE1A00000}, // +218 bne #0x2ac  -> nop
    {  232, CTX,  0xE35800FF, 0xFFFFFFFF, 0x00000000},  // +21C cmp r8, #0xff
    {  376, CTX,  0xE3A00057, 0xFFFFFFFF, 0x00000000},  // +2AC mov r0, #0x57           (ERROR_INVALID_PARAMETER)
};

// fakeril.dll, WM 6.5.3, voice-call progress on the dial thread: after "alerting" it polls the
// hang-up event every 50 ms for [obj+0x1ED0] (2) s, then sets the call active. Patched, the
// elapsed-time test becomes "has the gate counter changed since this call started?". The counter
// is the word at +838 (the original pre-loop test, jumped over once patched): r8 = counter at the
// start, loop while the counter still equals r8.
static const SigWord kGate653[] = {
    {    0, CTX,  0x03A07FFA, 0xFFFFFFFF, 0x00000000},  // +828 moveq r7, #1000
    {   -4, CTX,  0xE3560001, 0xFFFFFFFF, 0x00000000},  // +824 cmp r6, #1
    {    4, PATCH, 0xEB00D6BA, 0xFF000000, 0xE59F8004}, // +82C bl GetTickCount -> ldr r8, [pc, #4]
    {    8, PATCH, 0xE1A08000, 0xFFFFFFFF, 0xE1A00000}, // +830 mov r8, r0  -> nop
    {   12, PATCH, 0xEB00D6B8, 0xFF000000, 0xEA000002}, // +834 bl GetTickCount -> b #0x844
    {   16, DATA, 0xE0403008, 0xFFFFFFFF, 0x00000000},  // +838 sub r3, r0, r8 -> the gate counter
    {   20, CTX,  0xE1530007, 0xFFFFFFFF, 0x00000000},  // +83C cmp r3, r7
    {   24, CTX,  0x8A000009, 0xFFFFFFFF, 0x00000000},  // +840 bhi #0x86c
    {   28, CTX,  0xE2855A01, 0xFFFFFFFF, 0x00000000},  // +844 add r5, r5, #0x1000
    {   32, CTX,  0xE5950EB4, 0xFFFFFFFF, 0x00000000},  // +848 ldr r0, [r5, #0xeb4]     (hang-up event)
    {   36, CTX,  0xE3A01032, 0xFFFFFFFF, 0x00000000},  // +84C mov r1, #50
    {   40, CTX,  0xEB00D66D, 0xFF000000, 0x00000000},  // +850 bl WaitForSingleObject
    {   44, CTX,  0xE3500000, 0xFFFFFFFF, 0x00000000},  // +854 cmp r0, #0
    {   48, CTX,  0x0AFFFF42, 0xFFFFFFFF, 0x00000000},  // +858 beq #0x568
    {   52, PATCH, 0xEB00D6AE, 0xFF000000, 0xE51F302C}, // +85C bl GetTickCount -> ldr r3, [pc, #-0x2c]
    {   56, PATCH, 0xE0403008, 0xFFFFFFFF, 0xE1530008}, // +860 sub r3, r0, r8 -> cmp r3, r8
    {   60, PATCH, 0xE1530007, 0xFFFFFFFF, 0xE1A00000}, // +864 cmp r3, r7  -> nop
    {   64, PATCH, 0x9AFFFFF6, 0xFFFFFFFF, 0x0AFFFFF6}, // +868 bls #0x848 -> beq #0x848
    {   68, CTX,  0xE2843C1E, 0xFFFFFFFF, 0x00000000},  // +86C add r3, r4, #0x1e00
};

struct PatchDef {
    const char *name;
    uint32_t bit;
    const SigWord *w;
    int n;
};
#define PATCH_DEF(name, bit, t) {name, bit, t, (int)(sizeof t / sizeof t[0])}
const PatchDef kPatches[] = {
    PATCH_DEF("battdrvr 6.5.3 (low <20%, critical <10%, charging on AC)", CodePatcher::ST_BATTERY, kBatt653),
    PATCH_DEF("battdrvr 6.1.4 (low <20%, critical <10%, charging on AC)", CodePatcher::ST_BATTERY, kBatt614),
    PATCH_DEF("nleddrvr 6.5.3 (accept MetaCycleOff blinks)", CodePatcher::ST_NLED, kNled653),
    PATCH_DEF("nleddrvr 6.1.4 (accept MetaCycleOff blinks)", CodePatcher::ST_NLED, kNled614),
    PATCH_DEF("fakeril 6.5.3 (connect gate)", CodePatcher::ST_GATE, kGate653),
};
constexpr int kNumPatches = (int)(sizeof kPatches / sizeof kPatches[0]);
constexpr size_t kMaxSites = 32;

uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

}  // namespace

uint8_t *CodePatcher::pageOf(uint32_t pa) const {
    bool writable;
    uint8_t *h = bus.hostPage(pa & ~0xFFFu, writable);
    return h && writable ? h : nullptr;   // RAM only: never the flash image
}

CodePatcher::Match CodePatcher::match(int k, const uint8_t *page, int off) const {
    const PatchDef &d = kPatches[k];
    int orig = 0, done = 0;
    for (int i = 0; i < d.n; i++) {
        const SigWord &s = d.w[i];
        int p = off + s.off;
        if (p < 0 || p > 0xFFC) return NONE;   // signatures never span pages
        if (s.kind == DATA) continue;
        uint32_t v = rd32(page + p);
        bool original = (v & s.mask) == (s.value & s.mask);
        if (s.kind == CTX) {
            if (!original) return NONE;
        } else if (v == s.patched) {
            done++;
        } else if (original) {
            orig++;
        } else {
            return NONE;
        }
    }
    return orig == 0 ? PATCHED : done == 0 ? ORIGINAL : MIXED;
}

bool CodePatcher::verified(const Site &s, Match want) const {
    const uint8_t *page = pageOf(s.pa);
    return page && match(s.patch, page, (int)(s.pa & 0xFFF)) == want;
}

CodePatcher::Site *CodePatcher::site(int patch, uint32_t pa, bool &seen) {
    seen = true;
    for (Site &s : sites)
        if (s.patch == patch && s.pa == pa) return &s;
    seen = false;
    if (sites.size() >= kMaxSites) {   // forget copies that have been overwritten since
        for (size_t i = sites.size(); i-- > 0;)
            if (!verified(sites[i], PATCHED) && !verified(sites[i], ORIGINAL) && !verified(sites[i], MIXED))
                sites.erase(sites.begin() + (long)i);
        if (sites.size() >= kMaxSites) sites.erase(sites.begin());
    }
    sites.push_back(Site{patch, pa, {}});
    return &sites.back();
}

void CodePatcher::invalidate(uint32_t pa) {
    uint32_t ppn = pa >> 12;
    if (cpu.jit && !cpu.codePage.empty() && cpu.codePage[ppn]) jitInvalidatePage(cpu.jit, ppn);
}

bool CodePatcher::apply(Site &s, uint8_t *page, int off, bool log) {
    const PatchDef &d = kPatches[s.patch];
    s.original.assign((size_t)d.n, 0);
    for (int i = 0; i < d.n; i++) {
        const SigWord &w = d.w[i];
        uint32_t v = rd32(page + off + w.off);
        s.original[(size_t)i] = (w.kind == PATCH && v == w.patched) ? w.value : v;
    }
    // The counter first, then the code that reads it.
    for (int i = 0; i < d.n; i++)
        if (d.w[i].kind == DATA) wr32(page + off + d.w[i].off, gateCounter);
    for (int i = 0; i < d.n; i++)
        if (d.w[i].kind == PATCH) wr32(page + off + d.w[i].off, d.w[i].patched);
    invalidate(s.pa);
    if (log) LOGI("codepatch: %s applied at pa %08x", d.name, s.pa);
    return true;
}

void CodePatcher::unapply(Site &s) {
    uint8_t *page = pageOf(s.pa);
    if (!page || match(s.patch, page, (int)(s.pa & 0xFFF)) != PATCHED) return;
    const PatchDef &d = kPatches[s.patch];
    int off = (int)(s.pa & 0xFFF);
    bool saved = s.original.size() == (size_t)d.n;   // empty when it was found patched
    for (int i = 0; i < d.n; i++)
        if (d.w[i].kind != CTX) wr32(page + off + d.w[i].off, saved ? s.original[(size_t)i] : d.w[i].value);
    invalidate(s.pa);
    LOGI("codepatch: %s removed at pa %08x", d.name, s.pa);
}

void CodePatcher::scanPage(uint32_t pa) {
    pa &= ~0xFFFu;
    uint8_t *page = pageOf(pa);
    if (!page) return;
    pagesScanned++;
    for (int off = 0; off < 0x1000; off += 4) {
        uint32_t v = rd32(page + off);
        for (int k = 0; k < kNumPatches; k++) {
            const SigWord &a = kPatches[k].w[0];
            if ((v & a.mask) != a.value) continue;
            Match m = match(k, page, off);
            if (m == NONE) continue;
            uint32_t at = pa + (uint32_t)off;
            bool seen;
            Site &s = *site(k, at, seen);
            if (kPatches[k].bit == ST_GATE && !gateArmed) {
                if (m == ORIGINAL) continue;   // applied once the helper arms the gate
                gateArmed = true;              // patched already: a snapshot, or an older helper
                LOGI("codepatch: FakeRIL gate found in place, armed");
            }
            if (m != PATCHED) apply(s, page, off, true);
            else if (!seen) LOGI("codepatch: %s in place at pa %08x", kPatches[k].name, at);
        }
    }
}

void CodePatcher::onGuestReset() {
    gateArmed = false;
    sites.clear();
}

void CodePatcher::setGateArmed(bool on) {
    if (on == gateArmed) return;
    gateArmed = on;
    LOGI("codepatch: FakeRIL gate %s", on ? "armed" : "disarmed");
    for (Site &s : sites) {
        if (kPatches[s.patch].bit != ST_GATE) continue;
        uint8_t *page = pageOf(s.pa);
        if (!page) continue;
        Match m = match(s.patch, page, (int)(s.pa & 0xFFF));
        if (on && (m == ORIGINAL || m == MIXED)) apply(s, page, (int)(s.pa & 0xFFF), true);
        else if (!on && m == PATCHED) unapply(s);
    }
}

bool CodePatcher::openGate() {
    bool ok = false;
    uint32_t next = gateCounter + 1;
    for (int pass = 0; pass < 2; pass++) {
        for (Site &s : sites) {
            const PatchDef &d = kPatches[s.patch];
            if (d.bit != ST_GATE || !verified(s, PATCHED)) continue;
            uint8_t *page = pageOf(s.pa);
            for (int i = 0; i < d.n; i++) {
                if (d.w[i].kind != DATA) continue;
                uint8_t *p = page + (s.pa & 0xFFF) + d.w[i].off;
                if (pass == 0) {   // never step back to a value a waiting call may hold
                    uint32_t v = rd32(p);
                    if (v != d.w[i].value && v >= next) next = v + 1;
                } else {
                    wr32(p, next);
                    invalidate(s.pa);
                    ok = true;
                }
            }
        }
    }
    if (ok) gateCounter = next;
    LOGI("codepatch: FakeRIL gate %s (counter %u)", ok ? "opened" : "not in place", gateCounter);
    return ok;
}

uint32_t CodePatcher::status() {
    uint32_t st = gateArmed ? ST_GATE_ARMED : 0;
    for (const Site &s : sites)
        if (verified(s, PATCHED)) st |= kPatches[s.patch].bit;
    return st;
}

uint32_t CodePatcher::read(uint32_t pa, int size) {
    switch (pa & 0xFFF) {
    case 0x00: return ID;
    case 0x04: return status();
    case 0x08: return gateCounter;
    case 0x0C: return gateArmed ? 1 : 0;
    default: return 0;
    }
}

void CodePatcher::write(uint32_t pa, uint32_t v, int size) {
    switch (pa & 0xFFF) {
    case 0x08: openGate(); break;
    case 0x0C: setGateArmed(v != 0); break;
    default: break;
    }
}

}  // namespace wm
