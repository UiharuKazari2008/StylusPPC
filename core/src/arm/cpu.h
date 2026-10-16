// ARMv5TE (ARM920T/ARM926-class) system CPU: ARM + Thumb, banked modes, CP15 MMU.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>

namespace wm {

class StateIO;

class Bus;

enum Mode : uint32_t {
    MODE_USR = 0x10, MODE_FIQ = 0x11, MODE_IRQ = 0x12, MODE_SVC = 0x13,
    MODE_ABT = 0x17, MODE_UND = 0x1B, MODE_SYS = 0x1F,
};

enum CpuCore { CORE_ARMV4 = 0, CORE_ARMV5 = 1 };

// Thrown from memory accessors when the MMU raises an abort.
struct MemAbort {
    bool prefetch;
};

class Jit;

// Direct-access TLB entry: host address = addend + modified VA. Only pages backed by host memory
// with the needed permission are entered; everything else takes the slow path.
struct FastTlb {
    uint32_t tag;      // (mva >> 12) | gen << 20, 0 = invalid
    uint32_t pad;
    uint64_t addend;
};

// State with a fixed layout that generated code addresses directly (offsets in jit.cpp).
struct CpuState {
    uint32_t r[16];            // 0
    uint32_t nzcv;             // 64: flags in host NZCV format while JIT code runs
    uint32_t jitStatus;        // 68: bit0 abort, bit1 code page written, bit2 stop after insn
    uint64_t tlbRead;          // 72: FastTlb* for the current privilege
    uint64_t tlbWrite;         // 80
    uint64_t genPid;           // 88: (pid << 32) | (tlbGen << 20)
    uint32_t jitExecuted;      // 96: guest instructions completed since entering generated code
    uint32_t jitBudget;        // 100: instructions generated code may still run
    uint64_t jitLastExit;      // 104: patchable branch of the exit taken (chainable exits only)
};

class Cpu : public CpuState {
public:
    void serialize(StateIO &s);
    explicit Cpu(Bus &bus);
    void reset();
    // Runs up to `count` instructions; returns the number executed.
    uint64_t run(uint64_t count);

    void setIrq(bool level) { irqLine = level; }
    void setFiq(bool level) { fiqLine = level; }
    bool irqPending() const { return irqLine; }

    uint32_t reg(int i) const { return r[i]; }
    uint32_t pc() const { return r[15]; }
    uint32_t getCpsr() const;
    void setPc(uint32_t v) { r[15] = v; }
    void setReg(int i, uint32_t v) { r[i] = v; }
    void flushTlb();

    // Debug/trace hooks.
    void (*traceHook)(void *ctx, Cpu &cpu, uint32_t pc, uint32_t insn, bool thumb) = nullptr;
    void *traceCtx = nullptr;
    uint64_t insnCount = 0;
    bool halted = false;   // set by idle (wait-for-interrupt)
    uint64_t idleHits = 0;
    uint64_t prefetchAborts = 0, dataAborts = 0;  // diagnostics (CLI)
    CpuCore core = CORE_ARMV5;
    uint32_t cpuId = 0x69059201;  // what DE reports: Intel/ARMv5TE/ARM920

    // Virtual memory access, used by devices/debugger (no exceptions thrown: returns false on fault).
    bool debugRead32(uint32_t va, uint32_t &out);
    bool translate(uint32_t va, bool write, bool user, uint32_t &pa, uint32_t &fsr);

    bool traceUndef = true;

    // CP15 state
    uint32_t cp15_ctrl = 0;  // MMU off
    uint32_t cp15_ttb = 0, cp15_dacr = 0, cp15_fsr = 0, cp15_far = 0, cp15_ifsr = 0;
    uint32_t cp15_pid = 0;  // FCSE PID
    uint32_t cp15_tid = 0;
    bool mmuDisabled = false;  // set when the guest turns the MMU off (it restarts itself)

    // JIT
    Jit *jit = nullptr;
    bool jitEnabled = false;
    bool singleBlock = false;   // debug: return after one JIT block
    std::vector<uint8_t> codePage;   // per 4K physical page: 1 if translated code lives there
    void markCodePage(uint32_t pa);
    // Host code fixes (board/codepatch.h): codeScan(ctx, pagePa) runs before code in a physical page
    // first runs (JIT compile or interpreter fetch) and again once the page has been written since.
    // A scanned page takes the slow write path like a code page, so that write is seen.
    void (*codeScan)(void *ctx, uint32_t pa) = nullptr;
    void *codeScanCtx = nullptr;
    std::vector<uint8_t> scanned;    // per 4K physical page: 1 if codeScan has seen it as it is now
    void checkCode(uint32_t pa) { if (codeScan && (scanned.empty() || !scanned[pa >> 12])) scanCode(pa); }
    void scanCode(uint32_t pa);
    uint32_t packFlags() const { return (fN << 31) | (fZ << 30) | (fC << 29) | (fV << 28); }
    void unpackFlags(uint32_t v) { fN = v >> 31; fZ = (v >> 30) & 1; fC = (v >> 29) & 1; fV = (v >> 28) & 1; }

private:
    friend class Jit;
    friend uint32_t jitRead(Cpu *, uint32_t, uint32_t);
    friend void jitWrite(Cpu *, uint32_t, uint32_t, uint32_t);
    friend uint32_t jitInterp(Cpu *, uint32_t, uint32_t);
    Bus &bus;
    // CPSR split for speed.
    uint32_t fN, fZ, fC, fV;   // each 0 or 1
    uint32_t fQ;
    uint32_t mode;             // M[4:0]
    uint32_t fI, fF, fT;
    uint32_t spsr;
    // Banked storage.
    uint32_t bankUsr[7];       // r8-r14 for usr/sys (r8-r12 shared with all but FIQ)
    uint32_t bankFiq[7];       // r8-r14 fiq
    uint32_t bankSP[6], bankLR[6], bankSPSR[6];  // indexed by bankIndex(): 0 usr,1 fiq,2 irq,3 svc,4 abt,5 und
    bool irqLine = false, fiqLine = false;

