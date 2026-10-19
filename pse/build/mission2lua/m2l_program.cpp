// mission2lua (C++ port): PE image, functions, program - mirrors missionflow.Dll / Function and mission2lua.Program
// (ModKit conflict_ds_tools). Comments name the Python they follow.
#include "m2l_program.h"

#include <algorithm>
#include <cstring>

namespace pse::m2l {

namespace {

uint32_t Rd32(const std::vector<uint8_t>& f, size_t o) {
    if (o + 4 > f.size()) return 0;
    uint32_t v;
    std::memcpy(&v, f.data() + o, 4);
    return v;
}
uint16_t Rd16(const std::vector<uint8_t>& f, size_t o) {
    if (o + 2 > f.size()) return 0;
    uint16_t v;
    std::memcpy(&v, f.data() + o, 2);
    return v;
}

// missionflow._mem: base / index as 32-bit register (sub-registers count as their full register)
struct MemParts {
    uint16_t base, index;
    int scale;
    int64_t disp;   // disp & 0xFFFFFFFF if disp >= 0 else disp
};
MemParts MemOf(const Operand& o) {
    MemParts m;
    m.base = Full32(o.base);
    m.index = Full32(o.index);
    m.scale = o.scale;
    m.disp = o.disp >= 0 ? (o.disp & 0xFFFFFFFF) : o.disp;
    return m;
}

bool StartsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

bool IsJccMissionflow(const std::string& m) {   // missionflow._JCC keys
    static const char* const k[] = {"je", "jz", "jne", "jnz", "jl", "jb", "jg", "ja", "jle", "jbe", "jge", "jae", "js",
                                    "jns"};
    for (const char* s : k)
        if (m == s) return true;
    return false;
}

}  // namespace

// ---- Dll (pefile semantics) ------------------------------------------------------------------------------------------
bool Dll::Parse(const std::vector<uint8_t>& f, std::string* error) {
    auto fail = [&](const char* why) {
        if (error) *error = why;
        return false;
    };
    if (f.size() < 0x40 || f[0] != 'M' || f[1] != 'Z') return fail("not a PE file");
    const uint32_t pe = Rd32(f, 0x3C);
    if (pe + 0x18 > f.size() || Rd32(f, pe) != 0x4550) return fail("not a PE file");
    const uint16_t nsec = Rd16(f, pe + 6);
    const uint16_t optSize = Rd16(f, pe + 20);
    const size_t opt = pe + 24;
    if (Rd16(f, opt) != 0x10B) return fail("not a 32-bit PE");
    entryRva = Rd32(f, opt + 16);
    base = Rd32(f, opt + 28);
    const uint32_t sectionAlign = Rd32(f, opt + 32), fileAlign = Rd32(f, opt + 36);
    sizeOfImage = Rd32(f, opt + 56);
    const uint32_t ndirs = Rd32(f, opt + 92);
    auto dir = [&](int k, uint32_t* rva, uint32_t* size) {
        *rva = *size = 0;
        if (static_cast<uint32_t>(k) >= ndirs) return;
        *rva = Rd32(f, opt + 96 + k * 8);
        *size = Rd32(f, opt + 100 + k * 8);
    };
    (void)fileAlign;
    // sections
    const size_t secTable = opt + optSize;
    uint32_t lowestRaw = 0;
    for (int k = 0; k < nsec; ++k) {
        const size_t s = secTable + k * 40;
        if (s + 40 > f.size()) return fail("section table outside the file");
        Section sec;
        char nm[9] = {};
        std::memcpy(nm, f.data() + s, 8);
        sec.name = nm;
        sec.vsize = Rd32(f, s + 8);
        sec.va = Rd32(f, s + 12);
        sec.rawSize = Rd32(f, s + 16);
        sec.rawPtr = Rd32(f, s + 20);
        // get_data(): from PointerToRawData_adj (rounded down to 0x200) to PointerToRawData + SizeOfRawData
        uint32_t off = sec.rawPtr & ~0x1FFu;
        if (sectionAlign < 0x1000 && sec.rawPtr == sec.va) off = sec.va;
        size_t end = static_cast<size_t>(off) + sec.rawSize;
        if (end > static_cast<size_t>(sec.rawPtr) + sec.rawSize) end = static_cast<size_t>(sec.rawPtr) + sec.rawSize;
        if (off < f.size()) sec.data.assign(f.begin() + off, f.begin() + std::min(end, f.size()));
        if (sec.rawPtr > 0) {
            const uint32_t adj = sec.rawPtr & ~0x1FFu;
            if (!lowestRaw || adj < lowestRaw) lowestRaw = adj;
        }
        sections.push_back(std::move(sec));
    }
    // header = data[:lowest section offset] (or up to the section table's end)
    const size_t headerEnd = (!lowestRaw || lowestRaw < secTable + nsec * 40) ? secTable + nsec * 40 : lowestRaw;
    img.assign(f.begin(), f.begin() + std::min(headerEnd, f.size()));
    for (const Section& s : sections) {
        if (s.vsize == 0 && s.rawSize == 0) continue;
        const uint32_t prd = s.rawPtr & ~0x1FFu;
        uint32_t sa = sectionAlign < 0x1000 ? fileAlign : sectionAlign;
        uint32_t vaAdj = (sa && s.va % sa) ? sa * (s.va / sa) : s.va;
        if (s.rawSize > f.size() || prd > f.size() || static_cast<size_t>(s.rawSize) + prd > f.size() || vaAdj >= 0x10000000)
            continue;
        if (vaAdj > img.size()) img.resize(vaAdj, 0);
        else if (vaAdj < img.size()) img.resize(vaAdj);
        img.insert(img.end(), s.data.begin(), s.data.end());
    }
    for (const Section& s : sections)
        if (StartsWith(s.name, ".text")) {
            textLo = base + s.va;
            textHi = textLo + s.vsize;
            break;
        }
    if (!textLo) return fail("no .text section");
    auto rvaToOff = [&](uint32_t rva) -> int64_t {   // file offset of an RVA (through the sections)
        for (const Section& s : sections) {
            const uint32_t span = std::max(s.vsize, s.rawSize);
            if (rva >= s.va && rva < s.va + span) return static_cast<int64_t>(s.rawPtr & ~0x1FFu) + (rva - s.va);
        }
        return rva < f.size() ? rva : -1;
    };
    auto cstr = [&](uint32_t rva) {
        std::string out;
        int64_t o = rvaToOff(rva);
        if (o < 0) return out;
        for (size_t p = static_cast<size_t>(o); p < f.size() && f[p]; ++p) out += static_cast<char>(f[p]);
        return out;
    };
    // relocations (type 3 only; type 0 = padding; anything else -> Unsupported in the Python)
    uint32_t rrva, rsize;
    dir(5, &rrva, &rsize);
    if (rrva && rsize) {
        int64_t o = rvaToOff(rrva);
        uint32_t done = 0;
        while (o >= 0 && done + 8 <= rsize) {
            const uint32_t page = Rd32(f, static_cast<size_t>(o + done)), blk = Rd32(f, static_cast<size_t>(o + done + 4));
            if (blk < 8) break;
            for (uint32_t e = 8; e + 2 <= blk; e += 2) {
                const uint16_t v = Rd16(f, static_cast<size_t>(o + done + e));
                const int type = v >> 12;
                if (type == 3) relocs.insert(base + page + (v & 0xFFF));
                else if (type != 0) return fail(("relocation type " + std::to_string(type)).c_str());
            }
            done += blk;
        }
    }
    // imports
    uint32_t irva, isize;
    dir(1, &irva, &isize);
    if (irva) {
        int64_t o = rvaToOff(irva);
        for (int d = 0; o >= 0; ++d) {
            const size_t e = static_cast<size_t>(o) + d * 20;
            const uint32_t oft = Rd32(f, e), nameRva = Rd32(f, e + 12), ft = Rd32(f, e + 16);
            if (!oft && !nameRva && !ft) break;
            const std::string dllName = cstr(nameRva);
            const uint32_t thunks = oft ? oft : ft;
            int64_t to = rvaToOff(thunks);
            for (uint32_t k = 0; to >= 0; ++k) {
                const uint32_t t = Rd32(f, static_cast<size_t>(to) + k * 4);
                if (!t) break;
                Import imp;
                imp.iat = base + ft + k * 4;
                imp.dll = dllName;
                if (t & 0x80000000u) imp.ordinal = t & 0xFFFF;
                else imp.name = cstr(t + 2);
                imports.push_back(imp);
            }
        }
    }
    // exports (named)
    uint32_t erva, esize;
    dir(0, &erva, &esize);
    if (erva) {
        int64_t o = rvaToOff(erva);
        if (o >= 0) {
            const uint32_t nnames = Rd32(f, static_cast<size_t>(o) + 24), funcs = Rd32(f, static_cast<size_t>(o) + 28),
                           names = Rd32(f, static_cast<size_t>(o) + 32), ords = Rd32(f, static_cast<size_t>(o) + 36);
            const int64_t fo = rvaToOff(funcs), no = rvaToOff(names), oo = rvaToOff(ords);
            for (uint32_t k = 0; k < nnames && fo >= 0 && no >= 0 && oo >= 0; ++k) {
                const uint16_t ord = Rd16(f, static_cast<size_t>(oo) + k * 2);
                const uint32_t frva = Rd32(f, static_cast<size_t>(fo) + ord * 4);
                const std::string n = cstr(Rd32(f, static_cast<size_t>(no) + k * 4));
                if (!n.empty()) exports[n] = base + frva;
            }
        }
    }
    return true;
}

bool Dll::U32(uint32_t va, uint32_t* out) const {
    const int64_t o = static_cast<int64_t>(va) - base;
    if (o < 0 || o > static_cast<int64_t>(img.size()) - 4) return false;
    std::memcpy(out, img.data() + o, 4);
    return true;
}
bool Dll::U8(uint32_t va, uint8_t* out) const {
    const int64_t o = static_cast<int64_t>(va) - base;
    if (o < 0 || o >= static_cast<int64_t>(img.size())) return false;
    *out = img[static_cast<size_t>(o)];
    return true;
}

const Insn* Dll::At(uint32_t va) {
    auto it = cache_.find(va);
    if (it != cache_.end()) return it->second.get();
    if (bad_.count(va)) return nullptr;
    const int64_t o = static_cast<int64_t>(va) - base;
    auto in = std::make_unique<Insn>();
    if (o < 0 || o >= static_cast<int64_t>(img.size()) ||
        !Decode(img.data() + o, std::min<size_t>(16, img.size() - static_cast<size_t>(o)), va, in.get())) {
        bad_.insert(va);
        return nullptr;
    }
    return (cache_[va] = std::move(in)).get();
}

// ---- Function (missionflow.Function) ---------------------------------------------------------------------------------
Function::Function(Dll& dll, uint32_t e) : entry(e), d_(dll) {
    std::set<uint32_t> starts{entry}, seen;
    std::vector<uint32_t> todo{entry};
    // pass 1: block starts
    while (!todo.empty()) {
        uint32_t va = todo.back();
        todo.pop_back();
        while (!seen.count(va)) {
            seen.insert(va);
            const Insn* i = d_.At(va);
            if (!i || !d_.InText(va)) break;
            const std::string& m = i->mnemonic;
            if (m == "ret" || m == "retn") break;
            if (m == "jmp") {
                const Operand& op = i->ops[0];
                if (op.type == OpType::Imm) {
                    const uint32_t t = static_cast<uint32_t>(op.imm);
                    if (d_.InText(t)) {
                        starts.insert(t);
                        todo.push_back(t);
                    }
                } else {
                    for (uint32_t t : TableTargets(*i)) {
                        starts.insert(t);
                        todo.push_back(t);
                    }
                }
                break;
            }
            if (IsJccMissionflow(m) ||
                (StartsWith(m, "j") && m != "jmp" && i->nops && i->ops[0].type == OpType::Imm)) {
                const uint32_t t = static_cast<uint32_t>(i->ops[0].imm), nx = i->address + i->size;
                starts.insert(t);
                starts.insert(nx);
                todo.push_back(t);
                todo.push_back(nx);
                break;
            }
            if (m == "int3" || m == "hlt") break;
            va = i->address + i->size;
        }
    }
    // pass 2: cut blocks
    for (uint32_t s : starts) {
        if (blocks.count(s) || !d_.InText(s)) continue;
        Block b;
        b.start = s;
        uint32_t va = s;
        for (;;) {
            const Insn* i = d_.At(va);
            if (!i) {
                b.kind = Block::End;
                break;
            }
            b.insns.push_back(i);
            const std::string& m = i->mnemonic;
            const uint32_t nxt = i->address + i->size;
            if (m == "ret" || m == "retn") {
                b.kind = Block::Ret;
                break;
            }
            if (m == "jmp") {
                const Operand& op = i->ops[0];
                if (op.type == OpType::Imm) {
                    b.kind = Block::Jmp;
                    b.succ = {static_cast<uint32_t>(op.imm)};
                } else if (op.type == OpType::Mem && MemOf(op).base && !MemOf(op).index) {
                    b.kind = Block::TailCall;
                } else {
                    std::vector<uint32_t> tg = TableTargets(*i);
                    b.kind = tg.empty() ? Block::End : Block::Switch;
                    b.succ = tg;
                    b.table = std::move(last_);
                }
                break;
            }
            if (StartsWith(m, "j") && m != "jmp") {
                b.kind = Block::Cond;
                b.succ = {static_cast<uint32_t>(i->ops[0].imm), nxt};
                break;
            }
            if (m == "int3" || m == "hlt") {
                b.kind = Block::End;
                break;
            }
            if (starts.count(nxt)) {
                b.kind = Block::Fall;
                b.succ = {nxt};
                break;
            }
            va = nxt;
        }
        blocks.emplace(s, std::move(b));
    }
    for (auto& [va, b] : blocks) {
        std::vector<uint32_t> keep;
        for (uint32_t t : b.succ)
            if (blocks.count(t)) keep.push_back(t);
        b.succ = keep;
    }
}

// missionflow.Function._table_targets: jmp dword ptr [reg*4 + table], the index maybe from a byte table
std::vector<uint32_t> Function::TableTargets(const Insn& i) {
    std::vector<uint32_t> tg = TableTargetsBase(i);
    return tg.empty() ? RegisterBound(i) : tg;
}

// mission2lua._Function._table_targets (VERSION 2): the bound in a register set earlier -
// `mov esi, 0x2d ... cmp eax, esi; ja default; jmp [eax*4 + table]` (Training2.dll 0x1000228C, 46 cases); accepted only
// when every one of the bound + 1 table entries is a relocated pointer into .text.
std::vector<uint32_t> Function::RegisterBound(const Insn& i) {
    const Operand& op = i.ops[0];
    if (op.type != OpType::Mem) return {};
    const MemParts mp = MemOf(op);
    if (mp.base || !mp.index || mp.scale != 4) return {};
    std::vector<const Insn*> back;   // linear decode from up to 256 bytes back, the earliest start landing on i
    const int64_t maxK = std::min<int64_t>(256, static_cast<int64_t>(i.address) - d_.textLo);
    for (int64_t k = maxK; k > 0; --k) {
        std::vector<const Insn*> out;
        uint32_t a = static_cast<uint32_t>(i.address - k);
        while (a < i.address) {
            const Insn* p = d_.At(a);
            if (!p) break;
            out.push_back(p);
            a += p->size;
        }
        if (a == i.address && !out.empty()) {
            back = std::move(out);
            break;
        }
    }
    bool haveBound = false, haveReg2 = false;
    int64_t bound = 0;
    uint16_t reg2 = R_NONE;
    for (auto it = back.rbegin(); it != back.rend(); ++it) {
        const Insn& p = **it;
        if (!haveReg2) {
            if (p.mnemonic == "cmp" && p.nops == 2 && p.ops[0].type == OpType::Reg && p.ops[1].type == OpType::Reg &&
                Full32(p.ops[0].reg) && Full32(p.ops[0].reg) == mp.index) {
                reg2 = Full32(p.ops[1].reg);
                haveReg2 = reg2 != R_NONE;   // (a non-general register: the Python keeps looking, as here)
            }
            continue;
        }
        if (p.nops && p.ops[0].type == OpType::Reg && Full32(p.ops[0].reg) == reg2 && p.mnemonic != "cmp" &&
            p.mnemonic != "test" && p.mnemonic != "push") {
            if (p.mnemonic == "mov" && p.ops[1].type == OpType::Imm) {
                bound = p.ops[1].imm;
                haveBound = true;
            }
            break;
        }
    }
    if (!haveBound || bound > 0x400) return {};
    std::map<uint32_t, uint32_t> caseOf;
    for (int64_t k = 0; k < bound + 1; ++k) {
        const uint32_t at = static_cast<uint32_t>(mp.disp + 4 * k);
        uint32_t v;
        if (!d_.relocs.count(at) || !d_.U32(at, &v) || !d_.InText(v)) return {};
        caseOf[static_cast<uint32_t>(k)] = v;
    }
    if (caseOf.empty()) return {};
    last_ = std::make_unique<Table>();
    last_->values = caseOf;
    last_->tableVa = static_cast<uint32_t>(mp.disp);
    std::set<uint32_t> uniq;
    for (auto& kv : caseOf) uniq.insert(kv.second);
    return {uniq.begin(), uniq.end()};
}

std::vector<uint32_t> Function::TableTargetsBase(const Insn& i) {
    last_.reset();
    const Operand& op = i.ops[0];
    if (op.type != OpType::Mem) return {};
    const MemParts mp = MemOf(op);
    if (mp.base || !mp.index || mp.scale != 4) return {};
    std::vector<const Insn*> back = Back(i.address, 12);
    bool haveBound = false;
    int64_t bound = 0, byteTable = 0;
    uint16_t byteIndex = R_NONE;
    bool haveByteTable = false;
    for (auto it = back.rbegin(); it != back.rend(); ++it) {
        const Insn& p = **it;
        if ((p.mnemonic == "mov" || p.mnemonic == "movzx") && p.nops == 2 && p.ops[1].type == OpType::Mem &&
            p.ops[1].size == 1 && p.ops[0].type == OpType::Reg && Full32(p.ops[0].reg) && Full32(p.ops[0].reg) == mp.index) {
            const MemParts bm = MemOf(p.ops[1]);
            if (bm.base && !bm.index && bm.disp) {
                byteTable = bm.disp;
                byteIndex = bm.base;
                haveByteTable = true;
            }
        }
        if (p.mnemonic == "cmp" && p.ops[1].type == OpType::Imm && p.ops[0].type == OpType::Reg) {
            const uint16_t r = Full32(p.ops[0].reg);
            if (r && (r == mp.index || r == byteIndex)) {
                bound = p.ops[1].imm;
                haveBound = true;
                break;
            }
        }
    }
    if (!haveBound || bound > 0x400) return {};
    std::map<uint32_t, uint32_t> caseOf;
    if (haveByteTable && byteTable) {
        std::vector<uint8_t> idx;
        for (int64_t k = 0; k < bound + 1; ++k) {
            uint8_t v;
            if (byteTable + k < 0 || byteTable + k > 0xFFFFFFFFll || !d_.U8(static_cast<uint32_t>(byteTable + k), &v))
                return {};
            idx.push_back(v);
        }
        if (idx.empty()) return {};   // the Python fails here (max of an empty list); never seen
        const int n = *std::max_element(idx.begin(), idx.end()) + 1;
        std::vector<int64_t> entries;
        for (int k = 0; k < n; ++k) {
            uint32_t v;
            entries.push_back(d_.U32(static_cast<uint32_t>(mp.disp + 4 * k), &v) ? static_cast<int64_t>(v) : -1);
        }
        for (int64_t k = 0; k < bound + 1; ++k) {
            const int64_t t = entries[idx[static_cast<size_t>(k)]];
            if (t < 0 || !d_.InText(static_cast<uint32_t>(t))) return {};
            caseOf[static_cast<uint32_t>(k)] = static_cast<uint32_t>(t);
        }
    } else {
        for (int64_t k = 0; k < bound + 1; ++k) {
            uint32_t v;
            if (!d_.U32(static_cast<uint32_t>(mp.disp + 4 * k), &v) || !d_.InText(v)) return {};
            caseOf[static_cast<uint32_t>(k)] = v;
        }
    }
    last_ = std::make_unique<Table>();
    last_->values = caseOf;
    last_->tableVa = static_cast<uint32_t>(mp.disp);
    last_->byteTable = haveByteTable && byteTable;
    std::set<uint32_t> uniq;
    for (auto& kv : caseOf) uniq.insert(kv.second);
    return {uniq.begin(), uniq.end()};
}

// missionflow.Function._back: up to n instructions before va (linear, from the earliest start offset that lands on va)
std::vector<const Insn*> Function::Back(uint32_t va, size_t n) {
    const int64_t maxK = std::min<int64_t>(64, static_cast<int64_t>(va) - d_.textLo);
    for (int64_t k = maxK; k > 0; --k) {
        std::vector<const Insn*> out;
        uint32_t a = static_cast<uint32_t>(va - k);
        bool ok = false;
        while (a < va) {
            const Insn* i = d_.At(a);
            if (!i) break;
            out.push_back(i);
            a += i->size;
            if (a == va) ok = true;
        }
        if (ok && !out.empty()) {
            if (out.size() > n) out.erase(out.begin(), out.end() - n);
            return out;
        }
    }
    return {};
}

// ---- Program (mission2lua.Program) -----------------------------------------------------------------------------------
bool Program::Load(const std::vector<uint8_t>& file, const std::string& nm, std::string* error) {
    raw = file;
    name = nm;
    if (!dll.Parse(file, error)) return false;
    Dll& d = dll;
    entry = d.base + d.entryRva;
    taken.insert(entry);
    for (auto& e : d.exports) taken.insert(e.second);
    for (uint32_t r : d.relocs) {
        if (d.InText(r)) continue;
        uint32_t t;
        if (d.U32(r, &t) && d.InText(t)) taken.insert(t);
    }
    std::vector<uint32_t> todo(taken.begin(), taken.end());
    while (!todo.empty()) {
        const uint32_t va = todo.back();
        todo.pop_back();
        if (functions.count(va)) continue;
        auto f = std::make_unique<Function>(d, va);
        for (auto& [bva, b] : f->blocks)
            for (const Insn* i : b.insns) {
                if (i->mnemonic == "call" && i->nops && i->ops[0].type == OpType::Imm &&
                    d.InText(static_cast<uint32_t>(i->ops[0].imm)))
                    todo.push_back(static_cast<uint32_t>(i->ops[0].imm));
                for (int k = 0; k < i->nops; ++k) {
                    const Operand& op = i->ops[k];
                    if (op.type == OpType::Imm && i->imm_offset && d.relocs.count(i->address + i->imm_offset)) {
                        const uint32_t t = static_cast<uint32_t>(op.imm);
                        if (d.InText(t)) {
                            taken.insert(t);
                            todo.push_back(t);
                        }
                    }
                }
            }
        functions[va] = std::move(f);
    }
    return true;
}

}  // namespace pse::m2l
