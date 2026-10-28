// mission2lua_readable (C++ port): the translator (whole-program analyses + lifting) and the per-function dataflow
// (mission2lua_readable.Translator / FnOpt and the helpers around them).
#include <algorithm>
#include <cstring>
#include <functional>
#include <regex>

#include "m2r.h"

namespace pse::m2l::rd {

namespace {

[[noreturn]] void Fail(const char* fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    throw Unsupported{buf};
}

Sym RN(const char* n) { return Y(n); }

const std::map<Sym, int>& KindParams() {   // KIND_PARAMS
    static const std::map<Sym, int> m = {{kd::update, 0}, {kd::message, 2}, {kd::save, 1}, {kd::load, 1},
                                         {kd::size, 0},   {kd::init, 0},    {kd::setup, 1}, {kd::queue, 1}};
    return m;
}

const std::set<std::string>& Writes() {   // _clobbers' WRITES (+ the setcc mnemonics)
    static const std::set<std::string> w = [] {
        std::set<std::string> o = {"mov", "movzx", "movsx", "lea", "add", "sub", "and", "or", "xor", "adc",
                                   "sbb", "inc", "dec", "neg", "not", "shl", "sal", "shr", "sar", "imul",
                                   "pop", "xchg", "cdq", "idiv", "div", "mul", "lodsb", "fnstsw"};
        for (const char* c : {"o", "no", "b", "ae", "e", "ne", "be", "a", "s", "ns", "p", "np", "l", "ge", "le", "g"})
            o.insert(std::string("set") + c);
        return o;
    }();
    return w;
}

Sym SubName(uint16_t reg) {   // _SUB.get(reg, (None,))[0]
    if (IsReg32(reg) || IsReg16(reg) || IsLow8(reg) || IsHigh8(reg)) return Y(RegName(Full32(reg)));
    return nullptr;
}

E Loc(Sym n) { return T(s::loc, n); }

}  // namespace

int FormatDwords(const std::string& fmt) {
    static const std::regex re(R"(%[-+ #0]*(\d+|\*)?(\.\d+)?(hh|h|ll|l|L|I64)?([diouxXcspfeEgGn%]))");
    int n = 0;
    for (auto it = std::sregex_iterator(fmt.begin(), fmt.end(), re); it != std::sregex_iterator(); ++it) {
        const std::string conv = (*it)[4].str();
        if (conv == "%") continue;
        n += std::string("feEgG").find(conv[0]) != std::string::npos ? 2 : 1;
    }
    return n;
}

// ---- Translator ------------------------------------------------------------------------------------------------------
std::set<Sym> Translator::ClobbersOr(bool known, uint32_t va, std::initializer_list<const char*> dflt) const {
    if (known) {
        auto it = clobbers.find(va);
        if (it != clobbers.end()) return it->second;
    }
    std::set<Sym> o;
    for (const char* d : dflt) o.insert(Y(d));
    return o;
}

std::map<Sym, E> Translator::EntryValues(const Func& f) const {
    std::map<Sym, E> ev;
    for (int k = 0; k < std::max(f.nparams, 0) + 8; ++k) ev[SL(4 + 4 * k)] = T(s::param, static_cast<int64_t>(k));
    return ev;
}

E Translator::GlobalRead(uint32_t addr, int64_t size, Sym kind) {
    Program& p = prog;
    if (p.InRdata(addr)) {
        const std::string raw = p.BytesAt(addr, static_cast<size_t>(size));
        if (kind == s::f && size == 4) {
            float v = 0;
            if (raw.size() == 4) std::memcpy(&v, raw.data(), 4);
            return T(s::fc, static_cast<double>(v));
        }
        if (kind == s::d && size == 8) {
            double v = 0;
            if (raw.size() == 8) std::memcpy(&v, raw.data(), 8);
            return T(s::fc, v);
        }
        uint64_t v = 0;
        for (size_t k = 0; k < raw.size() && k < 8; ++k) v |= static_cast<uint64_t>(static_cast<uint8_t>(raw[k])) << (8 * k);
        return size == 4 ? C(static_cast<int64_t>(v)) : T(s::c, static_cast<int64_t>(v));
    }
    if (!p.InData(addr)) Fail("global read outside the data at %X", addr);
    gacc[addr].insert({size, kind});
    return T(s::g, static_cast<int64_t>(addr), size, kind);
}

bool Translator::Load(const std::vector<uint8_t>& file, const std::string& name, std::string* error) {
    if (!prog.Load(file, name, error)) return false;
    Program& p = prog;
    // slots: the built-in two, the json, later duplicates of a name get their slot appended
    slots.clear();
    {
        SlotInfo cv;
        cv.has = true;
        cv.name = "check_version";
        cv.hasArgs = true;
        cv.args = {{"major", "int"}, {"minor", "int"}, {"table_size", "int"}};
        slots[0x000] = cv;
        SlotInfo ra;
        ra.has = true;
        ra.name = "register_anim";
        ra.hasArgs = true;
        ra.args = {{"state", "string"}, {"file", "string"}};
        slots[0x004] = ra;
        for (auto& [k, v] : SlotsJson()) slots[k] = v;
        std::map<std::string, int64_t> seen;
        for (auto& [slot, info] : slots) {
            if (seen.count(info.name)) {
                char buf[16];
                std::snprintf(buf, sizeof buf, "_%03llX", static_cast<long long>(slot));
                info.name += buf;
            } else if (!info.name.empty()) {
                seen[info.name] = slot;
            }
        }
    }
    for (uint32_t va : p.funcOrder) F(va)->kind = kd::plain;
    {
        const Sym names[] = {kd::init, kd::update, kd::message, kd::save, kd::load, kd::size};
        for (size_t k = 0; k < 6 && k < p.cbs.size(); ++k) {
            auto it = p.funcs.find(p.cbs[k]);
            if (it != p.funcs.end() && it->second->kind == kd::plain) it->second->kind = names[k];
        }
    }
    F(p.setup)->kind = kd::setup;
    for (uint32_t va : p.queueCbs) {
        auto it = p.funcs.find(va);
        if (it != p.funcs.end() && it->second->kind == kd::plain) it->second->kind = kd::queue;
    }
    for (uint32_t va : p.funcOrder) lifters[va] = std::make_unique<Lifter>(this, F(va));
    for (uint32_t va : p.funcOrder) lifters[va]->Frame();
    ParamCounts();
    Clobbers();
    SiteArgs();
    for (uint32_t va : p.funcOrder) {
        Lifter& lf = *lifters[va];
        ir[va] = lf.LiftAll();
        if (!lf.escaped.empty()) {
            const std::vector<int64_t> offs(lf.escaped.begin(), lf.escaped.end());
            StmtArena tmpArena;
            IR tmp = DeepCopy(ir[va], &arena);
            FnOpt(F(va), &tmp, true, EntryValues(*F(va))).Run();
            const std::set<int64_t> vec = VectorOffsets(tmp);
            std::vector<std::pair<int64_t, int64_t>> regs;
            for (size_t k = 0; k < offs.size(); ++k) {
                const int64_t o = offs[k];
                int64_t hi = k + 1 < offs.size() ? offs[k + 1] : (o < 0 ? 0 : o + 4);
                if (vec.count(o)) hi = std::min(hi, o + 12);
                if (hi > o) regs.emplace_back(o, hi);
            }
            lf.regions = regs;
            lf.escaped.clear();
            ir[va] = lf.LiftAll();
            F(va)->buffers = regs;
        }
    }
    for (uint32_t va : p.funcOrder) {
        Lifter& lf = *lifters[va];
        Func* f = F(va);
        if (f->kind != kd::plain) continue;
        IR tmp = DeepCopy(ir[va], &arena);
        FnOpt(f, &tmp, true, EntryValues(*f)).Run();
        std::set<int64_t> ks;
        for (uint32_t b : tmp.order)
            for (Stmt* st : tmp.m[b]) {
                if (!st->e || st->kind == kd::copy) continue;
                for (E c : FindKind(StripT(st->e), s::call))
                    if (CallTarget(c)->t == s::ptr)
                        for (E pp : FindKind(CallTarget(c)->X(1), s::param)) ks.insert(pp->I(1));
            }
        if (!ks.empty()) {
            lf.apiSlotsIn.clear();
            for (int64_t k : ks) lf.apiSlotsIn.insert(4 + 4 * k);
            ir[va] = lf.LiftAll();
            apiParams[va] = ks;
        }
    }
    return true;
}

std::set<int64_t> Translator::VectorOffsets(const IR& irIn) {
    std::set<int64_t> vecOnly, other;
    for (uint32_t b : irIn.order)
        for (const Stmt* st : irIn.m.at(b)) {
            if (!st->e || st->kind == kd::copy) continue;
            for (E c : FindKind(st->e, s::call)) {
                std::vector<std::string> types;
                if (CallTarget(c)->t == s::slot)
                    if (const SlotInfo* si = Slot(CallTarget(c)->I(1)))
                        for (auto& a : si->args) types.push_back(a.type);
                const std::vector<E> args = ArgsOf(c);
                for (size_t k = 0; k < args.size(); ++k) {
                    E a = StripT(args[k]);
                    if (a->t != s::saddr) continue;
                    const std::string t = k < types.size() ? types[k] : std::string();
                    (t == "vector_ptr" || t == "out_ptr" ? vecOnly : other).insert(a->I(1));
                }
            }
        }
    std::set<int64_t> out;
    for (int64_t v : vecOnly)
        if (!other.count(v)) out.insert(v);
    return out;
}

void Translator::ParamCounts() {
    Program& p = prog;
    std::map<uint32_t, int> n;
    for (uint32_t va : p.funcOrder) {
        Func& f = *F(va);
        auto kp = KindParams().find(f.kind);
        int mx = kp == KindParams().end() ? 0 : kp->second;
        for (auto& [bva, b] : f.blocks) {
            for (const Insn* i : b.insns)
                for (int k = 0; k < i->nops; ++k) {
                    const Operand& op = i->ops[k];
                    if (op.type == OpType::Mem && op.index == R_NONE) {
                        Sym base = op.base ? R32(op.base) : nullptr;
                        bool have = false;
                        int64_t off = 0;
                        if (base == RN("esp")) {
                            off = f.espAt.at(i->address) + S32(op.disp);
                            have = true;
                        } else if (base == RN("ebp")) {
                            auto it = f.ebpAt.find(i->address);
                            if (it != f.ebpAt.end() && it->second.first) {
                                off = it->second.second + S32(op.disp);
                                have = true;
                            }
                        }
                        if (have && off >= 4) mx = std::max<int>(mx, static_cast<int>((off - 4) / 4 + 1));
                    }
                }
            const Insn* last = b.insns.empty() ? nullptr : b.insns.back();
            if (last && last->mnemonic == "jmp" && last->nops && last->ops[0].type == OpType::Mem &&
                last->ops[0].index == R_NONE && last->ops[0].base && 0 <= last->ops[0].disp && last->ops[0].disp < 0x8D8 &&
                last->ops[0].disp % 4 == 0) {
                const SlotInfo* si = Slot(last->ops[0].disp);
                mx = std::max<int>(mx, si ? static_cast<int>(si->args.size()) : 0);
            }
        }
        n[va] = mx;
    }
    for (int round = 0; round < 8; ++round) {
        bool ch = false;
        for (uint32_t va : p.funcOrder)
            for (auto& [bva, b] : F(va)->blocks)
                if (b.kind == kd::tail && n.count(b.tail) && n[b.tail] > n[va]) {
                    n[va] = n[b.tail];
                    ch = true;
                }
        if (!ch) break;
    }
    for (uint32_t va : p.funcOrder) F(va)->nparams = n[va];
}

void Translator::Clobbers() {
    Program& p = prog;
    std::map<uint32_t, std::set<Sym>> own;
    std::map<uint32_t, std::set<uint32_t>> calls;
    const Sym eax = RN("eax"), ecx = RN("ecx"), edx = RN("edx"), esi = RN("esi"), edi = RN("edi");
    for (uint32_t va : p.funcOrder) {
        Func& f = *F(va);
        std::set<Sym> w;
        std::set<uint32_t> c;
        for (auto& [bva, b] : f.blocks) {
            if (b.kind == kd::tail && p.funcs.count(b.tail)) c.insert(b.tail);
            else if (b.kind == kd::tail || b.kind == kd::tailmem) w.insert({eax, ecx, edx});
            for (const Insn* i : b.insns) {
                const std::string& m = i->mnemonic;
                if (m == "call") {
                    const Operand& op = i->ops[0];
                    const bool imm = op.type == OpType::Imm;
                    const uint32_t t = static_cast<uint32_t>(op.imm & M32);
                    if (imm && p.funcs.count(t)) c.insert(t);
                    else w.insert({eax, ecx, edx});
                    continue;
                }
                if (m == "cdq") w.insert(edx);
                if (m == "idiv" || m == "div" || m == "mul" || (m == "imul" && i->nops == 1)) w.insert({eax, edx});
                if (m.compare(0, 3, "rep") == 0 || m == "movsd" || m == "movsb" || m == "stosd" || m == "stosb")
                    w.insert({ecx, esi, edi});
                if (m == "fnstsw") w.insert(eax);
                if (Writes().count(m) && i->nops && i->ops[0].type == OpType::Reg)
                    if (Sym r = SubName(i->ops[0].reg)) w.insert(r);
                if (m == "xchg" && i->nops > 1 && i->ops[1].type == OpType::Reg)
                    if (Sym r = SubName(i->ops[1].reg)) w.insert(r);
            }
        }
        own[va] = w;
        calls[va] = c;
    }
    std::map<uint32_t, std::set<Sym>> total = own;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t va : p.funcOrder)
            for (uint32_t c : calls[va]) {
                const std::set<Sym>& tc = total[c];
                std::set<Sym>& tv = total[va];
                for (Sym r : tc)
                    if (!tv.count(r)) {
                        tv.insert(r);
                        changed = true;
                    }
            }
    }
    clobbers = total;
}

