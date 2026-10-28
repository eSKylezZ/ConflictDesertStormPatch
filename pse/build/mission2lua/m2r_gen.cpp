// mission2lua_readable (C++ port): IR -> Lua source for one DLL - mission2lua_readable.Generator / Printer and the
// text helpers (lua_str, _int_lit, _f32_short, lua_literal, bundle_text ...).
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <functional>
#include <optional>
#include <regex>

#include "m2r.h"
#include "mission2lua.h"

namespace pse::m2l::rd {

#include "m2r_data.inc"

const std::map<int64_t, SlotInfo>& SlotsJson() {
    static const std::map<int64_t, SlotInfo> m = [] {
        std::map<int64_t, SlotInfo> o;
        for (const SlotRow& r : SlotRows()) {
            SlotInfo si;
            si.has = true;
            si.name = r.name;
            si.hasArgs = r.hasArgs;
            for (auto& a : r.args) si.args.push_back({a.first, a.second});
            si.hasReturns = r.hasReturns;
            si.returns = r.returns;
            o[r.slot] = si;
        }
        return o;
    }();
    return m;
}
const std::map<int64_t, MsgInfo>& Messages() {
    static const std::map<int64_t, MsgInfo> m = [] {
        std::map<int64_t, MsgInfo> o;
        for (const MsgRow& r : MsgRows()) {
            MsgInfo mi;
            mi.name = r.name;
            mi.desc = r.desc;
            for (const char* p : r.params) mi.params.push_back(p);
            o[r.id] = mi;
        }
        return o;
    }();
    return m;
}

namespace {

std::string Join(const char* const* parts) {
    std::string s;
    for (const char* const* p = parts; *p; ++p) s += *p;
    return s;
}

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n >= 0 && n < static_cast<int>(sizeof buf)) return buf;
    std::string big(static_cast<size_t>(n) + 1, '\0');
    va_start(ap, fmt);
    std::vsnprintf(big.data(), big.size(), fmt, ap);
    va_end(ap);
    big.pop_back();
    return big;
}

std::string JoinStr(const std::vector<std::string>& v, const std::string& sep) {
    std::string o;
    for (size_t k = 0; k < v.size(); ++k) {
        if (k) o += sep;
        o += v[k];
    }
    return o;
}

bool StartsWith(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }

std::string ReplaceAll(std::string s, const std::string& a, const std::string& b) {
    size_t at = 0;
    while ((at = s.find(a, at)) != std::string::npos) {
        s.replace(at, a.size(), b);
        at += b.size();
    }
    return s;
}

std::vector<std::string> SplitLines(const std::string& text) {   // str.splitlines() for \n-only text
    std::vector<std::string> out;
    size_t at = 0;
    while (at < text.size()) {
        size_t e = text.find('\n', at);
        if (e == std::string::npos) {
            out.push_back(text.substr(at));
            break;
        }
        std::string line = text.substr(at, e - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        at = e + 1;
    }
    return out;
}

// ---- Python's text of numbers ----------------------------------------------------------------------------------------
std::string HexU(int64_t v) { return "0x" + Hex(v); }   // _hex: "0x%X" % v

std::string IntLit(int64_t v) {   // _int_lit
    if (-0x10000 < v && v < 0x10000) return std::to_string(v);
    if (v >= 0) return HexU(v);
    return "-" + HexU(-v);
}

std::string PyRepr(double v) {   // repr(float)
    if (std::isnan(v)) return "nan";
    if (std::isinf(v)) return v > 0 ? "inf" : "-inf";
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    bool neg = false;
    if (!sci.empty() && sci[0] == '-') {
        neg = true;
        sci = sci.substr(1);
    }
    const size_t e = sci.find('e');
    std::string mant = sci.substr(0, e);
    const int exp10 = std::stoi(sci.substr(e + 1));
    std::string digits;
    for (char ch : mant)
        if (ch != '.') digits += ch;
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    const int decpt = exp10 + 1;
    std::string out;
    if (digits == "0") {
        out = "0.0";
    } else if (decpt <= -4 || decpt > 16) {
        out = digits.substr(0, 1);
        if (digits.size() > 1) out += "." + digits.substr(1);
        const int x = decpt - 1;
        char eb[16];
        std::snprintf(eb, sizeof eb, "e%c%02d", x < 0 ? '-' : '+', x < 0 ? -x : x);
        out += eb;
    } else if (decpt <= 0) {
        out = "0." + std::string(static_cast<size_t>(-decpt), '0') + digits;
    } else if (decpt < static_cast<int>(digits.size())) {
        out = digits.substr(0, decpt) + "." + digits.substr(decpt);
    } else {
        out = digits + std::string(static_cast<size_t>(decpt) - digits.size(), '0') + ".0";
    }
    return neg ? "-" + out : out;
}

std::string FloatLiteral(double v) {   // _float_literal
    if (v != v) return "(0/0)";
    if (std::isinf(v)) return v > 0 ? "math.huge" : "-math.huge";
    std::string r = PyRepr(v);
    if (r.find('.') == std::string::npos && r.find('e') == std::string::npos) r += ".0";
    return r;
}

std::string F32Short(double v) {   // _f32_short
    if (v != v || std::isinf(v)) return FloatLiteral(v);
    for (int digits = 1; digits < 10; ++digits) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%.*g", digits, v);
        const double back = std::strtod(buf, nullptr);
        if (static_cast<double>(static_cast<float>(back)) == v) {
            std::string r = PyRepr(back);
            if (r.find('.') == std::string::npos && r.find('e') == std::string::npos) r += ".0";
            return r;
        }
    }
    return FloatLiteral(v);
}

double BitsToFloat(int64_t bits) {
    const uint32_t b = static_cast<uint32_t>(bits & M32);
    float f;
    std::memcpy(&f, &b, 4);
    return f;
}

bool LooksFloat(int64_t bits) {   // _looks_float
    bits &= M32;
    if (bits == 0 || bits == 0x80000000ll) return false;
    const int64_t e = (bits >> 23) & 0xFF;
    if (e < 0x60 || e > 0xA0) return false;
    std::string s = F32Short(BitsToFloat(bits));
    std::string t;
    for (char ch : s)
        if (ch != '-' && ch != '.' && ch != 'e') t += ch;
    size_t a = 0;
    while (a < t.size() && t[a] == '0') ++a;
    t = t.substr(a);
    while (!t.empty() && t.back() == '0') t.pop_back();
    return t.size() <= 7;
}

std::string LuaStr(const std::string& sv) {   // lua_str (latin-1 bytes = code points)
    std::string out = "\"";
    for (unsigned char ch : sv) {
        if (ch == '\\') out += "\\\\";
        else if (ch == '"') out += "\\\"";
        else if (ch == '\n') out += "\\n";
        else if (ch == '\r') out += "\\r";
        else if (ch == '\t') out += "\\t";
        else if (ch >= 32 && ch < 127) out += static_cast<char>(ch);
        else out += "\\" + std::to_string(ch);
    }
    return out + "\"";
}

const std::set<std::string>& LuaKeywords() {
    static const std::set<std::string> k = {"and",   "break", "do",  "else", "elseif", "end",    "false",
                                            "for",   "function", "goto", "if", "in",   "local",  "nil",
                                            "not",   "or",    "repeat", "return", "then", "true", "until",
                                            "while"};
    return k;
}
const std::set<std::string>& Reserved() {
    static const std::set<std::string> r = [] {
        std::set<std::string> o = LuaKeywords();
        for (const char* x :
             {"api",       "mem",     "msg",      "on",     "game",     "string",  "math",    "table",   "X",
              "self",      "_G",      "_ENV",     "init",   "update",   "on_message", "print", "pairs",   "ipairs",
              "type",      "error",   "select",   "sprintf", "crt_rand", "scratch", "ADDR",    "ult",     "ule",
              "ugt",       "uge",     "f32",      "fr",     "bits",     "asfloat", "b2i",     "sx8",     "sx16",
              "idiv",      "imod",    "udiv",     "umod",   "umulhi",   "float",   "qword",   "STRINGS", "EVENTS",
              "REFS",      "R",       "buffer",   "frame",  "call_ptr", "load",    "save",    "next",    "setmetatable",
              "tostring",  "tonumber", "D",       "obj",    "wp",       "rgn",     "mk"})
            o.insert(x);
        return o;
    }();
    return r;
}

std::string Ident(const std::string& text, const std::string& fallback = "v") {   // _ident
    std::string t;
    for (unsigned char ch : text) t += (std::isalnum(ch) && ch < 128) || ch == '_' ? static_cast<char>(ch) : '_';
    size_t a = 0, b = t.size();
    while (a < b && t[a] == '_') ++a;
    while (b > a && t[b - 1] == '_') --b;
    t = t.substr(a, b - a);
    for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string u;
    for (char ch : t)
        if (!(ch == '_' && !u.empty() && u.back() == '_')) u += ch;
    if (u.empty() || std::isdigit(static_cast<unsigned char>(u[0]))) u = fallback + (u.empty() ? "" : "_" + u);
    return u;
}

std::string ParamWord(const std::string& text) {   // _param_word
    std::string t;
    for (size_t k = 0; k < text.size();) {   // re.sub(r"\(.*?\)", "")
        if (text[k] == '(') {
            size_t e = k + 1;
            while (e < text.size() && text[e] != ')' && text[e] != '\n') ++e;
            if (e < text.size() && text[e] == ')') {
                k = e + 1;
                continue;
            }
        }
        t += text[k++];
    }
    t = ReplaceAll(t, "the ", "");
    t = ReplaceAll(t, "?", "");
    size_t a = 0, b = t.size();
    while (a < b && std::isspace(static_cast<unsigned char>(t[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(t[b - 1]))) --b;
    t = t.substr(a, b - a);
    const size_t sl = t.find(" / ");
    return Ident(sl == std::string::npos ? t : t.substr(0, sl), "a");
}

// ---- lua_literal -----------------------------------------------------------------------------------------------------
struct LV;
using LVp = std::shared_ptr<LV>;
struct LV {   // a Python value for lua_literal
    enum K { Bool, Int, Float, Str, Nil, List, Dict } k = Nil;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::string s;
    std::vector<LVp> list;
    std::vector<std::pair<LVp, LVp>> dict;   // keys: Int or Str
};
LVp LInt(int64_t v) {
    auto x = std::make_shared<LV>();
    x->k = LV::Int;
    x->i = v;
    return x;
}
LVp LStr(const std::string& v) {
    auto x = std::make_shared<LV>();
    x->k = LV::Str;
    x->s = v;
    return x;
}
LVp LBool(bool v) {
    auto x = std::make_shared<LV>();
    x->k = LV::Bool;
    x->b = v;
    return x;
}
LVp LList(std::vector<LVp> v = {}) {
    auto x = std::make_shared<LV>();
    x->k = LV::List;
    x->list = std::move(v);
    return x;
}
LVp LDict() {
    auto x = std::make_shared<LV>();
    x->k = LV::Dict;
    return x;
}
void Put(const LVp& d, const LVp& k, const LVp& v) {
    for (auto& kv : d->dict)
        if (kv.first->k == k->k && kv.first->i == k->i && kv.first->s == k->s) {
            kv.second = v;
            return;
        }
    d->dict.push_back({k, v});
}
void Put(const LVp& d, const std::string& k, const LVp& v) { Put(d, LStr(k), v); }

bool IsIdent(const std::string& k) {
    if (k.empty() || !(std::isalpha(static_cast<unsigned char>(k[0])) || k[0] == '_')) return false;
    for (unsigned char ch : k)
        if (!(std::isalnum(ch) || ch == '_') || ch >= 128) return false;
    return true;
}
std::string Lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::string LuaLiteral(const LVp& v, int ind = 0) {
    const std::string pad(static_cast<size_t>(2 * (ind + 1)), ' ');
    switch (v->k) {
        case LV::Bool: return v->b ? "true" : "false";
        case LV::Int: return (-0x10000 < v->i && v->i < 0x10000) || v->i < 0 ? IntLit(v->i) : HexU(v->i);
        case LV::Float: return FloatLiteral(v->f);
        case LV::Str: return LuaStr(v->s);
        case LV::Nil: return "nil";
        case LV::List: {
            std::vector<std::string> items;
            for (auto& x : v->list) items.push_back(LuaLiteral(x, ind + 1));
            const std::string one = "{" + JoinStr(items, ", ") + "}";
            if (one.size() <= 100 && one.find('\n') == std::string::npos) return one;
            std::string o = "{\n";
            for (auto& x : items) o += pad + x + ",\n";
            return o + std::string(static_cast<size_t>(2 * ind), ' ') + "}";
        }
        case LV::Dict: {
            std::vector<std::pair<LVp, LVp>> keys = v->dict;
            std::stable_sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) {
                const bool ai = a.first->k == LV::Int, bi = b.first->k == LV::Int;
                if (ai != bi) return ai;
                if (ai) return a.first->i < b.first->i;
                const std::string la = Lower(a.first->s), lb = Lower(b.first->s);
                if (la != lb) return la < lb;
                return a.first->s < b.first->s;
            });
            if (keys.empty()) return "{}";
            std::string o = "{\n";
            for (auto& [k, x] : keys) {
                std::string kt;
                if (k->k == LV::Str && IsIdent(k->s) && !LuaKeywords().count(k->s)) kt = k->s;
                else if (k->k == LV::Int) kt = "[" + (k->i >= 0x100 ? HexU(k->i) : std::to_string(k->i)) + "]";
                else kt = "[" + LuaStr(k->s) + "]";
                o += pad + kt + " = " + LuaLiteral(x, ind + 1) + ",\n";
            }
            return o + std::string(static_cast<size_t>(2 * ind), ' ') + "}";
        }
    }
    return "nil";
}

