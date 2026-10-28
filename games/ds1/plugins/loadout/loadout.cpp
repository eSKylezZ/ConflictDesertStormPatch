// Loadouts: the squad's starting weapons, chosen by the player on the Squad Loadout screen.
//
// Single-player kits are level data. Each mission's scene file (text, "Version 72", in missionN.dat) has one HLOI line
// per placed character; a squad member's line carries his starting kit:
//   HLOI <flags> "HERO01_ARMSTRONG" <parent> "NOTEXTURE" <pos> <rot> 0 4 4 "Bradley" 1 0 "HERO_BRADLEY" ... "NONE"
//        0 0 -1000.00 -1000.00 5 "US_WPN_M16A2" 5 10 "US_WPN_SIGP228" 5 5 ... -1 1.000000 ...
// = after the 6th quoted token: <count> x {"<item>" <type> <magazines>}, then -1 (holds for all 3460 HLOI lines).
// The playable soldier is the line whose unit is Bradley / Foley / Jones / Ramirez (= Connors), optionally
// "_Training", and whose instance name doesn't start with "CS_" (cutscene actors, flag 0x20000) - one per soldier.
//
// The level loader reads the scene into memory with FUN_004bfb90(name, &buf, &size, 0, 6) at 0x40B606 and parses it
// with FUN_004f8d00 right after; that call is hooked and the squad's item lists are rewritten in the buffer first.
// A longer text gets a new buffer from the game's allocator (FUN_004b8450(size, type)); the old one is freed with
// FUN_004b8590 - the caller frees whatever *buf holds after parsing.
#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "core/pse_saves_format.h"
#include "core/settings.h"
#include "loader.h"
#include "features/features.h"
#include "features/kit_weapons.h"

namespace {
constexpr uint32_t kSceneRead = 0x40B606;  // call FUN_004bfb90 in the level loader
constexpr uint32_t kReadFile = 0x4BFB90;   // cdecl(name, char** buf, int* size, int, int type) -> 1 ok
constexpr uint32_t kAlloc = 0x4B8450;      // cdecl(size, type) -> memory
constexpr uint32_t kFree = 0x4B8590;       // cdecl(memory)

using ReadFileFn = int(__cdecl*)(const char*, char**, int*, int, int);
using AllocFn = char*(__cdecl*)(int, int);
using FreeFn = void(__cdecl*)(void*);

const char* const kSquad[] = {"Bradley", "Foley", "Jones", "Ramirez"};

// The squad's kit weapons: features/kit_weapons.h (shared with dsfix.dll's KitWeaponIds).
using Kind = KitKind;
using Weapon = KitWeapon;
constexpr const auto& kWeapons = kKitWeapons;

// Weapons the list above doesn't know (added by mods): from Weaps.txt as the game loads it (mods.cpp merges mod rows
// in) - column 1 name, 2 model, 6 category.
Kind KindOfCategory(const std::string& c) {
    if (c == "PISTOL") return Kind::Pistol;
    if (c == "ANTI TANK") return Kind::Launcher;
    if (c == "GRENADE" || c == "SMOKE GRENADE") return Kind::Grenade;
    if (c == "ASSAULT RIFLE" || c == "MACHINE GUN" || c == "SHOTGUN" || c == "SNIPER RIFLE" || c == "SUB-MACHINE GUN" ||
        c == "GRENADE LAUNCH/RIFLE")
        return Kind::Main;
    return Kind::Other;
}

const Weapon* FindTableWeapon(const std::string& item) {
    struct Entry {
        std::string item, model;
        Weapon w;
    };
    static std::vector<std::unique_ptr<Entry>> table;
    static bool loaded = false;
    static int generation = -1;
    if (generation != features::ModsGeneration()) {  // mods reloaded: the merged Weaps.txt changed
        generation = features::ModsGeneration();
        table.clear();
        loaded = false;
    }
    if (!loaded) {
        loaded = true;
        std::string text;
        if (features::ReadGameTable("Weaps.txt", text))
            for (size_t pos = 0; pos < text.size();) {
                size_t nl = text.find('\n', pos);
                if (nl == std::string::npos) nl = text.size();
                std::vector<std::string> col;
                for (size_t c = pos; c <= nl && col.size() < 7;) {
                    size_t e = text.find(',', c);
                    if (e == std::string::npos || e > nl) e = nl;
                    col.push_back(text.substr(c, e - c));
                    c = e + 1;
                }
                if (col.size() >= 6) {
                    auto e = std::make_unique<Entry>();
                    e->item = col[0], e->model = col[1];
                    // No inventory picture (column 5 = -1): vehicle guns and the support items that are carried like
                    // weapons - the airstrike / mortar strike designators (Missions 5A, 5B, 7), US_WPN_LAW80_HELI
                    // (Mission 3) - all filed as ANTI TANK. Never a kit choice: the swap would hand out or take away
                    // a story item.
                    // Hand weapons hang on the right-hand bone (column 7 "Bip01 GunR Connector"); anything else is a
                    // vehicle / emplacement gun (Connector_Weapon01, _NSVT, _HMG, ...). The game's own have pictures
                    // too, and converted ones (PSE\Ports, PSE\Library) inherit a hand weapon's category - picked, they
                    // were a cannon-sized model with no hand animations that couldn't be aimed. Converted script props
                    // ("FAKE" / "TEST" items, projectiles: DS2's US_WPN_Fake_Mortar = an M16 model filed as a pistol)
                    // aren't weapons a soldier carries either.
                    std::string id = col[0];
                    for (char& ch : id) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
                    const bool hand = col.size() < 7 || _stricmp(col[6].c_str(), "Bip01 GunR Connector") == 0;
                    const bool prop = id.find("FAKE") != std::string::npos || id.find("_TEST_") != std::string::npos ||
                                      id.find("PROJECTILE") != std::string::npos;
                    e->w = {nullptr, nullptr,
                            col[4] == "-1" || !hand || prop ? Kind::Other : KindOfCategory(col[5])};
                    table.push_back(std::move(e));
                    table.back()->w.item = table.back()->item.c_str();
                    table.back()->w.model = table.back()->model.c_str();
                }
                pos = nl + 1;
            }
    }
    for (const auto& e : table)
        if (_stricmp(item.c_str(), e->w.item) == 0) return &e->w;
    return nullptr;
}

const Weapon* FindWeapon(const std::string& item) {
    for (const Weapon& w : kWeapons)
        if (_stricmp(item.c_str(), w.item) == 0) return &w;
    return FindTableWeapon(item);
}
Kind KindOf(const std::string& item) {
    const Weapon* w = FindWeapon(item);
    return w ? w->kind : Kind::Other;
}

// Archive name hash (FUN_004b9a10 over the upper-cased file name): LFSR, start 1, per bit LSB first.
uint32_t NameHash(const std::string& name) {
    uint32_t x = 1;
    for (char ch : name) {
        const uint8_t c = static_cast<uint8_t>(toupper(static_cast<uint8_t>(ch)));
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) & 1) ^ (x & 1) ^ ((x >> 1) & 1) ^ ((x >> 21) & 1) ^ (x >> 31);
            x = (x << 1) | in;
        }
    }
    return x;
}

// Name hashes in the current level's archive (<level>.dat next to the exe: {hash, offset, size} x n up to offset 0).
std::vector<uint32_t> g_levelFiles;

void LoadLevelDirectory(const char* scene) {
    g_levelFiles.clear();
    std::string base = scene;
    const size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    const size_t dot = base.find('.');
    if (dot != std::string::npos) base.resize(dot);
    const std::string path = features::GameFile(base + ".dat");   // (or PSE\Originals)
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    uint32_t head[2] = {};
    DWORD got = 0;
    if (ReadFile(f, head, sizeof head, &got, nullptr) && got == sizeof head && head[1] >= 12 && head[1] <= 0x100000) {
        std::vector<uint32_t> dir(head[1] / 4);
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        if (ReadFile(f, dir.data(), head[1], &got, nullptr) && got == head[1])
            for (size_t i = 0; i + 2 < dir.size() && (dir[i] || dir[i + 1]); i += 3) g_levelFiles.push_back(dir[i]);
    }
    CloseHandle(f);
    dslog::Write("Loadout: %s.dat has %u files", base.c_str(), static_cast<unsigned>(g_levelFiles.size()));
}

