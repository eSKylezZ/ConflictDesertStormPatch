// Soldier gear: hats, helmets, packs and face pieces (beards, glasses, cigars) are rigid models hung on the body's
// bone connectors, not part of the skin. All four hero bodies have the same connectors.
//
// Game side: CONNOBJS.TXT (catalog.dat: role, connector, model, faction, id) is loaded at start-up by FUN_00496eb0
// into 0x618490: [role * 4 + faction][6] x 0x28 bytes {u32 connector name hash (0x4B9C30), char[36] model}; roles 0
// rifleman / 1 sniper / 2 heavy / 3 engineer, factions 0 SAS / 1 US / 2 Russian / 3 Iraqi (= squad slots and skin
// sides). FUN_00435cd0 (thiscall soldier, role, faction) hangs a set on a soldier (level load FUN_00495d20, the
// network game's faction change FUN_00435ef0). Render instance = soldier +0x2C: connector index FUN_00502c30
// (thiscall instance, name hash; -1 none), records instance +0x218 + index * 16 (+8 = attached model; getter
// FUN_00502c10, setter FUN_00502bf0 (index, model) - model 0 = nothing), instance +0x21C flags |= 0x800 after an
// attach. Model name hash (0x4B9C40) at model +0x1BC. Riding a vehicle (soldier +0x267C, not a tripod gun) the
// back piece waits in soldier +0x2AFC. The 83 attachment models the game knows are listed at 0x5F9FF0 (count
// [0x5F9FE8]); they live in chardata.dat, a mission archive only has the ones its scene uses - loaded here like the
// network game does: archive slot 2 (chardata.dat) opened around the load (FUN_004bd000 close / FUN_004bcf70 open /
// FUN_004bd060 reopen), then the texture manager's pending batch.
//
// Night vision: FUN_00435e20 (thiscall soldier, goggles item) puts the goggles on the FACE connector and, taking
// them off, leaves it empty - a beard / glasses / cigar would be gone. Replaced: the face piece under the goggles is
// kept and put back.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "core/pse_saves_format.h"
#include "features/features.h"
#include "loader.h"

