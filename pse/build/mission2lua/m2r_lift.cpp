// mission2lua_readable (C++ port): x86 -> IR statements for one function - mission2lua_readable.Lifter + the helpers
// around it (_fp_delta, _x87_regs, _imm, _fsw_parity, _fset, _fcond, fsw_zero_cond ...).
#include <cstdio>
#include <cstring>

#include "m2r.h"

namespace pse::m2l::rd {

// ---- small helpers ---------------------------------------------------------------------------------------------------
std::string Hex(int64_t v) {
    char buf[32];
    if (v < 0) std::snprintf(buf, sizeof buf, "-%llX", static_cast<unsigned long long>(-v));
    else std::snprintf(buf, sizeof buf, "%llX", static_cast<unsigned long long>(v));
    return buf;
}

Sym R32(uint16_t reg) {
    if (!IsReg32(reg)) return nullptr;
    return Y(RegName(reg));
}

SubReg Sub(uint16_t reg) {
    if (IsReg32(reg)) return {Y(RegName(reg)), 0, M32};
    if (IsReg16(reg)) return {Y(RegName(Full32(reg))), 0, 0xFFFF};
    if (IsLow8(reg)) return {Y(RegName(Full32(reg))), 0, 0xFF};
    if (IsHigh8(reg)) return {Y(RegName(Full32(reg))), 8, 0xFF};
    throw Unsupported{std::string("register ") + RegName(reg)};
}

Sym SL(int64_t off) {
    char buf[32];
    if (off >= 0) std::snprintf(buf, sizeof buf, "s%lld", static_cast<long long>(off));
    else std::snprintf(buf, sizeof buf, "sm%lld", static_cast<long long>(-off));
    return Y(buf);
}

namespace {

[[noreturn]] void Fail(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw Unsupported{buf};
}

const std::map<std::string, Sym>& JccMap() {   // _JCC: mnemonic -> condition code
    static const std::map<std::string, Sym> m = {
        {"jo", Y("o")},   {"jno", Y("no")}, {"jb", Y("b")},  {"jae", Y("ae")}, {"je", Y("e")},   {"jne", Y("ne")},
        {"jbe", Y("be")}, {"ja", Y("a")},   {"js", Y("s")},  {"jns", Y("ns")}, {"jp", Y("p")},   {"jnp", Y("np")},
        {"jl", Y("l")},   {"jge", Y("ge")}, {"jle", Y("le")}, {"jg", Y("g")}};
    return m;
}
const std::map<std::string, Sym>& SetMap() {   // _SET
    static const std::map<std::string, Sym> m = [] {
        std::map<std::string, Sym> o;
        for (auto& [k, v] : JccMap()) o["set" + k.substr(1)] = v;
        return o;
    }();
    return m;
}
Sym Jcc(const std::string& m) {
    auto it = JccMap().find(m);
    return it == JccMap().end() ? nullptr : it->second;
}
bool StartsF(const std::string& m) { return !m.empty() && m[0] == 'f'; }

int FpDelta(const Insn& i) {   // _fp_delta
    const std::string& m = i.mnemonic;
    if (m == "fld" || m == "fild" || m == "fldz" || m == "fld1" || m == "fldpi") return 1;
    if (m == "fstp" || m == "fistp" || m == "fcomp" || m == "fucomp" || m == "faddp" || m == "fsubp" || m == "fmulp" ||
        m == "fdivp" || m == "fsubrp" || m == "fdivrp")
        return -1;
    if (m == "fcompp" || m == "fucompp") return -2;
    return 0;
}

std::pair<int, int> X87Regs(const Insn& i) {   // _x87_regs
    const uint8_t b0 = i.bytes[0], b1 = i.bytes[1];
    const int k = b1 & 7;
    if (b0 == 0xD8) return {0, k};
    if (b0 == 0xDC || b0 == 0xDE) return {k, 0};
    Fail("x87 encoding %02X %02X", b0, b1);
}

bool PureAddr(E v) {   // _pure_addr
    Sym k = v->t;
    return k == s::api || k == s::saddr || k == s::gaddr || k == s::apislotaddr || k == s::apislotptr || k == s::c;
}
E CanonC(E c, int size) {   // _canon_c
    if (IsC(c)) return C(c->I(1) & ((1ll << (8 * size)) - 1));
    return c;
}
E SignedView(E r, int64_t size) { return size == 4 ? r : T(s::sx, r, size); }
E SxExpr(E v) {   // _sx_expr
    if (v->t == s::sx && IsC(v->X(1))) return C(Sx(v->X(1)->I(1), static_cast<int>(v->I(2))));
    return v;
}

// _fsw_parity: r = (fsw & mask) -> (a, b, mask in fsw bits)
bool FswParity(E r, E* a, E* b, int64_t* mask) {
    if (r->t == s::bin && r->S(1) == s::band && IsC(r->X(3))) {
        E x = r->X(2);
        const int64_t m = r->X(3)->I(1);
        if (x->t == s::ext && x->X(1)->t == s::fsw && x->I(2) == 8) {
            *a = x->X(1)->X(1);
            *b = x->X(1)->X(2);
            *mask = m << 8;
            return true;
        }
        if (x->t == s::fsw) {
            *a = x->X(1);
            *b = x->X(2);
            *mask = m;
            return true;
        }
    }
    return false;
}
enum { O_LT = 1, O_EQ = 2, O_GT = 4 };
E FSet(E a, E b, int taken) {   // _fset
    switch (taken) {
        case O_LT: return T(s::cmp, s::flt, a, b);
        case O_GT: return T(s::cmp, s::fgt, a, b);
        case O_EQ: return T(s::cmp, s::feq, a, b);
        case O_LT | O_EQ: return T(s::cmp, s::fle, a, b);
        case O_GT | O_EQ: return T(s::cmp, s::fge, a, b);
        case O_LT | O_GT: return T(s::cmp, s::fne, a, b);
        case O_LT | O_GT | O_EQ: return T(s::cmp, s::eq, C0, C0);
        default: return T(s::cmp, s::ne, C0, C0);
    }
}
int Popcount(int64_t v) {
    int n = 0;
    for (uint64_t x = static_cast<uint64_t>(v); x; x &= x - 1) ++n;
    return n;
}
const std::pair<int, int64_t> kOutBits[] = {{O_LT, 0x100}, {O_EQ, 0x4000}, {O_GT, 0}};   // _OUT_BITS
E FCond(E a, E b, bool parityTaken, int64_t mask) {   // _fcond
    int taken = 0;
    for (auto& [o, bits] : kOutBits) {
        const int64_t v = bits & mask;
        const bool par = (mask >> 8) ? Popcount((v >> 8) & 0xFF) % 2 == 0 : Popcount(v & 0xFF) % 2 == 0;
        if (par == parityTaken) taken |= o;
    }
    return FSet(a, b, taken);
}
E FswZeroCond(E r, Sym rel) {   // fsw_zero_cond
    E a, b;
    int64_t mask;
    if (!FswParity(r, &a, &b, &mask)) return nullptr;
    int taken = 0;
    for (auto& [o, bits] : kOutBits) {
        const bool nz = (bits & mask) != 0;
        if ((rel == s::ne) == nz) taken |= o;
    }
    return FSet(a, b, taken);
}

E Imm(Program& p, const Insn& i, const Operand& op) {   // _imm
    const int64_t v = op.imm & M32;
    if (i.imm_offset && p.p.dll.relocs.count(i.address + i.imm_offset)) {
        if (p.p.dll.InText(static_cast<uint32_t>(v))) return T(s::fnptr, v);
        return T(s::gaddr, v);
    }
    if (op.size < 4) return C(v & ((1ll << (8 * op.size)) - 1));
    return C(v);
}

E Loc(Sym n) { return T(s::loc, n); }
E Loc(const char* n) { return T(s::loc, Y(n)); }
Sym Name(const char* fmt, ...) {
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return Y(buf);
}

Sym K_eaxfsw() {
    static Sym k = Y("eax_fsw");
    return k;
}
Sym K_fswcmp() {
    static Sym k = Y("fswcmp");
    return k;
}
const Sym& R(const char* n) {   // register name symbols
    static std::map<std::string, Sym> m;
    auto it = m.find(n);
    if (it == m.end()) it = m.emplace(n, Y(n)).first;
    return it->second;
}

}  // namespace

std::vector<uint32_t> Rpo(const std::map<uint32_t, Block>& blocks, uint32_t entry) {   // missionflow._rpo
    std::set<uint32_t> seen;
    std::vector<uint32_t> out;
    struct Fr {
        uint32_t node;
        size_t next;
    };
    std::vector<Fr> stack{{entry, 0}};
    seen.insert(entry);
    while (!stack.empty()) {
        Fr& top = stack.back();
        const std::vector<uint32_t>& succ = blocks.at(top.node).succ;
        if (top.next >= succ.size()) {
            out.push_back(top.node);
            stack.pop_back();
            continue;
        }
        const uint32_t nxt = succ[top.next++];
        if (!seen.count(nxt)) {
            seen.insert(nxt);
            stack.push_back({nxt, 0});
        }
    }
    return {out.rbegin(), out.rend()};
}

Lifter::Lifter(Translator* t, Func* f) : t_(t), p_(&t->prog), f_(f) {}

Stmt* Lifter::New(const Stmt& st) { return t_->arena.New(st); }

namespace {
Stmt MkSet(uint32_t at, Sym loc, E e, Sym extra = nullptr) {
    Stmt st;
    st.kind = kd::set;
    st.at = at;
    st.loc = loc;
    st.e = e;
    st.extra = extra;
    return st;
}
Stmt MkStmt(Sym kind, uint32_t at, E e) {
    Stmt st;
    st.kind = kind;
    st.at = at;
    st.e = e;
    return st;
}
Stmt MkStore(uint32_t at, E target, E e, Sym extra = nullptr) {
    Stmt st;
    st.kind = kd::store;
    st.at = at;
    st.target = target;
    st.e = e;
    st.extra = extra;
    return st;
}
}  // namespace

// --- static esp / x87 depth ---
void Lifter::Frame() {
    Func& f = *f_;
    const std::vector<uint32_t> order = Rpo(f.blocks, f.va);
    std::map<uint32_t, StepOut> state;
    state[f.va] = {0, 0, false, 0};
    for (uint32_t va : order) {
        auto it = state.find(va);
        if (it == state.end()) continue;
        StepOut cur = it->second;
        const Block& b = f.blocks.at(va);
        for (size_t k = 0; k < b.insns.size(); ++k) {
            const Insn& i = *b.insns[k];
            f.espAt[i.address] = cur.esp;
            f.fpdAt[i.address] = cur.fpd;
            f.ebpAt[i.address] = {cur.ebpKnown, cur.ebp};
            cur = Step(i, cur, b.insns, k + 1);
        }
        for (uint32_t sc : b.succ) {
            auto st = state.find(sc);
            if (st != state.end()) {
                if (st->second.esp != cur.esp || st->second.fpd != cur.fpd)
                    Fail("stack mismatch at %X", sc);
            } else {
                state[sc] = cur;
            }
        }
    }
}

Lifter::StepOut Lifter::Step(const Insn& i, StepOut in, const std::vector<const Insn*>& insns, size_t k) {
    const std::string& m = i.mnemonic;
    const Operand* ops = i.ops;
    StepOut o = in;
    if (m == "push") {
        o.esp -= 4;
        return o;
    }
    if (m == "pop") {
        if (ops[0].type == OpType::Reg && R32(ops[0].reg) == R("ebp")) o.ebpKnown = false;
        o.esp += 4;
        return o;
    }
    if ((m == "sub" || m == "add") && ops[0].type == OpType::Reg && R32(ops[0].reg) == R("esp")) {
        if (ops[1].type != OpType::Imm) Fail("esp arithmetic at %X", i.address);
        const int64_t v = S32(ops[1].imm);
        o.esp = m == "sub" ? in.esp - v : in.esp + v;
        return o;
    }
    if (m == "mov" && ops[0].type == OpType::Reg && ops[1].type == OpType::Reg) {
        if (R32(ops[0].reg) == R("ebp") && R32(ops[1].reg) == R("esp")) {
            o.ebpKnown = true;
            o.ebp = in.esp;
            return o;
        }
        if (R32(ops[0].reg) == R("esp") && R32(ops[1].reg) == R("ebp")) {
            if (!in.ebpKnown) Fail("esp from an unknown ebp at %X", i.address);
            o.esp = in.ebp;
            return o;
        }
    }
    if (m == "leave") {
        if (!in.ebpKnown) Fail("leave with an unknown ebp at %X", i.address);
        o.esp = in.ebp + 4;
        o.ebpKnown = false;
        return o;
    }
    if (m == "call") {
        int64_t pop = 0;
        if (ops[0].type == OpType::Imm) {
            const uint32_t tva = static_cast<uint32_t>(ops[0].imm & M32);
            auto it = p_->funcs.find(tva);
            if (it != p_->funcs.end()) pop = it->second->retPop;
        }
        if (ReturnsFloat(insns, k, in.fpd)) {
            f_->floatCall.insert(i.address);
            o.fpd += 1;
        }
        o.esp = in.esp + 4 - 4 + pop;
        return o;
    }
    if (StartsF(m)) {
        o.fpd += FpDelta(i);
        return o;
    }
    return o;
}

bool Lifter::ReturnsFloat(const std::vector<const Insn*>& insns, size_t from, int64_t fpd) {
    for (size_t k = from; k < insns.size(); ++k) {
        const std::string& mm = insns[k]->mnemonic;
        if (mm == "call" || mm == "ret" || mm == "jmp" || Jcc(mm)) return false;
        if (StartsF(mm) && mm != "fnstsw" && mm != "fnstcw" && mm != "fldcw" && mm != "fnclex" && mm != "wait") {
            if (mm == "fld" || mm == "fild" || mm == "fldz" || mm == "fld1") return false;
            return fpd == 0;
        }
    }
    return false;
}

// --- operands ---
E Lifter::Stack(int64_t off) {
    for (auto& [lo, hi] : regions)
        if (lo <= off && off < hi) return T(s::mem, Simp(T(s::bin, s::plus, T(s::saddr, lo), C(off - lo))));
    return T(s::stack, off);
}

void Lifter::Escape(E v, const State* st) {
    if (st && !st->empty() && v && v->t == s::loc) {
        auto it = st->find(v->S(1));
        if (it != st->end()) v = it->second;
    }
    if (v && HasKind(v, s::saddr))
        for (E x : FindKind(v, s::saddr)) escaped.insert(x->I(1));
}

E Lifter::MemTarget(const Insn& i, const Operand& op, const State& st) {
    const int64_t disp = S32(op.disp);
    Sym base = op.base ? R32(op.base) : nullptr;
    Sym index = op.index ? R32(op.index) : nullptr;
    if (op.seg != R_NONE && op.seg != R_DS && op.seg != R_SS && op.seg != R_ES) Fail("segment operand at %X", i.address);
    const Func& f = *f_;
    const auto ebp = f.ebpAt.at(i.address);
    if (base == R("esp") && !index) return Stack(f.espAt.at(i.address) + disp);
    if (base == R("ebp") && !index && ebp.first) return Stack(ebp.second + disp);
    if ((base == R("esp") || base == R("ebp")) && index && (base == R("esp") || ebp.first)) {
        const int64_t off = (base == R("esp") ? f.espAt.at(i.address) : ebp.second) + disp;
        escaped.insert(off);
        E iv = op.scale == 1 ? Loc(index) : T(s::bin, s::mul, Loc(index), C(op.scale));
        return T(s::mem, Simp(T(s::bin, s::plus, T(s::saddr, off), iv)));
    }
    if (!base && !index) {
        const int64_t a = disp & M32;
        if (p_->hasApiGlobal && a == p_->apiGlobal) return T(s::apiglobal);
        return T(s::g, a);
    }
    E bv = nullptr;
    if (base) {
        auto it = st.find(base);
        if (it != st.end()) bv = it->second;
    }
    if (bv && !index) {
        if (bv->t == s::api && bv->v.empty()) return T(s::apislot, disp);
        if (bv->t == s::apislotaddr && disp == 0) return T(s::apislot, bv->I(1));
        if (bv->t == s::saddr) return Stack(bv->I(1) + disp);
        if (bv->t == s::gaddr) return T(s::g, (bv->I(1) + disp) & M32);
    }
    std::vector<E> parts;
    if (base) parts.push_back(Loc(base));
    if (index) {
        E iv = Loc(index);
        parts.push_back(op.scale == 1 ? iv : T(s::bin, s::mul, iv, C(op.scale)));
    }
    E addr = parts[0];
    for (size_t q = 1; q < parts.size(); ++q) addr = T(s::bin, s::plus, addr, parts[q]);
    if (disp) {
        const bool relocated = i.disp_offset && p_->p.dll.relocs.count(i.address + i.disp_offset);
        const bool asAddr = relocated || (p_->InImage(static_cast<uint32_t>(disp & M32)) && !base && !index);
        addr = T(s::bin, s::plus, addr, asAddr ? T(s::gaddr, disp & M32) : C(disp));
    }
    return T(s::mem, addr);
}

IR Lifter::LiftAll() {
    Func& f = *f_;
    const std::vector<uint32_t> order = Rpo(f.blocks, f.va);
    static Node qnode{Y("?"), {}, 0};
    const E Q = &qnode;   // "?": a predecessor not lifted yet
    std::map<uint32_t, E> ex;
    std::map<uint32_t, State> exs;
    IR out;
    struct Pending {
        uint32_t pv;
        Sym name;
        V val;
    };
    std::vector<Pending> pending;
    for (uint32_t va : order) {
        const Block& b = f.blocks.at(va);
        E fl = nullptr;
        std::vector<E> pf;
        for (uint32_t p : b.preds) {
            auto it = ex.find(p);
            pf.push_back(it == ex.end() ? Q : it->second);
        }
        if (!pf.empty()) {
            bool all = true;
            for (E x : pf) all &= x == pf[0] && x != Q;
            if (all) fl = pf[0];
        }
        if (!fl && !pf.empty()) {
            bool ok = true;
            for (E x : pf) ok &= x != Q && x != nullptr;
            if (ok) {
                std::set<std::pair<Sym, size_t>> shapes;
                std::set<E> distinct;
                for (E x : pf) {
                    shapes.insert({x->t, x->n()});
                    distinct.insert(x);
                }
                if (shapes.size() == 1 && distinct.size() > 1) {
                    std::vector<V> merged;
                    for (size_t k = 1; k < pf[0]->n(); ++k) {
                        bool same = true;
                        for (E x : pf) same &= x->at(k) == pf[0]->at(k);
                        if (same || pf[0]->at(k).k != VK::Ex) {
                            merged.push_back(pf[0]->at(k));
                        } else {
                            Sym name = Name("flm%s_%d", Hex(va).c_str(), static_cast<int>(k));
                            merged.emplace_back(Loc(name));
                            for (size_t q = 0; q < b.preds.size(); ++q) pending.push_back({b.preds[q], name, pf[q]->at(k)});
                        }
                    }
                    fl = Mk(pf[0]->t, merged);
                }
            }
        }
        State st0;
        bool allKnown = !b.preds.empty();
        for (uint32_t p : b.preds) allKnown &= exs.count(p) != 0;
        if (allKnown) {
            const State& first = exs.at(b.preds[0]);
            for (auto& [k, v] : first) {
                bool keep = true;
                for (size_t q = 1; q < b.preds.size(); ++q) {
                    const State& o = exs.at(b.preds[q]);
                    auto it = o.find(k);
                    keep &= it != o.end() && it->second == v;
                }
                if (keep) st0[k] = v;
            }
        }
        out[va] = LiftBlock(b, fl, st0);
        ex[va] = exitFlags_;
        exs[va] = exitSt_;
    }
    for (const Pending& pm : pending) {
        StmtList& stmts = out[pm.pv];
        const uint32_t at = stmts.empty() ? pm.pv : stmts.back()->at;
        const size_t pos = (!stmts.empty() && (stmts.back()->kind == kd::cond || stmts.back()->kind == kd::sw))
                               ? stmts.size() - 1
                               : stmts.size();
        stmts.insert(stmts.begin() + pos, New(MkSet(at, pm.name, pm.val.e)));
    }
    return out;
}

// --- reads / writes ---
E Lifter::Read(const Insn& i, const Operand& op, State& st, StmtList& out) {
    (void)out;
    if (op.type == OpType::Imm) return Imm(*p_, i, op);
    if (op.type == OpType::Reg) {
        SubReg r = Sub(op.reg);
        if (r.name == R("esp")) return T(s::saddr, f_->espAt.at(i.address));
        E v = Loc(r.name);
        if (r.mask == M32) return v;
        return T(s::ext, v, static_cast<int64_t>(r.sh), r.mask);
    }
    if (op.type == OpType::Mem) {
        E mt = MemTarget(i, op, st);
        const int64_t size = op.size;
        if (mt->t == s::stack) {
            const int64_t off = mt->I(1);
            if (size == 4 && (apiSlotsIn.count(off) || (off == 4 && f_->kind == kd::setup))) return T(s::api);
            if (size == 4) return Loc(SL(off));
            if (size == 8) return T(s::qword, Loc(SL(off)), Loc(SL(off + 4)));
            const int64_t base = off & ~3ll;
            return T(s::ext, Loc(SL(base)), 8 * (off - base), (1ll << (8 * size)) - 1);
        }
        if (mt->t == s::apiglobal) return T(s::api);
        if (mt->t == s::apislot) return T(s::apislotptr, mt->I(1));
        if (mt->t == s::g) return t_->GlobalRead(static_cast<uint32_t>(mt->I(1)), size, size == 4 ? s::i : s::u);
        return T(s::mem, mt->X(1), size, size == 4 ? s::i : s::u);
    }
    Fail("operand at %X", i.address);
}

void Lifter::Write(const Insn& i, const Operand& op, E val, State& st, StmtList& out, bool full) {
    const uint32_t at = i.address;
    if (op.type == OpType::Reg) {
        SubReg r = Sub(op.reg);
        if (r.name == R("eax")) st.erase(K_eaxfsw());
        if (full || r.mask == M32) {
            out.push_back(New(MkSet(at, r.name, val)));
            const bool gp = r.name == R("esi") || r.name == R("edi") || r.name == R("ebx") || r.name == R("eax") ||
                            r.name == R("ecx") || r.name == R("edx");
            if (gp && PureAddr(val)) st[r.name] = val;
            else st.erase(r.name);
            return;
        }
        out.push_back(New(MkSet(at, r.name, Simp(T(s::ins, Loc(r.name), val, static_cast<int64_t>(r.sh), r.mask)))));
        st.erase(r.name);
        return;
    }
    if (op.type == OpType::Mem) {
        E mt = MemTarget(i, op, st);
        const int64_t size = op.size;
        if (mt->t == s::stack) {
            Escape(val, &st);
            const int64_t off = mt->I(1);
            if (size == 4) {
                out.push_back(New(MkSet(at, SL(off), val)));
            } else if (size == 8) {
                Fail("qword stack store at %X", at);
            } else {
                const int64_t base = off & ~3ll;
                out.push_back(New(MkSet(
                    at, SL(base), Simp(T(s::ins, Loc(SL(base)), val, 8 * (off - base), (1ll << (8 * size)) - 1)))));
            }
            return;
        }
        if (mt->t == s::g) {
            Escape(val, &st);
            out.push_back(New(MkStore(at, T(s::g, mt->I(1), size), val)));
            return;
        }
        if (mt->t == s::apiglobal) {
            out.push_back(New(MkStore(at, T(s::apiglobal), val)));
            return;
        }
        if (mt->t == s::mem) {
            Escape(val, &st);
            out.push_back(New(MkStore(at, T(s::mem, mt->X(1), size), val)));
            return;
        }
        Fail("write to %s at %X", mt->t->c_str(), at);
    }
    Fail("write operand at %X", at);
}

// --- conditions ---
E Lifter::Carry(E flags) {
    if (!flags) throw Unsupported{"carry without flags"};
    if (flags->t == s::sub) return T(s::cmp, s::ult, flags->X(1), flags->X(2));
    if (flags->t == s::neg) return T(s::cmp, s::ne, flags->X(1), C0);
    if (flags->t == s::res) return T(s::cmp, s::ne, C0, C0);
    throw Unsupported{"carry from " + *flags->t};
}

E Lifter::Cond(Sym cc, E flags, uint32_t at) {
    if (!flags) Fail("condition %s without flags at %X", cc->c_str(), at);
    Sym kind = flags->t;
    const std::string& c = *cc;
    if (kind == s::sub) {
        E a = flags->X(1), b = flags->X(2);
        static const std::map<std::string, Sym> rels = {{"e", s::eq},  {"ne", s::ne}, {"l", s::lt},  {"ge", s::ge},
                                                        {"le", s::le}, {"g", s::gt},  {"b", s::ult}, {"ae", s::uge},
                                                        {"be", s::ule}, {"a", s::ugt}};
        auto it = rels.find(c);
        if (it != rels.end()) return Simp(T(s::cmp, it->second, a, b));
        if (c == "s" || c == "ns") {
            E r = flags->X(4) ? flags->X(4) : Simp(T(s::bin, s::minus, a, b));
            return Simp(T(s::cmp, c == "s" ? s::lt : s::ge, r, C0));
        }
        Fail("condition %s after cmp at %X", cc->c_str(), at);
    }
    if (kind == s::add) {
        E r = flags->X(4);
        if (c == "e" || c == "ne") return Simp(T(s::cmp, c == "e" ? s::eq : s::ne, r, C0));
        if (c == "s" || c == "ns") return Simp(T(s::cmp, c == "s" ? s::lt : s::ge, SignedView(r, flags->I(3)), C0));
        Fail("condition %s after add at %X", cc->c_str(), at);
    }
    if (kind == s::neg) {
        E a = flags->X(1);
        if (c == "e" || c == "ne") return Simp(T(s::cmp, c == "e" ? s::eq : s::ne, a, C0));
        if (c == "b" || c == "ae") return Simp(T(s::cmp, c == "b" ? s::ne : s::eq, a, C0));
        Fail("condition %s after neg at %X", cc->c_str(), at);
    }
    if (kind == s::res) {
        E r = flags->X(1);
        const int64_t size = flags->I(2);
        if (c == "e" || c == "ne") {
            E fc = FswZeroCond(r, c == "e" ? s::eq : s::ne);
            if (fc) return fc;
        }
        E sv = SignedView(r, size);
        if (c == "e") return Simp(T(s::cmp, s::eq, r, C0));
        if (c == "ne") return Simp(T(s::cmp, s::ne, r, C0));
        if (c == "s" || c == "l") return Simp(T(s::cmp, s::lt, sv, C0));
        if (c == "ns" || c == "ge") return Simp(T(s::cmp, s::ge, sv, C0));
        if (c == "le") return Simp(T(s::cmp, s::le, sv, C0));
        if (c == "g") return Simp(T(s::cmp, s::gt, sv, C0));
        if (c == "b" || c == "o") return T(s::cmp, s::ne, C0, C0);
        if (c == "ae" || c == "no") return T(s::cmp, s::eq, C0, C0);
        if (c == "be") return Simp(T(s::cmp, s::eq, r, C0));
        if (c == "a") return Simp(T(s::cmp, s::ne, r, C0));
        if (c == "p" || c == "np") {
            E a, b;
            int64_t mask;
            if (FswParity(r, &a, &b, &mask)) return FCond(a, b, c == "p", mask);
        }
        Fail("condition %s after logic op at %X", cc->c_str(), at);
    }
    if (kind == s::sahf) {
        E fsw = flags->X(1);
        E a = fsw->X(1), b = fsw->X(2);
        int taken = -1;
        if (c == "b") taken = O_LT;
        else if (c == "ae") taken = O_EQ | O_GT;
        else if (c == "e") taken = O_EQ;
        else if (c == "ne") taken = O_LT | O_GT;
        else if (c == "be") taken = O_LT | O_EQ;
        else if (c == "a") taken = O_GT;
        if (taken < 0) Fail("condition %s after sahf at %X", cc->c_str(), at);
        return FSet(a, b, taken);
    }
    Fail("flags %s at %X", kind->c_str(), at);
}

// --- calls ---
void Lifter::Call(const Insn& i, State& st, StmtList& out, int64_t esp, int64_t fpd) {
    const uint32_t at = i.address;
    const Operand& op = i.ops[0];
    E target = nullptr;
    if (op.type == OpType::Imm) {
        const uint32_t tva = static_cast<uint32_t>(op.imm & M32);
        if (p_->cookie_.count(tva)) return;
        if (p_->sprintf_.count(tva)) target = T(s::sprintf);
        else if (p_->rand_.count(tva)) target = T(s::rand);
        else if (p_->funcs.count(tva)) target = T(s::fn, static_cast<int64_t>(tva));
        else Fail("call to %X at %X", tva, at);
    } else {
        E mt = op.type == OpType::Mem ? MemTarget(i, op, st) : nullptr;
        if (op.type == OpType::Reg) {
            Sym rn = R32(op.reg);
            auto it = rn ? st.find(rn) : st.end();
            E v = it != st.end() ? it->second : nullptr;
            if (v && v->t == s::apislotaddr) mt = T(s::apislot, v->I(1));
            else if (v && v->t == s::apislotptr) mt = T(s::apislot, v->I(1));
            else mt = T(s::reg, rn);
        }
        if (mt && mt->t == s::apislot) target = T(s::slot, mt->I(1));
        else if (mt && mt->t == s::stack) target = T(s::ptr, Loc(SL(mt->I(1))));
        else if (mt && mt->t == s::reg) target = T(s::ptr, Loc(mt->S(1)));
        else if (mt && mt->t == s::mem) target = T(s::ptr, T(s::mem, mt->X(1), 4, s::i));
        else Fail("call %s at %X", i.op_str.c_str(), at);
    }
    const int64_t args = t_->CallArgs(target, esp, at, f_);
    Sym ret = f_->floatCall.count(at) ? s::f : s::i;
    std::vector<E> a;
    for (int64_t k = 0; k < args; ++k) a.push_back(Loc(SL(esp + 4 * k)));
    E e = MkCall(target, a, f_->va, at, ret);
    out.push_back(New(MkSet(at, R("eax"), e, kd::call)));
    std::set<Sym> clob;
    if (target->t == s::fn) clob = t_->ClobbersOr(true, static_cast<uint32_t>(target->I(1)), {"ecx", "edx"});
    else clob = {R("ecx"), R("edx")};
    for (const char* r : {"ecx", "edx"})
        if (clob.count(R(r))) out.push_back(New(MkSet(at, R(r), T(s::undef, R(r)))));
    if (ret == s::f) {
        out.push_back(New(MkSet(at, Name("f%lld", static_cast<long long>(fpd)), Loc("eax"))));
        out.push_back(New(MkSet(at, R("eax"), T(s::undef, R("eax")))));
    }
}

void Lifter::Tailcall(const Insn& i, E target, StmtList& out, int64_t esp) {
    const uint32_t at = i.address;
    const int64_t n = t_->CallArgs(target, esp + 4, at, f_);
    std::vector<E> a;
    for (int64_t k = 0; k < n; ++k) a.push_back(Loc(SL(esp + 4 + 4 * k)));
    out.push_back(New(MkStmt(kd::tail, at, MkCall(target, a, f_->va, at, s::i))));
}

void Lifter::StringOp(const Insn& i, StmtList& out) {
    const uint32_t at = i.address;
    if (i.mnemonic == "rep movsd") {
        Stmt cp;
        cp.kind = kd::copy;
        cp.at = at;
        cp.ce[0] = Loc("edi");
        cp.ce[1] = Loc("esi");
        cp.ce[2] = T(s::bin, s::mul, Loc("ecx"), C(4));
        out.push_back(New(cp));
        out.push_back(New(MkSet(at, R("esi"), Simp(T(s::bin, s::plus, Loc("esi"), T(s::bin, s::mul, Loc("ecx"), C(4)))))));
        out.push_back(New(MkSet(at, R("edi"), Simp(T(s::bin, s::plus, Loc("edi"), T(s::bin, s::mul, Loc("ecx"), C(4)))))));
        out.push_back(New(MkSet(at, R("ecx"), C0)));
        return;
    }
    Fail("%s at %X", i.mnemonic.c_str(), at);
}

// --- the block ---
StmtList Lifter::LiftBlock(const Block& b, E flags, const State& stIn) {
    StmtList out;
    State st = stIn;
    const Func& f = *f_;
    for (size_t k = 0; k < b.insns.size(); ++k) {
        const Insn& i = *b.insns[k];
        const std::string& m = i.mnemonic;
        const Operand* ops = i.ops;
        const uint32_t at = i.address;
        const int64_t esp = f.espAt.at(at);
        const int64_t fpd = f.fpdAt.at(at);
        auto Rd = [&](const Operand& op) { return Read(i, op, st, out); };
        auto Wr = [&](const Operand& op, E val) { Write(i, op, val, st, out); };
        const size_t mark = out.size();
        auto SNAP = [&](E fl) -> E {
            std::vector<V> vals;
            for (const V& x : fl->v) {
                if (x.k == VK::Ex && !Stable(x.e) && !HasKind(x.e, s::fsw)) {
                    Sym name = Name("fl%s_%d", Hex(at).c_str(), static_cast<int>(vals.size()));
                    const size_t pos = std::min(out.size(), mark + vals.size());
                    out.insert(out.begin() + pos, New(MkSet(at, name, x.e)));
                    vals.emplace_back(Loc(name));
                } else {
                    vals.push_back(x);
                }
            }
            return Mk(fl->t, vals);
        };
        auto FSW = [&](E v) -> E {
            auto it = st.find(K_eaxfsw());
            if (it == st.end()) return v;
            return Simp(Subst(v, {{R("eax"), it->second}}));
        };
        auto IsRegOp = [&](int n, const char* name) {
            return ops[n].type == OpType::Reg && R32(ops[n].reg) == R(name);
        };
        auto R32Of = [&](int n) -> Sym { return ops[n].type == OpType::Reg ? R32(ops[n].reg) : nullptr; };

        if (m == "nop") continue;
        if (m == "push") {
            E v = Rd(ops[0]);
            Escape(v, &st);
            if (ops[0].size == 2) Fail("push16 at %X", at);
            out.push_back(New(MkSet(at, SL(esp - 4), v)));
            continue;
        }
        if (m == "pop") {
            Wr(ops[0], Loc(SL(esp)));
            continue;
        }
        if (m == "mov" || m == "movzx" || m == "movsx") {
            if ((ops[0].type == OpType::Reg && (R32Of(0) == R("esp") || R32Of(0) == R("ebp"))) ||
                (IsRegOp(1, "esp") && IsRegOp(0, "ebp"))) {
                if (R32Of(0) == R("ebp") && R32Of(1) == R("esp")) continue;
                if (R32Of(0) == R("esp")) continue;
            }
            E v;
            if (IsRegOp(1, "esp")) v = T(s::saddr, esp);
            else v = Rd(ops[1]);
            if (m == "movsx") {
                v = !IsC(v) ? T(s::sx, v, static_cast<int64_t>(ops[1].size)) : C(Sx(v->I(1), ops[1].size));
                v = SxExpr(v);
            }
            if (m == "movzx" || m == "movsx") Write(i, ops[0], v, st, out, true);
            else Wr(ops[0], v);
            continue;
        }
        if (m == "lea") {
            E mt = MemTarget(i, ops[1], st);
            E v;
            if (mt->t == s::stack) v = T(s::saddr, mt->I(1));
            else if (mt->t == s::g) v = T(s::gaddr, mt->I(1));
            else if (mt->t == s::apislot) v = T(s::apislotaddr, mt->I(1));
            else if (mt->t == s::mem) v = mt->X(1);
            else Fail("lea at %X", at);
            Wr(ops[0], v);
            continue;
        }
        if (m == "xchg") {
            E a = Rd(ops[0]), c = Rd(ops[1]);
            Sym tmp = Name("t%s", Hex(at).c_str());
            out.push_back(New(MkSet(at, tmp, a)));
            Wr(ops[0], c);
            Wr(ops[1], Loc(tmp));
            continue;
        }
        if (m == "add" || m == "sub" || m == "and" || m == "or" || m == "xor" || m == "adc" || m == "sbb") {
            if (IsRegOp(0, "esp")) continue;
            E a = Rd(ops[0]), c = Rd(ops[1]);
            const int size = ops[0].size;
            if (m == "xor" && ops[0].type == OpType::Reg && ops[1].type == OpType::Reg && ops[0].reg == ops[1].reg) {
                Wr(ops[0], C0);
                flags = SNAP(T(s::res, C0, static_cast<int64_t>(size)));
                continue;
            }
            if (m == "adc" || m == "sbb") {
                E cf = Carry(flags);
                E r;
                if (m == "sbb" && ops[0].type == OpType::Reg && ops[1].type == OpType::Reg && ops[0].reg == ops[1].reg) {
                    r = T(s::un, s::neg_, T(s::b2i, cf));
                } else {
                    Sym op = m == "adc" ? s::plus : s::minus;
                    r = T(s::bin, op, T(s::bin, op, a, c), T(s::b2i, cf));
                }
                Wr(ops[0], Simp(r));
                flags = nullptr;
                continue;
            }
            if (m == "and") a = FSW(a);
            Sym op = m == "add" ? s::plus : m == "sub" ? s::minus : m == "and" ? s::band : m == "or" ? s::bor : s::bxor;
            if (size < 4) c = CanonC(c, size);
            E r = T(s::bin, op, a, c);
            if (size < 4 && (op == s::plus || op == s::minus)) r = T(s::bin, s::band, r, C((1ll << (8 * size)) - 1));
            Wr(ops[0], Simp(r));
            if (m == "add" || m == "sub")
                flags = SNAP(T(m == "sub" ? s::sub : s::add, a, c, static_cast<int64_t>(size), Simp(r)));
            else
                flags = SNAP(T(s::res, Simp(r), static_cast<int64_t>(size)));
            continue;
        }
        if (m == "inc" || m == "dec") {
            E a = Rd(ops[0]);
            E r = Simp(T(s::bin, m == "inc" ? s::plus : s::minus, a, C1));
            if (ops[0].size < 4) r = Simp(T(s::bin, s::band, r, C((1ll << (8 * ops[0].size)) - 1)));
            Wr(ops[0], r);
            flags = m == "dec" ? SNAP(T(s::sub, a, C1, static_cast<int64_t>(ops[0].size), r))
                               : SNAP(T(s::add, a, C1, static_cast<int64_t>(ops[0].size), r));
            continue;
        }
        if (m == "neg") {
            E a = Rd(ops[0]);
            E r = Simp(T(s::un, s::neg_, a));
            Wr(ops[0], r);
            flags = SNAP(T(s::neg, a, static_cast<int64_t>(ops[0].size), r));
            continue;
        }
        if (m == "not") {
            E a = Rd(ops[0]);
            Wr(ops[0], Simp(T(s::un, s::notop, a)));
            continue;
        }
        if (m == "shl" || m == "sal" || m == "shr" || m == "sar") {
            E a = Rd(ops[0]);
            E c = i.nops > 1 ? Rd(ops[1]) : C1;
            Sym op = (m == "shl" || m == "sal") ? s::shl : m == "shr" ? s::shr : s::sar;
            E r = Simp(T(s::bin, op, a, c));
            Wr(ops[0], r);
            flags = SNAP(T(s::res, r, static_cast<int64_t>(ops[0].size)));
            continue;
        }
        if (m == "imul") {
            E r;
            if (i.nops == 3) r = Simp(T(s::bin, s::mul, Rd(ops[1]), Rd(ops[2])));
            else if (i.nops == 2) r = Simp(T(s::bin, s::mul, Rd(ops[0]), Rd(ops[1])));
            else Fail("one-operand imul at %X", at);
            Wr(ops[0], r);
            flags = nullptr;
            continue;
        }
        if (m == "cdq") {
            out.push_back(New(MkSet(at, R("edx"), T(s::bin, s::sar, Loc("eax"), C(31)))));
            continue;
        }
        if (m == "mul") {
            E d_ = Rd(ops[0]);
            E num = Loc("eax");
            Sym tmph = Name("h%s", Hex(at).c_str()), tmpl = Name("l%s", Hex(at).c_str());
            out.push_back(New(MkSet(at, tmph, T(s::bin, s::umulhi, num, d_))));
            out.push_back(New(MkSet(at, tmpl, Simp(T(s::bin, s::mul, num, d_)))));
            out.push_back(New(MkSet(at, R("eax"), Loc(tmpl))));
            out.push_back(New(MkSet(at, R("edx"), Loc(tmph))));
            flags = nullptr;
            continue;
        }
        if (m == "idiv" || m == "div") {
            E d_ = Rd(ops[0]);
            E num = Loc("eax");
            E q = T(s::bin, m == "idiv" ? s::idiv : s::udiv, num, d_);
            E rm = T(s::bin, m == "idiv" ? s::imod : s::umod, num, d_);
            Sym tq = Name("q%s", Hex(at).c_str()), tr = Name("r%s", Hex(at).c_str());
            out.push_back(New(MkSet(at, tq, q)));
            out.push_back(New(MkSet(at, tr, rm)));
            out.push_back(New(MkSet(at, R("eax"), Loc(tq))));
            out.push_back(New(MkSet(at, R("edx"), Loc(tr))));
            continue;
        }
        if (m == "cmp" || m == "test") {
            E a = FSW(Rd(ops[0])), c = Rd(ops[1]);
            if (ops[0].size < 4) c = CanonC(c, ops[0].size);
            if (m == "cmp") {
                flags = SNAP(T(s::sub, a, c, static_cast<int64_t>(ops[0].size), V()));
            } else if (ops[0].type == OpType::Reg && ops[1].type == OpType::Reg && ops[0].reg == ops[1].reg) {
                flags = SNAP(T(s::res, a, static_cast<int64_t>(ops[0].size)));
            } else {
                flags = SNAP(T(s::res, Simp(T(s::bin, s::band, a, c)), static_cast<int64_t>(ops[0].size)));
            }
            continue;
        }
        if (auto it = SetMap().find(m); it != SetMap().end()) {
            E cnd = Cond(it->second, flags, at);
            Wr(ops[0], Simp(T(s::b2i, cnd)));
            continue;
        }
        if (m == "call") {
            Call(i, st, out, esp, fpd);
            flags = nullptr;
            const bool imm = ops[0].type == OpType::Imm;
            std::set<Sym> clob =
                t_->ClobbersOr(imm, imm ? static_cast<uint32_t>(ops[0].imm & M32) : 0, {"eax", "ecx", "edx"});
            clob.insert(R("eax"));
            for (const char* r : {"eax", "ecx", "edx"})
                if (clob.count(R(r))) st.erase(R(r));
            continue;
        }
        if (m == "rep movsd" || m == "rep movsb" || m == "movsd" || m == "movsb" || m == "rep stosd" ||
            m == "rep stosb" || m == "stosd" || m == "stosb") {
            StringOp(i, out);
            continue;
        }
        if (m == "ret" || m == "retn") {
            out.push_back(New(MkStmt(kd::ret, at, Loc("eax"))));
            continue;
        }
        if (m == "leave") continue;
        if (m == "jmp") {
            const Operand& op = ops[0];
            if (b.kind == kd::tail) {
                Tailcall(i, T(s::fn, static_cast<int64_t>(b.tail)), out, esp);
            } else if (b.kind == kd::tailmem) {
                E mt = MemTarget(i, op, st);
                if (mt->t == s::apislot) Tailcall(i, T(s::slot, mt->I(1)), out, esp);
                else Fail("indirect tail jump at %X", at);
            } else if (b.kind == kd::sw) {
                Stmt sw = MkStmt(kd::sw, at, Loc(b.table->indexReg));
                sw.table = b.table;
                out.push_back(New(sw));
            }
            continue;
        }
        if (Sym cc = Jcc(m)) {
            out.push_back(New(MkStmt(kd::cond, at, Cond(cc, flags, at))));
            continue;
        }
        if (m == "sahf") {
            if (flags && flags->t == s::fswah) {
                flags = T(s::sahf, flags->X(1));
            } else {
                auto it = st.find(K_eaxfsw());
                if (it == st.end()) Fail("sahf without fnstsw at %X", at);
                flags = T(s::sahf, it->second);
            }
            continue;
        }
        if (StartsF(m)) {
            flags = Fpu(i, st, out, fpd, flags);
            continue;
        }
        if (m == "cld" || m == "std" || m == "wait") continue;
        Fail("%s at %X", m.c_str(), at);
    }
    exitFlags_ = flags;
    exitSt_ = st;
    return out;
}

// --- x87 ---
E Lifter::Fpu(const Insn& i, State& st, StmtList& out, int64_t fpd, E flags) {
    const std::string& m = i.mnemonic;
    const Operand* ops = i.ops;
    const uint32_t at = i.address;
    auto top = [&](int64_t k = 0) { return Name("f%lld", static_cast<long long>(fpd - 1 - k)); };
    auto stk = [&](const Operand& op) -> int64_t { return op.reg - R_ST0; };
    auto srcVal = [&](const Operand& op) -> E {
        if (op.type == OpType::Reg) return Loc(top(stk(op)));
        E mt = MemTarget(i, op, st);
        const int64_t size = op.size;
        if (mt->t == s::stack) {
            if (size == 4) return T(s::fromfbits, Loc(SL(mt->I(1))));
            if (size == 8) return T(s::fromdbits, Loc(SL(mt->I(1))), Loc(SL(mt->I(1) + 4)));
            Fail("x87 stack operand size %d at %X", static_cast<int>(size), at);
        }
        if (mt->t == s::g) {
            if (size == 4) return t_->GlobalRead(static_cast<uint32_t>(mt->I(1)), 4, s::f);
            if (size == 8) return t_->GlobalRead(static_cast<uint32_t>(mt->I(1)), 8, s::d);
        }
        if (mt->t == s::mem) return T(s::mem, mt->X(1), size, size == 4 ? s::f : s::d);
        Fail("x87 operand at %X", at);
    };
    auto intVal = [&](const Operand& op) -> E {
        E mt = MemTarget(i, op, st);
        const int64_t size = op.size;
        if (mt->t == s::stack) {
            const int64_t off = mt->I(1);
            if (size == 4 && off == 4 && f_->kind == kd::setup) return T(s::api);
            if (size == 4) return Loc(SL(off));
            if (size == 8) return T(s::qword, Loc(SL(off)), Loc(SL(off + 4)));
            if (size == 2) {
                const int64_t base = off & ~3ll;
                return T(s::sx, T(s::ext, Loc(SL(base)), 8 * (off - base), 0xFFFFll), 2ll);
            }
        }
        if (mt->t == s::g) return t_->GlobalRead(static_cast<uint32_t>(mt->I(1)), size, s::i);
        if (mt->t == s::mem) return T(s::mem, mt->X(1), size, s::i);
        Fail("x87 int operand at %X", at);
    };
    auto store = [&](const Operand& op, E v, bool asInt) {
        if (op.type == OpType::Reg) {
            out.push_back(New(MkSet(at, top(stk(op)), v)));
            return;
        }
        E mt = MemTarget(i, op, st);
        const int64_t size = op.size;
        E val = asInt ? T(s::fistp, v) : (size == 4 ? T(s::f32, v) : v);
        if (mt->t == s::stack) {
            if (size == 4) {
                out.push_back(New(MkSet(at, SL(mt->I(1)), asInt ? val : T(s::f32, v))));
                return;
            }
            if (size == 8 && !asInt) {
                out.push_back(New(MkSet(at, SL(mt->I(1)), T(s::dlo, v))));
                out.push_back(New(MkSet(at, SL(mt->I(1) + 4), T(s::dhi, v))));
                return;
            }
            Fail("x87 store size %d at %X", static_cast<int>(size), at);
        }
        if (mt->t == s::g) {
            out.push_back(New(MkStore(at, T(s::g, mt->I(1), size), val, asInt ? nullptr : s::f)));
            return;
        }
        if (mt->t == s::mem) {
            out.push_back(New(MkStore(at, T(s::mem, mt->X(1), size), val, asInt ? nullptr : s::f)));
            return;
        }
        Fail("x87 store at %X", at);
    };
    static const std::map<std::string, std::string> arith = {
        {"fadd", "+"},   {"fsub", "-"},   {"fmul", "*"},   {"fdiv", "/"},   {"fsubr", "r-"},   {"fdivr", "r/"},
        {"faddp", "+"},  {"fsubp", "-"},  {"fmulp", "*"},  {"fdivp", "/"},  {"fsubrp", "r-"},  {"fdivrp", "r/"},
        {"fiadd", "+"},  {"fisub", "-"},  {"fimul", "*"},  {"fidiv", "/"},  {"fisubr", "r-"},  {"fidivr", "r/"}};
    if (m == "fld") {
        E v = ops[0].type == OpType::Reg ? Loc(top(stk(ops[0]))) : srcVal(ops[0]);
        out.push_back(New(MkSet(at, Name("f%lld", static_cast<long long>(fpd)), v)));
        return flags;
    }
    if (m == "fild") {
        E v = intVal(ops[0]);
        out.push_back(New(MkSet(at, Name("f%lld", static_cast<long long>(fpd)), T(s::i2f, v))));
        return flags;
    }
    if (m == "fldz" || m == "fld1") {
        out.push_back(New(MkSet(at, Name("f%lld", static_cast<long long>(fpd)), T(s::fc, m == "fldz" ? 0.0 : 1.0))));
        return flags;
    }
    if (m == "fst" || m == "fstp") {
        store(ops[0], Loc(top()), false);
        return flags;
    }
    if (m == "fist" || m == "fistp") {
        store(ops[0], Loc(top()), true);
        return flags;
    }
    if (auto it = arith.find(m); it != arith.end()) {
        std::string op = it->second;
        E a, c;
        Sym dst;
        if (m.compare(0, 2, "fi") == 0) {
            a = Loc(top());
            c = T(s::i2f, intVal(ops[0]));
            dst = top();
        } else if (i.nops == 2) {
            auto [d0, s0] = X87Regs(i);
            a = Loc(top(d0));
            c = Loc(top(s0));
            dst = top(d0);
        } else if (i.nops == 1 && ops[0].type == OpType::Mem) {
            a = Loc(top());
            c = srcVal(ops[0]);
            dst = top();
        } else if (i.nops == 1) {
            a = Loc(top(stk(ops[0])));
            c = Loc(top());
            dst = top(stk(ops[0]));
        } else {
            a = Loc(top(1));
            c = Loc(top());
            dst = top(1);
        }
        if (op[0] == 'r') {
            std::swap(a, c);
            op = op.substr(1);
        }
        out.push_back(New(MkSet(at, dst, T(s::fr, T(s::bin, Y(op), a, c)))));
        return flags;
    }
    if (m == "fcom" || m == "fcomp" || m == "fcompp" || m == "fucom" || m == "fucomp" || m == "fucompp") {
        E c;
        if (i.nops) c = ops[0].type == OpType::Mem ? srcVal(ops[0]) : Loc(top(stk(ops[0])));
        else c = Loc(top(1));
        Sym tmpa = Name("ca%s", Hex(at).c_str()), tmpb = Name("cb%s", Hex(at).c_str());
        out.push_back(New(MkSet(at, tmpa, Loc(top()))));
        out.push_back(New(MkSet(at, tmpb, c)));
        st[K_fswcmp()] = T(s::pair, Loc(tmpa), Loc(tmpb));
        return flags;
    }
    if (m == "fnstsw") {
        auto it = st.find(K_fswcmp());
        if (it == st.end()) Fail("fnstsw without compare at %X", at);
        E v = T(s::fsw, it->second->X(1), it->second->X(2));
        out.push_back(New(MkSet(at, R("eax"), Simp(T(s::ins, Loc("eax"), v, 0ll, 0xFFFFll)))));
        st[K_eaxfsw()] = v;
        return flags;
    }
    if (m == "fxch") {
        const int64_t k = i.nops ? stk(ops[0]) : 1;
        Sym tmp = Name("x%s", Hex(at).c_str());
        out.push_back(New(MkSet(at, tmp, Loc(top()))));
        out.push_back(New(MkSet(at, top(), Loc(top(k)))));
        out.push_back(New(MkSet(at, top(k), Loc(tmp))));
        return flags;
    }
    if (m == "fchs") {
        out.push_back(New(MkSet(at, top(), T(s::un, s::neg_, Loc(top())))));
        return flags;
    }
    if (m == "fnclex" || m == "fwait") return flags;
    Fail("%s at %X", m.c_str(), at);
}

}  // namespace pse::m2l::rd
