// mission2lua (C++ port): Zydis -> capstone's view of an instruction (m2l_decode.h).
#include "m2l_decode.h"

#include <cstdio>
#include <cstring>

#include "../../../third_party/zydis/Zydis.h"

namespace pse::m2l {

namespace {

const char* const kNames[] = {"",      "eax",   "ecx",   "edx",   "ebx",   "esp",   "ebp",   "esi",   "edi",   "ax",
                              "cx",    "dx",    "bx",    "sp",    "bp",    "si",    "di",    "al",    "cl",    "dl",
                              "bl",    "ah",    "ch",    "dh",    "bh",    "es",    "cs",    "ss",    "ds",    "fs",
                              "gs",    "st(0)", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"};

uint16_t FromZydis(ZydisRegister r) {
    switch (r) {
    case ZYDIS_REGISTER_NONE: return R_NONE;
    case ZYDIS_REGISTER_EAX: return R_EAX;
    case ZYDIS_REGISTER_ECX: return R_ECX;
    case ZYDIS_REGISTER_EDX: return R_EDX;
    case ZYDIS_REGISTER_EBX: return R_EBX;
    case ZYDIS_REGISTER_ESP: return R_ESP;
    case ZYDIS_REGISTER_EBP: return R_EBP;
    case ZYDIS_REGISTER_ESI: return R_ESI;
    case ZYDIS_REGISTER_EDI: return R_EDI;
    case ZYDIS_REGISTER_AX: return R_AX;
    case ZYDIS_REGISTER_CX: return R_CX;
    case ZYDIS_REGISTER_DX: return R_DX;
    case ZYDIS_REGISTER_BX: return R_BX;
    case ZYDIS_REGISTER_SP: return R_SP;
    case ZYDIS_REGISTER_BP: return R_BP;
    case ZYDIS_REGISTER_SI: return R_SI;
    case ZYDIS_REGISTER_DI: return R_DI;
    case ZYDIS_REGISTER_AL: return R_AL;
    case ZYDIS_REGISTER_CL: return R_CL;
    case ZYDIS_REGISTER_DL: return R_DL;
    case ZYDIS_REGISTER_BL: return R_BL;
    case ZYDIS_REGISTER_AH: return R_AH;
    case ZYDIS_REGISTER_CH: return R_CH;
    case ZYDIS_REGISTER_DH: return R_DH;
    case ZYDIS_REGISTER_BH: return R_BH;
    case ZYDIS_REGISTER_ES: return R_ES;
    case ZYDIS_REGISTER_CS: return R_CS;
    case ZYDIS_REGISTER_SS: return R_SS;
    case ZYDIS_REGISTER_DS: return R_DS;
    case ZYDIS_REGISTER_FS: return R_FS;
    case ZYDIS_REGISTER_GS: return R_GS;
    default:
        if (r >= ZYDIS_REGISTER_ST0 && r <= ZYDIS_REGISTER_ST7) return static_cast<uint16_t>(static_cast<int>(R_ST0) + (static_cast<int>(r) - static_cast<int>(ZYDIS_REGISTER_ST0)));
        return static_cast<uint16_t>(static_cast<int>(R_OTHER) + static_cast<int>(r));
    }
}

void AppendHex(std::string* s, uint64_t v) {
    char b[32];
    std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
    *s += b;
}
// capstone's printImm with positive = true: <= 9 decimal, else hex
void AppendPos(std::string* s, uint64_t v) {
    if (v > 9) {
        AppendHex(s, v);
    } else {
        *s += static_cast<char>('0' + v);
    }
}
// capstone's default immediate: signed, "-0x10" / "-8"
void AppendSigned(std::string* s, int64_t v) {
    if (v >= 0) {
        AppendPos(s, static_cast<uint64_t>(v));
    } else {
        *s += '-';
        AppendPos(s, static_cast<uint64_t>(-v));
    }
}

const char* PtrName(int size) {
    switch (size) {
    case 1: return "byte ptr ";
    case 2: return "word ptr ";
    case 4: return "dword ptr ";
    case 6: return "fword ptr ";
    case 8: return "qword ptr ";
    case 10: return "xword ptr ";
    case 16: return "xmmword ptr ";
    case 32: return "ymmword ptr ";
    default: return "";
    }
}

std::string FormatMem(const Operand& o, bool ptr) {
    std::string s;
    if (ptr) s += PtrName(o.size);
    if (o.seg) {
        s += RegName(o.seg);
        s += ':';
    }
    s += '[';
    bool need = false;
    if (o.base) {
        s += RegName(o.base);
        need = true;
    }
    if (o.index) {
        if (need) s += " + ";
        s += RegName(o.index);
        if (o.scale != 1) {
            s += '*';
            s += static_cast<char>('0' + o.scale);
        }
        need = true;
    }
    if (o.disp) {
        if (need) {
            if (o.disp < 0) {
                s += " - ";
                AppendPos(&s, static_cast<uint64_t>(-o.disp));
            } else {
                s += " + ";
                AppendPos(&s, static_cast<uint64_t>(o.disp));
            }
        } else {
            AppendPos(&s, static_cast<uint64_t>(o.disp) & 0xFFFFFFFFull);
        }
    } else if (!need) {
        s += '0';
    }
    s += ']';
    return s;
}

// Zydis mnemonic -> capstone's
std::string MnemonicName(const ZydisDecodedInstruction& in) {
    std::string m = ZydisMnemonicGetString(in.mnemonic);
    static const struct {
        const char* z;
        const char* c;
    } kMap[] = {{"jz", "je"},          {"jnz", "jne"},         {"jnb", "jae"},         {"jnbe", "ja"},
                {"jnl", "jge"},        {"jnle", "jg"},         {"setz", "sete"},       {"setnz", "setne"},
                {"setnb", "setae"},    {"setnbe", "seta"},     {"setnl", "setge"},     {"setnle", "setg"},
                {"cmovz", "cmove"},    {"cmovnz", "cmovne"},   {"cmovnb", "cmovae"},   {"cmovnbe", "cmova"},
                {"cmovnl", "cmovge"},  {"cmovnle", "cmovg"},   {"fwait", "wait"},      {"pushad", "pushal"},
                {"popad", "popal"},    {"loopz", "loope"},     {"loopnz", "loopne"},   {"fcmovnb", "fcmovnb"}};
    for (const auto& e : kMap)
        if (m == e.z) {
            m = e.c;
            break;
        }
    return m;
}

bool IsStringOp(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
    case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD:
    case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
    case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD:
    case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
    case ZYDIS_MNEMONIC_INSB: case ZYDIS_MNEMONIC_INSW: case ZYDIS_MNEMONIC_INSD:
    case ZYDIS_MNEMONIC_OUTSB: case ZYDIS_MNEMONIC_OUTSW: case ZYDIS_MNEMONIC_OUTSD:
        return true;
    default:
        return false;
    }
}

bool IsShift(ZydisMnemonic m) {
    switch (m) {
    case ZYDIS_MNEMONIC_SHL: case ZYDIS_MNEMONIC_SHR: case ZYDIS_MNEMONIC_SAR: case ZYDIS_MNEMONIC_ROL:
    case ZYDIS_MNEMONIC_ROR: case ZYDIS_MNEMONIC_RCL: case ZYDIS_MNEMONIC_RCR:
        return true;
    default:
        return false;
    }
}

uint64_t SizeMask(int bytes) { return bytes >= 8 ? ~0ull : (1ull << (bytes * 8)) - 1; }

bool StartsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

}  // namespace

const char* RegName(uint16_t reg) {
    if (reg < sizeof kNames / sizeof kNames[0]) return kNames[reg];
    if (reg >= R_OTHER) return ZydisRegisterGetString(static_cast<ZydisRegister>(reg - R_OTHER));
    return "?";
}
bool IsReg32(uint16_t r) { return r >= R_EAX && r <= R_EDI; }
bool IsReg16(uint16_t r) { return r >= R_AX && r <= R_DI; }
bool IsLow8(uint16_t r) { return r >= R_AL && r <= R_BL; }
bool IsHigh8(uint16_t r) { return r >= R_AH && r <= R_BH; }
uint16_t Full32(uint16_t r) {
    if (IsReg32(r)) return r;
    if (IsReg16(r)) return static_cast<uint16_t>(r - R_AX + R_EAX);
    if (IsLow8(r)) return static_cast<uint16_t>(r - R_AL + R_EAX);
    if (IsHigh8(r)) return static_cast<uint16_t>(r - R_AH + R_EAX);
    return R_NONE;
}

namespace {
bool DecodeOne(const uint8_t* code, size_t len, uint32_t address, Insn* out) {
    static const ZydisDecoder decoder = [] {
        ZydisDecoder d;
        ZydisDecoderInit(&d, ZYDIS_MACHINE_MODE_LEGACY_32, ZYDIS_STACK_WIDTH_32);
        return d;
    }();
    ZydisDecodedInstruction in;
    ZydisDecodedOperand zo[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, len > 16 ? 16 : len, &in, zo))) return false;