void Translator::SiteArgs() {
    Program& p = prog;
    for (uint32_t va : p.funcOrder) {
        Func& f = *F(va);
        saves_ = PrologueSaves(f);
        for (auto& [bva, b] : f.blocks)
            for (size_t k = 0; k < b.insns.size(); ++k) {
                const Insn& i = *b.insns[k];
                if (i.mnemonic != "call") continue;
                const std::set<int64_t> w = WrittenBefore(f, b.start, k, {});
                const int64_t esp = f.espAt.at(i.address);
                int64_t n = 0;
                while (w.count(esp + 4 * n)) ++n;
                siteArgs[i.address] = n;
            }
    }
}

const std::map<uint32_t, std::set<int64_t>>& Translator::BlockWrites(Func& f, const Block& b) {
    auto key = std::make_pair(f.va, b.start);
    auto it = wbBlock_.find(key);
    if (it != wbBlock_.end()) return it->second;
    std::map<uint32_t, std::set<int64_t>> out;
    std::map<Sym, int64_t> regs;
    const Sym esp = RN("esp");
    for (const Insn* i : b.insns) {
        const int64_t e = f.espAt.at(i->address);
        const Operand* ops = i->ops;
        std::set<int64_t> ws;
        const std::string& m = i->mnemonic;
        if (m == "push" && !saves_.count(i->address)) {
            ws.insert(e - 4);
        } else if (i->nops && ops[0].type == OpType::Mem &&
                   (m == "mov" || m == "fstp" || m == "fst" || m == "fistp" || m == "fist") && ops[0].index == R_NONE) {
            Sym base = ops[0].base ? R32(ops[0].base) : nullptr;
            bool have = false;
            int64_t off = 0;
            if (base == esp) {
                off = e + S32(ops[0].disp);
                have = true;
            } else if (base && regs.count(base)) {
                off = regs[base] + S32(ops[0].disp);
                have = true;
            }
            if (have)
                for (int q = 0; q < std::max(4, static_cast<int>(ops[0].size)); q += 4) ws.insert(off + q);
        }
        if (!ws.empty()) out[i->address] = ws;
        if (i->nops && ops[0].type == OpType::Reg) {
            Sym r = R32(ops[0].reg);
            if (m == "mov" && ops[1].type == OpType::Reg && R32(ops[1].reg) == esp) {
                regs[r] = e;
            } else if (m == "lea" && ops[1].base && R32(ops[1].base) == esp && !ops[1].index) {
                regs[r] = e + S32(ops[1].disp);
            } else if (regs.count(r) && m != "push" && m != "cmp" && m != "test") {
                regs.erase(r);
            }
        }
        if (m == "call")
            for (const char* r : {"eax", "ecx", "edx"}) regs.erase(RN(r));
    }
    return wbBlock_[key] = out;
}