// The weapon can be used in this level (its model is in the level's archive); unknown items: yes.
bool Available(const std::string& item) {
    const Weapon* w = FindWeapon(item);
    if (!w || g_levelFiles.empty()) return true;
    if (features::ModHasFile((std::string(w->model) + ".evo").c_str())) return true;
    if (features::AssetStoreHasFile((std::string(w->model) + ".EVO").c_str())) return true;   // another level's (store)
    const uint32_t h = NameHash(std::string(w->model) + ".EVO");
    for (uint32_t f : g_levelFiles)
        if (f == h) return true;
    return false;
}

struct Token {
    size_t begin, end;  // in the line; quoted tokens include the quotes
    bool quoted;
};

std::vector<Token> Tokenize(const std::string& line) {
    std::vector<Token> t;
    for (size_t i = 0; i < line.size();) {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r') { ++i; continue; }
        if (line[i] == '"') {
            const size_t e = line.find('"', i + 1);
            if (e == std::string::npos) break;
            t.push_back({i, e + 1, true});
            i = e + 1;
        } else {
            size_t e = i;
            while (e < line.size() && line[e] != ' ' && line[e] != '\t' && line[e] != '\r') ++e;
            t.push_back({i, e, false});
            i = e;
        }
    }
    return t;
}

bool IsInt(const std::string& s, int* out = nullptr) {
    if (s.empty()) return false;
    char* end;
    const long v = strtol(s.c_str(), &end, 10);
    if (*end) return false;
    if (out) *out = static_cast<int>(v);
    return true;
}

struct Kit {
    size_t countTok;           // token index of <count>
    std::vector<size_t> item;  // token index of each item name
};

bool FindKit(const std::string& line, const std::vector<Token>& t, Kit& kit) {
    auto text = [&](size_t i) { return line.substr(t[i].begin, t[i].end - t[i].begin); };
    size_t quoted = 0, start = 0;
    for (size_t i = 0; i < t.size() && !start; ++i)
        if (t[i].quoted && ++quoted == 6) start = i + 1;
    if (!start) return false;
    for (size_t i = start; i < t.size(); ++i) {
        int n = 0;
        if (t[i].quoted || !IsInt(text(i), &n) || n < 0) continue;
        bool ok = i + 1 + 3 * n < t.size();
        for (int k = 0; ok && k < n; ++k) {
            const size_t a = i + 1 + 3 * k;
            ok = t[a].quoted && IsInt(text(a + 1)) && IsInt(text(a + 2));
        }
        if (ok && text(i + 1 + 3 * n) == "-1") {
            kit.countTok = i;
            kit.item.clear();
            for (int k = 0; k < n; ++k) kit.item.push_back(i + 1 + 3 * k);
            return true;
        }
    }
    return false;
}

// Squad member of a playable HLOI line (0-3), or -1.
int SquadMember(const std::string& line, const std::vector<Token>& t) {
    if (line.compare(0, 5, "HLOI ") != 0 || t.size() < 2) return -1;
    if (strtoul(line.c_str() + 5, nullptr, 10) & 0x20000) return -1;  // cutscene actor
    std::vector<std::string> q;
    for (const Token& k : t)
        if (k.quoted) q.push_back(line.substr(k.begin + 1, k.end - k.begin - 2));
    if (q.size() < 5 || _strnicmp(q[4].c_str(), "CS_", 3) == 0) return -1;
    for (int s = 0; s < 4; ++s) {
        const size_t n = strlen(kSquad[s]);
        if (_strnicmp(q[3].c_str(), kSquad[s], n) == 0 && (q[3].size() == n || _stricmp(q[3].c_str() + n, "_Training") == 0))
            return s;
    }
    return -1;
}

// Profiles: Default = the mission's own kit, optionally with each soldier's main weapon swapped; PistolOnly ("007")
// = no main weapon, launchers or grenades, a pistol (added if the kit has none); mission items stay in every profile.
enum class Profile { Default, PistolOnly };

struct Rules {
    Profile profile = Profile::Default;
    std::string mainWeapon[4];  // per squad member, empty = unchanged
};
Rules g_rules;
bool g_fillers = false;  // this level's kits hold 0-magazine duplicate pistols (see ApplyRules)

// Dev test for now: HKCU\Software\DesertStormFix\Dev\LoadoutTest = "Profile=007;Bradley=US_WPN_MP5SilencedSubMG;..."
void LoadRules() {
    g_rules = {};
#ifndef DS_DIST
    char buf[512] = {};
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "LoadoutTest", RRF_RT_REG_SZ, nullptr, buf,
                     &size) != ERROR_SUCCESS)
        return;
    for (char* part = strtok(buf, ";"); part; part = strtok(nullptr, ";")) {
        char* eq = strchr(part, '=');
        if (!eq) continue;
        *eq = 0;
        if (_stricmp(part, "Profile") == 0 && _stricmp(eq + 1, "007") == 0) g_rules.profile = Profile::PistolOnly;
        for (int s = 0; s < 4; ++s)
            if (_stricmp(part, kSquad[s]) == 0) g_rules.mainWeapon[s] = eq + 1;
    }
#endif
}

bool AnyRule() {
    if (g_rules.profile != Profile::Default) return true;
    for (const auto& w : g_rules.mainWeapon)
        if (!w.empty()) return true;
    return false;
}

struct Item {
    std::string name;
    std::string type, mags;  // kept as written
};

void ApplyRules(int s, std::vector<Item>& kit) {
    if (kit.empty()) return;  // starts unarmed (captive Foley in Mission 1) - stays that way
    if (g_rules.profile == Profile::PistolOnly) {
        // Item count stays the same: every kit item becomes a world object with the next object id, and the
        // mission scripts address objects by id - a shorter kit shifts them and the level crashes (0x4D081A).
        // Dropped items become 0-magazine copies of the pistol (merged into it by AddItem as duplicates).
        const Item* own = nullptr;
        for (const Item& it : kit)
            if (KindOf(it.name) == Kind::Pistol) { own = &it; break; }
        // No pistol in the kit: the first dropped slot becomes the SIG P228 (Beretta if the level lacks the SIG).
        std::string pistol = own ? own->name : Available("US_WPN_SIGP228") ? "US_WPN_SIGP228" : "US_WPN_Beretta92F";
        bool needPistol = !own;
        for (Item& it : kit) {
            if (&it == own) continue;
            const Kind k = KindOf(it.name);
            if (k != Kind::Main && k != Kind::Launcher && k != Kind::Grenade && k != Kind::Pistol) continue;
            it = Item{pistol, it.type, needPistol ? "5" : "0"};
            g_fillers |= !needPistol;
            needPistol = false;
        }
        return;
    }
    const std::string& want = g_rules.mainWeapon[s];
    if (want.empty()) return;
    if (!Available(want)) {
        dslog::Write("Loadout: %s - %s isn't in this level's archive, kit unchanged", kSquad[s], want.c_str());
        return;
    }
    for (Item& it : kit)
        if (KindOf(it.name) == Kind::Main) {
            dslog::Write("Loadout: %s main weapon %s -> %s", kSquad[s], it.name.c_str(), want.c_str());
            it.name = want;
            return;
        }
    dslog::Write("Loadout: %s has no main weapon in this mission's kit", kSquad[s]);
}

