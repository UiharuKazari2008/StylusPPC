// Minimal AArch64 machine-code emitter for the JIT.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace wm {
namespace a64 {

enum Reg : uint32_t {
    X0 = 0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15, X16, X17, X18,
    X19, X20, X21, X22, X23, X24, X25, X26, X27, X28, FP = 29, LR = 30, SP = 31, ZR = 31,
};
enum Shift : uint32_t { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
enum Cond : uint32_t { EQ, NE, CS, CC, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL, NV };

class Emitter {
public:
    explicit Emitter(uint32_t *buf, size_t cap) : base(buf), cur(buf), end(buf + cap) {}
    uint32_t *pos() const { return cur; }
    size_t size() const { return (size_t)(cur - base); }
    bool overflow() const { return cur >= end; }
    void put(uint32_t w) { if (cur < end) *cur++ = w; else ovf = true; }
    bool ovf = false;

    // ---- data processing (32-bit unless noted) ----
    void movz(Reg d, uint32_t imm16, int hw = 0, bool x = false) { put((x ? 0xD2800000u : 0x52800000u) | (hw << 21) | (imm16 << 5) | d); }
    void movk(Reg d, uint32_t imm16, int hw = 0, bool x = false) { put((x ? 0xF2800000u : 0x72800000u) | (hw << 21) | (imm16 << 5) | d); }
    void movn(Reg d, uint32_t imm16, int hw = 0) { put(0x12800000u | (hw << 21) | (imm16 << 5) | d); }
    void mov32(Reg d, uint32_t v) {
        if ((v & 0xFFFF0000u) == 0) movz(d, v);
        else if ((v & 0xFFFF) == 0) movz(d, v >> 16, 1);
        else if ((v | 0xFFFF) == 0xFFFFFFFFu) movn(d, (~v) & 0xFFFF);
        else if ((v | 0xFFFF0000u) == 0xFFFFFFFFu) movn(d, (~v) >> 16, 1);
        else { movz(d, v & 0xFFFF); movk(d, v >> 16, 1); }
    }
    void mov64(Reg d, uint64_t v) {
        movz(d, v & 0xFFFF, 0, true);
        for (int i = 1; i < 4; i++) if ((v >> (16 * i)) & 0xFFFF) movk(d, (v >> (16 * i)) & 0xFFFF, i, true);
    }
    void movr(Reg d, Reg m) { put(0x2A0003E0u | (m << 16) | d); }          // mov wd, wm (orr wd, wzr, wm)
    void movx(Reg d, Reg m) { put(0xAA0003E0u | (m << 16) | d); }          // mov xd, xm
    // add/sub shifted register: op 0 add,1 adds,2 sub,3 subs
    void addsub(int op, Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t amt = 0) {
        static const uint32_t base[4] = {0x0B000000u, 0x2B000000u, 0x4B000000u, 0x6B000000u};
        put(base[op] | (sh << 22) | (m << 16) | (amt << 10) | (n << 5) | d);
    }
    void add(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { addsub(0, d, n, m, sh, a); }
    void adds(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { addsub(1, d, n, m, sh, a); }
    void sub(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { addsub(2, d, n, m, sh, a); }
    void subs(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { addsub(3, d, n, m, sh, a); }
    void addi(Reg d, Reg n, uint32_t imm12, bool x = false) { put((x ? 0x91000000u : 0x11000000u) | (imm12 << 10) | (n << 5) | d); }
    void subi(Reg d, Reg n, uint32_t imm12, bool x = false) { put((x ? 0xD1000000u : 0x51000000u) | (imm12 << 10) | (n << 5) | d); }
    void addx(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(0x8B000000u | (sh << 22) | (m << 16) | (a << 10) | (n << 5) | d); }
    void adc(Reg d, Reg n, Reg m) { put(0x1A000000u | (m << 16) | (n << 5) | d); }
    void adcs(Reg d, Reg n, Reg m) { put(0x3A000000u | (m << 16) | (n << 5) | d); }
    void sbc(Reg d, Reg n, Reg m) { put(0x5A000000u | (m << 16) | (n << 5) | d); }
    void sbcs(Reg d, Reg n, Reg m) { put(0x7A000000u | (m << 16) | (n << 5) | d); }
    // logical shifted register: opc 0 and,1 orr,2 eor,3 ands; n=1 inverts m (bic/orn/eon/bics)
    void logic(int opc, bool inv, Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t amt = 0) {
        static const uint32_t base[4] = {0x0A000000u, 0x2A000000u, 0x4A000000u, 0x6A000000u};
        put(base[opc] | (sh << 22) | (inv ? (1u << 21) : 0) | (m << 16) | (amt << 10) | (n << 5) | d);
    }
    void andr(Reg d, Reg n, Reg m) { logic(0, false, d, n, m); }
    void orr(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { logic(1, false, d, n, m, sh, a); }
    void eor(Reg d, Reg n, Reg m) { logic(2, false, d, n, m); }
    void eorx(Reg d, Reg n, Reg m) { put(0xCA000000u | (m << 16) | (n << 5) | d); }
    void mvn(Reg d, Reg m) { logic(1, true, d, ZR, m); }
    // shifts by immediate (UBFM/SBFM/EXTR)
    void lsli(Reg d, Reg n, uint32_t s) { put(0x53000000u | (((32 - s) & 31) << 16) | ((31 - s) << 10) | (n << 5) | d); }
    void lsri(Reg d, Reg n, uint32_t s) { put(0x53000000u | (s << 16) | (31 << 10) | (n << 5) | d); }
    void asri(Reg d, Reg n, uint32_t s) { put(0x13000000u | (s << 16) | (31 << 10) | (n << 5) | d); }
    void rori(Reg d, Reg n, uint32_t s) { put(0x13800000u | (n << 16) | (s << 10) | (n << 5) | d); }
    void lsrix(Reg d, Reg n, uint32_t s) { put(0xD340FC00u | (s << 16) | (n << 5) | d); }
    void ubfx(Reg d, Reg n, uint32_t lsb, uint32_t w) { put(0x53000000u | (lsb << 16) | ((lsb + w - 1) << 10) | (n << 5) | d); }
    void bfi(Reg d, Reg n, uint32_t lsb, uint32_t w) { put(0x33000000u | (((32 - lsb) & 31) << 16) | ((w - 1) << 10) | (n << 5) | d); }
    void sxtb(Reg d, Reg n) { put(0x13001C00u | (n << 5) | d); }
    void sxth(Reg d, Reg n) { put(0x13003C00u | (n << 5) | d); }
    void sxtw(Reg d, Reg n) { put(0x93407C00u | (n << 5) | d); }           // xd = sign-extended wn
    // shifts by register
    void lslv(Reg d, Reg n, Reg m, bool x = false) { put((x ? 0x9AC02000u : 0x1AC02000u) | (m << 16) | (n << 5) | d); }
    void lsrv(Reg d, Reg n, Reg m, bool x = false) { put((x ? 0x9AC02400u : 0x1AC02400u) | (m << 16) | (n << 5) | d); }
    void asrv(Reg d, Reg n, Reg m, bool x = false) { put((x ? 0x9AC02800u : 0x1AC02800u) | (m << 16) | (n << 5) | d); }
    void rorv(Reg d, Reg n, Reg m) { put(0x1AC02C00u | (m << 16) | (n << 5) | d); }
    void madd(Reg d, Reg n, Reg m, Reg a) { put(0x1B000000u | (m << 16) | (a << 10) | (n << 5) | d); }
    void mul(Reg d, Reg n, Reg m) { madd(d, n, m, ZR); }
    void umaddl(Reg d, Reg n, Reg m, Reg a) { put(0x9BA00000u | (m << 16) | (a << 10) | (n << 5) | d); }
    void smaddl(Reg d, Reg n, Reg m, Reg a) { put(0x9B200000u | (m << 16) | (a << 10) | (n << 5) | d); }
    void clz(Reg d, Reg n) { put(0x5AC01000u | (n << 5) | d); }
    void csel(Reg d, Reg n, Reg m, Cond c) { put(0x1A800000u | (m << 16) | (c << 12) | (n << 5) | d); }
    void cmpi(Reg n, uint32_t imm12) { put(0x7100001Fu | (imm12 << 10) | (n << 5)); }
    void addsi(Reg d, Reg n, uint32_t imm12) { put(0x31000000u | (imm12 << 10) | (n << 5) | d); }
    void subsi(Reg d, Reg n, uint32_t imm12) { put(0x71000000u | (imm12 << 10) | (n << 5) | d); }
    void mrsNzcv(Reg t) { put(0xD53B4200u | t); }
    void msrNzcv(Reg t) { put(0xD51B4200u | t); }
    // orr with x-register to build 64-bit values
    void orrx(Reg d, Reg n, Reg m, Shift sh = LSL, uint32_t a = 0) { put(0xAA000000u | (sh << 22) | (m << 16) | (a << 10) | (n << 5) | d); }

    // ---- loads / stores ----
    // unsigned scaled immediate forms; size 0 byte,1 half,2 word,3 dword
    void ldrImm(int size, Reg t, Reg n, uint32_t off) { put(((uint32_t)size << 30) | 0x39400000u | ((off >> size) << 10) | (n << 5) | t); }
    void strImm(int size, Reg t, Reg n, uint32_t off) { put(((uint32_t)size << 30) | 0x39000000u | ((off >> size) << 10) | (n << 5) | t); }
    void ldrw(Reg t, Reg n, uint32_t off) { ldrImm(2, t, n, off); }
    void strw(Reg t, Reg n, uint32_t off) { strImm(2, t, n, off); }
    void ldrx(Reg t, Reg n, uint32_t off) { ldrImm(3, t, n, off); }
    void strx(Reg t, Reg n, uint32_t off) { strImm(3, t, n, off); }
    // register offset, Xn + Wm (uxtw); signedLoad: ldrsb/ldrsh into w
    void ldrReg(int size, Reg t, Reg n, Reg m, bool signedLoad = false) {
        uint32_t opc = signedLoad ? 0x00C00000u : 0x00400000u;
        put(((uint32_t)size << 30) | 0x38200000u | opc | (m << 16) | (2u << 13) | (2u << 10) | (n << 5) | t);
    }
    void strReg(int size, Reg t, Reg n, Reg m) { put(((uint32_t)size << 30) | 0x38200000u | (m << 16) | (2u << 13) | (2u << 10) | (n << 5) | t); }
    // ldr x, [xn, xm, lsl #4] style index for TLB: use add then ldp
    void ldp(Reg t1, Reg t2, Reg n, int32_t off, bool x = true) { put((x ? 0xA9400000u : 0x29400000u) | ((((uint32_t)(off >> (x ? 3 : 2))) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }
    void stpPre(Reg t1, Reg t2, Reg n, int32_t off) { put(0xA9800000u | ((((uint32_t)(off >> 3)) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }
    void ldpPost(Reg t1, Reg t2, Reg n, int32_t off) { put(0xA8C00000u | ((((uint32_t)(off >> 3)) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }
    void stp(Reg t1, Reg t2, Reg n, int32_t off) { put(0xA9000000u | ((((uint32_t)(off >> 3)) & 0x7F) << 15) | (t2 << 10) | (n << 5) | t1); }

    // ---- branches ----
    uint32_t *b() { uint32_t *p = cur; put(0x14000000u); return p; }
    uint32_t *bcond(Cond c) { uint32_t *p = cur; put(0x54000000u | c); return p; }
    uint32_t *cbz(Reg t, bool x = false) { uint32_t *p = cur; put((x ? 0xB4000000u : 0x34000000u) | t); return p; }
    uint32_t *cbnz(Reg t, bool x = false) { uint32_t *p = cur; put((x ? 0xB5000000u : 0x35000000u) | t); return p; }
    uint32_t *tbnz(Reg t, uint32_t bit) { uint32_t *p = cur; put(0x37000000u | ((bit >> 5) << 31) | ((bit & 31) << 19) | t); return p; }
    uint32_t *tbz(Reg t, uint32_t bit) { uint32_t *p = cur; put(0x36000000u | ((bit >> 5) << 31) | ((bit & 31) << 19) | t); return p; }
    void blr(Reg n) { put(0xD63F0000u | (n << 5)); }
    void br(Reg n) { put(0xD61F0000u | (n << 5)); }
    void ret() { put(0xD65F03C0u); }
    void adr(Reg d, const uint32_t *target) {
        int64_t off = (int64_t)((const char *)target - (const char *)cur);
        put(0x10000000u | ((uint32_t)(off & 3) << 29) | ((uint32_t)((off >> 2) & 0x7FFFF) << 5) | d);
    }
    void blTo(const uint32_t *target) { int64_t d = target - cur; put(0x94000000u | ((uint32_t)d & 0x03FFFFFF)); }
    void bTo(const uint32_t *target) { int64_t d = target - cur; put(0x14000000u | ((uint32_t)d & 0x03FFFFFF)); }

    // Patch a branch emitted earlier to point at `target` (default: here).
    static void patch(uint32_t *at, const uint32_t *target) {
        int64_t d = target - at;
        uint32_t w = *at;
        if ((w & 0x7C000000u) == 0x14000000u) *at = (w & 0xFC000000u) | ((uint32_t)d & 0x03FFFFFF);       // b
        else if ((w & 0xFF000010u) == 0x54000000u) *at = (w & 0xFF00001Fu) | (((uint32_t)d & 0x7FFFF) << 5);  // b.cond
        else if ((w & 0x7E000000u) == 0x34000000u) *at = (w & 0xFF00001Fu) | (((uint32_t)d & 0x7FFFF) << 5);  // cbz/cbnz
        else if ((w & 0x7E000000u) == 0x36000000u) *at = (w & 0xFFF8001Fu) | (((uint32_t)d & 0x3FFF) << 5);   // tbz/tbnz
    }
    void here(uint32_t *at) { patch(at, cur); }

private:
    uint32_t *base, *cur, *end;
};

}  // namespace a64
}  // namespace wm
