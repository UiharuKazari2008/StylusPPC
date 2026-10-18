// Thumb JIT against the interpreter. The guest program is ordinary Thumb-1 (what Windows Mobile
// user code is): arithmetic, shifts, loads and stores, push/pop including a page-crossing one,
// BL, BLX into a tiny ARM function, and a loop. Both CPUs run the same number of instructions.
#include "../src/arm/cpu.h"
#include "../src/arm/jit.h"
#include "../src/board/bus.h"
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace wm;

namespace {

void put16(std::vector<uint8_t> &m, uint32_t pa, uint16_t v) { memcpy(&m[pa], &v, 2); }
void put32(std::vector<uint8_t> &m, uint32_t pa, uint32_t v) { memcpy(&m[pa], &v, 4); }

struct Asm {
    uint32_t base;
    std::vector<uint16_t> c;
    std::string err;
    struct Load { size_t at; int rd; uint32_t value; int slot; };
    struct Fix { size_t at; uint32_t target; bool cond; };
    std::vector<Load> loads;
    std::vector<uint32_t> pool;
    std::vector<Fix> fixes;
    explicit Asm(uint32_t b) : base(b) {}
    uint32_t addr() const { return base + (uint32_t)c.size() * 2; }
    void emit(uint16_t w) { c.push_back(w); }
    void movs(int rd, int imm) { emit((uint16_t)(0x2000 | (rd << 8) | (imm & 0xFF))); }
    void addsi(int rd, int imm) { emit((uint16_t)(0x3000 | (rd << 8) | (imm & 0xFF))); }
    void subsi(int rd, int imm) { emit((uint16_t)(0x3800 | (rd << 8) | (imm & 0xFF))); }
    void addReg(int rd, int rn, int rm) { emit((uint16_t)(0x1800 | (rm << 6) | (rn << 3) | rd)); }
    void movHi(int rd, int rm) { emit((uint16_t)(0x4600 | ((rd & 8) << 4) | (rm << 3) | (rd & 7))); }
    void bx(int rm) { emit((uint16_t)(0x4700 | (rm << 3))); }
    void pushLr() { emit(0xB500); }
    void popPc() { emit(0xBD00); }
    void pushRegs(int mask) { emit((uint16_t)(0xB400 | (mask & 0xFF))); }
    void popRegs(int mask) { emit((uint16_t)(0xBC00 | (mask & 0xFF))); }
    void ldrImm(int rt, int rn, int words) { emit((uint16_t)(0x6800 | (words << 6) | (rn << 3) | rt)); }
    void strImm(int rt, int rn, int words) { emit((uint16_t)(0x6000 | (words << 6) | (rn << 3) | rt)); }
    void lslReg(int rd, int rm) { emit((uint16_t)(0x4080 | (rm << 3) | rd)); }
    void mul(int rd, int rm) { emit((uint16_t)(0x4340 | (rm << 3) | rd)); }
    void cmpReg(int rd, int rm) { emit((uint16_t)(0x4280 | (rm << 3) | rd)); }
    int poolSlot(uint32_t value) {
        for (size_t i = 0; i < pool.size(); i++) if (pool[i] == value) return (int)i;
        pool.push_back(value);
        return (int)pool.size() - 1;
    }
    void ldrLit(int rd, uint32_t value) { loads.push_back({c.size(), rd, value, poolSlot(value)}); emit(0); }
    size_t bcond(int cond) { size_t at = c.size(); emit((uint16_t)(0xD000 | (cond << 8))); return at; }
    size_t b() { size_t at = c.size(); emit(0xE000); return at; }
    void want(size_t at, uint32_t target, bool cond) { fixes.push_back({at, target, cond}); }
    void bl(uint32_t target, bool linkExchange) {
        uint32_t ia = addr();
        int32_t off = (int32_t)target - (int32_t)(ia + 4);
        int32_t hi = off >> 12;
        if (hi < -1024 || hi > 1023) { err = "bl out of range"; return; }
        emit((uint16_t)(0xF000 | (hi & 0x7FF)));
        emit((uint16_t)((linkExchange ? 0xE800 : 0xF800) | (((uint32_t)off >> 1) & 0x7FF)));
    }
    bool finish(std::vector<uint8_t> &mem) {
        if ((addr() & 2) && !pool.empty()) emit(0x46C0);  // mov r8, r8: align the pool, flags unchanged
        uint32_t poolAt = addr();
        for (uint32_t w : pool) { emit((uint16_t)w); emit((uint16_t)(w >> 16)); }
        for (auto &ld : loads) {
            uint32_t ia = base + (uint32_t)ld.at * 2;
            uint32_t pc4 = (ia + 4) & ~3u;
            uint32_t slot = poolAt + (uint32_t)ld.slot * 4;
            if (slot < pc4 || ((slot - pc4) & 3) || (slot - pc4) > 1020) { err = "literal out of range"; return false; }
            c[ld.at] = (uint16_t)(0x4800 | (ld.rd << 8) | ((slot - pc4) / 4));
        }
        for (auto &f : fixes) {
            uint32_t ia = base + (uint32_t)f.at * 2;
            int32_t off = (int32_t)f.target - (int32_t)(ia + 4);
            if (off & 1) { err = "odd branch"; return false; }
            int32_t imm = off / 2;
            if (f.cond) {
                if (imm < -128 || imm > 127) { err = "cond branch out of range"; return false; }
                c[f.at] = (uint16_t)((c[f.at] & 0xFF00) | (imm & 0xFF));
            } else {
                if (imm < -1024 || imm > 1023) { err = "branch out of range"; return false; }
                c[f.at] = (uint16_t)(0xE000 | (imm & 0x7FF));
            }
        }
        if (!err.empty()) return false;
        for (size_t i = 0; i < c.size(); i++) put16(mem, base + (uint32_t)i * 2, c[i]);
        return true;
    }
};

bool buildProgram(std::vector<uint8_t> &mem, uint32_t entry) {
    put32(mem, 0x00, 0xE59F0004);       // ldr r0, [pc, #4]
    put32(mem, 0x04, 0xE12FFF10);       // bx r0
    put32(mem, 0x08, 0xEAFFFFFE);       // b .  (idle)
    put32(mem, 0x0C, entry | 1);
    put32(mem, 0x3000, 0xE3A05011);     // mov r5, #0x11
    put32(mem, 0x3004, 0xE12FFF1E);     // bx lr
    put32(mem, 0x4000, 0x04030201);
    Asm a(entry);
    uint32_t fn = 0, popfn = 0, vOk = 0, vDone = 0, cOk = 0, cDone = 0, loop = 0;
    a.ldrLit(0, 0x8100);
    a.movHi(13, 0);
    a.ldrLit(0, 0x7FFFFFFF);
    a.movs(1, 1);
    a.addReg(0, 0, 1);
    size_t bvs = a.bcond(6);
    a.movs(7, 1);
    size_t bSkipV = a.b();
    vOk = a.addr();
    a.movs(7, 2);
    vDone = a.addr();
    a.want(bvs, vOk, true);
    a.want(bSkipV, vDone, false);
    a.movHi(9, 7);
    a.movs(0, 1);
    a.subsi(0, 1);
    a.emit(0x0001);                     // lsls r1, r0, #0  (C must stay set)
    size_t bcs = a.bcond(2);
    a.movs(7, 0);
    size_t bSkipC = a.b();
    cOk = a.addr();
    a.movs(7, 3);
    cDone = a.addr();
    a.want(bcs, cOk, true);
    a.want(bSkipC, cDone, false);
    a.movHi(10, 7);
    a.ldrLit(0, 0x4002);
    a.ldrImm(6, 0, 0);
    a.movs(0, 0x42);
    a.movHi(8, 0);
    a.ldrLit(2, 0x11111111);
    a.ldrLit(3, 0x22222222);
    a.ldrLit(0, 0x8004);
    a.movHi(13, 0);
    a.pushRegs(0x0C);
    a.movs(2, 0);
    a.movs(3, 0);
    a.popRegs(0x0C);
    a.ldrLit(0, 0x8100);
    a.movHi(13, 0);
    size_t blFnAt = a.c.size();
    a.bl(0, false);                     // rewritten once fn's address is known
    if (a.addr() & 2) a.emit(0x46C0);  // BLX suffix with bit 0 set is undefined; keep the pair word-aligned
    size_t blxAt = a.c.size();
    a.bl(0x3000, true);
    size_t blPop = a.c.size();
    a.bl(0, false);
    a.ldrLit(7, 0x410C);
    a.strImm(0, 7, 0);
    a.movs(0, 1);
    a.movs(1, 4);
    a.lslReg(0, 1);
    a.ldrLit(7, 0x4104);
    a.strImm(0, 7, 0);
    a.movs(0, 6);
    a.movs(1, 7);
    a.mul(0, 1);
    a.ldrLit(7, 0x4108);
    a.strImm(0, 7, 0);
    a.ldrLit(0, 0x4200);
    a.ldrLit(1, 0x89ABCDEF);
    a.strImm(1, 0, 0);
    a.ldrImm(7, 0, 0);
    a.movHi(11, 7);
    a.ldrLit(0, 0x4100);
    a.ldrLit(1, 0xA5A5A5A5);
    a.strImm(1, 0, 0);
    a.movs(4, 0);
    a.movs(1, 100);
    loop = a.addr();
    a.addsi(4, 1);
    a.subsi(1, 1);
    size_t bne = a.bcond(1);
    a.want(bne, loop, true);
    a.ldrLit(7, 0x08);
    a.bx(7);
    fn = a.addr();
    a.movs(7, 0x77);
    a.movHi(12, 7);
    a.bx(14);
    popfn = a.addr();
    a.movs(0, 9);
    a.pushLr();
    a.popPc();
    // Patch the two BL prefixes now that the targets exist. bl() already emitted a placeholder
    // pair aimed at 0; rewrite both halfwords in place.
    auto patchBl = [&](size_t at, uint32_t target) {
        uint32_t ia = entry + (uint32_t)at * 2;
        int32_t off = (int32_t)target - (int32_t)(ia + 4);
        int32_t hi = off >> 12;
        a.c[at] = (uint16_t)(0xF000 | (hi & 0x7FF));
        a.c[at + 1] = (uint16_t)(0xF800 | (((uint32_t)off >> 1) & 0x7FF));
    };
    patchBl(blFnAt, fn);
    patchBl(blPop, popfn);
    (void)blxAt;
    if (!a.err.empty()) { fprintf(stderr, "asm: %s\n", a.err.c_str()); return false; }
    return a.finish(mem);
}

struct Snap {
    uint32_t r[16] = {};
    uint32_t cpsr = 0;
    uint64_t insns = 0;
    uint32_t pc = 0;
    std::vector<uint8_t> mem;
};

Snap runImage(const std::vector<uint8_t> &image, bool useJit, uint64_t steps, bool *jitOn) {
    std::vector<uint8_t> mem = image;
    Bus bus;
    bus.logUnmapped = false;
    bus.mapMemory(0, (uint32_t)mem.size(), mem.data(), true);
    Snap s;
    {
        Cpu cpu(bus);
        cpu.core = CORE_ARMV5;
        if (useJit) {
            cpu.jit = jitCreate(cpu);
            cpu.jitEnabled = cpu.jit != nullptr;
            if (jitOn) *jitOn = cpu.jitEnabled;
        }
        cpu.setPc(0);
        cpu.run(steps);
        for (int i = 0; i < 16; i++) s.r[i] = cpu.reg(i);
        s.cpsr = cpu.getCpsr();
        s.insns = cpu.insnCount;
        s.pc = cpu.pc();
        if (cpu.jit) jitDestroy(cpu.jit);
        cpu.jit = nullptr;
    }
    s.mem = std::move(mem);
    return s;
}

uint64_t stepsUntilIdle(const std::vector<uint8_t> &image) {
    std::vector<uint8_t> mem = image;
    Bus bus;
    bus.logUnmapped = false;
    bus.mapMemory(0, (uint32_t)mem.size(), mem.data(), true);
    Cpu cpu(bus);
    cpu.core = CORE_ARMV5;
    cpu.setPc(0);
    uint64_t n = 0;
    while (cpu.pc() != 0x08 && n < 200000) {
        uint64_t before = cpu.insnCount;
        cpu.run(1);
        n += cpu.insnCount - before;
    }
    return cpu.pc() == 0x08 ? n : 0;
}

int expectProgram(const Snap &s, const char *name) {
    auto bad = [&](const char *what, uint32_t got, uint32_t want) {
        fprintf(stderr, "%s: %s = %08x, want %08x (pc %08x cpsr %08x)\n", name, what, got, want, s.pc, s.cpsr);
        return 1;
    };
    if (s.pc != 0x08) return bad("pc", s.pc, 0x08);
    if (s.cpsr & 0x20) return bad("T bit", s.cpsr, s.cpsr & ~0x20u);
    if ((s.cpsr & 0xF0000000u) != 0x60000000u) return bad("flags", s.cpsr & 0xF0000000u, 0x60000000u);
    if (s.r[1] != 0) return bad("r1", s.r[1], 0);
    if (s.r[2] != 0x11111111u) return bad("r2", s.r[2], 0x11111111u);
    if (s.r[3] != 0x22222222u) return bad("r3", s.r[3], 0x22222222u);
    if (s.r[4] != 100) return bad("r4", s.r[4], 100);
    if (s.r[5] != 0x11) return bad("r5", s.r[5], 0x11);
    if (s.r[6] != 0x02010403u) return bad("r6", s.r[6], 0x02010403u);
    if (s.r[8] != 0x42) return bad("r8", s.r[8], 0x42);
    if (s.r[9] != 2) return bad("r9", s.r[9], 2);
    if (s.r[10] != 3) return bad("r10", s.r[10], 3);
    if (s.r[11] != 0x89ABCDEFu) return bad("r11", s.r[11], 0x89ABCDEFu);
    if (s.r[12] != 0x77) return bad("r12", s.r[12], 0x77);
    if (s.r[13] != 0x8100) return bad("sp", s.r[13], 0x8100);
    uint32_t w;
    memcpy(&w, &s.mem[0x4100], 4);
    if (w != 0xA5A5A5A5u) return bad("mem 4100", w, 0xA5A5A5A5u);
    memcpy(&w, &s.mem[0x4104], 4);
    if (w != 16) return bad("mem 4104", w, 16);
    memcpy(&w, &s.mem[0x4108], 4);
    if (w != 42) return bad("mem 4108", w, 42);
    memcpy(&w, &s.mem[0x410C], 4);
    if (w != 9) return bad("mem 410C", w, 9);
    memcpy(&w, &s.mem[0x4200], 4);
    if (w != 0x89ABCDEFu) return bad("mem 4200", w, 0x89ABCDEFu);
    return 0;
}

int compareCase(uint32_t entry, bool haveJit) {
    std::vector<uint8_t> image(1u << 20, 0);
    if (!buildProgram(image, entry)) return 1;
    uint64_t n = stepsUntilIdle(image);
    if (!n) { fprintf(stderr, "entry %08x: interpreter did not reach idle\n", entry); return 1; }
    Snap interp = runImage(image, false, n + 8, nullptr);
    char name[32];
    snprintf(name, sizeof name, "thumb@%08x", entry);
    if (expectProgram(interp, name)) return 1;
    if (!haveJit) return 0;
    bool jitOn = false;
    Snap jit = runImage(image, true, n + 8, &jitOn);
    if (!jitOn) { fprintf(stderr, "jit was not created\n"); return 1; }
    if (jit.pc != interp.pc || jit.cpsr != interp.cpsr || jit.insns != interp.insns) {
        fprintf(stderr, "%s: jit pc %08x cpsr %08x insns %llu | interp pc %08x cpsr %08x insns %llu\n",
                name, jit.pc, jit.cpsr, (unsigned long long)jit.insns, interp.pc, interp.cpsr,
                (unsigned long long)interp.insns);
        return 1;
    }
    for (int i = 0; i < 16; i++) if (jit.r[i] != interp.r[i]) {
        fprintf(stderr, "%s: r%d jit %08x interp %08x\n", name, i, jit.r[i], interp.r[i]);
        return 1;
    }
    if (jit.mem != interp.mem) {
        for (size_t i = 0; i < jit.mem.size(); i += 4)
            if (memcmp(&jit.mem[i], &interp.mem[i], 4)) {
                fprintf(stderr, "%s: RAM differs at %08zx\n", name, i);
                return 1;
            }
    }
    printf("thumb@%08x matches (%llu insns)\n", entry, (unsigned long long)n);
    return 0;
}

int bench(bool haveJit) {
    if (!haveJit) return 0;
    const uint32_t N = 1000000;
    std::vector<uint8_t> mem(1u << 20, 0);
    put32(mem, 0x00, 0xE59F0004);
    put32(mem, 0x04, 0xE12FFF10);
    put32(mem, 0x08, 0xEAFFFFFE);
    put32(mem, 0x0C, 0x1001);
    Asm a(0x1000);
    a.movs(0, 0);
    a.ldrLit(1, N);
    uint32_t loop = a.addr();
    a.addsi(0, 1);
    a.cmpReg(0, 1);
    size_t bne = a.bcond(1);
    a.want(bne, loop, true);
    a.ldrLit(2, 0x08);
    a.bx(2);
    if (!a.finish(mem)) { fprintf(stderr, "bench asm: %s\n", a.err.c_str()); return 1; }
    std::vector<uint8_t> image = mem;
    Bus bus;
    bus.logUnmapped = false;
    bus.mapMemory(0, (uint32_t)image.size(), image.data(), true);
    Cpu cpu(bus);
    cpu.core = CORE_ARMV5;
    cpu.jit = jitCreate(cpu);
    cpu.jitEnabled = cpu.jit != nullptr;
    cpu.setPc(0);
    uint64_t steps = (uint64_t)N * 4 + 1000;
    auto t0 = std::chrono::steady_clock::now();
    cpu.run(steps);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    uint32_t r0 = cpu.reg(0);
    JitStats st = jitStats(cpu.jit);
    if (cpu.jit) jitDestroy(cpu.jit);
    if (r0 != N || cpu.pc() != 0x08) {
        fprintf(stderr, "bench: r0 %08x pc %08x\n", r0, cpu.pc());
        return 1;
    }
    printf("thumb loop: %.1f MIPS (%llu insns, %llu blocks, %llu chains)\n",
           cpu.insnCount / sec / 1e6, (unsigned long long)cpu.insnCount,
           (unsigned long long)st.blocksCompiled, (unsigned long long)st.chains);
    return 0;
}

uint32_t ror32(uint32_t v, uint32_t s) { return (v >> s) | (v << (32 - s)); }

void refShift(int kind, uint32_t a, uint32_t amount, uint32_t cIn, uint32_t &res, uint32_t &nib) {
    uint32_t s = amount & 0xFF, c = cIn;
    switch (kind) {
        case 0:
            if (s == 0) res = a;
            else if (s < 32) { c = (a >> (32 - s)) & 1; res = a << s; }
            else if (s == 32) { c = a & 1; res = 0; }
            else { c = 0; res = 0; }
            break;
        case 1:
            if (s == 0) res = a;
            else if (s < 32) { c = (a >> (s - 1)) & 1; res = a >> s; }
            else if (s == 32) { c = a >> 31; res = 0; }
            else { c = 0; res = 0; }
            break;
        case 2:
            if (s == 0) res = a;
            else if (s < 32) { c = (a >> (s - 1)) & 1; res = (uint32_t)((int32_t)a >> s); }
            else { c = a >> 31; res = (uint32_t)((int32_t)a >> 31); }
            break;
        default:
            if (s == 0) res = a;
            else if ((s & 31) == 0) { c = a >> 31; res = a; }
            else { c = (a >> ((s & 31) - 1)) & 1; res = ror32(a, s & 31); }
            break;
    }
    uint32_t n = res >> 31, z = res == 0;
    nib = (n << 3) | (z << 2) | (c << 1) | 1;  // V stays set by the setup add
}

bool buildShift(std::vector<uint8_t> &mem, int kind, bool same, uint32_t rows) {
    put32(mem, 0x00, 0xE59F0004);
    put32(mem, 0x04, 0xE12FFF10);
    put32(mem, 0x08, 0xEAFFFFFE);
    put32(mem, 0x0C, 0x1001);
    Asm a(0x1000);
    a.movs(2, 1); a.movHi(9, 2);
    a.movs(2, 2); a.movHi(10, 2);
    a.movs(2, 4); a.movHi(11, 2);
    a.movs(2, 8); a.movHi(12, 2);
    a.ldrLit(4, 0x5000);
    a.ldrLit(6, 0x6000);
    uint32_t loop = a.addr();
    a.ldrImm(0, 4, 0);
    a.ldrImm(1, 4, 1);
    a.ldrImm(5, 4, 2);
    a.movs(7, 0);
    a.emit(0x2D00);  // cmp r5, #0
    size_t toZero = a.bcond(0);
    a.ldrLit(2, 0x80000000);
    a.movHi(8, 2);
    a.movHi(3, 8);
    a.addReg(2, 2, 3);
    size_t toShift = a.b();
    a.want(toZero, a.addr(), true);
    a.ldrLit(2, 0x7FFFFFFF);
    a.movs(3, 1);
    a.addReg(2, 2, 3);
    a.want(toShift, a.addr(), false);
    static const uint16_t opBase[] = {0x4080, 0x40C0, 0x4100, 0x41C0};  // LSL LSR ASR ROR r0, r0
    a.emit((uint16_t)(opBase[kind] | (same ? 0 : (1 << 3))));
    auto sample = [&](int cond, uint16_t addEnc) {
        size_t taken = a.bcond(cond);
        size_t skip = a.b();
        a.want(taken, a.addr(), true);
        a.emit(addEnc);
        a.want(skip, a.addr(), false);
    };
    sample(4, 0x4467);  // N: add r7, r12
    sample(0, 0x445F);  // Z: add r7, r11
    sample(2, 0x4457);  // C: add r7, r10
    sample(6, 0x444F);  // V: add r7, r9
    a.strImm(0, 6, 0);
    a.strImm(7, 6, 1);
    a.addsi(6, 8);
    a.addsi(4, 12);
    a.ldrLit(3, 0x5000 + rows * 12);
    a.cmpReg(4, 3);
    size_t again = a.bcond(1);
    a.want(again, loop, true);
    a.ldrLit(3, 8);
    a.bx(3);
    if (!a.err.empty() || !a.finish(mem)) {
        fprintf(stderr, "shift asm: %s\n", a.err.empty() ? "range" : a.err.c_str());
        return false;
    }
    return true;
}

int shiftCases(bool haveJit) {
    const uint32_t vals[] = {0, 1, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu};
    const uint32_t amts[] = {0, 1, 8, 31, 32, 33, 64, 255};
    const char *name[] = {"lsl", "lsr", "asr", "ror"};
    for (int same = 0; same < 2; same++) {
        uint32_t rows = 0;
        uint32_t tv[80], ta[80], tc[80];
        for (uint32_t v : vals) for (uint32_t m : amts) for (uint32_t c = 0; c < 2; c++) {
            if (same && m != (v & 0xFF)) continue;  // amount is the value itself
            tv[rows] = v; ta[rows] = same ? v : m; tc[rows] = c; rows++;
        }
        for (int kind = 0; kind < 4; kind++) {
            std::vector<uint8_t> image(1u << 20, 0);
            if (!buildShift(image, kind, same, rows)) return 1;
            for (uint32_t i = 0; i < rows; i++) {
                put32(image, 0x5000 + i * 12, tv[i]);
                put32(image, 0x5000 + i * 12 + 4, ta[i]);
                put32(image, 0x5000 + i * 12 + 8, tc[i]);
            }
            uint64_t n = stepsUntilIdle(image);
            if (!n) { fprintf(stderr, "%s%s: did not finish\n", name[kind], same ? " same" : ""); return 1; }
            Snap got = runImage(image, false, n + 4, nullptr);
            for (uint32_t i = 0; i < rows; i++) {
                uint32_t res, nib, w, f;
                refShift(kind, tv[i], ta[i], tc[i], res, nib);
                memcpy(&w, &got.mem[0x6000 + i * 8], 4);
                memcpy(&f, &got.mem[0x6000 + i * 8 + 4], 4);
                if (w != res || f != nib) {
                    fprintf(stderr, "%s%s v=%08x s=%u cin=%u -> %08x flags %x, want %08x flags %x\n",
                            name[kind], same ? " same" : "", tv[i], ta[i] & 0xFF, tc[i], w, f, res, nib);
                    return 1;
                }
            }
            if (!haveJit) continue;
            bool on = false;
            Snap jit = runImage(image, true, n + 4, &on);
            if (!on || jit.mem != got.mem || jit.cpsr != got.cpsr) {
                fprintf(stderr, "%s%s: jit diverged (cpsr jit %08x interp %08x)\n",
                        name[kind], same ? " same" : "", jit.cpsr, got.cpsr);
                return 1;
            }
        }
    }
    printf("thumb shifts match\n");
    return 0;
}

int shiftBench() {
    const uint32_t N = 1000000;
    std::vector<uint8_t> mem(1u << 20, 0);
    put32(mem, 0x00, 0xE59F0004);
    put32(mem, 0x04, 0xE12FFF10);
    put32(mem, 0x08, 0xEAFFFFFE);
    put32(mem, 0x0C, 0x1001);
    Asm a(0x1000);
    a.movs(0, 1);
    a.movs(1, 0);          // ROR by 0: result stays 1, carry stays clear
    a.movs(2, 0);
    a.ldrLit(3, N);
    uint32_t loop = a.addr();
    a.emit(0x41C8);        // rors r0, r1
    a.addsi(2, 1);
    a.cmpReg(2, 3);
    size_t bne = a.bcond(1);
    a.want(bne, loop, true);
    a.ldrLit(7, 0x08);
    a.bx(7);
    if (!a.finish(mem)) { fprintf(stderr, "shift bench asm: %s\n", a.err.c_str()); return 1; }
    Bus bus;
    bus.logUnmapped = false;
    bus.mapMemory(0, (uint32_t)mem.size(), mem.data(), true);
    Cpu cpu(bus);
    cpu.core = CORE_ARMV5;
    cpu.jit = jitCreate(cpu);
    cpu.jitEnabled = cpu.jit != nullptr;
    cpu.setPc(0);
    auto t0 = std::chrono::steady_clock::now();
    cpu.run((uint64_t)N * 5 + 1000);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    JitStats st = jitStats(cpu.jit);
    uint32_t r0 = cpu.reg(0);
    if (cpu.jit) jitDestroy(cpu.jit);
    if (r0 != 1 || cpu.pc() != 0x08) {
        fprintf(stderr, "shift bench: r0 %08x pc %08x\n", r0, cpu.pc());
        return 1;
    }
    printf("thumb shift loop: %.1f MIPS (%llu helper calls)\n", cpu.insnCount / sec / 1e6,
           (unsigned long long)st.helperCalls);
    return 0;
}

}  // namespace

int thumbJitSelfTest() {
    Bus bus;
    Cpu probe(bus);
    probe.jit = jitCreate(probe);
    bool haveJit = probe.jit != nullptr;
    if (probe.jit) jitDestroy(probe.jit);
    if (compareCase(0x1000, haveJit) || compareCase(0x1002, haveJit) || shiftCases(haveJit)) return 1;
    if (!haveJit) {
        printf("thumb jit selftest: interpreter program ok; JIT skipped (host is not AArch64)\n");
        return 0;
    }
    if (bench(true) || shiftBench()) return 1;
    printf("thumb jit selftest: ok\n");
    return 0;
}
