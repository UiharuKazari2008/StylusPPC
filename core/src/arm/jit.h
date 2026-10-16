// ARM (A32) -> AArch64 dynamic recompiler.
#pragma once
#include <cstdint>

namespace wm {

class Cpu;
class Jit;

// Why a translated block returned to the dispatcher. r15 holds the value described.
enum JitExit : uint32_t {
    JIT_NEXT = 1,     // r15 = next instruction (ARM state unless an interpreted helper changed T)
    JIT_BX = 2,       // r15 = branch target with interworking bit 0
    JIT_LOADPC = 3,   // r15 = value loaded into PC (interworks on ARMv5)
    JIT_ABORT = 4,    // r15 = address of the instruction that took a data abort
    JIT_INTERP = 5,   // r15 = instruction the interpreter must execute
    JIT_IDLE = 6,     // r15 = start of an OAL idle loop whose exit condition is not met
    JIT_STOP = 7,     // r15 = next instruction; device or code-page side effects need attention
};

Jit *jitCreate(Cpu &cpu);
void jitDestroy(Jit *jit);
void jitFlush(Jit *jit);
void jitSetMaxBlock(int n);  // debug
void jitInvalidatePage(Jit *jit, uint32_t ppn);
// Called for writes to a page holding translated code. Returns true if translated code was hit
// (its blocks are then discarded).
bool jitCodeWrite(Jit *jit, uint32_t pa, uint32_t size);

// Runs one translated block at the current pc. Returns false when the next instruction has to be
// executed by the interpreter (Thumb, untranslatable, fetch fault, or block longer than budget).
bool jitRun(Jit *jit, uint64_t budget, uint32_t &reason, uint32_t &executed);

struct JitStats {
    uint64_t blocksCompiled, blocksRun, flushes, codeBytes, interpExits, helperCalls, invalidations, chains;
};
JitStats jitStats(Jit *jit);
void jitProfileInterp(Jit *jit, bool on);  // debug: histogram of instructions left to the interpreter
void jitProfileHelpers(bool on);  // debug: histogram of instructions run by jitInterp inside blocks
void jitDumpInterp(Jit *jit);
void jitDumpMap(Jit *jit, const char *path);  // debug: host code address -> guest pc of each live block

}  // namespace wm
