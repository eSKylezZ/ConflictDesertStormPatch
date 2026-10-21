#pragma once
// mission2lua (C++ port) - x86 instruction decoding in capstone's terms (docs/internal/mission-translation.md "C++ port").
//
// The Python generator (ModKit conflict_ds_tools/mission2lua.py + missionflow.py) is built on capstone 5: its
// mnemonics ("je", "rep movsd", "repne scasb"), its Intel operand text ("dword ptr [esp + 0x20]", "push -0xa") and its
// operand details (types, sizes, immediate values, imm / disp offsets). The generated Lua carries the disassembly as a
// comment above every instruction, so a byte-identical port needs exactly that view. Decoding is done by Zydis
// (third_party/zydis, MIT); this file turns Zydis' result into capstone's view. It covers what the mission DLLs need -
// the 32-bit integer + x87 subset of MSVC 6 code - and was checked against capstone at every byte offset of the 18
// mission DLLs' code sections (pse_build missions --dump, compare with the Python dump - mission-translation.md).
#include <cstdint>
#include <string>

namespace pse::m2l {

// Registers: the general ones get fixed ids (the emitter maps them to EAX..ESP); everything else keeps Zydis' id + 0x100
// so it still has a name.
enum Reg : uint16_t {
    R_NONE = 0,
    R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI,      // 1..8
    R_AX, R_CX, R_DX, R_BX, R_SP, R_BP, R_SI, R_DI,              // 9..16
    R_AL, R_CL, R_DL, R_BL, R_AH, R_CH, R_DH, R_BH,              // 17..24
    R_ES, R_CS, R_SS, R_DS, R_FS, R_GS,                          // 25..30
    R_ST0, R_ST1, R_ST2, R_ST3, R_ST4, R_ST5, R_ST6, R_ST7,      // 31..38
    R_OTHER = 0x100,
};
const char* RegName(uint16_t reg);       // capstone's name ("eax", "st(1)") or Zydis' for other registers
bool IsReg32(uint16_t r);
bool IsReg16(uint16_t r);
bool IsLow8(uint16_t r);
bool IsHigh8(uint16_t r);
uint16_t Full32(uint16_t r);             // al / ax / ah -> eax; R_NONE for anything else

enum class OpType : uint8_t { None = 0, Reg = 1, Imm = 2, Mem = 3 };

struct Operand {
    OpType type = OpType::None;
    uint8_t size = 0;          // bytes (capstone's op.size)
    uint16_t reg = R_NONE;     // Reg
    int64_t imm = 0;           // capstone's op.imm (sign as capstone gives it)
    uint16_t seg = R_NONE, base = R_NONE, index = R_NONE;   // Mem (seg only when capstone reports it)
    int scale = 1;
    int64_t disp = 0;
};

struct Insn {
    uint32_t address = 0;
    uint8_t size = 0;
    uint8_t bytes[16] = {};
    std::string mnemonic;      // capstone's (prefix included: "rep movsd")
    std::string op_str;        // capstone's Intel operand text
    Operand ops[4];
    uint8_t nops = 0;
    uint8_t imm_offset = 0;    // capstone's x86.imm_offset / disp_offset (0 = none)
    uint8_t disp_offset = 0;
};

// Decodes one instruction at `address` from up to `len` bytes. False where capstone 5 fails to decode.
bool Decode(const uint8_t* code, size_t len, uint32_t address, Insn* out);

// "add 4|add|eax, 4|r4:eax,i4:4|2|0" - the line format of the reference dump (scratchpad csdump.py): size, mnemonic,
// op_str, operands, imm_offset, disp_offset. Used by `pse_build missions --dump`.
std::string DumpLine(const Insn& i);

}  // namespace pse::m2l