namespace {
constexpr uint32_t kEnvironment = 0x63C948, kTextureManager = 0x63C930, kLevelName = 0x606880;
constexpr uint32_t kConnTable = 0x618490, kGearList = 0x5F9FF0, kGearListCount = 0x5F9FE8;
constexpr uint32_t kConnectorHash = 0x4B9C30, kModelHash = 0x4B9C40;
constexpr uint32_t kFindConnector = 0x502C30, kAttached = 0x502C10, kAttach = 0x502BF0;
constexpr uint32_t kFindModel = 0x4DD740, kLoadModel = 0x4F8BC0, kLoadRfx = 0x4E3D50, kLoadPending = 0x54A3E0;
constexpr uint32_t kArchiveClose = 0x4BD000, kArchiveOpen = 0x4BCF70, kArchiveReopen = 0x4BD060;
constexpr uint32_t kTexturesNow = 0x54A3B0, kControlFp = 0x569956, kLoadPrecision = 0x63C8C8;
constexpr uint32_t kGoggles = 0x435E20;

template <class T>
T& At(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }

const char* const kSlotConnector[features::kGearSlotCount] = {
    "BIP01 HAT CONNECTOR",     "BIP01 SPINE1 CONNECTOR",  "BIP01 FACE CONNECTOR",   "BIP01 SPINE2 CONNECTOR",
    "BIP01 SPINE2L CONNECTOR", "BIP01 SPINE2R CONNECTOR", "BIP01 HIPL CONNECTOR",   "BIP01 HIPR CONNECTOR",
    "BIP01 CHESTL CONNECTOR",  "BIP01 CHESTR CONNECTOR"};
constexpr int kPouchSpots = features::kGearSlotCount - features::kGearFirstPouch;
enum Kind { kKindHead, kKindBack, kKindFace, kKindPouch };   // what a slot takes
int KindOfSlot(int slot) { return slot < features::kGearFirstPouch ? slot : kKindPouch; }

// ---- names ----
struct Label {
    const char* model;
    const char* text;
};
// Shown names; models not listed get theirs from the model name.
const Label kLabels[] = {
    {"UK_CAP_GREEN", "GREEN CAP"},
    {"UK_HAT_DESERT", "BUSH HAT (DESERT)"},
    {"UK_HAT_CAMO", "BUSH HAT (CAMO)"},
    {"UK_WOOLLYHAT", "WOOLLY HAT"},
    {"UK_HELMET_DESERT", "BRITISH HELMET"},
    {"UK_SHEMAGH04_BLACKWHITE", "SHEMAGH (BLACK / WHITE)"},
    {"SAS_BERET_DRESS", "SAS BERET"},
    {"US_CAP_DESERT", "DESERT CAP"},
    {"US_HAT_DESERT", "US BUSH HAT"},
    {"US_HELMET_DESERT", "US HELMET (DESERT)"},
    {"US_HELMET_CAMO", "US HELMET (CAMO)"},
    {"US_BANDANA_SANDY", "BANDANA"},
    {"US_HELMET_M1ABRAMSCOMMANDER", "TANK COMMANDER HELMET"},
    {"US_HELMET_BLACKHAWKPILOT", "PILOT HELMET"},
    {"USCAP_DESERTSTORMCAMO", "DESERT STORM CAP"},
    {"USHAT_DESERTSTORMCAMO", "DESERT STORM BUSH HAT"},
    {"USCOMMANDER_HAT", "GENERAL'S CAP"},
    {"DELTAOFFICER_DRESSCAP", "OFFICER'S DRESS CAP"},
    {"DELTACOMMANDER_DRESSCAP", "COMMANDER'S DRESS CAP"},
    {"RU_FURHAT", "FUR HAT"},
    {"RU_HELMET_SANDY", "RUSSIAN HELMET"},
    {"RU_BERET_RED", "RUSSIAN BERET"},
    {"IR_HELMET_FLAG", "IRAQI HELMET (FLAG)"},
    {"IR_HELMET_WEBBING", "IRAQI HELMET (NET)"},
    {"IR_HELMET_GREEN", "IRAQI HELMET (GREEN)"},
    {"IR_HELMET_SANDY", "IRAQI HELMET (SAND)"},
    {"IR_SHEMAGH01_RED", "SHEMAGH (RED)"},
    {"IR_SHEMAGH02_REDWHITE", "SHEMAGH (RED / WHITE)"},
    {"IR_SHEMAGH03_BLACKWHITE", "SHEMAGH (BLACK)"},
    {"IR_SHEMAGH04_SANDY", "SHEMAGH (SAND)"},
    {"IR_SHEMAGH06_EMIR", "EMIR'S HEADDRESS"},
    {"IR_BERET_BLACK", "BERET (BLACK)"},
    {"IR_BERET_SANDY", "BERET (SAND)"},
    {"IR_BERET_RED", "BERET (RED)"},
    {"IRAQBERET_SANDY", "IRAQI BERET"},
    {"IRAQBERET_SADDAM", "SADDAM'S BERET"},
    {"IR_TANKDRIVERHAT", "TANK CREW CAP"},
    {"UK_BACKPACKMEDIUM", "SAS PACK"},
    {"UK_BACKPACKLARGE", "SAS PACK (LARGE)"},
    {"US_BACKPACKMEDIUM", "US PACK"},
    {"US_BACKPACKLARGE", "US PACK (LARGE)"},
    {"US_FRANKLIN_BACKPACKLARGE", "EXPERT'S PACK"},
    {"AN_RADIOPACK", "RADIO"},
    {"RU_BACKPACKMEDIUM", "RUSSIAN PACK"},
    {"RU_BACKPACKLARGE", "RUSSIAN PACK (LARGE)"},
    {"IR_BACKPACK_PLAIN", "IRAQI PACK (PLAIN)"},
    {"IR_BACKPACKMEDIUM", "IRAQI PACK"},
    {"IR_BACKPACKLARGE", "IRAQI PACK (LARGE)"},
    {"AN_SUNGLASSES", "SUNGLASSES"},
    {"AN_SPECTACLES", "GLASSES"},
    {"AN_CIGAR", "CIGAR"},
    {"UK_AMMOPOUCH01", "SAS AMMO POUCH"},
    {"UK_POUCHMEDIUM01", "SAS POUCH"},
    {"UK_BOTTLEPOUCH01", "SAS WATER BOTTLE"},
    {"US_HIPRPOUCH", "US HIP POUCH"},
    {"US_HIPLPOUCHES", "US HIP POUCHES"},
    {"US_HIPSATCHEL", "SATCHEL"},
    {"US_CHESTTORCH", "TORCH"},
    {"RU_AMMOPOUCH01", "RUSSIAN AMMO POUCH"},
    {"RU_BOTTLEPOUCH", "RUSSIAN WATER BOTTLE"},
    {"RU_HIPHOLSTER", "RUSSIAN HOLSTER"},
    {"IR_AMMOPOUCH01", "IRAQI AMMO POUCH"},
    {"IR_BOTTLEPOUCH", "IRAQI WATER BOTTLE"},
    {"IR_HIPHOLSTER", "IRAQI HOLSTER"},
    {"AN_WRENCH", "WRENCH"},
    {"AN_CHESTGRENADE", "GRENADE"},
};

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

// What kind of gear a model is, by its name (-1 = not offered: the goggles, truck parts).
int KindOf(const std::string& model) {
    const std::string m = Upper(model);
    for (const features::ModGearPiece& g : features::ModGear())   // a mod piece: its .gear file's slot
        if (_stricmp(g.model.c_str(), m.c_str()) == 0) return g.kind;
    auto has = [&](const char* s) { return m.find(s) != std::string::npos; };
    if (has("NIGHTVISION") || has("TRUCK")) return -1;
    if (has("POUCH") || has("HOLSTER") || has("SATCHEL") || has("TORCH") || has("WRENCH") || has("GRENADE"))
        return kKindPouch;
    if (has("BACKPACK") || has("RADIOPACK")) return kKindBack;
    if (has("SUNGLASSES") || has("SPECTACLES") || has("CIG") || has("MOUSTACHE") || has("BEARD")) return kKindFace;
    if (has("HAT") || has("HELMET") || has("CAP") || has("BERET") || has("SHEMAGH") || has("BANDANA")) return kKindHead;
    return -1;
}

// ---- which models can be loaded ----
uint32_t ArchiveHash(const std::string& name) {   // archive index hash (upper-cased name, as mods.cpp)
    uint32_t x = 1;
    for (unsigned char c : Upper(name))
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
            x = x << 1 | in;
        }
    return x;
}

