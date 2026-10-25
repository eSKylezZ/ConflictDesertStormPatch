// mods.cpp's lists of structs for dsfix.loadout: dsfix.dll hands them over as text rows (features::ModGearRows ...,
// host_iface.cpp - C++ objects don't cross DLLs here, every DLL has its own runtime) and these rebuild them, again
// whenever the mods are reloaded (ModsGeneration).
#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "features/features.h"

namespace {
std::vector<std::vector<std::string>> Rows(const std::string& text) {
    std::vector<std::vector<std::string>> out;
    for (size_t pos = 0; pos < text.size();) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> fields;
        for (size_t f = pos; f <= end;) {
            size_t tab = text.find('\t', f);
            if (tab == std::string::npos || tab > end) tab = end;
            fields.push_back(text.substr(f, tab - f));
            f = tab + 1;
        }
        out.push_back(std::move(fields));
        pos = end + 1;
    }
    return out;
}

template <class T, class Make>
const std::vector<T>& Cached(std::vector<T>& list, int& generation, std::string (*rows)(), Make make) {
    const int now = features::ModsGeneration();
    if (generation != now) {
        generation = now;
        list.clear();
        for (const auto& r : Rows(rows())) make(list, r);
    }
    return list;
}
}  // namespace

const std::vector<features::ModGearPiece>& features::ModGear() {
    static std::vector<ModGearPiece> list;
    static int generation = -1;
    return Cached(list, generation, &ModGearRows, [](std::vector<ModGearPiece>& l, const std::vector<std::string>& r) {
        if (r.size() >= 3) l.push_back({r[0], atoi(r[1].c_str()), r[2]});
    });
}

const std::vector<features::ModPouchSet>& features::ModPouchSets() {
    static std::vector<ModPouchSet> list;
    static int generation = -1;
    return Cached(list, generation, &ModPouchSetRows, [](std::vector<ModPouchSet>& l, const std::vector<std::string>& r) {
        if (r.size() < 8) return;
        ModPouchSet p;
        p.name = r[0];
        for (int i = 0; i < 7; ++i) p.model[i] = r[1 + i];
        l.push_back(std::move(p));
    });
}

const std::vector<features::ModVehicle>& features::ModVehicles() {
    static std::vector<ModVehicle> list;
    static int generation = -1;
    return Cached(list, generation, &ModVehicleRows, [](std::vector<ModVehicle>& l, const std::vector<std::string>& r) {
        if (r.size() >= 4) l.push_back({r[0], r[1], r[2], r[3]});
    });
}

const std::vector<features::SkinVariant>& features::SkinVariants() {
    static std::vector<SkinVariant> list;
    static int generation = -1;
    return Cached(list, generation, &SkinVariantRows, [](std::vector<SkinVariant>& l, const std::vector<std::string>& r) {
        if (r.size() >= 4) l.push_back({atoi(r[0].c_str()), atoi(r[1].c_str()), r[2], r[3]});
    });
}
