// mission2lua_readable (C++ port): structuring - blocks -> if / while / repeat / switch (mission2lua_readable.Structurer,
// _atom_key, _consistent, _minimise, _formula_expr, _tidy, _invisible_gotos, _has_continue).
#include <algorithm>
#include <functional>

#include "m2r.h"

namespace pse::m2l::rd {

bool Ast::operator==(const Ast& o) const { return k == o.k && k == Break; }

namespace {

constexpr int64_t EXIT = -1;

struct SNode {
    int64_t id = 0;
    StmtList stmts;
    Sym kind = nullptr;   // fall / branch / switch / ret / tail
    std::vector<std::pair<E, int64_t>> branches;
    std::vector<int64_t> succ, preds;
    Stmt* term = nullptr;
    std::map<int64_t, int64_t> table;
};

// a Python dict of nodes: insertion order, deletions
struct Nodes {
    std::vector<int64_t> order;
    std::map<int64_t, SNode> m;
    bool has(int64_t v) const { return m.count(v) != 0; }
    SNode* get(int64_t v) {
        auto it = m.find(v);
        return it == m.end() ? nullptr : &it->second;
    }
    SNode& at(int64_t v) { return m.at(v); }
    void erase(int64_t v) {
        m.erase(v);
        order.erase(std::remove(order.begin(), order.end(), v), order.end());
    }
    std::vector<int64_t> keys() const { return order; }
};

template <class T>
std::vector<T> Unique(const std::vector<T>& v) {   // list(dict.fromkeys(v))
    std::vector<T> out;
    std::set<T> seen;
    for (const T& x : v)
        if (seen.insert(x).second) out.push_back(x);
    return out;
}

bool PureCond(E e) {   // _pure_cond
    e = StripT(e);
    if (!e) return true;
    if (e->t == s::call || e->t == s::mem || e->t == s::loc) return false;
    for (E x : Kids(e))
        if (!PureCond(x)) return false;
    return true;
}

std::pair<E, bool> AtomKey(E c) {   // _atom_key
    if (c->t == s::not_) {
        auto [k, neg] = AtomKey(c->X(1));
        return {k, !neg};
    }
    if (c->t == s::cmp) {
        Sym r = c->S(1);
        if (r == s::ne || r == s::ge || r == s::gt || r == s::uge || r == s::ugt || r == s::fne)
            return {Simp(T(s::not_, c)), true};
    }
    return {c, false};
}

int SatAtom(E a, int64_t x) {   // _sat_atom: 1 / 0, -1 = None
    Sym rel = a->S(1);
    const int64_t b = a->X(3)->I(1), ux = x & M32, ub = b & M32;
    if (rel == s::eq) return x == b;
    if (rel == s::lt) return x < b;
    if (rel == s::le) return x <= b;
    if (rel == s::ult) return ux < ub;
    if (rel == s::ule) return ux <= ub;
    return -1;
}

bool Consistent(const std::vector<E>& atoms, const std::vector<bool>& val) {   // _consistent
    std::vector<E> keyOrder;
    std::map<E, std::vector<std::pair<E, bool>>> groups;
    for (size_t k = 0; k < atoms.size(); ++k) {
        E a = atoms[k];
        if (a->t == s::cmp &&
            (a->S(1) == s::eq || a->S(1) == s::lt || a->S(1) == s::le || a->S(1) == s::ult || a->S(1) == s::ule) &&
            IsC(a->X(3))) {
            if (!groups.count(a->X(2))) keyOrder.push_back(a->X(2));
            groups[a->X(2)].push_back({a, val[k]});
        }
    }
    for (E lhs : keyOrder) {
        const auto& items = groups[lhs];
        if (items.size() < 2) continue;
        std::set<int64_t> cands = {0, -1, 1, 0x7FFFFFFF, -0x80000000ll};
        for (auto& [a, v] : items) {
            const int64_t b = a->X(3)->I(1);
            cands.insert({b, b - 1, b + 1});
        }
        bool any = false;
        for (int64_t x : cands) {
            bool all = true;
            for (auto& [a, v] : items) all &= SatAtom(a, x) == (v ? 1 : 0);
            if (all) {
                any = true;
                break;
            }
        }
        if (!any) return false;
    }
    return true;
}

int Pop(int64_t v) {
    int n = 0;
    for (uint64_t x = static_cast<uint64_t>(v); x; x &= x - 1) ++n;
    return n;
}

using Term = std::vector<std::pair<int, bool>>;
std::vector<Term> Minimise(const std::vector<int64_t>& on, const std::vector<int64_t>& dc, int n) {   // _minimise
    if (on.empty()) return {};
    if (static_cast<int64_t>(on.size() + dc.size()) == (1ll << n)) return {Term{}};
    using P = std::pair<int64_t, int64_t>;
    std::set<P> terms;
    for (int64_t b : on) terms.insert({b, 0});
    for (int64_t b : dc) terms.insert({b, 0});
    std::set<P> primes;
    while (!terms.empty()) {
        std::set<P> nxt, used;
        std::vector<int64_t> maskOrder;
        std::map<int64_t, std::vector<P>> bymask;
        for (const P& t : terms) {   // sorted
            if (!bymask.count(t.second)) maskOrder.push_back(t.second);
            bymask[t.second].push_back(t);
        }
        for (int64_t mask : maskOrder) {
            const std::vector<P>& ts = bymask[mask];
            const std::set<P> sset(ts.begin(), ts.end());
            for (const P& vm : ts) {
                for (int k = 0; k < n; ++k) {
                    const int64_t bit = 1ll << k;
                    if ((vm.second & bit) || (vm.first & bit)) continue;
                    const P o{vm.first | bit, vm.second};
                    if (sset.count(o)) {
                        nxt.insert({vm.first, vm.second | bit});
                        used.insert(vm);
                        used.insert(o);
                    }
                }
            }
        }
        for (const P& t : terms)
            if (!used.count(t)) primes.insert(t);
        terms = nxt;
    }
    auto covers = [](const P& p, int64_t b) { return (b & ~p.second) == (p.first & ~p.second); };
    std::set<int64_t> left(on.begin(), on.end());
    std::vector<P> chosen;
    while (!left.empty()) {
        const P* best = nullptr;
        std::pair<int, int> bestKey{-1, -1};
        for (const P& p : primes) {   // max(sorted(primes)): the first of the largest
            int cnt = 0;
            for (int64_t b : left) cnt += covers(p, b);
            const std::pair<int, int> key{cnt, Pop(p.second)};
            if (!best || key > bestKey) {
                best = &p;
                bestKey = key;
            }
        }
        chosen.push_back(*best);
        std::set<int64_t> nl;
        for (int64_t b : left)
            if (!covers(*best, b)) nl.insert(b);
        left = nl;
    }
    std::vector<Term> out;
    for (const P& vm : chosen) {
        Term t;
        for (int k = 0; k < n; ++k)
            if (!((vm.second >> k) & 1)) t.push_back({k, ((vm.first >> k) & 1) == 1});
        out.push_back(t);
    }
    return out;
}

E FormulaExpr(const std::vector<Term>& terms, const std::vector<E>& atoms) {   // _formula_expr
    std::vector<E> ors;
    for (const Term& t : terms) {
        std::vector<E> ands;
        for (auto& [k, val] : t) ands.push_back(val ? atoms[k] : Simp(T(s::not_, atoms[k])));
        if (ands.empty()) return T(s::cmp, s::eq, C0, C0);
        E e = ands[0];
        for (size_t q = 1; q < ands.size(); ++q) e = T(s::and_, e, ands[q]);
        ors.push_back(e);
    }
    E e = ors[0];
    for (size_t q = 1; q < ors.size(); ++q) e = T(s::or_, e, ors[q]);
    return e;
}

bool HasContinue(const Body& body);
bool IsBreakOnly(const Body& b) { return b.size() == 1 && b[0].k == Ast::Break; }

Body Tidy(const Body& body) {   // _tidy
    Body out;
    for (const Ast& node : body) {
        if (node.k == Ast::If) {
            std::vector<std::pair<E, Body>> arms;
            for (auto& [c, b] : node.arms) arms.push_back({c, Tidy(b)});
            Body els = node.els.empty() ? Body{} : Tidy(node.els);
            if (arms.size() == 1 && arms[0].second.empty() && !els.empty()) {
                arms = {{Simp(T(s::not_, arms[0].first)), els}};
                els.clear();
            }
            if (arms.size() == 1 && arms[0].second.empty() && els.empty() && !HasCall(arms[0].first)) continue;
            while (!els.empty() && els.size() == 1 && els[0].k == Ast::If) {
                Ast inner = els[0];
                for (auto& a : inner.arms) arms.push_back(a);
                els = inner.els;
            }
            if (arms.size() > 1 && arms[0].second.empty()) {
                E c0 = Simp(T(s::not_, arms[0].first));
                if (arms.size() == 2 && els.empty()) {
                    arms = {{T(s::and_, c0, arms[1].first), arms[1].second}};
                } else {
                    Ast rest = Ast::Mk(Ast::If);
                    rest.arms.assign(arms.begin() + 1, arms.end());
                    rest.els = els;
                    arms = {{c0, Tidy(Body{rest})}};
                    els.clear();
                }
            }
            if (arms.size() > 1 && arms.back().second.empty() && !els.empty()) {
                arms.back() = {Simp(T(s::not_, arms.back().first)), els};
                els.clear();
            }
            while (arms.size() > 1 && arms.back().second.empty() && els.empty() && !HasCall(arms.back().first)) arms.pop_back();
            Ast n = Ast::Mk(Ast::If);
            n.arms = arms;
            n.els = els;
            out.push_back(n);
        } else if (node.k == Ast::While) {
            Body b = Tidy(node.els);
            while (!b.empty() && b.back().k == Ast::Continue) b.pop_back();
            if (!b.empty() && b[0].k == Ast::If && b[0].arms.size() == 1 && b[0].els.empty() &&
                IsBreakOnly(b[0].arms[0].second)) {
                Ast w = Ast::Mk(Ast::While);
                w.cond = Simp(T(s::not_, b[0].arms[0].first));
                w.els.assign(b.begin() + 1, b.end());
                out.push_back(w);
                continue;
            }
            if (!b.empty() && b.back().k == Ast::If && b.back().arms.size() == 1 && b.back().els.empty() &&
                IsBreakOnly(b.back().arms[0].second) && !HasContinue(Body(b.begin(), b.end() - 1))) {
                Ast r = Ast::Mk(Ast::Repeat);
                r.els.assign(b.begin(), b.end() - 1);
                r.cond = b.back().arms[0].first;
                out.push_back(r);
                continue;
            }
            if (!b.empty() && b.back().k == Ast::If && b.back().arms.size() == 1 && IsBreakOnly(b.back().els) &&
                !HasContinue(Body(b.begin(), b.end() - 1))) {
                bool allCont = true;
                for (const Ast& x : b.back().arms[0].second) allCont &= x.k == Ast::Continue;
                if (allCont) {
                    Ast r = Ast::Mk(Ast::Repeat);
                    r.els.assign(b.begin(), b.end() - 1);
                    r.cond = Simp(T(s::not_, b.back().arms[0].first));
                    out.push_back(r);
                    continue;
                }
            }
            Ast w = Ast::Mk(Ast::While);
            w.els = b;
            out.push_back(w);
        } else if (node.k == Ast::Switch) {
            Ast n = node;
            n.cases.clear();
            for (auto& [v, b] : node.cases) n.cases.push_back({v, Tidy(b)});
            n.els = node.els.empty() ? Body{} : Tidy(node.els);
            out.push_back(n);
        } else if (node.k == Ast::Repeat) {
            Ast n = node;
            n.els = Tidy(node.els);
            out.push_back(n);
        } else {
            out.push_back(node);
            if (node.k == Ast::Return || node.k == Ast::Break || node.k == Ast::Goto || node.k == Ast::Continue) break;
        }
    }
    return out;
}

bool HasContinue(const Body& body) {   // _has_continue
    for (const Ast& n : body) {
        if (n.k == Ast::Continue) return true;
        if (n.k == Ast::If) {
            for (auto& [c, b] : n.arms)
                if (HasContinue(b)) return true;
            if (!n.els.empty() && HasContinue(n.els)) return true;
        }
        if (n.k == Ast::Switch) {
            for (auto& [v, b] : n.cases)
                if (HasContinue(b)) return true;
            if (!n.els.empty() && HasContinue(n.els)) return true;
        }
    }
    return false;
}

// ---- Structurer ------------------------------------------------------------------------------------------------------
class Structurer {
public:
    Structurer(Translator& tr, Func& f, IR& ir, std::map<std::string, int64_t>& stats)
        : tr_(tr), f_(f), ir_(ir), stats_(stats) {}
    std::set<int64_t> labels, labelsFinal, noGoto;
    std::vector<Sym> flags;
    void Build();
    Body Run();

private:
    using Loop = std::tuple<int64_t, const std::set<int64_t>*, bool, int64_t>;   // (head, body, has follow, follow)
    void ForwardEmpty();
    void ShortCircuit();
    void AbsorbSwitchBounds();
    void CollapseConditions();
    bool Formulas(int64_t root, const std::set<int64_t>& rs, std::vector<std::pair<E, int64_t>>* out);
    void IPdom();
    const std::set<int64_t>& Reachable(int64_t v);
    int Size(int64_t v, int limit = 200);
    bool LoopFollow(int64_t h, int64_t* out);
    bool SmallTail(int64_t v);
    std::pair<std::pair<bool, int64_t>, std::pair<bool, int64_t>> ChooseJoin(int64_t v, const std::vector<int64_t>& targets,
                                                                             const Loop* loop);
    bool ReachesAvoiding(int64_t src, int64_t dst, int64_t avoid);
    Body Jump(int64_t t, const Loop* loop, bool hasStop, int64_t stop);
    Body DupOrGoto(int64_t t, bool hasStop, int64_t stop, const Loop* loop);
    Body Region(bool hasV, int64_t v, bool hasStop, int64_t stop, const Loop* loop, bool dup = false);
    Body EmitNode(int64_t v, bool hasStop, int64_t stop, const Loop* loop, bool dup = false);
    Body Guarded(Sym flag, int64_t m, int64_t j, const Loop* loop);
    Body Arm(bool hasT, int64_t t, bool hasJoin, int64_t join, const Loop* loop, bool hasSkip, int64_t skip, Sym flag);
    bool InLoop(const Loop* loop, int64_t v) const { return std::get<1>(*loop)->count(v) != 0; }
    bool InDom(int64_t x, int64_t v) const {   // v in self.dom.get(x, ())
        auto it = dom_.find(x);
        return it != dom_.end() && it->second.count(v);
    }