const std::set<uint32_t>& CharacterArchive() {   // names in chardata.dat (archive index: {hash, offset, size} x n)
    static std::set<uint32_t> hashes;
    static bool read = false;
    if (read) return hashes;
    read = true;
    const std::string path = features::GameFile("chardata.dat");   // (or PSE\Originals)
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return hashes;
    std::vector<uint32_t> index(0x8000 / 4);
    DWORD got = 0;
    ReadFile(f, index.data(), 0x8000, &got, nullptr);
    CloseHandle(f);
    for (size_t i = 0; i + 2 < got / 4; i += 3)
        if (index[i + 2]) hashes.insert(index[i]);
    return hashes;
}

void* FindModel(const char* name) {
    void* env = *reinterpret_cast<void**>(kEnvironment);
    return env ? reinterpret_cast<void*(__thiscall*)(void*, const char*)>(kFindModel)(env, name) : nullptr;
}

bool Loadable(const std::string& model) {
    return CharacterArchive().count(ArchiveHash(model + ".EVO")) || features::ModHasFile((model + ".evo").c_str()) ||
           FindModel(model.c_str());
}

// The model, loaded if this level doesn't have it yet (from chardata.dat, or a mod's copy).
void* LoadGear(const std::string& model) {
    if (void* m = FindModel(model.c_str())) return m;
    if (!Loadable(model)) return nullptr;
    void* env = *reinterpret_cast<void**>(kEnvironment);
    void* textures = *reinterpret_cast<void**>(kTextureManager);
    if (!env || !textures) return nullptr;
    using ControlFp = uint32_t(__cdecl*)(uint32_t, uint32_t);
    const uint32_t fpu = reinterpret_cast<ControlFp>(kControlFp)(0, 0);
    reinterpret_cast<ControlFp>(kControlFp)(*reinterpret_cast<uint32_t*>(kLoadPrecision), 0xFFFFF);
    const int models = At<int>(env, 0x1B88);
    const int prev = reinterpret_cast<int(__cdecl*)()>(kArchiveClose)();
    reinterpret_cast<int(__cdecl*)(int, const char*, int, int)>(kArchiveOpen)(2, nullptr, 1, 1);   // chardata.dat
    reinterpret_cast<void(__thiscall*)(void*, int)>(kTexturesNow)(textures, 1);
    void* m = reinterpret_cast<void*(__thiscall*)(void*, int, const char*)>(kLoadModel)(env, -1, model.c_str());
    reinterpret_cast<void(__thiscall*)(void*, int)>(kTexturesNow)(textures, 0);
    if (m && At<int>(env, 0x1B88) != models)   // its normal map (bump list), as the network game's faction change
        reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(kLoadRfx)(env, "Equipment.rfx", 0);
    reinterpret_cast<int(__cdecl*)()>(kArchiveClose)();
    if (prev >= 0) reinterpret_cast<int(__cdecl*)(int)>(kArchiveReopen)(prev);
    reinterpret_cast<void(__thiscall*)(void*, void*, int)>(kLoadPending)(textures, nullptr, 0);
    reinterpret_cast<ControlFp>(kControlFp)(fpu, 0xFFFFF);
    dslog::Write(m ? "Gear: %s loaded" : "[fail] Gear: %s didn't load", model.c_str());
    return m;
}