    Insn& i = *out;
    {
        size_t q = 0;
        while (q + 1 < len && (code[q] == 0x66 || code[q] == 0x67 || code[q] == 0xF0 || code[q] == 0xF2 ||
                               code[q] == 0xF3 || code[q] == 0x2E || code[q] == 0x36 || code[q] == 0x3E ||
                               code[q] == 0x26 || code[q] == 0x64 || code[q] == 0x65))
            ++q;
        if (q + 1 < len && code[q] == 0xDC && code[q + 1] >= 0xD0 && code[q + 1] <= 0xDF) return false;
    }
    i = Insn();
    i.address = address;
    i.size = in.length;
    std::memcpy(i.bytes, code, in.length);
    i.mnemonic = MnemonicName(in);
    i.disp_offset = in.raw.disp.size ? in.raw.disp.offset : 0;
    i.imm_offset = in.raw.imm[0].size ? in.raw.imm[0].offset : 0;

    const uint64_t attr = in.attributes;
    const int opw = in.operand_width / 8;   // operand size in bytes
    // primary opcode byte + ModRM (after the legacy prefixes); the segment prefix as written (last one wins)
    int pk = 0;
    uint16_t segPrefix = R_NONE;
    int group1 = 0;   // last of F0 / F2 / F3 (capstone shows only that one)
    while (pk < in.length && (code[pk] == 0x66 || code[pk] == 0x67 || code[pk] == 0xF0 || code[pk] == 0xF2 ||
                              code[pk] == 0xF3 || code[pk] == 0x2E || code[pk] == 0x36 || code[pk] == 0x3E ||
                              code[pk] == 0x26 || code[pk] == 0x64 || code[pk] == 0x65)) {
        switch (code[pk]) {
        case 0x2E: segPrefix = R_CS; break;
        case 0x36: segPrefix = R_SS; break;
        case 0x3E: segPrefix = R_DS; break;
        case 0x26: segPrefix = R_ES; break;
        case 0x64: segPrefix = R_FS; break;
        case 0x65: segPrefix = R_GS; break;
        case 0xF0: case 0xF2: case 0xF3: group1 = code[pk]; break;
        default: break;
        }
        ++pk;
    }
    const int opc = pk < in.length ? code[pk] : -1;
    const int modrm = in.raw.modrm.offset ? code[in.raw.modrm.offset] : (pk + 1 < in.length ? code[pk + 1] : 0);
    const int modrmReg = in.raw.modrm.offset ? in.raw.modrm.reg : -1;
    const bool x87Reg = opc >= 0xD8 && opc <= 0xDF && in.raw.modrm.offset && in.raw.modrm.mod == 3;
    std::string stText;
    bool hideCount = false;
    const bool logic = in.mnemonic == ZYDIS_MNEMONIC_AND || in.mnemonic == ZYDIS_MNEMONIC_OR ||
                       in.mnemonic == ZYDIS_MNEMONIC_XOR;

