// mission2lua_readable (C++ port): expressions - mission2lua_readable.py "expressions" + "dataflow" helpers.
#include <cstring>
#include <functional>
#include <mutex>
#include <unordered_set>

#include "m2r.h"

namespace pse::m2l::rd {

// ---- symbols ---------------------------------------------------------------------------------------------------------
Sym Y(const std::string& str) {
    static std::mutex mu;
    static std::unordered_set<std::string>* pool = new std::unordered_set<std::string>();
    std::lock_guard<std::mutex> lock(mu);
    return &*pool->insert(str).first;
}

namespace s {
Sym c = Y("c"), fc = Y("fc"), param = Y("param"), loc = Y("loc"), var = Y("var"), g = Y("g"), gaddr = Y("gaddr"),
    saddr = Y("saddr"), api = Y("api"), mem = Y("mem"), bin = Y("bin"), un = Y("un"), call = Y("call"), cmp = Y("cmp"),
    not_ = Y("not"), and_ = Y("and"), or_ = Y("or"), b2i = Y("b2i"), i2f = Y("i2f"), u2f = Y("u2f"), fr = Y("fr"),
    fbits = Y("fbits"), fromfbits = Y("fromfbits"), fsw = Y("fsw"), ins = Y("ins"), ext = Y("ext"), undef = Y("undef"),
    msg = Y("msg"), evt = Y("evt"), str = Y("str"), t = Y("t"), args = Y("#args"), slot = Y("slot"), fn = Y("fn"),
    sprintf = Y("sprintf"), ptr = Y("ptr"), rand = Y("rand"), sx = Y("sx"), qword = Y("qword"),
    fromdbits = Y("fromdbits"), f32 = Y("f32"), dlo = Y("dlo"), dhi = Y("dhi"), fistp = Y("fistp"), fnptr = Y("fnptr"),
    apislotaddr = Y("apislotaddr"), apislotptr = Y("apislotptr"), apiglobal = Y("apiglobal"), apislot = Y("apislot"),
    stack = Y("stack"), pair = Y("pair"), flag = Y("flag"), sub = Y("sub"), add = Y("add"), res = Y("res"),
    neg = Y("neg"), sahf = Y("sahf"), fswah = Y("fswah"), fswcmp = Y("fswcmp"), reg = Y("reg"), i = Y("i"),
    u = Y("u"), f = Y("f"), d = Y("d");
Sym plus = Y("+"), minus = Y("-"), mul = Y("*"), div = Y("/"), band = Y("&"), bor = Y("|"), bxor = Y("~"),
    shl = Y("<<"), shr = Y("shr"), sar = Y("sar"), idiv = Y("idiv"), imod = Y("imod"), udiv = Y("udiv"),
    umod = Y("umod"), umulhi = Y("umulhi"), eq = Y("=="), ne = Y("~="), lt = Y("<"), le = Y("<="), gt = Y(">"),
    ge = Y(">="), ult = Y("u<"), ule = Y("u<="), ugt = Y("u>"), uge = Y("u>="), flt = Y("f<"), fle = Y("f<="),
    fgt = Y("f>"), fge = Y("f>="), feq = Y("f=="), fne = Y("f~="), neg_ = Y("neg"), notop = Y("not");
}  // namespace s

namespace kd {
Sym set = Y("set"), store = Y("store"), expr = Y("expr"), copy = Y("copy"), cond = Y("cond"), sw = Y("switch"),
    ret = Y("ret"), tail = Y("tail"), tailmem = Y("tailmem"), fall = Y("fall"), jmp = Y("jmp"), end = Y("end"),
    call = Y("call"), read = Y("read"), plain = Y("plain"), update = Y("update"), message = Y("message"),
    save = Y("save"), load = Y("load"), size = Y("size"), init = Y("init"), setup = Y("setup"), queue = Y("queue"),
    branch = Y("branch");
}  // namespace kd

// ---- the pool --------------------------------------------------------------------------------------------------------
bool V::operator==(const V& o) const {
    if (k != o.k) return false;
    switch (k) {
        case VK::None: return true;
        case VK::Int: return i == o.i;
        case VK::Flt: return std::memcmp(&f, &o.f, sizeof f) == 0;
        case VK::Sym: return s == o.s;
        case VK::Ex: return e == o.e;
    }
    return false;
}

namespace {
size_t HashOf(Sym tag, const std::vector<V>& v) {
    size_t h = std::hash<const void*>()(tag);
    for (const V& x : v) {
        size_t y = 0;
        switch (x.k) {
            case VK::None: y = 7; break;
            case VK::Int: y = std::hash<int64_t>()(x.i); break;
            case VK::Flt: {
                uint64_t b;
                std::memcpy(&b, &x.f, 8);
                y = std::hash<uint64_t>()(b) ^ 0x9E37;
                break;
            }
            case VK::Sym: y = std::hash<const void*>()(x.s); break;
            case VK::Ex: y = std::hash<const void*>()(x.e); break;
        }
        h ^= y + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
    }
    return h;
}
struct NodeHash {
    size_t operator()(const Node* n) const { return n->hash; }
};
struct NodeEq {
    bool operator()(const Node* a, const Node* b) const { return a->t == b->t && a->v == b->v; }
};
}  // namespace

class Pool {
public:
    std::unordered_set<Node*, NodeHash, NodeEq> set;
    std::vector<std::unique_ptr<Node>> nodes;
};
thread_local Pool* g_pool = nullptr;
thread_local E C0 = nullptr, C1 = nullptr;

PoolScope::PoolScope() : pool_(new Pool()), prev_(g_pool), c0_(C0), c1_(C1) {
    g_pool = pool_;
    C0 = C(0);
    C1 = C(1);
}
PoolScope::~PoolScope() {
    g_pool = prev_;
    C0 = c0_;
    C1 = c1_;
    delete pool_;
}

E Mk(Sym tag, std::vector<V> items) {
    Node probe;
    probe.t = tag;
    probe.v = std::move(items);
    probe.hash = HashOf(tag, probe.v);
    auto it = g_pool->set.find(&probe);
    if (it != g_pool->set.end()) return *it;
    auto n = std::make_unique<Node>(std::move(probe));
    Node* p = n.get();
    g_pool->nodes.push_back(std::move(n));
    g_pool->set.insert(p);
    return p;
}

E Rebuild(E e, const std::vector<V>& items) { return Mk(e->t, items); }

int64_t S32(int64_t v) {
    v &= M32;
    return (v & 0x80000000ll) ? v - (1ll << 32) : v;
}
int64_t Sx(int64_t v, int size) {
    const int bits = size * 8;
    if (bits >= 64) return v;
    v &= (1ll << bits) - 1;
    return (v >> (bits - 1)) ? v - (1ll << bits) : v;
}
E C(int64_t v) { return T(s::c, S32(v)); }

E MkCall(E target, const std::vector<E>& args, uint32_t va, uint32_t at, Sym ret) {
    std::vector<V> a;
    for (E x : args) a.emplace_back(x);
    return T(s::call, target, Mk(s::args, a), static_cast<int64_t>(va), static_cast<int64_t>(at), ret);
}
std::vector<E> ArgsOf(E c) {
    std::vector<E> out;
    for (const V& a : CallArgs(c)) out.push_back(a.e);
    return out;
}

// ---- helpers ---------------------------------------------------------------------------------------------------------
std::vector<E> Kids(E e) {
    std::vector<E> out;
    if (!e) return out;
    if (e->t == s::call) {
        out.push_back(CallTarget(e));
        for (const V& a : CallArgs(e)) out.push_back(a.e);
        return out;
    }
    for (const V& x : e->v)
        if (x.k == VK::Ex) out.push_back(x.e);
    return out;
}

bool IsC(E e) { return e && e->t == s::c; }
bool IsBool(E e) { return e && (e->t == s::cmp || e->t == s::not_ || e->t == s::and_ || e->t == s::or_); }

bool HasCall(E e) {
    if (!e) return false;
    if (e->t == s::call) return true;
    for (E x : Kids(e))
        if (HasCall(x)) return true;
    return false;
}

bool ReadsMemory(E e) {
    if (!e) return false;
    if (e->t == s::g || e->t == s::mem || e->t == s::msg || e->t == s::evt || e->t == s::call) return true;
    for (E x : Kids(e))
        if (ReadsMemory(x)) return true;
    return false;
}

void LocsIn(E e, std::set<Sym>* out) {
    if (!e) return;
    if (e->t == s::loc) {
        out->insert(e->S(1));
        return;
    }
    for (E x : Kids(e)) LocsIn(x, out);
}
std::set<Sym> LocsIn(E e) {
    std::set<Sym> out;
    LocsIn(e, &out);
    return out;
}

E Subst(E e, const std::map<Sym, E>& m) {
    if (!e) return e;
    if (e->t == s::loc) {
        auto it = m.find(e->S(1));
        return it != m.end() ? it->second : e;
    }
    if (e->t == s::call) {
        std::vector<E> args;
        for (const V& a : CallArgs(e)) args.push_back(Subst(a.e, m));
        return MkCall(Subst(CallTarget(e), m), args, CallVa(e), CallAt(e), CallRet(e));
    }
    std::vector<V> items = e->v;
    bool ch = false;
    for (V& x : items)
        if (x.k == VK::Ex) {
            E y = Subst(x.e, m);
            if (y != x.e) {
                x = V(y);
                ch = true;
            }
        }
    return ch ? Rebuild(e, items) : e;
}

bool IsFloat(E e) {
    Sym k = e->t;
    if (k == s::fc || k == s::i2f || k == s::u2f || k == s::fr || k == s::fromfbits) return true;
    if (k == s::g || k == s::mem) return e->S(3) == s::f;
    if (k == s::msg) return e->S(2) == s::f;
    if (k == s::call) return CallRet(e) == s::f;
    if (k == s::bin && (e->S(1) == s::plus || e->S(1) == s::minus || e->S(1) == s::mul || e->S(1) == s::div))
        return IsFloat(e->X(2)) || IsFloat(e->X(3));
    if (k == s::un && e->S(1) == s::neg_) return IsFloat(e->X(2));
    return false;
}

bool VRange(E e, int64_t* lo, int64_t* hi) {
    if (e->t == s::t) e = e->X(2);
    Sym k = e->t;
    auto sizeMax = [](int64_t size) -> int64_t { return size >= 8 ? INT64_MAX : (1ll << (8 * size)) - 1; };
    if (k == s::c) {
        *lo = *hi = e->I(1);
        return true;
    }
    if (k == s::g && e->S(3) == s::u) {
        *lo = 0;
        *hi = sizeMax(e->I(2));
        return true;
    }
    if (k == s::mem && e->S(3) == s::u) {
        *lo = 0;
        *hi = sizeMax(e->I(2));
        return true;
    }
    if (k == s::b2i) {
        *lo = 0;
        *hi = 1;
        return true;
    }
    if (k == s::bin && e->S(1) == s::band) {
        int64_t a, b;
        if (VRange(e->X(3), &a, &b) && a >= 0) {
            *lo = 0;
            *hi = b;
            return true;
        }
    }
    if (k == s::ext) {
        *lo = 0;
        *hi = e->I(3);
        return true;
    }
    if (k == s::bin && e->S(1) == s::shr && IsC(e->X(3)) && e->X(3)->I(1) > 0) {
        *lo = 0;
        const int64_t sh = e->X(3)->I(1);
        *hi = sh >= 64 ? 0 : (M32 >> sh);
        return true;
    }
    return false;
}

namespace {
int64_t PyShl(int64_t x, int64_t y) {
    if (y < 0) throw Unsupported{"negative shift"};
    if (y >= 32) return 0;   // only the low 32 bits survive C()
    return static_cast<int64_t>(static_cast<uint64_t>(x) << y);
}
int64_t PySar(int64_t x, int64_t y) {
    if (y < 0) throw Unsupported{"negative shift"};
    if (y >= 63) return x < 0 ? -1 : 0;
    return x >> y;
}
int64_t PyShr(int64_t x, int64_t y) {
    if (y < 0) throw Unsupported{"negative shift"};
    if (y >= 32) return 0;
    return (x & M32) >> y;
}
bool IsAtomKind(Sym k) {
    return k == s::c || k == s::fc || k == s::param || k == s::loc || k == s::var || k == s::g || k == s::gaddr ||
           k == s::saddr || k == s::api || k == s::undef || k == s::msg || k == s::evt || k == s::str;
}
Sym InvRel(Sym r) {
    if (r == s::eq) return s::ne;
    if (r == s::ne) return s::eq;
    if (r == s::lt) return s::ge;
    if (r == s::ge) return s::lt;
    if (r == s::gt) return s::le;
    if (r == s::le) return s::gt;
    if (r == s::ult) return s::uge;
    if (r == s::uge) return s::ult;
    if (r == s::ugt) return s::ule;
    if (r == s::ule) return s::ugt;
    return nullptr;
}
}  // namespace

E Simp(E e) {
    if (!e) return e;
    Sym k = e->t;
    if (k == s::call) {
        std::vector<E> args;
        for (const V& a : CallArgs(e)) args.push_back(Simp(a.e));
        return MkCall(Simp(CallTarget(e)), args, CallVa(e), CallAt(e), CallRet(e));
    }
    if (IsAtomKind(k)) return e;
    {
        std::vector<V> items = e->v;
        bool ch = false;
        for (V& x : items)
            if (x.k == VK::Ex) {
                E y = Simp(x.e);
                if (y != x.e) {
                    x = V(y);
                    ch = true;
                }
            }
        if (ch) e = Rebuild(e, items);
    }
    if (k == s::bin) {
        Sym op = e->S(1);
        E a = e->X(2), b = e->X(3);
        if (IsC(a) && IsC(b) &&
            (op == s::plus || op == s::minus || op == s::mul || op == s::band || op == s::bor || op == s::bxor ||
             op == s::shl || op == s::shr || op == s::sar)) {
            const int64_t x = a->I(1), y = b->I(1);
            int64_t r = 0;
            if (op == s::plus) r = x + y;
            else if (op == s::minus) r = x - y;
            else if (op == s::mul) r = static_cast<int64_t>(static_cast<uint64_t>(x) * static_cast<uint64_t>(y));
            else if (op == s::band) r = x & y;
            else if (op == s::bor) r = x | y;
            else if (op == s::bxor) r = x ^ y;
            else if (op == s::shl) r = PyShl(x, y);
            else if (op == s::shr) r = PyShr(x, y);
            else r = PySar(x, y);
            return C(r);
        }
        if ((op == s::plus || op == s::minus) && IsC(b) && b->I(1) == 0) return a;
        if (op == s::plus && IsC(a) && a->I(1) == 0) return b;
        if ((op == s::plus || op == s::minus) && IsC(b) && a->t == s::bin &&
            (a->S(1) == s::plus || a->S(1) == s::minus) && IsC(a->X(3))) {
            const int64_t v = (a->S(1) == s::plus ? a->X(3)->I(1) : -a->X(3)->I(1)) +
                              (op == s::plus ? b->I(1) : -b->I(1));
            return Simp(T(s::bin, v >= 0 ? s::plus : s::minus, a->X(2), C(v < 0 ? -v : v)));
        }
        if (op == s::plus && IsC(b) && b->I(1) < 0 && b->I(1) != -0x80000000ll) return T(s::bin, s::minus, a, C(-b->I(1)));
        if (op == s::minus && IsC(b) && b->I(1) < 0 && b->I(1) != -0x80000000ll) return T(s::bin, s::plus, a, C(-b->I(1)));
        if (op == s::plus && a->t == s::gaddr && IsC(b)) return T(s::gaddr, a->I(1) + b->I(1));
        if (op == s::minus && a->t == s::gaddr && IsC(b)) return T(s::gaddr, a->I(1) - b->I(1));
        if (op == s::plus && a->t == s::saddr && IsC(b)) return T(s::saddr, a->I(1) + b->I(1));
        if (op == s::minus && a->t == s::saddr && IsC(b)) return T(s::saddr, a->I(1) - b->I(1));
        if (op == s::band && IsC(b) && a->t == s::bin && a->S(1) == s::band && IsC(a->X(3)))
            return Simp(T(s::bin, s::band, a->X(2), C(a->X(3)->I(1) & b->I(1))));
        if (op == s::band && IsC(b) && a->t == s::ins && (b->I(1) & ~(a->I(4) << a->I(3))) == 0) {
            E v = Simp(T(s::bin, s::band, a->X(2), C(a->I(4))));
            if (a->I(3)) v = Simp(T(s::bin, s::shl, v, C(a->I(3))));
            return Simp(T(s::bin, s::band, v, b));
        }
        if (op == s::band && IsC(b) && b->I(1) == -1) return a;
        if (op == s::band && IsC(b)) {
            int64_t lo, hi;
            if (VRange(a, &lo, &hi) && lo >= 0 && hi <= b->I(1) && (b->I(1) & (b->I(1) + 1)) == 0) return a;
        }
        if (op == s::band && IsC(b) && b->I(1) == 0) return C0;
        if ((op == s::bor || op == s::bxor) && IsC(b) && b->I(1) == 0) return a;
        if (op == s::bor && IsC(b) && b->I(1) == -1) return C(-1);
        if (op == s::bxor && a == b) return C0;
        if (op == s::mul && IsC(b) && b->I(1) == 1) return a;
        return e;
    }
    if (k == s::ext) {
        E x = e->X(1);
        const int64_t sh = e->I(2), mask = e->I(3);
        if (IsC(x)) return C(PySar(x->I(1), sh) & mask);
        if (x->t == s::ins) {
            E ix = x->X(1), iv = x->X(2);
            const int64_t ish = x->I(3), imask = x->I(4);
            if (ish == sh && imask == mask) return Simp(T(s::ext, iv, 0, mask));
            if ((mask << sh & imask << ish) == 0) return Simp(T(s::ext, ix, sh, mask));
        }
        if (sh == 0 && mask == M32) return x;
        if (sh == 0) {
            int64_t lo, hi;
            if (VRange(x, &lo, &hi) && lo >= 0 && hi <= mask) return x;
            return Simp(T(s::bin, s::band, x, C(mask)));
        }
        return e;
    }
    if (k == s::ins) {
        E x = e->X(1), v = e->X(2);
        const int64_t sh = e->I(3), mask = e->I(4);
        if (sh == 0 && mask == M32) return v;
        if (x == C0 || x->t == s::undef) {
            int64_t lo, hi;
            if (sh == 0 && VRange(v, &lo, &hi) && lo >= 0 && hi <= mask) return v;
            if (x == C0)
                return sh ? Simp(T(s::bin, s::shl, T(s::bin, s::band, v, C(mask)), C(sh)))
                          : Simp(T(s::bin, s::band, v, C(mask)));
        }
        if (IsC(x) && IsC(v)) return C((x->I(1) & ~(mask << sh)) | ((v->I(1) & mask) << sh));
        if (v->t == s::bin && v->S(1) == s::band && IsC(v->X(3)) && v->X(2) == T(s::ext, x, sh, mask)) {
            const int64_t clear = (mask & ~v->X(3)->I(1)) << sh;
            return Simp(T(s::bin, s::band, x, C(~clear)));
        }
        if (v->t == s::bin && v->S(1) == s::bor && IsC(v->X(3)) && v->X(2) == T(s::ext, x, sh, mask))
            return Simp(T(s::bin, s::bor, x, C((v->X(3)->I(1) & mask) << sh)));
        return e;
    }
    if (k == s::cmp) {
        Sym rel = e->S(1);
        E a = e->X(2), b = e->X(3);
        if (a->t == s::bin && (a->S(1) == s::plus || a->S(1) == s::minus) && IsC(a->X(3)) && IsC(b) &&
            (rel == s::eq || rel == s::ne)) {
            const int64_t v = a->S(1) == s::minus ? b->I(1) + a->X(3)->I(1) : b->I(1) - a->X(3)->I(1);
            return T(s::cmp, rel, a->X(2), C(v));
        }
        if (IsC(a) && IsC(b)) {
            const int64_t x = a->I(1), y = b->I(1), ux = x & M32, uy = y & M32;
            int r = -1;
            if (rel == s::eq) r = x == y;
            else if (rel == s::ne) r = x != y;
            else if (rel == s::lt) r = x < y;
            else if (rel == s::le) r = x <= y;
            else if (rel == s::gt) r = x > y;
            else if (rel == s::ge) r = x >= y;
            else if (rel == s::ult) r = ux < uy;
            else if (rel == s::ule) r = ux <= uy;
            else if (rel == s::ugt) r = ux > uy;
            else if (rel == s::uge) r = ux >= uy;
            if (r >= 0) return r ? T(s::cmp, s::eq, C0, C0) : T(s::cmp, s::ne, C0, C0);
        }
        if (rel == s::ult || rel == s::ule || rel == s::ugt || rel == s::uge) {
            int64_t al, ah, bl, bh;
            if (VRange(a, &al, &ah) && VRange(b, &bl, &bh) && al >= 0 && bl >= 0)
                return T(s::cmp, Y(rel->substr(1)), a, b);
        }
        if ((rel == s::lt || rel == s::ge) && IsC(b) && b->I(1) == 0 && a->t == s::sx && a->X(1)->t == s::ext &&
            a->X(1)->I(3) == 0xFF && a->I(2) == 1) {
            E x = a->X(1)->X(1);
            const int64_t sh = a->X(1)->I(2);
            return T(s::cmp, rel == s::lt ? s::ne : s::eq, T(s::bin, s::band, x, C(0x80ll << sh)), C0);
        }
        if ((rel == s::lt || rel == s::ge) && IsC(b) && b->I(1) == 0 && a->t == s::sx && (a->I(2) == 1 || a->I(2) == 2))
            return T(s::cmp, rel == s::lt ? s::ne : s::eq,
                     Simp(T(s::bin, s::band, a->X(1), C(a->I(2) == 1 ? 0x80 : 0x8000))), C0);
        if ((rel == s::eq || rel == s::ne) && a->t == s::b2i && IsC(b) && b->I(1) == 0)
            return rel == s::ne ? a->X(1) : Simp(T(s::not_, a->X(1)));
        return e;
    }
    if (k == s::un && e->X(2)->t == s::un && e->X(2)->S(1) == e->S(1)) return e->X(2)->X(2);
    if (k == s::not_) {
        E c = e->X(1);
        if (c->t == s::not_) return c->X(1);
        if (c->t == s::cmp) {
            if (Sym inv = InvRel(c->S(1))) return T(s::cmp, inv, c->X(2), c->X(3));
            if (c->S(1) == s::feq) return T(s::cmp, s::fne, c->X(2), c->X(3));
            if (c->S(1) == s::fne) return T(s::cmp, s::feq, c->X(2), c->X(3));
        }
        if (c->t == s::and_) return T(s::or_, Simp(T(s::not_, c->X(1))), Simp(T(s::not_, c->X(2))));
        if (c->t == s::or_) return T(s::and_, Simp(T(s::not_, c->X(1))), Simp(T(s::not_, c->X(2))));
        return e;
    }
    return e;
}

// ---- dataflow helpers ------------------------------------------------------------------------------------------------
bool Stable(E e) {
    if (!e) return true;
    Sym k = e->t;
    if (k == s::c || k == s::fc || k == s::param || k == s::api || k == s::gaddr || k == s::saddr || k == s::fnptr ||
        k == s::apislotaddr || k == s::apislotptr || k == s::undef)
        return true;
    if (k == s::loc || k == s::var || k == s::g || k == s::mem || k == s::msg || k == s::evt || k == s::call ||
        k == s::t || k == s::fsw)
        return false;
    for (E x : Kids(e))
        if (!Stable(x)) return false;
    return true;
}

E StripT(E e) {
    if (!e) return e;
    if (e->t == s::t) return StripT(e->X(2));
    if (e->t == s::call) {
        std::vector<E> args;
        for (const V& a : CallArgs(e)) args.push_back(StripT(a.e));
        return MkCall(StripT(CallTarget(e)), args, CallVa(e), CallAt(e), CallRet(e));
    }
    std::vector<V> items = e->v;
    bool ch = false;
    for (V& x : items)
        if (x.k == VK::Ex) {
            E y = StripT(x.e);
            if (y != x.e) {
                x = V(y);
                ch = true;
            }
        }
    return ch ? Rebuild(e, items) : e;
}

E TagReads(E e, int64_t t) {
    if (!e) return e;
    Sym k = e->t;
    if (k == s::t) return e;
    if (k == s::g || k == s::mem || k == s::msg || k == s::evt) {
        if (k == s::mem) return T(s::t, t, T(s::mem, TagReads(e->X(1), t), e->I(2), e->S(3)));
        return T(s::t, t, e);
    }
    if (k == s::call) {
        std::vector<E> args;
        for (const V& a : CallArgs(e)) args.push_back(TagReads(a.e, t));
        return MkCall(TagReads(CallTarget(e), t), args, CallVa(e), CallAt(e), CallRet(e));
    }
    std::vector<V> items = e->v;
    bool ch = false;
    for (V& x : items)
        if (x.k == VK::Ex) {
            E y = TagReads(x.e, t);
            if (y != x.e) {
                x = V(y);
                ch = true;
            }
        }
    return ch ? Rebuild(e, items) : e;
}

void Events(E e, int64_t now, std::vector<Ev>* out) {
    if (!e) return;
    Sym k = e->t;
    if (k == s::t) {
        Events(e->X(2), e->I(1), out);
        return;
    }
    if (k == s::call) {
        Events(CallTarget(e), now, out);
        for (const V& a : CallArgs(e)) Events(a.e, now, out);
        out->push_back({true, static_cast<int64_t>(CallAt(e))});
        return;
    }
    if (k == s::g || k == s::msg || k == s::evt) {
        out->push_back({false, now});
        return;
    }
    if (k == s::mem) {
        Events(e->X(1), now, out);
        out->push_back({false, now});
        return;
    }
    for (const V& x : e->v)
        if (x.k == VK::Ex) Events(x.e, now, out);
}

bool OrderOk(const std::vector<Ev>& ev) {
    for (size_t a = 0; a < ev.size(); ++a)
        for (size_t b = a + 1; b < ev.size(); ++b)
            if ((ev[a].call || ev[b].call) && ev[a].time > ev[b].time) return false;
    return true;
}

bool HasKind(E e, Sym k) {
    if (!e) return false;
    if (e->t == k) return true;
    for (E x : Kids(e))
        if (HasKind(x, k)) return true;
    return false;
}
void FindKind(E e, Sym k, std::vector<E>* out) {
    if (!e) return;
    if (e->t == k) out->push_back(e);
    for (E x : Kids(e)) FindKind(x, k, out);
}
std::vector<E> FindKind(E e, Sym k) {
    std::vector<E> out;
    FindKind(e, k, &out);
    return out;
}
int CountLoc(E e, Sym loc) {
    if (!e) return 0;
    if (e->t == s::loc) return e->S(1) == loc ? 1 : 0;
    int n = 0;
    for (E x : Kids(e)) n += CountLoc(x, loc);
    return n;
}

// ---- statements ------------------------------------------------------------------------------------------------------
std::vector<E> StmtExprs(const Stmt& st) {
    std::vector<E> out;
    if (st.kind == kd::copy) {
        for (E x : st.ce) out.push_back(x);
    } else if (st.e) {
        out.push_back(st.e);
    }
    if (st.kind == kd::store && st.target->t == s::mem) out.push_back(st.target->X(1));
    return out;
}

E StmtExpr(const Stmt& st) {
    if (st.kind == kd::store && st.target->t == s::mem) return T(s::pair, st.target->X(1), st.e);
    if (st.kind == kd::copy) return T(s::pair, st.ce[0], st.ce[1], st.ce[2]);
    return st.e;
}

std::set<Sym> StmtUses(const Stmt& st) {
    std::set<Sym> out;
    if (st.kind == kd::set || st.kind == kd::expr || st.kind == kd::cond || st.kind == kd::sw || st.kind == kd::ret || st.kind == kd::tail) {
        if (st.e) LocsIn(st.e, &out);
    }
    if (st.kind == kd::store) {
        LocsIn(st.e, &out);
        if (st.target->t == s::mem) LocsIn(st.target->X(1), &out);
    }
    if (st.kind == kd::copy)
        for (E x : st.ce) LocsIn(x, &out);
    return out;
}

Sym StmtEffect(const Stmt& st) {
    if (st.kind == kd::store || st.kind == kd::copy) return kd::store;
    if (st.e && HasCall(st.e)) return kd::call;
    if (st.e && ReadsMemory(st.e)) return kd::read;
    return nullptr;
}

void SubstStmt(Stmt* st, const std::map<Sym, E>& m) {
    if (st->kind == kd::copy) {
        for (E& x : st->ce) x = Simp(Subst(x, m));
        return;
    }
    if (st->e) st->e = Simp(Subst(st->e, m));
    if (st->kind == kd::store && st->target->t == s::mem)
        st->target = T(s::mem, Simp(Subst(st->target->X(1), m)), st->target->I(2));
}

int Occurrences(const Stmt& st, Sym loc) {
    int n = 0;
    for (E e : StmtExprs(st)) n += CountLoc(e, loc);
    return n;
}

IR DeepCopy(const IR& ir, StmtArena* arena) {
    IR out;
    out.order = ir.order;
    for (auto& [va, list] : ir.m) {
        StmtList& o = out.m[va];
        for (const Stmt* st : list) {
            Stmt* c = arena->New(*st);
            if (st->table) c->table = std::make_shared<TableInfo>(*st->table);
            o.push_back(c);
        }
    }
    return out;
}

}  // namespace pse::m2l::rd
