// Vehicle swaps (loadout screen, MISSION tab): the squad's vehicles in a mission - the ones the game marks as usable by
// the player - can be exchanged for another ground vehicle with enough seats (any faction's, or one a mod adds with a
// .vehicle file: a skin of a game vehicle or a new model, see mods.cpp BuildVehicles).
//
// A vehicle is built while the level loads, from its scene record: the scene loader hands every HLOI record to the
// object manager's NewHLO_callback (0x41B750, hooked in loadout.cpp), which looks up the definition by the record's unit
// name (+0xAB, stat type +0xD0 = 2 for vehicles) and builds a vehicle (FUN_00437cd0) that also mounts its crew. So a
// swap is made there, before the object exists: unit name, model name (+0x1C) and the model the scene loader already
// loaded (+0x60, cleared = loaded by name) are rewritten, the crew's seats fitted to the new vehicle. Everything else
// (script id +0, position +4, HLOI flags +0x20F0C: 0x200000 usable by the player, 0x20000 cutscene actor) stays, so the
// mission script drives / checks the new vehicle under the old id.
// Crew list (the HLOI "kit" of a vehicle): 25 entries, unit name char[0x23] at +0x20AD0, seat int at +0x20E3C, crew
// soldier's script id int at +0x20EA0; FUN_00442300 skips a seat the vehicle doesn't have (that soldier stays outside).
//
// The loadout screen opens after the intro, with the vehicles built - so a changed choice is remembered (registry
// Vehicles<SP|Coop><Level> = "ORIGINAL=NEW;...") and the mission restarts with it (pause menu RESTART LEVEL path,
// console.cpp), the intro skipped and the screen opened on the MISSION tab again. Files the new vehicle needs that the
// level's archive lacks are copied from the other levels' archives first (mods.cpp BorrowVehicleData).
// Saves keep the swaps they were made with ("V|ORIGINAL|NEW" lines in the .dsfix sidecar, loadout.cpp): a loaded save
// gets the same vehicles whatever is chosen now (its saved object states belong to them).
// Off: online (every PC would need the same vehicles), Lua missions, linked levels (view only), featLoadout off.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "core/pse_saves_format.h"
#include "core/settings.h"
#include "features/features.h"
#include "loader.h"

namespace {
constexpr uint32_t kFindDefinition = 0x493A00;   // cdecl(name, stat type) -> definition
constexpr int kStatVehicle = 2;
constexpr uint32_t kSplitFlag = 0x606410;
constexpr uint32_t kFlagPlayerUsable = 0x200000, kFlagCutscene = 0x20000;
constexpr int kRecUnit = 0xAB, kRecUnitSize = 0x25, kRecModel = 0x1C, kRecModelSize = 0x44, kRecModelPtr = 0x60,
              kRecStatType = 0xD0, kRecFlags = 0x20F0C;
constexpr int kCrewNames = 0x20AD0, kCrewNameSize = 0x23, kCrewSeats = 0x20E3C, kCrewIds = 0x20EA0, kCrewMax = 25;
// The scene loaders keep the record on the stack (frames 0x221E4 / 0x22224, record at the bottom) and the object base
// ctor FUN_004c9a90 reads up to +0x210D8 (RINFO lights) - a 0x21000 copy made every preview's NewHLO read past its
// end (access violation, "no preview" for every choice); loadout.cpp's records had the same fix.
constexpr size_t kRecordSize = 0x221D0;
constexpr uint32_t kNewHlo = 0x41B750, kBindDefinition = 0x495EC0, kLoadPendingTextures = 0x54A3E0,
                   kTextureManager = 0x63C930, kObjectManager = 0x63C9A0, kScriptObject = 0x60F578;
constexpr uint32_t kVehicleVtable = 0x5D8A58, kHiddenFlag = 0x10000000;   // render instance +0xC (script API hide_object)
constexpr uint32_t kOutOfPlayFlag = 4;   // render instance +0xC: not drawn (hide_instance 0x608; also set by SetActive(0))
constexpr uint32_t kDismount = 0x42E850, kSeatPlace = 0x442300, kMount = 0x433F00, kDeactivate = 0x482410;

struct Slot {
    std::string unit;    // the level's own vehicle (unit name, the game's spelling)
    int crew = 0;        // most crew any of them carries
    int count = 0;       // vehicles of that unit
    std::string loaded;  // what this load built ("" = the original)
    std::string chosen;  // the screen's choice ("" = the original)
    std::vector<int> ids;              // their script ids
    std::vector<uint8_t> record;       // the first one's scene record (preview copies are made from it)
    std::map<std::string, uint8_t*> previews;   // upper-case unit -> preview object (nullptr = couldn't be made)
};
std::vector<Slot> g_slots;
std::map<std::string, std::string> g_swaps;   // upper-case original unit -> unit for this load
std::string g_level;                          // "Mission9.dll" being loaded / played
bool g_enabled = false;                       // this level may swap
bool g_reloading = false;                     // the restart after a change: intro skipped, screen on MISSION
bool g_openOnMission = false;
bool g_loadingSave = false;                   // the next level load is a saved game
std::map<std::string, std::string> g_saveSwaps;

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string LevelBase(const std::string& level) {
    const size_t dot = level.find('.');
    return dot == std::string::npos ? level : level.substr(0, dot);
}

std::string ChoiceValueName() {
    return std::string("Vehicles") + (*reinterpret_cast<int*>(kSplitFlag) ? "Coop" : "SP") + LevelBase(g_level);
}

// Settings key (settings::Key(), HKCU\Software\PSE\ds1\Plugins\dsfix).
std::string ReadChoice(const std::string& name) {
    std::string v;
    return settings::ReadString(name.c_str(), v) ? v : std::string();
}
void WriteChoice(const std::string& name, const std::string& value) { settings::WriteString(name.c_str(), value); }

std::map<std::string, std::string> ParseSwaps(const std::string& text) {   // "A=B;C=D"
    std::map<std::string, std::string> out;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find(';', a);
        if (b == std::string::npos) b = text.size();
        const std::string pair = text.substr(a, b - a);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos && eq > 0 && eq + 1 < pair.size()) out[Upper(pair.substr(0, eq))] = pair.substr(eq + 1);
        a = b + 1;
    }
    return out;
}

// ---- vehicle definitions (the game's tables, mods included) ----
struct Kind {
    bool ok = false;
    int seats = 0, type = -1;   // type: 0 wheeled, 1 tracked, 3 other ground, 5 jet, 8 animal, 9/10 helicopter, 11 tripod
};
int ConnectorSeats(const std::string& unit);   // (below, from the table rows)
Kind KindOfDefinition(const std::string& unit);
Kind KindOf(const std::string& unit) {
    Kind k = KindOfDefinition(unit);
    if (k.ok) k.seats = std::min(k.seats, ConnectorSeats(unit));
    return k;
}
Kind KindOfDefinition(const std::string& unit) {
    Kind k;
    __try {
        // the unit (+8 name) -> its vehicle definition +0x80: seats +0x40, stats +0x44 (VSTAT row: +0x24 type)
        const auto* u = reinterpret_cast<const uint8_t* (__cdecl*)(const char*, int)>(kFindDefinition)(unit.c_str(), kStatVehicle);
        const auto* def = u ? *reinterpret_cast<const uint8_t* const*>(u + 0x80) : nullptr;
        if (!def) return k;
        k.seats = def[0x40];
        const auto* stat = *reinterpret_cast<const uint8_t* const*>(def + 0x44);
        k.type = stat ? *reinterpret_cast<const int*>(stat + 0x24) : -1;
        k.ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        k.ok = false;
    }
    return k;
}
bool Ground(const Kind& k) { return k.ok && (k.type == 0 || k.type == 1 || k.type == 3) && k.seats > 0; }

struct Row {
    std::string unit, model, stat;
    int side = 0;    // VEHICLE.TXT column 6: < 0 US / UK, 0 civilian, > 0 Iraqi
    int seats = 0;   // seat connectors named (columns 7-14); the definition's own count comes from the handling row
                     // (VSTAT) and is wrong for vehicles nobody can sit in (SA-8: 5)
    uint32_t gunnerSeats = 0;   // bit k: seat k mans a gun
};
std::vector<Row> g_rows;
int g_rowsGeneration = -1;

std::vector<std::string> SplitCsv(const std::string& line) {
    std::vector<std::string> out;
    size_t a = 0;
    while (true) {
        size_t b = line.find(',', a);
        std::string cell = line.substr(a, b == std::string::npos ? std::string::npos : b - a);
        while (!cell.empty() && (cell.back() == '\r' || cell.back() == ' ' || cell.back() == '\t')) cell.pop_back();
        while (!cell.empty() && (cell.front() == ' ' || cell.front() == '\t')) cell.erase(0, 1);
        out.push_back(cell);
        if (b == std::string::npos) break;
        a = b + 1;
    }
    return out;
}

const std::vector<Row>& Rows() {
    if (g_rowsGeneration == features::ModsGeneration() && !g_rows.empty()) return g_rows;
    g_rowsGeneration = features::ModsGeneration();
    g_rows.clear();
    std::string text;
    if (!features::ReadGameTable("VEHICLE.TXT", text)) return g_rows;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        const std::vector<std::string> c = SplitCsv(text.substr(a, b - a));
        a = b + 1;
        if (c.size() < 6 || c[0].empty() || c[0][0] == '#') continue;
        int seats = 0;
        uint32_t gunners = 0;   // the first <gunner count> (column 26) seats on a CONNECTOR_GUNNERnn
        int guns = c.size() > 25 ? atoi(c[25].c_str()) : 8;
        for (size_t k = 6; k < 14 && k < c.size(); ++k) {
            seats += !c[k].empty() && c[k] != "-1" ? 1 : 0;
            std::string up = c[k];
            for (char& ch : up) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
            if (guns > 0 && up.find("GUNNER") != std::string::npos) gunners |= 1u << (k - 6), --guns;
        }
        g_rows.push_back({c[0], c[1], c[4], atoi(c[5].c_str()), seats, gunners});
    }
    return g_rows;
}
const Row* RowOf(const std::string& unit) {
    for (const Row& r : Rows())
        if (_stricmp(r.unit.c_str(), unit.c_str()) == 0) return &r;
    return nullptr;
}

int ConnectorSeats(const std::string& unit) {
    const Row* r = RowOf(unit);
    return r ? r->seats : 0;
}

const features::ModVehicle* ModVehicleOf(const std::string& unit) {
    for (const auto& m : features::ModVehicles())
        if (_stricmp(m.id.c_str(), unit.c_str()) == 0) return &m;
    return nullptr;
}

