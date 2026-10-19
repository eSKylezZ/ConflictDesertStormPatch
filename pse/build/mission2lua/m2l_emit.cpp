// mission2lua (C++ port): the emitter - mission2lua.FunctionEmitter + translate() line by line (ModKit
// conflict_ds_tools/mission2lua.py). The text it writes must be byte-identical to the Python's (tools/m2l_gate.py).
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "m2l_program.h"
#include "mission2lua.h"

namespace pse::m2l {

namespace {
#include "m2l_runtime.inc"

constexpr const char* kPortRevision = "cpp.2";   // bump when the C++ port changes its output (cache stamps)

struct Unsupported : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Broken : std::runtime_error {   // where the Python itself would fail (KeyError ...)
    using std::runtime_error::runtime_error;
};

std::string F(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < static_cast<int>(sizeof buf)) return std::string(buf, n > 0 ? static_cast<size_t>(n) : 0);
    std::string big(static_cast<size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    big.resize(static_cast<size_t>(n));
    return big;
}
std::string H(int64_t v) { return F("0x%X", static_cast<uint32_t>(v)); }

uint32_t Mask(int size) {
    switch (size) {
    case 1: return 0xFF;
    case 2: return 0xFFFF;
    case 4: return 0xFFFFFFFF;
    default: throw Broken("operand size " + std::to_string(size));
    }
}
uint32_t Sign(int size) {
    switch (size) {
    case 1: return 0x80;
    case 2: return 0x8000;
    case 4: return 0x80000000;
    default: throw Broken("operand size " + std::to_string(size));
    }
}
const char* ReadFn(int size) {
    switch (size) {
    case 1: return "r8";
    case 2: return "r16";
    case 4: return "r32";
    default: throw Broken("read size " + std::to_string(size));
    }
}
const char* WriteFn(int size) {
    switch (size) {
    case 1: return "w8";
    case 2: return "w16";
    case 4: return "w32";
    default: throw Broken("write size " + std::to_string(size));
    }
}
const char* Upper32(uint16_t r) {   // _R32 / _R16 / _LO8 / _HI8 -> "EAX"
    static const char* const n[] = {"", "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
    const uint16_t f = Full32(r);
    return f ? n[f] : nullptr;
}

// conditions
struct Cond {
    const char* cc;
    const char* generic;
};
const Cond kCond[] = {{"o", "fOF()"},          {"no", "not fOF()"},           {"b", "fCF()"},
                      {"ae", "not fCF()"},     {"e", "fZF()"},                {"ne", "not fZF()"},
                      {"be", "(fCF() or fZF())"}, {"a", "not (fCF() or fZF())"}, {"s", "fSF()"},
                      {"ns", "not fSF()"},     {"p", "fPF()"},                {"np", "not fPF()"},
                      {"l", "(fSF() ~= fOF())"}, {"ge", "(fSF() == fOF())"},  {"le", "(fZF() or fSF() ~= fOF())"},
                      {"g", "not (fZF() or fSF() ~= fOF())"}};
const Cond kFastSub[] = {{"e", "FA == FB"},
                         {"ne", "FA ~= FB"},
                         {"b", "FA < FB"},
                         {"ae", "FA >= FB"},
                         {"be", "FA <= FB"},
                         {"a", "FA > FB"},
                         {"l", "(FA ~ FW) < (FB ~ FW)"},
                         {"ge", "(FA ~ FW) >= (FB ~ FW)"},
                         {"le", "(FA ~ FW) <= (FB ~ FW)"},
                         {"g", "(FA ~ FW) > (FB ~ FW)"},
                         {"s", "(FR & FW) ~= 0"},
                         {"ns", "(FR & FW) == 0"}};
const Cond kFastLogic[] = {{"e", "FR == 0"},
                           {"ne", "FR ~= 0"},
                           {"s", "(FR & FW) ~= 0"},
                           {"ns", "(FR & FW) == 0"},
                           {"l", "(FR & FW) ~= 0"},
                           {"ge", "(FR & FW) == 0"},
                           {"le", "(FR == 0 or (FR & FW) ~= 0)"},
                           {"g", "(FR ~= 0 and (FR & FW) == 0)"},
                           {"b", "false"},
                           {"ae", "true"},
                           {"be", "FR == 0"},
                           {"a", "FR ~= 0"},
                           {"o", "false"},
                           {"no", "true"}};
const Cond kFastRes[] = {{"e", "FR == 0"}, {"ne", "FR ~= 0"}, {"s", "(FR & FW) ~= 0"}, {"ns", "(FR & FW) == 0"}};

template <size_t N>
const char* Find(const Cond (&t)[N], const std::string& cc) {
    for (const Cond& c : t)
        if (cc == c.cc) return c.generic;
    return nullptr;
}
// mission2lua._JCC: jcc mnemonic -> condition code (also setcc via "set" + cc)
const char* JccCode(const std::string& m) {
    static const struct {
        const char* m;
        const char* cc;
    } k[] = {{"jo", "o"},  {"jno", "no"}, {"jb", "b"},  {"jae", "ae"}, {"je", "e"},  {"jne", "ne"},
             {"jbe", "be"}, {"ja", "a"},   {"js", "s"},  {"jns", "ns"}, {"jp", "p"},  {"jnp", "np"},
             {"jl", "l"},  {"jge", "ge"}, {"jle", "le"}, {"jg", "g"}};
    for (const auto& e : k)
        if (m == e.m) return e.cc;
    return nullptr;
}
const char* SetCode(const std::string& m) {
    if (m.size() < 4 || m.compare(0, 3, "set") != 0) return nullptr;
    return JccCode("j" + m.substr(3));
}

bool StartsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

using Lines = std::vector<std::string>;
void Add(Lines& to, const Lines& from) { to.insert(to.end(), from.begin(), from.end()); }

class Emitter {
public:
    Emitter(Program& p, Function& f, Stats& st) : p_(p), f_(f), st_(st) {}
    Lines Emit();

private:
    bool Reloc(uint32_t va) const { return p_.dll.relocs.count(va) != 0; }
    std::string Addr(const Insn& i, const Operand& op) const;
    static bool Seg(const Operand& op) { return op.type == OpType::Mem && op.seg == R_FS; }
    std::string Rd(const Insn& i, const Operand& op, int size = 0, const char* a = nullptr) const;
    std::string Wr(const Insn& i, const Operand& op, const std::string& val, int size = 0, const char* a = nullptr) const;
    const char* MemAddr(const Insn& i, const Operand& op, Lines& lines) const;
    std::string Flags(int kind, int size, const char* a = nullptr, const char* b = nullptr, const char* r = "r");
    std::string CondExpr(const std::string& cc) const;
    Lines Ins(const Insn& i);
    Lines Shift(const Insn& i, const std::string& m, Lines& L);
    Lines Fpu(const Insn& i, Lines& L);
    static std::string Farith(const std::string& o, const std::string& d, const std::string& s);
    Lines Call(const Insn& i) const;
    static Lines Ret(const Insn& i);
    Lines Terminator(const Block& b, const Insn& i, int64_t nxt);

    Program& p_;
    Function& f_;
    Stats& st_;
    int flagKind_ = -1;   // None
};

std::string Emitter::Addr(const Insn& i, const Operand& op) const {
    std::vector<std::string> parts;
    if (op.base) {
        if (!IsReg32(op.base)) throw Broken("16-bit base");
        parts.push_back(Upper32(op.base));
    }
    if (op.index) {
        if (!IsReg32(op.index)) throw Broken("16-bit index");
        const std::string r = Upper32(op.index);
        parts.push_back(op.scale == 1 ? r : F("%s*%d", r.c_str(), op.scale));
    }
    const uint32_t disp = static_cast<uint32_t>(op.disp);
    if (disp) {
        if (i.disp_offset && Reloc(i.address + i.disp_offset)) parts.push_back("(" + H(disp) + "+D)");
        else parts.push_back(H(disp));
    }
    if (parts.empty()) return "0";
    if (parts.size() == 1) return parts[0];
    std::string j;
    for (size_t k = 0; k < parts.size(); ++k) j += (k ? " + " : "") + parts[k];
    return "((" + j + ") & 0xFFFFFFFF)";
}

std::string Emitter::Rd(const Insn& i, const Operand& op, int size, const char* a) const {
    if (!size) size = op.size;
    if (op.type == OpType::Imm) {
        const uint32_t v = static_cast<uint32_t>(op.imm) & Mask(size);
        if (i.imm_offset && Reloc(i.address + i.imm_offset)) return "(" + H(v) + "+D)";
        return H(v);
    }
    if (op.type == OpType::Reg) {
        const uint16_t r = op.reg;
        if (IsReg32(r)) return Upper32(r);
        if (IsReg16(r)) return F("(%s & 0xFFFF)", Upper32(r));
        if (IsLow8(r)) return F("(%s & 0xFF)", Upper32(r));
        if (IsHigh8(r)) return F("((%s >> 8) & 0xFF)", Upper32(r));
        throw Unsupported(F("register %s", RegName(r)));
    }
    if (op.type == OpType::Mem) {
        if (Seg(op)) {
            if (size != 4) throw Unsupported(F("fs access size %d", size));
            return "(FS[" + (a ? std::string(a) : Addr(i, op)) + "] or 0)";
        }
        return std::string(ReadFn(size)) + "(" + (a ? std::string(a) : Addr(i, op)) + ")";
    }
    throw Unsupported("operand type");
}

std::string Emitter::Wr(const Insn& i, const Operand& op, const std::string& val, int size, const char* a) const {
    if (!size) size = op.size;
    if (op.type == OpType::Reg) {
        const uint16_t r = op.reg;
        if (IsReg32(r)) return std::string(Upper32(r)) + " = " + val;
        const char* n = Upper32(r);
        if (IsReg16(r)) return F("%s = (%s & 0xFFFF0000) | (", n, n) + val + ")";
        if (IsLow8(r)) return F("%s = (%s & 0xFFFFFF00) | (", n, n) + val + ")";
        if (IsHigh8(r)) return F("%s = (%s & 0xFFFF00FF) | ((", n, n) + val + ") << 8)";
        throw Unsupported(F("register %s", RegName(r)));
    }
    if (op.type == OpType::Mem) {
        if (Seg(op)) return "FS[" + (a ? std::string(a) : Addr(i, op)) + "] = " + val;
        return std::string(WriteFn(size)) + "(" + (a ? std::string(a) : Addr(i, op)) + ", " + val + ")";
    }
    throw Unsupported(F("write to operand type %d", static_cast<int>(op.type)));
}

const char* Emitter::MemAddr(const Insn& i, const Operand& op, Lines& lines) const {
    if (op.type == OpType::Mem) {
        lines.push_back("a = " + Addr(i, op));
        return "a";
    }
    return nullptr;
}

std::string Emitter::Flags(int kind, int size, const char* a, const char* b, const char* r) {
    std::string s = F("FK = %d; FW = %s; FR = %s", kind, H(Sign(size)).c_str(), r);
    if (a) s += F("; FA = %s; FB = %s", a, b);
    flagKind_ = kind;
    return s;
}

std::string Emitter::CondExpr(const std::string& cc) const {
    const char* fast = nullptr;
    switch (flagKind_) {
    case 2: fast = Find(kFastSub, cc); break;
    case 3: fast = Find(kFastLogic, cc); break;
    case 1:
    case 4: fast = Find(kFastRes, cc); break;
    default: break;
    }
    if (fast) return fast;
    const char* g = Find(kCond, cc);
    if (!g) throw Broken("condition " + cc);
    return g;
}

Lines Emitter::Ins(const Insn& i) {
    const std::string& m = i.mnemonic;
    const Operand* ops = i.ops;
    Lines L;
    if (m == "nop" || m == "wait" || m == "fnclex") return L;
    if (m == "cld") return {"DF = 1"};
    if (m == "std") return {"DF = -1"};
    if (m == "mov") return {Wr(i, ops[0], Rd(i, ops[1], ops[0].size))};
    if (m == "movzx") return {Wr(i, ops[0], Rd(i, ops[1]))};
    if (m == "movsx") {
        const std::string sb = H(Sign(ops[1].size));
        Lines out{"x = " + Rd(i, ops[1])};
        out.push_back(Wr(i, ops[0], F("(((x ~ %s) - %s) & %s)", sb.c_str(), sb.c_str(), H(Mask(ops[0].size)).c_str())));
        return out;
    }
    if (m == "lea") {
        if (ops[0].size != 4) throw Unsupported("lea16");
        return {Wr(i, ops[0], Addr(i, ops[1]))};
    }
    if (m == "push") {
        if (ops[0].size == 2) throw Unsupported("push16");
        return {"x = " + Rd(i, ops[0], 4), "ESP = (ESP - 4) & 0xFFFFFFFF", "w32(ESP, x)"};
    }
    if (m == "pop") return {"x = r32(ESP)", "ESP = (ESP + 4) & 0xFFFFFFFF", Wr(i, ops[0], "x")};
    if (m == "xchg") {
        const char* a = MemAddr(i, ops[0], L);
        if (!a) a = MemAddr(i, ops[1], L);
        const bool m0 = ops[0].type == OpType::Mem, m1 = ops[1].type == OpType::Mem;
        const std::string x = "x = " + Rd(i, ops[0], 0, m0 ? a : nullptr);
        const std::string y = "y = " + Rd(i, ops[1], 0, m1 ? a : nullptr);
        const std::string w0 = Wr(i, ops[0], "y", 0, m0 ? a : nullptr);
        const std::string w1 = Wr(i, ops[1], "x", 0, m1 ? a : nullptr);
        L.push_back(x), L.push_back(y), L.push_back(w0), L.push_back(w1);
        return L;
    }
    if (m == "leave") return {"ESP = EBP", "EBP = r32(ESP)", "ESP = (ESP + 4) & 0xFFFFFFFF"};
    if (m == "add" || m == "sub" || m == "cmp" || m == "and" || m == "or" || m == "xor" || m == "test" || m == "adc" ||
        m == "sbb") {
        const int size = ops[0].size;
        const std::string mk = H(Mask(size));
        const char* a = MemAddr(i, ops[0], L);
        if ((m == "xor" || m == "sub") && ops[0].type == OpType::Reg && ops[1].type == OpType::Reg &&
            ops[0].reg == ops[1].reg) {
            L.push_back(Wr(i, ops[0], "0"));
            L.push_back(Flags(m == "xor" ? 3 : 2, size, "0", "0", "0"));
            return L;
        }
        L.push_back("x = " + Rd(i, ops[0], 0, a));
        L.push_back("y = " + Rd(i, ops[1], size));
        if (m == "add") {
            L.push_back("r = (x + y) & " + mk);
            L.push_back(Flags(1, size, "x", "y"));
        } else if (m == "sub" || m == "cmp") {
            L.push_back("r = (x - y) & " + mk);
            L.push_back(Flags(2, size, "x", "y"));
        } else if (m == "and" || m == "test") {
            L.push_back("r = x & y");
            L.push_back(Flags(3, size));
        } else if (m == "or") {
            L.push_back("r = x | y");
            L.push_back(Flags(3, size));
        } else if (m == "xor") {
            L.push_back("r = x ~ y");
            L.push_back(Flags(3, size));
        } else if (m == "adc") {
            Add(L, {"c = fCF() and 1 or 0", "r = x + y + c", "FCF = r > " + mk, "r = r & " + mk,
                    "FOF = ((x ~ r) & (y ~ r) & " + H(Sign(size)) + ") ~= 0"});
            L.push_back(Flags(4, size));
        } else {   // sbb
            Add(L, {"c = fCF() and 1 or 0", "FCF = x < y + c", "r = (x - y - c) & " + mk,
                    "FOF = ((x ~ y) & (x ~ r) & " + H(Sign(size)) + ") ~= 0"});
            L.push_back(Flags(4, size));
        }
        if (m != "cmp" && m != "test") L.push_back(Wr(i, ops[0], "r", 0, a));
        return L;
    }
    if (m == "inc" || m == "dec") {
        const int size = ops[0].size;
        const char* a = MemAddr(i, ops[0], L);
        const uint32_t sb = Sign(size);
        L.push_back("c = fCF()");
        L.push_back("x = " + Rd(i, ops[0], 0, a));
        L.push_back(F("r = (x %s 1) & %s", m == "inc" ? "+" : "-", H(Mask(size)).c_str()));
        L.push_back("FCF = c");
        L.push_back("FOF = r == " + H(m == "inc" ? sb : sb - 1));
        L.push_back(Flags(4, size));
        L.push_back(Wr(i, ops[0], "r", 0, a));
        return L;
    }
    if (m == "neg") {
        const int size = ops[0].size;
        const char* a = MemAddr(i, ops[0], L);
        L.push_back("y = " + Rd(i, ops[0], 0, a));
        L.push_back("r = (-y) & " + H(Mask(size)));
        L.push_back(Flags(2, size, "0", "y"));
        L.push_back(Wr(i, ops[0], "r", 0, a));
        return L;
    }
    if (m == "not") {
        const char* a = MemAddr(i, ops[0], L);
        L.push_back(Wr(i, ops[0], "(~" + Rd(i, ops[0], 0, a) + ") & " + H(Mask(ops[0].size)), 0, a));
        return L;
    }
    if (m == "shl" || m == "sal" || m == "shr" || m == "sar" || m == "rol" || m == "ror" || m == "rcr" || m == "rcl")
        return Shift(i, m, L);
    if (m == "shld" || m == "shrd") throw Unsupported(m);
    if (m == "imul") {
        if (i.nops == 1) {
            if (ops[0].size != 4) throw Unsupported("imul8/16");
            Add(L, {"x = SX(EAX) * SX(" + Rd(i, ops[0]) + ")", "EAX = x & 0xFFFFFFFF", "EDX = (x >> 32) & 0xFFFFFFFF",
                    "FCF = SX(EAX) ~= x", "FOF = FCF", "FR = EAX", "FW = 0x80000000", "FK = 4"});
            flagKind_ = 4;
            return L;
        }
        const Operand& src = ops[1];
        const Operand& other = i.nops == 2 ? ops[0] : ops[2];
        if (ops[0].size != 4) throw Unsupported("imul16");
        L.push_back("x = SX(" + Rd(i, src) + ") * SX(" + Rd(i, other) + ")");
        Add(L, {"r = x & 0xFFFFFFFF", "FCF = SX(r) ~= x", "FOF = FCF"});
        L.push_back(Flags(4, 4));
        L.push_back(Wr(i, ops[0], "r"));
        return L;
    }
    if (m == "mul") {
        if (ops[0].size != 4) throw Unsupported("mul8/16");
        Add(L, {"x = EAX * " + Rd(i, ops[0]), "EAX = x & 0xFFFFFFFF", "EDX = (x >> 32) & 0xFFFFFFFF", "FCF = EDX ~= 0",
                "FOF = FCF", "FR = EAX", "FW = 0x80000000", "FK = 4"});
        flagKind_ = 4;
        return L;
    }
    if (m == "div" || m == "idiv") {
        if (ops[0].size != 4) throw Unsupported("div8/16");
        L.push_back(F("EAX, EDX = %s(EDX, EAX, ", m == "div" ? "UDIV" : "IDIV") + Rd(i, ops[0]) + ")");
        flagKind_ = -1;
        return L;
    }
    if (m == "cdq") return {"EDX = (EAX >= 0x80000000) and 0xFFFFFFFF or 0"};
    if (m == "cwde") return {"EAX = (((EAX & 0xFFFF) ~ 0x8000) - 0x8000) & 0xFFFFFFFF"};
    if (m == "cbw") return {"EAX = (EAX & 0xFFFF0000) | ((((EAX & 0xFF) ~ 0x80) - 0x80) & 0xFFFF)"};
    if (const char* cc = SetCode(m)) return {Wr(i, ops[0], "(" + CondExpr(cc) + ") and 1 or 0")};
    if (m == "sahf") {
        flagKind_ = 5;
        return {"x = (EAX >> 8) & 0xFF", "FCF = (x & 1) ~= 0", "FPF = (x & 4) ~= 0", "FZF = (x & 0x40) ~= 0",
                "FSF = (x & 0x80) ~= 0", "FOF = fOF()", "FK = 5"};
    }
    if (m == "lahf") return {"EAX = (EAX & 0xFFFF00FF) | (FLAGS() & 0xFF) << 8"};
    for (const char* s : {"movsd", "movsw", "movsb", "rep movsd", "rep movsw", "rep movsb"})
        if (m == s) {
            const int n = m.back() == 'd' ? 4 : m.back() == 'w' ? 2 : 1;
            return {F("MOVS(%d, %s)", n, StartsWith(m, "rep") ? "true" : "false")};
        }
    for (const char* s : {"stosd", "stosw", "stosb", "rep stosd", "rep stosw", "rep stosb"})
        if (m == s) {
            const int n = m.back() == 'd' ? 4 : m.back() == 'w' ? 2 : 1;
            return {F("STOS(%d, %s)", n, StartsWith(m, "rep") ? "true" : "false")};
        }
    if (m == "repne scasb") {
        flagKind_ = 2;
        return {"SCASB_NE()"};
    }
    if (m == "repe cmpsb") {
        flagKind_ = 2;
        return {"CMPSB_E()"};
    }
    if (m == "int3" || m == "hlt") return {"error(\"x86: " + m + " at " + H(i.address) + "\")"};
    if ((i.bytes[0] >= 0xD8 && i.bytes[0] < 0xE0) || (StartsWith(m, "f") && m != "fs")) return Fpu(i, L);
    throw Unsupported(m);
}

Lines Emitter::Shift(const Insn& i, const std::string& m, Lines& L) {
    const Operand* ops = i.ops;
    const int size = ops[0].size, bits = size * 8;
    const std::string mk = H(Mask(size)), sb = H(Sign(size));
    const char* a = MemAddr(i, ops[0], L);
    const std::string cnt = i.nops > 1 ? Rd(i, ops[1], 1) : "1";
    L.push_back("c = " + cnt + " & 31");
    L.push_back("if c ~= 0 then");
    L.push_back("  x = " + Rd(i, ops[0], 0, a));
    if (m == "shl" || m == "sal") {
        L.push_back("  r = (x << c) & " + mk);
        L.push_back(F("  FCF = c <= %d and ((x >> (%d - c)) & 1) == 1", bits, bits));
        L.push_back("  FOF = ((r & " + sb + ") ~= 0) ~= FCF");
        L.push_back("  FK = 4; FW = " + sb + "; FR = r");
    } else if (m == "shr") {
        Add(L, {"  r = x >> c", "  FCF = ((x >> (c - 1)) & 1) == 1", "  FOF = (x & " + sb + ") ~= 0",
                "  FK = 4; FW = " + sb + "; FR = r"});
    } else if (m == "sar") {
        Add(L, {"  y = ((x ~ " + sb + ") - " + sb + ")", "  r = (y >> c | (y < 0 and ~(-1 >> c) or 0)) & " + mk,
                "  FCF = ((y >> (c - 1)) & 1) == 1", "  FOF = false", "  FK = 4; FW = " + sb + "; FR = r"});
    } else {
        L.push_back("  FZF, FSF, FPF = fZF(), fSF(), fPF()");
        if (m == "rol") {
            Add(L, {F("  c = c %% %d", bits), F("  r = ((x << c) | (x >> (%d - c))) & ", bits) + mk,
                    "  FCF = (r & 1) == 1", "  FOF = ((r & " + sb + ") ~= 0) ~= FCF"});
        } else if (m == "ror") {
            Add(L, {F("  c = c %% %d", bits), F("  r = ((x >> c) | (x << (%d - c))) & ", bits) + mk,
                    "  FCF = (r & " + sb + ") ~= 0",
                    "  FOF = ((r & " + sb + ") ~= 0) ~= ((r & " + H(Sign(size) >> 1) + ") ~= 0)"});
        } else {
            L.push_back(F("  r, FCF, FOF = %s(x, c, %d, fCF())", m == "rcl" ? "RCL" : "RCR", bits));
        }
        L.push_back("  FK = 5");
    }
    L.push_back("  " + Wr(i, ops[0], "r", 0, a));
    L.push_back("end");
    flagKind_ = -1;
    return L;
}

std::string Emitter::Farith(const std::string& o, const std::string& d, const std::string& s) {
    if (o == "+") return "PR(" + d + " + " + s + ")";
    if (o == "*") return "PR(" + d + " * " + s + ")";
    if (o == "-") return "PR(" + d + " - " + s + ")";
    if (o == "/") return "PR(" + d + " / " + s + ")";
    if (o == "-r") return "PR(" + s + " - " + d + ")";
    if (o == "/r") return "PR(" + s + " / " + d + ")";
    throw Broken("x87 operation " + o);
}

Lines Emitter::Fpu(const Insn& i, Lines& L) {
    const uint8_t* by = i.bytes;
    const int len = i.size;
    int k = 0;
    while (k < len && (by[k] == 0x9B || by[k] == 0x66 || by[k] == 0xF2 || by[k] == 0xF3 || by[k] == 0x2E || by[k] == 0x3E ||
                       by[k] == 0x26 || by[k] == 0x36 || by[k] == 0x64 || by[k] == 0x65)) {
        if (by[k] == 0x9B && len == 1) return L;
        ++k;
    }
    if (k >= len) throw Broken("x87 prefixes only");
    const int op = by[k];
    const bool hasModrm = k + 1 < len;
    const int modrm = hasModrm ? by[k + 1] : -1;
    const int reg = hasModrm ? (modrm >> 3) & 7 : 0;
    const Operand* memop = nullptr;
    for (int n = 0; n < i.nops; ++n)
        if (i.ops[n].type == OpType::Mem) {
            memop = &i.ops[n];
            break;
        }
    auto ret = [&](std::initializer_list<const char*> more) {
        for (const char* s : more) L.push_back(s);
        return L;
    };
    if (hasModrm && modrm < 0xC0) {
        if (!memop) throw Broken("x87 memory form without a memory operand");
        L.push_back("a = " + Addr(i, *memop));
        static const char* const arith[] = {"+", "*", "cmp", "cmpp", "-", "-r", "/", "/r"};
        if (op == 0xD8 || op == 0xDC || op == 0xDA || op == 0xDE) {
            const char* src = op == 0xD8   ? "rf(a)"
                              : op == 0xDC ? "rd(a)"
                              : op == 0xDA ? "(SX(r32(a)) + 0.0)"
                                           : "((((r16(a)) ~ 0x8000) - 0x8000) + 0.0)";
            const std::string o = arith[reg];
            if (o == "cmp" || o == "cmpp") {
                L.push_back(std::string("fcmp(ST[FT], ") + src + ")");
                if (o == "cmpp") L.push_back("fpop()");
            } else {
                L.push_back("ST[FT] = " + Farith(o, "ST[FT]", src));
            }
            return L;
        }
        if (op == 0xD9) {
            if (reg == 0) return ret({"fpush(rf(a))"});
            if (reg == 2) return ret({"wf(a, ST[FT])"});
            if (reg == 3) return ret({"wf(a, fpop())"});
            if (reg == 5) return ret({"FLDCW(r16(a))"});
            if (reg == 7) return ret({"w16(a, FNSTCW())"});
        }
        if (op == 0xDD) {
            if (reg == 0) return ret({"fpush(rd(a))"});
            if (reg == 2) return ret({"wd(a, ST[FT])"});
            if (reg == 3) return ret({"wd(a, fpop())"});
            if (reg == 7) return ret({"w16(a, SW())"});
        }
        if (op == 0xDB) {
            if (reg == 0) return ret({"fpush(SX(r32(a)) + 0.0)"});
            if (reg == 2) return ret({"w32(a, FIST(ST[FT], 32))"});
            if (reg == 3) return ret({"w32(a, FIST(fpop(), 32))"});
        }
        if (op == 0xDF) {
            if (reg == 0) return ret({"fpush((((r16(a)) ~ 0x8000) - 0x8000) + 0.0)"});
            if (reg == 2) return ret({"w16(a, FIST(ST[FT], 16))"});
            if (reg == 3) return ret({"w16(a, FIST(fpop(), 16))"});
            if (reg == 5) return ret({"fpush(I64(r32(a), r32(a + 4)))"});
            if (reg == 7)
                return ret({"x = FIST64(fpop())", "w32(a, x & 0xFFFFFFFF)", "w32(a + 4, (x >> 32) & 0xFFFFFFFF)"});
        }
        throw Unsupported(F("x87 %02X /%d mem", op, reg));
    }
    if (!hasModrm) throw Broken("x87 register form without ModRM");
    // register forms
    const int sti = modrm & 7;
    const std::string s = sti ? F("ST[FT - %d]", sti) : "ST[FT]";
    if (op == 0xD8) {
        static const char* const ar[] = {"+", "*", "cmp", "cmpp", "-", "-r", "/", "/r"};
        const std::string o = ar[reg];
        if (o == "cmp" || o == "cmpp") {
            Lines out{"fcmp(ST[FT], " + s + ")"};
            if (o == "cmpp") out.push_back("fpop()");
            return out;
        }
        return {"ST[FT] = " + Farith(o, "ST[FT]", s)};
    }
    if (op == 0xDC || op == 0xDE) {
        if (op == 0xDE && modrm == 0xD9) return {"fcmp(ST[FT], ST[FT - 1])", "fpop()", "fpop()"};
        const char* o = reg == 0 ? "+" : reg == 1 ? "*" : reg == 4 ? "-r" : reg == 5 ? "-" : reg == 6 ? "/r" : reg == 7 ? "/" : nullptr;
        if (!o) throw Unsupported(F("x87 %02X %02X", op, modrm));
        Lines out{s + " = " + Farith(o, s, "ST[FT]")};
        if (op == 0xDE) out.push_back("fpop()");
        return out;
    }
    if (op == 0xD9) {
        if (modrm >= 0xC0 && modrm <= 0xC7) return {"fpush(" + s + ")"};
        if (modrm >= 0xC8 && modrm <= 0xCF) return {"ST[FT], " + s + " = " + s + ", ST[FT]"};
        static const struct {
            int modrm;
            const char* code;
        } simple[] = {{0xE0, "ST[FT] = -ST[FT]"},
                      {0xE1, "ST[FT] = math.abs(ST[FT])"},
                      {0xE4, "fcmp(ST[FT], 0.0)"},
                      {0xE8, "fpush(1.0)"},
                      {0xEE, "fpush(0.0)"},
                      {0xEB, "fpush(FCONST(3))"},
                      {0xE9, "fpush(FCONST(1))"},
                      {0xEA, "fpush(FCONST(2))"},
                      {0xEC, "fpush(FCONST(4))"},
                      {0xED, "fpush(FCONST(5))"},
                      {0xFA, "ST[FT] = PR(math.sqrt(ST[FT]))"},
                      {0xFC, "ST[FT] = FRND(ST[FT])"},
                      {0xFE, "ST[FT] = X.fpu(1, ST[FT])"},
                      {0xFF, "ST[FT] = X.fpu(2, ST[FT])"},
                      {0xF3, "x = fpop(); ST[FT] = X.fpu(3, ST[FT], x)"},
                      {0xF2, "ST[FT] = X.fpu(4, ST[FT]); fpush(1.0)"},
                      {0xF1, "x = fpop(); ST[FT] = X.fpu(5, ST[FT], x)"},
                      {0xF0, "ST[FT] = X.fpu(6, ST[FT])"},
                      {0xFD, "ST[FT] = X.fpu(7, ST[FT], ST[FT - 1])"},
                      {0xF8, "ST[FT] = X.fpu(8, ST[FT], ST[FT - 1])"},
                      {0xD0, ""},
                      {0xFB, "x = ST[FT]; ST[FT] = X.fpu(1, x); fpush(X.fpu(2, x))"}};
        for (const auto& e : simple)
            if (e.modrm == modrm) return *e.code ? Lines{e.code} : Lines{};
    }
    if (op == 0xDD) {
        if (modrm >= 0xD8 && modrm <= 0xDF) return sti ? Lines{s + " = ST[FT]", "fpop()"} : Lines{"fpop()"};
        if (modrm >= 0xD0 && modrm <= 0xD7) return {s + " = ST[FT]"};
        if (modrm >= 0xE0 && modrm <= 0xE7) return {"fcmp(ST[FT], " + s + ")"};
        if (modrm >= 0xE8 && modrm <= 0xEF) return {"fcmp(ST[FT], " + s + ")", "fpop()"};
        if (modrm >= 0xC0 && modrm <= 0xC7) return {};
    }
    if (op == 0xDA && modrm == 0xE9) return {"fcmp(ST[FT], ST[FT - 1])", "fpop()", "fpop()"};
    if (op == 0xDB && (modrm == 0xE2 || modrm == 0xE3)) return modrm == 0xE2 ? Lines{} : Lines{"FT = 0; ST = {}; FCW = 0x37F"};
    if (op == 0xDF && modrm == 0xE0) return {"EAX = (EAX & 0xFFFF0000) | SW()"};
    throw Unsupported(F("x87 %02X %02X (%s)", op, modrm, i.mnemonic.c_str()));
}

Lines Emitter::Call(const Insn& i) const {
    const Operand& op = i.ops[0];
    const std::string ret = "(" + H(i.address + i.size) + "+D)";
    if (op.type == OpType::Imm) {
        const uint32_t t = static_cast<uint32_t>(op.imm);
        if (p_.functions.count(t)) return {"ESP = (ESP - 4) & 0xFFFFFFFF", "w32(ESP, " + ret + ")", "F[" + H(t) + "]()"};
        return {"error(\"x86: call to unknown " + H(t) + "\")"};
    }
    return {"CALL(" + Rd(i, op, 4) + ", " + ret + ")"};
}

Lines Emitter::Ret(const Insn& i) {
    const int64_t n = i.nops ? i.ops[0].imm : 0;
    return {F("ESP = (ESP + %lld) & 0xFFFFFFFF", static_cast<long long>(4 + n)), "do return end"};
}

Lines Emitter::Terminator(const Block& b, const Insn& i, int64_t nxt) {
    const std::string& m = i.mnemonic;
    switch (b.kind) {
    case Block::Ret: return Ret(i);
    case Block::Cond: {
        const uint32_t taken = static_cast<uint32_t>(i.ops[0].imm);
        int64_t fall = static_cast<int64_t>(i.address) + i.size;
        if (!f_.blocks.count(taken)) return {"error(\"x86: branch out of the code at " + H(i.address) + "\")"};
        if (!f_.blocks.count(static_cast<uint32_t>(fall))) fall = -1;
        std::string c;
        if (m == "jecxz") c = "ECX == 0";
        else if (const char* cc = JccCode(m)) c = CondExpr(cc);
        else if (m == "loop") throw Unsupported("loop");
        else throw Unsupported(m);
        Lines out{F("if %s then goto L%X end", c.c_str(), taken)};
        if (fall >= 0 && fall != nxt) out.push_back(F("goto L%X", static_cast<uint32_t>(fall)));
        else if (fall < 0) out.push_back("error(\"x86: fell off " + H(i.address) + "\")");
        return out;
    }
    case Block::Jmp: {
        const uint32_t t = !b.succ.empty() ? b.succ[0] : static_cast<uint32_t>(i.ops[0].imm);
        if (!f_.blocks.count(t)) return {"error(\"x86: jump out of the function at " + H(i.address) + "\")"};
        if (static_cast<int64_t>(t) == nxt) return {};
        return {F("goto L%X", t)};
    }
    case Block::Switch: {
        Lines out{"t = (r32(" + Addr(i, i.ops[0]) + ") - D) & 0xFFFFFFFF"};
        std::set<uint32_t> targets;
        for (auto& kv : b.table->values) targets.insert(kv.second);
        bool first = true;
        for (uint32_t t : targets) {
            out.push_back(F("%s t == %s then goto L%X", first ? "if" : "elseif", H(t).c_str(), t));
            first = false;
        }
        out.push_back("end");
        out.push_back("error(\"x86: switch at " + H(i.address) + " out of range: \" .. string.format(\"%X\", t))");
        return out;
    }
    default: break;
    }
    if (b.kind == Block::TailCall || (b.kind == Block::End && m == "jmp")) return {"do return TAIL(" + Rd(i, i.ops[0], 4) + ") end"};
    if (b.kind == Block::End) {
        if (m == "int3" || m == "hlt") return {"error(\"x86: " + m + " at " + H(i.address) + "\")"};
        if (m == "call") {
            Lines out = Call(i);
            out.push_back("error(\"x86: returned from a no-return call at " + H(i.address) + "\")");
            return out;
        }
        Lines out = Ins(i);
        out.push_back("error(\"x86: ran off the decoded code after " + H(i.address) + "\")");
        return out;
    }
    throw Unsupported("block kind fall");
}

Lines Emitter::Emit() {
    Lines out;
    out.push_back("F[" + H(f_.entry) + "] = function()");
    out.push_back("local a, c, x, y, r, t");
    if (!f_.blocks.empty() && f_.blocks.begin()->first != f_.entry) out.push_back(F("goto L%X", f_.entry));
    for (auto it = f_.blocks.begin(); it != f_.blocks.end(); ++it) {
        const Block& b = it->second;
        auto nx = std::next(it);
        const int64_t nxt = nx == f_.blocks.end() ? -1 : static_cast<int64_t>(nx->first);
        out.push_back(F("::L%X::", it->first));
        flagKind_ = -1;
        const size_t last = b.insns.size() - 1;
        for (size_t n = 0; n < b.insns.size(); ++n) {
            const Insn& i = *b.insns[n];
            std::string text = i.mnemonic + " " + i.op_str;
            std::replace(text.begin(), text.end(), '\n', ' ');
            out.push_back(F("-- %08X ", i.address) + text);
            try {
                if (n == last && b.kind != Block::Fall) {
                    Add(out, Terminator(b, i, nxt));
                } else {
                    if (i.mnemonic == "call") {
                        Add(out, Call(i));
                        flagKind_ = -1;
                    } else if (i.mnemonic == "ret" || i.mnemonic == "retn") {
                        Add(out, Ret(i));
                    } else {
                        Add(out, Ins(i));
                    }
                    if (n == last && b.kind == Block::Fall && !b.succ.empty() && static_cast<int64_t>(b.succ[0]) != nxt)
                        out.push_back(F("goto L%X", b.succ[0]));
                }
            } catch (const Unsupported& e) {
                std::string why = e.what();
                ++st_.unsupported[why];
                std::replace(why.begin(), why.end(), '"', '\'');
                out.push_back("error(\"x86: unsupported " + why + " at " + H(i.address) + "\")");
            }
        }
    }
    out.push_back("end");
    return out;
}

std::string Hex(const std::vector<uint8_t>& data) {
    static const char d[] = "0123456789abcdef";
    std::string s(data.size() * 2, '0');
    for (size_t k = 0; k < data.size(); ++k) s[2 * k] = d[data[k] >> 4], s[2 * k + 1] = d[data[k] & 15];
    return s;
}

std::string Join(const char* const* parts) {
    std::string s;
    for (; *parts; ++parts) s += *parts;
    return s;
}

// Python "%(key)s" / "%%" formatting of MAIN
std::string FormatMain(const std::string& t, const std::map<std::string, std::string>& kv) {
    std::string out;
    for (size_t p = 0; p < t.size(); ++p) {
        if (t[p] != '%') {
            out += t[p];
            continue;
        }
        if (p + 1 < t.size() && t[p + 1] == '%') {
            out += '%';
            ++p;
            continue;
        }
        const size_t close = t.find(")s", p);
        if (p + 1 < t.size() && t[p + 1] == '(' && close != std::string::npos) {
            auto it = kv.find(t.substr(p + 2, close - p - 2));
            if (it == kv.end()) throw Broken("MAIN key " + t.substr(p + 2, close - p - 2));
            out += it->second;
            p = close + 1;
            continue;
        }
        throw Broken("MAIN format");
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

}  // namespace

int TranslatorVersion() { return kTranslatorVersion; }
const char* TranslatorBuild() {
    static const std::string s = std::to_string(kTranslatorVersion) + "+" + kPortRevision;
    return s.c_str();
}

std::string Sha256Hex(const std::vector<uint8_t>& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    uint8_t h[32] = {};
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0) {
        BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), h, 32);
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return Hex(std::vector<uint8_t>(h, h + 32));
}

bool Translate(const std::vector<uint8_t>& file, const std::string& name, std::string* lua, Stats* stats,
               std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    Stats local;
    Stats& st = stats ? *stats : local;
    st = Stats();
    try {
        Program p;
        if (!p.Load(file, name, error)) return false;
        auto setup = p.dll.exports.find("LevelSetup");
        if (setup == p.dll.exports.end()) {
            if (error) *error = "no LevelSetup export";
            return false;
        }
        st.functions = static_cast<uint32_t>(p.functions.size());
        Lines body;
        std::set<uint32_t> seen;
        for (auto& [va, f] : p.functions) {
            for (auto& [bva, b] : f->blocks)
                for (const Insn* i : b.insns) seen.insert(i->address);
            Add(body, Emitter(p, *f, st).Emit());
        }
        st.instructions = static_cast<uint32_t>(seen.size());
        std::vector<uint32_t> thunks;
        for (uint32_t t : p.taken)
            if (p.functions.count(t)) thunks.push_back(t);
        for (size_t k = 0; k + 1 < thunks.size(); ++k) {
            const uint32_t a = thunks[k], b = thunks[k + 1];
            if (b - a < 5) {
                uint8_t c[4] = {};
                for (int n = 0; n < 4; ++n) p.dll.U8(a + n, &c[n]);
                if (c[0] == 0x6A && c[1] == 0x01 && c[2] == 0x58 && c[3] == 0xC3) ++st.sehPairs;
                else st.warnings.push_back(F("thunks at %X and %X overlap", a, b));
            }
        }
        st.thunks = static_cast<uint32_t>(thunks.size());

        const Dll& d = p.dll;
        const std::string sha = Sha256Hex(file);
        const size_t dot = name.find_last_of('.');
        const std::string level = Lower(dot == std::string::npos ? name : name.substr(0, dot));
        std::string out;
        out.reserve(4 << 20);
        out += "--@archive " + level + "\n";
        out += "--@standard_anims off\n";   // (VERSION 2) the DLL registers its own animations, as on the DLL path
        out += F("-- %s translated by mission2lua %d (ConflictDesertStormModKit) from %s, SHA-256 %s.\n", name.c_str(),
                 kTranslatorVersion, name.c_str(), sha.c_str());
        out += "-- Generated from your own game files: game code. Never commit or distribute this file.\n";
        out += F("-- %u functions, %u instructions, %u thunks.", st.functions, st.instructions, st.thunks);
        std::string rt = Join(kRuntimeParts);
        const std::string ver = std::to_string(kTranslatorVersion);
        for (size_t at; (at = rt.find("%VERSION%")) != std::string::npos;) rt.replace(at, 9, ver);
        out += "\n" + rt;
        out += "\n-- ======================== functions ========================";
        for (const std::string& l : body) {
            out += '\n';
            out += l;
        }
        // image
        out += "\n-- ======================== image ========================";
        std::vector<std::string> secs;
        for (const Section& s : d.sections) {
            const size_t keep = s.vsize ? s.vsize : s.rawSize;
            std::vector<uint8_t> data(s.data.begin(), s.data.begin() + std::min(keep, s.data.size()));
            if (StartsWith(s.name, ".data"))
                while (!data.empty() && data.back() == 0) data.pop_back();
            if (StartsWith(s.name, ".reloc")) continue;
            secs.push_back("{" + H(s.va) + ", \"" + Hex(data) + "\"}");
        }
        std::string relocs;
        for (uint32_t r : d.relocs) {
            const uint32_t v = r - d.base;
            relocs += Hex({static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v >> 16),
                           static_cast<uint8_t>(v >> 24)});
        }
        std::vector<std::string> imps;
        for (const Import& im : d.imports)
            imps.push_back("{" + H(im.iat - d.base) + ", \"" + im.dll + "\", " +
                           (im.name.empty() ? std::to_string(im.ordinal) : "\"" + im.name + "\"") + "}");
        out += "\n" + F("local IMAGE = {size = %s, base = %s, text_lo = %s, text_hi = %s,", H(d.sizeOfImage).c_str(),
                        H(d.base).c_str(), H(d.textLo - d.base).c_str(), H(d.textHi - d.base).c_str());
        out += "\n  sections = {";
        for (size_t k = 0; k < secs.size(); ++k) out += (k ? ",\n    " : "") + secs[k];
        out += "},";
        out += "\n  relocs = \"" + relocs + "\",";
        out += "\n  thunks = {";
        for (size_t k = 0; k < thunks.size(); ++k) out += (k ? ", " : "") + H(thunks[k] - d.base);
        out += "},";
        out += "\n  imports = {";
        for (size_t k = 0; k < imps.size(); ++k) out += (k ? ",\n    " : "") + imps[k];
        out += "},";
        out += "\n}";
        out += "\n" + FormatMain(Join(kMainParts), {{"entry", H(p.entry)}, {"setup", H(setup->second)}, {"name", name}});
        out += "\n";
        *lua = std::move(out);
    } catch (const Broken& e) {
        if (error) *error = std::string("translator: ") + e.what();
        return false;
    } catch (const std::exception& e) {
        if (error) *error = std::string("translator: ") + e.what();
        return false;
    }
    st.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return true;
}

}  // namespace pse::m2l
