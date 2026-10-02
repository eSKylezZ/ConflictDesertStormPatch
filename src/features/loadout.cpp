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

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kSceneRead = 0x40B606;  // call FUN_004bfb90 in the level loader
constexpr uint32_t kReadFile = 0x4BFB90;   // cdecl(name, char** buf, int* size, int, int type) -> 1 ok
constexpr uint32_t kAlloc = 0x4B8450;      // cdecl(size, type) -> memory
constexpr uint32_t kFree = 0x4B8590;       // cdecl(memory)

using ReadFileFn = int(__cdecl*)(const char*, char**, int*, int, int);
using AllocFn = char*(__cdecl*)(int, int);
using FreeFn = void(__cdecl*)(void*);

const char* const kSquad[] = {"Bradley", "Foley", "Jones", "Ramirez"};

enum class Kind { Main, Pistol, Launcher, Grenade, Other };

// The squad's weapons with their models (catalog.dat Weaps.txt column 2). A weapon's model has to be in the level's
// own archive (<level>.dat) - one that isn't (the SPAS-12 in Mission 1) crashes the level load (0x4D081A).
struct Weapon {
    const char* item;
    const char* model;
    Kind kind;
};
const Weapon kWeapons[] = {
    {"US_WPN_M16A2", "Rifle02_M16wM203", Kind::Main},
    {"US_WPN_M16", "Rifle03_ColtCommando", Kind::Main},
    {"US_WPN_HKPSG1", "Sniper02_PSG1", Kind::Main},
    {"US_WPN_BarrettSniper", "SNIPER01_BARRETL50", Kind::Main},
    {"US_WPN_AccuracyInter", "SNIPER03_L96A1", Kind::Main},
    {"US_WPN_M60E3", "LMG04_M60E3", Kind::Main},
    {"US_WPN_SAW_lightmg", "LMG01_M249SAW", Kind::Main},
    {"US_WPN_MP5SilencedSubMG", "SMG01_MP5SD3", Kind::Main},
    {"US_WPN_Remmington870", "Shotgun01_REM870", Kind::Main},
    {"US_WPN_FranchiSPAS", "Shotgun02_FranchiSPAS12", Kind::Main},
    {"US_WPN_SIGP228", "Pistol03_SIGP228", Kind::Pistol},
    {"US_WPN_Beretta92F", "Pistol02_Beretta92F", Kind::Pistol},
    {"US_WPN_DesertEagle", "Pistol01_DesertEagle", Kind::Pistol},
    {"US_WPN_LAW66", "LAW66_AntiTankLauncher", Kind::Launcher},
    {"US_WPN_LAW80", "LAW80_AntiTankLauncher", Kind::Launcher},
    {"US_WPN_FIM92", "AA01_FIM92Stinger", Kind::Launcher},
    {"US_WPN_GrenadeFrag", "Grenade_Frag", Kind::Grenade},
    {"US_WPN_GrenadeSmoke", "Grenade_Smoke", Kind::Grenade},
};

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
                for (size_t c = pos; c <= nl && col.size() < 6;) {
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
                    e->w = {nullptr, nullptr, col[4] == "-1" ? Kind::Other : KindOfCategory(col[5])};
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
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string path = exe;
    path = path.substr(0, path.find_last_of("\\/") + 1) + base + ".dat";
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

bool features::LoadoutUsesFillers() { return g_fillers; }

void features::ApplyLoadout() {
    static bool hooked = false;
    if (hooked) return;
    hooked = patch::HookCall(kSceneRead, reinterpret_cast<const void*>(&ReadScene), kReadFile);
    dslog::Write("Loadout: scene hook %s", hooked ? "installed" : "FAILED");
    ApplyRuntimeSpawn();
    ApplyUnlocks();
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
constexpr size_t kRecordSize = 0x21000;  // FUN_004c9a90 (every object's base) reads record +0x20F04 / +0x20F2C
using NewHloFn = void*(__cdecl*)(uint8_t*);
NewHloFn g_newHloOriginal = nullptr;
std::vector<uint8_t> g_weaponRecord;
bool g_recordIsGun = false;  // the captured record is a real gun's (not the Fist's)

void ForgetWeaponRecord() {
    g_weaponRecord.clear();
    g_recordIsGun = false;
}

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


void* __cdecl NewHlo(uint8_t* record) {
    void* obj = g_newHloOriginal(record);
    if (!obj || *reinterpret_cast<int*>(static_cast<uint8_t*>(obj) + 4) != 6 || g_recordIsGun) return obj;
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
// live in the registry (Enhancements\UnlockedWeaponsSP / ...Coop, "id;id;..."); the customise screen offers them.
namespace {
constexpr uint32_t kAddItem = 0x4314D0, kAddItemCont = 0x4314D5;
constexpr uint8_t kAddItemEntry[] = {0x83, 0xEC, 0x08, 0x53, 0x55};
constexpr char kRegKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";
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
        char buf[4096] = {};
        DWORD size = sizeof buf;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, kRegKey, ModeValue(mode), RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            for (char* t = strtok(buf, ";"); t; t = strtok(nullptr, ";"))
                if (*t) g_unlocked[mode].push_back(t);
    }
    return g_unlocked[mode];
}

void SaveUnlocks(int mode) {
    std::string v;
    for (const std::string& w : g_unlocked[mode]) v += w + ";";
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExA(key, ModeValue(mode), 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>(v.size() + 1));
    RegCloseKey(key);
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
    if (!g_kitSwapping) Unlock(SquadWeaponId(soldier, item));   // picked up during play
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

std::vector<std::string> features::KitWeaponIds() {
    std::vector<std::string> out;
    for (const Weapon& w : kWeapons) out.push_back(w.item);
    for (const std::string& w : ModWeapons()) out.push_back(w);
    return out;
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
    if (item) GiveWeapon(s, item, mags > 0 ? mags : modMags > 0 ? modMags : kMags[slot & 3], slot == 0 || heldOld);
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
#endif
