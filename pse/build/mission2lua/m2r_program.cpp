// mission2lua_readable (C++ port): the mission code of one DLL - mission2lua_readable.Program / _table_targets and
// missionflow._script_object.
#include <algorithm>
#include <cstring>

#include "m2r.h"

namespace pse::m2l::rd {

namespace {

const std::set<std::string>& Jcc() {   // mission2lua_readable._JCC
    static const std::set<std::string> k = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja",
                                            "js", "jns", "jp", "jnp", "jl", "jge", "jle", "jg"};
    return k;
}
bool IsJcc(const std::string& m) { return Jcc().count(m) != 0; }

const uint8_t kSprintfSig[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20, 0x8b, 0x45, 0x08, 0x56};
const uint8_t kSprintfSig2[] = {0x55, 0x8b, 0xec, 0x83, 0xec, 0x20, 0x56, 0x8b, 0x75, 0x08, 0x57, 0x8d, 0x45, 0x10};
const uint8_t kRandSigAt1[] = {0x8b, 0x48, 0x14, 0x69, 0xc9, 0xfd, 0x43, 0x03, 0x00};
const uint8_t kCookie0[] = {0x3b, 0x0d}, kCookie1[] = {0x75, 0x01, 0xc3};

}  // namespace

bool Program::InRdata(uint32_t a) const {
    for (auto& r : rdata)
        if (r.first <= a && a < r.second) return true;
    return false;
}
bool Program::InData(uint32_t a) const {
    for (auto& r : data)
        if (r.first <= a && a < r.second) return true;
    return false;
}
std::string Program::BytesAt(uint32_t a, size_t n) const {
    const auto& img = p.dll.img;
    const int64_t o = static_cast<int64_t>(a) - base;
    if (o < 0) return {};   // (the Python slices from the end: never asked for)
    if (static_cast<size_t>(o) >= img.size()) return {};
    const size_t end = std::min(img.size(), static_cast<size_t>(o) + n);
    return std::string(img.begin() + static_cast<ptrdiff_t>(o), img.begin() + static_cast<ptrdiff_t>(end));
}
bool Program::Cstr(uint32_t va, std::string* out) const {
    const auto& img = p.dll.img;
    const int64_t o = static_cast<int64_t>(va) - base;
    if (o < 0 || static_cast<size_t>(o) >= img.size()) return false;
    size_t end = static_cast<size_t>(o);
    while (end < img.size() && img[end]) ++end;
    if (end >= img.size()) end = img.size() - 1;   // img.find(b"\0") = -1: img[o:-1]
    if (end < static_cast<size_t>(o)) return false;
    const size_t len = end - static_cast<size_t>(o);
    if (len == 0 || len >= 200) return false;
    std::string sv(img.begin() + static_cast<ptrdiff_t>(o), img.begin() + static_cast<ptrdiff_t>(end));
    for (unsigned char ch : sv)
        if (!((ch >= 32 && ch < 127) || ch == 9 || ch == 10 || ch == 13)) return false;
    *out = sv;
    return true;
}

Func* Program::AddFunc(uint32_t va, std::unique_ptr<Func> f) {
    auto it = funcs.find(va);
    if (it == funcs.end()) funcOrder.push_back(va);
    Func* raw = f.get();
    funcs[va] = std::move(f);
    return raw;
}