std::set<uint32_t> Translator::PrologueSaves(Func& f) {
    std::set<uint32_t> saves;
    std::set<Sym> written;
    const Block& b = f.blocks.at(f.va);
    for (const Insn* i : b.insns) {
        if (i->mnemonic == "call") break;
        if (i->mnemonic == "push" && i->ops[0].type == OpType::Reg) {
            Sym r = R32(i->ops[0].reg);
            if ((r == RN("ebx") || r == RN("esi") || r == RN("edi") || r == RN("ebp")) && !written.count(r)) {
                saves.insert(i->address);
                continue;
            }
        }
        if (i->nops && i->ops[0].type == OpType::Reg && i->mnemonic != "push" && i->mnemonic != "cmp" &&
            i->mnemonic != "test")
            written.insert(R32(i->ops[0].reg));
    }
    return saves;
}

std::set<int64_t> Translator::WrittenBefore(Func& f, uint32_t bva, size_t k, std::set<uint32_t> seen) {
    const Block& b = f.blocks.at(bva);
    std::set<int64_t> w;
    const auto& writes = BlockWrites(f, b);
    for (size_t q = k; q-- > 0;) {
        const Insn* i = b.insns[q];
        if (i->mnemonic == "call") return w;
        auto it = writes.find(i->address);
        if (it != writes.end()) w.insert(it->second.begin(), it->second.end());
    }
    if (bva == f.va || seen.count(bva) || b.preds.empty()) return w;
    auto key = std::make_pair(f.va, bva);
    if (!wbMemo_.count(key)) {
        wbMemo_[key] = {};
        seen.insert(bva);
        bool first = true;
        std::set<int64_t> common;
        for (uint32_t pv : b.preds) {
            auto ek = std::make_pair(f.va, pv);
            auto it = wbEnd_.find(ek);
            std::set<int64_t> memo;
            if (it == wbEnd_.end()) {
                memo = WrittenBefore(f, pv, f.blocks.at(pv).insns.size(), seen);
                wbEnd_[ek] = memo;
            } else {
                memo = it->second;
            }
            if (first) {
                common = memo;
                first = false;
            } else {
                std::set<int64_t> x;
                for (int64_t v : common)
                    if (memo.count(v)) x.insert(v);
                common = x;
            }
        }
        wbMemo_[key] = common;
    }
    const std::set<int64_t>& mm = wbMemo_[key];
    w.insert(mm.begin(), mm.end());
    return w;
}