// Rewrites one HLOI line; returns true if it changed.
bool RewriteLine(std::string& line) {
    const auto t = Tokenize(line);
    const int s = SquadMember(line, t);
    if (s < 0) return false;
    Kit kit;
    if (!FindKit(line, t, kit)) {
        dslog::Write("Loadout: %s - kit not found", kSquad[s]);
        return false;
    }
    std::vector<Item> items;
    for (size_t i : kit.item)
        items.push_back({line.substr(t[i].begin + 1, t[i].end - t[i].begin - 2),
                         line.substr(t[i + 1].begin, t[i + 1].end - t[i + 1].begin),
                         line.substr(t[i + 2].begin, t[i + 2].end - t[i + 2].begin)});
    std::string before;
    for (const Item& it : items) before += it.name + " ";
    ApplyRules(s, items);
    std::string list = std::to_string(items.size()), after;
    for (const Item& it : items) {
        list += " \"" + it.name + "\" " + it.type + " " + it.mags;
        after += it.name + " ";
    }
    if (after == before) return false;
    const size_t from = t[kit.countTok].begin;
    const size_t to = kit.item.empty() ? t[kit.countTok].end : t[kit.item.back() + 2].end;
    line.replace(from, to - from, list);
    dslog::Write("Loadout: %s kit: %s-> %s", kSquad[s], before.c_str(), after.c_str());
    return true;
}

void ForgetWeaponRecord();  // runtime spawn section below

int __cdecl ReadScene(const char* name, char** buf, int* size, int a4, int type) {
    const int r = reinterpret_cast<ReadFileFn>(kReadFile)(name, buf, size, a4, type);
    if (r != 1 || !*buf || *size < 8 || memcmp(*buf, "Version", 7) != 0) return r;
    g_fillers = false;
    ForgetWeaponRecord();  // the previous level's record points into freed data (front-end -> Mission 1 crashed)
    LoadRules();
    LoadLevelDirectory(name);  // also what the customise screen may hand out (Available)
    if (!AnyRule()) return r;

    std::string text(*buf, *size), out;
    out.reserve(text.size() + 1024);
    bool changed = false;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        nl = nl == std::string::npos ? text.size() : nl + 1;
        std::string line = text.substr(pos, nl - pos);
        if (line.compare(0, 5, "HLOI ") == 0) changed |= RewriteLine(line);
        out += line;
        pos = nl;
    }
    if (!changed) return r;
    char* mem = reinterpret_cast<AllocFn>(kAlloc)(static_cast<int>(out.size()) + 1, type);
    if (!mem) return r;
    memcpy(mem, out.data(), out.size());
    mem[out.size()] = 0;
    reinterpret_cast<FreeFn>(kFree)(*buf);
    *buf = mem;
    *size = static_cast<int>(out.size());
    dslog::Write("Loadout: scene %s rewritten (%d bytes)", name, *size);
    return r;
}
}  // namespace

void ApplyRuntimeSpawn();
void ApplyUnlocks();
void ApplySaveKits();

bool features::LoadoutUsesFillers() { return g_fillers; }

void features::ApplyLoadout() {
    static bool hooked = false;
    if (hooked) return;
    hooked = patch::HookCall(kSceneRead, reinterpret_cast<const void*>(&ReadScene), kReadFile);
    dslog::Write("Loadout: scene hook %s", hooked ? "installed" : "FAILED");
    ApplyRuntimeSpawn();
    ApplyUnlocks();
    ApplySaveKits();
}