// ---- connectors ----
uint32_t ModelHash(const char* name) { return reinterpret_cast<uint32_t(__cdecl*)(const char*)>(kModelHash)(name); }

int Connector(void* inst, uint32_t hash) {
    return reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(kFindConnector)(inst, hash);
}
int Connector(void* inst, const char* name) {
    return Connector(inst, reinterpret_cast<uint32_t(__cdecl*)(const char*)>(kConnectorHash)(name));
}
void* Attached(void* inst, int index) {
    return reinterpret_cast<void*(__thiscall*)(void*, int)>(kAttached)(inst, index);
}
void Attach(void* inst, int index, void* model) {
    reinterpret_cast<void(__thiscall*)(void*, int, void*)>(kAttach)(inst, index, model);
    if (model) At<uint32_t>(inst, 0x21C) |= 0x800;
}

// Riding a vehicle (not a tripod gun): the back piece waits in soldier +0x2AFC.
bool BackParked(void* soldier) {
    auto* v = At<uint8_t*>(soldier, 0x267C);
    if (!v) return false;
    auto* unit = At<uint8_t*>(v, 0x24);
    auto* type = unit ? At<uint8_t*>(unit, 0x80) : nullptr;
    auto* kind = type ? At<uint8_t*>(type, 0x44) : nullptr;
    return !kind || At<int>(kind, 0x24) != 0xB;
}

// Face piece under the night-vision goggles, per soldier (this level's soldiers / models only).
std::map<void*, void*> g_underGoggles;
char g_level[32] = {};

void NewLevelCheck() {
    if (strncmp(g_level, reinterpret_cast<const char*>(kLevelName), sizeof g_level - 1) != 0) {
        strncpy_s(g_level, reinterpret_cast<const char*>(kLevelName), _TRUNCATE);
        g_underGoggles.clear();
    }
}

bool GogglesOn(void* soldier) { return At<void*>(soldier, 0x2668) != nullptr; }

void __fastcall Goggles(void* soldier, void* /*edx*/, void* item) {
    NewLevelCheck();
    void* inst = At<void*>(soldier, 0x2C);
    const int face = inst ? Connector(inst, kSlotConnector[features::kGearFace]) : -1;
    if (face < 0) {
        At<void*>(soldier, 0x2668) = item;
        return;
    }
    if (item) {
        void* goggles = At<void*>(At<void*>(item, 0x2C), 0xC8);
        if (!GogglesOn(soldier)) {
            void* under = Attached(inst, face);
            if (under && under != goggles) g_underGoggles[soldier] = under;
            else g_underGoggles.erase(soldier);
        }
        reinterpret_cast<void(__thiscall*)(void*, int, void*)>(kAttach)(inst, face, goggles);
    } else {
        auto it = g_underGoggles.find(soldier);
        Attach(inst, face, it == g_underGoggles.end() ? nullptr : it->second);
        if (it != g_underGoggles.end()) g_underGoggles.erase(it);
    }
    At<void*>(soldier, 0x2668) = item;
}