    Translator& tr_;
    Func& f_;
    IR& ir_;
    std::map<std::string, int64_t>& stats_;
    Nodes nodes_;
    std::vector<int64_t> initOrder_;   // the blocks' order when the graph was built (Python: self.order then)
    std::vector<int64_t> order_;
    std::map<int64_t, std::set<int64_t>> dom_;
    std::set<std::pair<int64_t, int64_t>> back_, retreat_;
    std::vector<int64_t> loopOrder_;
    std::map<int64_t, std::set<int64_t>> loops_;
    std::map<int64_t, std::pair<bool, int64_t>> ipdom_;
    int flagNo_ = 0;
    std::vector<std::pair<int64_t, Sym>> skipStack_;
    std::set<int64_t> emitted_;
    int dupUsed_ = 0;
    std::vector<int64_t> active_;
    std::map<int64_t, std::set<int64_t>> reach_;
    bool hasNext_ = false;
    int64_t next_ = 0;
};

std::vector<int64_t> NodeRpo(Nodes& nodes, int64_t entry) {
    std::set<int64_t> seen{entry};
    std::vector<int64_t> out;
    std::vector<std::pair<int64_t, size_t>> stack{{entry, 0}};
    while (!stack.empty()) {
        auto& top = stack.back();
        const std::vector<int64_t>& succ = nodes.at(top.first).succ;
        if (top.second >= succ.size()) {
            out.push_back(top.first);
            stack.pop_back();
            continue;
        }
        const int64_t nx = succ[top.second++];
        if (!seen.count(nx)) {
            seen.insert(nx);
            stack.push_back({nx, 0});
        }
    }
    return {out.rbegin(), out.rend()};
}

void Structurer::Build() {
    const std::vector<uint32_t> order = Rpo(f_.blocks, f_.va);
    initOrder_.assign(order.begin(), order.end());
    for (uint32_t va : order) {
        const Block& b = f_.blocks.at(va);
        SNode n;
        n.id = va;
        StmtList st = ir_[va];
        Stmt* term = nullptr;
        if (!st.empty() && (st.back()->kind == kd::cond || st.back()->kind == kd::sw || st.back()->kind == kd::ret ||
                            st.back()->kind == kd::tail))
            term = st.back();
        if (term) st.pop_back();
        n.stmts = st;
        n.term = term;
        if (!term) {
            n.kind = kd::fall;
            if (!b.succ.empty()) n.succ = {b.succ[0]};
        } else if (term->kind == kd::cond) {
            n.kind = kd::branch;
            const int64_t taken = b.succ[0], fall = b.succ.size() > 1 ? b.succ[1] : b.succ[0];
            n.branches = {{term->e, taken}, {nullptr, fall}};
            n.succ = {taken, fall};
        } else if (term->kind == kd::sw) {
            n.kind = kd::sw;
            for (auto& [k, t] : term->table->values) n.table[k] = t;
            std::vector<int64_t> vs;
            for (auto& [k, t] : n.table) vs.push_back(t);
            n.succ = Unique(vs);
        } else {
            n.kind = term->kind;
        }
        nodes_.order.push_back(va);
        nodes_.m[va] = std::move(n);
    }
    flags.clear();
    flagNo_ = 0;
    ForwardEmpty();
    AbsorbSwitchBounds();
    ShortCircuit();
    CollapseConditions();
    for (auto& [id, n] : nodes_.m) n.preds.clear();
    for (int64_t id : nodes_.order) {
        SNode& n = nodes_.at(id);
        std::vector<int64_t> keep;
        for (int64_t sc : Unique(n.succ))
            if (nodes_.has(sc)) keep.push_back(sc);
        n.succ = keep;
        for (int64_t sc : n.succ) nodes_.at(sc).preds.push_back(n.id);
    }
    order_ = NodeRpo(nodes_, f_.va);
    // _dominators
    {
        const int64_t entry = f_.va;
        for (int64_t v : order_) dom_[v] = std::set<int64_t>(order_.begin(), order_.end());
        dom_[entry] = {entry};
        bool changed = true;
        while (changed) {
            changed = false;
            for (int64_t v : order_) {
                if (v == entry) continue;
                std::set<int64_t> nw;
                bool first = true;
                for (int64_t p : nodes_.at(v).preds) {
                    auto it = dom_.find(p);
                    if (it == dom_.end()) continue;
                    if (first) {
                        nw = it->second;
                        first = false;
                    } else {
                        std::set<int64_t> x;
                        for (int64_t y : nw)
                            if (it->second.count(y)) x.insert(y);
                        nw = x;
                    }
                }
                nw.insert(v);
                if (nw != dom_[v]) {
                    dom_[v] = nw;
                    changed = true;
                }
            }
        }
    }
    std::map<int64_t, size_t> pos;
    for (size_t k = 0; k < order_.size(); ++k) pos[order_[k]] = k;
    for (int64_t id : nodes_.order) {
        const SNode& n = nodes_.at(id);
        if (!pos.count(n.id)) continue;
        for (int64_t sc : n.succ) {
            if (pos.count(sc) && pos[sc] <= pos[n.id]) {
                retreat_.insert({n.id, sc});
                if (InDom(n.id, sc)) back_.insert({n.id, sc});
            }
        }
    }
    for (auto& [u, h] : back_) {   // sorted
        std::set<int64_t> body{h};
        std::vector<int64_t> todo{u};
        while (!todo.empty()) {
            const int64_t v = todo.back();
            todo.pop_back();
            if (body.count(v)) continue;
            body.insert(v);
            for (int64_t p : nodes_.at(v).preds) todo.push_back(p);
        }
        loops_[h].insert(body.begin(), body.end());
    }
    IPdom();
}

void Structurer::ForwardEmpty() {
    std::function<int64_t(int64_t, std::set<int64_t>)> final_ = [&](int64_t t, std::set<int64_t> seen) -> int64_t {
        SNode* n = nodes_.get(t);
        if (!n || t == f_.va || n->kind != kd::fall || !n->stmts.empty() || n->succ.empty() || seen.count(t)) return t;
        seen.insert(t);
        return final_(n->succ[0], seen);
    };
    for (int64_t id : nodes_.order) {
        SNode& n = nodes_.at(id);
        if (n.kind == kd::branch) {
            for (auto& br : n.branches) br.second = final_(br.second, {});
            n.succ.clear();
            for (auto& br : n.branches) n.succ.push_back(br.second);
        } else if (n.kind == kd::sw) {
            for (auto& [k, t] : n.table) t = final_(t, {});
            std::vector<int64_t> vs;
            for (auto& [k, t] : n.table) vs.push_back(t);
            n.succ = Unique(vs);
        } else if (n.kind == kd::fall && !n.succ.empty()) {
            n.succ = {final_(n.succ[0], {})};
        }
    }
    std::set<int64_t> reach;
    std::vector<int64_t> todo{f_.va};
    while (!todo.empty()) {
        const int64_t x = todo.back();
        todo.pop_back();
        if (reach.count(x)) continue;
        reach.insert(x);
        for (int64_t sc : nodes_.at(x).succ) todo.push_back(sc);
    }
    for (int64_t x : nodes_.keys())
        if (!reach.count(x)) nodes_.erase(x);
}

void Structurer::ShortCircuit() {
    bool changed = true;
    while (changed) {
        changed = false;
        std::map<int64_t, std::vector<int64_t>> preds;
        for (int64_t id : nodes_.order)
            for (int64_t sc : nodes_.at(id).succ) preds[sc].push_back(id);
        for (int64_t x : nodes_.keys()) {
            SNode* n = nodes_.get(x);
            if (!n || n->kind != kd::branch || n->branches.size() != 2) continue;
            E c = n->branches[0].first;
            const int64_t t = n->branches[0].second, fl = n->branches[1].second;
            for (char side : {'f', 't'}) {
                const int64_t y = side == 'f' ? fl : t;
                SNode* m = nodes_.get(y);
                if (!m || y == x || m->kind != kd::branch || m->branches.size() != 2 || !m->stmts.empty() ||
                    preds[y] != std::vector<int64_t>{x})
                    continue;
                E cy = m->branches[0].first;
                const int64_t ty = m->branches[0].second, fy = m->branches[1].second;
                E nc = nullptr;
                int64_t nt = 0, nf = 0;
                if (side == 'f' && ty == t) {
                    nc = T(s::or_, c, cy);
                    nt = t;
                    nf = fy;
                } else if (side == 'f' && fy == t) {
                    nc = T(s::or_, c, Simp(T(s::not_, cy)));
                    nt = t;
                    nf = ty;
                } else if (side == 't' && fy == fl) {
                    nc = T(s::and_, c, cy);
                    nt = ty;
                    nf = fl;
                } else if (side == 't' && ty == fl) {
                    nc = T(s::and_, c, Simp(T(s::not_, cy)));
                    nt = fy;
                    nf = fl;
                }
                if (!nc) continue;
                n->branches = {{nc, nt}, {nullptr, nf}};
                n->succ = {nt, nf};
                nodes_.erase(y);
                stats_["conditions_joined"] += 1;
                changed = true;
                break;
            }
            if (changed) break;
        }
    }
}

void Structurer::AbsorbSwitchBounds() {
    for (int64_t id : nodes_.keys()) {
        SNode* n = nodes_.get(id);
        if (!n || n->kind != kd::branch || !n->stmts.empty()) continue;
        E c = StripT(n->branches[0].first);
        const int64_t taken = n->branches[0].second, fall = n->branches[1].second;
        if (c->t != s::cmp || (c->S(1) != s::ugt && c->S(1) != s::uge) || !IsC(c->X(3))) continue;
        SNode* sw = nodes_.get(fall);
        if (!sw || sw->kind != kd::sw || !sw->stmts.empty() || StripT(sw->term->e) != c->X(2)) continue;
        const int64_t bound = c->S(1) == s::ugt ? c->X(3)->I(1) : c->X(3)->I(1) - 1;
        if (sw->table.rbegin()->first > bound) continue;
        auto ti = std::make_shared<TableInfo>(*sw->term->table);
        ti->hasDefault = true;
        ti->def = static_cast<uint32_t>(taken);
        sw->term->table = ti;
        std::vector<int64_t> su = sw->succ;
        su.push_back(taken);
        sw->succ = Unique(su);
        n->kind = kd::fall;
        n->branches.clear();
        n->succ = {fall};
        n->term = nullptr;
    }
}

void Structurer::CollapseConditions() {
    std::set<int64_t> purec;
    for (int64_t id : nodes_.order) {
        const SNode& n = nodes_.at(id);
        if (n.kind == kd::branch && n.stmts.empty() && PureCond(n.branches[0].first)) purec.insert(id);
    }
    std::map<int64_t, std::vector<int64_t>> preds;
    for (int64_t id : nodes_.order)
        for (int64_t sc : nodes_.at(id).succ) preds[sc].push_back(id);
    std::set<int64_t> done;
    for (int64_t v : initOrder_) {
        if (!purec.count(v) || done.count(v)) continue;
        std::set<int64_t> rs{v};
        bool changed = true;
        while (changed) {
            changed = false;
            const std::set<int64_t> now = rs;
            for (int64_t r : now)
                for (int64_t sc : nodes_.at(r).succ)
                    if (purec.count(sc) && !rs.count(sc) && sc != v) {
                        bool all = true;
                        for (int64_t p : preds[sc]) all &= rs.count(p) != 0;
                        if (all) {
                            rs.insert(sc);
                            changed = true;
                        }
                    }
        }
        if (rs.size() < 2) continue;
        std::vector<std::pair<E, int64_t>> res;
        if (!Formulas(v, rs, &res)) continue;
        done.insert(rs.begin(), rs.end());
        SNode& n = nodes_.at(v);
        n.branches = res;
        n.succ.clear();
        for (auto& [c, t] : res) n.succ.push_back(t);
        for (int64_t r : rs)
            if (r != v) nodes_.erase(r);
        stats_["conditions_combined"] += static_cast<int64_t>(rs.size());
    }
}

bool Structurer::Formulas(int64_t root, const std::set<int64_t>& rs, std::vector<std::pair<E, int64_t>>* out) {
    std::vector<E> atoms;
    std::map<E, int> index;
    for (int64_t r : rs) {   // sorted
        auto [key, neg] = AtomKey(StripT(nodes_.at(r).branches[0].first));
        if (!index.count(key)) {
            index[key] = static_cast<int>(atoms.size());
            atoms.push_back(key);
        }
    }
    if (atoms.size() > 12) return false;
    std::vector<int64_t> exitsOrder;
    std::map<int64_t, std::vector<int64_t>> table;
    const int n = static_cast<int>(atoms.size());
    for (int64_t bits = 0; bits < (1ll << n); ++bits) {
        std::vector<bool> val(n);
        for (int k = 0; k < n; ++k) val[k] = ((bits >> k) & 1) == 1;
        if (!Consistent(atoms, val)) continue;
        int64_t v = root;
        int steps = 0;
        while (rs.count(v) && steps < 64) {
            auto [key, neg] = AtomKey(StripT(nodes_.at(v).branches[0].first));
            const bool truth = val[index[key]] != neg;
            v = truth ? nodes_.at(v).branches[0].second : nodes_.at(v).branches[1].second;
            ++steps;
        }
        if (std::find(exitsOrder.begin(), exitsOrder.end(), v) == exitsOrder.end()) exitsOrder.push_back(v);
        table[v].push_back(bits);
    }
    std::set<int64_t> valid;
    for (auto& [e, vs] : table) valid.insert(vs.begin(), vs.end());
    std::vector<int64_t> dontcare;
    for (int64_t b = 0; b < (1ll << n); ++b)
        if (!valid.count(b)) dontcare.push_back(b);
    std::vector<int64_t> remaining = exitsOrder;
    std::vector<int64_t> handled;
    out->clear();
    while (remaining.size() > 1) {
        bool have = false;
        double bestCost = 0;
        int64_t bestE = 0;
        std::vector<Term> bestTerms;
        for (int64_t e : remaining) {
            std::vector<int64_t> dc = dontcare;
            for (int64_t h : handled) dc.insert(dc.end(), table[h].begin(), table[h].end());
            std::vector<Term> terms = Minimise(table[e], dc, n);
            double cost = 0.5 * static_cast<double>(terms.size());
            int64_t lens = 0;
            for (const Term& t : terms) lens += static_cast<int64_t>(t.size());
            cost = static_cast<double>(lens) + cost;
            if (!have || cost < bestCost - 1e-9) {
                have = true;
                bestCost = cost;
                bestE = e;
                bestTerms = terms;
            }
        }
        out->push_back({FormulaExpr(bestTerms, atoms), bestE});
        handled.push_back(bestE);
        remaining.erase(std::find(remaining.begin(), remaining.end(), bestE));
    }
    out->push_back({nullptr, remaining[0]});
    return true;
}

void Structurer::IPdom() {
    std::map<int64_t, std::vector<int64_t>> succ;
    for (int64_t v : nodes_.order) {
        std::vector<int64_t> ss;
        for (int64_t sc : nodes_.at(v).succ)
            if (!retreat_.count({v, sc})) ss.push_back(sc);
        if (ss.empty()) ss.push_back(EXIT);
        succ[v] = ss;
    }
    std::set<int64_t> allv(nodes_.order.begin(), nodes_.order.end());
    allv.insert(EXIT);
    std::map<int64_t, std::set<int64_t>> pdom;
    for (int64_t v : allv) pdom[v] = allv;
    pdom[EXIT] = {EXIT};
    bool changed = true;
    while (changed) {
        changed = false;
        for (int64_t v : nodes_.order) {
            std::set<int64_t> nw;
            bool first = true;
            for (int64_t sc : succ[v]) {
                if (first) {
                    nw = pdom[sc];
                    first = false;
                } else {
                    std::set<int64_t> x;
                    for (int64_t y : nw)
                        if (pdom[sc].count(y)) x.insert(y);
                    nw = x;
                }
            }
            nw.insert(v);
            if (nw != pdom[v]) {
                pdom[v] = nw;
                changed = true;
            }
        }
    }
    for (int64_t v : nodes_.order) {
        std::set<int64_t> cands = pdom[v];
        cands.erase(v);
        bool have = false;
        int64_t best = 0;
        for (int64_t c : cands) {   // sorted
            bool all = true;
            for (int64_t o : cands) all &= o == c || pdom[c].count(o) != 0;
            if (all) {
                best = c;
                have = true;
                break;
            }
        }
        ipdom_[v] = (!have || best == EXIT) ? std::make_pair(false, int64_t(0)) : std::make_pair(true, best);
    }
}

const std::set<int64_t>& Structurer::Reachable(int64_t v) {
    auto it = reach_.find(v);
    if (it != reach_.end()) return it->second;
    std::set<int64_t> seen;
    std::vector<int64_t> todo{v};
    while (!todo.empty()) {
        const int64_t x = todo.back();
        todo.pop_back();
        if (seen.count(x)) continue;
        seen.insert(x);
        for (int64_t sc : nodes_.at(x).succ)
            if (!retreat_.count({x, sc})) todo.push_back(sc);
    }
    return reach_[v] = seen;
}

int Structurer::Size(int64_t v, int limit) {
    int n = 0;
    for (int64_t x : Reachable(v)) {   // sorted
        n += static_cast<int>(nodes_.at(x).stmts.size()) + 1;
        if (n > limit) break;
    }
    return n;
}

bool Structurer::LoopFollow(int64_t h, int64_t* out) {
    const std::set<int64_t>& body = loops_.at(h);
    std::vector<int64_t> exits;
    for (int64_t v : body)   // sorted
        for (int64_t sc : nodes_.at(v).succ)
            if (!body.count(sc)) exits.push_back(sc);
    if (exits.empty()) return false;
    auto ip = ipdom_.find(h);
    if (ip != ipdom_.end() && ip->second.first && std::find(exits.begin(), exits.end(), ip->second.second) != exits.end()) {
        *out = ip->second.second;
        return true;
    }
    const std::set<int64_t> uniq(exits.begin(), exits.end());
    int64_t best = 0;
    long bestCount = -1;
    for (int64_t e : uniq) {   // max(sorted(set(exits)), key=exits.count): the first of the most frequent
        const long c = static_cast<long>(std::count(exits.begin(), exits.end(), e));
        if (c > bestCount) {
            bestCount = c;
            best = e;
        }
    }
    *out = best;
    return true;
}

bool Structurer::SmallTail(int64_t v) {
    int n = 0, steps = 0;
    int64_t x = v;
    bool hasX = true;
    while (hasX && steps < 4) {
        SNode* node = nodes_.get(x);
        if (!node) return false;
        n += static_cast<int>(node->stmts.size());
        if (n > 3) return false;
        if (node->kind == kd::ret || node->kind == kd::tail) return true;
        if (node->kind != kd::fall || node->succ.empty()) return false;
        x = node->succ[0];
        ++steps;
    }
    return false;
}

std::pair<std::pair<bool, int64_t>, std::pair<bool, int64_t>> Structurer::ChooseJoin(int64_t v,
                                                                                     const std::vector<int64_t>& targets,
                                                                                     const Loop* loop) {
    auto ipv = ipdom_.find(v);
    bool hasJ = ipv != ipdom_.end() && ipv->second.first;
    int64_t J = hasJ ? ipv->second.second : 0;
    if (loop && hasJ && !InLoop(loop, J)) hasJ = false;
    const std::vector<int64_t> ts = Unique(targets);
    std::vector<int64_t> coverOrder;
    std::map<int64_t, int> cover;
    for (int64_t t : ts)
        for (int64_t x : Reachable(t)) {   // sorted
            if (x != v && !(hasJ && x == J) && InDom(x, v) && (!loop || InLoop(loop, x))) {
                if (!cover.count(x)) coverOrder.push_back(x);
                cover[x] += 1;
            }
        }
    std::map<int64_t, int64_t> pos;
    for (size_t k = 0; k < order_.size(); ++k) pos[order_[k]] = static_cast<int64_t>(k);
    auto posOf = [&](int64_t x) -> int64_t {
        auto it = pos.find(x);
        return it == pos.end() ? 0 : it->second;
    };
    bool haveBest = false;
    int64_t best = 0;
    for (int64_t x : coverOrder) {
        const int c = cover[x];
        if (c < 2 || SmallTail(x)) continue;
        if (hasJ && !Reachable(x).count(J)) continue;
        if (!haveBest || std::make_pair(c, -posOf(x)) > std::make_pair(cover[best], -posOf(best))) {
            best = x;
            haveBest = true;
        }
    }
    if (hasJ && !SmallTail(J)) {
        if (!haveBest) return {{true, J}, {false, 0}};
        if (!ReachesAvoiding(v, J, best)) return {{true, best}, {false, 0}};
        return {{true, best}, {true, J}};
    }
    if (haveBest) return {{true, best}, {false, 0}};
    return {{hasJ, J}, {false, 0}};
}

bool Structurer::ReachesAvoiding(int64_t src, int64_t dst, int64_t avoid) {
    std::set<int64_t> seen;
    std::vector<int64_t> todo;
    for (int64_t sc : nodes_.at(src).succ)
        if (!retreat_.count({src, sc})) todo.push_back(sc);
    while (!todo.empty()) {
        const int64_t x = todo.back();
        todo.pop_back();
        if (x == dst) return true;
        if (seen.count(x) || x == avoid) continue;
        seen.insert(x);
        for (int64_t sc : nodes_.at(x).succ)
            if (!retreat_.count({x, sc})) todo.push_back(sc);
    }
    return false;
}

Body Structurer::Jump(int64_t t, const Loop* loop, bool hasStop, int64_t stop) {
    if (loop) {
        const int64_t h = std::get<0>(*loop);
        if (t == h) {
            Ast c = Ast::Mk(Ast::Continue);
            c.label = h;
            return {c};
        }
        if (!InLoop(loop, t)) {
            if (std::get<2>(*loop) && t == std::get<3>(*loop)) return {Ast::Mk(Ast::Break)};
            return DupOrGoto(t, false, 0, nullptr);
        }
    }
    return DupOrGoto(t, hasStop, stop, loop);
}

Body Structurer::DupOrGoto(int64_t t, bool hasStop, int64_t stop, const Loop* loop) {
    auto gotoT = [&]() {
        labels.insert(t);
        stats_["gotos"] += 1;
        Ast g = Ast::Mk(Ast::Goto);
        g.label = t;
        return Body{g};
    };
    if (std::find(active_.begin(), active_.end(), t) != active_.end() || loops_.count(t)) return gotoT();
    const int cost = Size(t);
    if (SmallTail(t) || dupUsed_ + cost <= 400 || (noGoto.count(t) && cost <= 60)) {
        dupUsed_ += cost;
        stats_["duplicated_statements"] += cost;
        return Region(true, t, hasStop, stop, loop, true);
    }
    return gotoT();
}

Body Structurer::Region(bool hasV, int64_t v, bool hasStop, int64_t stop, const Loop* loop, bool dup) {
    Body out;
    while (hasV && !(hasStop && v == stop)) {
        Sym hit = nullptr;
        for (auto it = skipStack_.rbegin(); it != skipStack_.rend(); ++it)
            if (it->first == v) {
                hit = it->second;
                break;
            }
        if (hit) {
            Ast a = Ast::Mk(Ast::SetFlag);
            a.flag = hit;
            a.flagValue = true;
            out.push_back(a);
            break;
        }
        if (loop && v == std::get<0>(*loop) && (emitted_.count(v) || dup)) {
            Ast c = Ast::Mk(Ast::Continue);
            c.label = v;
            out.push_back(c);
            break;
        }
        if (loop && !InLoop(loop, v)) {
            Body j = Jump(v, loop, hasStop, stop);
            out.insert(out.end(), j.begin(), j.end());
            break;
        }
        if (emitted_.count(v) && !dup) {
            Body j = DupOrGoto(v, hasStop, stop, loop);
            out.insert(out.end(), j.begin(), j.end());
            break;
        }
        if (std::find(active_.begin(), active_.end(), v) != active_.end()) {
            labels.insert(v);
            stats_["gotos"] += 1;
            Ast g = Ast::Mk(Ast::Goto);
            g.label = v;
            out.push_back(g);
            break;
        }
        if (loops_.count(v) && (!loop || std::get<0>(*loop) != v)) {
            const std::set<int64_t>& body = loops_.at(v);
            int64_t follow = 0;
            const bool hasFollow = LoopFollow(v, &follow);
            const Loop inner{v, &body, hasFollow, follow};
            active_.push_back(v);
            Body lbody = EmitNode(v, false, 0, &inner, dup);
            if (hasNext_) {
                Body r = Region(true, next_, false, 0, &inner, dup);
                lbody.insert(lbody.end(), r.begin(), r.end());
            }
            active_.pop_back();
            Ast w = Ast::Mk(Ast::While);
            w.els = lbody;
            out.push_back(w);
            hasV = hasFollow;
            v = follow;
            continue;
        }
        active_.push_back(v);
        Body e = EmitNode(v, hasStop, stop, loop, dup);
        out.insert(out.end(), e.begin(), e.end());
        active_.pop_back();
        hasV = hasNext_;
        v = next_;
    }
    return out;
}

Body Structurer::EmitNode(int64_t v, bool hasStop, int64_t stop, const Loop* loop, bool dup) {
    SNode& n = nodes_.at(v);
    if (!dup) emitted_.insert(v);
    Body out;
    if (labelsFinal.count(v) && !dup) {
        Ast l = Ast::Mk(Ast::Label);
        l.label = v;
        out.push_back(l);
    }
    for (Stmt* st : n.stmts) {
        Ast a = Ast::Mk(Ast::S);
        a.st = st;
        out.push_back(a);
    }
    hasNext_ = false;
    if (n.kind == kd::ret || n.kind == kd::tail) {
        Ast r = Ast::Mk(Ast::Return);
        r.cond = n.term->e;
        r.rkind = n.term->kind;
        out.push_back(r);
        return out;
    }
    if (n.kind == kd::fall) {
        if (n.succ.empty()) return out;
        const int64_t t = n.succ[0];
        if (loop && (t == std::get<0>(*loop) || !InLoop(loop, t))) {
            Body j = Jump(t, loop, hasStop, stop);
            out.insert(out.end(), j.begin(), j.end());
            return out;
        }
        hasNext_ = true;
        next_ = t;
        return out;
    }
    std::vector<int64_t> targets;
    if (n.kind == kd::branch) {
        for (auto& br : n.branches) targets.push_back(br.second);
    } else {
        for (auto& [k, t] : n.table) targets.push_back(t);
        if (n.term->table->hasDefault && n.term->table->def) targets.push_back(n.term->table->def);
    }
    auto [joinP, skipP] = ChooseJoin(v, targets, loop);
    bool hasJoin = joinP.first, hasSkip = skipP.first;
    int64_t join = joinP.second, skip = skipP.second;
    if (hasStop && hasJoin && join != stop && Reachable(v).count(stop) &&
        (!Reachable(v).count(join) || Reachable(stop).count(join))) {
        join = stop;
        hasSkip = false;
    }
    Sym flag = nullptr;
    if (hasSkip) {
        ++flagNo_;
        char buf[32];
        if (flagNo_ > 1) std::snprintf(buf, sizeof buf, "skip%d", flagNo_);
        else std::snprintf(buf, sizeof buf, "skip");
        flag = Y(buf);
        flags.push_back(flag);
        Ast a = Ast::Mk(Ast::SetFlag);
        a.flag = flag;
        a.flagValue = false;
        out.push_back(a);
        skipStack_.push_back({skip, flag});
    }
    if (n.kind == kd::branch) {
        std::vector<std::pair<E, Body>> arms;
        for (auto& br : n.branches) arms.push_back({br.first, Arm(true, br.second, hasJoin, join, loop, hasSkip, skip, flag)});
        if (flag) skipStack_.pop_back();
        Ast iff = Ast::Mk(Ast::If);
        iff.arms.assign(arms.begin(), arms.end() - 1);
        iff.els = arms.back().second;
        out.push_back(iff);
        hasNext_ = hasJoin;
        next_ = join;
        if (flag) {
            Body g = Guarded(flag, join, skip, loop);
            out.insert(out.end(), g.begin(), g.end());
        }
        return out;
    }
    std::vector<int64_t> groupOrder;
    std::map<int64_t, std::vector<int64_t>> groups;
    for (auto& [val, t] : n.table) {   // sorted(n.table.items())
        if (!groups.count(t)) groupOrder.push_back(t);
        groups[t].push_back(val);
    }
    std::stable_sort(groupOrder.begin(), groupOrder.end(),
                     [&](int64_t a, int64_t b) { return groups[a].front() < groups[b].front(); });
    const bool hasDefault = n.term->table->hasDefault;
    const int64_t def = n.term->table->def;
    Ast sw = Ast::Mk(Ast::Switch);
    for (int64_t t : groupOrder) {
        if (hasDefault && t == def) continue;
        sw.cases.push_back({groups[t], Arm(true, t, hasJoin, join, loop, hasSkip, skip, flag)});
    }
    if (hasDefault) sw.els = Arm(true, def, hasJoin, join, loop, hasSkip, skip, flag);
    if (flag) skipStack_.pop_back();
    sw.cond = n.term->e;
    out.push_back(sw);
    hasNext_ = hasJoin;
    next_ = join;
    if (flag) {
        Body g = Guarded(flag, join, skip, loop);
        out.insert(out.end(), g.begin(), g.end());
    }
    return out;
}

Body Structurer::Guarded(Sym flag, int64_t m, int64_t j, const Loop* loop) {
    Body body = Region(true, m, true, j, loop);
    hasNext_ = true;
    next_ = j;
    Ast iff = Ast::Mk(Ast::If);
    iff.arms.push_back({T(s::not_, T(s::flag, flag)), body});
    return {iff};
}

Body Structurer::Arm(bool hasT, int64_t t, bool hasJoin, int64_t join, const Loop* loop, bool hasSkip, int64_t skip,
                     Sym flag) {
    (void)hasSkip;
    (void)skip;
    (void)flag;
    if (!hasT || (hasJoin && t == join)) return {};
    if (loop && (t == std::get<0>(*loop) || !InLoop(loop, t))) return Jump(t, loop, hasJoin, join);
    const bool sh = hasNext_;
    const int64_t sv = next_;
    Body r = Region(true, t, hasJoin, join, loop);
    hasNext_ = sh;
    next_ = sv;
    return r;
}

Body Structurer::Run() {
    flags.clear();
    flagNo_ = 0;
    skipStack_.clear();
    emitted_.clear();
    dupUsed_ = 0;
    active_.clear();
    reach_.clear();
    return Tidy(Region(true, f_.va, false, 0, nullptr));
}

}  // namespace

std::set<int64_t> InvisibleGotos(const Body& body) {
    std::set<int64_t> bad;
    std::function<void(const Body&, std::set<int64_t>)> walk = [&](const Body& block, std::set<int64_t> vis) {
        for (const Ast& n : block)
            if (n.k == Ast::Label) vis.insert(n.label);
        for (const Ast& n : block) {
            if (n.k == Ast::Goto && !vis.count(n.label)) bad.insert(n.label);
            else if (n.k == Ast::If) {
                for (auto& [c, b] : n.arms) walk(b, vis);
                if (!n.els.empty()) walk(n.els, vis);
            } else if (n.k == Ast::While || n.k == Ast::Repeat) {
                walk(n.els, vis);
            } else if (n.k == Ast::Switch) {
                for (auto& [v, b] : n.cases) walk(b, vis);
                if (!n.els.empty()) walk(n.els, vis);
            }
        }
    };
    walk(body, {});
    return bad;
}

StructResult Structure(Translator& tr, Func& f, IR& ir, std::map<std::string, int64_t>& stats,
                       const std::set<int64_t>& noGoto, const std::set<int64_t>& labelsFinal) {
    Structurer st(tr, f, ir, stats);
    st.Build();
    st.noGoto = noGoto;
    st.labelsFinal = labelsFinal;
    StructResult r;
    r.ast = st.Run();
    r.labels = st.labels;
    r.flags = st.flags;
    return r;
}

}  // namespace pse::m2l::rd
