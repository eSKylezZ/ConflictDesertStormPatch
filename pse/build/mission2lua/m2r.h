#pragma once
// mission2lua_readable (C++ port) - internal declarations. The readable translator (stage 2: mission DLL -> readable,
// editable Lua) of ModKit conflict_ds_tools/mission2lua_readable.py, ported rule by rule: the output must be
// byte-identical with the Python (gate: scripts/m2r_gate.py). Comments name the Python they follow.
//
// Expressions are the Python's tuples: interned nodes (one node per distinct tuple), so structural equality is pointer
// equality, as `==` on the tuples. A node = a tag ("bin", "c", "loc" ...) + items (integers, floats, symbols, nodes,
// None). A call ("call", target, args, cid, ret) keeps its arguments in an "#args" node.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "m2l_program.h"

namespace pse::m2l::rd {

constexpr int64_t M32 = 0xFFFFFFFFll;
constexpr uint32_t API_TABLE = 0x5ED238;
constexpr int VERSION = 1;

struct Unsupported {
    std::string what;
};

// ---- symbols ---------------------------------------------------------------------------------------------------------
using Sym = const std::string*;
Sym Y(const std::string& s);   // interned symbol

// ---- expression nodes ------------------------------------------------------------------------------------------------
struct Node;
using E = const Node*;   // nullptr = Python None

enum class VK : uint8_t { None, Int, Flt, Sym, Ex };
struct V {
    VK k = VK::None;
    int64_t i = 0;
    double f = 0;
    Sym s = nullptr;
    E e = nullptr;
    V() = default;
    V(int v) : k(VK::Int), i(v) {}
    V(int64_t v) : k(VK::Int), i(v) {}
    V(uint32_t v) : k(VK::Int), i(v) {}
    V(double v) : k(VK::Flt), f(v) {}
    V(Sym v) : k(VK::Sym), s(v) {}
    V(E v) : k(v ? VK::Ex : VK::None), e(v) {}
    V(const char* v) : k(VK::Sym), s(Y(v)) {}
    bool operator==(const V& o) const;
};

struct Node {
    Sym t;
    std::vector<V> v;   // items after the tag: v[0] = Python e[1]
    size_t hash = 0;
    // Python e[k] for k >= 1
    const V& at(size_t k) const { return v[k - 1]; }
    int64_t I(size_t k) const { return v[k - 1].i; }
    double F(size_t k) const { return v[k - 1].f; }
    Sym S(size_t k) const { return v[k - 1].s; }
    E X(size_t k) const { return v[k - 1].e; }
    size_t n() const { return v.size() + 1; }   // len(e)
    bool is(Sym s) const { return t == s; }
};

E Mk(Sym tag, std::vector<V> items);
template <class... A>
E T(Sym tag, A... a) {
    return Mk(tag, std::vector<V>{V(a)...});
}

// the symbols used everywhere
namespace s {
extern Sym c, fc, param, loc, var, g, gaddr, saddr, api, mem, bin, un, call, cmp, not_, and_, or_, b2i, i2f, u2f, fr,
    fbits, fromfbits, fsw, ins, ext, undef, msg, evt, str, t, args, slot, fn, sprintf, ptr, rand, sx, qword, fromdbits,
    f32, dlo, dhi, fistp, fnptr, apislotaddr, apislotptr, apiglobal, apislot, stack, pair, flag, sub, add, res, neg,
    sahf, fswah, fswcmp, reg, i, u, f, d;
// operators
extern Sym plus, minus, mul, div, band, bor, bxor, shl, shr, sar, idiv, imod, udiv, umod, umulhi, eq, ne, lt, le, gt,
    ge, ult, ule, ugt, uge, flt, fle, fgt, fge, feq, fne, neg_, notop;
}  // namespace s
// statement / block / function kinds
namespace kd {
extern Sym set, store, expr, copy, cond, sw, ret, tail, tailmem, fall, jmp, end, call, read, plain, update, message,
    save, load, size, init, setup, queue, branch;
}  // namespace kd

E C(int64_t v);   // ("c", s32(v))
extern thread_local E C0, C1;

// The node pool of one translation (nodes live until it ends). Every translation runs inside one.
class Pool;
class PoolScope {
public:
    PoolScope();
    ~PoolScope();
    PoolScope(const PoolScope&) = delete;
    PoolScope& operator=(const PoolScope&) = delete;

private:
    Pool* pool_;
    Pool* prev_;
    E c0_, c1_;
};
int64_t S32(int64_t v);
int64_t Sx(int64_t v, int size);

// call nodes
E MkCall(E target, const std::vector<E>& args, uint32_t va, uint32_t at, Sym ret);
inline E CallTarget(E c) { return c->X(1); }
inline const std::vector<V>& CallArgs(E c) { return c->X(2)->v; }
inline uint32_t CallVa(E c) { return static_cast<uint32_t>(c->I(3)); }
inline uint32_t CallAt(E c) { return static_cast<uint32_t>(c->I(4)); }
inline Sym CallRet(E c) { return c->S(5); }
std::vector<E> ArgsOf(E c);

// the Python helpers over expressions
std::vector<E> Kids(E e);
bool IsC(E e);
bool IsBool(E e);
bool HasCall(E e);
bool ReadsMemory(E e);
void LocsIn(E e, std::set<Sym>* out);
std::set<Sym> LocsIn(E e);
E Subst(E e, const std::map<Sym, E>& m);
bool IsFloat(E e);
E Simp(E e);
bool VRange(E e, int64_t* lo, int64_t* hi);
bool Stable(E e);
E StripT(E e);
E TagReads(E e, int64_t t);
struct Ev {
    bool call;
    int64_t time;
};
void Events(E e, int64_t now, std::vector<Ev>* out);
bool OrderOk(const std::vector<Ev>& ev);
bool HasKind(E e, Sym k);
void FindKind(E e, Sym k, std::vector<E>* out);
std::vector<E> FindKind(E e, Sym k);
int CountLoc(E e, Sym loc);
// _map_expr: bottom-up rewrite, fn returns the replacement or nullptr
template <class F>
E MapExpr(E e, F&& fn);
E Rebuild(E e, const std::vector<V>& items);   // same tag, new items

// ---- CFG (mission2lua_readable.Block / Func / Program) ---------------------------------------------------------------
struct TableInfo {   // the switch's extra: {"values", "index_reg", "byte_table", "default"}
    std::map<int64_t, uint32_t> values;
    Sym indexReg = nullptr;
    bool hasDefault = false;
    uint32_t def = 0;
};
using TablePtr = std::shared_ptr<TableInfo>;

struct Block {
    uint32_t start = 0;
    std::vector<const Insn*> insns;
    std::vector<uint32_t> succ;
    Sym kind;   // fall / ret / jmp / cond / switch / tail / tailmem / end
    TablePtr table;
    std::vector<uint32_t> preds;
    bool hasTail = false;
    uint32_t tail = 0;
};

struct Func {
    uint32_t va = 0;
    std::map<uint32_t, Block> blocks;   // Python: dict filled in sorted order
    int64_t retPop = 0;
    std::unordered_map<uint32_t, int64_t> espAt, fpdAt;
    std::unordered_map<uint32_t, std::pair<bool, int64_t>> ebpAt;   // (known, value)
    std::set<uint32_t> floatCall;
    int nparams = 0;
    std::vector<std::pair<int64_t, int64_t>> buffers;
    Sym kind = nullptr;   // update / message / save / load / size / init / setup / queue / plain
    bool eventForm = false;
};

class Program {
public:
    bool Load(const std::vector<uint8_t>& file, const std::string& name, std::string* error);
    m2l::Program p;   // stage 1's view (functions, relocs, imports)
    Dll* dll = nullptr;
    std::string name;
    uint32_t base = 0;
    uint32_t obj = 0;
    std::vector<uint32_t> cbs;
    bool hasApiGlobal = false;
    uint32_t apiGlobal = 0;
    uint32_t setup = 0;
    struct Sec {
        uint32_t lo, hi;
        std::string name;
    };
    std::vector<Sec> sections;
    std::vector<std::pair<uint32_t, uint32_t>> rdata, data;
    std::set<uint32_t> sprintf_, rand_, cookie_, crt, entries, queueCbs;
    std::vector<uint32_t> funcOrder;   // Python dict insertion order of funcs
    std::map<uint32_t, std::unique_ptr<Func>> funcs;

