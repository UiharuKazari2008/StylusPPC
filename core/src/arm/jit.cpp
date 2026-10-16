// ARM (A32) -> AArch64 block translator.
//
// Design:
//  * One block = straight-line guest code up to a branch, PC write, untranslatable instruction,
//    page end or kMaxInsns. Blocks are keyed by guest PC and host address of the code (so the
//    same physical code at different VAs, and different code at the same VA, never mix).
//  * Guest NZCV lives in the host NZCV register while a block runs: A32 and A64 flag semantics
//    match for ADDS/SUBS/ADCS/SBCS. Logical S-ops merge N/Z with the shifter carry by hand.
//  * Up to six guest registers per block are cached in callee-saved host registers; the rest are
//    read and written through the CpuState block (x20).
//  * Loads/stores use the CPU's FastTlb inline (host address = addend + MVA); misses, I/O,
//    misalignment and faults call C++ helpers that report through CpuState::jitStatus. Those
//    thunks preserve x7-x15, so temporaries survive a slow access.
//  * The instruction budget (CpuState::jitBudget) lives in x29 while generated code runs; it is
//    loaded on entry and stored back on every exit.
//  * Instructions that are rare or need full CPU context run through jitInterp() inside the block;
//    ones that can change mode/MMU state end the block and are run by the dispatcher.
//  * Code pages are write-protected in the fast TLB; a write invalidates the page's blocks.
#include "jit.h"
#include "a64.h"
#include "cpu.h"
#include "../util/log.h"
#include "disasm.h"
#include <algorithm>
#include <cstdio>
#include <sys/mman.h>
#include <cstddef>
#include <deque>
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace wm {

using namespace a64;

namespace {

enum : uint32_t {
    OFF_NZCV = 64, OFF_STATUS = 68, OFF_TLBR = 72, OFF_TLBW = 80, OFF_GENPID = 88, OFF_EXEC = 96,
    OFF_BUDGET = 100, OFF_LASTEXIT = 104,
};
static_assert(offsetof(CpuState, nzcv) == OFF_NZCV, "layout");
static_assert(offsetof(CpuState, jitStatus) == OFF_STATUS, "layout");
static_assert(offsetof(CpuState, tlbRead) == OFF_TLBR, "layout");
static_assert(offsetof(CpuState, tlbWrite) == OFF_TLBW, "layout");
static_assert(offsetof(CpuState, genPid) == OFF_GENPID, "layout");
static_assert(offsetof(CpuState, jitExecuted) == OFF_EXEC, "layout");
static_assert(offsetof(CpuState, jitBudget) == OFF_BUDGET, "layout");
static_assert(offsetof(CpuState, jitLastExit) == OFF_LASTEXIT, "layout");
static_assert(sizeof(FastTlb) == 16, "layout");

const Reg STATE = X20, TLBR = X21, TLBW = X22, GENPID = X23, BUDGET = FP;
const Reg kCacheRegs[] = {X19, X24, X25, X26, X27, X28};
const int kNumCache = 6;
int kMaxInsns = 64;
const size_t kBufBytes = 128u << 20;

inline uint32_t ror32(uint32_t v, uint32_t s) { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; }

}  // namespace

// ---------------------------------------------------------------------------------------------
// Helpers called from generated code.

uint32_t jitRead(Cpu *c, uint32_t va, uint32_t kind) {
    int size = kind & 7;
    bool sgn = kind & 16;
    bool user = c->mode == MODE_USR;
    if (size > 1 && (va & (size - 1)) && (c->cp15_ctrl & 2)) {
        c->cp15_fsr = 1;
        c->cp15_far = va;
        c->jitStatus |= 1;
        return 0;
    }
    uint32_t a = va & ~(uint32_t)(size - 1);
    uint32_t v;
    if (!c->slowRead(c->vaMod(a), size, user, v)) {
        c->jitStatus |= 1;
        return 0;
    }
    if (size == 4) v = ror32(v, (va & 3) * 8);
    else if (sgn) v = size == 2 ? (uint32_t)(int32_t)(int16_t)v : (uint32_t)(int32_t)(int8_t)v;
    return v;
}

void jitWrite(Cpu *c, uint32_t va, uint32_t v, uint32_t size) {
    bool user = c->mode == MODE_USR;
    if (size > 1 && (va & (size - 1)) && (c->cp15_ctrl & 2)) {
        c->cp15_fsr = 1 | 0x800;
        c->cp15_far = va;
        c->jitStatus |= 1;
        return;
    }
    bool io;
    if (!c->slowWrite(c->vaMod(va & ~(size - 1)), (int)size, user, v, io)) {
        c->jitStatus |= 1;
        return;
    }
    if (io) c->jitStatus |= 4;
}

// Executes one ARM instruction with the interpreter. Returns 0 to continue the block, or an exit
// reason with r15 already set.
// Debug (jitProfileInterp): histogram of instructions run through jitInterp inside blocks.
static std::unordered_map<uint32_t, uint64_t> *g_helperHist = nullptr;

uint32_t jitInterp(Cpu *c, uint32_t insn, uint32_t pc) {
    if (g_helperHist) (*g_helperHist)[insn & 0x0FF000F0]++;
    c->unpackFlags(c->nzcv);
    c->r[15] = pc + 8;
    c->branched = false;
    bool userBefore = c->mode == MODE_USR;
    uint32_t genBefore = c->tlbGen, pidBefore = c->cp15_pid, ctrlBefore = c->cp15_ctrl;
    uint32_t ret = 0;
    try {
        c->execArm(insn);
        if (c->branched) {
            ret = JIT_NEXT;
        } else if ((c->jitStatus & 6) || c->halted || (c->mode == MODE_USR) != userBefore ||
                   c->tlbGen != genBefore || c->cp15_pid != pidBefore || c->cp15_ctrl != ctrlBefore ||
                   (c->irqLine && !c->fI) || (c->fiqLine && !c->fF) || c->prefetchLeft) {
            // Something generated code depends on (privilege, TLB, MMU, pending interrupt) changed.
            c->r[15] = pc + 4;
            ret = JIT_STOP;
        }
    } catch (const MemAbort &) {
        c->r[15] = pc;
        ret = JIT_ABORT;
    }
    c->nzcv = c->packFlags();
    return ret;
}

// ---------------------------------------------------------------------------------------------

struct Block {
    uint32_t pc;
    uint64_t host;
    uint32_t ppn;
    uint32_t ninsn;
    uint32_t *code;
    uint32_t words;  // generated code size (debug map)
    bool dead;
};

class Jit {
public:
    explicit Jit(Cpu &c);
    ~Jit();
    bool ok() const { return buf != nullptr; }
    bool run(uint64_t budget, uint32_t &reason, uint32_t &executed);
    void flush();
    void invalidatePage(uint32_t ppn);
    JitStats stats = {};
    bool profileInterp = false;
    void dumpMap(const char *path) const {
        FILE *f = fopen(path, "w");
        if (!f) return;
        fprintf(f, "stubs %llx %llx\n", (unsigned long long)(uintptr_t)buf, (unsigned long long)(uintptr_t)(buf + codeStart));
        for (const Block &b : pool)
            if (!b.dead) fprintf(f, "%llx %x %08x %u\n", (unsigned long long)(uintptr_t)b.code, b.words * 4, b.pc, b.ninsn);
        fclose(f);
        std::string bin = std::string(path) + ".bin";  // the code buffer itself, for disassembly
        if (FILE *g = fopen(bin.c_str(), "wb")) { fwrite(buf, 4, used, g); fclose(g); }
    }
    std::unordered_map<uint32_t, uint64_t> interpHist;

private:
    friend class Translator;
    Cpu &cpu;
    uint32_t *buf = nullptr;
    size_t capWords = 0, used = 0, codeStart = 0;
    uint32_t (*enter)(CpuState *, uint32_t *) = nullptr;
    uint32_t *exitLabel = nullptr;
    uint32_t *dispatchStub = nullptr;  // w0 = exit reason, w1 = target pc
    uint32_t *exitPc = nullptr;        // w0 = reason, w1 = pc: save flags/pc and leave
    uint32_t *exitChain = nullptr;     // as exitPc, plus x17 = patchable branch to link later
    // Shared slow-path thunks (preserve guest flags; called with bl):
    uint32_t *readThunk[32] = {};      // indexed by kind (size | signed<<4); w1 = va -> w0
    uint32_t *writeThunk[5] = {};      // indexed by size; w1 = va, w2 = value
    uint32_t *interpThunk = nullptr;   // w1 = insn, w2 = pc -> w0 = exit reason or 0
    // Indirect-branch table: generated code looks blocks up by host address + pc.
    struct IbEntry {
        uint64_t host;
        uint32_t pc, pad;
        uint32_t *code;
        uint64_t pad2;
    };
    static const int kIb = 1 << 16;
    IbEntry *ib = nullptr;
    static uint32_t ibIndex(uint64_t host) { return (uint32_t)(host >> 2) & (kIb - 1); }
    void ibInsert(Block *b) {
        IbEntry &e = ib[ibIndex(b->host)];
        e.host = b->host;
        e.pc = b->pc;
        e.code = b->code;
    }
    void ibRemove(Block *b) {
        IbEntry &e = ib[ibIndex(b->host)];
        if (e.host == b->host && e.pc == b->pc) e.host = 0;
    }
    static const int kFast = 1 << 16;
    Block *fast[kFast] = {};
    std::unordered_map<uint64_t, Block *> map;
    std::unordered_map<uint32_t, std::vector<Block *>> pages;
    std::unordered_map<uint32_t, uint64_t> codeLines;  // per page: 64-byte lines holding code
public:
    bool codeWrite(uint32_t pa, uint32_t size);
private:
    std::deque<Block> pool;
    std::vector<Block *> freeList;
    Block *compile(uint32_t pc, uint8_t *host);
    void chainTo(uint32_t *site);
    static uint64_t key(uint32_t pc, uint64_t host) { return host * 0x9E3779B97F4A7C15ull ^ pc; }
};

// ---------------------------------------------------------------------------------------------
// Translator

class Translator {
public:
    Translator(Jit &j, Emitter &e, uint32_t pc0, const uint32_t *code) : j(j), e(e), pc0(pc0), code(code) {}
    uint32_t translate(uint32_t maxInsns);  // returns number of guest instructions in the block

private:
    Jit &j;
    Emitter &e;
    uint32_t pc0;
    const uint32_t *code;
    int8_t cacheOf[16];      // guest reg -> index in kCacheRegs, or -1
    int8_t cachedGuest[kNumCache];
    uint32_t dirty = 0;
    std::vector<std::function<void()>> deferred;
    std::vector<uint32_t *> abortSites;
    uint32_t idx = 0;        // current instruction index
    uint32_t blockN = 0;     // instructions in the block
    uint32_t pc = 0;         // current instruction address

    enum Kind { INLINE, HELPER, END_BEFORE };
    Kind classify(uint32_t insn) const;
    void allocate(uint32_t n);
    void usage(uint32_t insn, int counts[16]) const;

    Reg rd(int g, Reg tmp, uint32_t pcOffset = 8);
    Reg dst(int g, Reg tmp) { return cacheOf[g] >= 0 ? kCacheRegs[cacheOf[g]] : tmp; }
    void commit(int g, Reg r);
    void spillDirty();
    void reloadCached();
    void addExecuted(uint32_t n);
    void exitConst(uint32_t reason, uint32_t pcValue, uint32_t executed);
    void exitReg(uint32_t reason, Reg pcReg, uint32_t executed);
    void deferredExitConst(uint32_t *branch, uint32_t reason, uint32_t pcValue, uint32_t executed);
    void callHelper(void *fn);
    void emitHelperInsn(uint32_t insn);
    void tlbProbe(Reg a, Reg table);
    // Load (into dest) or store (val) at the guest VA in addr. ioStop, for stores, emits what
    // runs when the slow path hit I/O or a code page: the rest of the instruction, then an exit.
    void memAccess(Reg addr, Reg val, Reg dest, bool store, int size, bool sgn,
                   std::function<void()> ioStop = nullptr);
    bool emit(uint32_t insn, bool &ended);           // returns false if not handled
    bool emitDataProc(uint32_t insn, bool &ended);
    bool emitSingle(uint32_t insn, bool &ended);
    bool emitHalf(uint32_t insn, bool &ended);
    bool emitBlock(uint32_t insn, bool &ended);
    bool emitMul(uint32_t insn);
    Reg operand2(uint32_t insn, Reg tmp, Reg tmpRm, int &carry);  // carry: -1 none, 0/1 const, 2 in X17 bit0
    void logicFlags(Reg res, int carry);
};

void Translator::usage(uint32_t insn, int counts[16]) const {
    switch ((insn >> 25) & 7) {
        case 4:
            counts[(insn >> 16) & 0xF] += 2;
            for (int i = 0; i < 15; i++) if (insn & (1u << i)) counts[i]++;
            return;
        case 5: return;
        default:
            counts[(insn >> 16) & 0xF]++;
            counts[(insn >> 12) & 0xF]++;
            counts[insn & 0xF]++;
            counts[(insn >> 8) & 0xF]++;
    }
}

void Translator::allocate(uint32_t n) {
    int counts[16] = {};
    for (uint32_t i = 0; i < n; i++) usage(code[i], counts);
    counts[15] = 0;
    memset(cacheOf, -1, sizeof cacheOf);
    for (int k = 0; k < kNumCache; k++) {
        int best = -1;
        for (int g = 0; g < 15; g++)
            if (cacheOf[g] < 0 && counts[g] > 1 && (best < 0 || counts[g] > counts[best])) best = g;
        cachedGuest[k] = (int8_t)best;
        if (best >= 0) cacheOf[best] = (int8_t)k;
    }
}

Reg Translator::rd(int g, Reg tmp, uint32_t pcOffset) {
    if (g == 15) { e.mov32(tmp, pc + pcOffset); return tmp; }
    if (cacheOf[g] >= 0) return kCacheRegs[cacheOf[g]];
    e.ldrw(tmp, STATE, g * 4);
    return tmp;
}

void Translator::commit(int g, Reg r) {
    if (cacheOf[g] >= 0) {
        Reg h = kCacheRegs[cacheOf[g]];
        if (h != r) e.movr(h, r);
        dirty |= 1u << g;
    } else {
        e.strw(r, STATE, g * 4);
    }
}

void Translator::spillDirty() {
    for (int k = 0; k < kNumCache; k++) {
        int g = cachedGuest[k];
        if (g >= 0 && (dirty & (1u << g))) e.strw(kCacheRegs[k], STATE, g * 4);
    }
}

void Translator::reloadCached() {
    for (int k = 0; k < kNumCache; k++)
        if (cachedGuest[k] >= 0) e.ldrw(kCacheRegs[k], STATE, cachedGuest[k] * 4);
    dirty = 0;
}

// Instruction accounting: a block reserves all of its instructions from the budget on entry, so
// only exits taken part-way through hand the unexecuted remainder back.
void Translator::addExecuted(uint32_t n) {
    if (n >= blockN) return;
    e.addi(BUDGET, BUDGET, blockN - n);
}

void Translator::exitConst(uint32_t reason, uint32_t pcValue, uint32_t executed) {
    spillDirty();
    addExecuted(executed);
    if (reason == JIT_NEXT && ((pcValue ^ pc0) & ~0xFFFu) == 0) {
        // Same-page target: this branch is patched to jump straight into the target block once
        // it exists (flags stay in NZCV, registers are in state).
        uint32_t *patch = e.b();
        e.here(patch);
        e.mov32(X1, pcValue);
        e.adr(X17, patch);
        e.movz(X0, reason);
        e.bTo(j.exitChain);
        return;
    }
    e.mov32(X1, pcValue);
    e.movz(X0, reason);
    e.bTo((reason == JIT_NEXT || reason == JIT_BX) ? j.dispatchStub : j.exitPc);
}

void Translator::exitReg(uint32_t reason, Reg pcReg, uint32_t executed) {
    spillDirty();
    if (pcReg != X1) e.movr(X1, pcReg);
    addExecuted(executed);
    e.movz(X0, reason);
    e.bTo(j.dispatchStub);
}

void Translator::deferredExitConst(uint32_t *branch, uint32_t reason, uint32_t pcValue, uint32_t executed) {
    uint32_t savedDirty = dirty;
    deferred.push_back([=]() {
        uint32_t d = dirty;
        dirty = savedDirty;
        e.here(branch);
        exitConst(reason, pcValue, executed);
        dirty = d;
    });
}

void Translator::callHelper(void *fn) {
    e.mov64(X16, (uint64_t)(uintptr_t)fn);
    e.blr(X16);
    j.stats.helperCalls++;
}

// Runs the instruction through jitInterp; cached registers and flags round-trip through state.
void Translator::emitHelperInsn(uint32_t insn) {
    spillDirty();
    e.mov32(X1, insn);
    e.mov32(X2, pc);
    e.blTo(j.interpThunk);
    j.stats.helperCalls++;
    reloadCached();
    uint32_t *out = e.cbnz(X0);
    uint32_t exec = idx + 1;
    deferred.push_back([=]() {
        e.here(out);
        addExecuted(exec);
        e.strx(ZR, STATE, OFF_LASTEXIT);
        e.bTo(j.exitLabel);  // w0 = reason; registers, flags and r15 already in state
    });
}

// X3 = modified VA of `a` (FCSE: VAs below 32 MB get the process ID), X6 = the fast TLB entry's
// addend from `table`, X5 = 0 on a hit. Flag-free; clobbers X4, X16, X17; X3 is zero-extended.
// VAs at or above 32 MB (most kernel and DLL accesses) skip the PID.
void Translator::tlbProbe(Reg a, Reg table) {
    e.ubfx(X4, a, 12, 12);  // the index does not depend on the PID (bits 25 and up)
    e.lsri(X16, a, 25);
    e.movr(X3, a);
    uint32_t *high = e.cbnz(X16);
    e.lsrix(X17, GENPID, 32);
    e.orr(X3, a, X17);
    e.here(high);
    e.addx(X4, table, X4, LSL, 4);
    e.ldp(X5, X6, X4, 0);
    e.lsri(X16, X3, 12);
    e.orr(X16, X16, GENPID);
    e.eor(X5, X5, X16);
}

void Translator::memAccess(Reg addr, Reg val, Reg dest, bool store, int size, bool sgn, std::function<void()> ioStop) {
    int lg = size == 4 ? 2 : size == 2 ? 1 : 0;
    tlbProbe(addr, store ? TLBW : TLBR);
    uint32_t *miss = e.cbnz(X5);
    uint32_t *misaligned = nullptr;
    if (size > 1) {
        e.ubfx(X16, X3, 0, lg);
        misaligned = e.cbnz(X16);
    }
    if (store) e.strReg(lg, val, X6, X3);
    else e.ldrReg(lg, dest, X6, X3, sgn);
    uint32_t *resume = e.pos();
    uint32_t thisPc = pc, exec = idx + 1;
    deferred.push_back([=]() {
        // Slow path: w1 = va, w2 = value; the thunk returns a loaded value in w0.
        e.here(miss);
        if (misaligned) e.here(misaligned);
        if (store && val != X2) e.movr(X2, val);
        if (addr != X1) e.movr(X1, addr);
        e.blTo(store ? j.writeThunk[size] : j.readThunk[(uint32_t)size | (sgn ? 16u : 0u)]);
        e.ldrw(X16, STATE, OFF_STATUS);
        uint32_t *ok = e.tbz(X16, 0);
        e.mov32(X1, thisPc);
        e.movz(X2, exec);
        abortSites.push_back(e.b());
        e.here(ok);
        if (!store && dest != X0) e.movr(dest, X0);
        if (ioStop) {
            // A store that reached I/O or a code page (status bit 1 or 2): finish and leave.
            e.lsri(X16, X16, 1);
            uint32_t *quiet = e.cbz(X16);
            ioStop();
            e.here(quiet);
        }
        e.bTo(resume);
    });
}

Translator::Kind Translator::classify(uint32_t insn) const {
    uint32_t cond = insn >> 28;
    if (cond == 0xF) {
        if ((insn & 0x0E000000) == 0x0A000000) return INLINE;   // BLX imm
        if ((insn & 0x0D70F000) == 0x0550F000) return INLINE;   // PLD
        return HELPER;                                           // undefined
    }
    switch ((insn >> 25) & 7) {
        case 0:
            if ((insn & 0xF0) == 0x90) {
                if ((insn & 0x0F000000) == 0) return (insn & (1 << 20)) ? HELPER : INLINE;  // multiply
                return HELPER;                                                              // swp
            }
            if ((insn & 0x90) == 0x90) {
                uint32_t sh = (insn >> 5) & 3;
                if (!(insn & (1 << 20)) && sh != 1) return HELPER;  // LDRD/STRD
                return INLINE;
            }
            if ((insn & 0x01900000) == 0x01000000) {  // misc space
                if ((insn & 0x0FFFFFD0) == 0x012FFF10) return INLINE;  // BX / BLX reg
                if ((insn & 0x0FFF0FF0) == 0x016F0F10) return INLINE;  // CLZ
                return HELPER;                                          // MRS/MSR/QADD/SMLA/BKPT
            }
            if (insn & 0x10) {  // register-specified shift: inline unless it needs the shifter carry or PC
                uint32_t op = (insn >> 21) & 0xF;
                bool logicalS = (insn & (1 << 20)) && (op <= 1 || op == 8 || op == 9 || op >= 12);
                if (logicalS || ((insn >> 16) & 0xF) == 15 || ((insn >> 12) & 0xF) == 15 || (insn & 0xF) == 15 ||
                    ((insn >> 8) & 0xF) == 15)
                    return HELPER;
                return INLINE;
            }
            if ((insn & (1 << 20)) && ((insn >> 12) & 0xF) == 15) return HELPER;  // exception return
            return INLINE;
        case 1:
            if ((insn & 0x01900000) == 0x01000000) return HELPER;  // MSR imm / undefined
            if ((insn & (1 << 20)) && ((insn >> 12) & 0xF) == 15) return HELPER;
            return INLINE;
        case 2:
        case 3:
            if ((insn & 0x02000010) == 0x02000010) return HELPER;  // undefined/media
            if (!(insn & (1 << 24)) && (insn & (1 << 21))) return HELPER;  // LDRT/STRT
            if ((insn & (1 << 25)) && ((insn >> 4) & 0xFF) == 0x06) return HELPER;  // RRX offset
            return INLINE;
        case 4:
            if (insn & (1 << 22)) return HELPER;  // user-bank transfer / exception return
            if (((insn >> 16) & 0xF) == 15 || (insn & 0xFFFF) == 0) return HELPER;
            return INLINE;
        case 5: return INLINE;
        default: return HELPER;  // coprocessor, SWI
    }
}

Reg Translator::operand2(uint32_t insn, Reg tmp, Reg tmpRm, int &carry) {
    carry = -1;
    if (insn & (1 << 25)) {
        uint32_t rot = (insn >> 7) & 0x1E, v = ror32(insn & 0xFF, rot);
        if (rot) carry = (int)(v >> 31);
        if (v == 0) return ZR;
        e.mov32(tmp, v);
        return tmp;
    }
    uint32_t rmIdx = insn & 0xF, type = (insn >> 5) & 3, s = (insn >> 7) & 0x1F;
    Reg rm = rd((int)rmIdx, tmpRm);
    if (insn & 0x10) {
        // Register-specified shift by Rs[7:0] (classify() keeps Rm/Rs != PC and ops that need the
        // shifter carry in the interpreter). Flag-free, since guest NZCV lives in the host flags.
        Reg rs = rd((int)((insn >> 8) & 0xF), X13);
        if (type == 3) {  // ROR: amount mod 32, which RORV does by itself
            e.rorv(tmp, rm, rs);
            return tmp;
        }
        // amount' = Rs[4:0] | (Rs[7:5] != 0 ? 32 : 0): 0..63, and >= 32 exactly when Rs[7:0] >= 32.
        e.ubfx(X14, rs, 5, 3);
        e.sub(X14, ZR, X14);
        e.lsri(X14, X14, 31);
        e.ubfx(X15, rs, 0, 5);
        e.orr(X14, X15, X14, LSL, 5);
        // 64-bit shifts of the extended value give ARM's results for 32..63 (0, or all sign bits).
        if (type == 2) e.sxtw(tmpRm, rm);
        else e.movr(tmpRm, rm);  // zero-extends
        if (type == 0) e.lslv(tmp, tmpRm, X14, true);
        else if (type == 1) e.lsrv(tmp, tmpRm, X14, true);
        else e.asrv(tmp, tmpRm, X14, true);
        return tmp;  // low 32 bits hold the operand; users are 32-bit operations
    }
    switch (type) {
        case 0:
            if (s == 0) return rm;
            e.ubfx(X17, rm, 32 - s, 1);
            carry = 2;
            e.lsli(tmp, rm, s);
            return tmp;
        case 1:
            if (s == 0) { e.lsri(X17, rm, 31); carry = 2; return ZR; }
            e.ubfx(X17, rm, s - 1, 1);
            carry = 2;
            e.lsri(tmp, rm, s);
            return tmp;
        case 2:
            if (s == 0) s = 32;
            e.ubfx(X17, rm, s - 1 > 31 ? 31 : s - 1, 1);
            carry = 2;
            e.asri(tmp, rm, s > 31 ? 31 : s);
            return tmp;
        default:
            if (s == 0) {  // RRX
                e.ubfx(X17, rm, 0, 1);
                carry = 2;
                e.mrsNzcv(X16);
                e.ubfx(X16, X16, 29, 1);
                e.lsri(tmp, rm, 1);
                e.orr(tmp, tmp, X16, LSL, 31);
                return tmp;
            }
            e.ubfx(X17, rm, s - 1, 1);
            carry = 2;
            e.rori(tmp, rm, s);
            return tmp;
    }
}

// N,Z from res; C from the shifter (if any); V unchanged.
void Translator::logicFlags(Reg res, int carry) {
    e.mrsNzcv(X16);
    e.logic(3, false, ZR, res, res);  // tst res, res
    e.mrsNzcv(X15);
    e.lsri(X15, X15, 30);
    e.bfi(X16, X15, 30, 2);
    if (carry == 0 || carry == 1) {
        e.movz(X15, (uint32_t)carry);
        e.bfi(X16, X15, 29, 1);
    } else if (carry == 2) {
        e.bfi(X16, X17, 29, 1);
    }
    e.msrNzcv(X16);
}

bool Translator::emitDataProc(uint32_t insn, bool &ended) {
    uint32_t op = (insn >> 21) & 0xF, rnI = (insn >> 16) & 0xF, rdI = (insn >> 12) & 0xF;
    bool S = insn & (1 << 20);
    bool logical = op == 0 || op == 1 || op == 8 || op == 9 || op >= 12;
    int carry;
    Reg b = operand2(insn, X10, X11, carry);
    Reg a = (op == 13 || op == 15) ? ZR : rd((int)rnI, X9);
    bool writes = !(op >= 8 && op <= 11);
    Reg d = writes ? (rdI == 15 ? X12 : dst((int)rdI, X12)) : ZR;
    switch (op) {
        case 0x0: e.logic(0, false, d, a, b); break;
        case 0x1: e.logic(2, false, d, a, b); break;
        case 0x2: e.addsub(S ? 3 : 2, d, a, b); break;
        case 0x3: e.addsub(S ? 3 : 2, d, b, a); break;
        case 0x4: e.addsub(S ? 1 : 0, d, a, b); break;
        case 0x5: if (S) e.adcs(d, a, b); else e.adc(d, a, b); break;
        case 0x6: if (S) e.sbcs(d, a, b); else e.sbc(d, a, b); break;
        case 0x7: if (S) e.sbcs(d, b, a); else e.sbc(d, b, a); break;
        case 0x8: e.logic(0, false, X12, a, b); d = X12; break;
        case 0x9: e.logic(2, false, X12, a, b); d = X12; break;
        case 0xA: e.subs(ZR, a, b); break;
        case 0xB: e.adds(ZR, a, b); break;
        case 0xC: e.logic(1, false, d, a, b); break;
        case 0xD: e.movr(d, b); break;
        case 0xE: e.logic(0, true, d, a, b); break;
        case 0xF: e.logic(1, true, d, ZR, b); break;
    }
    if (S && logical) logicFlags(d, carry);
    if (!writes) return true;
    if (rdI == 15) {
        e.lsri(X12, d, 2);
        e.lsli(X12, X12, 2);
        exitReg(JIT_NEXT, X12, idx + 1);
        ended = true;
        return true;
    }
    commit((int)rdI, d);
    return true;
}

bool Translator::emitMul(uint32_t insn) {
    uint32_t rdI = (insn >> 16) & 0xF, rnI = (insn >> 12) & 0xF, rsI = (insn >> 8) & 0xF, rmI = insn & 0xF;
    if (rdI == 15 || rnI == 15 || rsI == 15 || rmI == 15) return false;
    Reg m = rd((int)rmI, X9), s = rd((int)rsI, X10);
    if (!(insn & (1 << 23))) {
        Reg acc = (insn & (1 << 21)) ? rd((int)rnI, X11) : ZR;
        Reg d = dst((int)rdI, X12);
        e.madd(d, m, s, acc);
        commit((int)rdI, d);
        return true;
    }
    bool sgn = insn & (1 << 22), accum = insn & (1 << 21);
    Reg acc = ZR;
    if (accum) {
        Reg lo = rd((int)rnI, X11), hi = rd((int)rdI, X13);
        e.movr(X14, lo);                    // zero-extends
        e.orrx(X14, X14, hi, LSL, 32);
        acc = X14;
    }
    if (sgn) e.smaddl(X12, m, s, acc);
    else e.umaddl(X12, m, s, acc);
    e.lsrix(X13, X12, 32);
    commit((int)rnI, X12);   // RdLo
    commit((int)rdI, X13);   // RdHi
    return true;
}

bool Translator::emitSingle(uint32_t insn, bool &ended) {
    bool P = insn & (1 << 24), U = insn & (1 << 23), B = insn & (1 << 22), W = insn & (1 << 21), L = insn & (1 << 20);
    uint32_t rnI = (insn >> 16) & 0xF, rdI = (insn >> 12) & 0xF;
    bool regOff = insn & (1 << 25);
    uint32_t imm = insn & 0xFFF;
    Reg base = rd((int)rnI, X9);
    Reg off = ZR;
    // operand2 treats bit 25 as "immediate"; for LDR/STR bit 25 means register, so clear it.
    if (regOff) { int c; off = operand2(insn & ~0x10u & ~(1u << 25), X10, X11, c); }
    // base +/- offset into `to`. base and off (cached, x9-x11 or zr) survive the access.
    auto apply = [=](Reg to) {
        if (regOff) { if (U) e.add(to, base, off); else e.sub(to, base, off); }
        else if (U) e.addi(to, base, imm);
        else e.subi(to, base, imm);
    };
    bool wb = (!P || W) && rnI != 15;
    Reg addr = base;
    if (P && (regOff || imm)) { apply(X1); addr = X1; }
    Reg val = L ? ZR : rd((int)rdI, X2, 12);
    // Loads go straight into a cached destination unless a writeback has to come first.
    Reg dest = (L && !wb && rdI != 15) ? dst((int)rdI, X0) : X0;
    std::function<void()> stop;
    if (!L) {
        uint32_t dirty0 = dirty, nextPc = pc + 4, exec = idx + 1;
        stop = [=]() {
            uint32_t d = dirty;
            dirty = dirty0;
            if (wb) { Reg nd = dst((int)rnI, X8); apply(nd); commit((int)rnI, nd); }
            exitConst(JIT_STOP, nextPc, exec);
            dirty = d;
        };
    }
    memAccess(addr, val, dest, !L, B ? 1 : 4, false, stop);
    if (wb) {
        Reg nd = dst((int)rnI, X8);
        apply(nd);
        commit((int)rnI, nd);
    }
    if (L) {
        if (rdI == 15) {
            exitReg(JIT_LOADPC, X0, idx + 1);
            ended = true;
            return true;
        }
        commit((int)rdI, dest);
    }
    return true;
}

bool Translator::emitHalf(uint32_t insn, bool &ended) {
    bool P = insn & (1 << 24), U = insn & (1 << 23), I = insn & (1 << 22), W = insn & (1 << 21), L = insn & (1 << 20);
    uint32_t rnI = (insn >> 16) & 0xF, rdI = (insn >> 12) & 0xF, sh = (insn >> 5) & 3;
    uint32_t imm = ((insn >> 4) & 0xF0) | (insn & 0xF);
    Reg base = rd((int)rnI, X9);
    Reg off = I ? ZR : rd((int)(insn & 0xF), X10);
    auto apply = [=](Reg to) {
        if (!I) { if (U) e.add(to, base, off); else e.sub(to, base, off); }
        else if (U) e.addi(to, base, imm);
        else e.subi(to, base, imm);
    };
    bool wb = (!P || W) && rnI != 15;
    Reg addr = base;
    if (P && (!I || imm)) { apply(X1); addr = X1; }
    Reg val = L ? ZR : rd((int)rdI, X2, 12);
    Reg dest = (L && !wb && rdI != 15) ? dst((int)rdI, X0) : X0;
    if (!L) {
        uint32_t dirty0 = dirty, nextPc = pc + 4, exec = idx + 1;
        std::function<void()> stop = [=]() {
            uint32_t d = dirty;
            dirty = dirty0;
            if (wb) { Reg nd = dst((int)rnI, X8); apply(nd); commit((int)rnI, nd); }
            exitConst(JIT_STOP, nextPc, exec);
            dirty = d;
        };
        memAccess(addr, val, dest, true, 2, false, stop);
    } else {
        memAccess(addr, val, dest, false, sh == 2 ? 1 : 2, sh != 1);
    }
    if (wb) {
        Reg nd = dst((int)rnI, X8);
        apply(nd);
        commit((int)rnI, nd);
    }
    if (L) {
        if (rdI == 15) {
            exitReg(JIT_LOADPC, X0, idx + 1);
            ended = true;
            return true;
        }
        commit((int)rdI, dest);
    }
    return true;
}

bool Translator::emitBlock(uint32_t insn, bool &ended) {
    bool P = insn & (1 << 24), U = insn & (1 << 23), W = insn & (1 << 21), L = insn & (1 << 20);
    uint32_t rnI = (insn >> 16) & 0xF, list = insn & 0xFFFF;
    uint32_t n = (uint32_t)__builtin_popcount(list);
    Reg base = rd((int)rnI, X9);
    // Start address into w1.
    if (U) {
        if (P) e.addi(X1, base, 4);
        else e.movr(X1, base);
    } else {
        e.subi(X1, base, 4 * n - (P ? 0 : 4));
    }
    // Fast path: whole transfer inside one directly-mapped page, word aligned.
    tlbProbe(X1, L ? TLBR : TLBW);
    uint32_t *slow1 = e.cbnz(X5);
    e.ubfx(X16, X3, 0, 2);
    uint32_t *slow2 = e.cbnz(X16);
    e.ubfx(X16, X3, 0, 12);
    e.addi(X16, X16, 4 * n - 1);
    e.lsri(X16, X16, 12);
    uint32_t *slow3 = e.cbnz(X16);
    e.addx(X7, X6, X3);
    uint32_t savedDirty = dirty;
    // Writeback happens before register loads so a loaded base wins (as on hardware).
    if (W) {
        Reg b2 = rd((int)rnI, X9);
        // STM stores the original base, so keep a copy in X9 before the update.
        if (b2 != X9) e.movr(X9, b2);
        Reg nd = dst((int)rnI, X8);
        if (U) e.addi(nd, X9, 4 * n);
        else e.subi(nd, X9, 4 * n);
        commit((int)rnI, nd);
    }
    uint32_t k = 0;
    bool loadsPc = false;
    for (int i = 0; i < 16; i++) {
        if (!(list & (1u << i))) continue;
        if (L) {
            if (i == 15) { e.ldrw(X12, X7, 4 * k); loadsPc = true; }
            else {
                Reg d = dst(i, X8);
                e.ldrw(d, X7, 4 * k);
                commit(i, d);
            }
        } else {
            Reg v = (i == (int)rnI && W) ? X9 : rd(i, X8, 12);
            e.strw(v, X7, 4 * k);
        }
        k++;
    }
    uint32_t *resume = e.pos();
    // Slow path: run the whole instruction in the interpreter.
    uint32_t thisInsn = insn, thisIdx = idx, thisPc = pc;
    deferred.push_back([=]() {
        uint32_t d = dirty, savedIdx = idx, savedPc = pc;
        dirty = savedDirty;
        idx = thisIdx;  // emitHelperInsn reads the current instruction's index and address
        pc = thisPc;
        e.here(slow1);
        e.here(slow2);
        e.here(slow3);
        emitHelperInsn(thisInsn);
        e.bTo(resume);
        dirty = d;
        idx = savedIdx;
        pc = savedPc;
    });
    dirty |= savedDirty;
    if (loadsPc) {
        exitReg(JIT_LOADPC, X12, idx + 1);
        ended = true;
        // The slow path resumes here too; the helper has already branched, so it never gets here.
    }
    return true;
}

bool Translator::emit(uint32_t insn, bool &ended) {
    uint32_t cond = insn >> 28;
    if (cond == 0xF) {
        if ((insn & 0x0E000000) == 0x0A000000) {  // BLX imm
            int32_t off = ((int32_t)(insn << 8) >> 6) | (int32_t)((insn >> 23) & 2);
            e.mov32(X9, pc + 4);
            commit(14, X9);
            exitConst(JIT_BX, (pc + 8 + (uint32_t)off) | 1, idx + 1);
            ended = true;
        }
        return true;  // PLD: nothing
    }
    switch ((insn >> 25) & 7) {
        case 0:
            if ((insn & 0xF0) == 0x90) return emitMul(insn);
            if ((insn & 0x90) == 0x90) return emitHalf(insn, ended);
            if ((insn & 0x0FFFFFD0) == 0x012FFF10) {  // BX / BLX reg
                Reg t = rd((int)(insn & 0xF), X12);
                if (t != X12) e.movr(X12, t);
                if (insn & 0x20) {
                    e.mov32(X9, pc + 4);
                    commit(14, X9);
                }
                exitReg(JIT_BX, X12, idx + 1);
                ended = true;
                return true;
            }
            if ((insn & 0x0FFF0FF0) == 0x016F0F10) {  // CLZ
                uint32_t rdI = (insn >> 12) & 0xF;
                if (rdI == 15) return false;
                Reg m = rd((int)(insn & 0xF), X9);
                Reg d = dst((int)rdI, X12);
                e.clz(d, m);
                commit((int)rdI, d);
                return true;
            }
            return emitDataProc(insn, ended);
        case 1: return emitDataProc(insn, ended);
        case 2:
        case 3: return emitSingle(insn, ended);
        case 4: return emitBlock(insn, ended);
        case 5: {
            int32_t off = (int32_t)(insn << 8) >> 6;
            uint32_t target = pc + 8 + (uint32_t)off;
            if (insn & (1 << 24)) {
                e.mov32(X9, pc + 4);
                commit(14, X9);
            }
            bool idle = false;
            if (!(insn & (1 << 24)) && off == -16 && idx >= 2) {
                uint32_t i0 = code[idx - 2], i1 = code[idx - 1];
                idle = (i0 & 0x0F700000) == 0x05100000 && (i1 & 0x0FF00000) == 0x03500000 &&
                       ((i1 >> 16) & 0xF) == ((i0 >> 12) & 0xF);
            }
            exitConst(idle ? JIT_IDLE : JIT_NEXT, target, idx + 1);
            ended = true;
            return true;
        }
        default: return false;
    }
}

uint32_t Translator::translate(uint32_t maxInsns) {
    // Find the block extent first so register allocation sees the whole block.
    uint32_t n = 0;
    while (n < maxInsns) {
        uint32_t a = pc0 + 4 * n;
        if (n > 0 && (a & 0xFFF) == 0) break;
        uint32_t insn = code[n];
        Kind k = classify(insn);
        if (k == END_BEFORE) break;
        n++;
        uint32_t cls = (insn >> 25) & 7;
        bool branchy = cls == 5 || (insn >> 28) == 0xF || (insn & 0x0FFFFFD0) == 0x012FFF10 ||
                       (cls == 7 && (insn & (1 << 24))) ||                                  // SWI
                       ((cls == 0 || cls == 1) && (insn & (1 << 20)) && ((insn >> 12) & 0xF) == 15) ||
                       ((cls == 2 || cls == 3) && (insn & (1 << 20)) && ((insn >> 12) & 0xF) == 15) ||
                       (cls == 4 && (insn & (1 << 20)) && (insn & 0x8000)) ||
                       ((cls == 0 || cls == 1) && ((insn >> 12) & 0xF) == 15 && k == INLINE && (insn & 0x90) != 0x90);
        if (branchy) break;
    }
    if (n == 0) return 0;
    allocate(n);
    blockN = n;
    // Budget check: a block only runs if all of it fits, so instruction counts stay exact.
    e.subi(X16, BUDGET, n);
    uint32_t *bail = e.tbnz(X16, 31);
    e.movr(BUDGET, X16);
    deferred.push_back([=]() {
        e.here(bail);
        e.mov32(X1, pc0);
        e.movz(X0, JIT_NEXT);
        e.bTo(j.exitPc);
    });
    for (int k = 0; k < kNumCache; k++)
        if (cachedGuest[k] >= 0) e.ldrw(kCacheRegs[k], STATE, cachedGuest[k] * 4);
    dirty = 0;
    bool ended = false;
    for (idx = 0; idx < n && !ended; idx++) {
        pc = pc0 + 4 * idx;
        uint32_t insn = code[idx];
        uint32_t cond = insn >> 28;
        uint32_t *skip = nullptr;
        uint32_t dirtyBefore = dirty;
        if (cond != 0xE && cond != 0xF) skip = e.bcond((Cond)(cond ^ 1));
        bool insnEnded = false;
        if (classify(insn) == HELPER) {
            emitHelperInsn(insn);
        } else if (!emit(insn, insnEnded)) {
            emitHelperInsn(insn);
        }
        if (skip) {
            // Either path may have been taken: anything dirty on either side stays dirty.
            dirty |= dirtyBefore;
            e.here(skip);
            if (insnEnded) exitConst(JIT_NEXT, pc + 4, idx + 1);
        } else if (insnEnded) {
            ended = true;
        }
        if (insnEnded && skip) ended = true;
    }
    if (!ended) {
        uint32_t next = pc0 + 4 * n;
        // Stopped before an instruction the dispatcher must run, or at the block size limit.
        uint32_t nextInsn = (next & 0xFFF) ? code[n] : 0;
        bool interp = (next & 0xFFF) && n < maxInsns && classify(nextInsn) == END_BEFORE;
        exitConst(interp ? JIT_INTERP : JIT_NEXT, next, n);
    }
    for (size_t i = 0; i < deferred.size(); i++) deferred[i]();
    if (!abortSites.empty()) {
        // Data abort in an inline access: w1 = faulting pc, w2 = instructions completed.
        // Every cached register holds a valid value here, so all of them are written back.
        for (uint32_t *s : abortSites) e.here(s);
        for (int k = 0; k < kNumCache; k++)
            if (cachedGuest[k] >= 0) e.strw(kCacheRegs[k], STATE, cachedGuest[k] * 4);
        e.mov32(X17, n);
        e.sub(X17, X17, X2);
        e.add(BUDGET, BUDGET, X17);
        e.movz(X0, JIT_ABORT);
        e.bTo(j.exitPc);
    }
    return n;
}

// ---------------------------------------------------------------------------------------------
// Jit

Jit::Jit(Cpu &c) : cpu(c) {
    void *p = mmap(nullptr, kBufBytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        LOGE("jit: cannot map executable memory");
        return;
    }
    buf = (uint32_t *)p;
    capWords = kBufBytes / 4;
    Emitter e(buf, capWords);
    // enter(state, code): save callee-saved registers, load context, jump into the block.
    enter = (uint32_t (*)(CpuState *, uint32_t *))e.pos();
    e.stpPre(FP, LR, SP, -96);
    e.stp(X19, X20, SP, 16);
    e.stp(X21, X22, SP, 32);
    e.stp(X23, X24, SP, 48);
    e.stp(X25, X26, SP, 64);
    e.stp(X27, X28, SP, 80);
    e.movx(STATE, X0);
    e.ldrx(TLBR, STATE, OFF_TLBR);
    e.ldrx(TLBW, STATE, OFF_TLBW);
    e.ldrx(GENPID, STATE, OFF_GENPID);
    e.ldrw(BUDGET, STATE, OFF_BUDGET);
    e.ldrw(X16, STATE, OFF_NZCV);
    e.msrNzcv(X16);
    e.br(X1);
    exitLabel = e.pos();
    e.strw(BUDGET, STATE, OFF_BUDGET);
    e.ldp(X27, X28, SP, 80);
    e.ldp(X25, X26, SP, 64);
    e.ldp(X23, X24, SP, 48);
    e.ldp(X21, X22, SP, 32);
    e.ldp(X19, X20, SP, 16);
    e.ldpPost(FP, LR, SP, 96);
    e.ret();
    ib = new IbEntry[kIb]();
    exitPc = e.pos();
    e.mrsNzcv(X16);
    e.strw(X16, STATE, OFF_NZCV);
    e.strw(X1, STATE, 60);
    e.strx(ZR, STATE, OFF_LASTEXIT);
    e.bTo(exitLabel);
    exitChain = e.pos();
    e.mrsNzcv(X16);
    e.strw(X16, STATE, OFF_NZCV);
    e.strw(X1, STATE, 60);
    e.strx(X17, STATE, OFF_LASTEXIT);
    e.bTo(exitLabel);
    // dispatch: jump to the block for (w1 = target pc) if it is translated for the current
    // mapping, else return to C++ with w0 = reason and r15 = target.
    dispatchStub = e.pos();
    {
        e.ubfx(X16, X1, 0, 2);
        uint32_t *s1 = e.cbnz(X16);
        // Same probe as Translator::tlbProbe.
        e.ubfx(X4, X1, 12, 12);
        e.lsri(X16, X1, 25);
        e.movr(X3, X1);
        uint32_t *high = e.cbnz(X16);
        e.lsrix(X17, GENPID, 32);
        e.orr(X3, X1, X17);
        e.here(high);
        e.addx(X4, TLBR, X4, LSL, 4);
        e.ldp(X5, X6, X4, 0);
        e.lsri(X16, X3, 12);
        e.orr(X16, X16, GENPID);
        e.eor(X5, X5, X16);
        uint32_t *s2 = e.cbnz(X5);
        e.addx(X6, X6, X3);                  // host address of the target
        e.lsrix(X7, X6, 2);
        e.ubfx(X7, X7, 0, 16);
        e.mov64(X8, (uint64_t)(uintptr_t)ib);
        e.addx(X8, X8, X7, LSL, 5);
        e.ldp(X9, X10, X8, 0);
        e.eorx(X9, X9, X6);
        uint32_t *s3 = e.cbnz(X9, true);
        e.eor(X10, X10, X1);
        uint32_t *s4 = e.cbnz(X10);
        e.ldrx(X11, X8, 16);
        e.br(X11);
        e.here(s1);
        e.here(s2);
        e.here(s3);
        e.here(s4);
        e.mrsNzcv(X16);
        e.strw(X16, STATE, OFF_NZCV);
        e.strw(X1, STATE, 60);
        e.strx(ZR, STATE, OFF_LASTEXIT);
        e.bTo(exitLabel);
    }
    auto thunk = [&](void *fn, int argReg, uint32_t imm) {
        uint32_t *start = e.pos();
        // Callers keep temporaries in x7-x15 across slow accesses (see Translator::memAccess).
        e.stpPre(LR, X16, SP, -96);
        e.stp(X7, X8, SP, 16);
        e.stp(X9, X10, SP, 32);
        e.stp(X11, X12, SP, 48);
        e.stp(X13, X14, SP, 64);
        e.stp(X15, X17, SP, 80);
        e.mrsNzcv(X16);
        e.strw(X16, STATE, OFF_NZCV);
        e.movx(X0, STATE);
        if (argReg >= 0) e.movz((Reg)argReg, imm);
        e.mov64(X16, (uint64_t)(uintptr_t)fn);
        e.blr(X16);
        e.ldrw(X16, STATE, OFF_NZCV);
        e.msrNzcv(X16);
        e.ldp(X15, X17, SP, 80);
        e.ldp(X13, X14, SP, 64);
        e.ldp(X11, X12, SP, 48);
        e.ldp(X9, X10, SP, 32);
        e.ldp(X7, X8, SP, 16);
        e.ldpPost(LR, X16, SP, 96);
        e.ret();
        return start;
    };
    for (uint32_t k : {1u, 2u, 4u, 17u, 18u}) readThunk[k] = thunk((void *)&jitRead, X2, k);
    for (uint32_t s : {1u, 2u, 4u}) writeThunk[s] = thunk((void *)&jitWrite, X3, s);
    interpThunk = thunk((void *)&jitInterp, -1, 0);
    codeStart = used = e.size();
    __builtin___clear_cache((char *)buf, (char *)(buf + used));
}

Jit::~Jit() {
    if (buf) munmap(buf, kBufBytes);
    delete[] ib;
}

void Jit::flush() {
    used = codeStart;
    memset(fast, 0, sizeof fast);
    memset(ib, 0, sizeof(IbEntry) * kIb);
    map.clear();
    pages.clear();
    codeLines.clear();
    pool.clear();
    freeList.clear();
    if (!cpu.codePage.empty()) std::fill(cpu.codePage.begin(), cpu.codePage.end(), 0);
    if (!cpu.scanned.empty()) std::fill(cpu.scanned.begin(), cpu.scanned.end(), 0);
    cpu.flushTlb();
    stats.flushes++;
}

bool Jit::codeWrite(uint32_t pa, uint32_t size) {
    auto it = codeLines.find(pa >> 12);
    if (it == codeLines.end()) return false;
    uint32_t first = (pa & 0xFFF) >> 6, last = ((pa & 0xFFF) + size - 1) >> 6;
    uint64_t hit = 0;
    for (uint32_t l = first; l <= last && l < 64; l++) hit |= 1ull << l;
    if (!(it->second & hit)) return false;  // data next to code: leave the blocks alone
    invalidatePage(pa >> 12);
    return true;
}

void Jit::invalidatePage(uint32_t ppn) {
    stats.invalidations++;
    codeLines.erase(ppn);
    if (!cpu.codePage.empty()) cpu.codePage[ppn] = 0;
    if (!cpu.scanned.empty()) cpu.scanned[ppn] = 0;
    auto it = pages.find(ppn);
    if (it == pages.end()) return;
    for (Block *b : it->second) {
        b->dead = true;
        ibRemove(b);
        map.erase(key(b->pc, b->host));
        Block *&f = fast[(b->pc >> 2) & (kFast - 1)];
        if (f == b) f = nullptr;
    }
    pages.erase(it);
}

// Patches a same-page exit to jump directly into the block at the new pc, if it is translated.
void Jit::chainTo(uint32_t *site) {
    uint32_t pc = cpu.r[15];
    Block *b = fast[(pc >> 2) & (kFast - 1)];
    if (!b || b->pc != pc || b->dead || b->ninsn == 0) return;  // linked on a later exit
    // The target must be the block for the current mapping of this VA (FCSE processes share VAs).
    uint32_t mva = cpu.vaMod(pc);
    bool user = cpu.mode == MODE_USR;
    const FastTlb &f = cpu.ftlb[user * 2][(mva >> 12) & (Cpu::TLB_SIZE - 1)];
    if (f.tag != cpu.tlbKey(mva) || f.addend + mva != b->host) return;
    int64_t d = b->code - site;
    *site = 0x14000000u | ((uint32_t)d & 0x03FFFFFF);
    __builtin___clear_cache((char *)site, (char *)(site + 1));
    stats.chains++;
}

Block *Jit::compile(uint32_t pc, uint8_t *host) {
    uint32_t mva = cpu.vaMod(pc);
    bool user = cpu.mode == MODE_USR;
    uint32_t fsr;
    Cpu::TlbEntry *te = cpu.tlbFillNoThrow(mva, false, user, fsr);
    if (!te) return nullptr;
    uint32_t pa = te->paPage + (mva & 0xFFF);
    if (used + 64 * 1024 > capWords) flush();
    cpu.checkCode(pa);  // host code fixes go in before the page is translated
    cpu.markCodePage(pa);

    pool.emplace_back();
    Block *b = &pool.back();
    b->pc = pc;
    b->host = (uint64_t)(uintptr_t)host;
    b->ppn = pa >> 12;
    b->dead = false;
    b->code = buf + used;
    Emitter e(buf + used, capWords - used);
    Translator t(*this, e, pc, (const uint32_t *)host);
    uint32_t maxInsns = kMaxInsns;
    uint32_t room = (0x1000 - (pc & 0xFFF)) / 4;
    if (room < maxInsns) maxInsns = room;
    b->ninsn = t.translate(maxInsns);
    if (e.ovf) {
        flush();
        return nullptr;
    }
    b->words = (uint32_t)e.size();
    used += e.size();
    __builtin___clear_cache((char *)b->code, (char *)(buf + used));
    map[key(pc, b->host)] = b;
    pages[b->ppn].push_back(b);
    if (b->ninsn) ibInsert(b);
    {
        uint32_t lo = (pc & 0xFFF) >> 6, hi = ((pc & 0xFFF) + 4 * (b->ninsn ? b->ninsn : 1) - 1) >> 6;
        uint64_t &m = codeLines[b->ppn];
        for (uint32_t l = lo; l <= hi && l < 64; l++) m |= 1ull << l;
    }
    stats.blocksCompiled++;
    stats.codeBytes = used * 4;
    return b;
}

bool Jit::run(uint64_t budget, uint32_t &reason, uint32_t &executed) {
    uint32_t pc = cpu.r[15];
    if (pc & 3) return false;
    uint32_t mva = cpu.vaMod(pc);
    bool user = cpu.mode == MODE_USR;
    const FastTlb &f = cpu.ftlb[user * 2][(mva >> 12) & (Cpu::TLB_SIZE - 1)];
    uint8_t *host;
    if (f.tag == cpu.tlbKey(mva)) {
        host = (uint8_t *)(uintptr_t)(f.addend + mva);
    } else {
        uint32_t fsr;
        Cpu::TlbEntry *te = cpu.tlbFillNoThrow(mva, false, user, fsr);
        if (!te || !te->host) return false;
        host = te->host + (mva & 0xFFF);
    }
    uint64_t hk = (uint64_t)(uintptr_t)host;
    Block *&slot = fast[(pc >> 2) & (kFast - 1)];
    Block *b = slot;
    if (!b || b->pc != pc || b->host != hk) {
        auto it = map.find(key(pc, hk));
        if (it != map.end() && it->second->pc == pc && it->second->host == hk) b = it->second;
        else b = compile(pc, host);
        if (!b) return false;
        fast[(pc >> 2) & (kFast - 1)] = b;
    }
    if (b->ninsn == 0 || b->ninsn > budget) {
        if (b->ninsn == 0) {
            stats.interpExits++;
            if (profileInterp) interpHist[*(const uint32_t *)host & 0x0FF000F0]++;
        }
        return false;
    }
    ibInsert(b);
    cpu.syncJitState();
    cpu.nzcv = cpu.packFlags();
    cpu.jitStatus = 0;
    cpu.jitExecuted = 0;
    uint32_t budget0 = budget > 0x7FFFFFFF ? 0x7FFFFFFF : (uint32_t)budget;
    cpu.jitBudget = budget0;
    cpu.jitLastExit = 0;
    reason = enter(&cpu, b->code);
    cpu.unpackFlags(cpu.nzcv);
    executed = budget0 - cpu.jitBudget;
    stats.blocksRun++;
    if (cpu.jitLastExit && reason == JIT_NEXT) chainTo((uint32_t *)(uintptr_t)cpu.jitLastExit);
    return true;
}

// ---------------------------------------------------------------------------------------------

Jit *jitCreate(Cpu &cpu) {
#if !defined(__aarch64__)
    (void)cpu;
    return nullptr;  // the translator emits AArch64 code only
#endif
    Jit *j = new Jit(cpu);
    if (!j->ok()) {
        delete j;
        return nullptr;
    }
    return j;
}

void jitDestroy(Jit *jit) { delete jit; }
void jitSetMaxBlock(int n) { kMaxInsns = n; }
void jitFlush(Jit *jit) { if (jit) jit->flush(); }
void jitInvalidatePage(Jit *jit, uint32_t ppn) { if (jit) jit->invalidatePage(ppn); }
bool jitCodeWrite(Jit *jit, uint32_t pa, uint32_t size) { return jit && jit->codeWrite(pa, size); }
bool jitRun(Jit *jit, uint64_t budget, uint32_t &reason, uint32_t &executed) { return jit->run(budget, reason, executed); }
JitStats jitStats(Jit *jit) { return jit ? jit->stats : JitStats{}; }
void jitDumpMap(Jit *jit, const char *path) { if (jit) jit->dumpMap(path); }
void jitProfileInterp(Jit *jit, bool on) { if (jit) jit->profileInterp = on; }
void jitProfileHelpers(bool on) {
    static std::unordered_map<uint32_t, uint64_t> helperHist;
    g_helperHist = on ? &helperHist : nullptr;
}
void jitDumpInterp(Jit *jit) {
    if (!jit) return;
    auto dump = [](const char *what, const std::unordered_map<uint32_t, uint64_t> &h) {
        std::vector<std::pair<uint64_t, uint32_t>> v;
        for (auto &kv : h) v.push_back({kv.second, kv.first});
        std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
        for (size_t i = 0; i < v.size() && i < 15; i++)
            LOGI("%s %10llu  %08x  %s", what, (unsigned long long)v[i].first, v[i].second, disasmArm(0, v[i].second | 0xE0000000).c_str());
    };
    dump("interp", jit->interpHist);
    if (g_helperHist) dump("helper", *g_helperHist);
}

}  // namespace wm