    if (IsStringOp(in.mnemonic)) {
        // capstone shows the implicit operands: es:[edi] destination, [esi] source, al / eax for stos / lods / scas
        const int w = opw;
        const uint16_t acc = w == 1 ? R_AL : w == 2 ? R_AX : R_EAX;
        Operand di, si, a;
        const bool a16 = in.address_width == 16;
        di.type = OpType::Mem, di.size = static_cast<uint8_t>(w), di.seg = R_ES, di.base = a16 ? R_DI : R_EDI;
        si.type = OpType::Mem, si.size = static_cast<uint8_t>(w), si.base = a16 ? R_SI : R_ESI, si.seg = segPrefix;
        a.type = OpType::Reg, a.size = static_cast<uint8_t>(w), a.reg = acc;
        Operand dx;
        dx.type = OpType::Reg, dx.size = 2, dx.reg = R_DX;
        switch (in.mnemonic) {
        case ZYDIS_MNEMONIC_MOVSB: case ZYDIS_MNEMONIC_MOVSW: case ZYDIS_MNEMONIC_MOVSD:
            i.ops[0] = di, i.ops[1] = si, i.nops = 2; break;
        case ZYDIS_MNEMONIC_STOSB: case ZYDIS_MNEMONIC_STOSW: case ZYDIS_MNEMONIC_STOSD:
            i.ops[0] = di, i.ops[1] = a, i.nops = 2; break;
        case ZYDIS_MNEMONIC_LODSB: case ZYDIS_MNEMONIC_LODSW: case ZYDIS_MNEMONIC_LODSD:
            i.ops[0] = a, i.ops[1] = si, i.nops = 2; break;
        case ZYDIS_MNEMONIC_SCASB: case ZYDIS_MNEMONIC_SCASW: case ZYDIS_MNEMONIC_SCASD:
            i.ops[0] = a, i.ops[1] = di, i.nops = 2; break;
        case ZYDIS_MNEMONIC_CMPSB: case ZYDIS_MNEMONIC_CMPSW: case ZYDIS_MNEMONIC_CMPSD:
            i.ops[0] = si, i.ops[1] = di, i.nops = 2; break;
        case ZYDIS_MNEMONIC_INSB: case ZYDIS_MNEMONIC_INSW: case ZYDIS_MNEMONIC_INSD:
            i.ops[0] = di, i.ops[1] = dx, i.nops = 2; break;
        default:   // outs
            i.ops[0] = dx, i.ops[1] = si, i.nops = 2; break;
        }
        if (attr & ZYDIS_ATTRIB_HAS_REPNE) i.mnemonic = "repne " + i.mnemonic;
        else if (attr & ZYDIS_ATTRIB_HAS_REPE) i.mnemonic = "repe " + i.mnemonic;
        else if (attr & ZYDIS_ATTRIB_HAS_REP) i.mnemonic = "rep " + i.mnemonic;
    } else if (x87Reg) {
        // x87 register forms, capstone's way: DC = "st(i), st(0)"; fcmov = "st(0), st(i)"; fxch shows "st(i)" but has
        // both registers as operands; everything else that names a stack register = "st(i)"
        const uint16_t sti = static_cast<uint16_t>(R_ST0 + (modrm & 7));
        bool anySt = false;
        for (int k = 0; k < in.operand_count; ++k)
            if (zo[k].type == ZYDIS_OPERAND_TYPE_REGISTER && zo[k].visibility != ZYDIS_OPERAND_VISIBILITY_HIDDEN &&
                zo[k].reg.value >= ZYDIS_REGISTER_ST0 && zo[k].reg.value <= ZYDIS_REGISTER_ST7)
                anySt = true;
        auto st = [&](uint16_t r) {
            Operand o;
            o.type = OpType::Reg, o.size = 10, o.reg = r;
            return o;
        };
        if (opc == 0xDC && anySt) {
            i.ops[0] = st(sti), i.ops[1] = st(R_ST0), i.nops = 2;
        } else if ((opc == 0xDA || opc == 0xDB) && modrm < 0xE0) {
            i.ops[0] = st(R_ST0), i.ops[1] = st(sti), i.nops = 2;
        } else if (opc == 0xD9 && modrm >= 0xD8 && modrm <= 0xDF) {
            i.ops[0] = st(sti), i.ops[1] = st(R_ST0), i.nops = 2;
        } else if (opc == 0xD9 && modrm >= 0xC8 && modrm <= 0xCF) {
            i.ops[0] = st(R_ST0), i.ops[1] = st(sti), i.nops = 2;
            stText = RegName(sti);
        } else if (anySt) {
            i.ops[0] = st(sti), i.nops = 1;
        } else {
            for (int k = 0; k < in.operand_count && i.nops < 4; ++k)
                if (zo[k].visibility != ZYDIS_OPERAND_VISIBILITY_HIDDEN && zo[k].type == ZYDIS_OPERAND_TYPE_REGISTER) {
                    Operand& o = i.ops[i.nops++];
                    o.type = OpType::Reg, o.size = static_cast<uint8_t>(zo[k].size / 8), o.reg = FromZydis(zo[k].reg.value);
                }
        }
    } else {
        int immSeen = 0;
        for (int k = 0; k < in.operand_count && i.nops < 4; ++k) {
            const ZydisDecodedOperand& z = zo[k];
            if (z.visibility == ZYDIS_OPERAND_VISIBILITY_HIDDEN) continue;
            if (z.type == ZYDIS_OPERAND_TYPE_POINTER) {   // lcall / ljmp seg:offset
                Operand& s = i.ops[i.nops++];
                s.type = OpType::Imm, s.size = 2, s.imm = z.ptr.segment;
                if (i.nops < 4) {
                    Operand& o = i.ops[i.nops++];
                    o.type = OpType::Imm, o.size = 4, o.imm = z.ptr.offset;
                }
                i.imm_offset = static_cast<uint8_t>(in.length - 2);
                continue;
            }
            Operand& o = i.ops[i.nops++];
            o.size = static_cast<uint8_t>(z.size / 8);
            if (z.type == ZYDIS_OPERAND_TYPE_REGISTER) {
                o.type = OpType::Reg;
                o.reg = FromZydis(z.reg.value);
                if ((opc == 0x8C || opc == 0x8E) && IsReg16(o.reg)) {   // mov sreg <-> r32: capstone names the r32
                    o.reg = Full32(o.reg);
                    o.size = 4;
                }
            } else if (z.type == ZYDIS_OPERAND_TYPE_MEMORY) {
                o.type = OpType::Mem;
                o.seg = segPrefix;
                o.base = FromZydis(z.mem.base);
                o.index = FromZydis(z.mem.index);
                o.scale = z.mem.index ? z.mem.scale : (in.raw.sib.offset ? 1 << in.raw.sib.scale : 1);
                o.disp = z.mem.disp.has_displacement ? z.mem.disp.value : 0;
                if (!o.base && !o.index)   // capstone: a ModRM absolute address is signed, a moffs (A0-A3) one not
                    o.disp = (opc >= 0xA0 && opc <= 0xA3) ? (o.disp & 0xFFFFFFFF) : static_cast<int32_t>(o.disp);
            } else if (z.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
                o.type = OpType::Imm;
                const int which = immSeen++;
                if (z.imm.is_relative) {
                    o.imm = static_cast<int64_t>((address + in.length + z.imm.value.u) & 0xFFFFFFFFull);
                    o.size = static_cast<uint8_t>(opw);
                } else if (z.visibility == ZYDIS_OPERAND_VISIBILITY_IMPLICIT) {   // D0 / D1: the count 1
                    // capstone: rcl r/m8 (D0 /2) on memory has an imm operand of size 0 and no ", 1" in the text
                    o.size = static_cast<uint8_t>((opc == 0xD0 || opc == 0xD1) && modrmReg == 2 && in.raw.modrm.mod != 3
                                                      ? 0 : (i.ops[0].size ? i.ops[0].size : opw));
                    if (o.size == 0) hideCount = true;
                    o.imm = 1;
                } else {
                    const int enc = in.raw.imm[which < 2 ? which : 0].size / 8;
                    int sz = opw;
                    if (IsShift(in.mnemonic) || in.mnemonic == ZYDIS_MNEMONIC_INT || in.mnemonic == ZYDIS_MNEMONIC_IN ||
                        in.mnemonic == ZYDIS_MNEMONIC_OUT || in.mnemonic == ZYDIS_MNEMONIC_AAM ||
                        in.mnemonic == ZYDIS_MNEMONIC_AAD)
                        sz = enc;
                    if (in.mnemonic == ZYDIS_MNEMONIC_ENTER || in.mnemonic == ZYDIS_MNEMONIC_RET) sz = 4;
                    o.size = static_cast<uint8_t>(sz);
                    const bool sext = (enc < sz && !IsShift(in.mnemonic) && in.mnemonic != ZYDIS_MNEMONIC_RET) ||
                                      (opc == 0xF6 && enc == 1 && modrmReg == 1);   // test alias F6 /1: signed
                    if (sext) {
                        const int64_t s = enc == 1 ? static_cast<int8_t>(z.imm.value.u)
                                          : enc == 2 ? static_cast<int16_t>(z.imm.value.u)
                                                     : static_cast<int32_t>(z.imm.value.u);
                        o.imm = logic ? static_cast<int64_t>(static_cast<uint64_t>(s) & SizeMask(sz)) : s;
                    } else {
                        o.imm = static_cast<int64_t>(z.imm.value.u & SizeMask(enc ? enc : sz));
                    }
                }
            } else {
                o.type = OpType::None;
            }
        }
        if (in.mnemonic == ZYDIS_MNEMONIC_ENTER && in.raw.imm[1].size) i.imm_offset = in.raw.imm[1].offset;
    }