int64_t Translator::CallArgs(E target, int64_t esp, uint32_t at, Func* f) {
    if (target->t == s::slot) {
        const SlotInfo* si = Slot(target->I(1));
        auto it = siteArgs.find(at);
        const bool haveEv = it != siteArgs.end();
        const int64_t ev = haveEv ? it->second : 0;
        const bool haveJs = si && si->hasArgs;
        const int64_t js = haveJs ? static_cast<int64_t>(si->args.size()) : 0;
        if (haveEv && haveJs && ev != js) stats["arg_count_differs_from_json"] += 1;
        if (haveJs && !si->args.empty() && (si->args.back().name == "args" || si->args.back().name == "format_args") &&
            haveEv)
            return std::max(js - 1, ev);
        if (haveJs) return js;
        return haveEv ? ev : 0;
    }
    if (target->t == s::fn) return F(static_cast<uint32_t>(target->I(1)))->nparams;
    if (target->t == s::rand) return 0;
    if (target->t == s::sprintf) return 2 + SprintfFormat(*f, at, esp);
    auto it = siteArgs.find(at);
    return it != siteArgs.end() ? it->second : 0;
}

int64_t Translator::SprintfFormat(Func& f, uint32_t at, int64_t esp) {
    for (auto& [bva, blk] : f.blocks)
        for (size_t k = 0; k < blk.insns.size(); ++k)
            if (blk.insns[k]->address == at) {
                const std::set<int64_t> vals = SlotImm(f, bva, k, esp + 4, {});
                std::vector<std::string> fmts;
                bool ok = true;
                for (int64_t v : vals) {
                    if (v < 0) continue;   // None
                    std::string sv;
                    if (!prog.Cstr(static_cast<uint32_t>(v), &sv)) ok = false;
                    else fmts.push_back(sv);
                }
                if (!fmts.empty() && ok) {
                    int best = 0;
                    for (const std::string& x : fmts) best = std::max(best, FormatDwords(x));
                    return best;
                }
            }
    Fail("sprintf format at %X", at);
}