// Every connector gear goes on (the gear table's + the rows'), by name - for saves.
const char* const kGearConnectors[] = {"BIP01 HAT CONNECTOR", "BIP01 FACE CONNECTOR", "BIP01 SPINE1 CONNECTOR",
                                       "BIP01 SPINE2 CONNECTOR", "BIP01 SPINE2L CONNECTOR", "BIP01 SPINE2R CONNECTOR",
                                       "BIP01 HIPL CONNECTOR", "BIP01 HIPR CONNECTOR", "BIP01 CHESTL CONNECTOR",
                                       "BIP01 CHESTR CONNECTOR"};

// What hangs on a connector, the face piece under the goggles and the parked back piece included.
void* WornAt(void* soldier, void* inst, int index) {
    if (index == Connector(inst, kSlotConnector[features::kGearFace]) && GogglesOn(soldier)) {
        auto it = g_underGoggles.find(soldier);
        return it == g_underGoggles.end() ? nullptr : it->second;
    }
    if (index == Connector(inst, kSlotConnector[features::kGearBack]) && BackParked(soldier)) return At<void*>(soldier, 0x2AFC);
    return Attached(inst, index);
}

void PutAt(void* soldier, void* inst, int index, void* model) {
    if (index == Connector(inst, kSlotConnector[features::kGearFace]) && GogglesOn(soldier)) {
        if (model) g_underGoggles[soldier] = model;
        else g_underGoggles.erase(soldier);
    } else if (index == Connector(inst, kSlotConnector[features::kGearBack]) && BackParked(soldier)) {
        At<void*>(soldier, 0x2AFC) = model;
    } else {
        Attach(inst, index, model);
    }
}

// Saved gear handed back once the loaded level plays (by role; models load then, not inside the level load).
std::map<int, std::vector<features::GearPiece>> g_pendingSave;

// Every model name the gear rows know: the game's attachment list, then the gear table's.
const std::vector<std::string>& KnownModels() {
    static std::vector<std::string> names;
    static int generation = -1;
    if (generation == features::ModsGeneration() && !names.empty()) return names;
    generation = features::ModsGeneration();
    names.clear();
    auto add = [&](const char* n) {
        if (!n || !*n) return;
        const std::string u = Upper(n);
        if (std::find(names.begin(), names.end(), u) == names.end()) names.push_back(u);
    };
    const int count = *reinterpret_cast<int*>(kGearListCount);
    for (int i = 0; i < count && i < 256; ++i) add(reinterpret_cast<const char* const*>(kGearList)[i]);
    for (int k = 0; k < 16 * 6; ++k) add(reinterpret_cast<const char*>(kConnTable + k * 0x28 + 4));
    for (const features::ModGearPiece& g : features::ModGear()) add(g.model.c_str());
    return names;
}

std::string NameOf(void* model) {   // "" = nothing, "?" = a model the gear lists don't have
    if (!model) return "";
    const uint32_t hash = At<uint32_t>(model, 0x1BC);
    for (const std::string& m : KnownModels())
        if (ModelHash(m.c_str()) == hash) return m;
    return "?";
}
}  // namespace

std::vector<std::string> features::GearChoices(int slot) {
    std::vector<std::string> out = {""};   // none
    for (const std::string& m : KnownModels())
        if (KindOf(m) == KindOfSlot(slot) && Loadable(m)) out.push_back(m);
    return out;
}

std::string features::GearOn(void* soldier, int slot) {
    if (!soldier || slot < 0 || slot >= kGearSlotCount) return "?";
    NewLevelCheck();
    void* inst = At<void*>(soldier, 0x2C);
    const int index = inst ? Connector(inst, kSlotConnector[slot]) : -1;
    if (index < 0) return "?";
    return NameOf(WornAt(soldier, inst, index));
}