// ---- runtime weapon spawn (customise screen kit changes; dev F8) ----
// The scene loader hands every object record to the object manager's NewHLO_callback (0x41B750, cdecl(record) ->
// object; installed at [0x63C9A0]+0x1C): record +0xAB stat (definition) name, +0xD0 stat type; weapons are built by
// FUN_004a0ec0(record, stat) (0xA4 bytes, vtable 0x5D9760). A weapon record captured during the load is replayed
// later with another stat name -> a new weapon object mid-level, handed out like the network-MP kits do
// (FUN_00435fb0): AddItem, rounds, magazines (vtable +0x38), FUN_004a1350, FUN_0042ec10 to hold it. Taking one
// back: FUN_004318a0(soldier, item, 0, 0) (the script API's remove-item; the object is left alone).
namespace {
constexpr uint32_t kNewHlo = 0x41B750, kNewHloCont = 0x41B756;
constexpr uint8_t kNewHloEntry[] = {0x51, 0x55, 0x8B, 0x6C, 0x24, 0x0C};
// The scene loaders keep a record on the stack (frames 0x221E4 at 0x41AAF0, 0x22224 at 0x4197B0, record at the
// bottom) and FUN_004c9a90 (every object's base) reads up to +0x210D8 (RINFO lights, FUN_004fb2c0) - a 0x21000 copy
// let it read heap garbage past the copy's end (photo mode's soldier copies faulted in the heap now and then).
constexpr size_t kRecordSize = 0x221D0;
using NewHloFn = void*(__cdecl*)(uint8_t*);
NewHloFn g_newHloOriginal = nullptr;
std::vector<uint8_t> g_weaponRecord;
bool g_recordIsGun = false;  // the captured record is a real gun's (not the Fist's)
std::vector<uint8_t> g_medikitRecord;  // a MediKit's (equipment), for followers.cpp
// Photo mode's soldiers: one record per unit name of the level's own non-squad soldiers (their models, skins, kits
// and animations are loaded already). Record +0 script id (copied to object +8; the id table is filled by the scene
// loader, not here), +4 position, +0x10 rotation (degrees), +0xAB unit, kit = item names (each soldier builds his own).
struct SoldierRecord {
    std::string unit;
    std::vector<uint8_t> rec;
};
std::vector<SoldierRecord> g_soldierRecords;
constexpr size_t kMaxSoldierRecords = 32;
// Where every scene soldier started and what its record hangs under (+0x64: the parent the base ctor attaches the
// object to - FUN_004c9980 / FUN_004c9a00 - its zone): a copy takes the parent of the soldier that started nearest.
struct SoldierPlace {
    float pos[3];
    void* parent;
};
std::vector<SoldierPlace> g_soldierPlaces;

void ForgetWeaponRecord() {
    g_weaponRecord.clear();
    g_recordIsGun = false;
    g_medikitRecord.clear();
    g_soldierRecords.clear();
    g_soldierPlaces.clear();
}

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


// The object is a MediKit: its stat name (a soldier's kit) or its definition's id (+0x24 -> +4; scene pick-ups have
// numbered stats).
bool IsMedikit(const uint8_t* record, const uint8_t* obj) {
    if (_stricmp(reinterpret_cast<const char*>(record + 0xAB), "MediKit") == 0) return true;
    __try {
        const auto* def = *reinterpret_cast<const uint8_t* const*>(obj + 0x24);
        const char* id = def ? *reinterpret_cast<const char* const*>(def + 4) : nullptr;
        return id && _stricmp(id, "MediKit") == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* __cdecl NewHlo(uint8_t* record) {
#ifndef DS_DIST
    {   // dev: who creates vehicles (once)
        static bool logged = false;
        if (!logged && *reinterpret_cast<int*>(record + 0xD0) == 2) {
            logged = true;
            dslog::Write("[dev] Loadout: NewHLO for a vehicle called from %p", _ReturnAddress());
        }
    }
#endif
    features::VehicleRecord(record);   // a vehicle swap (vehicles.cpp) rewrites the record first
    void* obj = g_newHloOriginal(record);
    features::VehicleCreated(record, obj);
    if (obj && *reinterpret_cast<uint32_t*>(obj) == 0x5D88E0 && *reinterpret_cast<int*>(record) >= 0 &&
        g_soldierPlaces.size() < 4096) {
        SoldierPlace p;
        memcpy(p.pos, record + 4, 12);
        p.parent = *reinterpret_cast<void**>(record + 0x64);
        g_soldierPlaces.push_back(p);
    }
    if (obj && *reinterpret_cast<uint32_t*>(obj) == 0x5D88E0 && g_soldierRecords.size() < kMaxSoldierRecords &&
        *reinterpret_cast<int*>(record) >= 0) {   // (not photo mode's own copies: id -1)
        const auto* unit = *reinterpret_cast<const uint8_t* const*>(static_cast<uint8_t*>(obj) + 0x24);
        const std::string name = reinterpret_cast<const char*>(record + 0xAB);
        const bool squad = unit && static_cast<int8_t>(unit[0x1C7]) >= 0;   // a squad role: the player mapping takes them
        bool known = name.empty() || squad;
        for (const SoldierRecord& r : g_soldierRecords) known = known || _stricmp(r.unit.c_str(), name.c_str()) == 0;
        if (!known) {
            SoldierRecord r{name, std::vector<uint8_t>(kRecordSize)};
            if (SafeCopy(r.rec.data(), record, kRecordSize)) {
                dslog::Write("Photo mode: soldier record %s captured (id %d)", name.c_str(), *reinterpret_cast<int*>(record));
#ifndef DS_DIST
                if (g_soldierRecords.empty()) {   // dev: record fields that point into heap memory (dangling after the load?)
                    std::string list;
                    for (size_t off = 0; off + 4 <= kRecordSize; off += 4) {
                        const uint32_t v = *reinterpret_cast<const uint32_t*>(r.rec.data() + off);
                        if (v < 0x10000 || (v >= 0x400000 && v < 0x800000)) continue;
                        MEMORY_BASIC_INFORMATION mbi{};
                        if (VirtualQuery(reinterpret_cast<void*>(v), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT &&
                            mbi.Type == MEM_PRIVATE) {
                            char b[40];
                            snprintf(b, sizeof b, " +%X=%08X", static_cast<unsigned>(off), v);
                            list += b;
                            if (list.size() > 1500) break;
                        }
                    }
                    dslog::Write("[dev] Photo mode: heap pointers in the %s record:%s", name.c_str(), list.c_str());
                }
#endif
                g_soldierRecords.push_back(std::move(r));
            }
        }
    }
    if (obj && g_medikitRecord.empty() && IsMedikit(record, static_cast<uint8_t*>(obj))) {
        std::vector<uint8_t> copy(kRecordSize);
        if (SafeCopy(copy.data(), record, kRecordSize)) g_medikitRecord.swap(copy);
    }
    if (!obj || *reinterpret_cast<int*>(static_cast<uint8_t*>(obj) + 4) != 6 || g_recordIsGun) return obj;
    // Not a vehicle's gun (made by the vehicle's set-up FUN_00437cd0 from a record of its own, id -1): VEHWPNS weapons
    // are named *_VWPN_* (the T-72's cannon *_VWNP_*) and count as machine guns, so the first vehicle's gun became the
    // template every soldier weapon of the loadout screen was made from.
    if (strstr(reinterpret_cast<char*>(record + 0xAB), "_VWPN_") || strstr(reinterpret_cast<char*>(record + 0xAB), "_VWNP_"))
        return obj;
    // Prefer a main weapon's record: weapons cloned from the first one (the Fist, an invisible melee weapon) were not
    // drawn - the soldier fell back to his knife. The Fist's is only kept until a gun comes along.
    const bool gun = features::KitSlotOf(reinterpret_cast<char*>(record + 0xAB)) == 0;
    if (!g_weaponRecord.empty() && !gun) return obj;
    std::vector<uint8_t> copy(kRecordSize);
    if (SafeCopy(copy.data(), record, kRecordSize)) {
        g_weaponRecord.swap(copy);
        g_recordIsGun = gun;
        dslog::Write("Loadout: weapon record captured (%s, model %s, type %d)", reinterpret_cast<char*>(record + 0xAB),
                     reinterpret_cast<char*>(record + 0x1C), *reinterpret_cast<int*>(record + 0xD0));

    }
    return obj;
}

void* SpawnWeapon(const char* stat) {
    if (g_weaponRecord.empty()) return nullptr;
    std::vector<uint8_t> rec = g_weaponRecord;
    char* name = reinterpret_cast<char*>(rec.data() + 0xAB);
    strncpy_s(name, 0x20, stat, _TRUNCATE);
    // The record's model decides what is drawn: +0x60 = the model already loaded for the captured object, +0x1C its
    // name. FUN_004c9a90 -> FUN_004c9770 loads the model by name (FUN_004dd5c0) when +0x60 is null - so every swapped
    // weapon looked like the captured M16A2 until the target's own model name went in here.
    if (const Weapon* w = FindWeapon(stat)) {
        char* model = reinterpret_cast<char*>(rec.data() + 0x1C);
        if (_stricmp(model, w->model) != 0) {
            strncpy_s(model, 0x44, w->model, _TRUNCATE);
            *reinterpret_cast<void**>(rec.data() + 0x60) = nullptr;
        }
    }
    void* obj = reinterpret_cast<NewHloFn>(kNewHlo)(rec.data());
    // The level load gives every object's definition its resources (FUN_00496020 -> FUN_00495ec0(object): for a weapon
    // the model at def +0x74, then FUN_004959f0: decals and the aim / reload animations at def +0x2B8, +0x2E0 ...).
    // A weapon whose definition had no object in the level (most swapped-in guns, every mod weapon) had none of them:
    // no reload animation, so a reload refilled the magazine at once. Already-bound definitions are left as they are.
    if (obj) reinterpret_cast<void(__cdecl*)(void*)>(0x495EC0)(obj);
    // A model loaded mid-level only registers its textures (drawn white) - load the texture manager's pending batch.
    using LoadPending = void(__thiscall*)(void*, void*, int);  // (progress callback, its argument) - ret 8
    if (obj) reinterpret_cast<LoadPending>(0x54A3E0)(*reinterpret_cast<void**>(0x63C930), nullptr, 0);
    return obj;
}

void GiveWeapon(uint8_t* soldier, uint8_t* item, int mags, bool hold = true) {
    using AddItem = void(__thiscall*)(void*, void*, int, int);
    using SetRounds = void(__thiscall*)(void*, int);
    using Vcall = void(__thiscall*)(void*, int);
    using Fill = void(__thiscall*)(void*);
    using Hold = void(__thiscall*)(void*, void*, int);
    // Rounds first, then AddItem: with one of the type already there AddItem moves the new item's rounds onto it (the
    // new one stays as an empty shell for launchers / C4, is dropped for grenades) - that is how the game keeps a
    // stack (three LAWs = 3 rockets on the first item, two empty ones). Set afterwards, the shell got its own rounds.
    reinterpret_cast<SetRounds>(0x4A4BA0)(item, 0);
    (*reinterpret_cast<Vcall*>(*reinterpret_cast<uint8_t**>(item) + 0x38))(item, mags);
    reinterpret_cast<Fill>(0x4A1350)(item);
    reinterpret_cast<AddItem>(0x4314D0)(soldier, item, 0, 0);
    if (hold) reinterpret_cast<Hold>(0x42EC10)(soldier, item, 1);
}
}  // namespace

// Weapons that fit a kit slot and this level can load (the squad's list + weapons made by .weapon files).
int features::KitSlotOf(const char* weapon) {
    switch (KindOf(weapon ? weapon : "")) {
    case Kind::Main: return 0;
    case Kind::Pistol: return 1;
    case Kind::Launcher: return 2;
    case Kind::Grenade: return 3;
    default: return -1;
    }
}

// ---- weapon unlocks (kept per mode: single player / co-op) ----
// Every weapon a squad soldier receives - the mission's starting kits during the level load, anything picked up -
// counts as found: AddItem FUN_004314d0(soldier, item, a, b) (thiscall, entry 83 EC 08 53 55) is wrapped. The lists
// live in the registry (settings key UnlockedWeaponsSP / ...Coop, "id;id;..."); the customise screen offers them.
namespace {
constexpr uint32_t kAddItem = 0x4314D0, kAddItemCont = 0x4314D5;
constexpr uint8_t kAddItemEntry[] = {0x83, 0xEC, 0x08, 0x53, 0x55};
using AddItemFn = int(__thiscall*)(void*, void*, int, int);
AddItemFn g_addItem = nullptr;
std::vector<std::string> g_unlocked[2];   // [0] single player, [1] co-op
bool g_unlocksLoaded[2] = {};
bool g_kitSwapping = false;  // the customise screen hands out a weapon: a choice, not a find

int Mode() { return *reinterpret_cast<int*>(0x606410) ? 1 : 0; }  // local multiplayer flag = co-op
const char* ModeValue(int mode) { return mode ? "UnlockedWeaponsCoop" : "UnlockedWeaponsSP"; }

std::vector<std::string>& Unlocked(int mode) {
    if (!g_unlocksLoaded[mode]) {
        g_unlocksLoaded[mode] = true;
        std::string text;
        if (settings::ReadString(ModeValue(mode), text))
            for (char* t = strtok(text.data(), ";"); t; t = strtok(nullptr, ";"))
                if (*t) g_unlocked[mode].push_back(t);
    }
    return g_unlocked[mode];
}

void SaveUnlocks(int mode) {
    std::string v;
    for (const std::string& w : g_unlocked[mode]) v += w + ";";
    settings::WriteString(ModeValue(mode), v);
}

bool IsSquadSoldier(uint8_t* soldier) {
    auto mgr = *reinterpret_cast<const uint8_t* const*>(0x63C9A0);
    if (!mgr || !soldier) return false;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(0x5F9B94);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return false;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return false;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s; s = *reinterpret_cast<uintptr_t*>(s + 0x14))
        if (reinterpret_cast<uint8_t*>(s) == soldier) return true;
    return false;
}

// The weapon id of an item a squad soldier just got, or nullptr (guarded: the item comes from game code).
const char* SquadWeaponId(uint8_t* soldier, uint8_t* item) {
    __try {
        if (!item || *reinterpret_cast<int*>(item + 4) != 6 || !IsSquadSoldier(soldier)) return nullptr;
        auto* def = *reinterpret_cast<uint8_t**>(item + 0x24);
        return def ? *reinterpret_cast<const char**>(def + 4) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void Unlock(const char* id) {
    if (!id || features::KitSlotOf(id) < 0) return;
    const int mode = Mode();
    std::vector<std::string>& list = Unlocked(mode);
    if (std::find_if(list.begin(), list.end(), [&](const std::string& w) { return _stricmp(w.c_str(), id) == 0; }) != list.end())
        return;
    list.push_back(id);
    SaveUnlocks(mode);
    dslog::Write("Loadout: %s unlocked (%s)", id, mode ? "co-op" : "single player");
}

int __fastcall AddItemHook(uint8_t* soldier, void* /*edx*/, uint8_t* item, int a, int b) {
    const int r = g_addItem(soldier, item, a, b);
    // picked up during play (only with the loadout screen on - settings featLoadout, live: nothing to offer them in)
    if (!g_kitSwapping && settings::Get().featLoadout) Unlock(SquadWeaponId(soldier, item));
    return r;
}
}  // namespace

void features::KitUnlock(const char* weapon) { Unlock(weapon); }

bool features::KitUnlocked(const char* weapon) {
    for (const std::string& w : Unlocked(Mode()))
        if (_stricmp(w.c_str(), weapon) == 0) return true;
    for (const std::string& w : ModWeapons())
        if (_stricmp(w.c_str(), weapon) == 0) return true;   // mod weapons: always there to choose
    return false;
}

void ApplyUnlocks() {
    static bool hooked = false;
    if (hooked || !patch::Matches(kAddItem, kAddItemEntry, sizeof kAddItemEntry)) return;
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return;
    memcpy(tramp, kAddItemEntry, sizeof kAddItemEntry);
    tramp[5] = 0xE9;
    const int32_t rel = static_cast<int32_t>(kAddItemCont - (reinterpret_cast<uint32_t>(tramp) + 10));
    memcpy(tramp + 6, &rel, 4);
    g_addItem = reinterpret_cast<AddItemFn>(tramp);
    hooked = patch::WriteJump(kAddItem, reinterpret_cast<const void*>(&AddItemHook));
    dslog::Write("Loadout: unlock tracking %s", hooked ? "installed" : "FAILED");
}

std::vector<std::string> features::KitChoices(int slot) {
    std::vector<std::string> out;
    auto add = [&](const std::string& w) {
        if (KitSlotOf(w.c_str()) == slot && Available(w) &&
            std::find_if(out.begin(), out.end(), [&](const std::string& o) { return _stricmp(o.c_str(), w.c_str()) == 0; }) == out.end())
            out.push_back(w);
    };
    for (const Weapon& w : kWeapons)
        if (KitUnlocked(w.item)) add(w.item);
    for (const std::string& w : Unlocked(Mode())) add(w);   // found enemy weapons etc.
    for (const std::string& w : ModWeapons()) add(w);
    return out;
}

// Rounds an item holds: reserve +0x54 + magazine +0x80 (FUN_004a1350 reloads from one into the other; +0x78 = rounds
// missing from the magazine). A launcher's magazine is 1, so this is its rocket count.
int features::KitRounds(void* item) {
    auto* it = static_cast<uint8_t*>(item);
    return it ? *reinterpret_cast<int*>(it + 0x54) + *reinterpret_cast<int*>(it + 0x80) : 0;
}

// Magazines for a gun swapped in for oldItem (main weapon / sidearm): the same share of its own allowance as the old
// one held. The magazine setter (vtable +0x38, FUN_004a10a0) gives min(n, definition +0xCE max magazines) x +0x92
// magazine size; the missions' kits are mostly full (Mission 3: M16A2 300 = 10 x 30), the old fixed 5 / 3 gave about
// half. No old item (an empty slot): full.
int SwapMagazines(uint8_t* oldItem, uint8_t* newItem) {
    auto allowance = [](uint8_t* it, int& cap, int& size) {
        auto* def = *reinterpret_cast<uint8_t**>(it + 0x24);
        cap = def ? def[0xCE] : 0;
        size = def ? *reinterpret_cast<uint16_t*>(def + 0x92) : 0;
    };
    int newCap, newSize;
    allowance(newItem, newCap, newSize);
    if (newCap <= 0) return 5;
    if (!oldItem) return newCap;
    int oldCap, oldSize;
    allowance(oldItem, oldCap, oldSize);
    if (oldCap <= 0 || oldSize <= 0) return newCap;
    const float share = std::clamp(features::KitRounds(oldItem) / static_cast<float>(oldCap * oldSize), 0.0f, 1.0f);
    return std::max(1, static_cast<int>(std::ceil(share * newCap - 0.01f)));
}

bool features::KitSwap(void* soldier, void* oldItem, const std::string& weapon, int slot, int mags) {
    static const int kMags[4] = {5, 3, 1, 2};
    auto* s = static_cast<uint8_t*>(soldier);
    uint8_t* item = nullptr;
    if (!weapon.empty()) {
        item = static_cast<uint8_t*>(SpawnWeapon(weapon.c_str()));
        if (!item) {
            dslog::Write("[fail] Loadout: can't make %s (%s)", weapon.c_str(), g_weaponRecord.empty() ? "no weapon record" : "spawn failed");
            return false;
        }
    }
    // The new weapon first, in the hands if the old one was (or it's the main weapon); then the old one goes. Taking a
    // held weapon first made the game fall back to the knife and draw it over the new gun.
    const bool heldOld = oldItem && *reinterpret_cast<void**>(s + 0x26B0) == oldItem;
    g_kitSwapping = true;   // (a weapon chosen from a mod's list stays a mod weapon: not unlocked for good)
    const int modMags = features::ModWeaponMagazines(weapon);  // a .weapon file's "magazines"
    int give = mags > 0 ? mags : modMags > 0 ? modMags : kMags[slot & 3];
    if (item && mags <= 0 && modMags <= 0 && slot <= 1) give = SwapMagazines(static_cast<uint8_t*>(oldItem), item);
    if (item) GiveWeapon(s, item, give, slot == 0 || heldOld);
    g_kitSwapping = false;
    if (oldItem) {
        using Take = void(__thiscall*)(void*, void*, int, int);
        // TakeItem refuses throwables (FUN_004a4c60: definition class 7 frag, 11 smoke, 13): the class is cleared for
        // the call (the definition is shared, the game thread is the only user) and put back.
        auto* def = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(oldItem) + 0x24);
        int* cls = def ? reinterpret_cast<int*>(def + 0xC0) : nullptr;
        const int saved = cls ? *cls : 0;
        const bool throwable = cls && (saved == 7 || saved == 11 || saved == 13);
        if (throwable) *cls = 0;
        reinterpret_cast<Take>(0x4318A0)(s, oldItem, 0, 0);
        if (throwable) *cls = saved;
    }
    return true;
}

int NewHloFault(EXCEPTION_POINTERS* e) {
    const CONTEXT* c = e->ContextRecord;
    const auto* st = reinterpret_cast<const uint32_t*>(c->Esp);
    char frames[160] = {};
    int n = 0;
    for (int i = 0; i < 64 && n < 6; ++i)   // exe return addresses on the stack
        if (st[i] >= 0x401000 && st[i] < 0x5D8000)
            snprintf(frames + strlen(frames), sizeof frames - strlen(frames), " %08X", st[i]), ++n;
    dslog::Write("[fail] Photo mode: NewHLO exception %08X at %08X (eax %08X ecx %08X esi %08X edi %08X) stack%s",
                 e->ExceptionRecord->ExceptionCode, c->Eip, c->Eax, c->Ecx, c->Esi, c->Edi, frames);
    return EXCEPTION_EXECUTE_HANDLER;
}
void* NewHloGuarded(uint8_t* rec) {
    __try {
        return reinterpret_cast<NewHloFn>(kNewHlo)(rec);
    } __except (NewHloFault(GetExceptionInformation())) {
        return nullptr;
    }
}

// Photo mode: the unit names that can be spawned in this level, and a new soldier of one of them.
std::vector<std::string> features::PhotoSpawnUnits() {
    std::vector<std::string> out;
    for (const SoldierRecord& r : g_soldierRecords) out.push_back(r.unit);
    return out;
}
void* features::PhotoSpawnSoldier(int index, const float pos[3], float yaw) {
    if (index < 0 || index >= static_cast<int>(g_soldierRecords.size())) return nullptr;
    std::vector<uint8_t> rec = g_soldierRecords[index].rec;
    *reinterpret_cast<int*>(rec.data()) = -1;   // no script id: scripts / the id table never see it
    memcpy(rec.data() + 4, pos, 12);
    reinterpret_cast<float*>(rec.data() + 0x10)[1] = yaw;
    // The scene lights the base ctor attaches (+0x20F30 count <= 6, indices +0x20F34, FUN_004d04d0) are the ones around
    // the original's place - a reserve waiting indoors came out black in the sun. The copy gets none.
    *reinterpret_cast<uint32_t*>(rec.data() + 0x20F30) = 0;
    float best = 1e30f;
    for (const SoldierPlace& p : g_soldierPlaces) {
        const float dx = p.pos[0] - pos[0], dy = p.pos[1] - pos[1], dz = p.pos[2] - pos[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < best) best = d, *reinterpret_cast<void**>(rec.data() + 0x64) = p.parent;
    }
    void* obj = NewHloGuarded(rec.data());
    if (!obj) {
        dslog::Write("[fail] Photo mode: creating a %s failed", g_soldierRecords[index].unit.c_str());
        return nullptr;
    }
    auto* o = static_cast<uint8_t*>(obj);
    if (!(o[0xC] & 1)) (*reinterpret_cast<void(__thiscall**)(void*, int)>(*reinterpret_cast<uint8_t**>(o) + 4))(o, 1);
    // Never saved: the save game's object walk (FUN_004c8e90 from 0x41589E, mask 0x20000) skips instances with flag
    // 0x20000 - the cutscene actors' flag (also kept out of the squad picker / panels). Despawned ones stay deactivated
    // in the team lists, and a loaded save rebuilds the level anyway.
    if (auto* inst = *reinterpret_cast<uint8_t**>(o + 0x2C)) *reinterpret_cast<uint32_t*>(inst + 0xC) |= 0x20000;
    using LoadPending = void(__thiscall*)(void*, void*, int);   // textures of anything loaded mid-level
    reinterpret_cast<LoadPending>(0x54A3E0)(*reinterpret_cast<void**>(0x63C930), nullptr, 0);
#ifndef DS_DIST
    {   // dev: the copy's look vs. the original soldier's (record id)
        auto look = [](const char* what, const uint8_t* so) {
            const uint8_t* in = so ? *reinterpret_cast<uint8_t* const*>(so + 0x2C) : nullptr;
            if (!in) return;
            dslog::Write("[dev] Photo mode: %s instance %p +0x1D4 %p +0x2B0 %p flags %08X skin entry %p", what, in,
                         *reinterpret_cast<void* const*>(in + 0x1D4), *reinterpret_cast<void* const*>(in + 0x2B0),
                         *reinterpret_cast<const uint32_t*>(in + 0xC), *reinterpret_cast<void* const*>(so + 0x2A68));
        };
        look("spawned", o);
        const int id = *reinterpret_cast<int*>(g_soldierRecords[index].rec.data());
        const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(0x63C9A0);
        if (mgr && id >= 0 && static_cast<uint32_t>(id) <= *reinterpret_cast<const uint32_t*>(mgr + 0x34))
            look("original", (*reinterpret_cast<uint8_t* const* const*>(mgr + 0x30))[id]);
        dslog::Write("[dev] Photo mode: record skin texture \"%s\" model \"%s\"",
                     reinterpret_cast<const char*>(g_soldierRecords[index].rec.data() + 0x68),
                     reinterpret_cast<const char*>(g_soldierRecords[index].rec.data() + 0x1C));
    }
#endif
    return obj;
}

// A weapon for a soldier outside the kit system (followers.cpp: Franklin's / the Emir's pistol), in his hands, not
// counted as an unlock.
void* features::GiveSpawnedWeapon(void* soldier, const char* weapon, int mags) {
    auto* item = static_cast<uint8_t*>(SpawnWeapon(weapon));
    if (!item) return nullptr;
    g_kitSwapping = true;
    GiveWeapon(static_cast<uint8_t*>(soldier), item, mags, true);
    g_kitSwapping = false;
    return item;
}

// The weapon ids the level's soldiers carry (inventory +0x26B8, 25 items; weapon = item +4 type 6, definition +0x24 ->
// id +4) - the definitions live as long as the level.
static int SceneWeaponIds(const char** out, int max) {
    int n = 0;
    __try {
        const uint8_t* mgr = *reinterpret_cast<uint8_t* const*>(0x63C9A0);
        uint8_t* const* table = mgr ? *reinterpret_cast<uint8_t* const* const*>(mgr + 0x30) : nullptr;
        const uint32_t maxId = mgr ? *reinterpret_cast<const uint32_t*>(mgr + 0x34) : 0;
        for (uint32_t i = 0; table && i <= maxId && i < 100000 && n < max; ++i) {
            const uint8_t* s = table[i];
            if (!s || *reinterpret_cast<const uint32_t*>(s) != 0x5D88E0) continue;
            auto* inv = *reinterpret_cast<uint8_t* const* const*>(s + 0x26B8);
            for (int k = 0; inv && k < 25 && n < max; ++k) {
                const uint8_t* it = inv[k];
                if (!it || *reinterpret_cast<const int*>(it + 4) != 6) continue;
                const uint8_t* def = *reinterpret_cast<uint8_t* const*>(it + 0x24);
                if (def && *reinterpret_cast<const char* const*>(def + 4)) out[n++] = *reinterpret_cast<const char* const*>(def + 4);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// Photo mode: what a spawned soldier can be handed - main weapons, pistols and launchers this level can load (the
// squad's list), the weapons the level's own soldiers carry (enemy rifles: models and shot banks loaded already) and
// mod weapons. Vehicle guns and story items (no inventory picture) are Kind::Other and never offered.
std::vector<std::string> features::PhotoWeaponChoices() {
    std::vector<std::string> out;
    auto add = [&](const char* w) {
        if (!w || !*w) return;
        const int slot = KitSlotOf(w);
        if (slot < 0 || slot > 2 || !Available(w)) return;
        if (std::find_if(out.begin(), out.end(), [&](const std::string& o) { return _stricmp(o.c_str(), w) == 0; }) == out.end())
            out.push_back(w);
    };
    for (const Weapon& w : kWeapons) add(w.item);
    static const char* scene[2048];
    const int n = SceneWeaponIds(scene, 2048);
    for (int i = 0; i < n; ++i) add(scene[i]);
    for (const std::string& w : ModWeapons()) add(w.c_str());
    return out;
}

// A MediKit (cloned from the level's first MediKit record) into a soldier's inventory.
bool features::GiveSpawnedMedikit(void* soldier) {
    if (g_medikitRecord.empty()) return false;
    std::vector<uint8_t> rec = g_medikitRecord;
    void* obj = reinterpret_cast<NewHloFn>(kNewHlo)(rec.data());
    if (!obj) return false;
    reinterpret_cast<void(__cdecl*)(void*)>(0x495EC0)(obj);
    g_kitSwapping = true;
    reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(0x4314D0)(soldier, obj, 0, 0);
    g_kitSwapping = false;
    return true;
}

// ---- spawned weapons in saves ----
// A save stores a soldier's inventory as object ids (item +8, FUN_00416830; restored by FUN_00417180 through the id
// table). Weapons made by SpawnWeapon (loadout screen, carried kits, mod weapons) aren't in that table (id -1), so a
// save dropped them and the soldier came back without his main weapon. They are kept in the save's PSE co-save
// ("<name>.pse", chunk 'KITS' of dsfix.loadout - core/pse_saves_format.h) and handed out again after the restore. The
// squad's gear (dsfix.gear 'GEAR') and the vehicle swaps (dsfix.vehicles 'VEHS') go into the same co-save; until
// 3.3.0-Alpha all three were lines of a "<name>.sav.dsfix" text sidecar, still read for saves without a .pse
// (core/pse_saves.cpp).
// The game layer (pse/games/ds1/ds1_events.cpp, owner pse.game.ds1) hooks the save FUN_00415650 / load FUN_00415b60
// entries and runs the co-save around them: right after the game wrote the file, every plugin's save callback
// (KitsSave below captures the squad's spawned weapons then - the moment this file's old save hook captured them),
// right after the game read one, every plugin's load / revert callback.
//   restore FUN_00416310 at 0x40CBAE (level init, when a save is being loaded) - hooked here: the weapons go back
namespace {
constexpr uint32_t kRestoreSave = 0x416310;
constexpr uint32_t kRestoreSite = 0x40CBAE;

using SavedWeapon = pse_saves::SavedWeapon;
std::vector<SavedWeapon> g_saveKits;  // of the last save written or read (a save without a path reloads from memory)

template <class F>
void ForEachSquadSoldier(F f) {
    auto mgr = *reinterpret_cast<const uint8_t* const*>(0x63C9A0);
    if (!mgr) return;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(0x5F9B94);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s; s = *reinterpret_cast<uintptr_t*>(s + 0x14)) {
        auto* soldier = reinterpret_cast<uint8_t*>(s);
        auto* unit = *reinterpret_cast<uint8_t**>(soldier + 0x24);
        const int role = unit ? *reinterpret_cast<int8_t*>(unit + 0x1C7) : -1;
        if (role >= 0 && role <= 3) f(soldier, role);
    }
}

void CaptureSquadKits(std::vector<SavedWeapon>& out) {
    out.clear();
    __try {
        ForEachSquadSoldier([&out](uint8_t* soldier, int role) {
            auto* inv = *reinterpret_cast<uint8_t***>(soldier + 0x26B8);
            if (!inv) return;
            for (int i = 0; i < 25; ++i) {
                uint8_t* it = inv[i];
                if (!it || *reinterpret_cast<int*>(it + 4) != 6 || *reinterpret_cast<int*>(it + 8) != -1) continue;
                auto* def = *reinterpret_cast<uint8_t**>(it + 0x24);
                const char* id = def ? *reinterpret_cast<const char**>(def + 4) : nullptr;
                if (!id || !*id) continue;
                SavedWeapon w;
                w.role = role;
                w.id = id;
                w.reserve = *reinterpret_cast<int*>(it + 0x54);
                w.magazine = *reinterpret_cast<int*>(it + 0x80);
                w.held = *reinterpret_cast<uint8_t**>(soldier + 0x26B0) == it;
                out.push_back(std::move(w));
            }
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dslog::Write("[fail] Loadout: reading the squad's spawned weapons for a save faulted");
    }
}

// dsfix.loadout's co-save chunk. The game layer calls the save callbacks only after the game wrote a save file (flag 0,
// a path, success): the squad's spawned weapons are captured here, right after the write - as the old save hook did.
const PSESerializationInterface* Ser() { return pse::SerializationInterface(); }

void KitsSave(PSESaveWriter* w) {
    CaptureSquadKits(g_saveKits);
    dslog::Write("Loadout: %zu spawned weapon(s) kept with the save", g_saveKits.size());
    if (g_saveKits.empty()) return;
    const std::vector<uint8_t> b = pse_saves::EncodeKits(g_saveKits);
    Ser()->WriteChunk(w, pse_saves::kKitsTag, pse_saves::kChunkVersion, b.data(), static_cast<uint32_t>(b.size()));
}
void KitsLoadChunk(uint32_t tag, uint32_t version, const std::vector<uint8_t>& b) {
    if (tag != pse_saves::kKitsTag || version != 1) return;
    if (!pse_saves::DecodeKits(b.data(), b.size(), &g_saveKits))
        dslog::Write("[fail] Loadout: the save's spawned-weapon chunk is damaged (%zu read)", g_saveKits.size());
}
void KitsClear() { g_saveKits.clear(); }
const features::CosavePart kKitsCosave = {&KitsSave, &KitsClear, &KitsLoadChunk, &KitsClear};

// One callback set per plugin (pse.serialization): the kits', the gear's and the vehicle swaps' chunks, read once and
// handed out by tag (each part ignores the others' tags).
const features::CosavePart* const kCosaveParts[] = {&kKitsCosave, &features::kGearCosave, &features::kVehiclesCosave};
void PSE_CALL CosaveSave(PSESaveWriter* w, void*) {
    for (const features::CosavePart* p : kCosaveParts) p->save(w);
}
void PSE_CALL CosaveLoad(PSESaveReader* r, void*) {
    for (const features::CosavePart* p : kCosaveParts) p->loadBegin();
    uint32_t tag, version, len;
    while (Ser()->NextChunk(r, &tag, &version, &len)) {
        std::vector<uint8_t> b(len);
        if (len) b.resize(Ser()->Read(r, b.data(), len));
        for (const features::CosavePart* p : kCosaveParts) p->loadChunk(tag, version, b);
    }
}
void PSE_CALL CosaveRevert(void*) {
    for (const features::CosavePart* p : kCosaveParts) p->revert();
}

void GiveSaved(uint8_t* soldier, const SavedWeapon& w) {
    auto* item = static_cast<uint8_t*>(SpawnWeapon(w.id.c_str()));
    if (!item) {
        dslog::Write("[fail] Loadout: saved weapon %s couldn't be made (%s)", w.id.c_str(),
                     g_weaponRecord.empty() ? "no weapon record" : "spawn failed");
        return;
    }
    using SetRounds = void(__thiscall*)(void*, int);
    using Vcall = void(__thiscall*)(void*, int);
    using Fill = void(__thiscall*)(void*);
    using AddItem = void(__thiscall*)(void*, void*, int, int);
    using Hold = void(__thiscall*)(void*, void*, int);
    // One magazine loaded (as GiveWeapon does) to learn its size, then the saved counts; rounds before AddItem so a
    // stack keeps them (see GiveWeapon).
    reinterpret_cast<SetRounds>(0x4A4BA0)(item, 0);
    (*reinterpret_cast<Vcall*>(*reinterpret_cast<uint8_t**>(item) + 0x38))(item, 1);
    reinterpret_cast<Fill>(0x4A1350)(item);
    int& mag = *reinterpret_cast<int*>(item + 0x80);
    const int full = mag + *reinterpret_cast<int*>(item + 0x78);
    mag = std::clamp(w.magazine, 0, full);
    *reinterpret_cast<int*>(item + 0x78) = full - mag;
    *reinterpret_cast<int*>(item + 0x54) = std::max(0, w.reserve);
    g_kitSwapping = true;  // handed back, not found
    reinterpret_cast<AddItem>(0x4314D0)(soldier, item, 0, 0);
    g_kitSwapping = false;
    if (w.held) reinterpret_cast<Hold>(0x42EC10)(soldier, item, 1);
    dslog::Write("Loadout: saved weapon %s back to role %d (%d + %d rounds%s)", w.id.c_str(), w.role, w.magazine,
                 w.reserve, w.held ? ", held" : "");
}

void HandBackGear() {   // (own function: RestoreSaveHook's __try can't hold objects with destructors)
    features::GearRestoreSaved();   // on once the level plays
}

void __fastcall RestoreSaveHook(void* self) {
    reinterpret_cast<void(__fastcall*)(void*)>(kRestoreSave)(self);
    HandBackGear();
    if (g_saveKits.empty()) return;
    __try {
        ForEachSquadSoldier([](uint8_t* soldier, int role) {
            for (const SavedWeapon& w : g_saveKits)
                if (w.role == role) GiveSaved(soldier, w);
        });
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dslog::Write("[fail] Loadout: handing back the save's spawned weapons faulted");
    }
}

// A save keeps each soldier's skin as its key (FUN_00416830: skin entry +0 -> soldier record +0xB8); the restore
// (FUN_00417180) looks it up in the current uniform's table for the soldier's squad slot (FUN_00496500 at 0x417449) and
// applies the result at 0x417454 - null when the key is no longer in the table (a mod skin switched off since the save:
// the user's "Special Delivery 1" has Bradley in a mod skin, key 6BBE2AA7). The skin apply FUN_00435be0 with null clears
// the entry and sets +0x2A64 = unit +0x98, which is -1 for the heroes, so the soldier panel's no-portrait path
// (FUN_00480620 -> 0x48087A) drew image -1 of the HUD sheet [0x60EE18]: a garbage page index -> null page -> AV at
// 0x53CC7C. A squad soldier now falls back to his default skin (unit +0x1C8 key), else keeps the one he has.
constexpr uint32_t kRestoreSkinSite = 0x417454, kApplySkin = 0x435BE0, kFindSkin = 0x496500;

void __fastcall RestoreSkin(uint8_t* soldier, void*, void* entry) {
    using Apply = void(__thiscall*)(void*, void*);
    using Find = void*(__cdecl*)(uint32_t, int, int);
    if (!entry) {
        auto* unit = *reinterpret_cast<uint8_t**>(soldier + 0x24);
        const int slot = unit ? *reinterpret_cast<int8_t*>(unit + 0x1C7) : -1;
        if (slot >= 0 && slot <= 3) {
            const int uniform = *reinterpret_cast<int*>(0x60ED1C);
            entry = reinterpret_cast<Find>(kFindSkin)(*reinterpret_cast<uint32_t*>(unit + 0x1C8), uniform, slot);
            if (!entry) entry = *reinterpret_cast<void**>(soldier + 0x2A68);
            dslog::Write("Loadout: the save's skin for squad slot %d isn't loaded (mod switched off?) - %s", slot,
                         entry ? reinterpret_cast<const char*>(entry) + 4 : "none");
        }
    }
    reinterpret_cast<Apply>(kApplySkin)(soldier, entry);
}
}  // namespace

void ApplySaveKits() {
    static bool hooked = false;
    if (hooked) return;
    hooked = true;
    // (The save / load themselves: the game layer's hooks + co-save, pse/games/ds1/ds1_events.cpp.)
    const bool ok = patch::HookCall(kRestoreSite, reinterpret_cast<const void*>(&RestoreSaveHook), kRestoreSave) &&
                    patch::HookCall(kRestoreSkinSite, reinterpret_cast<const void*>(&RestoreSkin), kApplySkin);
    dslog::Write("Loadout: save kit hooks %s", ok ? "installed" : "FAILED");
    // Co-save chunks (pse.serialization): this plugin's kits, the gear's and the vehicle swaps'.
    Ser()->SetCallbacks(pse_host::Find("dsfix.loadout"), &CosaveSave, &CosaveLoad, &CosaveRevert, nullptr);
    features::VehicleCosaveInit();
}

void ApplyRuntimeSpawn() {
    static bool hooked = false;
    if (hooked || !patch::Matches(kNewHlo, kNewHloEntry, sizeof kNewHloEntry)) return;
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    memcpy(tramp, kNewHloEntry, sizeof kNewHloEntry);
    tramp[6] = 0xE9;
    const int32_t rel = static_cast<int32_t>(kNewHloCont - (reinterpret_cast<uint32_t>(tramp) + 11));
    memcpy(tramp + 7, &rel, 4);
    g_newHloOriginal = reinterpret_cast<NewHloFn>(tramp);
    hooked = patch::WriteJump(kNewHlo, reinterpret_cast<const void*>(&NewHlo));
    dslog::Write("Loadout: NewHLO hook %s", hooked ? "installed" : "FAILED");
}

#ifndef DS_DIST

void features::OnFrameLoadoutDev() {
    static bool down = false;
    const HWND wnd = *reinterpret_cast<HWND*>(0x606A60);
    const bool now = wnd && GetForegroundWindow() == wnd && (GetAsyncKeyState(VK_F8) & 0x8000);
    if (now && !down) {
        auto* soldier = *reinterpret_cast<uint8_t**>(0x60F5B8 + 0x310);
        if (!soldier) {
            dslog::Write("[dev]  Loadout: F8 - no soldier");
        } else if (auto* item = static_cast<uint8_t*>(SpawnWeapon("US_WPN_MP5SilencedSubMG"))) {
            GiveWeapon(soldier, item, 5);
            dslog::Write("[dev]  Loadout: F8 spawned MP5 %p (type %d) for player 1", item,
                         *reinterpret_cast<int*>(item + 4));
        } else {
            dslog::Write("[dev]  Loadout: F8 spawn failed (record %s)", g_weaponRecord.empty() ? "missing" : "ok");
        }
    }
    down = now;

    // F7: player 1's soldier to the other uniform (skin table [uniform*4 + squad slot], FUN_00496500 / FUN_00435be0)
    static bool down7 = false;
    const bool now7 = wnd && GetForegroundWindow() == wnd && (GetAsyncKeyState(VK_F7) & 0x8000);
    if (now7 && !down7) {
        auto* soldier = *reinterpret_cast<uint8_t**>(0x60F5B8 + 0x310);
        auto* entry = soldier ? *reinterpret_cast<uint32_t**>(soldier + 0x2A68) : nullptr;
        auto* unit = soldier ? *reinterpret_cast<uint8_t**>(soldier + 0x24) : nullptr;
        if (entry && unit) {
            const int slot = *reinterpret_cast<int8_t*>(unit + 0x1C7);
            static int uniform = -1;
            if (uniform < 0) uniform = *reinterpret_cast<int*>(0x60ED1C);
            uniform ^= 1;
            using Find = uint32_t*(__cdecl*)(uint32_t, int, int);
            using Apply = void(__thiscall*)(void*, void*);
            uint32_t* next = reinterpret_cast<Find>(0x496500)(entry[0], uniform, slot);
            dslog::Write("[dev]  Loadout: F7 slot %d uniform -> %d, skin %s -> %s", slot, uniform,
                         reinterpret_cast<char*>(entry) + 4, next ? reinterpret_cast<char*>(next) + 4 : "(none)");
            if (next) {
                reinterpret_cast<Apply>(0x435BE0)(soldier, next);
                // New texture names are only registered; the level load's batch (FUN_0054a3e0 on the texture manager
                // [0x63C930], dirty flag +0x18) loads them - run it now, without a progress callback.
                using LoadPending = void(__thiscall*)(void*, void*, int);  // (progress callback, its argument) - ret 8
                reinterpret_cast<LoadPending>(0x54A3E0)(*reinterpret_cast<void**>(0x63C930), nullptr, 0);
            }
        }
    }
    down7 = now7;
}
#else
void features::OnFrameLoadoutDev() {}   // (in dsfix.loadout's table in every build)
#endif