    bool InRdata(uint32_t a) const;
    bool InData(uint32_t a) const;
    bool InImage(uint32_t a) const { return a >= base && a < base + p.dll.sizeOfImage; }
    bool U8(uint32_t a, uint8_t* v) const { return p.dll.U8(a, v); }
    bool U32(uint32_t a, uint32_t* v) const { return p.dll.U32(a, v); }
    std::string BytesAt(uint32_t a, size_t n) const;
    bool Cstr(uint32_t va, std::string* out) const;   // missionflow.Dll.cstr
    Func* AddFunc(uint32_t va, std::unique_ptr<Func> f);

private:
    void Discover();
    std::set<uint32_t> CalleesRaw(const m2l::Function& f);
    std::set<uint32_t> Callees(const Func& f);
    uint32_t Limit(uint32_t va);
    std::unique_ptr<Func> Build(uint32_t va);
};

// _table_targets(d, i, lo, hi): (targets, table) - missionflow's shapes, then the run of dwords into the function
std::vector<uint32_t> TableTargets(Dll& d, const Insn& i, bool haveRange, uint32_t lo, uint32_t hi, TablePtr* table);

// ---- IR --------------------------------------------------------------------------------------------------------------
struct Stmt {
    Sym kind;   // set / store / expr / copy / cond / switch / ret / tail
    uint32_t at = 0;
    Sym loc = nullptr;
    E target = nullptr;
    E e = nullptr;
    E ce[3] = {nullptr, nullptr, nullptr};   // copy: (dst, src, n)
    Sym extra = nullptr;                     // "call" / "f"
    TablePtr table;                          // switch
};
struct StmtArena {
    std::vector<std::unique_ptr<Stmt>> all;
    Stmt* New(const Stmt& s) {
        all.push_back(std::make_unique<Stmt>(s));
        return all.back().get();
    }
};
using StmtList = std::vector<Stmt*>;
// {block: [Stmt]} in insertion order (Python dict)
struct IR {
    std::vector<uint32_t> order;
    std::map<uint32_t, StmtList> m;
    StmtList& operator[](uint32_t va) {
        auto it = m.find(va);
        if (it == m.end()) {
            order.push_back(va);
            return m[va];
        }
        return it->second;
    }
    bool has(uint32_t va) const { return m.count(va) != 0; }
};
IR DeepCopy(const IR& ir, StmtArena* arena);

std::vector<E> StmtExprs(const Stmt& s);   // _stmt_exprs / _all_exprs
E StmtExpr(const Stmt& s);                 // _stmt_expr
std::set<Sym> StmtUses(const Stmt& s);
Sym StmtEffect(const Stmt& s);   // call / store / read / nullptr
void SubstStmt(Stmt* s, const std::map<Sym, E>& m);
int Occurrences(const Stmt& s, Sym loc);

// ---- API slots + messages (generated from the ModKit's data: m2r_data.inc) -------------------------------------------
struct SlotArg {
    std::string name, type;
};
struct SlotInfo {
    bool has = false;       // the slot is in the json
    std::string name;       // "" = no name
    bool hasArgs = false;   // "args" in the entry
    std::vector<SlotArg> args;
    bool hasReturns = false;
    std::string returns;
};
struct MsgInfo {
    std::string name, desc;
    std::vector<std::string> params;
};
const std::map<int64_t, SlotInfo>& SlotsJson();
const std::map<int64_t, MsgInfo>& Messages();

// ---- reaching definitions ----------------------------------------------------------------------------------------------
struct DefId {   // (va, k) or ("in", loc)
    bool in = false;
    uint32_t va = 0;
    int k = 0;
    Sym loc = nullptr;
    bool operator<(const DefId& o) const {
        if (in != o.in) return in < o.in;
        if (in) return loc < o.loc;
        return va != o.va ? va < o.va : k < o.k;
    }
    bool operator==(const DefId& o) const { return in == o.in && (in ? loc == o.loc : (va == o.va && k == o.k)); }
    std::string PyStr() const;   // str() of the Python tuple
};
inline DefId InDef(Sym l) {
    DefId d;
    d.in = true;
    d.loc = l;
    return d;
}
inline DefId AtDef(uint32_t va, int k) {
    DefId d;
    d.va = va;
    d.k = k;
    return d;
}
using DefSet = std::set<DefId>;
using RDState = std::map<Sym, DefSet>;

std::vector<uint32_t> Rpo(const std::map<uint32_t, Block>& blocks, uint32_t entry);

// registers (mission2lua_readable._R32 / _SUB)
Sym R32(uint16_t reg);   // the 32-bit general register's name, nullptr for anything else
struct SubReg {
    Sym name;
    int sh;
    int64_t mask;
};
SubReg Sub(uint16_t reg);   // throws Unsupported for a non-general register
Sym SL(int64_t off);        // _sl: stack slot name
std::string Hex(int64_t v); // "%X"

class Translator;

// ---- Lifter ----------------------------------------------------------------------------------------------------------
class Lifter {
public:
    Lifter(Translator* t, Func* f);
    void Frame();
    IR LiftAll();
    std::vector<std::pair<int64_t, int64_t>> regions;
    std::set<int64_t> apiSlotsIn;
    std::set<int64_t> escaped;
    Func* f() const { return f_; }

private:
    using State = std::map<Sym, E>;
    struct StepOut {
        int64_t esp, fpd;
        bool ebpKnown;
        int64_t ebp;
    };
    StepOut Step(const Insn& i, StepOut in, const std::vector<const Insn*>& insns, size_t k);
    bool ReturnsFloat(const std::vector<const Insn*>& insns, size_t from, int64_t fpd);
    E Stack(int64_t off);
    void Escape(E v, const State* st);
    E MemTarget(const Insn& i, const Operand& op, const State& st);
    StmtList LiftBlock(const Block& b, E flags, const State& st);
    E Read(const Insn& i, const Operand& op, State& st, StmtList& out);
    void Write(const Insn& i, const Operand& op, E val, State& st, StmtList& out, bool full = false);
    E Carry(E flags);
    E Cond(Sym cc, E flags, uint32_t at);
    void Call(const Insn& i, State& st, StmtList& out, int64_t esp, int64_t fpd);
    void Tailcall(const Insn& i, E target, StmtList& out, int64_t esp);
    void StringOp(const Insn& i, StmtList& out);
    E Fpu(const Insn& i, State& st, StmtList& out, int64_t fpd, E flags);
    Stmt* New(const Stmt& s);