bool features::SetGear(void* soldier, int slot, const std::string& model) {
    if (!soldier || slot < 0 || slot >= kGearSlotCount) return false;
    NewLevelCheck();
    void* inst = At<void*>(soldier, 0x2C);
    const int index = inst ? Connector(inst, kSlotConnector[slot]) : -1;
    if (index < 0) return false;
    void* m = nullptr;
    if (!model.empty() && !(m = LoadGear(model))) return false;
    PutAt(soldier, inst, index, m);   // (under the goggles / parked while riding: the game puts it on later)
    return true;
}

// The faction's standard gear for the role (the gear table): every connector the table uses is emptied first, so
// nothing of the old faction stays.
void features::FactionGear(void* soldier, int role, int side) {
    if (!soldier || role < 0 || role > 3 || side < 0 || side > 3) return;
    void* inst = At<void*>(soldier, 0x2C);
    if (!inst) return;
    std::set<uint32_t> connectors;
    for (int k = 0; k < 16 * 6; ++k)
        if (const uint32_t h = *reinterpret_cast<uint32_t*>(kConnTable + k * 0x28)) connectors.insert(h);
    for (uint32_t h : connectors)
        if (const int index = Connector(inst, h); index >= 0) PutAt(soldier, inst, index, nullptr);
    for (int e = 0; e < 6; ++e) {
        const uint8_t* entry = reinterpret_cast<const uint8_t*>(kConnTable + ((role * 4 + side) * 6 + e) * 0x28);
        const uint32_t h = *reinterpret_cast<const uint32_t*>(entry);
        const char* name = reinterpret_cast<const char*>(entry + 4);
        if (!h || !*name) continue;
        const int index = Connector(inst, h);
        if (index < 0) continue;
        if (void* m = LoadGear(name)) PutAt(soldier, inst, index, m);
    }
    dslog::Write("Gear: role %d -> faction %d's gear", role, side);
}

// ---- pouch sets ----
namespace {
struct PouchSet {
    std::string label;
    std::string model[kPouchSpots];   // "" = nothing on that spot
};

// One set per faction / role from the gear table (pouch spots only), identical ones once, "NONE" first; a set whose
// models can't all be loaded here is left out.
const std::vector<PouchSet>& PouchSets() {
    static std::vector<PouchSet> sets;
    static int generation = -1;
    static std::string level;   // (a piece only some levels have decides per level)
    const std::string now = reinterpret_cast<const char*>(kLevelName);
    if (generation == features::ModsGeneration() && level == now && !sets.empty()) return sets;
    generation = features::ModsGeneration();
    level = now;
    sets.clear();
    sets.push_back({"NONE", {}});
    static const char* const kSides[4] = {"SAS", "US", "RUSSIAN", "IRAQI"};
    static const char* const kRoles[4] = {"RIFLEMAN", "SNIPER", "HEAVY WEAPONS", "ENGINEER"};
    uint32_t spot[kPouchSpots];
    for (int k = 0; k < kPouchSpots; ++k)
        spot[k] = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(kConnectorHash)(kSlotConnector[features::kGearFirstPouch + k]);
    for (int side = 0; side < 4; ++side)
        for (int role = 0; role < 4; ++role) {
            PouchSet s{std::string(kSides[side]) + " " + kRoles[role], {}};
            bool any = false, ok = true;
            for (int e = 0; e < 6; ++e) {
                const uint8_t* entry = reinterpret_cast<const uint8_t*>(kConnTable + ((role * 4 + side) * 6 + e) * 0x28);
                const uint32_t h = *reinterpret_cast<const uint32_t*>(entry);
                const char* name = reinterpret_cast<const char*>(entry + 4);
                for (int k = 0; k < kPouchSpots; ++k)
                    if (h && h == spot[k] && *name) {
                        s.model[k] = Upper(name);
                        any = true;
                        ok = ok && Loadable(s.model[k]);
                    }
            }
            if (!any || !ok) continue;
            bool dup = false;
            for (const PouchSet& o : sets)
                dup = dup || std::equal(std::begin(o.model), std::end(o.model), std::begin(s.model));
            if (!dup) sets.push_back(s);
        }
    for (const features::ModPouchSet& m : features::ModPouchSets()) {   // .pouches files
        PouchSet s{m.name, {}};
        bool ok = true;
        for (int k = 0; k < kPouchSpots; ++k) {
            s.model[k] = m.model[k];
            if (!s.model[k].empty() && !Loadable(s.model[k])) {
                features::ModLog("[fail] Mods: pouch set %s left out here - %s isn't in chardata.dat or a mod, and this level "
                                 "doesn't have it", m.name.c_str(), s.model[k].c_str());
                ok = false;
            }
        }
        if (ok) sets.push_back(s);
    }
    return sets;
}
}  // namespace