std::string BundleText(const std::string& level, const std::string& name, const std::string& sha,
                       const std::string& mission, const std::string& refs, const std::string& runtime,
                       const std::string& events, const std::string& main) {
    auto body = [](const std::string& text) {
        std::vector<std::string> keep;
        for (const std::string& l : SplitLines(text))
            if (!StartsWith(l, "--@")) keep.push_back(l);
        return JoinStr(keep, "\n");
    };
    std::vector<std::string> parts = {
        "--@archive " + level,
        Fmt("-- %s as readable Lua (mission2lua_readable %d), SHA-256 %s. Built from the mod folder: mission.lua,",
            name.c_str(), VERSION, sha.c_str()),
        "-- refs.lua, scripts/runtime.lua, scripts/events.lua, scripts/start.lua. Game code: never commit or distribute.",
        "local MISSION = (function()",
        body(mission),
        "end)()",
        "local REFS = (function()",
        body(refs),
        "end)()",
        body(runtime),
        "-- ======================== scripts/events.lua ========================",
        body(events),
        "-- ======================== scripts/start.lua ========================",
        body(main),
        ""};
    return JoinStr(parts, "\n");
}

std::vector<E> TopCalls(E e) {   // _top_calls
    std::vector<E> out;
    if (!e) return out;
    if (e->t == s::call) return {e};
    for (E x : Kids(e)) {
        std::vector<E> y = TopCalls(x);
        out.insert(out.end(), y.begin(), y.end());
    }
    return out;
}

const std::set<std::string>& ObjectWords() {
    static const std::set<std::string> w = {"victim",  "killer",  "soldier", "item",     "unit",     "object",
                                            "vehicle", "door",    "enemy",   "observer", "target",   "attacker",
                                            "giver",   "receiver", "launcher", "weapon", "shooter"};
    return w;
}

// insertion-ordered small map
template <class K, class Vv>
struct OMap {
    std::vector<K> keys;
    std::map<K, Vv> m;
    Vv& operator[](const K& k) {
        auto it = m.find(k);
        if (it == m.end()) {
            keys.push_back(k);
            return m[k];
        }
        return it->second;
    }
    bool has(const K& k) const { return m.count(k) != 0; }
    const Vv* get(const K& k) const {
        auto it = m.find(k);
        return it == m.end() ? nullptr : &it->second;
    }
};

// ---- Generator -------------------------------------------------------------------------------------------------------
struct GVar {
    std::string name;
    int64_t size = 0;
    Sym kind = nullptr;
    bool initFloat = false;
    double initF = 0;
    int64_t initI = 0;
    bool mixed = false, memory = false;
};

class Generator {
public:
    Translator tr;
    Program* prog = nullptr;
    std::map<int64_t, std::map<int64_t, std::string>> refs;   // 0 objects, 1 waypoints, 2 regions, 3 markers
    std::map<uint32_t, Body> ast;
    std::map<uint32_t, std::vector<std::pair<std::string, bool>>> vars;
    std::map<uint32_t, std::vector<std::string>> params;
    std::set<uint32_t> returns;
    std::set<int64_t> slotFloat, slotUsed;
    std::map<int64_t, std::string> strings;
    std::set<uint32_t> takenData;
    std::vector<std::pair<int64_t, int64_t>> memoryAbs;
    std::map<uint32_t, GVar> gvars;
    std::map<uint32_t, std::string> fnames;
    std::map<uint32_t, std::set<int64_t>> labelsOf;
    std::map<uint32_t, std::vector<Sym>> flagsOf;
    std::map<std::string, std::map<std::string, int64_t>> refUsed;
    std::string sceneName;
    bool haveScene = false;
    std::vector<std::pair<uint32_t, std::optional<int64_t>>> objectiveTitles;
    std::map<uint32_t, std::unique_ptr<FnOpt>> opt;
    OMap<uint32_t, std::string> globalHints;
    std::optional<std::pair<int64_t, int64_t>> saveRange;
    std::map<uint32_t, std::set<std::pair<int64_t, Sym>>> gacc;

    bool Init(const std::vector<uint8_t>& dll, const std::string& name, std::string* error) {
        if (!tr.Load(dll, name, error)) return false;
        prog = &tr.prog;
        Optimise();
        return true;
    }
    std::map<Sym, E> EntryValuesOf(const Func& f) {   // _entry_values
        std::map<Sym, E> ev = tr.EntryValues(f);
        if (f.kind == kd::setup) ev[SL(4)] = T(s::api);
        return ev;
    }
    void Optimise();
    bool FindScene(std::string* out);
    void CollectGlobals();
    void NoteGaddr(uint32_t a, bool data);
    void MemoryRanges();
    bool InMemory(int64_t a) const {
        for (auto& [lo, hi] : memoryAbs)
            if (lo <= a && a < hi) return true;
        return false;
    }
    void NameGlobals();
    void HintGlobals();
    void NameFunctions();
    std::string QueueName(uint32_t va, bool* have);
    void Special(uint32_t va);
    void RecoverVars(uint32_t va);
    const std::vector<std::string>& ParamNames(uint32_t va);
    std::string VarName(FnOpt& o, const std::vector<DefId>& ds);
    void NamePhaseFunctions();
    std::string SlotName(int64_t slot) const {
        const SlotInfo* si = tr.Slot(slot);
        return si ? si->name : std::string();
    }
    std::string MsgName(int64_t mid) const {
        auto it = Messages().find(mid);
        if (it != Messages().end()) return it->second.name;
        return Fmt("msg_%02llx", static_cast<long long>(mid));
    }
};

void Generator::Optimise() {
    for (uint32_t va : prog->funcOrder) {
        Func* f = tr.F(va);
        auto o = std::make_unique<FnOpt>(f, &tr.ir[va], true, EntryValuesOf(*f));
        o->Run();
        opt[va] = std::move(o);
    }
    std::set<uint32_t> ret;
    for (uint32_t va : prog->funcOrder) {
        Sym k = tr.F(va)->kind;
        if (k == kd::message || k == kd::save || k == kd::load || k == kd::size || k == kd::setup || k == kd::queue)
            ret.insert(va);
    }
    std::set<uint32_t> usedSites;
    OMap<uint32_t, std::set<uint32_t>> tails;
    for (uint32_t va : prog->funcOrder) {
        FnOpt& o = *opt[va];
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b]) {
                E top = (st->kind == kd::expr || st->kind == kd::tail) && st->e ? StripT(st->e) : nullptr;
                for (E e : StmtExprs(*st))
                    for (E c : FindKind(StripT(e), s::call)) {
                        if (CallTarget(c)->t != s::fn) continue;
                        const uint32_t callee = static_cast<uint32_t>(CallTarget(c)->I(1));
                        if (st->kind == kd::tail && c == top) tails[callee].insert(va);
                        else if (!(st->kind == kd::expr && c == top)) usedSites.insert(callee);
                    }
            }
    }
    ret.insert(usedSites.begin(), usedSites.end());
    bool changed = true;
    while (changed) {
        changed = false;
        for (uint32_t callee : tails.keys) {
            if (ret.count(callee)) continue;
            bool any = false;
            for (uint32_t c : tails.m[callee]) any |= ret.count(c) != 0;
            if (any) {
                ret.insert(callee);
                changed = true;
            }
        }
    }
    returns = ret;
    for (uint32_t va : prog->funcOrder) {
        if (returns.count(va)) continue;
        FnOpt& o = *opt[va];
        for (uint32_t bva : o.ir->order) {
            StmtList& b = o.ir->m[bva];
            const StmtList snapshot = b;
            for (Stmt* st : snapshot) {
                if (st->kind == kd::ret && st->e) {
                    if (HasCall(st->e)) {
                        Stmt ex;
                        ex.kind = kd::expr;
                        ex.at = st->at;
                        ex.e = st->e;
                        auto pos = std::find(b.begin(), b.end(), st);
                        b.insert(pos, tr.arena.New(ex));
                    }
                    st->e = nullptr;
                }
            }
        }
        o.Run();
    }
}

bool Generator::FindScene(std::string* out) {
    for (uint32_t va : prog->funcOrder) {
        FnOpt& o = *opt[va];
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b])
                for (E e : StmtExprs(*st))
                    for (E c : FindKind(StripT(e), s::call)) {
                        E t = CallTarget(c);
                        if (t->t == s::slot && t->I(1) == 0x44 && !CallArgs(c).empty() &&
                            StripT(CallArgs(c)[0].e)->t == s::gaddr)
                            return prog->Cstr(static_cast<uint32_t>(StripT(CallArgs(c)[0].e)->I(1)), out);
                    }
    }
    return false;
}

void Generator::CollectGlobals() {
    gacc.clear();
    for (uint32_t va : prog->funcOrder) {
        FnOpt& o = *opt[va];
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b]) {
                for (E e : StmtExprs(*st))
                    for (E g : FindKind(e, s::g)) gacc[static_cast<uint32_t>(g->I(1))].insert({g->I(2), g->S(3)});
                if (st->kind == kd::store && st->target->t == s::g)
                    gacc[static_cast<uint32_t>(st->target->I(1))].insert(
                        {st->target->I(2), st->extra ? st->extra : (st->target->I(2) < 4 ? s::u : s::i)});
            }
    }
    std::set<uint32_t> asValue, asData;
    std::function<void(E, bool)> walk = [&](E e, bool top) {
        if (!e) return;
        if (e->t == s::t) {
            walk(e->X(2), top);
            return;
        }
        if (e->t == s::gaddr) {
            (top ? asValue : asData).insert(static_cast<uint32_t>(e->I(1)));
            return;
        }
        if (e->t == s::call) {
            walk(CallTarget(e), false);
            for (const V& a : CallArgs(e)) walk(a.e, true);
            return;
        }
        for (E x : Kids(e)) walk(x, false);
    };
    for (uint32_t va : prog->funcOrder) {
        FnOpt& o = *opt[va];
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b]) {
                const std::vector<E> exprs = StmtExprs(*st);
                for (size_t k = 0; k < exprs.size(); ++k)
                    walk(exprs[k], (st->kind == kd::set || st->kind == kd::store) && k == 0 && exprs[k] == st->e);
            }
    }
    std::set<uint32_t> all = asValue;
    all.insert(asData.begin(), asData.end());
    for (uint32_t a : all) NoteGaddr(a, asData.count(a) != 0);
    saveRange.reset();
    for (uint32_t va : prog->funcOrder) {
        FnOpt& o = *opt[va];
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b]) {
                if (st->kind != kd::copy) continue;
                E d = StripT(st->ce[0]), sv = StripT(st->ce[1]), n = StripT(st->ce[2]);
                E g = d->t == s::gaddr ? d : sv->t == s::gaddr ? sv : nullptr;
                if (g && IsC(n)) {
                    saveRange = std::make_pair(g->I(1), g->I(1) + n->I(1));
                    takenData.erase(static_cast<uint32_t>(g->I(1)));
                }
            }
    }
}

void Generator::NoteGaddr(uint32_t a, bool data) {
    Program& p = *prog;
    if (p.p.dll.InText(a) || !p.InImage(a)) return;
    if (data) {
        takenData.insert(a);
        return;
    }
    std::string sv;
    const bool haveS = p.Cstr(a, &sv);
    uint8_t prev = 0;
    const bool havePrev = p.U8(a - 1, &prev);
    if (haveS && !sv.empty() && (p.InRdata(a) || p.InData(a)) && !gacc.count(a) &&
        ((havePrev && prev == 0) || (a % 4 == 0 && havePrev && !(32 <= prev && prev < 127)))) {
        strings[static_cast<int64_t>(a) - p.base] = sv;
        return;
    }
    takenData.insert(a);
}