    // Software TLB: two sets (privileged, user).
    struct TlbEntry {
        uint32_t tag;        // tlbKey(mva); 0 = invalid
        uint32_t paPage;     // physical page base
        uint8_t *host;       // host pointer to page base for direct access (null => io)
        uint8_t perm;        // bit0 read, bit1 write
        uint8_t hostWrite;   // direct write allowed
    };
    static const int TLB_SIZE = 4096;
    TlbEntry tlb[2][TLB_SIZE];
    FastTlb ftlb[4][TLB_SIZE];   // [user * 2 + write]
    uint32_t tlbGen = 1;
    void syncJitState() {
        bool u = mode == MODE_USR;
        tlbRead = (uint64_t)(uintptr_t)ftlb[u * 2];
        tlbWrite = (uint64_t)(uintptr_t)ftlb[u * 2 + 1];
        genPid = ((uint64_t)cp15_pid << 32) | (tlbGen << 20);
    }
    // Slow-path accessors shared by the interpreter and JIT; return false on an MMU fault
    // (FSR/FAR already set).
    bool slowRead(uint32_t va, int size, bool user, uint32_t &out);
    bool slowWrite(uint32_t va, int size, bool user, uint32_t v, bool &io);
    uint32_t tlbKey(uint32_t mva) const { return (mva >> 12) | (tlbGen << 20); }

    // Instruction fetch cache.
    uint32_t fetchTag = ~0u;
    uint8_t *fetchHost = nullptr;

    struct Walk {
        uint32_t pa, fsr;
        uint8_t ap, dom;
        bool page, uniform, manager;
    };
    bool walk(uint32_t va, Walk &w);
    bool access(const Walk &w, bool write, bool user, uint32_t &fsr) const;
    bool branched = false;
    uint32_t prefetchVa = 0, prefetchPa = 0, prefetchLeft = 0;
    uint32_t fetchPa = 0;
    uint8_t *fetchPage(uint32_t va);
    bool canFetch(uint32_t va);  // false (IFSR set) if fetching at va takes a prefetch abort
    void setPC(uint32_t v);
    uint32_t getUserReg(int i) const;
    void setUserReg(int i, uint32_t v);

    void setCpsr(uint32_t v, bool flagsOnly = false, uint32_t mask = 0xFFFFFFFF);
    void switchMode(uint32_t newMode);
    static int bankIndex(uint32_t m);
    void exception(uint32_t vector, uint32_t newMode, uint32_t lrValue, bool disableFiq);
    void checkInterrupts();

    // Memory
    TlbEntry &tlbFill(uint32_t va, bool write, bool user, bool fetch);
    TlbEntry *tlbFillNoThrow(uint32_t mva, bool write, bool user, uint32_t &fsr);
    // FCSE: addresses below 32 MB are relocated by the process ID, but only through the MMU.
    // With the MMU off (e.g. the OAL's reboot path jumping to the boot code) addresses are physical.
    uint32_t vaMod(uint32_t va) const {
        return (va < 0x02000000 && (cp15_ctrl & 1)) ? (va | cp15_pid) : va;
    }
    uint32_t read32(uint32_t va, bool user = false);
    uint16_t read16(uint32_t va, bool user = false);
    uint8_t read8(uint32_t va, bool user = false);
    void write32(uint32_t va, uint32_t v, bool user = false);
    void write16(uint32_t va, uint16_t v, bool user = false);
    void write8(uint32_t va, uint8_t v, bool user = false);
    uint32_t fetch32(uint32_t va);
    uint16_t fetch16(uint32_t va);
    bool isUser() const { return mode == MODE_USR; }

    void execArm(uint32_t insn);
    void execThumb(uint16_t insn);
    bool condPass(uint32_t cond) const;
    uint32_t shifterOperand(uint32_t insn, uint32_t &carry);
    void dataProcessing(uint32_t insn);
    void singleTransfer(uint32_t insn);
    void halfTransfer(uint32_t insn);
    void blockTransfer(uint32_t insn);
    void multiply(uint32_t insn);
    void coprocessor(uint32_t insn);
    void dspMisc(uint32_t insn);
    void undefined();
    void checkIdleLoop(uint32_t target);
    void handleJitExit(uint32_t reason);
    void writePcFromLoad(uint32_t v);  // v5: bit0 selects Thumb
    void cp15Write(uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2, uint32_t v);
    uint32_t cp15Read(uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2);
};

}  // namespace wm
