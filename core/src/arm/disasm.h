// Compact ARMv5TE / Thumb disassembler for traces and the debug bridge.
#pragma once
#include <cstdint>
#include <string>

namespace wm {
std::string disasmArm(uint32_t pc, uint32_t insn);
std::string disasmThumb(uint32_t pc, uint16_t insn);
}
