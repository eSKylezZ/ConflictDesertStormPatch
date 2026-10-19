#pragma once
// Compact 32-bit x86 instruction length decoder for the hooks broker (HookFunction moves whole instructions into a
// trampoline). Table-driven; covers the general-purpose set, x87, MMX/SSE (0F, 0F 38, 0F 3A) and 3DNow!. VEX / EVEX /
// XOP encodings (C4 / C5 / 62 with mod = 3, 8F with reg != 0) are reported as undecodable - none occur in 2002 code.
#include <cstdint>

namespace pse::x86 {

struct Insn {
    uint32_t len = 0;        // total bytes, 0 = undecodable
    uint32_t prefixLen = 0;  // legacy prefixes before the opcode
    uint8_t escape = 0;      // 0 one-byte map, 0x0F two-byte, 0x38 = 0F 38, 0x3A = 0F 3A
    uint8_t opcode = 0;      // last opcode byte
    bool hasModrm = false;
    uint8_t modrm = 0;
    bool opsize16 = false;   // 66 prefix
    bool addr16 = false;     // 67 prefix
    uint32_t dispLen = 0;    // ModRM displacement or moffs
    uint32_t immLen = 0;     // immediates (incl. rel / far pointer)
    uint32_t relLen = 0;     // relative branch displacement size (1, 2, 4), 0 = not a relative branch
};

// Decodes one instruction from `code`, reading at most `max` bytes (and never more than 15). Returns its length,
// 0 when it is undecodable or does not fit in `max`.
uint32_t Decode(const uint8_t* code, uint32_t max, Insn* out);

inline uint32_t Length(const uint8_t* code, uint32_t max = 15) {
    Insn insn;
    return Decode(code, max, &insn);
}

}  // namespace pse::x86
