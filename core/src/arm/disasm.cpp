#include "disasm.h"
#include <cstdarg>
#include <cstdio>

namespace wm {

static const char *kCond[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                "hi", "ls", "ge", "lt", "gt", "le", "",   "nv"};
static const char *kReg[16] = {"r0", "r1", "r2",  "r3",  "r4",  "r5", "r6", "r7",
                               "r8", "r9", "r10", "r11", "r12", "sp", "lr", "pc"};
static const char *kShift[4] = {"lsl", "lsr", "asr", "ror"};
static const char *kDp[16] = {"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
                              "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};

static std::string fmt(const char *f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char *f, ...) {
    char buf[160];
    va_list ap;
    va_start(ap, f);
    vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    return buf;
}

static std::string regList(uint32_t list) {
    std::string s = "{";
    bool first = true;
    for (int i = 0; i < 16; i++) {
        if (!(list & (1 << i))) continue;
        if (!first) s += ",";
        s += kReg[i];
        first = false;
    }
    return s + "}";
}

static std::string shifted(uint32_t insn) {
    uint32_t rm = insn & 0xF, type = (insn >> 5) & 3;
    if (insn & 0x10) return fmt("%s, %s %s", kReg[rm], kShift[type], kReg[(insn >> 8) & 0xF]);
    uint32_t s = (insn >> 7) & 0x1F;
    if (s == 0 && type == 0) return kReg[rm];
    if (s == 0 && type == 3) return fmt("%s, rrx", kReg[rm]);
    return fmt("%s, %s #%u", kReg[rm], kShift[type], s ? s : 32);
}

std::string disasmArm(uint32_t pc, uint32_t insn) {
    const char *c = kCond[insn >> 28];
    uint32_t rn = (insn >> 16) & 0xF, rd = (insn >> 12) & 0xF;
    if ((insn >> 28) == 0xF) {
        if ((insn & 0x0E000000) == 0x0A000000) {
            int32_t off = ((int32_t)(insn << 8) >> 6) | ((insn >> 23) & 2);
            return fmt("blx #0x%08x", pc + 8 + off);
        }
        if ((insn & 0x0D70F000) == 0x0550F000) return "pld";
        return fmt(".word 0x%08x", insn);
    }
    switch ((insn >> 25) & 7) {
        case 0:
        case 1: {
            if ((insn & 0x0FFFFFF0) == 0x012FFF10) return fmt("bx%s %s", c, kReg[insn & 0xF]);
            if ((insn & 0x0FFFFFF0) == 0x012FFF30) return fmt("blx%s %s", c, kReg[insn & 0xF]);
            if ((insn & 0x0FFF0FF0) == 0x016F0F10) return fmt("clz%s %s, %s", c, kReg[rd], kReg[insn & 0xF]);
            if ((insn & 0x0FBF0FFF) == 0x010F0000) return fmt("mrs%s %s, %s", c, kReg[rd], (insn & (1 << 22)) ? "spsr" : "cpsr");
            if ((insn & 0x0DB0F000) == 0x0120F000) {
                std::string f;
                if (insn & (1 << 16)) f += "c";
                if (insn & (1 << 17)) f += "x";
                if (insn & (1 << 18)) f += "s";
                if (insn & (1 << 19)) f += "f";
                const char *psr = (insn & (1 << 22)) ? "spsr" : "cpsr";
                if (insn & (1 << 25)) {
                    uint32_t imm = insn & 0xFF, rot = (insn >> 7) & 0x1E;
                    imm = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
                    return fmt("msr%s %s_%s, #0x%x", c, psr, f.c_str(), imm);
                }
                return fmt("msr%s %s_%s, %s", c, psr, f.c_str(), kReg[insn & 0xF]);
            }
            if (!(insn & (1 << 25)) && (insn & 0xF0) == 0x90) {
                if ((insn & 0x0FC000F0) == 0x00000090) {
                    if (insn & (1 << 21))
                        return fmt("mla%s%s %s, %s, %s, %s", c, (insn & (1 << 20)) ? "s" : "", kReg[rn], kReg[insn & 0xF],
                                   kReg[(insn >> 8) & 0xF], kReg[rd]);
                    return fmt("mul%s%s %s, %s, %s", c, (insn & (1 << 20)) ? "s" : "", kReg[rn], kReg[insn & 0xF],
                               kReg[(insn >> 8) & 0xF]);
                }
                if ((insn & 0x0F8000F0) == 0x00800090) {
                    static const char *n[4] = {"umull", "umlal", "smull", "smlal"};
                    return fmt("%s%s %s, %s, %s, %s", n[(insn >> 21) & 3], c, kReg[rd], kReg[rn], kReg[insn & 0xF],
                               kReg[(insn >> 8) & 0xF]);
                }
                if ((insn & 0x0FB00FF0) == 0x01000090)
                    return fmt("swp%s%s %s, %s, [%s]", c, (insn & (1 << 22)) ? "b" : "", kReg[rd], kReg[insn & 0xF], kReg[rn]);
            }
            if (!(insn & (1 << 25)) && (insn & 0x90) == 0x90) {
                static const char *n[2][4] = {{"?", "strh", "ldrd", "strd"}, {"?", "ldrh", "ldrsb", "ldrsh"}};
                bool L = insn & (1 << 20), P = insn & (1 << 24), U = insn & (1 << 23), W = insn & (1 << 21);
                const char *op = n[L][(insn >> 5) & 3];
                std::string off = (insn & (1 << 22)) ? fmt("#%s%u", U ? "" : "-", ((insn >> 4) & 0xF0) | (insn & 0xF))
                                                     : fmt("%s%s", U ? "" : "-", kReg[insn & 0xF]);
                if (P) return fmt("%s%s %s, [%s, %s]%s", op, c, kReg[rd], kReg[rn], off.c_str(), W ? "!" : "");
                return fmt("%s%s %s, [%s], %s", op, c, kReg[rd], kReg[rn], off.c_str());
            }
            uint32_t op = (insn >> 21) & 0xF;
            bool S = insn & (1 << 20);
            std::string op2;
            if (insn & (1 << 25)) {
                uint32_t imm = insn & 0xFF, rot = (insn >> 7) & 0x1E;
                imm = rot ? (imm >> rot) | (imm << (32 - rot)) : imm;
                op2 = fmt("#0x%x", imm);
            } else {
                op2 = shifted(insn);
            }
            if (op >= 8 && op <= 11) return fmt("%s%s %s, %s", kDp[op], c, kReg[rn], op2.c_str());
            if (op == 13 || op == 15) return fmt("%s%s%s %s, %s", kDp[op], c, S ? "s" : "", kReg[rd], op2.c_str());
            return fmt("%s%s%s %s, %s, %s", kDp[op], c, S ? "s" : "", kReg[rd], kReg[rn], op2.c_str());
        }
        case 2:
        case 3: {
            if ((insn & 0x02000010) == 0x02000010) return fmt(".undef 0x%08x", insn);
            bool L = insn & (1 << 20), B = insn & (1 << 22), P = insn & (1 << 24), U = insn & (1 << 23),
                 W = insn & (1 << 21);
            std::string off = (insn & (1 << 25)) ? fmt("%s%s", U ? "" : "-", shifted(insn & ~0x10u).c_str())
                                                 : fmt("#%s0x%x", U ? "" : "-", insn & 0xFFF);
            const char *op = L ? (B ? "ldrb" : "ldr") : (B ? "strb" : "str");
            if (rn == 15 && !(insn & (1 << 25)) && P) {
                uint32_t a = pc + 8 + (U ? (insn & 0xFFF) : -(insn & 0xFFF));
                return fmt("%s%s %s, [pc, %s] ; =0x%08x", op, c, kReg[rd], off.c_str(), a);
            }
            if (P) return fmt("%s%s %s, [%s, %s]%s", op, c, kReg[rd], kReg[rn], off.c_str(), W ? "!" : "");
            return fmt("%s%s%s %s, [%s], %s", op, c, W ? "t" : "", kReg[rd], kReg[rn], off.c_str());
        }
        case 4: {
            static const char *mode[4] = {"da", "ia", "db", "ib"};
            bool L = insn & (1 << 20);
            uint32_t m = ((insn >> 23) & 1) | (((insn >> 24) & 1) << 1);
            return fmt("%s%s%s %s%s, %s%s", L ? "ldm" : "stm", c, mode[m], kReg[rn], (insn & (1 << 21)) ? "!" : "",
                       regList(insn & 0xFFFF).c_str(), (insn & (1 << 22)) ? "^" : "");
        }
        case 5: {
            int32_t off = (int32_t)(insn << 8) >> 6;
            return fmt("b%s%s #0x%08x", (insn & (1 << 24)) ? "l" : "", c, pc + 8 + off);
        }
        case 6: return fmt("ldc/stc%s 0x%08x", c, insn);
        case 7:
            if (insn & (1 << 24)) return fmt("swi%s #0x%x", c, insn & 0xFFFFFF);
            if (insn & 0x10)
                return fmt("%s%s p%u, %u, %s, c%u, c%u, %u", (insn & (1 << 20)) ? "mrc" : "mcr", c, (insn >> 8) & 0xF,
                           (insn >> 21) & 7, kReg[rd], rn, insn & 0xF, (insn >> 5) & 7);
            return fmt("cdp%s 0x%08x", c, insn);
    }
    return "?";
}

std::string disasmThumb(uint32_t pc, uint16_t insn) {
    uint32_t rd = insn & 7, rs = (insn >> 3) & 7;
    switch (insn >> 11) {
        case 0x00: return fmt("lsls %s, %s, #%u", kReg[rd], kReg[rs], (insn >> 6) & 0x1F);
        case 0x01: return fmt("lsrs %s, %s, #%u", kReg[rd], kReg[rs], (insn >> 6) & 0x1F);
        case 0x02: return fmt("asrs %s, %s, #%u", kReg[rd], kReg[rs], (insn >> 6) & 0x1F);
        case 0x03:
            if (insn & (1 << 10))
                return fmt("%s %s, %s, #%u", (insn & (1 << 9)) ? "subs" : "adds", kReg[rd], kReg[rs], (insn >> 6) & 7);
            return fmt("%s %s, %s, %s", (insn & (1 << 9)) ? "subs" : "adds", kReg[rd], kReg[rs], kReg[(insn >> 6) & 7]);
        case 0x04: return fmt("movs %s, #%u", kReg[(insn >> 8) & 7], insn & 0xFF);
        case 0x05: return fmt("cmp %s, #%u", kReg[(insn >> 8) & 7], insn & 0xFF);
        case 0x06: return fmt("adds %s, #%u", kReg[(insn >> 8) & 7], insn & 0xFF);
        case 0x07: return fmt("subs %s, #%u", kReg[(insn >> 8) & 7], insn & 0xFF);
        case 0x08:
            if (!(insn & 0x400)) {
                static const char *n[16] = {"ands", "eors", "lsls", "lsrs", "asrs", "adcs", "sbcs", "rors",
                                            "tst",  "negs", "cmp",  "cmn",  "orrs", "muls", "bics", "mvns"};
                return fmt("%s %s, %s", n[(insn >> 6) & 0xF], kReg[rd], kReg[rs]);
            } else {
                uint32_t d = (insn & 7) | ((insn >> 4) & 8), m = (insn >> 3) & 0xF;
                switch ((insn >> 8) & 3) {
                    case 0: return fmt("add %s, %s", kReg[d], kReg[m]);
                    case 1: return fmt("cmp %s, %s", kReg[d], kReg[m]);
                    case 2: return fmt("mov %s, %s", kReg[d], kReg[m]);
                    default: return fmt("%s %s", (insn & 0x80) ? "blx" : "bx", kReg[m]);
                }
            }
        case 0x09:
            return fmt("ldr %s, [pc, #%u] ; =0x%08x", kReg[(insn >> 8) & 7], (insn & 0xFF) * 4,
                       ((pc + 4) & ~3u) + (insn & 0xFF) * 4);
        case 0x0A:
        case 0x0B: {
            static const char *n[8] = {"str", "strh", "strb", "ldrsb", "ldr", "ldrh", "ldrb", "ldrsh"};
            return fmt("%s %s, [%s, %s]", n[(insn >> 9) & 7], kReg[rd], kReg[rs], kReg[(insn >> 6) & 7]);
        }
        case 0x0C: return fmt("str %s, [%s, #%u]", kReg[rd], kReg[rs], ((insn >> 6) & 0x1F) * 4);
        case 0x0D: return fmt("ldr %s, [%s, #%u]", kReg[rd], kReg[rs], ((insn >> 6) & 0x1F) * 4);
        case 0x0E: return fmt("strb %s, [%s, #%u]", kReg[rd], kReg[rs], (insn >> 6) & 0x1F);
        case 0x0F: return fmt("ldrb %s, [%s, #%u]", kReg[rd], kReg[rs], (insn >> 6) & 0x1F);
        case 0x10: return fmt("strh %s, [%s, #%u]", kReg[rd], kReg[rs], ((insn >> 6) & 0x1F) * 2);
        case 0x11: return fmt("ldrh %s, [%s, #%u]", kReg[rd], kReg[rs], ((insn >> 6) & 0x1F) * 2);
        case 0x12: return fmt("str %s, [sp, #%u]", kReg[(insn >> 8) & 7], (insn & 0xFF) * 4);
        case 0x13: return fmt("ldr %s, [sp, #%u]", kReg[(insn >> 8) & 7], (insn & 0xFF) * 4);
        case 0x14: return fmt("add %s, pc, #%u", kReg[(insn >> 8) & 7], (insn & 0xFF) * 4);
        case 0x15: return fmt("add %s, sp, #%u", kReg[(insn >> 8) & 7], (insn & 0xFF) * 4);
        case 0x16:
        case 0x17: {
            uint32_t sub = (insn >> 8) & 0xF;
            if (sub == 0) return fmt("%s sp, #%u", (insn & 0x80) ? "sub" : "add", (insn & 0x7F) * 4);
            if (sub == 4 || sub == 5) return fmt("push %s", regList((insn & 0xFF) | ((insn & 0x100) ? 0x4000 : 0)).c_str());
            if (sub == 0xC || sub == 0xD) return fmt("pop %s", regList((insn & 0xFF) | ((insn & 0x100) ? 0x8000 : 0)).c_str());
            if (sub == 0xE) return fmt("bkpt #%u", insn & 0xFF);
            return fmt(".hword 0x%04x", insn);
        }
        case 0x18: return fmt("stmia %s!, %s", kReg[(insn >> 8) & 7], regList(insn & 0xFF).c_str());
        case 0x19: return fmt("ldmia %s!, %s", kReg[(insn >> 8) & 7], regList(insn & 0xFF).c_str());
        case 0x1A:
        case 0x1B: {
            uint32_t cond = (insn >> 8) & 0xF;
            if (cond == 0xF) return fmt("swi #%u", insn & 0xFF);
            return fmt("b%s #0x%08x", kCond[cond], pc + 4 + (int32_t)(int8_t)(insn & 0xFF) * 2);
        }
        case 0x1C: return fmt("b #0x%08x", pc + 4 + ((int32_t)(insn << 21) >> 20));
        case 0x1D: return fmt("blx(suffix) #0x%x", (insn & 0x7FF) << 1);
        case 0x1E: return fmt("bl(prefix) #0x%x", (uint32_t)((int32_t)(insn << 21) >> 9));
        case 0x1F: return fmt("bl(suffix) #0x%x", (insn & 0x7FF) << 1);
    }
    return "?";
}

}  // namespace wm