void Generator::MemoryRanges() {
    Program& p = *prog;
    auto secHiOf = [&](int64_t a, int64_t dflt) {
        for (auto& sc : p.sections)
            if (sc.lo <= a && a < sc.hi) return static_cast<int64_t>(sc.hi);
        return dflt;
    };
    std::vector<std::pair<int64_t, int64_t>> out;
    for (uint32_t a : takenData) {
        const int64_t secHi = secHiOf(a, static_cast<int64_t>(a) + 4);
        int64_t hi = std::min<int64_t>(secHi, static_cast<int64_t>(a) + 0x100);
        auto nx = takenData.upper_bound(a);
        if (nx != takenData.end()) hi = std::min<int64_t>(hi, *nx);
        out.push_back({a, std::max<int64_t>(hi, static_cast<int64_t>(a) + 4)});
    }
    for (uint32_t va : prog->funcOrder) {
        FnOpt& o = *opt[va];
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b]) {
                if (st->kind != kd::cond) continue;
                for (E c : FindKind(StripT(st->e), s::cmp)) {
                    E r = StripT(c->X(3));
                    if (r->t == s::gaddr)
                        for (auto& rg : out)
                            if (rg.first <= r->I(1) && r->I(1) <= rg.first + 0x1000) rg.second = std::max(rg.second, r->I(1));
                }
            }
    }
    for (auto& rg : out) {
        int64_t secLo = rg.first;
        for (auto& sc : p.sections)
            if (sc.lo <= rg.first && rg.first < sc.hi) {
                secLo = sc.lo;
                break;
            }
        rg.first = std::max(secLo, rg.first - 16);
    }
    auto merge = [](std::vector<std::pair<int64_t, int64_t>> rs) {
        std::sort(rs.begin(), rs.end());
        std::vector<std::pair<int64_t, int64_t>> m;
        for (auto& [lo, hi] : rs) {
            if (!m.empty() && lo <= m.back().second) m.back().second = std::max(m.back().second, hi);
            else m.push_back({lo, hi});
        }
        return m;
    };
    std::vector<std::pair<int64_t, int64_t>> merged = merge(out);
    const std::set<uint32_t>& relocs = p.p.dll.relocs;
    for (int round = 0; round < 16; ++round) {
        std::vector<std::pair<int64_t, int64_t>> added;
        for (auto& [lo, hi] : merged)
            for (uint32_t r : relocs) {
                if (!(lo <= r && r < hi && lo <= static_cast<int64_t>(r) + 4 && static_cast<int64_t>(r) + 4 <= hi)) continue;
                uint32_t t;
                if (!p.U32(r, &t) || !p.InImage(t) || p.p.dll.InText(t)) continue;
                bool inside = false;
                for (auto& [a, b] : merged) inside |= a <= t && t < b;
                if (inside) continue;
                const int64_t secHi = secHiOf(t, static_cast<int64_t>(t) + 4);
                int64_t h = std::min<int64_t>(secHi, static_cast<int64_t>(t) + 0x100);
                auto nx = takenData.upper_bound(t);
                if (nx != takenData.end()) h = std::min<int64_t>(h, *nx);
                added.push_back({t, std::max<int64_t>(h, static_cast<int64_t>(t) + 4)});
            }
        if (added.empty()) break;
        std::vector<std::pair<int64_t, int64_t>> all = merged;
        all.insert(all.end(), added.begin(), added.end());
        merged = merge(all);
    }
    merged.push_back({p.obj, static_cast<int64_t>(p.obj) + 24});
    memoryAbs = merged;
}

void Generator::NameGlobals() {
    Program& p = *prog;
    std::set<std::string> used;
    for (auto& [a, sizes] : gacc) {
        int64_t size = 0;
        std::set<Sym> kinds;
        for (auto& [sz, k] : sizes) {
            size = std::max(size, sz);
            kinds.insert(k);
        }
        Sym kind = kinds.count(s::f) ? s::f : (size < 4 ? s::u : s::i);
        const int64_t rva = static_cast<int64_t>(a) - p.base;
        std::string nm;
        if (const std::string* h = globalHints.get(a); h && !h->empty()) nm = *h;
        else nm = (size == 1 ? "b" : size == 2 ? "w" : kind == s::f ? "f" : "g") + std::string("_") + Hex(rva);
        const std::string base = nm;
        int k = 2;
        while (used.count(nm) || Reserved().count(nm)) nm = base + "_" + std::to_string(k++);
        used.insert(nm);
        const std::string raw = p.BytesAt(a, static_cast<size_t>(size));
        GVar gv;
        gv.name = nm;
        gv.size = size;
        gv.kind = kind;
        uint64_t v = 0;
        for (size_t q = 0; q < raw.size() && q < 8; ++q) v |= static_cast<uint64_t>(static_cast<uint8_t>(raw[q])) << (8 * q);
        if (kind == s::f && size == 4) {
            gv.initFloat = true;
            gv.initF = BitsToFloat(static_cast<int64_t>(v));
        } else if (size == 4) {
            gv.initI = S32(static_cast<int64_t>(v));
        } else {
            gv.initI = static_cast<int64_t>(v);
        }
        gv.mixed = kinds.size() > 1;
        gv.memory = InMemory(a);
        gvars[a] = gv;
    }
}

void Generator::HintGlobals() {
    const uint32_t upd = prog->cbs.size() > 1 ? prog->cbs[1] : 0;
    auto it = opt.find(upd);
    if (it != opt.end()) {
        FnOpt& o = *it->second;
        OMap<uint32_t, std::set<int64_t>> cnt;
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b]) {
                if (st->kind != kd::cond) continue;
                for (E c : FindKind(StripT(st->e), s::cmp)) {
                    E l = c->X(2), r = c->X(3);
                    if (l->t == s::g && IsC(r)) cnt[static_cast<uint32_t>(l->I(1))].insert(r->I(1));
                }
            }
        std::vector<uint32_t> keys = cnt.keys;
        std::stable_sort(keys.begin(), keys.end(), [&](uint32_t a, uint32_t b) { return cnt.m[a].size() > cnt.m[b].size(); });
        for (uint32_t a : keys)
            if (cnt.m[a].size() >= 2) {
                globalHints[a] = "mission_phase";
                break;
            }
    }
    int n = 0;
    std::vector<uint32_t> fvas;
    for (auto& [va, o] : opt) fvas.push_back(va);   // sorted
    for (uint32_t va : fvas) {
        FnOpt& o = *opt[va];
        std::vector<uint32_t> bs(o.ir->order.begin(), o.ir->order.end());
        std::sort(bs.begin(), bs.end());
        for (uint32_t b : bs) {
            StmtList& stmts = o.ir->m[b];
            for (size_t k = 0; k < stmts.size(); ++k) {
                Stmt* st = stmts[k];
                if (st->kind != kd::store || st->target->t != s::g ||
                    globalHints.has(static_cast<uint32_t>(st->target->I(1))))
                    continue;
                E e = StripT(st->e);
                std::vector<Sym> locs;
                for (Sym l : LocsIn(e)) locs.push_back(l);
                std::sort(locs.begin(), locs.end(), [](Sym a, Sym b) { return *a < *b; });
                for (Sym l : locs)
                    for (size_t j = k; j-- > 0;)
                        if (stmts[j]->kind == kd::set && stmts[j]->loc == l) {
                            e = T(s::pair, e, StripT(stmts[j]->e));
                            break;
                        }
                std::vector<E> calls = FindKind(e, s::call);
                if (calls.empty() || CallTarget(calls[0])->t != s::slot) continue;
                const std::string nm = SlotName(CallTarget(calls[0])->I(1));
                if (nm == "add_objective") {
                    ++n;
                    globalHints[static_cast<uint32_t>(st->target->I(1))] = "objective_" + std::to_string(n);
                    E a0 = CallArgs(calls[0]).empty() ? nullptr : StripT(CallArgs(calls[0])[0].e);
                    std::optional<int64_t> title;
                    if (a0 && IsC(a0)) title = a0->I(1) & M32;
                    objectiveTitles.push_back({static_cast<uint32_t>(st->target->I(1)), title});
                } else if (!nm.empty() && StartsWith(nm, "get_") && va == prog->setup) {
                    globalHints[static_cast<uint32_t>(st->target->I(1))] = nm.substr(4);
                }
            }
        }
    }
}

std::string Generator::QueueName(uint32_t va, bool* have) {
    FnOpt& o = *opt[va];
    std::vector<int64_t> slots;
    for (uint32_t b : o.ir->order)
        for (Stmt* st : o.ir->m[b])
            for (E e : StmtExprs(*st))
                for (E c : FindKind(StripT(e), s::call))
                    if (CallTarget(c)->t == s::slot) slots.push_back(CallTarget(c)->I(1));
    std::vector<int64_t> uniq;
    std::set<int64_t> seen;
    for (int64_t x : slots)
        if (seen.insert(x).second) uniq.push_back(x);
    if (uniq.empty()) {
        *have = false;
        return {};
    }
    *have = true;
    std::string nm = SlotName(uniq.back());
    if (nm.empty()) nm = Fmt("slot_%03llX", static_cast<long long>(uniq.back()));
    return "cb_" + nm;
}

void Generator::NameFunctions() {
    Program& p = *prog;
    std::map<Sym, std::string> fixed = {{kd::update, "mission_update"}, {kd::message, "mission_message"},
                                        {kd::save, "save_block"},       {kd::load, "load_block"},
                                        {kd::size, "save_block_size"},  {kd::init, "mission_init"},
                                        {kd::setup, "level_setup"}};
    std::map<uint32_t, std::set<uint32_t>> fromMsg;
    std::map<uint32_t, std::set<int64_t>> caseOf;
    const uint32_t msgfn = p.cbs.size() > 2 ? p.cbs[2] : 0;
    auto it = opt.find(msgfn);
    if (it != opt.end()) {
        FnOpt& o = *it->second;
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b])
                for (E e : StmtExprs(*st))
                    for (E c : FindKind(StripT(e), s::call))
                        if (CallTarget(c)->t == s::fn) fromMsg[static_cast<uint32_t>(CallTarget(c)->I(1))].insert(b);
        for (uint32_t b : o.ir->order) {
            StmtList& sl = o.ir->m[b];
            if (!sl.empty() && sl.back()->kind == kd::sw)
                for (auto& [val, t] : sl.back()->table->values) caseOf[t].insert(val);
        }
    }
    std::set<std::string> used = Reserved();
    for (auto& [va, f] : p.funcs) {
        std::string nm;
        auto fx = fixed.find(f->kind);
        if (fx != fixed.end()) nm = fx->second;
        if (nm.empty() && f->kind == kd::queue) {
            bool have;
            nm = QueueName(va, &have);
        }
        if (nm.empty() && fromMsg.count(va)) {
            std::set<int64_t> ids;
            for (uint32_t b : fromMsg[va]) {
                auto c = caseOf.find(b);
                if (c != caseOf.end()) ids.insert(c->second.begin(), c->second.end());
            }
            if (ids.size() == 1) nm = "on_" + MsgName(*ids.begin());
        }
        if (nm.empty()) nm = "f_" + Hex(static_cast<int64_t>(va) - p.base);
        const std::string base = nm;
        int k = 2;
        while (used.count(nm)) nm = base + "_" + std::to_string(k++);
        used.insert(nm);
        fnames[va] = nm;
    }
}