    if (in.mnemonic == ZYDIS_MNEMONIC_LEA && i.nops == 2) i.ops[1].size = i.ops[0].size;   // capstone: lea's m = dest size

    // mnemonic quirks
    if (in.mnemonic == ZYDIS_MNEMONIC_SHL && modrmReg == 6) i.mnemonic = "sal";
    if (opc == 0xCA || opc == 0xCB) i.mnemonic = "retf";
    if (in.mnemonic == ZYDIS_MNEMONIC_XLAT) i.mnemonic = "xlatb";
    if (in.mnemonic == ZYDIS_MNEMONIC_FUCOMIP) i.mnemonic = "fucompi";
    if (in.mnemonic == ZYDIS_MNEMONIC_FCOMIP) i.mnemonic = "fcompi";
    bool farPtr = false;
    if (zo[0].type == ZYDIS_OPERAND_TYPE_POINTER) {
        i.mnemonic = in.mnemonic == ZYDIS_MNEMONIC_CALL ? "lcall" : "ljmp";
    } else if ((in.mnemonic == ZYDIS_MNEMONIC_CALL || in.mnemonic == ZYDIS_MNEMONIC_JMP) && opc == 0xFF &&
               (modrmReg == 3 || modrmReg == 5)) {
        farPtr = true;   // capstone: "jmp ptr [..]" (m6) / "call ptr [..]" (m4)
        for (int k = 0; k < i.nops; ++k)
            if (i.ops[k].type == OpType::Mem) i.ops[k].size = in.mnemonic == ZYDIS_MNEMONIC_JMP ? 6 : 4;
    }
    switch (in.mnemonic) {
    case ZYDIS_MNEMONIC_LES: case ZYDIS_MNEMONIC_LDS: case ZYDIS_MNEMONIC_LSS: case ZYDIS_MNEMONIC_LFS:
    case ZYDIS_MNEMONIC_LGS:
        farPtr = true;
        break;
    default: break;
    }
    if ((attr & ZYDIS_ATTRIB_HAS_LOCK) && group1 == 0xF0 && !StartsWith(i.mnemonic, "lock "))
        i.mnemonic = "lock " + i.mnemonic;
    if (segPrefix == R_DS && (in.mnemonic == ZYDIS_MNEMONIC_JMP || in.mnemonic == ZYDIS_MNEMONIC_CALL) && !farPtr &&
        zo[0].type != ZYDIS_OPERAND_TYPE_POINTER)
        i.mnemonic = "notrack " + i.mnemonic;
    const bool branch = in.meta.category == ZYDIS_CATEGORY_COND_BR || in.meta.category == ZYDIS_CATEGORY_UNCOND_BR ||
                        in.meta.category == ZYDIS_CATEGORY_CALL || in.meta.category == ZYDIS_CATEGORY_RET;
    if (group1 == 0xF2 && branch && !StartsWith(i.mnemonic, "bnd ")) i.mnemonic = "bnd " + i.mnemonic;