bool Program::Load(const std::vector<uint8_t>& file, const std::string& nm, std::string* error) {
    if (!p.Load(file, nm, error)) return false;
    dll = &p.dll;
    Dll& d = p.dll;
    name = p.name;
    base = d.base;
    // missionflow._script_object
    auto ex = d.exports.find("LevelSetup");
    if (ex == d.exports.end()) {
        if (error) *error = "no LevelSetup export";
        return false;
    }
    setup = ex->second;
    {
        const uint32_t exRva = ex->second - base;
        std::vector<uint8_t> code;
        for (uint32_t k = 0; k < 0x60 && exRva + k < d.img.size(); ++k) code.push_back(d.img[exRva + k]);
        size_t k = 0;
        while (k < code.size() && code[k] != 0xB8) ++k;
        if (k + 5 > code.size()) {
            if (error) *error = "no script object in LevelSetup";
            return false;
        }
        std::memcpy(&obj, code.data() + k + 1, 4);
        for (int n = 0; n < 6; ++n) {
            uint32_t v = 0;
            d.U32(obj + 4 * n, &v);
            cbs.push_back(v);
        }
        size_t at = 0;
        while (at < code.size()) {
            Insn i;
            if (!Decode(code.data() + at, code.size() - at, ex->second + static_cast<uint32_t>(at), &i)) break;
            if (i.mnemonic == "mov" && i.nops && i.ops[0].type == OpType::Mem && !Full32(i.ops[0].base)) {
                hasApiGlobal = true;
                const int64_t disp = i.ops[0].disp;
                apiGlobal = static_cast<uint32_t>(disp >= 0 ? (disp & 0xFFFFFFFF) : disp);
                break;
            }
            if (i.mnemonic == "ret" || i.mnemonic == "retn") break;
            at += i.size;
        }
    }
    for (const Section& sc : d.sections) {
        std::string n = sc.name;
        while (!n.empty() && n.back() == '\0') n.pop_back();
        Sec x{base + sc.va, base + sc.va + std::max(sc.vsize, sc.rawSize), n};
        sections.push_back(x);
        if (n == ".rdata") rdata.emplace_back(x.lo, x.hi);
        if (n == ".data" || n == ".bss") data.emplace_back(x.lo, x.hi);
    }
    Discover();
    return true;
}

std::set<uint32_t> Program::CalleesRaw(const m2l::Function& f) {
    Dll& d = p.dll;
    std::set<uint32_t> out;
    for (auto& [bva, b] : f.blocks)
        for (const Insn* i : b.insns) {
            if ((i->mnemonic == "call" || i->mnemonic == "jmp") && i->nops && i->ops[0].type == OpType::Imm) {
                const uint32_t t = static_cast<uint32_t>(i->ops[0].imm);
                if (i->mnemonic == "call" || p.functions.count(t)) out.insert(t);
            }
            for (int k = 0; k < i->nops; ++k) {
                const Operand& op = i->ops[k];
                if (op.type == OpType::Imm && i->imm_offset && p.dll.relocs.count(i->address + i->imm_offset) &&
                    d.InText(static_cast<uint32_t>(op.imm)))
                    out.insert(static_cast<uint32_t>(op.imm));
            }
        }
    return out;
}

std::set<uint32_t> Program::Callees(const Func& f) {
    Dll& d = p.dll;
    std::set<uint32_t> out;
    for (auto& [bva, b] : f.blocks) {
        if (b.kind == kd::tail && b.hasTail) out.insert(b.tail);
        for (const Insn* i : b.insns) {
            if (i->mnemonic == "call" && i->ops[0].type == OpType::Imm) out.insert(static_cast<uint32_t>(i->ops[0].imm));
            for (int k = 0; k < i->nops; ++k) {
                const Operand& op = i->ops[k];
                if (op.type == OpType::Imm && i->imm_offset && p.dll.relocs.count(i->address + i->imm_offset) &&
                    d.InText(static_cast<uint32_t>(op.imm))) {
                    out.insert(static_cast<uint32_t>(op.imm));
                    queueCbs.insert(static_cast<uint32_t>(op.imm));
                }
            }
        }
    }
    return out;
}

uint32_t Program::Limit(uint32_t va) {
    uint32_t best = 0;
    bool have = false;
    auto f = p.functions.upper_bound(va);
    if (f != p.functions.end()) {
        best = f->first;
        have = true;
    }
    auto e = entries.upper_bound(va);
    if (e != entries.end() && (!have || *e < best)) {
        best = *e;
        have = true;
    }
    return have ? best : p.dll.textHi;
}

