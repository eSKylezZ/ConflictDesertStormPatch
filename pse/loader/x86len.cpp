#include "x86len.h"

#include <array>

namespace {
// Per-opcode flags. Immediates add up (C8 enter = I16 + I8, 9A / EA far pointer = IZ + I16).
enum : uint8_t {
    M = 0x01,       // ModRM follows
    I8 = 0x02,
    I16 = 0x04,
    IZ = 0x08,      // imm16 with 66, else imm32
    MOFFS = 0x10,   // A0-A3: address-size offset
    PFX = 0x20,     // legacy prefix
    BAD = 0x40,     // undefined in 32-bit mode
    GRP3 = 0x80,    // F6 / F7: immediate only for reg 0 / 1 (TEST)
};

using Table = std::array<uint8_t, 256>;

constexpr void Set(Table& t, int lo, int hi, uint8_t f) {
    for (int i = lo; i <= hi; ++i) t[i] = f;
}

constexpr Table MakeOneByte() {
    Table t{};
    // ALU blocks 00-3F: op r/m,r (4 x M), op al,imm8, op eax,immz, then push/pop seg or prefix / BCD.
    for (int base = 0; base < 0x40; base += 8) {
        Set(t, base, base + 3, M);
        t[base + 4] = I8;
        t[base + 5] = IZ;
    }
    t[0x0F] = 0;  // escape, handled in Decode
    t[0x26] = t[0x2E] = t[0x36] = t[0x3E] = PFX;
    // 40-5F inc/dec/push/pop: no operands (already 0).
    t[0x62] = M;  // bound (EVEX when mod = 3)
    t[0x63] = M;  // arpl
    Set(t, 0x64, 0x67, PFX);
    t[0x68] = IZ;
    t[0x69] = M | IZ;
    t[0x6A] = I8;
    t[0x6B] = M | I8;
    Set(t, 0x70, 0x7F, I8);  // jcc rel8
    t[0x80] = M | I8;
    t[0x81] = M | IZ;
    t[0x82] = M | I8;
    t[0x83] = M | I8;
    Set(t, 0x84, 0x8F, M);
    t[0x9A] = IZ | I16;  // call far ptr16:32
    Set(t, 0xA0, 0xA3, MOFFS);
    t[0xA8] = I8;
    t[0xA9] = IZ;
    Set(t, 0xB0, 0xB7, I8);
    Set(t, 0xB8, 0xBF, IZ);
    t[0xC0] = t[0xC1] = M | I8;
    t[0xC2] = I16;
    t[0xC4] = t[0xC5] = M;  // les / lds (VEX when mod = 3)
    t[0xC6] = M | I8;
    t[0xC7] = M | IZ;
    t[0xC8] = I16 | I8;
    t[0xCA] = I16;
    t[0xCD] = I8;
    Set(t, 0xD0, 0xD3, M);
    t[0xD4] = t[0xD5] = I8;
    Set(t, 0xD8, 0xDF, M);  // x87: always a ModRM byte (register forms use mod = 3)
    Set(t, 0xE0, 0xE7, I8);  // loop*/jecxz rel8, in/out imm8
    t[0xE8] = t[0xE9] = IZ;  // call / jmp rel
    t[0xEA] = IZ | I16;
    t[0xEB] = I8;
    t[0xF0] = t[0xF2] = t[0xF3] = PFX;
    t[0xF6] = t[0xF7] = M | GRP3;
    t[0xFE] = t[0xFF] = M;
    return t;
}

constexpr Table MakeTwoByte() {
    Table t{};
    Set(t, 0x00, 0xFF, M);  // most of the 0F map has a ModRM byte; exceptions below
    t[0x04] = t[0x0A] = t[0x0C] = BAD;
    Set(t, 0x05, 0x09, 0);  // syscall, clts, sysret, invd, wbinvd
    t[0x0B] = 0;            // ud2
    t[0x0E] = 0;            // femms
    t[0x0F] = M | I8;       // 3DNow!: ModRM + opcode suffix byte
    Set(t, 0x24, 0x27, BAD);
    Set(t, 0x30, 0x37, 0);  // wrmsr rdtsc rdmsr rdpmc sysenter sysexit - getsec
    t[0x36] = BAD;
    t[0x39] = BAD;
    Set(t, 0x3B, 0x3F, BAD);
    Set(t, 0x70, 0x73, M | I8);  // pshufw / shift groups with imm8
    t[0x77] = 0;                 // emms
    t[0x7A] = t[0x7B] = BAD;
    Set(t, 0x80, 0x8F, IZ);      // jcc rel
    Set(t, 0xA0, 0xA2, 0);       // push fs, pop fs, cpuid
    t[0xA4] = t[0xAC] = M | I8;  // shld / shrd imm8
    t[0xA6] = t[0xA7] = BAD;
    Set(t, 0xA8, 0xAA, 0);       // push gs, pop gs, rsm
    t[0xBA] = M | I8;            // bt group imm8
    Set(t, 0xC2, 0xC2, M | I8);  // cmpps
    Set(t, 0xC4, 0xC6, M | I8);  // pinsrw, pextrw, shufps
    Set(t, 0xC8, 0xCF, 0);       // bswap
    return t;
}

constexpr Table kOne = MakeOneByte();
constexpr Table kTwo = MakeTwoByte();
}  // namespace