std::set<int64_t> Translator::SlotImm(Func& f, uint32_t bva, size_t k, int64_t off, std::set<uint32_t> seen) {
    const Block& blk = f.blocks.at(bva);
    for (size_t q = k; q-- > 0;) {
        const Insn* j = blk.insns[q];
        if (j->mnemonic == "push" && f.espAt.at(j->address) - 4 == off) {
            const Operand& op = j->ops[0];
            return {op.type == OpType::Imm ? (op.imm & M32) : -1};
        }
    }
    if (seen.count(bva) || bva == f.va) return {-1};
    std::set<int64_t> out;
    seen.insert(bva);
    for (uint32_t pv : blk.preds) {
        std::set<int64_t> r = SlotImm(f, pv, f.blocks.at(pv).insns.size(), off, seen);
        out.insert(r.begin(), r.end());
    }
    return out;
}

// ---- FnOpt -----------------------------------------------------------------------------------------------------------
std::string DefId::PyStr() const {
    char buf[96];
    if (in) std::snprintf(buf, sizeof buf, "('in', '%s')", loc->c_str());
    else std::snprintf(buf, sizeof buf, "(%u, %d)", va, k);
    return buf;
}

FnOpt::FnOpt(Func* fn, IR* irp, bool returns, std::map<Sym, E> ev) : f(fn), ir(irp), entryValues(std::move(ev)) {
    (void)returns;
    order = Rpo(f->blocks, f->va);
}