    // operand text
    const bool lea = in.mnemonic == ZYDIS_MNEMONIC_LEA;
    for (int k = 0; k < i.nops; ++k) {
        Operand& o = i.ops[k];
        if (k) i.op_str += (zo[0].type == ZYDIS_OPERAND_TYPE_POINTER && i.mnemonic == "ljmp") ? ":" : ", ";
        switch (o.type) {
        case OpType::Reg: i.op_str += RegName(o.reg); break;
        case OpType::Mem: {
            const char* prefix = nullptr;   // nullptr = by size
            switch (in.mnemonic) {
            case ZYDIS_MNEMONIC_LEA: case ZYDIS_MNEMONIC_SGDT: case ZYDIS_MNEMONIC_SIDT: case ZYDIS_MNEMONIC_LGDT:
            case ZYDIS_MNEMONIC_LIDT: case ZYDIS_MNEMONIC_FLDENV: case ZYDIS_MNEMONIC_FNSTENV:
                prefix = "";
                break;
            case ZYDIS_MNEMONIC_FNSAVE: case ZYDIS_MNEMONIC_FRSTOR:
                o.size = 4;
                break;
            case ZYDIS_MNEMONIC_FBLD: case ZYDIS_MNEMONIC_FBSTP:
                prefix = "tbyte ptr ";
                break;
            default: break;
            }
            if (farPtr) {
                prefix = "ptr ";
                if (in.mnemonic != ZYDIS_MNEMONIC_JMP) o.size = 4;
            }
            std::string s = FormatMem(o, false);
            i.op_str += (prefix ? prefix : PtrName(o.size)) + s;
            break;
        }
        case OpType::Imm:
            if (in.mnemonic == ZYDIS_MNEMONIC_JMP && zo[0].type != ZYDIS_OPERAND_TYPE_POINTER)
                AppendHex(&i.op_str, static_cast<uint64_t>(o.imm) & 0xFFFFFFFFull);
            else if (zo[0].type == ZYDIS_OPERAND_TYPE_POINTER || in.mnemonic == ZYDIS_MNEMONIC_CALL)
                AppendPos(&i.op_str, static_cast<uint64_t>(o.imm) & 0xFFFFFFFFull);
            else if (logic && !(o.imm >= 0 && o.imm <= 9))
                AppendHex(&i.op_str, static_cast<uint64_t>(o.imm) & SizeMask(o.size));
            else
                AppendSigned(&i.op_str, o.imm);
            break;
        default: break;
        }
    }
    if (!stText.empty()) i.op_str = stText;
    if (hideCount) i.op_str = i.op_str.substr(0, i.op_str.rfind(", "));
    (void)lea;
    return true;
}

}  // namespace