void Generator::Special(uint32_t va) {
    Func* f = tr.F(va);
    FnOpt& o = *opt[va];
    auto mapStmt = [](Stmt* st, const std::function<E(E)>& fn) {   // _map_stmt
        if (st->kind == kd::copy) {
            for (E& x : st->ce) x = MapExpr(x, fn);
            return;
        }
        if (st->e) st->e = MapExpr(st->e, fn);
        if (st->kind == kd::store && st->target->t == s::mem)
            st->target = T(s::mem, MapExpr(st->target->X(1), fn), st->target->I(2));
    };
    if (f->kind == kd::message) {
        std::function<E(E)> m = [](E e) -> E {
            if (e->t == s::mem && e->I(2) == 4) {
                E a = StripT(e->X(1));
                if (a == T(s::param, 1ll)) return T(s::msg, 0ll, e->S(3));
                if (a->t == s::bin && a->S(1) == s::plus && a->X(2) == T(s::param, 1ll) && IsC(a->X(3)) &&
                    a->X(3)->I(1) % 4 == 0)
                    return T(s::msg, a->X(3)->I(1) / 4, e->S(3));
            }
            return nullptr;
        };
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b]) {
                mapStmt(st, m);
                if (st->kind == kd::store && st->target->t == s::mem) {
                    E t = m(T(s::mem, st->target->X(1), st->target->I(2), s::i));
                    if (t) st->target = T(s::msg, t->I(1));
                }
            }
    }
    if (f->kind == kd::queue) {
        bool ok = true;
        std::set<int64_t> fields;
        std::function<void(E)> chk = [&](E e) {
            if (!e) return;
            if (e->t == s::mem) {
                E a = StripT(e->X(1));
                if (a->t == s::bin && a->S(1) == s::plus && a->X(2) == T(s::param, 0ll) && IsC(a->X(3)) &&
                    (a->X(3)->I(1) == 0x34 || a->X(3)->I(1) == 0x38)) {
                    fields.insert(a->X(3)->I(1));
                    return;
                }
            }
            if (e == T(s::param, 0ll)) {
                ok = false;
                return;
            }
            for (E x : Kids(e)) chk(x && x->t == s::t ? StripT(x) : x);
        };
        for (uint32_t b : o.ir->order)
            for (Stmt* st : o.ir->m[b])
                for (E e : StmtExprs(*st)) chk(StripT(e));
        f->eventForm = ok && !fields.empty();
        if (f->eventForm) {
            std::function<E(E)> m2 = [](E e) -> E {
                if (e->t == s::mem && e->I(2) == 4) {
                    E a = StripT(e->X(1));
                    if (a->t == s::bin && a->S(1) == s::plus && a->X(2) == T(s::param, 0ll) && IsC(a->X(3)) &&
                        (a->X(3)->I(1) == 0x34 || a->X(3)->I(1) == 0x38))
                        return T(s::evt, a->X(3)->I(1));
                }
                return nullptr;
            };
            for (uint32_t b : o.ir->order)
                for (Stmt* st : o.ir->m[b]) mapStmt(st, m2);
        }
    }
    if (f->kind == kd::setup) {
        for (uint32_t b : o.ir->order) {
            StmtList& sl = o.ir->m[b];
            StmtList keep;
            for (Stmt* st : sl)
                if (!(st->kind == kd::store && st->target->t == s::apiglobal)) keep.push_back(st);
            sl = keep;
        }
    }
}

const std::vector<std::string>& Generator::ParamNames(uint32_t va) {
    auto it = params.find(va);
    if (it != params.end()) return it->second;
    Func* f = tr.F(va);
    const int n = f->nparams;
    std::vector<std::string> names;
    bool set = true;
    if (f->kind == kd::message) names = {"id", "msg"};
    else if (f->kind == kd::save || f->kind == kd::load) names = {"buf"};
    else if (f->kind == kd::queue && f->eventForm) names = {};
    else if (f->kind == kd::queue) names = {"event"};
    else if (f->kind == kd::setup) names = {"api_table"};
    else set = false;
    if (!set) {
        names.clear();
        auto fn = fnames.find(va);
        const std::string nm = fn == fnames.end() ? std::string() : fn->second;
        if (StartsWith(nm, "on_")) {
            const size_t cut = nm.find("_2");
            const std::string head = cut == std::string::npos ? nm : nm.substr(0, cut);
            for (auto& [k, mi] : Messages()) {
                if ("on_" + mi.name == head || "on_" + mi.name == nm) {
                    for (const std::string& x : mi.params) names.push_back(ParamWord(x));
                    break;
                }
            }
        }
        if (static_cast<int>(names.size()) > n) names.resize(static_cast<size_t>(std::max(n, 0)));
        while (static_cast<int>(names.size()) < n) names.push_back("arg" + std::to_string(names.size() + 1));
        std::set<std::string> seen;
        for (size_t k = 0; k < names.size(); ++k) {
            if (seen.count(names[k]) || Reserved().count(names[k])) names[k] = "arg" + std::to_string(k + 1);
            seen.insert(names[k]);
        }
    }
    return params[va] = names;
}

std::string Generator::VarName(FnOpt& o, const std::vector<DefId>& ds) {
    for (const DefId& d : ds) {
        if (d.in) continue;
        E e = StripT((*o.ir)[d.va][d.k]->e);
        if (!e) continue;
        if (e->t == s::call && CallTarget(e)->t == s::slot) {
            std::string nm = SlotName(CallTarget(e)->I(1));
            if (!nm.empty()) {
                for (const char* pre : {"get_", "find_", "is_", "has_", "count_"})
                    if (StartsWith(nm, pre)) {
                        if (std::strcmp(pre, "get_") == 0 || std::strcmp(pre, "find_") == 0) nm = nm.substr(std::strlen(pre));
                        break;
                    }
                return Ident(nm);
            }
            return "result";
        }
        if (e->t == s::call) return "result";
        if (e->t == s::g) {
            auto g = gvars.find(static_cast<uint32_t>(e->I(1)));
            return "cur_" + (g != gvars.end() ? g->second.name : std::string("value"));
        }
        if (e->t == s::gaddr) return "p";
        if (e->t == s::c || (e->t == s::bin && (e->S(1) == s::plus || e->S(1) == s::minus))) continue;
        if (e->t == s::fr || e->t == s::fc || e->t == s::i2f) return "x";
    }
    for (const DefId& d : ds)
        if (!d.in) {
            E e = StripT((*o.ir)[d.va][d.k]->e);
            if (e->t == s::c || e->t == s::bin) return "i";
        }
    return "v";
}

void Generator::RecoverVars(uint32_t va) {
    FnOpt& o = *opt[va];
    auto ins = o.Rd();
    std::vector<DefId> porder;
    std::map<DefId, DefId> parent;
    auto setdef = [&](const DefId& d) {
        if (!parent.count(d)) {
            parent[d] = d;
            porder.push_back(d);
        }
    };
    std::function<DefId(DefId)> find = [&](DefId x) {
        for (;;) {
            auto it = parent.find(x);
            const DefId px = it == parent.end() ? x : it->second;
            if (px == x) return x;
            auto it2 = parent.find(px);
            parent[x] = it2 == parent.end() ? px : it2->second;
            x = parent[x];
        }
    };
    auto unite = [&](const DefId& a, const DefId& b) {
        const DefId ra = find(a), rb = find(b);
        if (!(ra == rb)) parent[ra] = rb;
    };
    struct UK {
        uint32_t b;
        int k;
        Sym l;
        bool operator<(const UK& o) const { return std::tie(b, k, l) < std::tie(o.b, o.k, o.l); }
    };
    std::map<UK, std::vector<DefId>> useDefs;
    for (uint32_t b : o.order) {
        RDState st = ins.count(b) ? ins[b] : RDState{};
        StmtList& sl = (*o.ir)[b];
        for (size_t k = 0; k < sl.size(); ++k) {
            std::vector<Sym> uses;
            for (Sym l : StmtUses(*sl[k])) uses.push_back(l);
            std::sort(uses.begin(), uses.end(), [](Sym a, Sym c) { return *a < *c; });
            for (Sym l : uses) {
                auto it = st.find(l);
                DefSet dset = it != st.end() ? it->second : DefSet{InDef(l)};
                std::vector<DefId> ds(dset.begin(), dset.end());
                std::stable_sort(ds.begin(), ds.end(), [](const DefId& a, const DefId& c) { return a.PyStr() < c.PyStr(); });
                useDefs[{b, static_cast<int>(k), l}] = ds;
                for (const DefId& d : ds) setdef(d);
                for (size_t q = 1; q < ds.size(); ++q) unite(ds[0], ds[q]);
            }
            if (sl[k]->kind == kd::set) {
                const DefId d = AtDef(b, static_cast<int>(k));
                setdef(d);
                st[sl[k]->loc] = DefSet{d};
            }
        }
    }
    auto websOf = [&]() {
        OMap<DefId, std::vector<DefId>> w;
        for (const DefId& d : porder) w[find(d)].push_back(d);
        return w;
    };
    OMap<DefId, std::vector<DefId>> webs = websOf();
    if (webs.keys.size() > 150) {
        auto locOf = [&](const DefId& d) -> Sym { return d.in ? d.loc : (*o.ir)[d.va][d.k]->loc; };
        std::map<Sym, DefId> byLoc;
        for (const DefId& d : std::vector<DefId>(porder)) {
            Sym l = locOf(d);
            auto it = byLoc.find(l);
            if (it != byLoc.end()) unite(it->second, d);
            else byLoc[l] = d;
        }
        webs = websOf();
        tr.stats["functions_with_merged_locals"] += 1;
    }
    std::set<std::string> used = Reserved();
    for (auto& [v, n] : fnames) used.insert(n);
    for (auto& [a, g] : gvars) used.insert(g.name);
    const std::vector<std::string> pnames = ParamNames(va);
    used.insert(pnames.begin(), pnames.end());
    std::map<DefId, std::string> webName;
    std::vector<std::pair<std::string, bool>> locals;
    std::vector<DefId> roots = webs.keys;
    std::stable_sort(roots.begin(), roots.end(), [](const DefId& a, const DefId& b) { return a.PyStr() < b.PyStr(); });
    int64_t undefined = 0;
    for (const DefId& root : roots) {
        const std::vector<DefId>& ds = webs.m[root];
        const DefId* pk = nullptr;
        for (const DefId& d : ds) {
            if (!d.in) continue;
            const std::string& l = *d.loc;
            if (l.empty() || l[0] != 's' || StartsWith(l, "sm")) continue;
            const int64_t idx = static_cast<int64_t>(std::floor((std::stoll(l.substr(1)) - 4) / 4.0));
            if (idx < static_cast<int64_t>(pnames.size())) {
                pk = &d;
                break;
            }
        }
        if (pk) {
            int64_t idx = static_cast<int64_t>(std::floor((std::stoll(pk->loc->substr(1)) - 4) / 4.0));
            if (idx < 0) idx += static_cast<int64_t>(pnames.size());
            webName[root] = pnames[static_cast<size_t>(idx)];
            continue;
        }
        bool anyIn = false;
        for (const DefId& d : ds) anyIn |= d.in;
        if (anyIn) ++undefined;
        std::string nm = VarName(o, ds);
        const std::string base = nm;
        int k = 2;
        while (used.count(nm)) nm = base + std::to_string(k++);
        used.insert(nm);
        webName[root] = nm;
        locals.push_back({nm, anyIn});
    }
    tr.stats["undefined_reads"] += undefined;
    for (uint32_t b : o.order) {
        StmtList& sl = (*o.ir)[b];
        for (size_t k = 0; k < sl.size(); ++k) {
            std::map<Sym, E> m;
            for (Sym l : StmtUses(*sl[k])) {
                auto it = useDefs.find({b, static_cast<int>(k), l});
                if (it != useDefs.end() && !it->second.empty()) m[l] = T(s::var, Y(webName[find(it->second[0])]));
            }
            if (!m.empty()) SubstStmt(sl[k], m);
            if (sl[k]->kind == kd::set) sl[k]->loc = Y(webName[find(AtDef(b, static_cast<int>(k)))]);
        }
    }
    vars[va] = locals;
}

void Generator::NamePhaseFunctions() {
    bool havePhase = false;
    uint32_t phase = 0;
    for (uint32_t a : globalHints.keys)
        if (globalHints.m[a] == "mission_phase") {
            phase = a;
            havePhase = true;
            break;
        }
    const uint32_t upd = prog->cbs.size() > 1 ? prog->cbs[1] : 0;
    if (!havePhase || !ast.count(upd)) return;
    OMap<uint32_t, int64_t> found;
    auto note = [&](const Body& arm, int64_t v) {
        for (const Ast& x : arm) {
            if (x.k != Ast::Return || !x.cond) continue;
            E e = StripT(x.cond);
            if (e && e->t == s::call && CallTarget(e)->t == s::fn) {
                found[static_cast<uint32_t>(CallTarget(e)->I(1))] = v;
                return;
            }
        }
    };
    auto isPhase = [&](E l) {
        E x = StripT(l);
        return x->t == s::g && x->I(1) == phase;
    };
    std::function<void(const Body&)> walk = [&](const Body& body) {
        for (const Ast& node : body) {
            if (node.k != Ast::If) continue;
            for (auto& [c0, arm] : node.arms) {
                E c = StripT(c0);
                if (c->t == s::cmp && c->S(1) == s::eq && isPhase(c->X(2)) && IsC(c->X(3))) note(arm, c->X(3)->I(1));
                if (c->t == s::cmp && c->S(1) == s::ne && isPhase(c->X(2)) && IsC(c->X(3)) && !node.els.empty() &&
                    c == StripT(node.arms.back().first))
                    note(node.els, c->X(3)->I(1));
                walk(arm);
            }
            if (!node.els.empty()) walk(node.els);
        }
    };
    walk(ast[upd]);
    static const std::map<int64_t, std::string> names = {{0, "intro_update"}, {1, "overview_update"}, {2, "play_update"}};
    std::set<std::string> used;
    for (auto& [v, n] : fnames) used.insert(n);
    for (uint32_t va : found.keys) {
        auto nmIt = names.find(found.m[va]);
        if (nmIt == names.end()) continue;
        const std::string& nm = nmIt->second;
        if (!used.count(nm) && StartsWith(fnames[va], "f_")) {
            used.erase(fnames[va]);
            fnames[va] = nm;
            used.insert(nm);
        }
    }
}