std::unique_ptr<Func> Program::Build(uint32_t va) {
    Dll& d = p.dll;
    auto f = std::make_unique<Func>();
    f->va = va;
    const uint32_t lim = Limit(va);
    auto outside = [&](uint32_t t) {
        if (t != va && (p.functions.count(t) || entries.count(t))) return true;
        return !(va <= t && t < lim);
    };
    std::set<uint32_t> starts{va}, seen;
    std::vector<uint32_t> todo{va};
    while (!todo.empty()) {
        uint32_t a = todo.back();
        todo.pop_back();
        while (!seen.count(a)) {
            seen.insert(a);
            const Insn* i = d.At(a);
            if (!i) break;
            const std::string& m = i->mnemonic;
            if (m == "ret" || m == "retn") {
                if (i->nops) f->retPop = i->ops[0].imm;
                break;
            }
            if (m == "jmp") {
                const Operand& op = i->ops[0];
                if (op.type == OpType::Imm) {
                    const uint32_t t = static_cast<uint32_t>(op.imm);
                    if (!outside(t)) {
                        starts.insert(t);
                        todo.push_back(t);
                    }
                } else {
                    TablePtr tab;
                    for (uint32_t t : TableTargets(d, *i, true, va, lim, &tab)) {
                        starts.insert(t);
                        todo.push_back(t);
                    }
                }
                break;
            }
            if (IsJcc(m)) {
                const uint32_t t = static_cast<uint32_t>(i->ops[0].imm), nx = i->address + i->size;
                starts.insert(t);
                starts.insert(nx);
                todo.push_back(t);
                todo.push_back(nx);
                break;
            }
            if (m == "int3" || m == "hlt") break;
            a = i->address + i->size;
        }
    }
    for (uint32_t st : starts) {
        Block b;
        b.start = st;
        b.kind = kd::fall;
        uint32_t a = st;
        for (;;) {
            const Insn* i = d.At(a);
            if (!i) {
                b.kind = kd::end;
                break;
            }
            b.insns.push_back(i);
            const std::string& m = i->mnemonic;
            const uint32_t nxt = i->address + i->size;
            if (m == "ret" || m == "retn") {
                b.kind = kd::ret;
                break;
            }
            if (m == "jmp") {
                const Operand& op = i->ops[0];
                if (op.type == OpType::Imm) {
                    const uint32_t t = static_cast<uint32_t>(op.imm);
                    if (outside(t)) {
                        b.kind = kd::tail;
                        b.hasTail = true;
                        b.tail = t;
                    } else {
                        b.kind = kd::jmp;
                        b.succ = {t};
                    }
                } else if (op.type == OpType::Mem && op.index == R_NONE) {
                    b.kind = kd::tailmem;
                } else {
                    TablePtr tab;
                    std::vector<uint32_t> tg = TableTargets(d, *i, true, va, lim, &tab);
                    if (tg.empty()) {
                        char buf[64];
                        std::snprintf(buf, sizeof buf, "indirect jump at %X", i->address);
                        throw Unsupported{buf};
                    }
                    b.kind = kd::sw;
                    b.succ = tg;
                    b.table = tab;
                }
                break;
            }
            if (IsJcc(m)) {
                b.kind = kd::cond;
                b.succ = {static_cast<uint32_t>(i->ops[0].imm), nxt};
                break;
            }
            if (m == "int3" || m == "hlt") {
                b.kind = kd::end;
                break;
            }
            if (starts.count(nxt)) {
                b.kind = kd::fall;
                b.succ = {nxt};
                break;
            }
            a = nxt;
        }
        f->blocks[st] = std::move(b);
    }
    for (auto& [bva, b] : f->blocks) {
        std::vector<uint32_t> keep;
        for (uint32_t t : b.succ)
            if (f->blocks.count(t)) keep.push_back(t);
        b.succ = keep;
    }
    for (auto& [bva, b] : f->blocks)
        for (uint32_t t : b.succ) f->blocks[t].preds.push_back(b.start);
    return f;
}