    Translator* t_;
    Program* p_;
    Func* f_;
    E exitFlags_ = nullptr;
    State exitSt_;
};

// ---- Translator ------------------------------------------------------------------------------------------------------
class Translator {
public:
    bool Load(const std::vector<uint8_t>& file, const std::string& name, std::string* error);
    Program prog;
    std::map<int64_t, SlotInfo> slots;
    std::map<uint32_t, std::set<std::pair<int64_t, Sym>>> gacc;
    std::map<std::string, int64_t> stats;
    std::unordered_map<uint32_t, int64_t> siteArgs;
    std::map<uint32_t, std::set<int64_t>> apiParams;
    std::map<uint32_t, std::set<Sym>> clobbers;
    std::map<uint32_t, std::unique_ptr<Lifter>> lifters;
    std::map<uint32_t, IR> ir;
    StmtArena arena;

    const SlotInfo* Slot(int64_t slot) const {
        auto it = slots.find(slot);
        return it == slots.end() ? nullptr : &it->second;
    }
    E GlobalRead(uint32_t addr, int64_t size, Sym kind);
    int64_t CallArgs(E target, int64_t esp, uint32_t at, Func* f);
    std::map<Sym, E> EntryValues(const Func& f) const;
    std::set<Sym> ClobbersOr(bool known, uint32_t va, std::initializer_list<const char*> dflt) const;
    Func* F(uint32_t va) { return prog.funcs.at(va).get(); }

private:
    void ParamCounts();
    void Clobbers();
    void SiteArgs();
    std::set<int64_t> BlockWritesAt(Func& f, const Block& b, uint32_t at);
    const std::map<uint32_t, std::set<int64_t>>& BlockWrites(Func& f, const Block& b);
    std::set<uint32_t> PrologueSaves(Func& f);
    std::set<int64_t> WrittenBefore(Func& f, uint32_t bva, size_t k, std::set<uint32_t> seen);
    int64_t SprintfFormat(Func& f, uint32_t at, int64_t esp);
    std::set<int64_t> SlotImm(Func& f, uint32_t bva, size_t k, int64_t off, std::set<uint32_t> seen);
    std::set<int64_t> VectorOffsets(const IR& ir);