// ---- Printer ---------------------------------------------------------------------------------------------------------
const std::map<std::string, int>& Prec() {
    static const std::map<std::string, int> p = {{"or", 1}, {"and", 2}, {"cmp", 3}, {"|", 4},  {"~", 5},
                                                 {"&", 6},  {"<<", 7},  {">>", 7},  {"+", 9},  {"-", 9},
                                                 {"*", 10}, {"/", 10},  {"//", 10}, {"%", 10}, {"un", 11},
                                                 {"atom", 13}};
    return p;
}
int P(const char* k) { return Prec().at(k); }

class Printer {
public:
    Printer(Generator& g, const std::string& level) : g_(g), tr_(g.tr), p_(*g.prog) {
        level_ = level;
        if (level_.empty()) {
            std::string n = p_.name;
            const size_t dot = n.rfind('.');
            if (dot != std::string::npos && dot > 0) n = n.substr(0, dot);   // os.path.splitext
            level_ = Lower(n);
        }
    }
    void Emit(ReadableParts* out);

private:
    using Hint = std::string;
    std::string Ex(E e, int prec = 0, const Hint& hint = "") {
        auto [sv, p] = Ex2(e, hint);
        return p < prec ? "(" + sv + ")" : sv;
    }
    std::pair<std::string, int> Ex2(E e, const Hint& hint);
    std::string DField(uint32_t addr, int64_t size, Sym kind);
    std::string MaskLit(E c);
    const char* IsRefValue(E a);
    std::optional<std::string> RefName(const std::string& kind, int64_t v);
    std::string BufferExpr(int64_t off);
    std::string Call(E e);
    std::string ApiName(int64_t slot, bool fret);
    std::string Sprintf(const std::vector<E>& args);
    void StmtOut(Stmt* st, int ind);
    std::string StoreValue(E e, int64_t size, Sym extra);
    void Block(const Body& body, int ind, const std::string& loop = "");
    void Switch(const Ast& n, int ind, const std::string& loop);
    std::string Cond(E c) { return Ex(StripT(c)); }
    std::string NewLoop() { return "continue_" + std::to_string(++loopNo_); }
    void W(int ind, const std::string& text) { lines_.push_back(std::string(static_cast<size_t>(2 * ind), ' ') + text); }
    void Function(uint32_t va);
    void Locals(uint32_t va);
    void MessageFunction(uint32_t va, const std::string& nm, const std::vector<std::string>& params);
    void EmitHandlers(uint32_t va);
    std::string EventName(int64_t mid) {
        const std::string nm = g_.MsgName(mid);
        eventsUsed_[nm] = mid;
        return nm;
    }
    std::vector<std::string> LogicHeader();
    void ApiSection();
    void StateSection();
    LVp MissionData();
    LVp RefsData();
    Sym CurKind() { return tr_.F(cur_)->kind; }

    Generator& g_;
    Translator& tr_;
    Program& p_;
    std::string level_;
    uint32_t cur_ = 0;
    std::vector<std::string> lines_;
    int loopNo_ = 0;
    std::set<std::string> usedLoops_;
    std::map<uint32_t, std::vector<std::pair<int64_t, std::pair<std::string, int64_t>>>> buffers_;
    std::map<std::string, int64_t> eventsUsed_;
    std::map<std::string, std::pair<int64_t, std::string>> dfields_;
    std::vector<std::pair<std::vector<int64_t>, Body>> handlers_;
    std::map<std::string, int64_t>& stats() { return tr_.stats; }
};

std::pair<std::string, int> Printer::Ex2(E e, const Hint& hint) {
    Sym k = e->t;
    const int atom = P("atom"), un = P("un");
    if (k == s::t) return Ex2(e->X(2), hint);
    if (k == s::c) {
        const int64_t v = e->I(1);
        if ((hint == "float" || hint == "time_ms") && LooksFloat(v)) return {F32Short(BitsToFloat(v)), atom};
        if (hint == "u32") {
            if (-0x10000 < v && v < 0) return {std::to_string(v), un};
            if (v < 0 || v >= 0x10000) return {HexU(v & M32), atom};
            if (LooksFloat(v)) return {F32Short(BitsToFloat(v)), atom};
            return {std::to_string(v), atom};
        }
        if (StartsWith(hint, "ref:") || StartsWith(hint, "cref:")) {
            const std::string kind = hint.substr(hint.find(':') + 1);
            auto r = RefName(kind, v);
            if (r) return {*r, atom};
            if (StartsWith(hint, "ref:") && (v <= -0x10000 || v >= 0x10000)) return {HexU(v & M32), atom};
        }
        return {IntLit(v), v >= 0 ? atom : un};
    }
    if (k == s::fc) {
        const double v = e->F(1);
        const std::string sh = F32Short(v);
        std::string sv;
        if (std::strtod(sh.c_str(), nullptr) == v || hint == "float" || hint == "u32" || hint == "time_ms") sv = sh;
        else sv = "f32(" + sh + ")";
        return {sv, StartsWith(sv, "-") ? un : atom};
    }
    if (k == s::param) {
        if (e->I(1) == 1 && CurKind() == kd::message) return {"msg.p", atom};
        auto it = g_.params.find(cur_);
        const int64_t i = e->I(1);
        if (it != g_.params.end() && i < static_cast<int64_t>(it->second.size())) return {it->second[static_cast<size_t>(i)], atom};
        return {"arg" + std::to_string(i + 1), atom};
    }
    if (k == s::var) return {*e->S(1), atom};
    if (k == s::loc) {
        stats()["unresolved_locations"] += 1;
        return {"nil --[[" + *e->S(1) + "]]", atom};
    }
    if (k == s::g) {
        auto gv = g_.gvars.find(static_cast<uint32_t>(e->I(1)));
        if (gv == g_.gvars.end()) {
            stats()["raw_memory"] += 1;
            const int64_t sz = e->I(2);
            const char* fn = sz == 1 ? "u8" : sz == 2 ? "u16" : sz == 4 ? (e->S(3) == s::f ? "f32" : "i32") : "f64";
            return {Fmt("mem.%s(ADDR(%s))", fn, HexU(e->I(1) - p_.base).c_str()), atom};
        }
        if (gv->second.memory) {
            stats()["memory_globals"] += 1;
            return {"D." + DField(static_cast<uint32_t>(e->I(1)), e->I(2), e->S(3)), atom};
        }
        const std::string& nm = gv->second.name;
        if (e->S(3) == s::f && gv->second.kind != s::f) return {"asfloat(" + nm + ")", atom};
        if (e->S(3) != s::f && gv->second.kind == s::f) return {"bits(" + nm + ")", atom};
        return {nm, atom};
    }
    if (k == s::gaddr) {
        const int64_t rva = e->I(1) - p_.base;
        auto it = g_.strings.find(rva);
        if (it != g_.strings.end() && hint != "addr") return {LuaStr(it->second), atom};
        return {"ADDR(" + HexU(rva) + ")", atom};
    }
    if (k == s::saddr) return {BufferExpr(e->I(1)), P("+")};
    if (k == s::api) return {"API_TABLE", atom};
    if (k == s::apislotptr) return {"api_slot(" + HexU(e->I(1)) + ")", atom};
    if (k == s::apislotaddr) return {"(API_TABLE + " + HexU(e->I(1)) + ")", atom};
    if (k == s::fnptr) {
        auto it = g_.fnames.find(static_cast<uint32_t>(e->I(1)));
        return {it != g_.fnames.end() ? it->second : "f_" + Hex(e->I(1) - p_.base), atom};
    }
    if (k == s::mem) {
        E a = StripT(e->X(1));
        const int64_t sz = e->I(2);
        const char* fn = sz == 1 ? "u8" : sz == 2 ? "u16" : sz == 4 ? (e->S(3) == s::f ? "f32" : "i32") : "f64";
        if (HasKind(a, s::saddr)) stats()["buffer_access"] += 1;
        else stats()["raw_memory"] += 1;
        return {Fmt("mem.%s(%s)", fn, Ex(a).c_str()), atom};
    }
    if (k == s::msg) {
        if (e->S(2) == s::f) return {"msg:float(" + std::to_string(e->I(1)) + ")", atom};
        return {"msg[" + std::to_string(e->I(1)) + "]", atom};
    }
    if (k == s::evt) return {e->I(1) == 0x34 ? "a" : "b", atom};
    if (k == s::str) return {LuaStr(*e->S(1)), atom};
    if (k == s::undef) {
        stats()["undefined_values"] += 1;
        return {"0 --[[undefined " + *e->S(1) + "]]", atom};
    }
    if (k == s::bin) {
        Sym op = e->S(1);
        E a = e->X(2), b = e->X(3);
        const std::string& o = *op;
        if (op == s::plus || op == s::minus || op == s::mul || op == s::div) {
            const int pr = P(o.c_str());
            const Hint ha = StripT(a)->t == s::gaddr ? "addr" : "", hb = StripT(b)->t == s::gaddr ? "addr" : "";
            return {Ex(a, pr, ha) + " " + o + " " + Ex(b, pr + 1, hb), pr};
        }
        if (op == s::band || op == s::bor || op == s::bxor) {
            const int pr = P(o.c_str());
            const std::string bs = IsC(b) ? MaskLit(b) : Ex(b, pr + 1);
            return {Ex(a, pr) + " " + o + " " + bs, pr};
        }
        if (op == s::shl) return {Ex(a, 8) + " << " + Ex(b, 8), P("<<")};
        if (op == s::shr) {
            int64_t lo, hi;
            if (VRange(a, &lo, &hi) && lo >= 0) return {Ex(a, 8) + " >> " + Ex(b, 8), P(">>")};
            return {"(" + Ex(a, 7) + " & 0xFFFFFFFF) >> " + Ex(b, 8), P(">>")};
        }
        if (op == s::sar) {
            if (IsC(b)) {
                const int64_t sh = b->I(1);
                const std::string d = sh >= 0 && sh < 63 ? std::to_string(1ll << sh) : std::to_string(0);
                return {Ex(a, 10) + " // " + d, P("//")};
            }
            return {Ex(a, 10) + " // (1 << " + Ex(b) + ")", P("//")};
        }
        if (op == s::idiv || op == s::imod || op == s::udiv || op == s::umod || op == s::umulhi)
            return {o + "(" + Ex(a) + ", " + Ex(b) + ")", atom};
        throw Unsupported{"operator " + o};
    }
    if (k == s::un) {
        std::string inner = Ex(e->X(2), un);
        if (StartsWith(inner, "-") || StartsWith(inner, "~")) inner = "(" + inner + ")";
        if (e->S(1) == s::neg_) return {"-" + inner, un};
        return {"~" + inner, un};
    }
    if (k == s::cmp) {
        Sym rel = e->S(1);
        E a = e->X(2), b = e->X(3);
        if (a == C0 && b == C0 && (rel == s::eq || rel == s::ne)) return {rel == s::eq ? "true" : "false", atom};
        const char* rk = IsRefValue(a);
        Hint hint2 = rk ? std::string("cref:") + rk : "";
        if ((rel == s::eq || rel == s::ne) && IsC(b) && StripT(a) == T(s::param, 0ll) && CurKind() == kd::message)
            return {Ex(a, 4) + " " + *rel + " EVENTS." + EventName(b->I(1)), P("cmp")};
        if (rel == s::eq || rel == s::ne || rel == s::lt || rel == s::le || rel == s::gt || rel == s::ge) {
            if (StripT(b)->t == s::gaddr) hint2 = "addr";
            return {Ex(a, 4, StripT(a)->t == s::gaddr ? "addr" : "") + " " + *rel + " " + Ex(b, 4, hint2), P("cmp")};
        }
        if ((*rel)[0] == 'f') return {Ex(a, 4) + " " + rel->substr(1) + " " + Ex(b, 4), P("cmp")};
        if (rel == s::ult || rel == s::ule || rel == s::ugt || rel == s::uge) {
            const char* fn = rel == s::ult ? "ult" : rel == s::ule ? "ule" : rel == s::ugt ? "ugt" : "uge";
            return {std::string(fn) + "(" + Ex(a) + ", " + Ex(b) + ")", atom};
        }
        throw Unsupported{"comparison " + *rel};
    }
    if (k == s::not_) return {"not " + Ex(e->X(1), un), un};
    if (k == s::and_) return {Ex(e->X(1), 2) + " and " + Ex(e->X(2), 3), P("and")};
    if (k == s::or_) return {Ex(e->X(1), 1) + " or " + Ex(e->X(2), 2), P("or")};
    if (k == s::b2i) return {"b2i(" + Ex(e->X(1)) + ")", atom};
    if (k == s::i2f) {
        E x = StripT(e->X(1));
        if (IsC(x)) return {FloatLiteral(static_cast<double>(x->I(1))), atom};
        if (x->t == s::qword) {
            E lo = StripT(x->X(1)), hi = StripT(x->X(2));
            if (hi == C0) return {"float(" + Ex(lo, 6) + " & 0xFFFFFFFF)", atom};
            return {"qword(" + Ex(lo) + ", " + Ex(hi) + ")", atom};
        }
        return {"float(" + Ex(x) + ")", atom};
    }
    if (k == s::u2f) return {"float(" + Ex(e->X(1), 6) + " & 0xFFFFFFFF)", atom};
    if (k == s::fr) return {"fr(" + Ex(e->X(1)) + ")", atom};
    if (k == s::f32) {
        E x = StripT(e->X(1));
        if (x->t == s::fr) x = x->X(1);
        if (x->t == s::fc) return Ex2(x, "");
        return {"f32(" + Ex(x) + ")", atom};
    }
    if (k == s::fromfbits) {
        E x = StripT(e->X(1));
        if (IsFloat(x)) return Ex2(x, "");
        if (IsC(x)) return {F32Short(BitsToFloat(x->I(1))), atom};
        return {"asfloat(" + Ex(x) + ")", atom};
    }
    if (k == s::fistp) return {"ftoi(" + Ex(e->X(1)) + ")", atom};
    if (k == s::sx) return {"sx" + std::to_string(8 * e->I(2)) + "(" + Ex(e->X(1)) + ")", atom};
    if (k == s::ext) {
        E x = e->X(1);
        const int64_t sh = e->I(2), mask = e->I(3);
        if (sh == 0) return {Ex(x, 6) + " & " + HexU(mask), P("&")};
        return {"(" + Ex(x, 8) + " >> " + std::to_string(sh) + ") & " + HexU(mask), P("&")};
    }
    if (k == s::ins) {
        E x = e->X(1), v = e->X(2);
        const int64_t sh = e->I(3), mask = e->I(4);
        const int64_t clear = ~(mask << sh);
        return {"(" + Ex(x, 6) + " & " + MaskLit(C(clear)) + ") | ((" + Ex(v, 6) + " & " + HexU(mask) + ") << " +
                    std::to_string(sh) + ")",
                P("|")};
    }
    if (k == s::dlo || k == s::dhi) return {*k + "(" + Ex(e->X(1)) + ")", atom};
    if (k == s::qword) return {"qword(" + Ex(e->X(1)) + ", " + Ex(e->X(2)) + ")", atom};
    if (k == s::fsw) {
        stats()["unresolved_fpu_status"] += 1;
        return {"0", atom};
    }
    if (k == s::call) return {Call(e), atom};
    if (k == s::pair) {
        std::vector<std::string> parts;
        for (E x : Kids(e)) parts.push_back(Ex(x));
        return {JoinStr(parts, ", "), atom};
    }
    if (k == s::flag) return {*e->S(1), atom};
    throw Unsupported{"expression " + *k};
}