uint32_t pse::x86::Decode(const uint8_t* code, uint32_t max, Insn* out) {
    Insn insn;
    *out = insn;
    if (max > 15) max = 15;
    uint32_t i = 0;
    while (i < max && (kOne[code[i]] & PFX)) {
        if (code[i] == 0x66) insn.opsize16 = true;
        if (code[i] == 0x67) insn.addr16 = true;
        ++i;
    }
    if (i >= max) return 0;
    insn.prefixLen = i;
    uint8_t op = code[i++];
    uint8_t flags = kOne[op];
    if (op == 0x0F) {
        if (i >= max) return 0;
        op = code[i++];
        insn.escape = 0x0F;
        if (op == 0x38 || op == 0x3A) {
            if (i >= max) return 0;
            insn.escape = op;
            flags = op == 0x3A ? (M | I8) : M;
            op = code[i++];
        } else {
            flags = kTwo[op];
        }
    }
    insn.opcode = op;
    if (flags & BAD) return 0;

    if (flags & M) {
        if (i >= max) return 0;
        uint8_t m = code[i++];
        insn.hasModrm = true;
        insn.modrm = m;
        uint32_t mod = m >> 6, reg = (m >> 3) & 7, rm = m & 7;
        if (insn.escape == 0) {
            // VEX / EVEX / XOP use these opcodes with forms that are invalid as les / lds / bound / pop.
            if ((op == 0xC4 || op == 0xC5 || op == 0x62) && mod == 3) return 0;
            if (op == 0x8F && reg != 0) return 0;
        }
        if (insn.addr16) {
            if (mod == 0 && rm == 6) insn.dispLen = 2;
            else if (mod == 1) insn.dispLen = 1;
            else if (mod == 2) insn.dispLen = 2;
        } else if (mod != 3) {
            if (rm == 4) {
                if (i >= max) return 0;
                uint8_t sib = code[i++];
                if (mod == 0 && (sib & 7) == 5) insn.dispLen = 4;
            }
            if (mod == 0 && rm == 5) insn.dispLen = 4;
            else if (mod == 1) insn.dispLen = 1;
            else if (mod == 2) insn.dispLen = 4;
        }
        if ((flags & GRP3) && reg < 2) insn.immLen += op == 0xF6 ? 1 : (insn.opsize16 ? 2 : 4);
    }
    if (flags & MOFFS) insn.dispLen = insn.addr16 ? 2 : 4;
    if (flags & I8) insn.immLen += 1;
    if (flags & I16) insn.immLen += 2;
    if (flags & IZ) insn.immLen += insn.opsize16 ? 2 : 4;

    uint32_t len = i + insn.dispLen + insn.immLen;
    if (len > max) return 0;
    insn.len = len;

    if (insn.escape == 0) {
        if ((op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3) || op == 0xEB) insn.relLen = 1;
        else if (op == 0xE8 || op == 0xE9) insn.relLen = insn.opsize16 ? 2 : 4;
    } else if (insn.escape == 0x0F && op >= 0x80 && op <= 0x8F) {
        insn.relLen = insn.opsize16 ? 2 : 4;
    }
    *out = insn;
    return len;
}