int features::PouchSetCount() { return static_cast<int>(PouchSets().size()); }

std::string features::PouchSetLabel(int set) {
    return set >= 0 && set < PouchSetCount() ? PouchSets()[set].label : "CUSTOM";
}

int features::PouchSetOn(void* soldier) {
    std::string on[kPouchSpots];
    for (int k = 0; k < kPouchSpots; ++k) on[k] = GearOn(soldier, kGearFirstPouch + k);
    for (int i = 0; i < PouchSetCount(); ++i) {
        bool same = true;
        for (int k = 0; k < kPouchSpots && same; ++k) same = _stricmp(on[k].c_str(), PouchSets()[i].model[k].c_str()) == 0;
        if (same) return i;
    }
    return -1;
}

bool features::SetPouchSet(void* soldier, int set) {
    if (set < 0 || set >= PouchSetCount()) return false;
    bool ok = true;
    for (int k = 0; k < kPouchSpots; ++k) ok = SetGear(soldier, kGearFirstPouch + k, PouchSets()[set].model[k]) && ok;
    return ok;
}

std::vector<features::GearPiece> features::GearSnapshot(void* soldier) {
    std::vector<GearPiece> out;
    NewLevelCheck();
    void* inst = soldier ? At<void*>(soldier, 0x2C) : nullptr;
    if (!inst) return out;
    for (const char* c : kGearConnectors) {
        const int index = Connector(inst, c);
        if (index < 0) continue;
        const std::string m = NameOf(WornAt(soldier, inst, index));
        if (m != "?") out.push_back({c, m});   // (a model we can't name is left as the level has it)
    }
    return out;
}

void features::ApplyGearSnapshot(void* soldier, const std::vector<GearPiece>& pieces) {
    NewLevelCheck();
    void* inst = soldier ? At<void*>(soldier, 0x2C) : nullptr;
    if (!inst) return;
    for (const GearPiece& p : pieces) {
        const int index = Connector(inst, p.connector.c_str());
        if (index < 0) continue;
        if (p.model.empty()) PutAt(soldier, inst, index, nullptr);
        else if (void* m = LoadGear(p.model)) PutAt(soldier, inst, index, m);
    }
}

void features::SetSavedGear(int role, std::vector<GearPiece> pieces) {
    if (pieces.empty()) g_pendingSave.erase(role);
    else g_pendingSave[role] = std::move(pieces);
}

// A loaded save's gear back on the squad on the first frame the level plays.
void features::OnFrameGear() {
    if (g_pendingSave.empty() || *reinterpret_cast<int8_t*>(0x60EC8C) != 0) return;   // dispatch mode 0 = playing
    auto mgr = *reinterpret_cast<const uint8_t* const*>(0x63C9A0);
    if (!mgr) return;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(0x5F9B94);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s; s = *reinterpret_cast<uintptr_t*>(s + 0x14)) {
        auto* soldier = reinterpret_cast<uint8_t*>(s);
        auto* unit = At<uint8_t*>(soldier, 0x24);
        const int role = unit ? *reinterpret_cast<int8_t*>(unit + 0x1C7) : -1;
        auto it = g_pendingSave.find(role);
        if (it == g_pendingSave.end()) continue;
        ApplyGearSnapshot(soldier, it->second);
        dslog::Write("Gear: role %d's saved gear back on (%zu pieces)", role, it->second.size());
    }
    g_pendingSave.clear();
}