std::string Printer::DField(uint32_t addr, int64_t size, Sym kind) {
    const GVar& gv = g_.gvars.at(addr);
    const std::string fn = size == 1 ? "u8" : size == 2 ? "u16" : (kind == s::f ? "f32" : "i32");
    const std::string key = (fn == "i32" || fn == "u8" || fn == "u16" || gv.kind == s::f) ? gv.name : gv.name + "_f";
    dfields_[key] = {static_cast<int64_t>(addr) - p_.base, fn};
    return key;
}

std::string Printer::MaskLit(E c) {
    const int64_t v = c->I(1);
    if (v < 0 && v != -1) return "~" + HexU(~v);
    if (v >= 0x10000) return HexU(v);
    return v < 0x100 ? std::to_string(v) : HexU(v);
}

const char* Printer::IsRefValue(E a) {
    a = StripT(a);
    if (a->t == s::msg) return nullptr;
    if (a->t == s::param) {
        auto it = g_.params.find(cur_);
        std::string nm;
        if (it != g_.params.end() && a->I(1) < static_cast<int64_t>(it->second.size())) nm = it->second[static_cast<size_t>(a->I(1))];
        if (nm.find("waypoint") != std::string::npos) return "waypoint";
        if (ObjectWords().count(nm)) return "object";
    }
    if (a->t == s::call && CallTarget(a)->t == s::slot) {
        const SlotInfo* si = tr_.Slot(CallTarget(a)->I(1));
        std::string ret = si ? si->returns : std::string();
        const size_t sp = ret.find(' ');
        if (sp != std::string::npos) ret = ret.substr(0, sp);
        if (ret == "object") return "object";
        if (ret == "waypoint") return "waypoint";
    }
    return nullptr;
}

std::optional<std::string> Printer::RefName(const std::string& kind, int64_t v) {
    int table;
    const char* tname;
    const char* alias;
    if (kind == "object") table = 0, tname = "objects", alias = "obj";
    else if (kind == "waypoint") table = 1, tname = "waypoints", alias = "wp";
    else if (kind == "region") table = 2, tname = "regions", alias = "rgn";
    else if (kind == "marker") table = 3, tname = "markers", alias = "mk";
    else throw Unsupported{"ref kind " + kind};
    auto t = g_.refs.find(table);
    if (t == g_.refs.end()) return std::nullopt;
    auto it = t->second.find(v);
    if (it == t->second.end()) return std::nullopt;
    g_.refUsed[tname][it->second] = v;
    stats()["refs_named"] += 1;
    return std::string(alias) + "." + it->second;
}

std::string Printer::BufferExpr(int64_t off) {
    auto it = buffers_.find(cur_);
    if (it != buffers_.end())
        for (auto& [lo, ns] : it->second)
            if (lo <= off && off < lo + ns.second) {
                const int64_t d = off - lo;
                return d == 0 ? ns.first : ns.first + " + " + std::to_string(d);
            }
    stats()["stack_addresses"] += 1;
    return "scratch_at(" + std::to_string(off) + ")";
}

std::string Printer::ApiName(int64_t slot, bool fret) {
    (void)fret;
    const SlotInfo* si = tr_.Slot(slot);
    std::string nm = si ? si->name : std::string();
    if (nm.empty()) nm = Fmt("slot_%03llX", static_cast<long long>(slot));
    return nm;   // (slot_mixed is always empty: never "_f")
}

std::string Printer::Call(E e) {
    E tgt = CallTarget(e);
    std::vector<E> args;
    for (const V& a : CallArgs(e)) args.push_back(StripT(a.e));
    if (tgt->t == s::slot) {
        const int64_t slot = tgt->I(1);
        g_.slotUsed.insert(slot);
        if (CallRet(e) == s::f) g_.slotFloat.insert(slot);
        const SlotInfo* si = tr_.Slot(slot);
        std::vector<std::string> types;
        if (si)
            for (auto& a : si->args) types.push_back(a.type);
        std::vector<std::string> out;
        for (size_t k = 0; k < args.size(); ++k) {
            const std::string t = k < types.size() ? types[k] : std::string();
            const Hint h = t == "float" ? "float"
                           : (t == "object" || t == "waypoint" || t == "marker" || t == "region") ? "ref:" + t
                                                                                                 : "u32";
            out.push_back(Ex(args[k], 0, h));
        }
        const std::string nm = ApiName(slot, CallRet(e) == s::f);
        stats()[si && !si->name.empty() ? "api_calls_named" : "api_calls_unnamed"] += 1;
        return "api." + nm + "(" + JoinStr(out, ", ") + ")";
    }
    if (tgt->t == s::fn) {
        auto it = g_.fnames.find(static_cast<uint32_t>(tgt->I(1)));
        const std::string nm = it != g_.fnames.end() ? it->second : "f_" + Hex(tgt->I(1));
        std::vector<std::string> out;
        for (E a : args) out.push_back(Ex(a));
        return nm + "(" + JoinStr(out, ", ") + ")";
    }
    if (tgt->t == s::sprintf) return Sprintf(args);
    if (tgt->t == s::rand) return "crt_rand()";
    if (tgt->t == s::ptr) {
        stats()["indirect_calls"] += 1;
        std::string o = "call_ptr(" + Ex(StripT(tgt->X(1)));
        for (E a : args) o += ", " + Ex(a);
        return o + ")";
    }
    throw Unsupported{"call target " + *tgt->t};
}

std::string Printer::Sprintf(const std::vector<E>& args) {
    if (args.size() < 2) throw Unsupported{"sprintf arguments"};
    E fmt = args[1];
    const std::string* text = nullptr;
    if (fmt->t == s::gaddr) {
        auto it = g_.strings.find(fmt->I(1) - p_.base);
        if (it != g_.strings.end()) text = &it->second;
    }
    if (!text) {
        std::vector<std::string> o;
        for (E a : args) o.push_back(Ex(a));
        return "csprintf(" + JoinStr(o, ", ") + ")";
    }
    std::vector<std::string> out = {Ex(args[0]), Ex(fmt)};
    std::vector<E> rest(args.begin() + 2, args.end());
    size_t k = 0;
    static const std::regex re(R"(%[-+ #0]*(?:\d+|\*)?(?:\.\d+)?(?:hh|h|ll|l|L|I64)?([diouxXcspfeEgGn%]))");
    for (auto it = std::sregex_iterator(text->begin(), text->end(), re); it != std::sregex_iterator(); ++it) {
        const char c = (*it)[1].str()[0];
        if (c == '%') continue;
        if (std::string("feEgG").find(c) != std::string::npos && k + 1 < rest.size()) {
            E lo = rest[k], hi = rest[k + 1];
            if (lo->t == s::dlo && hi->t == s::dhi && lo->X(1) == hi->X(1)) out.push_back(Ex(lo->X(1)));
            else out.push_back("double(" + Ex(lo) + ", " + Ex(hi) + ")");
            k += 2;
        } else if (k < rest.size()) {
            out.push_back(Ex(rest[k]));
            k += 1;
        }
    }
    return "sprintf(" + JoinStr(out, ", ") + ")";
}

std::string Printer::StoreValue(E e, int64_t size, Sym extra) {
    e = StripT(e);
    if (extra == s::f) return Ex(T(s::f32, e));
    if (size < 4) {
        int64_t lo, hi;
        if (!(VRange(e, &lo, &hi) && lo >= 0 && hi < (1ll << (8 * size))))
            return Ex(e, 6) + " & " + HexU((1ll << (8 * size)) - 1);
    }
    return Ex(e);
}

void Printer::StmtOut(Stmt* st, int ind) {
    if (st->kind == kd::set) {
        W(ind, *st->loc + " = " + Ex(st->e));
    } else if (st->kind == kd::expr) {
        E e = StripT(st->e);
        std::vector<E> calls = e->t == s::call ? std::vector<E>{e} : TopCalls(e);
        for (E c : calls) W(ind, Ex(c));
    } else if (st->kind == kd::store) {
        E t = st->target;
        if (t->t == s::g) {
            auto gv = g_.gvars.find(static_cast<uint32_t>(t->I(1)));
            const std::string val = StoreValue(st->e, t->I(2), st->extra);
            if (gv == g_.gvars.end()) {
                stats()["raw_memory"] += 1;
                const int64_t sz = t->I(2);
                const char* fn = sz == 1 ? "w8" : sz == 2 ? "w16" : (st->extra == s::f ? "wf" : "w32");
                W(ind, Fmt("mem.%s(ADDR(%s), %s)", fn, HexU(t->I(1) - p_.base).c_str(), val.c_str()));
            } else if (gv->second.memory) {
                stats()["memory_globals"] += 1;
                W(ind, "D." + DField(static_cast<uint32_t>(t->I(1)), t->I(2),
                                     st->extra == s::f ? s::f : (t->I(2) < 4 ? s::u : s::i)) +
                           " = " + val);
            } else {
                W(ind, gv->second.name + " = " + val);
            }
        } else if (t->t == s::msg) {
            W(ind, "msg[" + std::to_string(t->I(1)) + "] = " + Ex(st->e));
        } else if (t->t == s::mem) {
            E a = StripT(t->X(1));
            if (HasKind(a, s::saddr)) stats()["buffer_access"] += 1;
            else stats()["raw_memory"] += 1;
            const int64_t sz = t->I(2);
            const char* fn = sz == 1 ? "w8" : sz == 2 ? "w16" : sz == 4 ? (st->extra == s::f ? "wf" : "w32") : "wd";
            E ev = StripT(st->e);
            std::string val;
            if (sz == 4 && IsC(ev) && LooksFloat(ev->I(1)) && HasKind(a, s::saddr)) val = F32Short(BitsToFloat(ev->I(1)));
            else val = StoreValue(st->e, sz, st->extra);
            W(ind, Fmt("mem.%s(%s, %s)", fn, Ex(a).c_str(), val.c_str()));
        } else {
            throw Unsupported{"store " + *t->t};
        }
    } else if (st->kind == kd::copy) {
        E d = StripT(st->ce[0]), src = StripT(st->ce[1]), n = StripT(st->ce[2]);
        const auto& rng = g_.saveRange;
        if (rng && src == T(s::gaddr, rng->first) && IsC(n) && n->I(1) == rng->second - rng->first) {
            W(ind, "write_state(" + Ex(d) + ")");
        } else if (rng && d == T(s::gaddr, rng->first) && IsC(n) && n->I(1) == rng->second - rng->first) {
            W(ind, "read_state(" + Ex(src) + ")");
        } else {
            stats()["raw_memory"] += 1;
            W(ind, "mem.copy(" + Ex(d) + ", " + Ex(src) + ", " + Ex(n) + ")");
        }
    } else {
        throw Unsupported{"statement " + *st->kind};
    }
}

