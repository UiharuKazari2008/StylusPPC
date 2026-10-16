#include "cpu.h"
#include "../board/bus.h"
#include "../util/log.h"
#include "jit.h"

namespace wm {

static inline uint32_t ror32(uint32_t v, uint32_t s) {
    s &= 31;
    return s ? (v >> s) | (v << (32 - s)) : v;
}
static inline int32_t sat32(int64_t v, uint32_t &q) {
    if (v > INT32_MAX) { q = 1; return INT32_MAX; }
    if (v < INT32_MIN) { q = 1; return INT32_MIN; }
    return (int32_t)v;
}

Cpu::Cpu(Bus &b) : bus(b) {
    memset(tlb, 0, sizeof tlb);
    memset(ftlb, 0, sizeof ftlb);
    reset();
}

void Cpu::reset() {
    memset(r, 0, sizeof r);
    memset(bankUsr, 0, sizeof bankUsr);
    memset(bankFiq, 0, sizeof bankFiq);
    memset(bankSP, 0, sizeof bankSP);
    memset(bankLR, 0, sizeof bankLR);
    memset(bankSPSR, 0, sizeof bankSPSR);
    fN = fZ = fC = fV = fQ = 0;
    fI = fF = 1;
    fT = 0;
    mode = MODE_SVC;
    spsr = 0;
    cp15_ctrl = 0;
    cp15_ttb = cp15_dacr = cp15_fsr = cp15_far = cp15_ifsr = cp15_pid = cp15_tid = 0;
    halted = false;
    bankSP[3] = r[13] = 0x30010000;  // DE starts with SP_svc in RAM
    flushTlb();
}

void Cpu::flushTlb() {
    // Entries carry a 12-bit generation in their tag, so a flush is just a generation bump.
    if (++tlbGen >= 4096) {
        for (int s = 0; s < 2; s++)
            for (int i = 0; i < TLB_SIZE; i++) tlb[s][i].tag = 0;
        for (int s = 0; s < 4; s++)
            for (int i = 0; i < TLB_SIZE; i++) ftlb[s][i].tag = 0;
        tlbGen = 1;
    }
    fetchTag = ~0u;
}

uint32_t Cpu::getCpsr() const {
    return (fN << 31) | (fZ << 30) | (fC << 29) | (fV << 28) | (fQ << 27) | (fI << 7) | (fF << 6) |
           (fT << 5) | mode;
}

int Cpu::bankIndex(uint32_t m) {
    switch (m) {
        case MODE_FIQ: return 1;
        case MODE_IRQ: return 2;
        case MODE_SVC: return 3;
        case MODE_ABT: return 4;
        case MODE_UND: return 5;
        default: return 0;
    }
}

void Cpu::switchMode(uint32_t nm) {
    nm |= 0x10;
    int oi = bankIndex(mode), ni = bankIndex(nm);
    if (oi != ni) {
        bankSP[oi] = r[13];
        bankLR[oi] = r[14];
        bankSPSR[oi] = spsr;
        if (oi == 1 || ni == 1) {
            uint32_t *save = (oi == 1) ? bankFiq : bankUsr;
            uint32_t *load = (ni == 1) ? bankFiq : bankUsr;
            for (int i = 0; i < 5; i++) save[i] = r[8 + i];
            for (int i = 0; i < 5; i++) r[8 + i] = load[i];
        }
        r[13] = bankSP[ni];
        r[14] = bankLR[ni];
        spsr = bankSPSR[ni];
    }
    if ((mode == MODE_USR) != (nm == MODE_USR)) fetchTag = ~0u;
    mode = nm;
}

void Cpu::setCpsr(uint32_t v, bool flagsOnly, uint32_t mask) {
    if (mask & 0xFF000000) {
        fN = v >> 31; fZ = (v >> 30) & 1; fC = (v >> 29) & 1; fV = (v >> 28) & 1; fQ = (v >> 27) & 1;
    }
    if (flagsOnly || mode == MODE_USR) return;
    if (mask & 0xFF) {
        fI = (v >> 7) & 1;
        fF = (v >> 6) & 1;
        fT = (v >> 5) & 1;
        switchMode(v & 0x1F);
    }
}

void Cpu::exception(uint32_t vector, uint32_t newMode, uint32_t lrValue, bool disableFiq) {
    uint32_t old = getCpsr();
    switchMode(newMode);
    spsr = old;
    r[14] = lrValue;
    fT = 0;
    fI = 1;
    if (disableFiq) fF = 1;
    r[15] = ((cp15_ctrl & (1 << 13)) ? 0xFFFF0000 : 0) + vector;
    branched = true;
}

void Cpu::checkInterrupts() {
    // r[15] is the address of the next instruction to execute.
    if (fiqLine && !fF) exception(0x1C, MODE_FIQ, r[15] + 4, true);
    else if (irqLine && !fI) exception(0x18, MODE_IRQ, r[15] + 4, false);
}

// ---------------------------------------------------------------------------------------------
// MMU

// Page-table descriptors live in RAM: read them through the host pointer, not the device path.
static inline uint32_t ptRead(Bus &bus, uint32_t pa) {
    bool wr;
    if (const uint8_t *h = bus.hostPage(pa, wr)) { uint32_t v; memcpy(&v, h, 4); return v; }
    return bus.read(pa, 4);
}

bool Cpu::walk(uint32_t va, Walk &w) {
    w.uniform = true;
    if (!(cp15_ctrl & 1)) { w.pa = va; w.ap = 3; w.dom = 0; w.page = false; w.manager = true; return true; }
    w.manager = false;
    uint32_t l1 = ptRead(bus, (cp15_ttb & 0xFFFFC000) | ((va >> 20) << 2));
    w.dom = (l1 >> 5) & 0xF;
    w.page = false;
    switch (l1 & 3) {
        case 0: w.fsr = 0x5; return false;
        case 2:
            w.pa = (l1 & 0xFFF00000) | (va & 0xFFFFF);
            w.ap = (l1 >> 10) & 3;
            break;
        default: {
            uint32_t l2addr;
            if ((l1 & 3) == 1) l2addr = (l1 & 0xFFFFFC00) | ((va >> 10) & 0x3FC);
            else l2addr = (l1 & 0xFFFFF000) | ((va >> 8) & 0xFFC);
            uint32_t l2 = ptRead(bus, l2addr);
            w.page = true;
            uint32_t aps = (l2 >> 4) & 0xFF;
            switch (l2 & 3) {
                case 0: w.fsr = 0x7 | (w.dom << 4); return false;
                case 1:
                    w.pa = (l2 & 0xFFFF0000) | (va & 0xFFFF);
                    w.ap = (aps >> ((va >> 13) & 6)) & 3;
                    w.uniform = aps == (aps & 3) * 0x55;
                    break;
                case 2:
                    w.pa = (l2 & 0xFFFFF000) | (va & 0xFFF);
                    w.ap = (aps >> ((va >> 9) & 6)) & 3;
                    w.uniform = aps == (aps & 3) * 0x55;
                    break;
                case 3:
                    if ((l1 & 3) == 3) { w.pa = (l2 & 0xFFFFFC00) | (va & 0x3FF); w.uniform = false; }
                    else w.pa = (l2 & 0xFFFFF000) | (va & 0xFFF);
                    w.ap = aps & 3;
                    break;
            }
            break;
        }
    }
    unsigned d = (cp15_dacr >> (w.dom * 2)) & 3;
    if (d == 0 || d == 2) { w.fsr = (w.page ? 0xB : 0x9) | (w.dom << 4); return false; }
    w.manager = d == 3;
    return true;
}

bool Cpu::access(const Walk &w, bool write, bool user, uint32_t &fsr) const {
    if (w.manager) return true;
    bool ok;
    switch (w.ap) {
        // Device Emulator semantics (S=0, R=1): privileged reads always succeed, privileged writes
        // need AP!=0; user mode has no access at AP=1 and may only write at AP=3.
        case 0: ok = !write; break;
        case 1: ok = !user; break;
        case 2: ok = !user || !write; break;
        default: ok = true; break;
    }
    if (!ok) fsr = (w.page ? 0xF : 0xD) | (w.dom << 4);
    return ok;
}

bool Cpu::translate(uint32_t va, bool write, bool user, uint32_t &pa, uint32_t &fsr) {
    Walk w;
    if (!walk(va, w)) { fsr = w.fsr; return false; }
    if (!access(w, write, user, fsr)) return false;
    pa = w.pa;
    return true;
}

Cpu::TlbEntry *Cpu::tlbFillNoThrow(uint32_t mva, bool write, bool user, uint32_t &fsr) {
    static TlbEntry scratch;
    Walk w;
    fsr = 0;
    bool ok = walk(mva, w);
    if (!ok) fsr = w.fsr;
    else ok = access(w, write, user, fsr);
    if (!ok) return nullptr;
    uint32_t f;
    uint8_t perm = (access(w, false, user, f) ? 1 : 0) | (access(w, true, user, f) ? 2 : 0);
    uint32_t idx = (mva >> 12) & (TLB_SIZE - 1);
    TlbEntry &out = w.uniform ? tlb[user][idx] : scratch;
    bool hw;
    if (w.uniform) {
        out.tag = tlbKey(mva);
        out.paPage = w.pa & ~0xFFFu;
        out.host = bus.hostPage(out.paPage, hw);
    } else {
        // 1K granularity: bias the base so (mva & 0xFFF) still indexes correctly.
        out.tag = 0;
        out.paPage = (w.pa & ~0x3FFu) - (mva & 0xC00);
        out.host = bus.hostPage(w.pa & ~0x3FFu, hw);
        if (out.host) out.host -= (mva & 0xC00);
    }
    out.hostWrite = hw;
    out.perm = perm;
    if (w.uniform && out.host) {
        uint64_t addend = (uint64_t)(uintptr_t)out.host - (mva & ~0xFFFu);
        if (perm & 1) { FastTlb &e = ftlb[user * 2][idx]; e.tag = out.tag; e.addend = addend; }
        bool code = (!codePage.empty() && codePage[out.paPage >> 12]) ||
                    (!scanned.empty() && scanned[out.paPage >> 12]);
        if ((perm & 2) && hw && !code) { FastTlb &e = ftlb[user * 2 + 1][idx]; e.tag = out.tag; e.addend = addend; }
    }
    return &out;
}

Cpu::TlbEntry &Cpu::tlbFill(uint32_t mva, bool write, bool user, bool fetch) {
    uint32_t fsr;
    TlbEntry *e = tlbFillNoThrow(mva, write, user, fsr);
    if (!e) {
        if (fetch) { cp15_ifsr = fsr; throw MemAbort{true}; }
        cp15_fsr = fsr | (write ? 0x800 : 0);
        cp15_far = mva;
        throw MemAbort{false};
    }
    return *e;
}

void Cpu::scanCode(uint32_t pa) {
    if (scanned.empty()) scanned.assign(1u << 20, 0);
    scanned[pa >> 12] = 1;
    if (codePage.empty() || !codePage[pa >> 12]) flushTlb();  // drop direct-write entries for the page
    codeScan(codeScanCtx, pa & ~0xFFFu);
}

void Cpu::markCodePage(uint32_t pa) {
    if (codePage.empty()) codePage.assign(1u << 20, 0);
    if (codePage[pa >> 12]) return;
    codePage[pa >> 12] = 1;
    flushTlb();  // drop direct-write entries for the page
}

// The slow paths serve I/O, code-page writes, misaligned accesses and fast-TLB misses. The first
// three usually hit the soft TLB, so look there before walking the page tables.
bool Cpu::slowRead(uint32_t mva, int size, bool user, uint32_t &out) {
    TlbEntry *e = &tlb[user][(mva >> 12) & (TLB_SIZE - 1)];
    if (e->tag != tlbKey(mva) || !(e->perm & 1)) {
        uint32_t fsr;
        e = tlbFillNoThrow(mva, false, user, fsr);
        if (!e) { cp15_fsr = fsr; cp15_far = mva; return false; }
    }
    uint32_t off = mva & 0xFFF;
    if (e->host) {
        if (size == 4) memcpy(&out, e->host + off, 4);
        else if (size == 2) { uint16_t v; memcpy(&v, e->host + off, 2); out = v; }
        else out = e->host[off];
    } else {
        out = bus.read(e->paPage + off, size);
    }
    return true;
}

bool Cpu::slowWrite(uint32_t mva, int size, bool user, uint32_t v, bool &io) {
    uint32_t idx = (mva >> 12) & (TLB_SIZE - 1);
    TlbEntry *e = &tlb[user][idx];
    bool hit = e->tag == tlbKey(mva) && (e->perm & 2);
    if (!hit) {
        uint32_t fsr;
        e = tlbFillNoThrow(mva, true, user, fsr);
        if (!e) { cp15_fsr = fsr | 0x800; cp15_far = mva; return false; }
    }
    uint32_t off = mva & 0xFFF, pa = e->paPage + off;
    if (!codePage.empty() && codePage[pa >> 12] && jit && jitCodeWrite(jit, pa, (uint32_t)size))
        jitStatus |= 2;
    if (!scanned.empty() && scanned[pa >> 12]) scanned[pa >> 12] = 0;  // scan again before it runs
    if (hit && e->host && e->hostWrite && (codePage.empty() || !codePage[pa >> 12])) {
        // As tlbFillNoThrow would: direct writes again once the page no longer holds code.
        FastTlb &f = ftlb[user * 2 + 1][idx];
        f.tag = e->tag;
        f.addend = (uint64_t)(uintptr_t)e->host - (mva & ~0xFFFu);
    }
    if (e->host && e->hostWrite) {
        if (size == 4) memcpy(e->host + off, &v, 4);
        else if (size == 2) { uint16_t h = (uint16_t)v; memcpy(e->host + off, &h, 2); }
        else e->host[off] = (uint8_t)v;
        io = false;
    } else {
        bus.write(pa, v, size);
        io = true;
    }
    return true;
}

#define FAST_TLB(va, wr, usr)                                                               \
    uint32_t mva = vaMod(va);                                                               \
    bool u_ = (usr) || mode == MODE_USR;                                                    \
    const FastTlb &f_ = ftlb[u_ * 2 + (wr)][(mva >> 12) & (TLB_SIZE - 1)];                  \
    uint8_t *h_ = f_.tag == tlbKey(mva) ? (uint8_t *)(uintptr_t)(f_.addend + mva) : nullptr;

uint32_t Cpu::read32(uint32_t va, bool user) {
    if ((va & 3) && (cp15_ctrl & 2)) { cp15_fsr = 1; cp15_far = va; throw MemAbort{false}; }
    va &= ~3u;
    FAST_TLB(va, 0, user);
    uint32_t v;
    if (h_) { memcpy(&v, h_, 4); return v; }
    if (!slowRead(mva, 4, u_, v)) throw MemAbort{false};
    return v;
}

uint16_t Cpu::read16(uint32_t va, bool user) {
    if ((va & 1) && (cp15_ctrl & 2)) { cp15_fsr = 1; cp15_far = va; throw MemAbort{false}; }
    va &= ~1u;
    FAST_TLB(va, 0, user);
    if (h_) { uint16_t v; memcpy(&v, h_, 2); return v; }
    uint32_t v;
    if (!slowRead(mva, 2, u_, v)) throw MemAbort{false};
    return (uint16_t)v;
}

uint8_t Cpu::read8(uint32_t va, bool user) {
    FAST_TLB(va, 0, user);
    if (h_) return *h_;
    uint32_t v;
    if (!slowRead(mva, 1, u_, v)) throw MemAbort{false};
    return (uint8_t)v;
}

void Cpu::write32(uint32_t va, uint32_t v, bool user) {
    if ((va & 3) && (cp15_ctrl & 2)) { cp15_fsr = 1 | 0x800; cp15_far = va; throw MemAbort{false}; }
    va &= ~3u;
    FAST_TLB(va, 1, user);
    if (h_) { memcpy(h_, &v, 4); return; }
    bool io;
    if (!slowWrite(mva, 4, u_, v, io)) throw MemAbort{false};
    if (io) jitStatus |= 4;
}

void Cpu::write16(uint32_t va, uint16_t v, bool user) {
    if ((va & 1) && (cp15_ctrl & 2)) { cp15_fsr = 1 | 0x800; cp15_far = va; throw MemAbort{false}; }
    va &= ~1u;
    FAST_TLB(va, 1, user);
    if (h_) { memcpy(h_, &v, 2); return; }
    bool io;
    if (!slowWrite(mva, 2, u_, v, io)) throw MemAbort{false};
    if (io) jitStatus |= 4;
}

void Cpu::write8(uint32_t va, uint8_t v, bool user) {
    FAST_TLB(va, 1, user);
    if (h_) { *h_ = v; return; }
    bool io;
    if (!slowWrite(mva, 1, u_, v, io)) throw MemAbort{false};
    if (io) jitStatus |= 4;
}

uint8_t *Cpu::fetchPage(uint32_t va) {
    uint32_t mva = vaMod(va);
    bool u = mode == MODE_USR;
    TlbEntry *e = &tlb[u][(mva >> 12) & (TLB_SIZE - 1)];
    if (e->tag != tlbKey(mva) || !(e->perm & 1)) e = &tlbFill(mva, false, u, true);
    if (codeScan && e->host && (scanned.empty() || !scanned[(e->paPage + (mva & 0xFFF)) >> 12])) {
        scanCode(e->paPage + (mva & 0xFFF));
        e = &tlbFill(mva, false, u, true);  // the scan flushed the TLB
    }
    fetchPa = e->paPage;
    if (e->tag == tlbKey(mva) && e->host) {
        fetchTag = (va >> 12) | (u ? 0x80000000u : 0) ;
        fetchHost = e->host;
        return e->host;
    }
    fetchTag = ~0u;
    return nullptr;
}

// Windows CE makes every system call by jumping to an unmapped trap address (0xF000xxxx), so
// prefetch aborts run at thousands per second. Checking up front lets Cpu::run take them without
// a C++ exception (an unwind costs ~15 us; this is a TLB lookup).
bool Cpu::canFetch(uint32_t va) {
    if (prefetchLeft && va == prefetchVa) return true;
    uint32_t tag = (va >> 12) | (mode == MODE_USR ? 0x80000000u : 0);
    if (tag == fetchTag) return true;
    uint32_t mva = vaMod(va);
    bool u = mode == MODE_USR;
    const TlbEntry &e = tlb[u][(mva >> 12) & (TLB_SIZE - 1)];
    if (e.tag == tlbKey(mva) && (e.perm & 1)) return true;
    uint32_t fsr;
    if (tlbFillNoThrow(mva, false, u, fsr)) return true;
    cp15_ifsr = fsr;
    return false;
}

uint32_t Cpu::fetch32(uint32_t va) {
    if (prefetchLeft) {
        prefetchLeft--;
        if (va == prefetchVa) {
            uint32_t pa = prefetchPa;
            prefetchVa += 4;
            prefetchPa += 4;
            return bus.read(pa, 4);
        }
        prefetchLeft = 0;
    }
    uint32_t tag = (va >> 12) | (mode == MODE_USR ? 0x80000000u : 0);
    if (tag == fetchTag) { uint32_t v; memcpy(&v, fetchHost + (va & 0xFFC), 4); return v; }
    uint8_t *h = fetchPage(va);
    if (h) { uint32_t v; memcpy(&v, h + (va & 0xFFC), 4); return v; }
    return bus.read(fetchPa + (vaMod(va) & 0xFFC), 4);
}

uint16_t Cpu::fetch16(uint32_t va) {
    uint32_t tag = (va >> 12) | (mode == MODE_USR ? 0x80000000u : 0);
    if (tag == fetchTag) { uint16_t v; memcpy(&v, fetchHost + (va & 0xFFE), 2); return v; }
    uint8_t *h = fetchPage(va);
    if (h) { uint16_t v; memcpy(&v, h + (va & 0xFFE), 2); return v; }
    return (uint16_t)bus.read(fetchPa + (vaMod(va) & 0xFFE), 2);
}

bool Cpu::debugRead32(uint32_t va, uint32_t &out) {
    uint32_t pa, fsr;
    if (!translate(vaMod(va), false, false, pa, fsr)) return false;
    out = bus.read(pa & ~3u, 4);
    return true;
}

// ---------------------------------------------------------------------------------------------
// Execution

uint64_t Cpu::run(uint64_t count) {
    uint64_t done = 0;
    while (done < count) {
        if (halted) {
            if (irqLine || fiqLine) halted = false;
            else break;
        }
        if ((irqLine && !fI) || (fiqLine && !fF)) checkInterrupts();
        if (jitEnabled && !fT && !traceHook && !prefetchLeft) {
            uint32_t reason, executed;
            if (jitRun(jit, count - done, reason, executed)) {
                done += executed;
                handleJitExit(reason);
                if (singleBlock) break;
                continue;
            }
        }
        uint32_t pcCur = r[15];
        if (!canFetch(pcCur)) {  // prefetch abort, as the catch below would take it
            prefetchAborts++;
            cp15_far = pcCur;  // ARM920T leaves FAR alone; harmless for CE.
            exception(0x0C, MODE_ABT, pcCur + 4, false);
            done++;
            continue;
        }
        try {
            branched = false;
            if (fT) {
                uint16_t insn = fetch16(pcCur);
                r[15] = pcCur + 4;
                if (traceHook) traceHook(traceCtx, *this, pcCur, insn, true);
                execThumb(insn);
                if (!branched) r[15] = pcCur + 2;
            } else {
                uint32_t insn = fetch32(pcCur);
                r[15] = pcCur + 8;
                if (traceHook) traceHook(traceCtx, *this, pcCur, insn, false);
                uint32_t cond = insn >> 28;
                if (cond == 0xE || (cond != 0xF ? condPass(cond) : true)) execArm(insn);
                if (!branched) r[15] = pcCur + 4;
            }
        } catch (const MemAbort &a) {
            r[15] = pcCur;
            if (a.prefetch) {
                prefetchAborts++;
                cp15_far = pcCur;  // ARM920T leaves FAR alone; harmless for CE.
                exception(0x0C, MODE_ABT, pcCur + 4, false);
            } else {
                dataAborts++;
                exception(0x10, MODE_ABT, pcCur + 8, false);
            }
        }
        done++;
    }
    insnCount += done;
    return done;
    // (single-step interpreter path also honours singleBlock via count)
}

void Cpu::handleJitExit(uint32_t reason) {
    switch (reason) {
        case JIT_BX: {
            uint32_t v = r[15];
            fT = v & 1;
            r[15] = v & (fT ? ~1u : ~3u);
            if (fT) fetchTag = ~0u;
            break;
        }
        case JIT_LOADPC:
            if (core >= CORE_ARMV5) {
                uint32_t v = r[15];
                fT = v & 1;
                r[15] = v & (fT ? ~1u : ~3u);
            } else {
                r[15] &= ~3u;
            }
            break;
        case JIT_ABORT: {
            dataAborts++;
            uint32_t p = r[15];
            exception(0x10, MODE_ABT, p + 8, false);
            break;
        }
        case JIT_IDLE:
            if (!fI && mode != MODE_USR) {
                halted = true;
                idleHits++;
            }
            break;
        default: break;
    }
}

bool Cpu::condPass(uint32_t cond) const {
    switch (cond) {
        case 0x0: return fZ;
        case 0x1: return !fZ;
        case 0x2: return fC;
        case 0x3: return !fC;
        case 0x4: return fN;
        case 0x5: return !fN;
        case 0x6: return fV;
        case 0x7: return !fV;
        case 0x8: return fC && !fZ;
        case 0x9: return !fC || fZ;
        case 0xA: return fN == fV;
        case 0xB: return fN != fV;
        case 0xC: return !fZ && fN == fV;
        case 0xD: return fZ || fN != fV;
        case 0xE: return true;
        default: return false;
    }
}

// The OAL idles in "LDR Rd,[Rn,#imm]; CMP Rd,#imm; B<cond> back" until an interrupt handler changes
// the word. With interrupts enabled nothing else can change it, so stop until the next IRQ.
void Cpu::checkIdleLoop(uint32_t target) {
    uint32_t i0, i1;
    if (!debugRead32(target, i0) || !debugRead32(target + 4, i1)) return;
    bool ldr = (i0 & 0x0F700000) == 0x05100000;           // LDR Rd, [Rn, #+/-imm]
    bool cmp = (i1 & 0x0FF00000) == 0x03500000 &&         // CMP Rd, #imm
               ((i1 >> 16) & 0xF) == ((i0 >> 12) & 0xF);
    if (ldr && cmp) {
        halted = true;
        idleHits++;
    }
}

void Cpu::undefined() {
    uint32_t next = r[15] - (fT ? 2 : 4);
    exception(0x04, MODE_UND, next, false);
}

void Cpu::setPC(uint32_t v) {
    r[15] = v & (fT ? ~1u : ~3u);
    branched = true;
}

void Cpu::writePcFromLoad(uint32_t v) {
    if (core >= CORE_ARMV5) {
        fT = v & 1;
        r[15] = v & (fT ? ~1u : ~3u);
    } else {
        r[15] = v & ~3u;
    }
    branched = true;
}

uint32_t Cpu::getUserReg(int i) const {
    if (i < 8 || i == 15) return r[i];
    int b = bankIndex(mode);
    if (i <= 12) return b == 1 ? bankUsr[i - 8] : r[i];
    if (b == 0) return r[i];
    return i == 13 ? bankSP[0] : bankLR[0];
}

void Cpu::setUserReg(int i, uint32_t v) {
    if (i < 8) { r[i] = v; return; }
    if (i == 15) { setPC(v); return; }
    int b = bankIndex(mode);
    if (i <= 12) { if (b == 1) bankUsr[i - 8] = v; else r[i] = v; return; }
    if (b == 0) { r[i] = v; return; }
    if (i == 13) bankSP[0] = v; else bankLR[0] = v;
}

inline uint32_t Cpu::shifterOperand(uint32_t insn, uint32_t &carry) {
    if (insn & (1 << 25)) {
        uint32_t imm = insn & 0xFF, rot = (insn >> 7) & 0x1E;
        uint32_t v = ror32(imm, rot);
        carry = rot ? v >> 31 : fC;
        return v;
    }
    uint32_t rm = insn & 0xF;
    uint32_t type = (insn >> 5) & 3;
    uint32_t v;
    if (insn & 0x10) {  // register shift
        v = r[rm] + (rm == 15 ? 4 : 0);
        uint32_t s = r[(insn >> 8) & 0xF] & 0xFF;
        if (s == 0) { carry = fC; return v; }
        switch (type) {
            case 0:
                if (s < 32) { carry = (v >> (32 - s)) & 1; return v << s; }
                carry = s == 32 ? v & 1 : 0;
                return 0;
            case 1:
                if (s < 32) { carry = (v >> (s - 1)) & 1; return v >> s; }
                carry = s == 32 ? v >> 31 : 0;
                return 0;
            case 2:
                if (s < 32) { carry = (v >> (s - 1)) & 1; return (uint32_t)((int32_t)v >> s); }
                carry = v >> 31;
                return (uint32_t)((int32_t)v >> 31);
            default:
                s &= 31;
                if (s == 0) { carry = v >> 31; return v; }
                carry = (v >> (s - 1)) & 1;
                return ror32(v, s);
        }
    }
    v = r[rm];
    uint32_t s = (insn >> 7) & 0x1F;
    switch (type) {
        case 0:
            if (s == 0) { carry = fC; return v; }
            carry = (v >> (32 - s)) & 1;
            return v << s;
        case 1:
            if (s == 0) { carry = v >> 31; return 0; }
            carry = (v >> (s - 1)) & 1;
            return v >> s;
        case 2:
            if (s == 0) { carry = v >> 31; return (uint32_t)((int32_t)v >> 31); }
            carry = (v >> (s - 1)) & 1;
            return (uint32_t)((int32_t)v >> s);
        default:
            if (s == 0) { carry = v & 1; return (fC << 31) | (v >> 1); }
            carry = (v >> (s - 1)) & 1;
            return ror32(v, s);
    }
}

void Cpu::dataProcessing(uint32_t insn) {
    uint32_t op = (insn >> 21) & 0xF;
    bool S = insn & (1 << 20);
    uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    uint32_t carry;
    uint32_t b = shifterOperand(insn, carry);
    uint32_t a = r[rn];
    if (rn == 15 && !(insn & (1 << 25)) && (insn & 0x10)) a += 4;
    uint32_t res = 0;
    bool logical = false, writes = true;
    uint64_t wide;
    switch (op) {
        case 0x0: res = a & b; logical = true; break;
        case 0x1: res = a ^ b; logical = true; break;
        case 0x2:
            res = a - b;
            if (S) { carry = a >= b; fV = ((a ^ b) & (a ^ res)) >> 31; }
            break;
        case 0x3:
            res = b - a;
            if (S) { carry = b >= a; fV = ((b ^ a) & (b ^ res)) >> 31; }
            break;
        case 0x4:
            wide = (uint64_t)a + b; res = (uint32_t)wide;
            if (S) { carry = wide >> 32; fV = (~(a ^ b) & (a ^ res)) >> 31; }
            break;
        case 0x5:
            wide = (uint64_t)a + b + fC; res = (uint32_t)wide;
            if (S) { carry = wide >> 32; fV = (~(a ^ b) & (a ^ res)) >> 31; }
            break;
        case 0x6:
            wide = (uint64_t)a - b - (1 - fC); res = (uint32_t)wide;
            if (S) { carry = ((uint64_t)a >= (uint64_t)b + (1 - fC)); fV = ((a ^ b) & (a ^ res)) >> 31; }
            break;
        case 0x7:
            wide = (uint64_t)b - a - (1 - fC); res = (uint32_t)wide;
            if (S) { carry = ((uint64_t)b >= (uint64_t)a + (1 - fC)); fV = ((b ^ a) & (b ^ res)) >> 31; }
            break;
        case 0x8: res = a & b; logical = true; writes = false; break;
        case 0x9: res = a ^ b; logical = true; writes = false; break;
        case 0xA:
            res = a - b; writes = false;
            carry = a >= b; fV = ((a ^ b) & (a ^ res)) >> 31;
            break;
        case 0xB:
            wide = (uint64_t)a + b; res = (uint32_t)wide; writes = false;
            carry = wide >> 32; fV = (~(a ^ b) & (a ^ res)) >> 31;
            break;
        case 0xC: res = a | b; logical = true; break;
        case 0xD: res = b; logical = true; break;
        case 0xE: res = a & ~b; logical = true; break;
        case 0xF: res = ~b; logical = true; break;
    }
    (void)logical;
    if (writes && rd == 15) {
        if (S) {
            uint32_t s = spsr;
            setCpsr(s, false, 0xFFFFFFFF);
            fT = (s >> 5) & 1;
        }
        setPC(res);
        return;
    }
    if (S) {
        fN = res >> 31;
        fZ = res == 0;
        fC = carry;
    }
    if (writes) r[rd] = res;
}

void Cpu::singleTransfer(uint32_t insn) {
    bool P = insn & (1 << 24), U = insn & (1 << 23), B = insn & (1 << 22), W = insn & (1 << 21),
         L = insn & (1 << 20);
    uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    uint32_t off;
    if (insn & (1 << 25)) {
        uint32_t c;
        off = shifterOperand(insn & ~0x10u & ~(1u << 25), c);
    } else {
        off = insn & 0xFFF;
    }
    uint32_t base = r[rn];
    uint32_t offAddr = U ? base + off : base - off;
    uint32_t addr = P ? offAddr : base;
    bool user = !P && W;
    bool wb = !P || W;
    if (L) {
        uint32_t v;
        if (B) v = read8(addr, user);
        else v = ror32(read32(addr, user), (addr & 3) * 8);
        if (wb && rn != 15) r[rn] = offAddr;
        if (rd == 15) writePcFromLoad(v);
        else r[rd] = v;
    } else {
        uint32_t v = r[rd] + (rd == 15 ? 4 : 0);
        if (B) write8(addr, (uint8_t)v, user);
        else write32(addr, v, user);
        if (wb && rn != 15) r[rn] = offAddr;
    }
}

void Cpu::halfTransfer(uint32_t insn) {
    bool P = insn & (1 << 24), U = insn & (1 << 23), I = insn & (1 << 22), W = insn & (1 << 21),
         L = insn & (1 << 20);
    uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    uint32_t sh = (insn >> 5) & 3;
    uint32_t off = I ? (((insn >> 4) & 0xF0) | (insn & 0xF)) : r[insn & 0xF];
    uint32_t base = r[rn];
    uint32_t offAddr = U ? base + off : base - off;
    uint32_t addr = P ? offAddr : base;
    bool wb = !P || W;
    if (L) {
        uint32_t v;
        if (sh == 1) v = read16(addr);
        else if (sh == 2) v = (uint32_t)(int32_t)(int8_t)read8(addr);
        else v = (uint32_t)(int32_t)(int16_t)read16(addr);
        if (wb && rn != 15) r[rn] = offAddr;
        if (rd == 15) writePcFromLoad(v);
        else r[rd] = v;
    } else if (sh == 1) {
        write16(addr, (uint16_t)(r[rd] + (rd == 15 ? 4 : 0)));
        if (wb && rn != 15) r[rn] = offAddr;
    } else {
        if (core < CORE_ARMV5 || (rd & 1)) { undefined(); return; }
        if (sh == 2) {  // LDRD
            uint32_t v0 = read32(addr), v1 = read32(addr + 4);
            if (wb && rn != 15) r[rn] = offAddr;
            r[rd] = v0;
            if (rd + 1 == 15) writePcFromLoad(v1); else r[rd + 1] = v1;
        } else {  // STRD
            write32(addr, r[rd]);
            write32(addr + 4, r[rd + 1]);
            if (wb && rn != 15) r[rn] = offAddr;
        }
    }
}

void Cpu::blockTransfer(uint32_t insn) {
    bool P = insn & (1 << 24), U = insn & (1 << 23), S = insn & (1 << 22), W = insn & (1 << 21),
         L = insn & (1 << 20);
    uint32_t rn = (insn >> 16) & 0xF;
    uint32_t list = insn & 0xFFFF;
    uint32_t n = __builtin_popcount(list);
    uint32_t base = r[rn];
    if (list == 0) { list = 0x8000; n = 16; }  // unpredictable; mimic ARM9 behaviour roughly
    uint32_t start;
    if (U) start = P ? base + 4 : base;
    else start = P ? base - 4 * n : base - 4 * n + 4;
    uint32_t newBase = U ? base + 4 * n : base - 4 * n;
    uint32_t addr = start & ~3u;
    if (L) {
        uint32_t vals[16];
        uint32_t a = addr;
        for (int i = 0; i < 16; i++)
            if (list & (1 << i)) { vals[i] = read32(a); a += 4; }
        if (W) r[rn] = newBase;
        bool userBank = S && !(list & 0x8000);
        for (int i = 0; i < 15; i++) {
            if (!(list & (1 << i))) continue;
            if (userBank) setUserReg(i, vals[i]);
            else r[i] = vals[i];
        }
        if (list & 0x8000) {
            if (S) {
                uint32_t s = spsr;
                setCpsr(s, false, 0xFFFFFFFF);
                fT = (s >> 5) & 1;
                setPC(vals[15]);
            } else {
                writePcFromLoad(vals[15]);
            }
        }
    } else {
        uint32_t a = addr;
        for (int i = 0; i < 16; i++) {
            if (!(list & (1 << i))) continue;
            uint32_t v = S ? getUserReg(i) : r[i];
            if (i == 15) v += 4;
            write32(a, v);
            a += 4;
        }
        if (W) r[rn] = newBase;
    }
}

void Cpu::multiply(uint32_t insn) {
    bool S = insn & (1 << 20);
    uint32_t rd = (insn >> 16) & 0xF, rn = (insn >> 12) & 0xF, rs = (insn >> 8) & 0xF, rm = insn & 0xF;
    if (!(insn & (1 << 23))) {
        uint32_t res = r[rm] * r[rs];
        if (insn & (1 << 21)) res += r[rn];
        r[rd] = res;
        if (S) { fN = res >> 31; fZ = res == 0; }
    } else {
        uint64_t res;
        if (insn & (1 << 22)) res = (uint64_t)((int64_t)(int32_t)r[rm] * (int64_t)(int32_t)r[rs]);
        else res = (uint64_t)r[rm] * r[rs];
        if (insn & (1 << 21)) res += ((uint64_t)r[rd] << 32) | r[rn];
        r[rn] = (uint32_t)res;
        r[rd] = (uint32_t)(res >> 32);
        if (S) { fN = (res >> 63) & 1; fZ = res == 0; }
    }
}

void Cpu::dspMisc(uint32_t insn) {
    // Called for the opcode space 0b00010xx0 with bit 7/4 patterns (MRS/MSR/BX/CLZ/QADD/SMLAxy/BKPT).
    uint32_t rd = (insn >> 12) & 0xF;
    if ((insn & 0x0FBF0FFF) == 0x010F0000) {  // MRS
        r[rd] = (insn & (1 << 22)) ? spsr : getCpsr();
        return;
    }
    if ((insn & 0x0FB0FFF0) == 0x0120F000) {  // MSR reg
        uint32_t v = r[insn & 0xF];
        uint32_t mask = 0;
        if (insn & (1 << 16)) mask |= 0xFF;
        if (insn & (1 << 17)) mask |= 0xFF00;
        if (insn & (1 << 18)) mask |= 0xFF0000;
        if (insn & (1 << 19)) mask |= 0xFF000000;
        if (insn & (1 << 22)) {
            if (mode != MODE_USR && mode != MODE_SYS) spsr = (spsr & ~mask) | (v & mask);
        } else {
            setCpsr((v & ~0x20u) | (fT << 5), false, mask);
        }
        return;
    }
    if ((insn & 0x0FFFFFF0) == 0x012FFF10) {  // BX
        uint32_t v = r[insn & 0xF];
        fT = v & 1;
        setPC(v);
        return;
    }
    if ((insn & 0x0FFFFFF0) == 0x012FFF30 && core >= CORE_ARMV5) {  // BLX reg
        uint32_t v = r[insn & 0xF];
        r[14] = r[15] - 4;
        fT = v & 1;
        setPC(v);
        return;
    }
    if ((insn & 0x0FFF0FF0) == 0x016F0F10 && core >= CORE_ARMV5) {  // CLZ
        uint32_t v = r[insn & 0xF];
        r[rd] = v ? __builtin_clz(v) : 32;
        return;
    }
    if ((insn & 0x0FF000F0) == 0x01200070) {  // BKPT
        cp15_ifsr = 2;
        uint32_t pcCur = r[15] - 8;
        exception(0x0C, MODE_ABT, pcCur + 4, false);
        return;
    }
    if (core >= CORE_ARMV5 && (insn & 0x0F900FF0) == 0x01000050) {  // QADD/QSUB/QDADD/QDSUB
        int64_t a = (int32_t)r[insn & 0xF], b = (int32_t)r[(insn >> 16) & 0xF];
        uint32_t op = (insn >> 21) & 3;
        if (op & 2) b = sat32(b * 2, fQ);
        r[rd] = (uint32_t)sat32((op & 1) ? a - b : a + b, fQ);
        return;
    }
    if (core >= CORE_ARMV5 && (insn & 0x0F900090) == 0x01000080) {  // signed halfword multiplies
        uint32_t op = (insn >> 21) & 3;
        uint32_t rdd = (insn >> 16) & 0xF, rn = (insn >> 12) & 0xF, rs = (insn >> 8) & 0xF, rm = insn & 0xF;
        bool x = insn & (1 << 5), y = insn & (1 << 6);
        int32_t m = x ? (int16_t)(r[rm] >> 16) : (int16_t)r[rm];
        int32_t s = y ? (int16_t)(r[rs] >> 16) : (int16_t)r[rs];
        switch (op) {
            case 0: {  // SMLAxy
                int64_t res = (int64_t)(m * s) + (int32_t)r[rn];
                if (res != (int32_t)res) fQ = 1;
                r[rdd] = (uint32_t)res;
                break;
            }
            case 1: {  // SMLAWy / SMULWy
                int64_t prod = ((int64_t)(int32_t)r[rm] * s) >> 16;
                if (!x) {
                    int64_t res = prod + (int32_t)r[rn];
                    if (res != (int32_t)res) fQ = 1;
                    r[rdd] = (uint32_t)res;
                } else {
                    r[rdd] = (uint32_t)prod;
                }
                break;
            }
            case 2: {  // SMLALxy
                int64_t acc = (int64_t)(((uint64_t)r[rdd] << 32) | r[rn]);
                acc += (int64_t)m * s;
                r[rn] = (uint32_t)acc;
                r[rdd] = (uint32_t)((uint64_t)acc >> 32);
                break;
            }
            case 3: r[rdd] = (uint32_t)(m * s); break;
        }
        return;
    }
    undefined();
}

void Cpu::coprocessor(uint32_t insn) {
    uint32_t cp = (insn >> 8) & 0xF;
    if ((insn & 0x0F000010) == 0x0E000010) {  // MRC/MCR
        bool L = insn & (1 << 20);
        uint32_t op1 = (insn >> 21) & 7, crn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF,
                 op2 = (insn >> 5) & 7, crm = insn & 0xF;
        if (cp == 15) {
            if (mode == MODE_USR) { undefined(); return; }
            if (L) {
                uint32_t v = cp15Read(crn, crm, op1, op2);
                if (rd == 15) { fN = v >> 31; fZ = (v >> 30) & 1; fC = (v >> 29) & 1; fV = (v >> 28) & 1; }
                else r[rd] = v;
            } else {
                cp15Write(crn, crm, op1, op2, r[rd] + (rd == 15 ? 4 : 0));
            }
            return;
        }
        if (cp == 14) {  // debug coprocessor: reads as zero
            if (L && rd != 15) r[rd] = 0;
            return;
        }
    }
    if (traceUndef) LOGW("cpu: coprocessor insn %08x at %08x (cp%u)", insn, r[15] - 8, cp);
    undefined();
}

uint32_t Cpu::cp15Read(uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2) {
    (void)op1;
    switch (crn) {
        case 0:
            if (op2 == 1) return 0x0B172172;
            if (op2 == 2) return 0;
            return cpuId;
        case 1: return cp15_ctrl;
        case 2: return cp15_ttb;
        case 3: return cp15_dacr;
        case 5: return op2 == 1 ? cp15_ifsr : cp15_fsr;
        case 6: return cp15_far;
        case 7:
            if ((crm == 10 || crm == 14) && op2 == 3) return 0x40000000;  // test-and-clean: done
            return 0;
        case 13: return op2 == 1 ? cp15_tid : cp15_pid;
        default: return 0;
    }
    (void)crm;
}

void Cpu::cp15Write(uint32_t crn, uint32_t crm, uint32_t op1, uint32_t op2, uint32_t v) {
    (void)op1;
    switch (crn) {
        case 1: {
            uint32_t old = cp15_ctrl;
            if ((old ^ v) & 1) {
                // The next instructions are already in the pipeline: keep fetching them through the
                // old mapping (code that switches the MMU relies on this, e.g. the IPL's jump to image).
                uint32_t nextVa = r[15] - 4, pa, fsr;
                if (translate(vaMod(nextVa), false, false, pa, fsr)) {
                    prefetchVa = nextVa;
                    prefetchPa = pa;
                    prefetchLeft = 2;
                }
            }
            // Only M, A and V are honoured; cache/write-buffer bits read back as set.
            cp15_ctrl = (v & 0xC0002003u) | 0x187Cu;
            if ((old ^ cp15_ctrl) & 0x2001) flushTlb();
            if ((old & 1) && !(cp15_ctrl & 1)) mmuDisabled = true;  // the board notices a guest restart
            break;
        }
        case 2: cp15_ttb = v; flushTlb(); break;
        case 3: if (cp15_dacr != v) { cp15_dacr = v; flushTlb(); } break;
        case 5: if (op2 == 1) cp15_ifsr = v; else cp15_fsr = v; break;
        case 6: cp15_far = v; break;
        case 7:
            if (crm == 0 && op2 == 4) halted = true;  // wait for interrupt
            break;
        case 8: flushTlb(); break;
        case 13:
            if (op2 == 1) cp15_tid = v;
            // The TLB is indexed by modified VA, so a PID switch only invalidates the fetch cache.
            else if ((v & 0xFE000000) != cp15_pid) { cp15_pid = v & 0xFE000000; fetchTag = ~0u; }
            break;
        default: break;
    }
}

void Cpu::execArm(uint32_t insn) {
    if ((insn >> 28) == 0xF) {
        if (core >= CORE_ARMV5 && (insn & 0x0E000000) == 0x0A000000) {  // BLX imm
            int32_t off = ((int32_t)(insn << 8) >> 6) | ((insn >> 23) & 2);
            r[14] = r[15] - 4;
            fT = 1;
            r[15] = r[15] + off;
            branched = true;
            return;
        }
        if ((insn & 0x0D70F000) == 0x0550F000) return;  // PLD
        undefined();
        return;
    }
    switch ((insn >> 25) & 7) {
        case 0:
            if ((insn & 0xF0) == 0x90) {
                if ((insn & 0x0F000000) == 0 && ((insn & (1 << 23)) || !(insn & (1 << 22)))) {
                    multiply(insn);
                    return;
                }
                if ((insn & 0x0FB00FF0) == 0x01000090) {  // SWP/SWPB
                    uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF, rm = insn & 0xF;
                    uint32_t addr = r[rn];
                    if (insn & (1 << 22)) {
                        uint8_t t = read8(addr);
                        write8(addr, (uint8_t)r[rm]);
                        r[rd] = t;
                    } else {
                        uint32_t t = ror32(read32(addr), (addr & 3) * 8);
                        write32(addr, r[rm]);
                        r[rd] = t;
                    }
                    return;
                }
                undefined();
                return;
            }
            if ((insn & 0x90) == 0x90) { halfTransfer(insn); return; }
            if ((insn & 0x01900000) == 0x01000000) { dspMisc(insn); return; }
            dataProcessing(insn);
            return;
        case 1:
            if ((insn & 0x01900000) == 0x01000000) {
                if (insn & (1 << 21)) {  // MSR immediate
                    uint32_t v = ror32(insn & 0xFF, (insn >> 7) & 0x1E);
                    uint32_t mask = 0;
                    if (insn & (1 << 16)) mask |= 0xFF;
                    if (insn & (1 << 17)) mask |= 0xFF00;
                    if (insn & (1 << 18)) mask |= 0xFF0000;
                    if (insn & (1 << 19)) mask |= 0xFF000000;
                    if (insn & (1 << 22)) {
                        if (mode != MODE_USR && mode != MODE_SYS) spsr = (spsr & ~mask) | (v & mask);
                    } else {
                        setCpsr((v & ~0x20u) | (fT << 5), false, mask);
                    }
                    return;
                }
                undefined();
                return;
            }
            dataProcessing(insn);
            return;
        case 2: singleTransfer(insn); return;
        case 3:
            if (insn & 0x10) { undefined(); return; }
            singleTransfer(insn);
            return;
        case 4: blockTransfer(insn); return;
        case 5: {
            int32_t off = (int32_t)(insn << 8) >> 6;
            if (insn & (1 << 24)) r[14] = r[15] - 4;
            else if (off == -16 && !fI && mode != MODE_USR) checkIdleLoop(r[15] - 16);
            r[15] = r[15] + off;
            branched = true;
            return;
        }
        case 6: coprocessor(insn); return;
        case 7:
            if (insn & (1 << 24)) {
                exception(0x08, MODE_SVC, r[15] - 4, false);
                return;
            }
            coprocessor(insn);
            return;
    }
}

// ---------------------------------------------------------------------------------------------
// Thumb

void Cpu::execThumb(uint16_t insn) {
    uint32_t pc = r[15];  // current + 4
    switch (insn >> 11) {
        case 0x00: case 0x01: case 0x02: {  // shift by immediate
            uint32_t rd = insn & 7, rm = (insn >> 3) & 7, s = (insn >> 6) & 0x1F;
            uint32_t v = r[rm], c = fC;
            switch (insn >> 11) {
                case 0: if (s) { c = (v >> (32 - s)) & 1; v <<= s; } break;
                case 1:
                    if (s == 0) { c = v >> 31; v = 0; } else { c = (v >> (s - 1)) & 1; v >>= s; }
                    break;
                default:
                    if (s == 0) { c = v >> 31; v = (uint32_t)((int32_t)v >> 31); }
                    else { c = (v >> (s - 1)) & 1; v = (uint32_t)((int32_t)v >> s); }
                    break;
            }
            r[rd] = v; fN = v >> 31; fZ = v == 0; fC = c;
            return;
        }
        case 0x03: {  // add/sub
            uint32_t rd = insn & 7, rn = (insn >> 3) & 7;
            uint32_t b = (insn & (1 << 10)) ? (insn >> 6) & 7 : r[(insn >> 6) & 7];
            uint32_t a = r[rn], res;
            if (insn & (1 << 9)) {
                res = a - b; fC = a >= b; fV = ((a ^ b) & (a ^ res)) >> 31;
            } else {
                uint64_t w = (uint64_t)a + b; res = (uint32_t)w; fC = w >> 32; fV = (~(a ^ b) & (a ^ res)) >> 31;
            }
            r[rd] = res; fN = res >> 31; fZ = res == 0;
            return;
        }
        case 0x04: case 0x05: case 0x06: case 0x07: {  // mov/cmp/add/sub imm8
            uint32_t rd = (insn >> 8) & 7, imm = insn & 0xFF, a = r[rd], res;
            switch ((insn >> 11) & 3) {
                case 0: res = imm; r[rd] = res; break;
                case 1: res = a - imm; fC = a >= imm; fV = ((a ^ imm) & (a ^ res)) >> 31; break;
                case 2: { uint64_t w = (uint64_t)a + imm; res = (uint32_t)w; fC = w >> 32; fV = (~(a ^ imm) & (a ^ res)) >> 31; r[rd] = res; break; }
                default: res = a - imm; fC = a >= imm; fV = ((a ^ imm) & (a ^ res)) >> 31; r[rd] = res; break;
            }
            fN = res >> 31; fZ = res == 0;
            return;
        }
        case 0x08:
            if (!(insn & 0x400)) {  // ALU
                uint32_t rd = insn & 7, rm = (insn >> 3) & 7;
                uint32_t a = r[rd], b = r[rm], res = 0;
                bool wr = true;
                switch ((insn >> 6) & 0xF) {
                    case 0x0: res = a & b; break;
                    case 0x1: res = a ^ b; break;
                    case 0x2: {
                        uint32_t s = b & 0xFF;
                        if (s == 0) res = a;
                        else if (s < 32) { fC = (a >> (32 - s)) & 1; res = a << s; }
                        else { fC = s == 32 ? a & 1 : 0; res = 0; }
                        break;
                    }
                    case 0x3: {
                        uint32_t s = b & 0xFF;
                        if (s == 0) res = a;
                        else if (s < 32) { fC = (a >> (s - 1)) & 1; res = a >> s; }
                        else { fC = s == 32 ? a >> 31 : 0; res = 0; }
                        break;
                    }
                    case 0x4: {
                        uint32_t s = b & 0xFF;
                        if (s == 0) res = a;
                        else if (s < 32) { fC = (a >> (s - 1)) & 1; res = (uint32_t)((int32_t)a >> s); }
                        else { fC = a >> 31; res = (uint32_t)((int32_t)a >> 31); }
                        break;
                    }
                    case 0x5: { uint64_t w = (uint64_t)a + b + fC; res = (uint32_t)w; fC = w >> 32; fV = (~(a ^ b) & (a ^ res)) >> 31; break; }
                    case 0x6: {
                        uint32_t nc = 1 - fC;
                        res = a - b - nc; fC = (uint64_t)a >= (uint64_t)b + nc; fV = ((a ^ b) & (a ^ res)) >> 31;
                        break;
                    }
                    case 0x7: {
                        uint32_t s = b & 0xFF;
                        if (s == 0) res = a;
                        else if ((s & 31) == 0) { fC = a >> 31; res = a; }
                        else { fC = (a >> ((s & 31) - 1)) & 1; res = ror32(a, s & 31); }
                        break;
                    }
                    case 0x8: res = a & b; wr = false; break;
                    case 0x9: res = 0 - b; fC = b == 0; fV = (b & res) >> 31; break;
                    case 0xA: res = a - b; fC = a >= b; fV = ((a ^ b) & (a ^ res)) >> 31; wr = false; break;
                    case 0xB: { uint64_t w = (uint64_t)a + b; res = (uint32_t)w; fC = w >> 32; fV = (~(a ^ b) & (a ^ res)) >> 31; wr = false; break; }
                    case 0xC: res = a | b; break;
                    case 0xD: res = a * b; break;
                    case 0xE: res = a & ~b; break;
                    case 0xF: res = ~b; break;
                }
                if (wr) r[rd] = res;
                fN = res >> 31; fZ = res == 0;
                return;
            } else {  // hi register ops / BX
                uint32_t op = (insn >> 8) & 3;
                uint32_t rd = (insn & 7) | ((insn >> 4) & 8), rm = (insn >> 3) & 0xF;
                uint32_t b = r[rm];
                switch (op) {
                    case 0:
                        if (rd == 15) setPC(r[15] + b);
                        else r[rd] += b;
                        return;
                    case 1: {
                        uint32_t a = r[rd], res = a - b;
                        fN = res >> 31; fZ = res == 0; fC = a >= b; fV = ((a ^ b) & (a ^ res)) >> 31;
                        return;
                    }
                    case 2:
                        if (rd == 15) setPC(b);
                        else r[rd] = b;
                        return;
                    default:
                        if (insn & 0x80) {
                            if (core < CORE_ARMV5) { undefined(); return; }
                            r[14] = (pc - 2) | 1;
                        }
                        fT = b & 1;
                        setPC(b);
                        return;
                }
            }
        case 0x09: {  // LDR pc-relative
            uint32_t rd = (insn >> 8) & 7;
            r[rd] = read32((pc & ~3u) + (insn & 0xFF) * 4);
            return;
        }
        case 0x0A: case 0x0B: {  // load/store register offset
            uint32_t rd = insn & 7, rn = (insn >> 3) & 7, rm = (insn >> 6) & 7;
            uint32_t addr = r[rn] + r[rm];
            switch ((insn >> 9) & 7) {
                case 0: write32(addr, r[rd]); break;
                case 1: write16(addr, (uint16_t)r[rd]); break;
                case 2: write8(addr, (uint8_t)r[rd]); break;
                case 3: r[rd] = (uint32_t)(int32_t)(int8_t)read8(addr); break;
                case 4: r[rd] = ror32(read32(addr), (addr & 3) * 8); break;
                case 5: r[rd] = read16(addr); break;
                case 6: r[rd] = read8(addr); break;
                case 7: r[rd] = (uint32_t)(int32_t)(int16_t)read16(addr); break;
            }
            return;
        }
        case 0x0C: case 0x0D: case 0x0E: case 0x0F: {  // ldr/str imm5
            uint32_t rd = insn & 7, rn = (insn >> 3) & 7, imm = (insn >> 6) & 0x1F;
            bool B = insn & (1 << 12), L = insn & (1 << 11);
            uint32_t addr = r[rn] + (B ? imm : imm * 4);
            if (L) r[rd] = B ? read8(addr) : ror32(read32(addr), (addr & 3) * 8);
            else if (B) write8(addr, (uint8_t)r[rd]);
            else write32(addr, r[rd]);
            return;
        }
        case 0x10: case 0x11: {  // ldrh/strh imm5
            uint32_t rd = insn & 7, rn = (insn >> 3) & 7, addr = r[rn] + ((insn >> 6) & 0x1F) * 2;
            if (insn & (1 << 11)) r[rd] = read16(addr);
            else write16(addr, (uint16_t)r[rd]);
            return;
        }
        case 0x12: case 0x13: {  // sp-relative
            uint32_t rd = (insn >> 8) & 7, addr = r[13] + (insn & 0xFF) * 4;
            if (insn & (1 << 11)) r[rd] = ror32(read32(addr), (addr & 3) * 8);
            else write32(addr, r[rd]);
            return;
        }
        case 0x14: case 0x15: {  // add rd, pc/sp, imm
            uint32_t rd = (insn >> 8) & 7, imm = (insn & 0xFF) * 4;
            r[rd] = (insn & (1 << 11)) ? r[13] + imm : (pc & ~3u) + imm;
            return;
        }
        case 0x16: case 0x17: {  // misc
            uint32_t sub = (insn >> 8) & 0xF;
            if (sub == 0) {
                uint32_t imm = (insn & 0x7F) * 4;
                if (insn & 0x80) r[13] -= imm; else r[13] += imm;
                return;
            }
            if (sub == 4 || sub == 5) {  // PUSH
                uint32_t list = insn & 0xFF;
                uint32_t n = __builtin_popcount(list) + ((insn >> 8) & 1);
                uint32_t addr = r[13] - 4 * n, a = addr;
                for (int i = 0; i < 8; i++) if (list & (1 << i)) { write32(a, r[i]); a += 4; }
                if (insn & 0x100) write32(a, r[14]);
                r[13] = addr;
                return;
            }
            if (sub == 0xC || sub == 0xD) {  // POP
                uint32_t list = insn & 0xFF, vals[9], a = r[13];
                for (int i = 0; i < 8; i++) if (list & (1 << i)) { vals[i] = read32(a); a += 4; }
                if (insn & 0x100) { vals[8] = read32(a); a += 4; }
                r[13] = a;
                for (int i = 0; i < 8; i++) if (list & (1 << i)) r[i] = vals[i];
                if (insn & 0x100) {
                    if (core >= CORE_ARMV5) writePcFromLoad(vals[8]);
                    else setPC(vals[8]);
                }
                return;
            }
            if (sub == 0xE) {  // BKPT
                cp15_ifsr = 2;
                exception(0x0C, MODE_ABT, pc, false);
                return;
            }
            undefined();
            return;
        }
        case 0x18: case 0x19: {  // stmia/ldmia
            uint32_t rn = (insn >> 8) & 7, list = insn & 0xFF, a = r[rn];
            if (insn & (1 << 11)) {
                uint32_t vals[8];
                for (int i = 0; i < 8; i++) if (list & (1 << i)) { vals[i] = read32(a); a += 4; }
                if (!(list & (1 << rn))) r[rn] = a;
                for (int i = 0; i < 8; i++) if (list & (1 << i)) r[i] = vals[i];
            } else {
                for (int i = 0; i < 8; i++) if (list & (1 << i)) { write32(a, r[i]); a += 4; }
                r[rn] = a;
            }
            return;
        }
        case 0x1A: case 0x1B: {  // conditional branch / swi
            uint32_t cond = (insn >> 8) & 0xF;
            if (cond == 0xF) { exception(0x08, MODE_SVC, pc - 2, false); return; }
            if (cond == 0xE) { undefined(); return; }
            if (condPass(cond)) {
                int32_t off = (int32_t)(int8_t)(insn & 0xFF) * 2;
                r[15] = pc + off;
                branched = true;
            }
            return;
        }
        case 0x1C: {
            int32_t off = ((int32_t)(insn << 21) >> 20);
            r[15] = pc + off;
            branched = true;
            return;
        }
        case 0x1D: {  // BLX suffix
            if (core < CORE_ARMV5 || (insn & 1)) { undefined(); return; }
            uint32_t target = (r[14] + ((insn & 0x7FF) << 1)) & ~3u;
            r[14] = (pc - 2) | 1;
            fT = 0;
            r[15] = target;
            branched = true;
            return;
        }
        case 0x1E: {  // BL prefix
            int32_t off = ((int32_t)(insn << 21) >> 9);
            r[14] = pc + off;
            return;
        }
        case 0x1F: {  // BL suffix
            uint32_t target = r[14] + ((insn & 0x7FF) << 1);
            r[14] = (pc - 2) | 1;
            r[15] = target & ~1u;
            branched = true;
            return;
        }
    }
}

}  // namespace wm