std::map<uint32_t, RDState> FnOpt::Rd() {
    std::map<uint32_t, std::map<Sym, DefId>> gen;
    for (uint32_t va : order) {
        std::map<Sym, DefId>& g = gen[va];
        const StmtList& sl = (*ir)[va];
        for (size_t k = 0; k < sl.size(); ++k)
            if (sl[k]->kind == kd::set) g[sl[k]->loc] = AtDef(va, static_cast<int>(k));
    }
    RDState entry;
    for (uint32_t va : order)
        for (const Stmt* st : (*ir)[va]) {
            for (Sym l : StmtUses(*st)) entry[l] = DefSet{InDef(l)};
            if (st->kind == kd::set && !entry.count(st->loc)) entry[st->loc] = DefSet{InDef(st->loc)};
        }
    std::map<uint32_t, RDState> ins;
    std::map<uint32_t, bool> haveIn;
    ins[f->va] = entry;
    haveIn[f->va] = true;
    std::map<uint32_t, RDState> outs;
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t va : order) {
            RDState st;
            if (va == f->va) {
                st = entry;
            } else {
                for (uint32_t p : f->blocks.at(va).preds) {
                    auto it = outs.find(p);
                    if (it == outs.end()) continue;
                    for (auto& [l, ds] : it->second) st[l].insert(ds.begin(), ds.end());
                }
            }
            if (!haveIn[va] || ins[va] != st) {
                ins[va] = st;
                haveIn[va] = true;
                changed = true;
            }
            RDState o = st;
            for (auto& [l, d] : gen[va]) o[l] = DefSet{d};
            outs[va] = std::move(o);
        }
    }
    return ins;
}

void FnOpt::Run() {
    for (int n = 0; n < 100000; ++n) {
        bool ch = PassPure();
        ch |= PassInline();
        ch |= PassDead();
        if (!ch) ch = PassInlineX();
        if (!ch) ch = PassNarrow();
        if (!ch) break;
    }
}

namespace {
bool OnlyBits(const std::vector<E>& exprs, Sym loc, int64_t keep) {   // _only_bits
    E L = Loc(loc);
    std::function<bool(E)> ok = [&](E e) -> bool {
        if (!e) return true;
        if (e->t == s::loc) return e->S(1) != loc;
        if (e->t == s::ext && e->X(1) == L) return ((e->I(3) << e->I(2)) & ~keep) == 0;
        if (e->t == s::bin && e->S(1) == s::band && e->X(2) == L && IsC(e->X(3)))
            return (e->X(3)->I(1) & M32 & ~keep) == 0;
        for (E x : Kids(e))
            if (!ok(x)) return false;
        return true;
    };
    for (E e : exprs)
        if (!ok(e)) return false;
    return true;
}
bool Cheap(E e) {   // _cheap
    e = StripT(e);
    Sym k = e->t;
    if (k == s::g || k == s::c || k == s::fc || k == s::param || k == s::gaddr || k == s::saddr || k == s::api) return true;
    if (k == s::bin && (e->S(1) == s::plus || e->S(1) == s::minus) && IsC(e->X(3)) &&
        (e->X(2)->t == s::g || e->X(2)->t == s::param))
        return true;
    return false;
}
DefSet DefsAt(const RDState& st, Sym l) {
    auto it = st.find(l);
    return it != st.end() ? it->second : DefSet{InDef(l)};
}
}  // namespace

bool FnOpt::PassNarrow() {
    auto ins = Rd();
    auto uses = UsesOf(ins);
    for (uint32_t va : order) {
        StmtList& sl = (*ir)[va];
        for (size_t i = 0; i < sl.size(); ++i) {
            Stmt* st = sl[i];
            if (st->kind != kd::set || !st->e || st->e->t != s::ins || st->e->X(1) != Loc(st->loc)) continue;
            const int64_t keep = st->e->I(4) << st->e->I(3);
            auto it = uses.find(AtDef(va, static_cast<int>(i)));
            if (it == uses.end() || it->second.empty()) continue;
            bool all = true;
            for (const Use& u : it->second)
                all &= OnlyBits(StmtExprs(*(*ir)[u.va][u.k]), u.loc, keep);
            if (all) {
                st->e = Simp(T(s::ins, C0, st->e->X(2), st->e->I(3), st->e->I(4)));
                return true;
            }
        }
    }
    return false;
}

E FnOpt::DefVal(const DefId& d) {
    if (d.in) {
        auto it = entryValues.find(d.loc);
        return it != entryValues.end() ? it->second : T(s::undef, d.loc);
    }
    return (*ir)[d.va][d.k]->e;
}