bool Decode(const uint8_t* code, size_t len, uint32_t address, Insn* out) {
    const size_t n = len > 16 ? 16 : len;
    size_t pk = 0;
    while (pk < n && (code[pk] == 0x66 || code[pk] == 0x67 || code[pk] == 0xF0 || code[pk] == 0xF2 || code[pk] == 0xF3 ||
                      code[pk] == 0x2E || code[pk] == 0x36 || code[pk] == 0x3E || code[pk] == 0x26 || code[pk] == 0x64 ||
                      code[pk] == 0x65))
        ++pk;
    // capstone: 0F FF = "ud0" with no operands (2 bytes); Zydis reads a ModRM
    if (pk + 1 < n && code[pk] == 0x0F && code[pk + 1] == 0xFF) {
        Insn& i = *out;
        i = Insn();
        i.address = address;
        i.size = static_cast<uint8_t>(pk + 2);
        std::memcpy(i.bytes, code, i.size);
        i.mnemonic = "ud0";
        return true;
    }
    if (DecodeOne(code, n, address, out)) return true;
    uint8_t buf[16];
    // capstone accepts a lock prefix on any instruction (Zydis refuses it where the CPU would fault)
    for (size_t k = 0; k < pk; ++k) {
        if (code[k] != 0xF0) continue;
        std::memcpy(buf, code, k);
        std::memcpy(buf + k, code + k + 1, n - k - 1);
        if (!DecodeOne(buf, n - 1, address, out)) break;
        Insn& i = *out;
        // ... only on the lockable instructions, and only with a memory operand
        static const char* const lockable[] = {"add", "adc", "and", "dec", "inc", "neg", "not", "or", "sbb", "sub",
                                               "xor", "xchg", "btc", "btr", "bts", "cmpxchg", "xadd"};
        bool okLock = false, mem = false;
        for (const char* m : lockable)
            if (i.mnemonic == m) okLock = true;
        for (int o = 0; o < i.nops; ++o)
            if (i.ops[o].type == OpType::Mem) mem = true;
        const uint8_t op1 = buf[pk - 1 < n - 1 ? pk - 1 : 0];
        if (!okLock || !mem || op1 == 0x1A || op1 == 0x1B) break;
        i.size = static_cast<uint8_t>(i.size + 1);
        std::memcpy(i.bytes, code, i.size);
        if (i.imm_offset && i.imm_offset >= k) ++i.imm_offset;
        if (i.disp_offset && i.disp_offset >= k) ++i.disp_offset;
        if (i.mnemonic.compare(0, 5, "lock ") != 0) i.mnemonic = "lock " + i.mnemonic;
        return true;
    }
    // capstone accepts "mov cs, r/m16" (8E /1)
    if (pk + 1 < n && code[pk] == 0x8E && ((code[pk + 1] >> 3) & 7) == 1) {
        std::memcpy(buf, code, n);
        buf[pk + 1] = static_cast<uint8_t>(buf[pk + 1] & ~0x38);   // es
        if (DecodeOne(buf, n, address, out)) {
            Insn& i = *out;
            std::memcpy(i.bytes, code, i.size);
            if (i.nops && i.ops[0].type == OpType::Reg && i.ops[0].reg == R_ES) i.ops[0].reg = R_CS;
            if (i.op_str.compare(0, 2, "es") == 0) i.op_str.replace(0, 2, "cs");
            return true;
        }
    }
    return false;
}

