#pragma once
// mission2lua_readable (C++ port): _map_expr (bottom-up rewrite; included by m2r.h)
namespace pse::m2l::rd {

template <class F>
E MapExpr(E e, F&& fn) {
    if (!e) return e;
    E e2;
    if (e->t == s::call) {
        std::vector<E> args;
        for (const V& a : CallArgs(e)) args.push_back(MapExpr(a.e, fn));
        e2 = MkCall(MapExpr(CallTarget(e), fn), args, CallVa(e), CallAt(e), CallRet(e));
    } else {
        std::vector<V> items = e->v;
        bool changed = false;
        for (V& x : items)
            if (x.k == VK::Ex) {
                E y = MapExpr(x.e, fn);
                if (y != x.e) {
                    x = V(y);
                    changed = true;
                }
            }
        e2 = changed ? Rebuild(e, items) : e;
    }
    E r = fn(e2);
    return r ? r : e2;
}

}  // namespace pse::m2l::rd
