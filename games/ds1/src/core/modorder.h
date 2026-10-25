// Mod priority list (load order): the order the Mods folders are loaded in - when two mods ship a file of the same
// name, the one loaded LAST wins. Stored as the settings value ModOrder = the folder names without the '_' off-prefix,
// joined by '|' (a character Windows doesn't allow in folder names), so switching a mod off and on keeps its place.
// Header-only and pure (no Windows, no game): mods.cpp / modsmenu.cpp / the online plugin's mod sync use it, and
// pse/tests/test_modorder.cpp tests it.
//
// Rules (docs/internal/modding-parity.md A.3):
//   - first start (no list yet) = the folder names alphabetically, as before the list existed;
//   - a folder not on the list yet (a new mod) goes to the END - loaded last, so it wins;
//   - names on the list whose folder is gone drop off;
//   - names compare case-insensitively; the folder's own spelling is kept.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace modorder {

inline bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

inline bool LessName(const std::string& a, const std::string& b) {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
    });
}

// "_Name" / "Name" -> "Name" (the off-prefix is the switch, not part of the name)
inline std::string Bare(const std::string& folder) {
    size_t i = 0;
    while (i < folder.size() && folder[i] == '_') ++i;
    return folder.substr(i);
}

inline std::vector<std::string> Split(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        const size_t bar = value.find('|', start);
        const std::string name = value.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
        if (!name.empty()) out.push_back(name);
        if (bar == std::string::npos) break;
        start = bar + 1;
    }
    return out;
}

inline std::string Join(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) out += (out.empty() ? "" : "|") + n;
    return out;
}

// The load order for the mods on disk (`present` = bare names, any order, on and off): the saved list's names that
// still exist, in its order, then the others alphabetically (new mods at the end; no list yet = all alphabetical).
inline std::vector<std::string> Resolve(const std::vector<std::string>& present, const std::vector<std::string>& saved) {
    std::vector<std::string> out, rest;
    for (const std::string& s : saved)
        for (const std::string& p : present)
            if (SameName(s, p) && std::none_of(out.begin(), out.end(), [&](const std::string& o) { return SameName(o, p); })) {
                out.push_back(p);
                break;
            }
    for (const std::string& p : present)
        if (std::none_of(out.begin(), out.end(), [&](const std::string& o) { return SameName(o, p); }) &&
            std::none_of(rest.begin(), rest.end(), [&](const std::string& o) { return SameName(o, p); }))
            rest.push_back(p);
    std::sort(rest.begin(), rest.end(), LessName);
    out.insert(out.end(), rest.begin(), rest.end());
    return out;
}

// Moves `name` one place earlier (delta -1) or later (+1); false when it can't move (ends, not on the list).
inline bool Move(std::vector<std::string>& order, const std::string& name, int delta) {
    for (size_t i = 0; i < order.size(); ++i) {
        if (!SameName(order[i], name)) continue;
        const long j = static_cast<long>(i) + delta;
        if (j < 0 || j >= static_cast<long>(order.size())) return false;
        std::swap(order[i], order[static_cast<size_t>(j)]);
        return true;
    }
    return false;
}

// `order` rearranged so the mods in `wanted` (another PC's load order) come in that order, in front, followed by the
// rest in their own order - what a joiner takes over from the host so both load the same files. A wanted mod `order`
// doesn't have yet (just downloaded) takes its place there too.
inline std::vector<std::string> Adopt(const std::vector<std::string>& order, const std::vector<std::string>& wanted) {
    std::vector<std::string> out;
    for (const std::string& w : wanted) {
        auto it = std::find_if(order.begin(), order.end(), [&](const std::string& o) { return SameName(o, w); });
        const std::string name = it != order.end() ? *it : w;
        if (std::none_of(out.begin(), out.end(), [&](const std::string& x) { return SameName(x, name); }))
            out.push_back(name);
    }
    for (const std::string& o : order)
        if (std::none_of(out.begin(), out.end(), [&](const std::string& x) { return SameName(x, o); })) out.push_back(o);
    return out;
}

// True when the mods both lists have come in the same relative order (the ones only one side has are ignored).
inline bool SameRelativeOrder(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::vector<std::string> x, y;
    for (const std::string& n : a)
        if (std::any_of(b.begin(), b.end(), [&](const std::string& m) { return SameName(m, n); })) x.push_back(n);
    for (const std::string& n : b)
        if (std::any_of(a.begin(), a.end(), [&](const std::string& m) { return SameName(m, n); })) y.push_back(n);
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i)
        if (!SameName(x[i], y[i])) return false;
    return true;
}

}  // namespace modorder