bool FnOpt::PassPure() {
    auto ins = Rd();
    bool changed = false;
    for (uint32_t va : order) {
        RDState st = ins.count(va) ? ins[va] : RDState{};
        StmtList& sl = (*ir)[va];
        for (size_t k = 0; k < sl.size(); ++k) {
            Stmt* s0 = sl[k];
            std::map<Sym, E> m;
            for (Sym l : StmtUses(*s0)) {
                const DefSet ds = DefsAt(st, l);
                std::set<E> vals;
                for (const DefId& d : ds) vals.insert(DefVal(d));
                if (vals.size() == 1) {
                    E v = *vals.begin();
                    if (Stable(v)) m[l] = v;
                }
            }
            if (!m.empty()) {
                SubstStmt(s0, m);
                changed = true;
            }
            if (s0->kind == kd::set) st[s0->loc] = DefSet{AtDef(va, static_cast<int>(k))};
        }
    }
    return changed;
}

std::map<DefId, std::vector<FnOpt::Use>> FnOpt::UsesOf(std::map<uint32_t, RDState>& ins) {
    std::map<DefId, std::vector<Use>> out;
    for (uint32_t va : order) {
        RDState st = ins.count(va) ? ins[va] : RDState{};
        const StmtList& sl = (*ir)[va];
        for (size_t k = 0; k < sl.size(); ++k) {
            for (Sym l : StmtUses(*sl[k]))
                for (const DefId& d : DefsAt(st, l)) out[d].push_back({va, static_cast<int>(k), l});
            if (sl[k]->kind == kd::set) st[sl[k]->loc] = DefSet{AtDef(va, static_cast<int>(k))};
        }
    }
    return out;
}

bool FnOpt::PassInline() {
    auto ins = Rd();
    auto uses = UsesOf(ins);
    bool changed = false;
    for (uint32_t va : order) {
        StmtList& stmts = (*ir)[va];
        RDState st = ins.count(va) ? ins[va] : RDState{};
        std::vector<RDState> rdAt;
        for (size_t k = 0; k < stmts.size(); ++k) {
            rdAt.push_back(st);
            if (stmts[k]->kind == kd::set) st[stmts[k]->loc] = DefSet{AtDef(va, static_cast<int>(k))};
        }
        for (size_t i = 0; i < stmts.size(); ++i) {
            Stmt* s0 = stmts[i];
            if (s0->kind != kd::set) continue;
            auto it = uses.find(AtDef(va, static_cast<int>(i)));
            if (it == uses.end() || it->second.size() != 1) continue;
            const Use u = it->second[0];
            if (u.va != va || u.k <= static_cast<int>(i)) continue;
            auto r = rdAt[u.k].find(u.loc);
            if (r == rdAt[u.k].end() || r->second != DefSet{AtDef(va, static_cast<int>(i))}) continue;
            if (!CanInline(stmts, i, u.k)) continue;
            if ((HasCall(s0->e) || ReadsMemory(s0->e)) && Occurrences(*stmts[u.k], u.loc) > 1) continue;
            E e = TagReads(s0->e, s0->at);
            Stmt* t = stmts[u.k];
            const E oldE = t->e, oldT = t->target;
            E oldC[3] = {t->ce[0], t->ce[1], t->ce[2]};
            SubstStmt(t, {{u.loc, e}});
            std::vector<Ev> ev;
            Events(StmtExpr(*t), t->at, &ev);
            if (!OrderOk(ev)) {
                t->e = oldE;
                t->target = oldT;
                for (int q = 0; q < 3; ++q) t->ce[q] = oldC[q];
                continue;
            }
            stmts.erase(stmts.begin() + i);
            changed = true;
            break;
        }
    }
    return changed;
}

bool FnOpt::CanInline(const StmtList& stmts, size_t i, size_t j) {
    const Stmt* s0 = stmts[i];
    E e = s0->e;
    const std::set<Sym> rhsLocs = LocsIn(e);
    const bool rhsMem = ReadsMemory(e), rhsCall = HasCall(e);
    for (size_t k = i + 1; k < j; ++k) {
        const Stmt* t = stmts[k];
        if (t->kind == kd::set && rhsLocs.count(t->loc)) return false;
        Sym eff = StmtEffect(*t);
        if (rhsCall && eff) return false;
        if (rhsMem && (eff == kd::call || eff == kd::store)) return false;
    }
    const Stmt* t = stmts[j];
    if (t->kind == kd::store && rhsCall && t->target->t == s::mem && HasCall(t->target->X(1))) return false;
    return true;
}