// ---- co-save (dsfix.gear, chunk 'GEAR' v1 - core/pse_saves_format.h) ----
// The squad's gear when the game last saved (captured in GearSave: the game layer runs the save callbacks right after
// the game wrote a save file - where loadout.cpp's old save hook captured it) or from the loaded save's co-save; put
// back on by the save restore (GearRestoreSaved -> SetSavedGear -> OnFrameGear).
namespace {
std::map<int, std::vector<features::GearPiece>> g_savedGear;   // by role

const PSESerializationInterface* Ser() { return pse::SerializationInterface(); }

void GearSave(PSESaveWriter* w) {
    features::GearCaptureForSave();
    if (g_savedGear.empty()) return;
    pse_saves::SavedGear out;
    for (const auto& [role, pieces] : g_savedGear)
        for (const features::GearPiece& g : pieces) out[role].push_back({g.connector, g.model});
    const std::vector<uint8_t> b = pse_saves::EncodeGear(out);
    Ser()->WriteChunk(w, pse_saves::kGearTag, pse_saves::kChunkVersion, b.data(), static_cast<uint32_t>(b.size()));
}
void GearLoadChunk(uint32_t tag, uint32_t version, const std::vector<uint8_t>& b) {
    if (tag != pse_saves::kGearTag || version != 1) return;
    pse_saves::SavedGear in;
    if (!pse_saves::DecodeGear(b.data(), b.size(), &in)) dslog::Write("[fail] Gear: the save's gear chunk is damaged");
    for (const auto& [role, pieces] : in)
        for (const pse_saves::SavedGearPiece& g : pieces) g_savedGear[role].push_back({g.connector, g.model});
}
void GearClear() { g_savedGear.clear(); }
}  // namespace

void features::GearCaptureForSave() {
    g_savedGear.clear();
    auto mgr = *reinterpret_cast<const uint8_t* const*>(0x63C9A0);
    if (!mgr) return;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(0x5F9B94);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s; s = *reinterpret_cast<uintptr_t*>(s + 0x14)) {
        auto* soldier = reinterpret_cast<uint8_t*>(s);
        auto* unit = At<uint8_t*>(soldier, 0x24);
        const int role = unit ? *reinterpret_cast<int8_t*>(unit + 0x1C7) : -1;
        if (role >= 0 && role <= 3) g_savedGear[role] = GearSnapshot(soldier);
    }
}

void features::GearRestoreSaved() {
    for (const auto& [role, pieces] : g_savedGear) SetSavedGear(role, pieces);
}

// Its GEAR chunk goes through loadout.cpp's co-save callbacks (one set per plugin: dsfix.loadout writes KITS, GEAR and
// VEHS; saves from before 2026-10-25 have GEAR under dsfix.gear - the manifest adopts that name).
const features::CosavePart features::kGearCosave = {&GearSave, &GearClear, &GearLoadChunk, &GearClear};

std::string features::GearLabel(const std::string& model) {
    if (model.empty()) return "NONE";
    if (model == "?") return "OTHER";
    for (const ModGearPiece& g : ModGear())
        if (_stricmp(g.model.c_str(), model.c_str()) == 0) return g.name;
    for (const Label& l : kLabels)
        if (_stricmp(l.model, model.c_str()) == 0) return l.text;
    // IR_BEARD02_SUNGLASSES_CIG -> BEARD 2 + SUNGLASSES + CIGARETTE
    std::string s = Upper(model);
    for (const char* prefix : {"UK_", "US_", "IR_", "RU_", "AN_", "SAS_"})
        if (s.rfind(prefix, 0) == 0) {
            s = s.substr(strlen(prefix));
            break;
        }
    std::string out;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find('_', a);
        if (b == std::string::npos) b = s.size();
        std::string w = s.substr(a, b - a);
        if (w == "CIG") w = "CIGARETTE";
        size_t d = w.find_first_of("0123456789");
        if (d != std::string::npos && d > 0) {   // BEARD02 -> BEARD 2
            const int n = atoi(w.c_str() + d);
            w = w.substr(0, d) + " " + std::to_string(n);
        }
        if (!w.empty()) out += (out.empty() ? "" : " + ") + w;
        a = b + 1;
    }
    return out;
}

void features::ApplyGear() {
    static const uint8_t kEntry[] = {0x56, 0x8B, 0xF1, 0x57, 0x68, 0xFC, 0x9D, 0x5E, 0x00};   // push esi; mov esi,ecx; ...
    if (!patch::Matches(kGoggles, kEntry, sizeof kEntry) || !patch::WriteJump(kGoggles, reinterpret_cast<const void*>(&Goggles)))
        dslog::Write("[fail] Gear: night-vision hook");
}