std::string DumpLine(const Insn& i) {
    std::string s = std::to_string(i.size) + "|" + i.mnemonic + "|" + i.op_str + "|";
    for (int k = 0; k < i.nops; ++k) {
        const Operand& o = i.ops[k];
        if (k) s += ",";
        char b[160];
        switch (o.type) {
        case OpType::Reg: std::snprintf(b, sizeof b, "r%d:%s", o.size, RegName(o.reg)); break;
        case OpType::Imm: std::snprintf(b, sizeof b, "i%d:%lld", o.size, static_cast<long long>(o.imm)); break;
        case OpType::Mem:
            std::snprintf(b, sizeof b, "m%d:%s:%s:%s:%d:%lld", o.size, o.seg ? RegName(o.seg) : "",
                          o.base ? RegName(o.base) : "", o.index ? RegName(o.index) : "", o.scale,
                          static_cast<long long>(o.disp));
            break;
        default: std::snprintf(b, sizeof b, "?"); break;
        }
        s += b;
    }
    char t[64];
    std::snprintf(t, sizeof t, "|%d|%d|", i.imm_offset, i.disp_offset);
    s += t;
    for (int k = 0; k < i.size; ++k) {
        std::snprintf(t, sizeof t, "%02x", i.bytes[k]);
        s += t;
    }
    return s;
}

}  // namespace pse::m2l