void Printer::Block(const Body& body, int ind, const std::string& loop) {
    for (size_t k = 0; k < body.size(); ++k) {
        const Ast& n = body[k];
        switch (n.k) {
            case Ast::S: StmtOut(n.st, ind); break;
            case Ast::If: {
                for (size_t j = 0; j < n.arms.size(); ++j) {
                    W(ind, std::string(j == 0 ? "if" : "elseif") + " " + Cond(n.arms[j].first) + " then");
                    Block(n.arms[j].second, ind + 1, loop);
                }
                if (!n.els.empty()) {
                    W(ind, "else");
                    Block(n.els, ind + 1, loop);
                }
                W(ind, "end");
                break;
            }
            case Ast::While: {
                const std::string lab = NewLoop();
                W(ind, "while " + (n.cond ? Cond(n.cond) : std::string("true")) + " do");
                Block(n.els, ind + 1, lab);
                if (usedLoops_.count(lab)) W(ind + 1, "::" + lab + "::");
                W(ind, "end");
                break;
            }
            case Ast::Repeat:
                W(ind, "repeat");
                Block(n.els, ind + 1, "");
                W(ind, "until " + Cond(n.cond));
                break;
            case Ast::Break: W(ind, "break"); break;
            case Ast::Continue:
                if (loop.empty()) {
                    stats()["gotos"] += 1;
                    W(ind, "goto continue_outer --[[unstructured]]");
                } else {
                    usedLoops_.insert(loop);
                    stats()["continues"] += 1;
                    W(ind, "goto " + loop);
                }
                break;
            case Ast::Goto: W(ind, "goto L_" + Hex(n.label - p_.base)); break;
            case Ast::Return: {
                if (!n.cond) {
                    W(ind, "return");
                    break;
                }
                E ee = StripT(n.cond);
                const bool rets = g_.returns.count(cur_) != 0;
                if (!rets && n.rkind == kd::tail) {
                    W(ind, Ex(ee));
                    if (k != body.size() - 1 || ind > 1) W(ind, "return");
                } else if (!rets) {
                    W(ind, "return");
                } else {
                    W(ind, "return " + Ex(ee, 0, ""));
                }
                break;
            }
            case Ast::Switch: Switch(n, ind, loop); break;
            case Ast::Label: W(ind, "::L_" + Hex(n.label - p_.base) + "::"); break;
            case Ast::SetFlag: W(ind, *n.flag + " = " + (n.flagValue ? "true" : "false")); break;
        }
    }
}

void Printer::Switch(const Ast& n, int ind, const std::string& loop) {
    const std::string on = Ex(n.cond, 4);
    bool first = true;
    for (auto& [vals, body] : n.cases) {
        std::vector<std::string> cs;
        for (int64_t v : vals) cs.push_back(on + " == " + IntLit(v));
        W(ind, std::string(first ? "if" : "elseif") + " " + JoinStr(cs, " or ") + " then");
        Block(body, ind + 1, loop);
        first = false;
    }
    if (!n.els.empty()) {
        if (first) {
            Block(n.els, ind, loop);
            return;
        }
        W(ind, "else");
        Block(n.els, ind + 1, loop);
    }
    if (!first) W(ind, "end");
}

void Printer::Function(uint32_t va) {
    Func* f = tr_.F(va);
    cur_ = va;
    loopNo_ = 0;
    usedLoops_.clear();
    const std::string nm = g_.fnames.at(va);
    std::vector<std::string> params = g_.params.count(va) ? g_.params[va] : std::vector<std::string>{};
    static const std::map<Sym, std::string> what = {{kd::update, "update callback (every frame)"},
                                                    {kd::message, "message callback"},
                                                    {kd::save, "save callback"},
                                                    {kd::load, "load callback"},
                                                    {kd::size, "save size callback"},
                                                    {kd::init, "init callback"},
                                                    {kd::setup, "LevelSetup (the DLL's export)"},
                                                    {kd::queue, "queue_call callback"}};
    auto wi = what.find(f->kind);
    lines_.push_back("");
    lines_.push_back("-- " + HexU(va) + (wi != what.end() ? " - " + wi->second : std::string()));
    if (f->kind == kd::queue && f->eventForm) params = {"a", "b"};
    if (f->kind == kd::message) {
        MessageFunction(va, nm, params);
        return;
    }
    lines_.push_back("function " + nm + "(" + JoinStr(params, ", ") + ")");
    Locals(va);
    Body body = g_.ast[va];
    if (!body.empty() && body.back().k == Ast::Return && !body.back().cond) body.pop_back();
    Block(body, 1);
    lines_.push_back("end");
}

void Printer::Locals(uint32_t va) {
    Func* f = tr_.F(va);
    std::vector<std::pair<int64_t, std::pair<std::string, int64_t>>> bufs;
    int n = 0;
    for (auto& [lo, hi] : f->buffers) {
        ++n;
        const std::string nm = n == 1 ? "buf" : "buf" + std::to_string(n);
        bool replaced = false;
        for (auto& b : bufs)
            if (b.first == lo) {
                b.second = {nm, hi - lo};
                replaced = true;
            }
        if (!replaced) bufs.push_back({lo, {nm, hi - lo}});
    }
    buffers_[va] = bufs;
    const auto& vs = g_.vars[va];
    std::vector<std::string> plain, und;
    for (auto& [v, u] : vs)
        if (!u) plain.push_back(v);
    for (Sym fl : g_.flagsOf[va]) plain.push_back(*fl);
    for (auto& [v, u] : vs)
        if (u) und.push_back(v);
    if (!plain.empty()) W(1, "local " + JoinStr(plain, ", "));
    for (auto& v : und) W(1, "local " + v + " = 0 -- read before any assignment in the DLL (a register left over)");
    auto sorted = bufs;
    std::sort(sorted.begin(), sorted.end());
    if (bufs.size() == 1) {
        auto& [lo, ns] = bufs[0];
        W(1, Fmt("local %s = scratch(%lld)   -- stack buffer [esp%+lld] handed to the game", ns.first.c_str(),
                 static_cast<long long>(ns.second), static_cast<long long>(lo)));
    } else if (!bufs.empty()) {
        int64_t lo0 = INT64_MAX, hi0 = INT64_MIN;
        for (auto& [lo, ns] : bufs) {
            lo0 = std::min(lo0, lo);
            hi0 = std::max(hi0, lo + ns.second);
        }
        W(1, Fmt("local frame = scratch(%lld)   -- stack locals [esp%+lld .. esp%+lld) handed to the game / indexed",
                 static_cast<long long>(hi0 - lo0), static_cast<long long>(lo0), static_cast<long long>(hi0)));
        for (auto& [lo, ns] : sorted)
            W(1, Fmt("local %s = frame + %lld   -- %lld bytes", ns.first.c_str(), static_cast<long long>(lo - lo0),
                     static_cast<long long>(ns.second)));
    }
    const std::vector<std::string> names = g_.params.count(va) ? g_.params[va] : std::vector<std::string>{};
    for (auto& [lo, ns] : sorted)
        for (size_t k = 0; k < names.size(); ++k) {
            const int64_t off = 4 + 4 * static_cast<int64_t>(k);
            if (lo <= off && off < lo + ns.second && f->kind != kd::queue)
                W(1, Fmt("mem.w32(%s, %s)   -- argument %d lives at [esp+%lld]",
                         (off == lo ? ns.first : ns.first + " + " + std::to_string(off - lo)).c_str(), names[k].c_str(),
                         static_cast<int>(k + 1), static_cast<long long>(off)));
        }
}

void Printer::MessageFunction(uint32_t va, const std::string& nm, const std::vector<std::string>& params) {
    const Body& body = g_.ast[va];
    size_t sw = body.size();
    for (size_t k = 0; k < body.size(); ++k)
        if (body[k].k == Ast::Switch && StripT(body[k].cond) == T(s::param, 0ll)) {
            sw = k;
            break;
        }
    lines_.push_back("function " + nm + "(" + JoinStr(params, ", ") + ")");
    Locals(va);
    if (sw == body.size()) {
        Block(body, 1);
        lines_.push_back("end");
        return;
    }
    Block(Body(body.begin(), body.begin() + sw), 1);
    const Ast& n = body[sw];
    W(1, "local handler = HANDLERS[id]");
    W(1, "if handler then return handler(msg, id) end");
    if (!n.els.empty()) Block(n.els, 1);
    lines_.push_back("end");
    handlers_.clear();
    for (auto& c : n.cases) handlers_.push_back(c);
}

void Printer::EmitHandlers(uint32_t va) {
    cur_ = va;
    loopNo_ = 0;
    usedLoops_.clear();
    std::vector<std::pair<std::vector<std::string>, std::vector<std::string>>> groups;
    for (auto& [vals, b] : handlers_) {
        std::vector<std::string> save = std::move(lines_);
        lines_.clear();
        Block(b, 1);
        std::vector<std::string> body = std::move(lines_);
        lines_ = std::move(save);
        std::vector<std::string> names;
        for (int64_t v : vals) names.push_back(EventName(v));
        bool merged = false;
        for (auto& gr : groups)
            if (gr.second == body) {
                gr.first.insert(gr.first.end(), names.begin(), names.end());
                merged = true;
                break;
            }
        if (!merged) groups.push_back({names, body});
    }
    for (auto& [names, body] : groups) {
        std::string key;
        if (names.size() == 1) {
            key = LuaStr(names[0]);
        } else {
            std::vector<std::string> q;
            for (auto& x : names) q.push_back(LuaStr(x));
            key = "{" + JoinStr(q, ", ") + "}";
        }
        lines_.push_back("");
        lines_.push_back("on(" + key + ", function(msg, id)");
        lines_.insert(lines_.end(), body.begin(), body.end());
        lines_.push_back("end)");
    }
}

std::vector<std::string> Printer::LogicHeader() {
    const std::string sha = Sha256Hex(p_.p.raw);
    return {Fmt("-- %s as readable Lua: mission2lua_readable %d (ConflictDesertStormModKit), SHA-256 %s.", p_.name.c_str(),
                VERSION, sha.c_str()),
            "-- Generated from your own game files: game code. Never commit or distribute this file.",
            "-- Functions are the DLL's (address in the comment above each); API calls by name (api.*); the DLL's",
            "-- globals as named variables (address in the comment); messages as on(<event>, handler); scene objects",
            "-- by editor ID (obj.* / wp.* / rgn.*, numbers in refs.lua)."};
}

void Printer::ApiSection() {
    lines_.push_back("-- ======================== the script API this mission uses ========================");
    std::vector<int64_t> rest;
    for (auto& [slot, info] : tr_.slots)
        if (!g_.slotUsed.count(slot) && !info.name.empty()) rest.push_back(slot);
    std::vector<std::pair<bool, int64_t>> seq;
    for (int64_t sl : g_.slotUsed) seq.push_back({true, sl});
    seq.push_back({false, 0});
    for (int64_t sl : rest) seq.push_back({true, sl});
    for (auto& [isSlot, slot] : seq) {
        if (!isSlot) {
            lines_.push_back(
                "-- ======================== the rest of the script API (not called by this mission) "
                "========================");
            continue;
        }
        const SlotInfo* si = tr_.Slot(slot);
        std::vector<std::string> args;
        if (si)
            for (auto& a : si->args) args.push_back(Ident(a.name, "a"));
        std::set<std::string> seen;
        for (size_t k = 0; k < args.size(); ++k) {
            if (seen.count(args[k]) || LuaKeywords().count(args[k])) args[k] = args[k] + std::to_string(k + 1);
            seen.insert(args[k]);
        }
        std::string nm = si ? si->name : std::string();
        if (nm.empty()) nm = Fmt("slot_%03llX", static_cast<long long>(slot));
        const bool floatRet = g_.slotFloat.count(slot) ||
                              (!g_.slotUsed.count(slot) && si && StartsWith(si->returns, "float"));
        const std::string fret = floatRet ? "true" : "false";
        std::string sig, body;
        if (!args.empty() && (si->args.back().name == "args" || si->args.back().name == "format_args")) {
            std::vector<std::string> a2(args.begin(), args.end() - 1);
            a2.push_back("...");
            sig = JoinStr(a2, ", ");
            body = "return call(" + HexU(slot) + ", " + fret + ", " + sig + ")";
        } else if (!args.empty()) {
            sig = JoinStr(args, ", ");
            body = "return call(" + HexU(slot) + ", " + fret + ", " + sig + ")";
        } else {
            sig = "...";
            body = "return call(" + HexU(slot) + ", " + fret + ", ...)";
        }
        if (slot == 0x004) body = "return register_anim(" + sig + ")";
        const std::string ret = si ? si->returns : std::string();
        const std::string note = !ret.empty() && ret != "none" ? " -- -> " + ret : "";
        lines_.push_back("function api." + nm + "(" + sig + ") " + body + " end" + note);
    }
}