void Program::Discover() {
    Dll& d = p.dll;
    std::vector<uint32_t> todo{p.entry};
    while (!todo.empty()) {
        const uint32_t v = todo.back();
        todo.pop_back();
        if (crt.count(v) || !p.functions.count(v)) continue;
        crt.insert(v);
        for (uint32_t c : CalleesRaw(*p.functions.at(v))) todo.push_back(c);
    }
    for (auto& [v, fn] : p.functions) {
        const std::string b10 = BytesAt(v, sizeof kSprintfSig), b14 = BytesAt(v, sizeof kSprintfSig2);
        if ((b10.size() == sizeof kSprintfSig && std::memcmp(b10.data(), kSprintfSig, sizeof kSprintfSig) == 0) ||
            (b14.size() == sizeof kSprintfSig2 && std::memcmp(b14.data(), kSprintfSig2, sizeof kSprintfSig2) == 0))
            sprintf_.insert(v);
        const std::string e8 = BytesAt(v, 1), r = BytesAt(v + 5, sizeof kRandSigAt1);
        if (e8 == "\xE8" && r.size() == sizeof kRandSigAt1 && std::memcmp(r.data(), kRandSigAt1, sizeof kRandSigAt1) == 0)
            rand_.insert(v);
        const std::string c0 = BytesAt(v, 2), c1 = BytesAt(v + 6, 3);
        if (c0.size() == 2 && std::memcmp(c0.data(), kCookie0, 2) == 0 && c1.size() == 3 &&
            std::memcmp(c1.data(), kCookie1, 3) == 0)
            cookie_.insert(v);
    }
    (void)d;
    auto special = [&](uint32_t v) { return sprintf_.count(v) || rand_.count(v) || cookie_.count(v); };
    std::vector<uint32_t> roots{setup};
    for (uint32_t c : cbs) roots.push_back(c);
    entries.insert(roots.begin(), roots.end());
    todo = roots;
    while (!todo.empty()) {
        const uint32_t v = todo.back();
        todo.pop_back();
        if (funcs.count(v) || special(v)) continue;
        if (crt.count(v)) {
            char buf[64];
            std::snprintf(buf, sizeof buf, "mission code calls CRT function %X", v);
            throw Unsupported{buf};
        }
        Func* f = AddFunc(v, Build(v));
        for (uint32_t c : Callees(*f))
            if (!funcs.count(c)) {
                entries.insert(c);
                todo.push_back(c);
            }
    }
    for (int round = 0; round < 4; ++round) {
        bool changed = false;
        const std::vector<uint32_t> now = funcOrder;
        for (uint32_t v : now) {
            std::unique_ptr<Func> nf = Build(v);
            std::set<uint32_t> a, b;
            for (auto& kv : nf->blocks) a.insert(kv.first);
            for (auto& kv : funcs[v]->blocks) b.insert(kv.first);
            const std::set<uint32_t> cs = Callees(*nf);   // (the new build's, whichever is kept)
            if (a != b) {
                AddFunc(v, std::move(nf));
                changed = true;
            }
            for (uint32_t c : cs)
                if (!funcs.count(c) && !special(c)) {
                    entries.insert(c);
                    AddFunc(c, Build(c));
                    changed = true;
                }
        }
        if (!changed) break;
    }
}

std::vector<uint32_t> TableTargets(Dll& d, const Insn& i, bool haveRange, uint32_t lo, uint32_t hi, TablePtr* table) {
    table->reset();
    std::unique_ptr<m2l::Table> t;
    std::vector<uint32_t> tg = m2l::Function::BaseTableTargets(d, i, &t);
    if (!tg.empty() || !haveRange) {
        if (t) {
            auto ti = std::make_shared<TableInfo>();
            for (auto& [k, v] : t->values) ti->values[k] = v;
            ti->indexReg = t->indexReg ? Y(RegName(t->indexReg)) : nullptr;
            *table = ti;
        }
        return tg;
    }
    const Operand& op = i.ops[0];
    if (op.type != OpType::Mem || op.base != R_NONE || op.index == R_NONE || op.scale != 4) return {};
    const uint32_t tab = static_cast<uint32_t>(op.disp);
    std::map<int64_t, uint32_t> vals;
    for (int k = 0; k < 256; ++k) {
        uint32_t v;
        if (!d.U32(tab + 4 * k, &v) || !(lo <= v && v < hi)) break;
        vals[k] = v;
    }
    if (vals.empty()) return {};
    std::set<uint32_t> u;
    for (auto& kv : vals) u.insert(kv.second);
    auto ti = std::make_shared<TableInfo>();
    ti->values = vals;
    ti->indexReg = IsReg32(op.index) ? Y(RegName(op.index)) : nullptr;
    *table = ti;
    return {u.begin(), u.end()};
}

}  // namespace pse::m2l::rd