std::set<uint32_t> FnOpt::Between(uint32_t dva, uint32_t uva) {
    std::set<uint32_t> fwd, back;
    std::vector<uint32_t> todo;
    for (uint32_t sc : f->blocks.at(dva).succ)
        if (sc != dva) todo.push_back(sc);
    while (!todo.empty()) {
        const uint32_t v = todo.back();
        todo.pop_back();
        if (fwd.count(v) || v == dva) continue;
        fwd.insert(v);
        for (uint32_t sc : f->blocks.at(v).succ) todo.push_back(sc);
    }
    todo.assign(f->blocks.at(uva).preds.begin(), f->blocks.at(uva).preds.end());
    while (!todo.empty()) {
        const uint32_t v = todo.back();
        todo.pop_back();
        if (back.count(v) || v == dva) continue;
        back.insert(v);
        for (uint32_t p : f->blocks.at(v).preds) todo.push_back(p);
    }
    std::set<uint32_t> out;
    for (uint32_t v : fwd)
        if (back.count(v) && v != uva) out.insert(v);
    return out;
}

bool FnOpt::PassInlineX() {
    auto ins = Rd();
    auto uses = UsesOf(ins);
    for (uint32_t va : order) {
        StmtList& sl = (*ir)[va];
        for (size_t i = 0; i < sl.size(); ++i) {
            Stmt* s0 = sl[i];
            if (s0->kind != kd::set || HasCall(s0->e)) continue;
            auto it = uses.find(AtDef(va, static_cast<int>(i)));
            if (it == uses.end() || it->second.empty()) continue;
            const std::vector<Use>& us = it->second;
            if (us.size() > 1 && !Cheap(s0->e)) continue;
            if (us.size() == 1 && us[0].va == va) continue;
            bool ok = true;
            for (const Use& u : us) {
                RDState st = ins.count(u.va) ? ins[u.va] : RDState{};
                const StmtList& ul = (*ir)[u.va];
                for (int k = 0; k < u.k; ++k)
                    if (ul[k]->kind == kd::set) st[ul[k]->loc] = DefSet{AtDef(u.va, k)};
                auto r = st.find(u.loc);
                if (r == st.end() || r->second != DefSet{AtDef(va, static_cast<int>(i))}) {
                    ok = false;
                    break;
                }
                std::vector<Stmt*> seq;
                if (u.va == va) {
                    if (u.k <= static_cast<int>(i)) {
                        ok = false;
                        break;
                    }
                    seq.assign(sl.begin() + i + 1, sl.begin() + u.k);
                } else {
                    const std::set<uint32_t> btw = Between(va, u.va);
                    if (btw.count(u.va) || btw.count(va)) {
                        ok = false;
                        break;
                    }
                    seq.assign(sl.begin() + i + 1, sl.end());
                    for (uint32_t b : btw) {
                        const StmtList& bl = (*ir)[b];
                        seq.insert(seq.end(), bl.begin(), bl.end());
                    }
                    seq.insert(seq.end(), ul.begin(), ul.begin() + u.k);
                }
                const std::set<Sym> rl = LocsIn(s0->e);
                const bool rm = ReadsMemory(s0->e);
                for (const Stmt* t : seq) {
                    if (t->kind == kd::set && rl.count(t->loc)) {
                        ok = false;
                        break;
                    }
                    if (rm) {
                        Sym eff = StmtEffect(*t);
                        if (eff == kd::call || eff == kd::store) {
                            ok = false;
                            break;
                        }
                    }
                }
                if (!ok) break;
            }
            if (!ok) continue;
            for (const Use& u : us) SubstStmt((*ir)[u.va][u.k], {{u.loc, TagReads(s0->e, s0->at)}});
            sl.erase(sl.begin() + i);
            return true;
        }
    }
    return false;
}

bool FnOpt::PassDead() {
    auto ins = Rd();
    auto uses = UsesOf(ins);
    bool changed = false;
    for (uint32_t va : order) {
        StmtList& stmts = (*ir)[va];
        StmtList keep;
        for (size_t k = 0; k < stmts.size(); ++k) {
            Stmt* st = stmts[k];
            if (st->kind == kd::set && st->e == Loc(st->loc)) {
                changed = true;
                continue;
            }
            if (st->kind == kd::set) {
                auto it = uses.find(AtDef(va, static_cast<int>(k)));
                if (it == uses.end() || it->second.empty()) {
                    if (HasCall(st->e)) {
                        st->kind = kd::expr;
                        st->loc = nullptr;
                        changed = true;
                    } else {
                        changed = true;
                        continue;
                    }
                }
            }
            keep.push_back(st);
        }
        stmts = keep;
    }
    return changed;
}

}  // namespace pse::m2l::rd