void Printer::StateSection() {
    lines_.push_back("");
    lines_.push_back("-- ======================== mission state (the DLL's globals) ========================");
    for (auto& [a, gv] : g_.gvars) {
        if (gv.memory) {
            lines_.push_back("-- D." + gv.name + ": " + HexU(a) + " in the memory block (its address is handed to the game)");
            continue;
        }
        std::string init;
        if (gv.kind == s::f) init = gv.initFloat ? F32Short(gv.initF) : F32Short(static_cast<double>(gv.initI));
        else init = IntLit(gv.initI);
        const std::string tag = gv.size == 1 ? "byte" : gv.size == 2 ? "word" : gv.size == 4 ? (gv.kind == s::f ? "float" : "dword") : "?";
        const bool inr = g_.saveRange && g_.saveRange->first <= a && a < g_.saveRange->second;
        lines_.push_back(gv.name + " = " + init + " -- " + HexU(a) + " " + tag + (inr ? ", in the save block" : ""));
    }
}

LVp Printer::MissionData() {
    LVp d = LDict();
    std::string stem = p_.name;
    const size_t dot = stem.rfind('.');
    if (dot != std::string::npos && dot > 0) stem = stem.substr(0, dot);
    const std::string sha = Sha256Hex(p_.p.raw);
    Put(d, "name", LStr(stem));
    Put(d, "dll", LStr(p_.name));
    Put(d, "sha256", LStr(sha));
    Put(d, "archive", LStr(level_));
    Put(d, "translator", LStr("mission2lua_readable " + std::to_string(VERSION)));
    if (g_.haveScene) Put(d, "scene", LStr(g_.sceneName));
    LVp objs = LList();
    for (auto& [a, t] : g_.objectiveTitles)
        if (g_.gvars.count(a) && t) {
            LVp o = LDict();
            Put(o, "variable", LStr(g_.gvars[a].name));
            Put(o, "title", LInt(*t));
            objs->list.push_back(o);
        }
    Put(d, "objectives", objs);
    LVp ev = LDict();
    for (auto& [nm, mid] : eventsUsed_) Put(ev, nm, LInt(mid));
    Put(d, "events", ev);
    LVp save = LDict();
    if (g_.saveRange) {
        const int64_t lo = g_.saveRange->first, hi = g_.saveRange->second;
        Put(save, "size", LInt(hi - lo));
        Put(save, "template", LStr(p_.BytesAt(static_cast<uint32_t>(lo), static_cast<size_t>(hi - lo))));
        LVp fields = LList();
        Put(save, "rva", LInt(lo - p_.base));
        LVp memory = LList();
        for (auto& [mlo, mhi] : g_.memoryAbs) {
            const int64_t a = std::max(lo, mlo), b = std::min(hi, mhi);
            if (a < b) memory->list.push_back(LList({LInt(a - lo), LInt(b - lo)}));
        }
        Put(save, "memory", memory);
        for (auto& [a, gv] : g_.gvars)
            if (lo <= a && a < hi) {
                const std::string fmt = gv.size == 1 ? "B" : gv.size == 2 ? "<I2" : (gv.kind == s::f ? "<f" : "<i4");
                if (gv.memory) fields->list.push_back(LList({LInt(a - lo), LStr(fmt), LBool(false), LInt(a - p_.base)}));
                else fields->list.push_back(LList({LInt(a - lo), LStr(fmt), LStr(gv.name)}));
            }
        Put(save, "fields", fields);
    } else {
        Put(save, "size", LInt(0));
        Put(save, "template", LStr(""));
        Put(save, "fields", LList());
    }
    Put(d, "save", save);
    LVp secs = LList(), relocs = LList();
    for (auto& [lo, hi] : g_.memoryAbs) {
        const std::string raw = p_.BytesAt(static_cast<uint32_t>(lo), static_cast<size_t>(hi - lo));
        std::string hex;
        static const char* digits = "0123456789abcdef";
        for (unsigned char ch : raw) {
            hex += digits[ch >> 4];
            hex += digits[ch & 15];
        }
        secs->list.push_back(LList({LInt(lo - p_.base), LStr(hex)}));
        for (uint32_t r : p_.p.dll.relocs)
            if (lo <= r && r < hi) relocs->list.push_back(LInt(static_cast<int64_t>(r) - p_.base));
    }
    LVp entry = LDict();
    for (auto& [va, f] : p_.funcs) {
        Sym k = f->kind;
        if (k == kd::init || k == kd::update || k == kd::message || k == kd::save || k == kd::load || k == kd::size ||
            k == kd::queue || p_.queueCbs.count(va))
            Put(entry, LInt(static_cast<int64_t>(va) - p_.base),
                LList({LStr(g_.fnames[va]), LStr(*k), LBool(f->eventForm)}));
    }
    LVp fields = LDict();
    for (auto& [k, rf] : dfields_) Put(fields, k, LList({LInt(rf.first), LStr(rf.second)}));
    Put(d, "fields", fields);
    LVp image = LDict();
    Put(image, "size", LInt(p_.p.dll.sizeOfImage));
    Put(image, "base", LInt(p_.base));
    LVp strs = LDict();
    for (auto& [rva, sv] : g_.strings) Put(strs, LInt(rva), LStr(sv));
    Put(image, "strings", strs);
    Put(image, "memory", secs);
    Put(image, "relocs", relocs);
    Put(image, "object", LInt(static_cast<int64_t>(p_.obj) - p_.base));
    LVp cbs = LList();
    for (uint32_t c : p_.cbs) cbs->list.push_back(LInt(static_cast<int64_t>(c) - p_.base));
    Put(image, "object_callbacks", cbs);
    Put(image, "entry", entry);
    Put(d, "image", image);
    return d;
}

LVp Printer::RefsData() {
    LVp out = LDict();
    for (const char* table : {"objects", "waypoints", "regions", "markers"}) {
        auto it = g_.refUsed.find(table);
        if (it == g_.refUsed.end() || it->second.empty()) continue;
        LVp t = LDict();
        for (auto& [nm, v] : it->second) Put(t, nm, LInt(v));
        Put(out, table, t);
    }
    return out;
}

void Printer::Emit(ReadableParts* out) {
    buffers_.clear();
    eventsUsed_.clear();
    dfields_.clear();
    std::vector<uint32_t> order;
    for (auto& [va, f] : p_.funcs) order.push_back(va);
    auto isCb = [&](uint32_t v) {
        if (v == p_.setup) return true;
        for (uint32_t c : p_.cbs)
            if (c == v) return true;
        return false;
    };
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        const bool na = !isCb(a), nb = !isCb(b);
        if (na != nb) return !na;
        return a < b;
    });
    for (uint32_t va : order) Function(va);
    if (p_.cbs.size() > 2) EmitHandlers(p_.cbs[2]);
    std::vector<std::string> body = std::move(lines_);
    lines_.clear();
    StateSection();
    std::vector<std::string> state = std::move(lines_);
    lines_.clear();
    std::vector<std::string> ev = LogicHeader();
    ev.insert(ev.end(), state.begin(), state.end());
    ev.push_back("");
    ev.push_back("-- ======================== mission code ========================");
    ev.insert(ev.end(), body.begin(), body.end());
    const std::string events = JoinStr(ev, "\n") + "\n";
    ApiSection();
    std::vector<std::string> rt = {
        Fmt("-- Generated glue (mission2lua_readable %d): connects scripts/events.lua to the game. Not mission logic - "
            "regenerate rather than edit.",
            VERSION),
        ReplaceAll(ReplaceAll(Join(kBridgeParts), "%VERSION%", std::to_string(VERSION)), "%BASE%", HexU(p_.base))};
    rt.insert(rt.end(), lines_.begin(), lines_.end());
    const std::string runtime = JoinStr(rt, "\n") + "\n";
    std::string main = Join(kMainParts);
    main = ReplaceAll(main, "%SIZE%", HexU(p_.p.dll.sizeOfImage));
    main = ReplaceAll(main, "%BASE%", HexU(p_.base));
    main = ReplaceAll(main, "%NAME%", p_.name);
    main = ReplaceAll(main, "%SETUP%", g_.fnames.at(p_.setup));
    const std::string mission =
        Fmt("-- %s: mission data (literals only). Generated by mission2lua_readable %d from your own %s - game data, "
            "never commit or distribute.\nreturn ",
            p_.name.c_str(), VERSION, p_.name.c_str()) +
        LuaLiteral(MissionData(), 0) + "\n";
    const std::string refs =
        "-- " + p_.name +
        ": the scene objects, waypoints and regions its logic names (editor ID = number in the level's scene).\nreturn " +
        LuaLiteral(RefsData(), 0) + "\n";
    out->mission = mission;
    out->refs = refs;
    out->events = events;
    out->runtime = runtime;
    out->start = main;
    out->bundle = BundleText(level_, p_.name, Sha256Hex(p_.p.raw), mission, refs, runtime, events, main);
}

}  // namespace

// ---- entry -----------------------------------------------------------------------------------------------------------
bool TranslateReadableImpl(const std::vector<uint8_t>& dll, const std::string& name, const std::string& level,
                           const std::function<bool(const std::string&, SceneRefs*)>& sceneRefs, ReadableParts* out,
                           std::string* error) {
    PoolScope pool;
    try {
        Generator g;
        if (!g.Init(dll, name, error)) return false;
        std::string scene;
        const bool haveScene = g.FindScene(&scene);
        if (haveScene && sceneRefs) {
            SceneRefs r;
            if (sceneRefs(scene, &r)) {
                g.refs[0] = r.objects;
                g.refs[1] = r.waypoints;
                g.refs[2] = r.regions;
                g.refs[3] = {};
            }
        }
        // generate()
        g.CollectGlobals();
        g.haveScene = g.FindScene(&g.sceneName);
        g.HintGlobals();
        g.MemoryRanges();
        g.NameGlobals();
        g.NameFunctions();
        for (uint32_t va : g.prog->funcOrder) g.Special(va);
        for (uint32_t va : g.prog->funcOrder) g.ParamNames(va);
        for (uint32_t va : g.prog->funcOrder) {
            Func& f = *g.tr.F(va);
            g.RecoverVars(va);
            std::set<int64_t> noGoto, labels;
            for (int attempt = 0; attempt < 8; ++attempt) {
                std::map<std::string, int64_t> scratch;
                StructResult r1 = Structure(g.tr, f, g.tr.ir[va], scratch, noGoto, {});
                labels = r1.labels;
                std::map<std::string, int64_t> scratch2;
                StructResult r2 = Structure(g.tr, f, g.tr.ir[va], scratch2, noGoto, labels);
                const std::set<int64_t> bad = InvisibleGotos(r2.ast);
                if (bad.empty()) break;
                noGoto.insert(bad.begin(), bad.end());
            }
            StructResult r = Structure(g.tr, f, g.tr.ir[va], g.tr.stats, noGoto, labels);
            g.ast[va] = r.ast;
            const std::set<int64_t> bad = InvisibleGotos(r.ast);
            if (!bad.empty()) g.tr.stats["invalid_gotos"] += static_cast<int64_t>(bad.size());
            g.labelsOf[va] = r.labels;
            g.flagsOf[va] = r.flags;
        }
        g.NamePhaseFunctions();
        out->scene = g.haveScene ? g.sceneName : std::string();
        Printer(g, level).Emit(out);
        return true;
    } catch (const Unsupported& u) {
        if (error) *error = "unsupported: " + u.what;
        return false;
    }
}

}  // namespace pse::m2l::rd

namespace pse::m2l {

int ReadableVersion() { return rd::VERSION; }

bool TranslateReadable(const std::vector<uint8_t>& dll, const std::string& name, const std::string& level,
                       const std::function<bool(const std::string& scene, SceneRefs* refs)>& sceneRefs,
                       ReadableParts* out, std::string* error) {
    return rd::TranslateReadableImpl(dll, name, level, sceneRefs, out, error);
}

}  // namespace pse::m2l
