// dsfix.mods: the mod data other DLLs need, in a form that crosses DLLs as plain C (static CRT per DLL: no C++ objects)
// - text rows for the struct lists (dsfix.loadout's glue.cpp and dsfix.dll's console turn them back), the vehicle
// handling list as count + item. Until 2026-10-25 dsfix.dll's host_iface.cpp built these rows itself.
#include <windows.h>

#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "features/features.h"

namespace {
void Row(std::string& out, std::initializer_list<std::string> fields) {
    bool first = true;
    for (const std::string& f : fields) {
        if (!first) out += '\t';
        out += f;
        first = false;
    }
    out += '\n';
}
}  // namespace

// The system's dinput8.dll for core/gamepad.cpp (the asset store's first-boot window reads the pads; the game's
// dinput8.dll is the PSE shim) - as dsfix.dll's RealDInput8 and dsfix.coop's glue.cpp.
FARPROC RealDInput8(const char* name) {
    static HMODULE real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        const UINT n = GetSystemDirectoryA(path, MAX_PATH);   // SysWOW64 for this 32-bit process
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        strcat_s(path, "\\dinput8.dll");
        real = LoadLibraryA(path);
        if (!real) return nullptr;
    }
    return GetProcAddress(real, name);
}

// For dsfix.loadout (its glue.cpp turns the rows back into the structs).
void features::BorrowVehicleDataList(const char* level, const std::string& units) {
    std::vector<std::string> list;
    for (size_t pos = 0; pos <= units.size();) {
        size_t end = units.find('\n', pos);
        if (end == std::string::npos) end = units.size();
        if (end > pos) list.push_back(units.substr(pos, end - pos));
        pos = end + 1;
    }
    BorrowVehicleData(level, list);
}
std::string features::ModGearRows() {
    std::string out;
    for (const ModGearPiece& g : ModGear()) Row(out, {g.model, std::to_string(g.kind), g.name});
    return out;
}
std::string features::ModPouchSetRows() {
    std::string out;
    for (const ModPouchSet& p : ModPouchSets())
        Row(out, {p.name, p.model[0], p.model[1], p.model[2], p.model[3], p.model[4], p.model[5], p.model[6]});
    return out;
}
std::string features::ModVehicleRows() {
    std::string out;
    for (const ModVehicle& v : ModVehicles()) Row(out, {v.id, v.name, v.base, v.faction});
    return out;
}
std::string features::SkinVariantRows() {
    std::string out;
    for (const SkinVariant& v : SkinVariants()) Row(out, {std::to_string(v.uniform), std::to_string(v.role), v.texture, v.name});
    return out;
}

// For dsfix.dll's memory.cpp (the Global Heap is sized for the largest mod file).
uint32_t features::LargestModFileSize(std::string& name) { return LargestModFile(&name); }

// For dsfix.dll's developer console ("levels"): name TAB mod TAB 1 when built from a .dllpatch, per line.
std::string features::ModLevelRows() {
    std::string out;
    for (const ModLevel& m : ModLevels()) Row(out, {m.name, m.mod, m.built ? "1" : "0"});
    return out;
}

// For dsfix.dll's dsfix.services (VehicleHandlingCount / At, read by the plugin dsfix.handling).
uint32_t features::ModVehicleHandlingCount() { return static_cast<uint32_t>(ModVehicleHandling().size()); }
bool features::ModVehicleHandlingAt(uint32_t i, std::string& model, float& grip, float& spin) {
    const auto& all = ModVehicleHandling();
    if (i >= all.size()) return false;
    model = all[i].model, grip = all[i].grip, spin = all[i].spin;
    return true;
}