    std::map<std::pair<uint32_t, uint32_t>, std::map<uint32_t, std::set<int64_t>>> wbBlock_;
    std::map<std::pair<uint32_t, uint32_t>, std::set<int64_t>> wbMemo_;
    std::map<std::pair<uint32_t, uint32_t>, std::set<int64_t>> wbEnd_;
    std::set<uint32_t> saves_;
};

int FormatDwords(const std::string& fmt);   // _format_dwords

// ---- FnOpt -----------------------------------------------------------------------------------------------------------
class FnOpt {
public:
    FnOpt(Func* f, IR* ir, bool returns, std::map<Sym, E> entryValues);
    std::map<uint32_t, RDState> Rd();
    void Run();
    struct Use {
        uint32_t va;
        int k;
        Sym loc;
    };
    std::map<DefId, std::vector<Use>> UsesOf(std::map<uint32_t, RDState>& ins);
    Func* f;
    IR* ir;
    std::vector<uint32_t> order;
    std::map<Sym, E> entryValues;

private:
    bool PassNarrow();
    E DefVal(const DefId& d);
    bool PassPure();
    bool PassInline();
    bool CanInline(const StmtList& stmts, size_t i, size_t j);
    std::set<uint32_t> Between(uint32_t dva, uint32_t uva);
    bool PassInlineX();
    bool PassDead();
};

// ---- structuring (AST) -----------------------------------------------------------------------------------------------
// ("s", Stmt) | ("if", [(cond, body)], else) | ("while", cond | None, body) | ("repeat", body, cond) | ("break",)
// | ("continue", head) | ("goto", label) | ("label", label) | ("return", expr | None, kind)
// | ("switch", on, [(values, body)], default) | ("setflag", name, value). An empty else / default = None.
struct Ast;
using Body = std::vector<Ast>;
struct Ast {
    enum K { S, If, While, Repeat, Break, Continue, Goto, Label, Return, Switch, SetFlag } k;
    Stmt* st = nullptr;
    std::vector<std::pair<E, Body>> arms;   // if: (cond, body)
    Body els;                               // if: else; while / repeat: body; switch: default
    E cond = nullptr;                       // while / repeat condition, switch: on, return: expr
    Sym rkind = nullptr;                    // return: ret / tail
    int64_t label = 0;                      // goto / label / continue head
    std::vector<std::pair<std::vector<int64_t>, Body>> cases;
    Sym flag = nullptr;                     // setflag
    bool flagValue = false;
    static Ast Mk(K k) {
        Ast a;
        a.k = k;
        return a;
    }
    bool operator==(const Ast& o) const;
};

struct StructResult {
    Body ast;
    std::set<int64_t> labels;
    std::vector<Sym> flags;
};
// Structurer(tr, f, ir, stats) with no_goto / labels_final, then run()
StructResult Structure(Translator& tr, Func& f, IR& ir, std::map<std::string, int64_t>& stats,
                       const std::set<int64_t>& noGoto, const std::set<int64_t>& labelsFinal);
std::set<int64_t> InvisibleGotos(const Body& body);

}  // namespace pse::m2l::rd

#include "m2r_map.inl"