struct Name {
    const char* unit;
    const char* label;
};
const Name kNames[] = {
    {"US_VEH_Hum_Vee", "HUMVEE"},
    {"US_VEH_Hum_Vee_M60", "HUMVEE (M60)"},
    {"US_VEH_Hum_Vee_TOW", "HUMVEE (TOW)"},
    {"US_VEH_Hum_Vee_TOWMG", "HUMVEE (TOW + .50 CAL)"},
    {"UK_VEH_SAS_LandRover_Milan_LMG_SAW", "SAS LAND ROVER (MILAN + .50 CAL)"},
    {"UK_VEH_SAS_LandRover_MG", "SAS LAND ROVER (.50 CAL)"},     // rows whose own bodies were never built - usable
    {"UK_VEH_SAS_LandRover_Milan", "SAS LAND ROVER (MILAN)"},    // with a mod pointing them at the body above
    {"US_VEH_M2A1_Bradley", "M2A1 BRADLEY"},
    {"US_VEH_M1Abrams", "M1A1 ABRAMS"},
    {"US_VEH_Truck", "US ARMY TRUCK"},
    {"IR_VEH_T-62_BattleTank", "T-62"},
    {"IR_VEH_T-72_BattleTank", "T-72"},
    {"BMP1", "BMP-1"},
    {"IR_VEH_BMP-1_Command", "BMP-1 (COMMAND)"},
    {"IR_VEH_BMP-2", "BMP-2"},
    {"IR_VEH_ZSU_23_4_Aagun", "ZSU-23-4 SHILKA"},
    {"IR_VEH_Iraqi_Truck1", "URAL TRUCK"},
    {"IR_VEH_Iraqi_Truck2", "URAL TRUCK (OPEN)"},
    {"IR_VEH_FUEL_BOWSER", "FUEL BOWSER"},
    {"NU_VEH_Civ_Truck1", "CIVILIAN TRUCK"},
};

std::string Label(const std::string& unit) {
    if (const auto* m = ModVehicleOf(unit)) return m->name;
    for (const Name& n : kNames)
        if (_stricmp(n.unit, unit.c_str()) == 0) return n.label;
    std::string s = unit;   // "XX_VEH_Some_Name" -> "SOME NAME"
    const size_t veh = Upper(s).find("_VEH_");
    if (veh != std::string::npos) s = s.substr(veh + 5);
    for (char& c : s) c = c == '_' ? ' ' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string FactionOf(const std::string& unit) {
    if (const auto* m = ModVehicleOf(unit)) {
        if (!m->faction.empty()) return m->faction;
        return FactionOf(m->base);
    }
    const Row* r = RowOf(unit);
    if (!r) return {};
    const std::string u = Upper(unit);
    if (u.rfind("UK_", 0) == 0) return "UK";
    return r->side < 0 ? "US" : r->side > 0 ? "IRAQI" : "CIVILIAN";
}

// Vehicles the slot can take: its own first, then every ground vehicle with seats for its crew - one per look
// (model + handling + seats + guns: the SAS Land Rovers share one body and differ only in their guns; the game's
// level-specific copies such as IR_VEH_T-62_Mission7 / *_BradleyGC left out).
std::vector<std::string> Choices(const Slot& s) {
    std::vector<std::string> out = {s.unit};
    std::vector<std::string> seen;
    if (const Row* own = RowOf(s.unit))
        seen.push_back(Upper(own->model + "|" + own->stat) + "|" + std::to_string(KindOf(s.unit).seats) + "|" +
                       features::VehicleGuns(s.unit));
    for (const Row& r : Rows()) {
        const std::string u = Upper(r.unit);
        if (_stricmp(r.unit.c_str(), s.unit.c_str()) == 0) continue;
        const bool mod = ModVehicleOf(r.unit) != nullptr;
        if (!mod && (u.find("MISSION") != std::string::npos || u.find("BATTLETANK2") != std::string::npos ||
                     (u.size() > 2 && u.compare(u.size() - 2, 2, "GC") == 0)))
            continue;
        const Kind k = KindOf(r.unit);
        if (!Ground(k) || k.seats < std::max(1, s.crew)) continue;
        if (!features::VehicleDataComplete(r.unit)) continue;   // a table row without its models
        const std::string look =
            Upper(r.model + "|" + r.stat) + "|" + std::to_string(k.seats) + "|" + features::VehicleGuns(r.unit);
        if (!mod && std::find(seen.begin(), seen.end(), look) != seen.end()) continue;
        seen.push_back(look);
        out.push_back(r.unit);
    }
    // mod vehicles next to the vehicle they are based on
    std::stable_sort(out.begin() + 1, out.end(), [](const std::string& a, const std::string& b) {
        auto key = [](const std::string& u) {
            const auto* m = ModVehicleOf(u);
            const std::string base = m ? m->base : u;
            const Row* r = RowOf(base);
            const int side = r ? (r->side < 0 ? 0 : r->side > 0 ? 1 : 2) : 3;   // own side's first, then Iraqi, civilian
            return std::make_pair(side, Upper(base));
        };
        return key(a) < key(b);
    });
    return out;
}

bool Usable(const std::string& unit, const Slot& s) {
    if (_stricmp(unit.c_str(), s.unit.c_str()) == 0) return true;
    const Kind k = KindOf(unit);
    return Ground(k) && k.seats >= std::max(1, s.crew);
}

Slot* SlotOf(const std::string& unit) {
    for (Slot& s : g_slots)
        if (_stricmp(s.unit.c_str(), unit.c_str()) == 0) return &s;
    return nullptr;
}

std::string CurrentValue() {   // the screen's choices as "ORIGINAL=NEW;..."
    std::string v;
    for (const Slot& s : g_slots)
        if (!s.chosen.empty()) v += (v.empty() ? "" : ";") + s.unit + "=" + s.chosen;
    return v;
}

// The vehicle's model, loaded now if the level doesn't have it. Creating an object from a scene record loads its model
// asking for the model type the scene's own vehicle had (the loader FUN_004f8bc0 rejects a file whose type, model +8:
// 0 static / 4 hierarchy, differs - "Attempting to re-compile EVO file ... in release build" - and the object is then
// made without a render instance: crash at 0x4FB2CD). Loaded here with type -1 (any), as gear.cpp loads hats, and
// handed to the record as its loaded model (+0x60). now = mid-level: textures loaded at once.
constexpr uint32_t kEnvironment = 0x63C948, kFindModel = 0x4DD740, kLoadModel = 0x4F8BC0, kTexturesNow = 0x54A3B0,
                   kControlFp = 0x569956, kLoadPrecision = 0x63C8C8;
void* LoadModelNow(void* env, const char* name) {
    __try {
        return reinterpret_cast<void*(__thiscall*)(void*, int, const char*)>(kLoadModel)(env, -1, name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
void* VehicleModel(const std::string& model, bool now) {
    void* env = *reinterpret_cast<void**>(kEnvironment);
    if (!env || model.empty()) return nullptr;
    if (void* m = reinterpret_cast<void*(__thiscall*)(void*, const char*)>(kFindModel)(env, model.c_str())) return m;
    using ControlFp = uint32_t(__cdecl*)(uint32_t, uint32_t);
    const uint32_t fpu = reinterpret_cast<ControlFp>(kControlFp)(0, 0);
    reinterpret_cast<ControlFp>(kControlFp)(*reinterpret_cast<uint32_t*>(kLoadPrecision), 0xFFFFF);
    void* textures = *reinterpret_cast<void**>(kTextureManager);
    if (now && textures) reinterpret_cast<void(__thiscall*)(void*, int)>(kTexturesNow)(textures, 1);
    void* m = LoadModelNow(env, model.c_str());
    if (now && textures) reinterpret_cast<void(__thiscall*)(void*, int)>(kTexturesNow)(textures, 0);
    reinterpret_cast<ControlFp>(kControlFp)(fpu, 0xFFFFF);
    if (!m) dslog::Write("[fail] Vehicles: model %s didn't load", model.c_str());
    return m;
}

// Crew seats fitted to a vehicle with `seats` seats: a seat it doesn't have -> its first free one.
void FitCrew(uint8_t* record, int seats) {
    bool taken[64] = {};
    for (int i = 0; i < kCrewMax; ++i) {
        const int seat = *reinterpret_cast<int*>(record + kCrewSeats + i * 4);
        if (record[kCrewNames + i * kCrewNameSize] && seat >= 0 && seat < seats && seat < 64) taken[seat] = true;
    }
    for (int i = 0; i < kCrewMax; ++i) {
        if (!record[kCrewNames + i * kCrewNameSize]) continue;
        int& seat = *reinterpret_cast<int*>(record + kCrewSeats + i * 4);
        if (seat >= 0 && seat < seats) continue;
        int free = -1;
        for (int k = 0; k < seats && k < 64 && free < 0; ++k)
            if (!taken[k]) free = k;
        dslog::Write("Vehicles:  crew %s seat %d -> %d", reinterpret_cast<char*>(record + kCrewNames + i * kCrewNameSize),
                     seat, free);
        if (free < 0) continue;   // stays outside (FUN_00442300 refuses the seat)
        taken[free] = true;
        seat = free;
    }
}
}  // namespace

// Level init (scripthost.cpp's wrapper, before the level's archive and sound cache are read).
#ifndef DS_DIST
// Dev\VehicleTriLog = 1: the vehicle-vs-world triangle generator (0x4FE270, entry jump; pushed at 0x4FE1CC, stdcall(pair, a, b) -> triangles)
// logged (1500 lines per level: zone changes, empty queries, every 30th call): both models' owner roots, body user
// data, the zone searched, triangles found.
namespace {
using TriGenFn = int(__stdcall*)(void**, void*, void*);
int g_triCalls = 0;
DWORD g_triStart = 0;
__declspec(naked) int __stdcall TriGenOrig(void**, void*, void*) {
    __asm {
        sub esp, 0x24
        push ebx
        push ebp
        push 0x4FE275
        ret
    }
}
struct TriSeen {
    void* object;
    void* zone;
    int calls;
    float x, z;
};
TriSeen g_triSeen[16];
int __stdcall TriGenLog(void** pair, void* a, void* b) {
    const int n = TriGenOrig(pair, a, b);
    if (g_triCalls >= 4000 || *reinterpret_cast<float*>(&b) < 1.0f) return n;   // props (r 0.43 m) used the log up
    __try {
        // per body: the zone whose octree the query searched (0x4CCF80 thiscall(object) on the first model's object,
        // else the second's - as 0x4FE2D5 / 0x4FE2E8); logged on a zone change, and every 10th call while it moves
        auto ud0 = reinterpret_cast<void**(__stdcall*)(void*)>(0x575FC0);
        void** u0 = ud0(pair[0]);
        void** u1 = ud0(pair[1]);
        void* o = u0 && u0[0] ? u0[0] : (u1 ? u1[0] : nullptr);
        void* zone = o ? reinterpret_cast<void*(__thiscall*)(void*)>(0x4CCF80)(o) : nullptr;
        TriSeen* s = nullptr;
        for (TriSeen& e : g_triSeen)
            if (e.object == o || !e.object) {
                s = &e;
                break;
            }
        if (!s) return n;
        const float* c = static_cast<const float*>(a);
        if (!s->object) *s = {o, nullptr, 0, c[0], c[2]};
        const bool zoneChanged = zone != s->zone;
        const bool moved = std::fabs(c[0] - s->x) + std::fabs(c[2] - s->z) > 0.3f;
        if (!zoneChanged && (++s->calls % 10 || !moved)) return n;
        if (zoneChanged)
            dslog::Write("[dev] Vehicles: tri zone %p -> %p (object %p)", s->zone, zone, o);
        s->zone = zone;
        s->x = c[0], s->z = c[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ++g_triCalls;
    __try {
        auto ud = reinterpret_cast<void**(__stdcall*)(void*)>(0x575FC0);   // McdModel user data -> {render object, ...}
        auto body = reinterpret_cast<void*(__stdcall*)(void*)>(0x58BCE0);
        auto bodyUd = reinterpret_cast<int(__stdcall*)(void*)>(0x58E0B0);
        void* objs[2];
        uint8_t* roots[2];
        int bud[2];
        for (int k = 0; k < 2; ++k) {
            void** u = ud(pair[k]);
            objs[k] = u ? u[0] : nullptr;
            auto* r = static_cast<uint8_t*>(objs[k]);
            while (r && *reinterpret_cast<uint8_t**>(r + 0xBC)) r = *reinterpret_cast<uint8_t**>(r + 0xBC);
            roots[k] = r;
            void* bd = body(pair[k]);
            bud[k] = bd ? bodyUd(bd) : 0x7FFFFFFF;
        }
        const float* c = static_cast<const float*>(a);
        dslog::Write("[dev] Vehicles: tri %d models %p %p objs %p %p roots %p %p oct %p bodyud %d %d at %.2f %.2f %.2f r %.2f -> %d (hits %d)",
                     g_triCalls, pair[0], pair[1], objs[0], objs[1], roots[0], roots[1],
                     roots[0] ? *reinterpret_cast<void**>(roots[0] + 0x20C) : nullptr, bud[0], bud[1], c[0], c[1], c[2],
                     *reinterpret_cast<float*>(&b), n, *reinterpret_cast<int*>(0x640454));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}
void InstallTriLog() {
    patch::OwnerScope owner(pse_host::Find("dsfix.vehicles"));  // lazy hooks: owned by this feature's PSE plugin
    DWORD v = 0, size = sizeof v;
    static bool done = false;
    g_triCalls = 0;
    for (TriSeen& e : g_triSeen) e = {};
    g_triStart = GetTickCount();
    if (done || RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleTriLog", RRF_RT_REG_DWORD, nullptr, &v,
                             &size) != ERROR_SUCCESS || !v)
        return;
    const uint8_t orig[5] = {0x83, 0xEC, 0x24, 0x53, 0x55};   // entry: sub esp,0x24; push ebx; push ebp
    if (patch::Matches(0x4FE270, orig, 5) && patch::WriteJump(0x4FE270, &TriGenLog)) done = true;
    dslog::Write("[dev] Vehicles: triangle log %s", done ? "on" : "FAILED");
}
}  // namespace
#endif

void features::VehiclesLevelStart(const char* level) {
    // (NEW_GAME + the co-save revert callbacks ran before this: the game layer's level-init hook, OUTER at 0x40E05D.)
#ifndef DS_DIST
    InstallTriLog();
#endif
    g_level = level ? level : "";
    g_slots.clear();
    g_swaps.clear();
    const bool save = g_loadingSave;
    g_loadingSave = false;
    g_reloading = g_openOnMission;   // (for this load only, even if the screen never opens)
    g_openOnMission = false;
    const std::string base = Upper(LevelBase(g_level));
    // Desert Storm's own missions and every Lua mission (custom missions / campaigns, editor levels, converted
    // campaigns' Desert Storm II missions - user decision 2026-10-24: squad vehicles swap there too)
    const std::string up = Upper(g_level);
    const bool lua = up.size() > 4 && up.compare(up.size() - 4, 4, ".LUA") == 0;
    g_enabled = settings::Get().featLoadout &&
                ((base.rfind("MISSION", 0) == 0 && up.find(".DLL") != std::string::npos) || lua) &&
                !features::HostSync() && !features::LockstepActive();
    if (!g_enabled) {
        g_reloading = false;
        return;
    }
    std::map<std::string, std::string> wanted = save ? g_saveSwaps : ParseSwaps(ReadChoice(ChoiceValueName()));
    std::vector<std::string> units;
    for (auto& [from, to] : wanted) {
        if (!RowOf(to) || !features::VehicleDataComplete(to)) {
            dslog::Write("[fail] Vehicles: %s -> %s: no such vehicle or its model is missing (mod switched off?) - %s kept",
                         from.c_str(), to.c_str(), from.c_str());
            continue;
        }
        g_swaps[from] = to;
        units.push_back(to);
    }
    if (!units.empty()) {
        std::string joined;
        for (const std::string& u : units) joined += u + '\n';
        features::BorrowVehicleDataList(g_level.c_str(), joined);
    }
    // room in the render pools for a swapped-in vehicle with more parts and for the VEHICLE tab's previews
    features::PadLevelPools(g_level.c_str(), 1500);
    dslog::Write("Vehicles: %s - %zu swap(s)%s", g_level.c_str(), g_swaps.size(), save ? " from the saved game" : "");
}

// NewHLO, before the object is made from the record.
void features::VehicleRecord(uint8_t* record) {
    if (!g_enabled || *reinterpret_cast<int*>(record + kRecStatType) != kStatVehicle) return;
#ifndef DS_DIST
    {   // dev: Dev\VehicleDump = 1 writes every vehicle record to %TEMP%\dsfix_vrec_<id>.bin (as it arrives)
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleDump", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v) {
            char tmp[MAX_PATH] = {}, path[MAX_PATH];
            GetTempPathA(MAX_PATH, tmp);
            sprintf_s(path, "%sdsfix_vrec_%d.bin", tmp, *reinterpret_cast<int*>(record));
            if (FILE* f = fopen(path, "wb")) {
                fwrite(record, 1, kRecordSize, f);
                fclose(f);
            }
        }
    }
#endif
    const uint32_t flags = *reinterpret_cast<uint32_t*>(record + kRecFlags);
    int onlyId = -1;   // dev A/B: Dev\VehicleOnlyId = script id + 1 -> only that record is a slot (any flags / crew)
#ifndef DS_DIST
    {
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleOnlyId", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v)
            onlyId = static_cast<int>(v) - 1;
    }
#endif
    if (onlyId >= 0 && *reinterpret_cast<int*>(record) != onlyId) return;
    if (onlyId < 0 && (!(flags & kFlagPlayerUsable) || (flags & kFlagCutscene))) return;
    char unit[kRecUnitSize + 1] = {};
    memcpy(unit, record + kRecUnit, kRecUnitSize);
    const Kind own = KindOfDefinition(unit);
    if (!own.ok || !(own.type == 0 || own.type == 1 || own.type == 3)) return;   // tripod guns, aircraft
    // the squad's own vehicles: one of the four heroes (UNITS.TXT Bradley / Foley / Ramirez = Connors / Jones) in its
    // crew - not every vehicle the player may drive (Mission 1's fuel truck, Mission 9's escorting Abrams)
    int crew = 0;
    bool squad = false;
    for (int i = 0; i < kCrewMax; ++i) {
        const char* name = reinterpret_cast<const char*>(record + kCrewNames + i * kCrewNameSize);
        if (!name[0]) continue;
        ++crew;
        for (const char* hero : {"Bradley", "Foley", "Ramirez", "Jones"}) squad |= _stricmp(name, hero) == 0;
    }
    if (!squad && onlyId < 0) return;
    Slot* s = SlotOf(unit);
    if (!s) {
        g_slots.push_back({unit, 0, 0, {}, {}});
        s = &g_slots.back();
    }
    s->crew = std::max(s->crew, crew);
    ++s->count;
    s->ids.push_back(*reinterpret_cast<int*>(record));
    if (s->record.empty()) s->record.assign(record, record + kRecordSize);
    auto it = g_swaps.find(Upper(unit));
    bool force = false;   // dev: Dev\VehicleForce = 1 runs a same-vehicle "swap" through the whole path (A/B tests)
#ifndef DS_DIST
    {
        DWORD v = 0, size = sizeof v;
        force = RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleForce", RRF_RT_REG_DWORD, nullptr,
                             &v, &size) == ERROR_SUCCESS && v;
    }
#endif
    if (it == g_swaps.end() || (!force && _stricmp(it->second.c_str(), unit) == 0)) return;
    const Kind to = KindOf(it->second);
    const Row* row = RowOf(it->second);
    bool anySeats = false;   // dev: Dev\VehicleAnySeats = 1 allows fewer seats than crew (A/B tests)
#ifndef DS_DIST
    {
        DWORD v = 0, size = sizeof v;
        anySeats = RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleAnySeats", RRF_RT_REG_DWORD,
                                nullptr, &v, &size) == ERROR_SUCCESS && v;
    }
#endif
    if (!Ground(to) || !row || (to.seats < crew && !anySeats)) {
        dslog::Write("[fail] Vehicles: %s can't take the place of %s (%d seats for %d crew)", it->second.c_str(), unit,
                     to.seats, crew);
        return;
    }
    bool preload = true;
#ifndef DS_DIST
    {   // dev A/B: Dev\VehicleNoPreload = 1 -> no early model load (the game loads it, type-checked)
        DWORD v = 0, size = sizeof v;
        preload = !(RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleNoPreload", RRF_RT_REG_DWORD,
                                 nullptr, &v, &size) == ERROR_SUCCESS && v);
    }
#endif
    void* model = preload ? VehicleModel(row->model, false) : reinterpret_cast<void*>(1);
    if (!model) {
        dslog::Write("[fail] Vehicles: %s kept - %s's model didn't load", unit, it->second.c_str());
        return;
    }
    memset(record + kRecUnit, 0, kRecUnitSize);
    strncpy_s(reinterpret_cast<char*>(record + kRecUnit), kRecUnitSize, it->second.c_str(), _TRUNCATE);
    memset(record + kRecModel, 0, kRecModelSize);
    strncpy_s(reinterpret_cast<char*>(record + kRecModel), kRecModelSize, row->model.c_str(), _TRUNCATE);
    *reinterpret_cast<void**>(record + kRecModelPtr) = preload ? model : nullptr;
#ifndef DS_DIST
    {   // dev A/B: Dev\VehicleModelPtr = 0 -> the game loads the model itself (type-checked)
        DWORD v = 1, size = sizeof v;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleModelPtr", RRF_RT_REG_DWORD, nullptr, &v, &size);
        if (!v) *reinterpret_cast<void**>(record + kRecModelPtr) = nullptr;
        dslog::Write("[dev] Vehicles: record +0x60 %p (scene's was %p), y %.1f", *reinterpret_cast<void**>(record + kRecModelPtr),
                     *reinterpret_cast<void**>(record + kRecModelPtr) ? model : nullptr, *reinterpret_cast<float*>(record + 8));
    }
#endif
#ifndef DS_DIST
    {   // dev A/B: Dev\VehicleUA = n (1..3) writes n - 1 into record +0xA8 (the HLOI's first number)
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleUA", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v)
            record[0xA8] = static_cast<uint8_t>(v - 1);
        v = 0, size = sizeof v;   // Dev\VehicleNoPhys = 1 clears HLOI flag 0x200000 (no physics body)
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleNoPhys", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v)
            *reinterpret_cast<uint32_t*>(record + kRecFlags) &= ~kFlagPlayerUsable;
        v = 0, size = sizeof v;   // Dev\VehicleDX = cm added to the spawn x (location A/B)
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleDX", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v)
            *reinterpret_cast<float*>(record + 4) += static_cast<float>(static_cast<int>(v));
        v = 0, size = sizeof v;   // Dev\VehicleNoCrew = 1 empties the crew list
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleNoCrew", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v)
            for (int i = 0; i < kCrewMax; ++i) record[kCrewNames + i * kCrewNameSize] = 0;
        v = 0, size = sizeof v;   // Dev\VehicleF28 = n (1..) writes n - 1 into record +0x20F28
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleF28", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v)
            *reinterpret_cast<int*>(record + 0x20F28) = static_cast<int>(v) - 1;
    }
#endif
    FitCrew(record, to.seats);
    {   // placed a little higher than the scene's vehicle (dev Dev\VehicleLift cm, default 60): its wheels / tracks
        // start above the ground and settle - at the old vehicle's height they may start inside it
        DWORD lift = 60;
#ifndef DS_DIST
        DWORD size = sizeof lift;
        RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleLift", RRF_RT_REG_DWORD, nullptr, &lift, &size);
#endif
        *reinterpret_cast<float*>(record + 8) += static_cast<float>(lift);
    }
    s->loaded = s->chosen = it->second;
    dslog::Write("Vehicles: %d %s -> %s (model %s, %d seats, %d crew)", *reinterpret_cast<int*>(record), unit,
                 it->second.c_str(), row->model.c_str(), to.seats, crew);
}

// ---- dev trace: a swapped vehicle's height after it is made, then for its first frames ----
namespace {
std::vector<std::pair<int, uint8_t*>> g_trace;   // (script id, object)
int g_traceFrames = 0;
uint8_t* ObjectById(int id);   // (below)
uint8_t* TableEntry(int id);   // (below: the object if it is a vehicle)
void HideGunnerWeapons();      // (below: gunners' own weapons not drawn)
}  // namespace

void features::VehicleCreated(uint8_t* record, void* obj) {
#ifndef DS_DIST
    if (!g_enabled || *reinterpret_cast<int*>(record + kRecStatType) != kStatVehicle) return;
    {   // Dev\VehicleLogAll = 1: every vehicle as made - record position, render matrix position, first wheel body
        DWORD v = 0, size = sizeof v;
        if (obj && RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleLogAll", RRF_RT_REG_DWORD, nullptr,
                                &v, &size) == ERROR_SUCCESS && v) {
            __try {
                auto* o = static_cast<uint8_t*>(obj);
                auto* inst = *reinterpret_cast<uint8_t**>(o + 0x2C);
                auto* ctl = *reinterpret_cast<uint8_t**>(o + 0x2C1C);
                const float* m = inst ? reinterpret_cast<const float*>(inst + 0x10C) : nullptr;
                dslog::Write("[dev] Vehicles: made %d %s rec y %.0f obj y %.0f inst m[12..14] %.0f %.0f %.0f ctl %p wheel %.2f",
                             *reinterpret_cast<int*>(record), reinterpret_cast<char*>(record + kRecUnit),
                             *reinterpret_cast<float*>(record + 8), reinterpret_cast<float*>(o + 0x30)[1], m ? m[12] : 0.0f,
                             m ? m[13] : 0.0f, m ? m[14] : 0.0f, ctl, ctl ? *reinterpret_cast<float*>(ctl + 0x440) : 0.0f);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    {   // Dev\VehicleSpaceLog = 1: collision models in the space ([0x643A2C]: +0x3C max, +0x40 free) after each vehicle
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleSpaceLog", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v) {
            __try {
                const auto* sp = *reinterpret_cast<const uint8_t* const*>(0x643A2C);
                const int used = sp ? *reinterpret_cast<const int*>(sp + 0x3C) - *reinterpret_cast<const int*>(sp + 0x40) : -1;
                dslog::Write("[dev] Vehicles: after %d %s - %d collision models", *reinterpret_cast<int*>(record),
                             reinterpret_cast<char*>(record + kRecUnit), used);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }
    {   // Dev\VehicleObjDump = 1: every vehicle object (0x2D00 B) + render instance (0x2C8 B) as made -> %TEMP%\dsfix_vobj_<id>.bin
        DWORD v = 0, size = sizeof v;
        if (obj && RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleObjDump", RRF_RT_REG_DWORD, nullptr,
                                &v, &size) == ERROR_SUCCESS && v) {
            char tmp[MAX_PATH] = {}, path[MAX_PATH];
            GetTempPathA(MAX_PATH, tmp);
            sprintf_s(path, "%sdsfix_vobj_%d.bin", tmp, *reinterpret_cast<int*>(record));
            if (FILE* f = fopen(path, "wb")) {
                __try {
                    fwrite(obj, 1, 0x2D00, f);
                    fwrite(*reinterpret_cast<void**>(static_cast<uint8_t*>(obj) + 0x2C), 1, 0x2C8, f);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
                fclose(f);
            }
        }
    }
    bool swapped = false;
    for (const Slot& s : g_slots)
        swapped |= !s.loaded.empty() && !s.ids.empty() && s.ids.back() == *reinterpret_cast<int*>(record);
    if (!swapped) return;
    auto* o = static_cast<uint8_t*>(obj);
    __try {
        dslog::Write("[dev] Vehicles: made %p vtable %08X pos %.0f %.0f %.0f instance %p", o, o ? *reinterpret_cast<uint32_t*>(o) : 0,
                     o ? reinterpret_cast<float*>(o + 0x30)[0] : 0, o ? reinterpret_cast<float*>(o + 0x30)[1] : 0,
                     o ? reinterpret_cast<float*>(o + 0x30)[2] : 0, o ? *reinterpret_cast<void**>(o + 0x2C) : nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (o) g_trace.push_back({*reinterpret_cast<int*>(record), o}), g_traceFrames = 0;
#else
    (void)record, (void)obj;
#endif
}

void features::OnFrameVehicles() {
    HideGunnerWeapons();
#ifndef DS_DIST
    if (g_trace.empty()) return;
    {   // Dev\VehicleTraceAll = 1: frames 100-200 after a swap, every vehicle's height (who goes wrong first)
        DWORD v = 0, size = sizeof v;
        if (g_traceFrames >= 99 && g_traceFrames < 200 &&
            RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleTraceAll", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v) {
            char line[2048];
            int n = sprintf_s(line, "[dev] Vehicles: all f%d", g_traceFrames + 1);
            for (int id = 0; id < 1024 && n < 1900; ++id) {
                uint8_t* o = TableEntry(id);
                if (!o) continue;
                __try {
                    n += sprintf_s(line + n, sizeof line - n, " %d:%.0f", id, reinterpret_cast<float*>(o + 0x30)[1]);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
            dslog::Write("%s", line);
            __try {   // every Karma body (world [0x643A30]: list [w+8], next +0x1E4; body pos +0x100, vel +0x170, mass +0x200)
                const auto* w = *reinterpret_cast<const uint8_t* const*>(0x643A30);
                int count = 0, bad = 0;
                for (const uint8_t* b = w ? *reinterpret_cast<const uint8_t* const*>(w + 8) : nullptr; b && count < 4096;
                     b = *reinterpret_cast<const uint8_t* const*>(b + 0x1E4), ++count) {
                    const float* f = reinterpret_cast<const float*>(b);
                    bool odd = false;
                    for (int k : {0x100, 0x104, 0x108, 0x170, 0x174, 0x178, 0x200})
                        odd |= !(f[k / 4] == f[k / 4]) || std::fabs(f[k / 4]) > 1e5f;
                    if (odd && bad++ < 6)
                        dslog::Write("[dev] Vehicles: body f%d #%d %p pos %g %g %g vel %g %g %g mass %g", g_traceFrames + 1, count, b,
                                     f[0x100 / 4], f[0x104 / 4], f[0x108 / 4], f[0x170 / 4], f[0x174 / 4], f[0x178 / 4], f[0x200 / 4]);
                }
                dslog::Write("[dev] Vehicles: bodies f%d %d, odd %d", g_traceFrames + 1, count, bad);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
            for (auto& [id, o] : g_trace) {   // the swapped vehicle's Karma bodies: tank controller +0xEB8, wheeled +0xDE8
                __try {
                    auto* ctl = *reinterpret_cast<uint8_t**>(o + 0x2C1C);
                    for (uint32_t off : {0xEB8u, 0xDE8u}) {
                        auto* b = ctl ? *reinterpret_cast<uint8_t**>(ctl + off) : nullptr;
                        if (!b || reinterpret_cast<uintptr_t>(b) < 0x10000) continue;
                        const float* f = reinterpret_cast<const float*>(b);
                        dslog::Write("[dev] Vehicles: body f%d id %d ctl+%X %p pos %g %g %g vel %g %g %g mass %g", g_traceFrames + 1, id,
                                     off, b, f[0x100 / 4], f[0x104 / 4], f[0x108 / 4], f[0x170 / 4], f[0x174 / 4], f[0x178 / 4],
                                     f[0x200 / 4]);
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                }
            }
        }
    }
    if (++g_traceFrames > 400) {
        g_trace.clear();
        return;
    }
    if (g_traceFrames % 10 != 1 && g_traceFrames > 20) return;
    for (auto& [id, o] : g_trace) {
        __try {
            const float* p = reinterpret_cast<const float*>(o + 0x30);
            dslog::Write("[dev] Vehicles: frame %d id %d y %.1f table %s hp %.1f", g_traceFrames, id, p[1],
                         ObjectById(id) == o ? "same" : "OTHER", *reinterpret_cast<float*>(o + 0x54));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
#endif
}

bool features::VehicleSwapsThisLoad() { return g_enabled && !g_swaps.empty(); }

int features::VehicleSlots() { return g_enabled ? static_cast<int>(g_slots.size()) : 0; }

std::string features::VehicleSlotLabel(int i) {
    if (i < 0 || i >= VehicleSlots()) return {};
    const Slot& s = g_slots[i];
    return g_slots.size() == 1 ? "VEHICLE" : "VEHICLE (" + Label(s.unit) + ")";
}

std::string features::VehicleChoiceLabel(int i) {
    if (i < 0 || i >= VehicleSlots()) return {};
    const Slot& s = g_slots[i];
    const std::string unit = s.chosen.empty() ? s.unit : s.chosen;
    std::string label = Label(unit);
    const std::string faction = FactionOf(unit);
    if (!faction.empty() && faction != FactionOf(s.unit)) label += " - " + faction;
    if (s.count > 1) label += " (x" + std::to_string(s.count) + ")";
    return label;
}

std::string features::VehicleSlotName(int i) {   // VEHICLE tab list: the mission's own vehicle
    return i >= 0 && i < VehicleSlots() ? Label(g_slots[i].unit) : std::string();
}

std::string features::VehicleSlotNote(int i) {
    if (i < 0 || i >= VehicleSlots()) return {};
    const Slot& s = g_slots[i];
    std::string note = s.crew ? std::to_string(s.crew) + " CREW" : "NO CREW";
    if (s.count > 1) note = std::to_string(s.count) + " VEHICLES, " + note;
    if (_stricmp(s.chosen.c_str(), s.loaded.c_str()) != 0) note += " - CHANGED";
    return note;
}

std::vector<std::string> features::VehicleChoiceInfo(int i) {   // under the choice: side, seats, where it comes from
    std::vector<std::string> out;
    if (i < 0 || i >= VehicleSlots()) return out;
    const Slot& s = g_slots[i];
    const std::string unit = s.chosen.empty() ? s.unit : s.chosen;
    if (const std::string f = FactionOf(unit); !f.empty()) out.push_back("SIDE: " + f);
    const Kind k = KindOf(unit);
    if (k.ok)
        out.push_back("SEATS: " + std::to_string(k.seats) + (k.type == 1 ? " - TRACKED" : k.type == 0 ? " - WHEELED" : ""));
    if (const auto* m = ModVehicleOf(unit)) out.push_back("FROM A MOD, BASED ON " + Label(m->base));
    else if (s.chosen.empty()) out.push_back("THE MISSION'S OWN");
    return out;
}

bool features::VehicleHasChoices(int i) {
    return i >= 0 && i < VehicleSlots() && Choices(g_slots[i]).size() > 1;
}

void features::StepVehicle(int i, int dir) {
    if (i < 0 || i >= VehicleSlots()) return;
    Slot& s = g_slots[i];
    const std::vector<std::string> list = Choices(s);
    if (list.size() < 2) return;
    const std::string cur = s.chosen.empty() ? s.unit : s.chosen;
    int at = 0;
    for (int k = 0; k < static_cast<int>(list.size()); ++k)
        if (_stricmp(list[k].c_str(), cur.c_str()) == 0) at = k;
    at = (at + (dir < 0 ? -1 : 1) + static_cast<int>(list.size())) % static_cast<int>(list.size());
    s.chosen = at == 0 ? std::string() : list[at];
    dslog::Write("Vehicles: %s -> %s", s.unit.c_str(), at == 0 ? "(own)" : list[at].c_str());
}

bool features::VehiclesChanged() {
    for (const Slot& s : g_slots)
        if (_stricmp(s.chosen.c_str(), s.loaded.c_str()) != 0) return true;
    return false;
}

// Loadout screen closed: the choices remembered; a change restarts the mission with them.
bool features::VehiclesCommit() {
    if (!VehicleSlots()) return false;
    std::map<std::string, std::string> keep = ParseSwaps(ReadChoice(ChoiceValueName()));
    for (const Slot& s : g_slots) keep.erase(Upper(s.unit));   // (slots of vehicles this load didn't see stay)
    std::string value = CurrentValue();
    for (auto& [from, to] : keep) value += (value.empty() ? "" : ";") + from + "=" + to;
    WriteChoice(ChoiceValueName(), value);
    if (!VehiclesChanged()) return false;
    if (!features::RestartLevel()) {
        dslog::Write("[fail] Vehicles: the mission couldn't restart - the new vehicles come next time");
        return false;
    }
    g_openOnMission = true;
    dslog::Write("Vehicles: restarting %s with %s", g_level.c_str(), value.empty() ? "the mission's own vehicles" : value.c_str());
    return true;
}

// (A converted campaign's level opens the screen before its intro, so the restart keeps the intro: it shows the
// chosen vehicle.)
bool features::VehicleReloadSkip() { return g_reloading && !features::CampaignPortLevel(g_level.c_str()); }

// The loadout screen opened: after a vehicle restart it starts on the MISSION tab (once).
bool features::VehicleReloadOpened() {
    const bool was = g_reloading;
    g_reloading = false;
    return was;
}

// ---- saves ----
// Co-save chunk 'VEHS' v1 of dsfix.vehicles (core/pse_saves_format.h): the swaps this level was built with. Older
// saves had them as "V|ORIGINAL|NEW" lines of the .dsfix sidecar - core/pse_saves.cpp turns those into this chunk.
namespace {
const PSESerializationInterface* Ser() { return pse::SerializationInterface(); }

void VehiclesSave(PSESaveWriter* w) {
    if (!g_enabled) return;
    pse_saves::SavedVehicles out;
    for (const Slot& s : g_slots)
        if (!s.loaded.empty()) out[s.unit] = s.loaded;
    if (out.empty()) return;
    const std::vector<uint8_t> b = pse_saves::EncodeVehicles(out);
    Ser()->WriteChunk(w, pse_saves::kVehiclesTag, pse_saves::kChunkVersion, b.data(), static_cast<uint32_t>(b.size()));
}
// A save is being loaded (between the game layer's PSE_MSG_LOAD_BEGIN and _END): its load / revert callback below runs
// only when the game read the file - then the next level load is that save (the old load hook's VehicleSaveBegin).
bool g_loadBegun = false;

void PSE_CALL OnLoaderMessage(const PSEMessage* m, void*) {
    if (m->type == PSE_MSG_LOAD_BEGIN) g_loadBegun = true;
    else if (m->type == PSE_MSG_LOAD_END) g_loadBegun = false;
}

void VehiclesLoadBegin() {
    if (g_loadBegun) g_loadingSave = true;
    g_saveSwaps.clear();
}
void VehiclesLoadChunk(uint32_t tag, uint32_t version, const std::vector<uint8_t>& b) {
    if (tag != pse_saves::kVehiclesTag || version != 1) return;
    pse_saves::SavedVehicles in;
    if (!pse_saves::DecodeVehicles(b.data(), b.size(), &in)) dslog::Write("[fail] Vehicles: the save's vehicle chunk is damaged");
    for (const auto& [from, to] : in) g_saveSwaps[Upper(from)] = to;
}
void VehiclesRevert() {
    if (g_loadBegun) g_loadingSave = true;   // a save without vehicle swaps (or no co-save at all)
    g_saveSwaps.clear();
}
}  // namespace

// A save was loaded: the next level load uses its swaps (g_saveSwaps, from its co-save - none if it had no chunk).
// Its VEHS chunk goes through loadout.cpp's co-save callbacks (saves from before 2026-10-25: under dsfix.vehicles,
// adopted by the manifest).
const features::CosavePart features::kVehiclesCosave = {&VehiclesSave, &VehiclesLoadBegin, &VehiclesLoadChunk,
                                                        &VehiclesRevert};
void features::VehicleCosaveInit() {
    pse::MessagingInterface()->RegisterListener(pse_host::Find("dsfix.vehicles"), "pse", &OnLoaderMessage, nullptr);
}

// ---- the VEHICLE tab's preview (loadout screen) ----
// The camera looks at the slot's vehicle. A choice other than the one this load built is shown by a copy of the
// slot's scene record made into an object now (NewHLO, as the loadout's weapon swaps do): script id -1 (not in the id
// table), no crew, HLOI flag 0x200000 kept: the set-up helpers it gates make the tracks / road wheels together with the
// physics body (cleared, previews had no wheels; the world runs at 1/10000 speed on this screen, so the body stays
// put) - files borrowed from the other levels first, the definition's resources bound and the textures loaded. The
// level's own vehicle and its wheel instances are hidden meanwhile (render flag 4, SetHidden). One copy per vehicle,
// kept hidden while not shown and taken out of play (SetActive(0) + FUN_00482410) when the screen closes.
namespace {
uint8_t* ObjectById(int id) {
    __try {
        const auto* mgr = *reinterpret_cast<const uint8_t* const*>(kObjectManager);
        auto* const* table = mgr ? *reinterpret_cast<uint8_t* const* const*>(mgr + 0x30) : nullptr;
        return table && id >= 0 && id < 0x10000 ? table[id] : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// Hidden = render-instance flag 4 (script API hide_instance 0x608, FUN_004d1220 - not drawn, still in play). Flag
// 0x10000000 (hide_object) alone left the vehicles drawn (verified: Mission 9's Bradleys covered every preview with
// it set). Flag 4 is also the game's own "out of play" mark, so each object's original bit is kept and put back.
std::unordered_map<uint8_t*, bool> g_hadFlag4;


void SetInstanceHidden(uint8_t* key, uint32_t* flags, bool hidden) {
    if (hidden) {
        g_hadFlag4.emplace(key, (*flags & kOutOfPlayFlag) != 0);   // (first hide only)
        *flags |= kHiddenFlag | kOutOfPlayFlag;
    } else {
        *flags &= ~kHiddenFlag;
        if (auto it = g_hadFlag4.find(key); it != g_hadFlag4.end()) {
            if (!it->second) *flags &= ~kOutOfPlayFlag;
            g_hadFlag4.erase(it);
        }
    }
}

// A vehicle's tracks / road wheels are render instances of their own (made from its .DYN by the set-up helpers
// FUN_00438a80 wheeled / FUN_00438e20 tracked: FUN_004ddb10, instance +0xC0 = the vehicle), kept in 18 slots at
// vehicle +0x2AF8 - hiding only the body left the Bradley's tracks and wheels under every preview.
constexpr int kWheelInstances = 0x2AF8, kWheelSlots = 18;

// Checked without faulting (the crash logger records even caught faults): committed, readable memory.
bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return static_cast<const uint8_t*>(p) + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

uint32_t* InstanceFlags(uint8_t* obj) {   // render instance +0xC, or null
    if (!Readable(obj, 0x30)) return nullptr;
    auto* inst = *reinterpret_cast<uint8_t**>(obj + 0x2C);
    return Readable(inst, 0x10) ? reinterpret_cast<uint32_t*>(inst + 0xC) : nullptr;
}

uint8_t* WheelInstance(uint8_t* obj, int k) {   // only slots holding an instance whose +0xC0 is this vehicle
    if (!Readable(obj, kWheelInstances + kWheelSlots * 4) || *reinterpret_cast<uint32_t*>(obj) != kVehicleVtable)
        return nullptr;
    auto* inst = reinterpret_cast<uint8_t**>(obj + kWheelInstances)[k];
    if (!Readable(inst, 0xC4) || *reinterpret_cast<uint8_t**>(inst + 0xC0) != obj) return nullptr;
    return inst;
}

// The chassis' Karma body (wheeled type 0: [v+0x2C18]+0xDE8, tracked 1: [v+0x2C1C]+0xEB8 - as authority.cpp Chassis).
uint8_t* ChassisBody(uint8_t* v) {
    if (!Readable(v, 0x2C20) || *reinterpret_cast<uint32_t*>(v) != kVehicleVtable) return nullptr;
    const uint8_t* unit = *reinterpret_cast<uint8_t* const*>(v + 0x24);
    const uint8_t* a = Readable(unit, 0x84) ? *reinterpret_cast<uint8_t* const*>(unit + 0x80) : nullptr;
    const uint8_t* b = Readable(a, 0x48) ? *reinterpret_cast<uint8_t* const*>(a + 0x44) : nullptr;
    const uint32_t type = Readable(b, 0x28) ? *reinterpret_cast<const uint32_t*>(b + 0x24) : 99;
    const uint8_t* ctrl = type == 0   ? *reinterpret_cast<uint8_t* const*>(v + 0x2C18)
                          : type == 1 ? *reinterpret_cast<uint8_t* const*>(v + 0x2C1C)
                                      : nullptr;
    const int at = type == 0 ? 0xDE8 : 0xEB8;
    uint8_t* body = Readable(ctrl, at + 4) ? *reinterpret_cast<uint8_t* const*>(ctrl + at) : nullptr;
    return Readable(body, 0x214) ? body : nullptr;
}

// ---- gunners' own weapons ----
// A soldier at a vehicle's gun keeps his own weapon in his hands. The game's gunner animation (VehicleGunnerAim) holds
// it inside the mounted gun, so it never showed; a standing pose (a mod's gunner animation, WEAPS column 54) shows it
// above the gun. A held weapon is drawn as a model on a connector of the soldier's body: the hold FUN_0042ec10 puts
// [weapon render instance +0xC8] on connector FUN_00502c30(body, definition +0x94 connector hash) with FUN_00502bf0
// (body, index, model) - so a soldier in a gunner seat (seat connector GUNNERnn = index in the occupant array +0x2608)
// gets that connector emptied while he sits there, and his weapon back when he leaves or holds another. Drawing only.
constexpr uint32_t kConnectorIndex = 0x502C30, kConnectorModel = 0x502C10, kSetConnectorModel = 0x502BF0;
struct GunnerVehicle {
    uint8_t* v;
    uint32_t seats;   // gunner seat bits
};
struct HiddenWeapon {
    uint8_t* weapon;
    int connector;
    void* model;
};
std::vector<GunnerVehicle> g_gunnerVehicles;
std::unordered_map<uint8_t*, HiddenWeapon> g_gunnerWeapons;   // soldier -> the weapon we took off his hand connector
int g_gunnerScan = 0;

void ScanGunnerVehicles() {
    g_gunnerVehicles.clear();
    std::unordered_map<const uint8_t*, uint32_t> byDefinition;   // vehicle definition (unit +0x80) -> gunner seats
    for (const Row& r : Rows()) {
        if (!r.gunnerSeats) continue;
        const auto* u = reinterpret_cast<const uint8_t* (__cdecl*)(const char*, int)>(kFindDefinition)(r.unit.c_str(), kStatVehicle);
        if (Readable(u, 0x84) && *reinterpret_cast<const uint8_t* const*>(u + 0x80))
            byDefinition[*reinterpret_cast<const uint8_t* const*>(u + 0x80)] = r.gunnerSeats;
    }
    if (byDefinition.empty()) return;
    const auto* mgr = *reinterpret_cast<const uint8_t* const*>(kObjectManager);
    auto* const* table = Readable(mgr, 0x34) ? *reinterpret_cast<uint8_t* const* const*>(mgr + 0x30) : nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!Readable(table, 4) || VirtualQuery(table, &mbi, sizeof mbi) != sizeof mbi) return;
    const size_t n = std::min<size_t>(4096, (static_cast<uint8_t*>(mbi.BaseAddress) + mbi.RegionSize -
                                             reinterpret_cast<const uint8_t*>(table)) / 4);
    uintptr_t lo = 0, hi = 0;   // last readable region seen (objects share a few heap regions)
    for (size_t id = 0; id < n; ++id) {
        uint8_t* v = table[id];
        const auto a = reinterpret_cast<uintptr_t>(v);
        if (!v) continue;
        if (a < lo || a >= hi || hi - a < 0x2C00) {   // (no a + size: 0xFFFFFFFF entries wrap round)
            if (!Readable(v, 0x2C00) || VirtualQuery(v, &mbi, sizeof mbi) != sizeof mbi) continue;
            lo = reinterpret_cast<uintptr_t>(mbi.BaseAddress), hi = lo + mbi.RegionSize;
        }
        if (*reinterpret_cast<uint32_t*>(v) != kVehicleVtable) continue;
        const auto* unit = *reinterpret_cast<const uint8_t* const*>(v + 0x24);
        if (!Readable(unit, 0x84)) continue;
        auto it = byDefinition.find(*reinterpret_cast<const uint8_t* const*>(unit + 0x80));
        if (it != byDefinition.end()) g_gunnerVehicles.push_back({v, it->second});
    }
    static size_t logged = ~size_t(0);
    if (logged != g_gunnerVehicles.size())
        logged = g_gunnerVehicles.size(), dslog::Write("Vehicles: %zu vehicle(s) with gunner seats (%zu kinds)", logged, byDefinition.size());
}

// The soldier's body instance and the connector his held weapon's model hangs on (-1: none). Not looked up by the
// definition's connector hash: seated, the weapon is on another connector than the hold put it on.
uint8_t* HandConnector(uint8_t* soldier, uint8_t* weapon, int& connector, void*& model) {
    connector = -1, model = nullptr;
    auto* body = *reinterpret_cast<uint8_t**>(soldier + 0x2C);
    if (!Readable(weapon, 0x30) || !Readable(body, 0x21C)) return nullptr;
    const auto* inst = *reinterpret_cast<const uint8_t* const*>(weapon + 0x2C);
    if (!Readable(inst, 0xCC)) return nullptr;
    model = *reinterpret_cast<void* const*>(inst + 0xC8);
    const int count = *reinterpret_cast<int*>(body + 0x214);
    const auto* entries = *reinterpret_cast<const uint8_t* const*>(body + 0x218);   // 16 bytes each, +8 model
    if (!model || count <= 0 || count > 256 || !Readable(entries, count * 16)) return nullptr;
    for (int k = 0; k < count; ++k)
        if (*reinterpret_cast<void* const*>(entries + k * 16 + 8) == model) return connector = k, body;
    return nullptr;
}
void* ConnectorModel(uint8_t* body, int connector) {
    return reinterpret_cast<void*(__thiscall*)(void*, int)>(kConnectorModel)(body, connector);
}
void SetConnectorModel(uint8_t* body, int connector, void* model) {
    reinterpret_cast<void(__thiscall*)(void*, int, void*)>(kSetConnectorModel)(body, connector, model);
}

void HideGunnerWeapons() {
    if (*reinterpret_cast<int*>(0x60EC8C) != 0) {   // loading / unloading: objects go away, nothing to put back
        g_gunnerVehicles.clear(), g_gunnerWeapons.clear(), g_gunnerScan = 0;
        return;
    }
    if (g_gunnerScan-- <= 0) ScanGunnerVehicles(), g_gunnerScan = 60;
    std::unordered_map<uint8_t*, uint8_t*> seated;   // soldier in a gunner seat -> the weapon in his hands
    for (const GunnerVehicle& g : g_gunnerVehicles) {
        if (!Readable(g.v, 0x260C) || *reinterpret_cast<uint32_t*>(g.v) != kVehicleVtable) continue;
        auto* const* seats = *reinterpret_cast<uint8_t* const* const*>(g.v + 0x2608);
        for (int k = 0; k < 8; ++k) {
            if (!(g.seats >> k & 1) || !Readable(seats + k, 4)) continue;
            uint8_t* s = seats[k];
            if (!Readable(s, 0x26B4) || *reinterpret_cast<uint8_t**>(s + 0x267C) != g.v) continue;
            if (uint8_t* w = *reinterpret_cast<uint8_t**>(s + 0x26B0)) seated.emplace(s, w);
        }
    }
    for (auto it = g_gunnerWeapons.begin(); it != g_gunnerWeapons.end();) {   // left the seat / another weapon in hand
        auto found = seated.find(it->first);
        if (found != seated.end() && found->second == it->second.weapon) {
            ++it;
            continue;
        }
        uint8_t* s = it->first;
        if (Readable(s, 0x2C00) && *reinterpret_cast<uint8_t**>(s + 0x26B0) == it->second.weapon) {
            auto* body = *reinterpret_cast<uint8_t**>(s + 0x2C);
            if (Readable(body, 0x21C) && !ConnectorModel(body, it->second.connector))
                SetConnectorModel(body, it->second.connector, it->second.model);
        }
        it = g_gunnerWeapons.erase(it);
    }
    for (auto& [s, w] : seated) {
        if (auto it = g_gunnerWeapons.find(s); it != g_gunnerWeapons.end()) {   // (put back by the game meanwhile)
            auto* body = *reinterpret_cast<uint8_t**>(s + 0x2C);
            if (Readable(body, 0x21C) && ConnectorModel(body, it->second.connector) == it->second.model)
                SetConnectorModel(body, it->second.connector, nullptr);
            continue;
        }
        int connector;
        void* model;
        uint8_t* body = HandConnector(s, w, connector, model);
        if (!body) continue;   // (crews inside a hull: their weapon isn't drawn at all)
        SetConnectorModel(body, connector, nullptr);
        g_gunnerWeapons.emplace(s, HiddenWeapon{w, connector, model});
    }
}

// A preview stands on its vehicle's spot with a physics body of its own, and so did every earlier preview: the
// push-out between them threw vehicles metres away (Mission 9: the hidden Bradley 1-2.7 m, a hidden preview 49 m;
// disabling their Karma bodies didn't hold - an active body wakes a disabled one it touches, and disabling a fresh
// preview's body ended the game). So a vehicle that isn't on screen is out of play - SetActive(0) (vtable +4; in
// play = object +0xC bit 0) - and back in when shown; only the shown one has a body.
std::unordered_map<uint8_t*, bool> g_wasInPlay;

void SetInPlay(uint8_t* v, bool inPlay) {
    if (!Readable(v, 0x10) || *reinterpret_cast<uint32_t*>(v) != kVehicleVtable) return;
    auto setActive = reinterpret_cast<void(__thiscall*)(void*, int)>(
        reinterpret_cast<void* const*>(*reinterpret_cast<uint8_t* const*>(v))[1]);
    if (!inPlay) {
        const bool was = (*reinterpret_cast<uint32_t*>(v + 0xC) & 1) != 0;
        g_wasInPlay.emplace(v, was);   // (first time only)
        if (was) setActive(v, 0);
    } else if (auto it = g_wasInPlay.find(v); it != g_wasInPlay.end()) {
        if (it->second && !(*reinterpret_cast<uint32_t*>(v + 0xC) & 1)) setActive(v, 1);
        g_wasInPlay.erase(it);
    }
}

// Where a vehicle stood when first hidden is kept and it is put back there when shown again - the game's
// place call FUN_00442650 thiscall(veh, pos cm, rot deg) ret 8 (chassis + wheels, as the save loader / authority.cpp),
// heading FUN_004c9a40 thiscall(veh, float rot[3]), position = render instance +0x13C (physics) / +0xF4, velocities 0.
constexpr uint32_t kVehPlace = 0x442650, kHeading = 0x4C9A40, kSetLinVel = 0x58E3E0, kSetAngVel = 0x58E410;
struct Placement { float pos[3], rot[3]; };
std::unordered_map<uint8_t*, Placement> g_placedAt;

const float* VehiclePos(uint8_t* v) {
    if (!Readable(v, 0x30)) return nullptr;
    const uint8_t* inst = *reinterpret_cast<uint8_t* const*>(v + 0x2C);
    if (!Readable(inst, 0x1CC)) return nullptr;
    return reinterpret_cast<const float*>(inst + (*reinterpret_cast<const uint32_t*>(inst + 0x1C8) ? 0x13C : 0xF4));
}

void KeepPlacement(uint8_t* v, bool hidden) {
    if (!Readable(v, 4) || *reinterpret_cast<uint32_t*>(v) != kVehicleVtable) return;
    const float* p = VehiclePos(v);
    if (!p) return;
    if (hidden) {
        if (g_placedAt.count(v)) return;
        Placement pl{};
        memcpy(pl.pos, p, sizeof pl.pos);
        reinterpret_cast<void(__thiscall*)(void*, float*)>(kHeading)(v, pl.rot);
        g_placedAt.emplace(v, pl);
        return;
    }
    auto it = g_placedAt.find(v);
    if (it == g_placedAt.end()) return;
    Placement pl = it->second;
    g_placedAt.erase(it);
    const float dx = p[0] - pl.pos[0], dy = p[1] - pl.pos[1], dz = p[2] - pl.pos[2];
    if (dx * dx + dy * dy + dz * dz < 2.0f * 2.0f) return;
    reinterpret_cast<void(__thiscall*)(void*, float*, float*)>(kVehPlace)(v, pl.pos, pl.rot);
    if (uint8_t* body = ChassisBody(v)) {
        reinterpret_cast<void(__stdcall*)(void*, float, float, float)>(kSetLinVel)(body, 0.0f, 0.0f, 0.0f);
        reinterpret_cast<void(__stdcall*)(void*, float, float, float)>(kSetAngVel)(body, 0.0f, 0.0f, 0.0f);
    }
    dslog::Write("[dev] Vehicles: %p put back (it had moved %.0f cm while hidden)", static_cast<void*>(v),
                 std::sqrt(dx * dx + dy * dy + dz * dz));
}

void SetHidden(uint8_t* obj, bool hidden) {
    uint32_t* flags = obj ? InstanceFlags(obj) : nullptr;
    if (!flags) return;
    if (hidden) {
        KeepPlacement(obj, true);   // (before it leaves play)
        SetInstanceHidden(obj, flags, true);
        SetInPlay(obj, false);
    } else {
        SetInPlay(obj, true);       // (first: SetActive(1) sets its own render flags)
        SetInstanceHidden(obj, flags, false);
        KeepPlacement(obj, false);
    }
    for (int k = 0; k < kWheelSlots; ++k)
        if (uint8_t* w = WheelInstance(obj, k)) SetInstanceHidden(w, reinterpret_cast<uint32_t*>(w + 0xC), hidden);
}

DWORD g_faultEip = 0, g_faultStack[6] = {};   // where a preview's NewHLO faulted (logged with the failure)

int FaultFilter(EXCEPTION_POINTERS* ep) {
    g_faultEip = static_cast<DWORD>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress));
    const DWORD* sp = reinterpret_cast<const DWORD*>(static_cast<uintptr_t>(ep->ContextRecord->Esp));
    int n = 0;
    __try {   // game code addresses on the stack (0x401000..0x5D7000) = the call chain, nearest first
        for (int i = 0; i < 512 && n < 6; ++i)
            if (sp[i] >= 0x401000 && sp[i] < 0x5D7000) g_faultStack[n++] = sp[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// The vehicle ctors look their .DYN (wheels / tracks) up through the dynamics-data manager [0x60EDD4] (FUN_00512770:
// list at +4, not there = loaded by FUN_00512b50 and added) - level init makes it (0x18 bytes, zeroed, 0x40B42C) and
// frees it once the scene is built (0x40CDF9), so a preview made on the loadout screen found it null and faulted at
// 0x51277F / 0x5127DF. Previews get this one for the call; its list keeps the .DYNs they loaded (a few KB, reused).
constexpr uint32_t kDynamicsData = 0x60EDD4;
uint8_t g_previewDynamics[0x18] = {};

uint8_t* NewObject(uint8_t* record, int& stage, DWORD& code) {   // (own function: __try can't share a frame with
    stage = 0;                                                    // objects that unwind)
    void** dyn = reinterpret_cast<void**>(kDynamicsData);
    void* const levelDyn = *dyn;
    if (!levelDyn) *dyn = g_previewDynamics;
    __try {
        uint8_t* obj = reinterpret_cast<uint8_t* (__cdecl*)(uint8_t*)>(kNewHlo)(record);
        if (!levelDyn) *dyn = nullptr;
        stage = 1;
        if (obj) {
            reinterpret_cast<void(__cdecl*)(void*)>(kBindDefinition)(obj);
            stage = 2;
            reinterpret_cast<void(__thiscall*)(void*, void*, int)>(kLoadPendingTextures)(
                *reinterpret_cast<void**>(kTextureManager), nullptr, 0);
            stage = 3;
        }
        return obj;
    } __except (code = GetExceptionCode(), FaultFilter(GetExceptionInformation())) {
        if (!levelDyn) *dyn = nullptr;
        return nullptr;
    }
}

uint8_t* MakePreview(Slot& s, const std::string& unit) {
    const Row* row = RowOf(unit);
    if (!row || s.record.empty()) return nullptr;
    features::BorrowVehicleDataList(g_level.c_str(), unit);
    void* model = VehicleModel(row->model, true);
    if (!model) return nullptr;
    std::vector<uint8_t> rec = s.record;
    uint8_t* r = rec.data();
    *reinterpret_cast<int*>(r) = -1;
    if (uint8_t* own = ObjectById(s.ids.empty() ? -1 : s.ids[0]))   // where the level's vehicle stands now
        memcpy(r + 4, own + 0x30, 12);
    memset(r + kRecUnit, 0, kRecUnitSize);
    strncpy_s(reinterpret_cast<char*>(r + kRecUnit), kRecUnitSize, unit.c_str(), _TRUNCATE);
    memset(r + kRecModel, 0, kRecModelSize);
    strncpy_s(reinterpret_cast<char*>(r + kRecModel), kRecModelSize, row->model.c_str(), _TRUNCATE);
    *reinterpret_cast<void**>(r + kRecModelPtr) = model;
    for (int i = 0; i < kCrewMax; ++i) r[kCrewNames + i * kCrewNameSize] = 0;
    int stage = 0;
    DWORD code = 0;
    uint8_t* obj = NewObject(r, stage, code);
    const uint32_t vtable = obj ? *reinterpret_cast<uint32_t*>(obj) : 0;
    if (!obj || vtable != kVehicleVtable) {
        dslog::Write("[fail] Vehicles: no preview of %s (object %p, vtable %08X, stage %d, exception %08X at %08X, "
                     "stack %08X %08X %08X %08X %08X %08X)", unit.c_str(), obj, vtable, stage, code, g_faultEip,
                     g_faultStack[0], g_faultStack[1], g_faultStack[2], g_faultStack[3], g_faultStack[4], g_faultStack[5]);
        return nullptr;
    }
    dslog::Write("Vehicles: preview of %s made", unit.c_str());
    return obj;
}
}  // namespace

namespace {
// Other vehicles within 30 m of the squad's (Mission 9: the intro's copies of the Bradley parked on the same spot) -
// hidden on the soldier tabs too, so only the soldier is in the picture. Collected once per screen.
std::vector<uint8_t*> g_nearby;
bool g_nearbyFound = false;
uint8_t* TableEntry(int id) {   // (checked, not SEH: ids past the table's end read garbage pointers, and every caught
                                //  fault was logged as a crash with a dump each time the screen opened)
    const auto* mgr = *reinterpret_cast<const uint8_t* const*>(kObjectManager);
    auto* const* table = Readable(mgr, 0x34) ? *reinterpret_cast<uint8_t* const* const*>(mgr + 0x30) : nullptr;
    uint8_t* obj = Readable(table + id, 4) ? table[id] : nullptr;
    return Readable(obj, 4) && *reinterpret_cast<uint32_t*>(obj) == kVehicleVtable ? obj : nullptr;
}
void FindNearby() {
    g_nearbyFound = true;
    g_nearby.clear();
    std::vector<const float*> centres;
    for (const Slot& s : g_slots)
        for (int id : s.ids)
            if (uint8_t* v = ObjectById(id)) centres.push_back(reinterpret_cast<const float*>(v + 0x30));
    for (int id = 0; id < 4096 && !centres.empty(); ++id) {
        uint8_t* v = TableEntry(id);
        if (!v) continue;
        bool ours = false;
        for (const Slot& s : g_slots)
            for (int own : s.ids) ours |= own == id;
        if (ours) continue;
        const float* p = reinterpret_cast<const float*>(v + 0x30);
        for (const float* c : centres) {
            const float dx = p[0] - c[0], dy = p[1] - c[1], dz = p[2] - c[2];
            if (dx * dx + dy * dy + dz * dz < 3000.0f * 3000.0f) {
                g_nearby.push_back(v);
                break;
            }
        }
    }
}
bool CrewWaitingToLeave(uint8_t* vehicle);   // (below: a soldier still waits to get out of it for the screen)
}  // namespace

// Called every frame the screen is up: slot = the VEHICLE tab's vehicle (-1 = another tab: the squad's vehicles all
// hidden, the soldiers stand beside them).
void features::VehicleShow(int slot) {
    if (!g_enabled) return;
    if (!g_nearbyFound) FindNearby();
    bool previewing = false;   // a preview on screen: the other vehicles parked on its spot (Mission 9's intro copies of
                               // the Bradley) are hidden too, or they cover it
    for (int i = 0; i < static_cast<int>(g_slots.size()); ++i)
        previewing |= i == slot && _stricmp(g_slots[i].chosen.c_str(), g_slots[i].loaded.c_str()) != 0;
    for (uint8_t* v : g_nearby) SetHidden(v, slot < 0 || previewing);
    for (int i = 0; i < static_cast<int>(g_slots.size()); ++i) {
        Slot& s = g_slots[i];
        const bool here = i == slot;
        const bool other = _stricmp(s.chosen.c_str(), s.loaded.c_str()) != 0;
        const std::string want = Upper(s.chosen.empty() ? s.unit : s.chosen);
        if (here && other && !s.previews.count(want)) {   // the level's vehicle and the other previews out of play first:
            for (int id : s.ids) SetHidden(ObjectById(id), true);   // the new one is made on their spot
            for (auto& [unit, obj] : s.previews) SetHidden(obj, true);
            s.previews[want] = MakePreview(s, s.chosen.empty() ? s.unit : s.chosen);
        }
        const bool previewShown = here && other && s.previews[want];
        for (int id : s.ids) {
            uint8_t* own = ObjectById(id);
            SetHidden(own, !(here && !previewShown) && !CrewWaitingToLeave(own));   // (in play until they're out)
        }
        for (auto& [unit, obj] : s.previews) SetHidden(obj, !(here && other && unit == want));
    }
}

// The vehicle the camera looks at on the VEHICLE tab.
uint8_t* features::VehicleObject(int slot) {
    if (slot < 0 || slot >= VehicleSlots()) return nullptr;
    Slot& s = g_slots[slot];
    if (_stricmp(s.chosen.c_str(), s.loaded.c_str()) != 0) {
        auto it = s.previews.find(Upper(s.chosen.empty() ? s.unit : s.chosen));
        if (it != s.previews.end() && it->second) return it->second;
    }
    return s.ids.empty() ? nullptr : ObjectById(s.ids[0]);
}

namespace {
void TakeOutOfPlay(uint8_t* obj) {
    __try {
        (*reinterpret_cast<void(__thiscall**)(void*, int)>(*reinterpret_cast<uint8_t**>(obj) + 4))(obj, 0);
        reinterpret_cast<void(__cdecl*)(void*, int)>(kDeactivate)(obj, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
}  // namespace

// The screen closed: the level's vehicles back, the previews out of play.
void features::VehicleShowEnd() {
    for (uint8_t* v : g_nearby) SetHidden(v, false);
    g_nearby.clear();
    g_nearbyFound = false;
    for (Slot& s : g_slots) {
        for (int id : s.ids) SetHidden(ObjectById(id), false);
        for (auto& [unit, obj] : s.previews) {
            if (!obj) continue;
            SetHidden(obj, true);
            TakeOutOfPlay(obj);
            g_hadFlag4.erase(obj), g_wasInPlay.erase(obj), g_placedAt.erase(obj);   // (gone for good)
        }
        s.previews.clear();
    }
}

// ---- the squad out of their vehicles while the screen is up ----
// A seated soldier isn't drawn (and the camera would sit inside the hull), so the screen takes each one out with the
// game's own dismount (FUN_0042e850 thiscall(soldier, 0, 0): frees the seat, FUN_0042e8f0 puts him beside it) and puts
// him back in the same seat when it closes, as the vehicle's set-up seats its crew (FUN_00442300 thiscall(vehicle, seat,
// &a, &b), FUN_00433f00 thiscall(soldier, vehicle, a, b, 0)). Seat = index in the vehicle's occupant array +0x2608
// (def +0x40 seats). The mission script's message callback ([0x60F578]) is taken away meanwhile: it never hears of it
// (DISMOUNTED 0x1C / MOUNTED 0x1B). Tripod guns (type 11) and online games are left alone.
namespace {
struct Staged {
    uint8_t* soldier;
    uint8_t* vehicle;
    int seat;
};
std::vector<Staged> g_staged;
// Soldiers whose vehicle found no free exit spot when the screen opened (Mission 9's Bradley swapped for a Land Rover:
// a swapped-in vehicle starts 60 cm up and is still settling ~1 s into play, when the screen opens; the same soldiers
// get out fine once it stands) - tried again each frame while the screen's first seconds run in normal time (Settle).
// Their vehicle stays in play meanwhile (VehicleShow): one out of play lets nobody out (FUN_0043CA60 finds no exit
// spot - its connector matrices aren't updated), and putting it in play only for the call wasn't enough.
std::vector<uint8_t*> g_unstaged;
DWORD g_unstagedSince = 0;
constexpr DWORD kStageRetryMs = 4000;

bool CrewWaitingToLeave(uint8_t* vehicle) {
    if (!vehicle) return false;
    for (uint8_t* s : g_unstaged)
        if (*reinterpret_cast<uint8_t**>(s + 0x267C) == vehicle) return true;
    return false;
}

int SeatOf(uint8_t* vehicle, uint8_t* soldier, int& type) {
    __try {
        const auto* unit = *reinterpret_cast<const uint8_t* const*>(vehicle + 0x24);
        const auto* def = *reinterpret_cast<const uint8_t* const*>(unit + 0x80);
        const auto* stat = *reinterpret_cast<const uint8_t* const*>(def + 0x44);
        type = stat ? *reinterpret_cast<const int*>(stat + 0x24) : -1;
        auto* const* seats = *reinterpret_cast<uint8_t* const* const*>(vehicle + 0x2608);
        for (int k = 0; seats && k < def[0x40]; ++k)
            if (seats[k] == soldier) return k;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return -1;
}

bool Dismount(uint8_t* soldier) {
    __try {
        reinterpret_cast<int(__thiscall*)(void*, int, int)>(kDismount)(soldier, 0, 0);
        return *reinterpret_cast<uint8_t**>(soldier + 0x267C) == nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Remount(const Staged& st) {
    __try {
        if (*reinterpret_cast<uint8_t**>(st.soldier + 0x267C)) return false;   // (in something again already)
        int a = 0, b = 0;
        if (!reinterpret_cast<int(__thiscall*)(void*, int, int*, int*)>(kSeatPlace)(st.vehicle, st.seat, &a, &b)) return false;
        reinterpret_cast<void(__thiscall*)(void*, void*, int, int, int)>(kMount)(st.soldier, st.vehicle, a, b, 0);
        return *reinterpret_cast<uint8_t**>(st.soldier + 0x267C) == st.vehicle;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
}  // namespace

void features::StageSquadOutside(uint8_t* const* soldiers, int count) {
    g_staged.clear();
    g_unstaged.clear();
    g_unstagedSince = GetTickCount();
    if (features::HostSync() || features::LockstepActive()) return;
#ifndef DS_DIST
    {   // dev: Dev\VehicleNoStage = 1 leaves the crew seated
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleNoStage", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v)
            return;
    }
#endif
    void*& script = *reinterpret_cast<void**>(kScriptObject);
    void* const saved = script;
    script = nullptr;
    for (int i = 0; i < count; ++i) {
        uint8_t* s = soldiers[i];
        uint8_t* v = s ? *reinterpret_cast<uint8_t**>(s + 0x267C) : nullptr;
        if (!v) continue;
        int type = -1;
        const int seat = SeatOf(v, s, type);
        if (seat < 0 || type == 11) continue;
        if (!Dismount(s)) {
            g_unstaged.push_back(s);
            continue;
        }
        g_staged.push_back({s, v, seat});
    }
    script = saved;
    if (!g_staged.empty()) dslog::Write("Vehicles: %zu soldier(s) out of their vehicle for the loadout screen", g_staged.size());
    if (!g_unstaged.empty())
        dslog::Write("Vehicles: %zu soldier(s) found no free spot beside their vehicle - trying again", g_unstaged.size());
}

// Every frame the screen is up, after VehicleShow: the soldiers that couldn't get out at the start, once more.
void features::RetryStageSquad() {
    if (g_unstaged.empty()) return;
    void*& script = *reinterpret_cast<void**>(kScriptObject);
    void* const saved = script;
    script = nullptr;
    for (auto it = g_unstaged.begin(); it != g_unstaged.end();) {
        uint8_t* s = *it;
        uint8_t* v = *reinterpret_cast<uint8_t**>(s + 0x267C);
        int type = -1;
        const int seat = v ? SeatOf(v, s, type) : -1;
        if (seat < 0) {
            it = g_unstaged.erase(it);   // (out already, or in something we don't stage)
            continue;
        }
        if (Dismount(s)) {
            g_staged.push_back({s, v, seat});
            dslog::Write("Vehicles: a soldier out of his vehicle for the loadout screen (%lu ms after it opened)",
                         GetTickCount() - g_unstagedSince);
            it = g_unstaged.erase(it);
        } else {
            ++it;
        }
    }
    script = saved;
    if (!g_unstaged.empty() && GetTickCount() - g_unstagedSince >= kStageRetryMs) {
        dslog::Write("[fail] Vehicles: %zu soldier(s) couldn't leave their vehicle for the loadout screen", g_unstaged.size());
        g_unstaged.clear();
    }
}

void features::ReturnSquadInside() {
    g_unstaged.clear();
    if (g_staged.empty()) return;
    void*& script = *reinterpret_cast<void**>(kScriptObject);
    void* const saved = script;
    script = nullptr;
    int back = 0;
    for (const Staged& st : g_staged) back += Remount(st) ? 1 : 0;
    script = saved;
    dslog::Write("Vehicles: %d of %zu soldier(s) back in their seats", back, g_staged.size());
    g_staged.clear();
}
