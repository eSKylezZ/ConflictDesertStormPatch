// ds2fix.loadout - a pre-mission LOADOUT screen for Conflict: Desert Storm II (games/ds2/plugins/loadout), the first
// DS2 port of DesertStormFix's loadout / customise screen (games/ds1/src/features/customise.cpp + loadout.cpp).
// Research + address table: games/ds2/PORTING.md "Loadout screen"; docs/sdk/reference/game-ds2.md "ds2fix.loadout".
//
// What it does (first version, on purpose small): when a mission's intro is over (front-end state 0x13, no cutscene,
// no letterbox, the player has control, his vehicle - if he sits in one - stands still for a second) the world pauses
// and one screen lists the squad: per soldier MAIN WEAPON / SIDEARM / LAUNCHER, each choosable from the hand-held
// weapons some soldier in this level already carries (so the model, its animations and the shot sound bank are
// loaded - DS1's Available() rule made stricter). START MISSION applies the changes and the world goes on.
//
// CDS II.exe 1.0 (DS1 addresses in brackets):
//   CreateInventoryHLO 0x4830E0 thiscall(soldier, weapon def) ret 4: builds a scene record on its own stack (frame
//     0x22584 B - DS2 needs no record copy, so DS1's record-size trap [0x221D0] can't happen), creates the object
//     through the object manager 0x4CDEA0 and adds it with AddItem(item, 1, 0) (= no hold); the grunt ctor's kit loop
//     0x483BF0.. calls it, then SetRounds(0) + AddRounds(magazines x size).
//   AddItem 0x4828E0 thiscall(soldier, item, a, b) ret 0xC [0x4314D0]: a weapon whose definition the soldier already
//     carries is merged into it (rounds moved, no new slot) -> such choices are never offered.
//   TakeItem 0x47F3A0 thiscall(soldier, item, drop, flag) ret 0xC [0x4318A0]: drop 0 = out of the inventory only; a held
//     item is first put away (hold 0 - the knife fallback DS1 hit) -> the new weapon is given + held first.
//   Hold 0x47EFF0 thiscall(soldier, item, flag) ret 8 [0x42EC10] (script API select_item_by_name passes flag 0).
//   soldier: +8 script id, +0x24 unit (+4 name, +0x1CF squad role), +0x690 vehicle (mounted), +0x6CC held item
//     [+0x26B0], +0x6D4 -> item*[25] inventory [+0x26B8]; item: +4 type (6 weapon), +8 script id (kit items: the
//     scene's; made at run time: -1), +0x24 definition, +0x58 rounds left in all [+0x54], +0x84 rounds fired since the
//     last reload (magazine = size - that; a reload resets it), +0x78 owner; definition (Weaps.txt, [0x635F68] x 0x3BC,
//     count [0x635508]): +0 category
//     (5 weapon), +4 id, +0x92 u16 magazine size, +0xC0 class (Weaps col 5 via 0x451450: 0 assault rifle, 1 SMG,
//     2 sniper, 3 pistol, 4 MG, 5 anti tank, 6 AT with delay, 9 grenade launcher, 11 shotgun, 12 auto shotgun,
//     14 rifle + GL ...), +0xCE u8 Weaps "magazines".
//   rounds: 0x49D280 set / 0x49D200 add (thiscall(item, n) ret 4), as the kit loop does.
//   freeze: [0x6197BC] float, multiplies every timer's seconds dt (0x4BE7B6) [0x5F7E64] -> 0.0001 (0 = DS1's fog-colour
//     screen; re-asserted every frame); input: each input block (0x62FF7C + i x 0x58C, count u16 [0x6315B0]) joystick
//     +0x3A4 = -1 and devices +0xC = 0 [+0x3C8].
//   intro over: letterbox state [[0x6800CC] + 8] (view controller FUN_004DA740: 0 off, 1 opening, 2 on, 3 closing) = 0,
//     block 0 +8 (player control, script API set_player_control) != 0, block 0 +0x2EC soldier.
//   saves: Save_Game 0x41C600 -> 0x41BE80 -> header 0x419AD0 copies the saver's ([0x62F458]) per-type object counts
//     +0xC..+0x1C, set ONCE at level init by 0x4198C0 from 0x4CDAE0(type, 3, -1, 0x20000); the object writer 0x4CD8C0
//     writes every live object (instance +0x10 flag 0x20000 = not saved) - DS1's flaw [FUN_00415530]: an item made after
//     level init was written but not counted. Fix: at PSE_MSG_SAVE_BEGIN the counts are taken again the same way.
//     Taken-away weapons get instance flag 0x20000 (neither counted nor written). Without the fix a save made after a
//     sidearm was left behind crashed when loaded (AV 0x423357 in the restore 0x41C3B0, seen with Dev\LoadoutNoRecount).
//     The game's restore can't give back a weapon made at run time (script id -1; see "co-save" below) -> co-save KITS.
#include <windows.h>
#include <d3d9.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ds2fix_options.h"
#include "ds2fix_pads.h"
#include "pse/pse_api.h"
#include "pse/pse_game_ds2.h"

namespace {
PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEMessagingInterface* g_msg = nullptr;
const PSESettingsInterface* g_settings = nullptr;
const PSEGameDs2V1* g_game = nullptr;
const Ds2FixOptionsV1* g_options = nullptr;
const PSEHooksInterface* g_hooks = nullptr;
const struct Ds2FixCoopV1* g_coop = nullptr;

void Log(PSELogLevel level, const char* fmt, ...) {
    if (!g_log) return;
    char line[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_log->Write(g_self, level, "%s", line);
}
#define LOGI(...) Log(PSE_LOG_INFO, __VA_ARGS__)
#define LOGE(...) Log(PSE_LOG_ERROR, __VA_ARGS__)

// ---- game memory (every read guarded: these are game pointers) -------------------------------------------------------
template <class T>
T Rd(uint32_t addr) {
    __try {
        return *reinterpret_cast<const T*>(addr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return T{};
    }
}
template <class T>
T Rd(const void* p, uint32_t off) { return p ? Rd<T>(reinterpret_cast<uint32_t>(p) + off) : T{}; }
template <class T>
bool Wr(void* p, uint32_t off, T v) {
    __try {
        *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void CopyStr(char* b, size_t n, const char* p) {
    __try {
        if (p) strncpy_s(b, n, p, _TRUNCATE);
        else b[0] = 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        b[0] = 0;
    }
}
std::string Str(const char* p) {
    char b[96] = {};
    CopyStr(b, sizeof b, p);
    return b;
}

constexpr uint32_t kObjects = 0x6800E0, kPlayerTeam = 0x61B0EC, kBlocks = 0x62FF7C, kBlockStride = 0x58C;
constexpr uint32_t kUniform = 0x627154;   // 0 SAS, 1 US Delta
constexpr uint32_t kBlockCount = 0x6315B0, kTimeMult = 0x6197BC, kViewCtrl = 0x6800CC, kSaver = 0x62F458;
constexpr uint32_t kCatalog = 0x62F460, kTextLookup = 0x4FBE50, kById = 0x4CF4F0, kCountObjects = 0x4CDAE0;
constexpr uint32_t kCreateItem = 0x4830E0, kTakeItem = 0x47F3A0, kHold = 0x47EFF0, kSetReserve = 0x49D280,
                   kAddReserve = 0x49D200;
constexpr uint32_t kFpuSave = 0x4C12C0, kFpuLevel = 0x4C12A0, kFpuRestore = 0x4C12D0;
constexpr uint32_t kApi = 0x60D638;   // script API table (cdecl functions)
constexpr uint32_t kSaveNoFlag = 0x20000;

uint8_t* Block(int i) { return reinterpret_cast<uint8_t*>(kBlocks + i * kBlockStride); }
int BlockCount() { return std::min<int>(Rd<uint16_t>(kBlockCount), 4); }

// Archive / catalog hash (FUN_004c0b30): LFSR over the lower-cased name.
uint32_t LowerHash(const char* name) {
    uint32_t x = 1;
    for (const char* p = name; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(tolower(static_cast<unsigned char>(*p)));
        for (int b = 0; b < 8; ++b) x = x << 1 | (((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1);
    }
    return x;
}

// The catalog's text for a hash (UTF-16; "*no string*"-style misses start with '*'), copied.
void CatalogTextRaw(uint32_t hash, wchar_t* buf, size_t n) {
    __try {
        void* cat = *reinterpret_cast<void**>(kCatalog);
        const wchar_t* t = cat ? reinterpret_cast<const wchar_t*(__thiscall*)(void*, uint32_t)>(kTextLookup)(cat, hash)
                               : nullptr;
        if (t && t[0] && t[0] != L'*') wcsncpy_s(buf, n, t, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        buf[0] = 0;
    }
}
std::wstring CatalogText(uint32_t hash) {
    wchar_t buf[96] = {};
    CatalogTextRaw(hash, buf, 96);
    return buf;
}

// ---- weapons --------------------------------------------------------------------------------------------------------
enum Slot { kMain = 0, kSidearm = 1, kLauncher = 2, kSlots = 3 };
const char* const kSlotNames[kSlots] = {"MAIN WEAPON", "SIDEARM", "LAUNCHER"};

int SlotOfClass(int cls) {
    switch (cls) {
    case 0: case 1: case 2: case 4: case 11: case 12: case 14: return kMain;
    case 3: return kSidearm;
    case 5: case 6: case 9: return kLauncher;
    default: return -1;
    }
}
int SlotOfDef(void* def) {
    if (!def || Rd<int>(def, 0) != 5) return -1;
    const std::string id = Str(Rd<const char*>(def, 4));
    if (id.empty() || id.find("VWPN") != std::string::npos || id.find("TRIPOD") != std::string::npos) return -1;
    return SlotOfClass(Rd<int>(def, 0xC0));
}
std::wstring WeaponName(void* def) {
    if (!def) return L"NONE";
    const std::string id = Str(Rd<const char*>(def, 4));
    std::wstring t = CatalogText(LowerHash(id.c_str()));
    if (t.empty()) {   // US_WPN_HKPSG1 -> HKPSG1
        std::string s = id;
        const size_t w = s.find("WPN_");
        if (w != std::string::npos) s = s.substr(w + 4);
        for (char& c : s) c = c == '_' ? ' ' : static_cast<char>(toupper(static_cast<unsigned char>(c)));
        t.assign(s.begin(), s.end());
    }
    for (wchar_t& c : t) c = static_cast<wchar_t>(towupper(c));
    return t;
}

void** Inventory(void* soldier) { return Rd<void**>(soldier, 0x6D4); }
void* InvAt(void* soldier, int i) { return Rd<void*>(Inventory(soldier), i * 4); }
bool Carries(void* soldier, void* item) {
    for (int i = 0; i < 25; ++i)
        if (item && InvAt(soldier, i) == item) return true;
    return false;
}
bool CarriesDef(void* soldier, void* def) {
    for (int i = 0; i < 25; ++i)
        if (void* it = InvAt(soldier, i); it && Rd<void*>(it, 0x24) == def) return true;
    return false;
}
// The first weapon in the soldier's inventory that fits the slot (held one first).
void* ItemInSlot(void* soldier, int slot) {
    void* held = Rd<void*>(soldier, 0x6CC);
    if (held && Rd<int>(held, 4) == 6 && SlotOfDef(Rd<void*>(held, 0x24)) == slot && Carries(soldier, held)) return held;
    for (int i = 0; i < 25; ++i) {
        void* it = InvAt(soldier, i);
        if (it && Rd<int>(it, 4) == 6 && SlotOfDef(Rd<void*>(it, 0x24)) == slot) return it;
    }
    return nullptr;
}

// Every soldier object (any team; active + inactive lists, next +0x18).
template <class Fn>
void ForEachSoldier(Fn fn) {
    uint8_t* mgr = Rd<uint8_t*>(kObjects);
    if (!mgr) return;
    const uint32_t teams = Rd<uint8_t>(mgr, 0x10), types = Rd<uint8_t>(mgr, 0x11);
    if (types < 2) return;
    for (int list = 0; list < 2; ++list) {
        uint8_t** lists = Rd<uint8_t**>(mgr, list ? 8 : 4);
        for (uint32_t team = 0; team < teams; ++team) {
            int guard = 0;
            for (uint8_t* s = Rd<uint8_t*>(lists, (team * types + 1) * 4); s && guard < 4096;
                 s = Rd<uint8_t*>(s, 0x18), ++guard)
                fn(s, static_cast<int>(team), list == 0);
        }
    }
}

// ---- settings ---------------------------------------------------------------------------------------------------------
constexpr char kKeyFeat[] = "FeatLoadout";
bool Enabled() { return g_settings && g_settings->GetInt(g_self, kKeyFeat) != 0; }
int PSE_CALL ToggleGet() { return Enabled() ? 1 : 0; }
void PSE_CALL ToggleSet(int on) {
    if (g_settings) g_settings->SetInt(g_self, kKeyFeat, on ? 1 : 0);
    LOGI("Loadout: screen %s (OPTIONS)", on ? "on" : "off");
}
const Ds2FixToggleV1 kToggle = {"LOADOUT SCREEN", "Choose the squad's weapons after each mission's intro", &ToggleGet,
                                &ToggleSet};

// ---- dev values (HKCU\Software\DesertStorm2Fix\Dev; dev builds only) ------------------------------------------------
#ifndef DS_DIST
DWORD DevDword(const char* name, DWORD def) {
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", name, RRF_RT_REG_DWORD, nullptr, &v,
                        &size) == ERROR_SUCCESS ? v : def;
}
std::string DevString(const char* name) {
    char buf[512] = {};
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", name, RRF_RT_REG_SZ, nullptr, buf, &size) ==
        ERROR_SUCCESS)
        return buf;
    const DWORD v = DevDword(name, 0xFFFFFFFF);   // a test runner may have written a number as a DWORD
    return v == 0xFFFFFFFF ? std::string() : std::to_string(v);
}
#endif

// ---- the screen's model ---------------------------------------------------------------------------------------------
struct Member {
    void* soldier = nullptr;
    int role = -1;                         // unit +0x1CF squad role
    std::wstring name;
    void* item[kSlots] = {};              // what he carries now
    std::vector<void*> choices[kSlots];   // definitions (nullptr = NONE)
    int pick[kSlots] = {};                // index into choices
    // look (DS2's skin tables / CONNOBJS): uniform 0 SAS / 1 Delta, face = row of that uniform's skin table, gear 0 SAS
    // set / 1 Delta set / 2 none; the values when the screen opened, to see what changed
    int skinTable = -1;                   // which role's skin table holds his entry (-1 = none: no look rows)
    int uniform = 0, face = 0, gear = 0;
    int uniform0 = 0, face0 = 0, gear0 = 0;   // as the level made him
    int uniformA = 0, faceA = 0, gearA = 0;   // what he wears now (the rows apply at once: a live preview)
    int owner = 0;   // split screen: the player who controls him (soldier +0xEC4, set by 0x475610 from the role)
};
std::vector<Member> g_squad;
std::vector<void*> g_levelDefs;   // hand-held weapon definitions some soldier of this level carries
int g_member = 0, g_row = 0;      // rows: 0 soldier, 1 uniform, 2 face, 3 gear, 4..6 weapon slots, 7 START MISSION
constexpr int kRowSoldier = 0, kRowUniform = 1, kRowFace = 2, kRowGear = 3, kRowFirstSlot = 4, kRowStart = 7,
              kRows = 8;
bool g_open = false, g_dirty = true;
DWORD g_camStart = 0;     // the camera's orbit starts with the screen
bool g_camLogged = false;
// Split screen (split flag [0x626C18], players [0x626C1C]): the players take turns, each edits only his own soldiers
// (owner getter 0x475970 = soldier +0xEC4 in split screen) with his own device; -1 = no turns (single player).
int g_turn = -1;
bool Mine(const Member& m) { return g_turn < 0 || m.owner == g_turn; }

// ---- look: skin tables + CONNOBJS gear ------------------------------------------------------------------------------
// Skin tables (loadCharSkinStats 0x4527F0): [0x635524 + (uniform x 4 + table) x 4] -> entries of 0x5C (+0 key hash,
// +4 texture char[0x23], +0x27 portrait, +0x4C portrait texture, +0x50 / +0x54 -> first / last name records char[0x24]),
// counts [0x6354C8 + (uniform x 4 + table) x 4] - the game reads at most 2 rows per table (count forced to 2). Uniform
// [0x627154] 0 SAS / 1 US Delta (one for the whole squad). Unit +8 / +0x2B = the body model for SAS / Delta, +0x1D0 his
// skin key. Skin apply 0x475020 thiscall(soldier, entry) ret 4 sets +0xE24 (names) and the portrait; the body / texture
// go through ds2fix.skins (Dress). Gear (CONNOBJS.TXT, loadConnectorObjectsStats 0x4529D0): [0x635548 + ((role x 2 +
// faction) x 8 + n) x 0x28] {u32 connector hash, char[36] model}, factions SAS / US only, roles RIFLEMAN 0 SNIPER 1
// HEAVY WEAPONS 2 COMBAT ENGINEER 3; attach 0x47D160 thiscall(soldier, role, faction) ret 8 (per entry: connector index
// 0x55DC40 thiscall(instance, hash), model 0x4E2F20 / load 0x509540 on the world [0x68008C], attach 0x55EBB0
// thiscall(instance, index, model) - model 0 = take off).
const Ds2FixSkinsV1* g_skinsI = nullptr;
uint8_t* SkinEntry(int uniform, int table, int row) {
    if (uniform < 0 || uniform > 1 || table < 0 || table > 3 || row < 0) return nullptr;
    const int count = Rd<int>(0x6354C8 + (uniform * 4 + table) * 4);
    uint8_t* base = Rd<uint8_t*>(0x635524 + (uniform * 4 + table) * 4);
    return base && row < std::min(count, 2) ? base + row * 0x5C : nullptr;
}
int SkinRows(int uniform, int table) {
    int n = 0;
    while (n < 2 && SkinEntry(uniform, table, n)) ++n;
    return n;
}
// The table + row of his current entry (soldier +0xE24), or of his unit's skin key (+0x1D0).
bool FindSkin(void* soldier, int& uniform, int& table, int& row) {
    const uint8_t* cur = Rd<uint8_t*>(soldier, 0xE24);
    const uint32_t key = Rd<uint32_t>(Rd<void*>(soldier, 0x24), 0x1D0);
    for (int pass = 0; pass < 2; ++pass)
        for (int u = 0; u < 2; ++u)
            for (int t = 0; t < 4; ++t)
                for (int r = 0; r < 2; ++r) {
                    uint8_t* e = SkinEntry(u, t, r);
                    if (e && (pass == 0 ? e == cur : Rd<uint32_t>(e, 0) == key)) {
                        uniform = u, table = t, row = r;
                        return true;
                    }
                }
    return false;
}
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::wstring FaceName(int uniform, int table, int row) {
    uint8_t* e = SkinEntry(uniform, table, row);
    std::string first = Str(Rd<const char*>(e, 0x50)), last = Str(Rd<const char*>(e, 0x54));
    std::string n = first.empty() ? last : last.empty() ? first : first + " " + last;
    if (n.empty()) n = "FACE " + std::to_string(row + 1);
    for (char& c : n) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return Widen(n);
}
void TakeOffGear(void* soldier, int role) {
    void* inst = Rd<void*>(soldier, 0x30);
    for (int faction = 0; faction < 2; ++faction)
        for (int n = 0; n < 8; ++n) {
            const uint32_t at = 0x635548 + ((role * 2 + faction) * 8 + n) * 0x28;
            const uint32_t hash = Rd<uint32_t>(at);
            if (!hash) continue;
            int index = -1;
            __try {
                index = reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x55DC40)(inst, hash);
                if (index >= 0) reinterpret_cast<void(__thiscall*)(void*, int, void*)>(0x55EBB0)(inst, index, nullptr);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
}
bool PutOnGear(void* soldier, int role, int faction) {
    const uint32_t fpu = reinterpret_cast<uint32_t(__cdecl*)()>(0x4C12C0)();   // model loads at level precision
    reinterpret_cast<void(__cdecl*)()>(0x4C12A0)();
    bool ok = true;
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int, int)>(0x47D160)(soldier, role, faction);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    reinterpret_cast<void(__cdecl*)(uint32_t)>(0x4C12D0)(fpu);
    return ok;
}
void ApplySkinEntry(void* soldier, uint8_t* entry) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, void*)>(0x475020)(soldier, entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// One soldier's look: skin row (uniform, table, row) and / or gear set (0 SAS kit, 1 Delta kit, 2 none).
void DressLook(void* s, const wchar_t* name, int uniform, int table, int face, bool look, int gear, bool gearToo) {
    void* unit = Rd<void*>(s, 0x24);
    if (look) {
        const int row = std::min(face, std::max(0, SkinRows(uniform, table) - 1));
        uint8_t* entry = SkinEntry(uniform, table, row);
        char body[0x24] = {}, texture[0x24] = {};
        CopyStr(body, sizeof body, unit ? static_cast<const char*>(unit) + (uniform ? 0x2B : 8) : nullptr);
        CopyStr(texture, sizeof texture, entry ? reinterpret_cast<const char*>(entry + 4) : nullptr);
        if (entry) ApplySkinEntry(s, entry);
        const int dressed = g_skinsI ? g_skinsI->Dress(s, body, texture) : 0;
        LOGI("Loadout: %ls look: %s, %ls (body %s, texture %s)%s", name, uniform ? "US Delta" : "SAS",
             FaceName(uniform, table, row).c_str(), body, texture,
             dressed ? "" : " - body / texture NOT changed (ds2fix.skins missing or refused)");
    }
    if (gearToo) {
        const int role = Rd<int8_t>(unit, 0x1CF);
        if (role >= 0 && role < 4) {
            TakeOffGear(s, role);
            const bool ok = gear < 2 ? PutOnGear(s, role, gear) : true;
            LOGI("Loadout: %ls gear: %s%s", name, gear == 0 ? "SAS kit" : gear == 1 ? "Delta kit" : "none",
                 ok ? "" : " - attach faulted");
        }
    }
}
// What this level's soldiers wear differently from how the level made them (co-save chunk LOOK, version 1).
struct LookRecord {
    int32_t soldierId, uniform, table, face, gear, lookChanged, gearChanged, pad;
};
std::vector<LookRecord> g_looks;
void ApplyLook(Member& m) {
    if (m.skinTable < 0) return;
    const bool lookChanged = m.uniform != m.uniformA || m.face != m.faceA;
    const bool gearChanged = m.gear != m.gearA;
    m.uniformA = m.uniform, m.faceA = m.face, m.gearA = m.gear;
    if (!lookChanged && !gearChanged) return;
    DressLook(m.soldier, m.name.c_str(), m.uniform, m.skinTable, m.face, lookChanged, m.gear, gearChanged);
    const int id = Rd<int>(m.soldier, 8);
    g_looks.erase(std::remove_if(g_looks.begin(), g_looks.end(), [&](const LookRecord& r) { return r.soldierId == id; }),
                  g_looks.end());
    const bool look = m.uniform != m.uniform0 || m.face != m.face0, gear = m.gear != m.gear0;
    if (look || gear) g_looks.push_back({id, m.uniform, m.skinTable, m.face, m.gear, look, gear, 0});
}

void CollectLevelDefs() {
    g_levelDefs.clear();
    ForEachSoldier([](uint8_t* s, int, bool) {
        for (int i = 0; i < 25; ++i) {
            void* it = InvAt(s, i);
            if (!it || Rd<int>(it, 4) != 6) continue;
            void* def = Rd<void*>(it, 0x24);
            if (SlotOfDef(def) >= 0 && std::find(g_levelDefs.begin(), g_levelDefs.end(), def) == g_levelDefs.end())
                g_levelDefs.push_back(def);
        }
    });
}

bool Alive(void* soldier) {
    __try {
        void** vt = *reinterpret_cast<void***>(soldier);
        return reinterpret_cast<int(__thiscall*)(void*)>(vt[3])(soldier) == 1;   // vtable +0xC (script API's "alive")
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The name the HUD shows: the skin entry's (soldier +0xE24) last-name record (+0x54 -> char[0x24]; the name getters
// 0x4756F0 / 0x475710 read +0x50 / +0x54), else the unit name.
std::wstring SoldierName(void* soldier) {
    std::string n = Str(static_cast<const char*>(Rd<void*>(Rd<void*>(soldier, 0xE24), 0x54)));
    bool printable = !n.empty();
    for (char c : n) printable = printable && c >= 0x20 && c < 0x7F;
    if (!printable) n = Str(Rd<const char*>(Rd<void*>(soldier, 0x24), 4));
    std::wstring w;
    for (char c : n) w += static_cast<wchar_t>(toupper(static_cast<unsigned char>(c)));
    return w;
}

// ---- unlocks and remembered choices (DS1 loadout.cpp; per mode: single player / split-screen co-op) -------------
// Unlocked = every weapon a squad soldier has carried when the screen opened, plus every weapon a squad soldier picked
// up in play (AddItem 0x4828E0 wrapped). The screen offers a level weapon only when it is unlocked in this mode (an
// enemy's AK-47 after one was picked up once). Remembered = the last choice per squad role (unit +0x1CF) and mode,
// preselected when the screen opens and the weapon is offered. Hidden pse.settings strings in the plugin's key:
// UnlockedWeaponsSP / UnlockedWeaponsCoop ("id;id;"), LoadoutSP<role> / LoadoutCoop<role> ("main;sidearm;launcher",
// "-" = none).
int Mode() { return Rd<int>(0x626C18) ? 1 : 0; }   // split-screen object +0 local multiplayer flag = co-op
const char* const kUnlockKey[2] = {"UnlockedWeaponsSP", "UnlockedWeaponsCoop"};
const char* const kRememberKey[2][4] = {{"LoadoutSP0", "LoadoutSP1", "LoadoutSP2", "LoadoutSP3"},
                                        {"LoadoutCoop0", "LoadoutCoop1", "LoadoutCoop2", "LoadoutCoop3"}};
std::vector<std::string> g_unlocked[2];
bool g_unlocksRead[2] = {};
bool g_swapping = false;   // the screen gives a weapon: a choice, not a find
uint32_t g_playFrames = 0;   // mission frames since the level load ended (kits are handed out while it loads)

std::string GetText(const char* key) {
    char buf[1024] = {};
    if (g_settings) g_settings->GetString(g_self, key, buf, sizeof buf);
    return buf;
}
std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        const size_t j = s.find(';', i);
        out.push_back(s.substr(i, j == std::string::npos ? std::string::npos : j - i));
        if (j == std::string::npos) break;
        i = j + 1;
    }
    return out;
}
std::vector<std::string>& Unlocked(int mode) {
    if (!g_unlocksRead[mode]) {
        g_unlocksRead[mode] = true;
        for (const std::string& id : Split(GetText(kUnlockKey[mode])))
            if (!id.empty()) g_unlocked[mode].push_back(id);
    }
    return g_unlocked[mode];
}
bool IsUnlocked(int mode, const std::string& id) {
    for (const std::string& u : Unlocked(mode))
        if (_stricmp(u.c_str(), id.c_str()) == 0) return true;
    return false;
}
void Unlock(int mode, const std::string& id, const char* how) {
    if (id.empty() || IsUnlocked(mode, id)) return;
    Unlocked(mode).push_back(id);
    std::string v;
    for (const std::string& u : Unlocked(mode)) v += u + ";";
    if (g_settings) g_settings->SetString(g_self, kUnlockKey[mode], v.c_str());
    LOGI("Loadout: %s unlocked (%s, %s)", id.c_str(), mode ? "co-op" : "single player", how);
}
bool IsSquadSoldier(void* s) {
    void* unit = Rd<void*>(s, 0x24);
    return Rd<uint32_t>(s, 4) == 1 && Rd<int8_t>(s, 0x15) == Rd<int8_t>(kPlayerTeam) && Rd<int8_t>(unit, 0x1CF) >= 0;
}

// AddItem 0x4828E0 thiscall(soldier, item, a, b) ret 0xC (entry 83 EC 0C 53 55 56): a squad soldier's pick-up in play
// unlocks the weapon (the level's own kits are given while it loads - dispatch mode 2 - and don't count).
constexpr uint32_t kAddItem = 0x4828E0;
const uint8_t kAddItemEntry[] = {0x83, 0xEC, 0x0C, 0x53, 0x55, 0x56};
void* g_addItemNext = nullptr;
int __fastcall AddItemHook(void* soldier, void*, void* item, int a, int b) {
    const int r = reinterpret_cast<int(__thiscall*)(void*, void*, int, int)>(g_addItemNext)(soldier, item, a, b);
    if (!g_swapping && g_playFrames > 0 && g_game->DispatchMode() == 1 && Enabled()) {
        void* def = Rd<void*>(item, 0x24);
        if (Rd<int>(item, 4) == 6 && SlotOfDef(def) >= 0 && IsSquadSoldier(soldier))
            Unlock(Mode(), Str(Rd<const char*>(def, 4)), "picked up");
    }
    return r;
}

void BuildSquad() {
    g_squad.clear();
    CollectLevelDefs();
    const int team = Rd<int8_t>(kPlayerTeam), mode = Mode();
    std::vector<std::pair<int, void*>> found;
    ForEachSoldier([&](uint8_t* s, int t, bool active) {
        void* unit = Rd<void*>(s, 0x24);
        const int role = Rd<int8_t>(unit, 0x1CF);
        if (t == team && active && role >= 0 && Alive(s)) found.push_back({role, s});
    });
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& f : found)   // the squad's own kits count as found
        for (int slot = 0; slot < kSlots; ++slot)
            if (void* it = ItemInSlot(f.second, slot)) Unlock(mode, Str(Rd<const char*>(Rd<void*>(it, 0x24), 4)), "squad kit");
    for (const auto& f : found) {
        Member m;
        m.soldier = f.second;
        m.role = f.first;
        m.name = SoldierName(f.second);
        m.owner = Rd<int>(0x626C18) ? Rd<int>(f.second, 0xEC4) : 0;
        const std::vector<std::string> remembered = f.first < 4 ? Split(GetText(kRememberKey[mode][f.first])) :
                                                                  std::vector<std::string>();
        for (int slot = 0; slot < kSlots; ++slot) {
            m.item[slot] = ItemInSlot(f.second, slot);
            void* cur = Rd<void*>(m.item[slot], 0x24);
            m.choices[slot].push_back(cur);   // first choice = what he has (nullptr = none)
            for (void* d : g_levelDefs)
                if (SlotOfDef(d) == slot && d != cur && !CarriesDef(f.second, d) &&
                    IsUnlocked(mode, Str(Rd<const char*>(d, 4))))
                    m.choices[slot].push_back(d);
            if (cur && slot != kMain) m.choices[slot].push_back(nullptr);   // sidearm / launcher can be left behind
            if (slot < static_cast<int>(remembered.size()) && !remembered[slot].empty())
                for (size_t i = 0; i < m.choices[slot].size(); ++i) {
                    void* d = m.choices[slot][i];
                    if (remembered[slot] == "-" ? d == nullptr : d && _stricmp(Str(Rd<const char*>(d, 4)).c_str(),
                                                                               remembered[slot].c_str()) == 0) {
                        m.pick[slot] = static_cast<int>(i);
                        break;
                    }
                }
        }
        // look: his skin table / row, the squad's uniform; gear = the uniform's kit (what the level put on)
        int u = 0, t = -1, r = 0;
        if (FindSkin(f.second, u, t, r)) {
            m.skinTable = t;
            m.uniform = m.uniform0 = m.uniformA = u;
            m.face = m.face0 = m.faceA = r;
        }
        m.gear = m.gear0 = m.gearA = std::clamp(Rd<int>(kUniform), 0, 1);
        auto field = [&](size_t i) { return i < remembered.size() && !remembered[i].empty() && remembered[i] != "-" ?
                                                atoi(remembered[i].c_str()) : -1; };
        if (m.skinTable >= 0) {
            if (field(3) >= 0 && field(3) <= 1 && SkinRows(field(3), t) > 0) m.uniform = field(3);
            if (field(4) >= 0 && field(4) < SkinRows(m.uniform, t)) m.face = field(4);
            else if (m.uniform != m.uniform0) m.face = 0;
        }
        if (field(5) >= 0 && field(5) <= 2) m.gear = field(5);
        g_squad.push_back(std::move(m));
    }
}

void RememberChoices() {   // at START MISSION: each role's choice in this mode
    const int mode = Mode();
    for (const Member& m : g_squad) {
        if (m.role < 0 || m.role >= 4) continue;
        std::string v;
        for (int slot = 0; slot < kSlots; ++slot) {
            void* d = m.choices[slot].empty() ? nullptr : m.choices[slot][m.pick[slot]];
            v += (slot ? ";" : "") + (d ? Str(Rd<const char*>(d, 4)) : std::string("-"));
        }
        // fields 4-6: uniform (0 SAS / 1 Delta), face row, gear (0 SAS kit / 1 Delta kit / 2 none); "-" = his own
        v += m.skinTable >= 0 ? ";" + std::to_string(m.uniform) + ";" + std::to_string(m.face) : std::string(";-;-");
        v += ";" + std::to_string(m.gear);
        if (g_settings) g_settings->SetString(g_self, kRememberKey[mode][m.role], v.c_str());
    }
}

// ---- freezing the world ---------------------------------------------------------------------------------------------
float g_savedMult = 1.0f;
int g_savedJoy[4] = {}, g_savedDev[4] = {};
bool g_frozen = false;

void Freeze() {
    if (g_frozen) return;
    g_frozen = true;
    g_savedMult = Rd<float>(kTimeMult);
    if (!(g_savedMult > 0.0f)) g_savedMult = 1.0f;
    for (int i = 0; i < BlockCount(); ++i) {
        g_savedJoy[i] = Rd<int>(Block(i), 0x3A4);
        g_savedDev[i] = Rd<int>(Block(i), 0xC);
    }
}
void HoldFrozen() {   // every frame while open: the game may write these back
    if (!g_frozen) return;
    Wr<float>(reinterpret_cast<void*>(kTimeMult), 0, 0.0001f);
    for (int i = 0; i < BlockCount(); ++i) {
        Wr<int>(Block(i), 0x3A4, -1);
        Wr<int>(Block(i), 0xC, 0);
    }
}
void GiveInputBack() {
    for (int i = 0; i < BlockCount(); ++i) {
        Wr<int>(Block(i), 0x3A4, g_savedJoy[i]);
        Wr<int>(Block(i), 0xC, g_savedDev[i]);
    }
}
// Time runs at once; the players' input comes back once the key / button that closed the screen is released (else the
// Enter / A that started the mission also reached the game - DS1's photo mode had that), at most after 1 s.
DWORD g_inputBackAt = 0;
void Unfreeze(bool waitForRelease = false) {
    if (!g_frozen) return;
    g_frozen = false;
    Wr<float>(reinterpret_cast<void*>(kTimeMult), 0, g_savedMult);
    if (waitForRelease) {
        g_inputBackAt = GetTickCount() + 1000;
        return;
    }
    GiveInputBack();
}

// ---- swapping -------------------------------------------------------------------------------------------------------
void* CreateItem(void* soldier, void* def) {
    const uint32_t fpu = reinterpret_cast<uint32_t(__cdecl*)()>(kFpuSave)();
    reinterpret_cast<void(__cdecl*)()>(kFpuLevel)();
    void* item = nullptr;
    __try {
        item = reinterpret_cast<void*(__thiscall*)(void*, void*)>(kCreateItem)(soldier, def);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        item = nullptr;
        LOGE("[fail] Loadout: CreateInventoryHLO faulted (code %08lX)", GetExceptionCode());
    }
    reinterpret_cast<void(__cdecl*)(uint32_t)>(kFpuRestore)(fpu);
    return item;
}
bool Call2(uint32_t fn, void* self, void* a, int b) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, void*, int)>(fn)(self, a, b);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool Call3(uint32_t fn, void* self, void* a, int b, int c) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, void*, int, int)>(fn)(self, a, b, c);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CallN(uint32_t fn, void* self, int n) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int)>(fn)(self, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void NotSaved(void* item) {   // taken-away weapon: neither counted nor written by the saver (0x4CD8C0 / 0x4CDAE0)
    void* inst = Rd<void*>(item, 0x30);
    if (inst) Wr<uint32_t>(inst, 0x10, Rd<uint32_t>(inst, 0x10) | kSaveNoFlag);
}

struct Given {
    int soldierId;
    int slot;
    void* item;
};
std::vector<Given> g_given;   // this level's

// Magazines for the new gun (DS1's SwapMagazines): the old weapon's share of its own allowance (Weaps "magazines"
// +0xCE x magazine size +0x92; the squad's kits hold up to 2x that - capped at full) x the new one's magazines, at
// least 1. No old weapon in that slot: the new one's full allowance.
int Magazines(void* oldItem, void* newDef) {
    int cap = Rd<uint8_t>(newDef, 0xCE);
    if (cap <= 0 || cap > 60) cap = 3;
    if (!oldItem) return cap;
    void* od = Rd<void*>(oldItem, 0x24);
    const int oldCap = Rd<uint8_t>(od, 0xCE), oldSize = Rd<uint16_t>(od, 0x92);
    if (oldCap <= 0 || oldSize <= 0) return cap;
    const int rounds = Rd<int>(oldItem, 0x58) + Rd<int>(oldItem, 0x8C);
    const float share = std::clamp(rounds / static_cast<float>(oldCap * oldSize), 0.0f, 1.0f);
    return std::max(1, static_cast<int>(std::ceil(share * cap - 0.01f)));
}

bool Swap(Member& m, int slot, void* newDef) {
    void* s = m.soldier;
    void* old = m.item[slot];
    const bool oldHeld = old && Rd<void*>(s, 0x6CC) == old;
    const std::string oldId = Str(Rd<const char*>(Rd<void*>(old, 0x24), 4));
    const std::string newId = Str(Rd<const char*>(newDef, 4));
    void* item = nullptr;
    if (newDef) {   // the new one first (taking a held weapon first falls back to the knife - DS1)
        g_swapping = true;
        item = CreateItem(s, newDef);
        g_swapping = false;
        if (!item || !Carries(s, item)) {
            LOGE("[fail] Loadout: %ls: %s not given (%s)", m.name.c_str(), newId.c_str(),
                 item ? "not in the inventory - merged or full" : "not created");
            if (item) NotSaved(item);
            return false;
        }
        NotSaved(item);   // never in the game's save (see "co-save" above); the co-save chunk KITS brings it back
        const int mags = Magazines(old, newDef), size = Rd<uint16_t>(newDef, 0x92);
        CallN(kSetReserve, item, 0);
        CallN(kAddReserve, item, mags * size);
        if (oldHeld) Call2(kHold, s, item, 0);
        LOGI("Loadout: %ls %s: %s -> %s (%d magazines = %d rounds%s, object id %d)", m.name.c_str(), kSlotNames[slot],
             old ? oldId.c_str() : "none", newId.c_str(), mags, Rd<int>(item, 0x58), oldHeld ? ", held" : "",
             Rd<int>(item, 8));
    } else {
        LOGI("Loadout: %ls %s: %s left behind", m.name.c_str(), kSlotNames[slot], oldId.c_str());
    }
    if (old) {
        if (!Call3(kTakeItem, s, old, 0, 0)) LOGE("[fail] Loadout: TakeItem faulted");
        NotSaved(old);
        // Hold 0x47EFF0 keeps the item held before at soldier +0x6D0 (saved by 0x41A3E1, re-held by 0x47F203 ...);
        // TakeItem leaves it - it must not point at a weapon he no longer carries.
        if (Rd<void*>(s, 0x6D0) == old) Wr<void*>(s, 0x6D0, nullptr);
    }
    m.item[slot] = item;
    if (item) g_given.push_back({Rd<int>(s, 8), slot, item});
    return true;
}

// ---- weapons given by this screen in saves (co-save chunk KITS) ----------------------------------------------------
// The game's grunt restore (0x41AFB0) strips the inventory (0x482CD0) and re-adds the items listed by script id; kit
// items have the ids the scene gave them, a weapon made here has -1 (CreateInventoryHLO's record) - so a load gives the
// soldier neither the swapped-in weapon nor the taken one (not listed). Also, by the code, the restore 0x41C110 skips
// a type-6 record with an unknown id by 0x30 bytes while the record is 0x41ABD0's common header + 0x30 (DS1 had the
// same mismatch, [0x4162A9]) - a weapon with id -1 in the save would shift every record after it. So the screen's
// weapons are flagged not-saved (the game's save never holds one) and the co-save remembers them (soldier script id,
// slot, weapon id, rounds, held) and puts them back on the first mission frame after the load.

struct KitRecord {             // co-save layout, version 1 (64 bytes)
    int32_t soldierId, slot, reserve, fired, held, pad;
    char weapon[40];
};
std::vector<KitRecord> g_pendingKits;
DWORD g_pendingAt = 0;
const PSESerializationInterface* g_ser = nullptr;
constexpr uint32_t kTagKits = PSE_TAG('K', 'I', 'T', 'S');
constexpr uint32_t kTagLook = PSE_TAG('L', 'O', 'O', 'K');   // LookRecord x n: uniform / face / gear changes
std::vector<LookRecord> g_pendingLooks;

void PSE_CALL SaveKits(PSESaveWriter* w, void*) {
    std::vector<KitRecord> out;
    for (const Given& g : g_given) {
        void* s = nullptr;
        ForEachSoldier([&](uint8_t* o, int, bool) {
            if (Rd<int>(o, 8) == g.soldierId && Carries(o, g.item)) s = o;
        });
        if (!s) continue;
        KitRecord r = {};
        r.soldierId = g.soldierId;
        r.slot = g.slot;
        r.reserve = Rd<int>(g.item, 0x58);
        r.fired = Rd<int>(g.item, 0x84);
        r.held = Rd<void*>(s, 0x6CC) == g.item;
        CopyStr(r.weapon, sizeof r.weapon, Rd<const char*>(Rd<void*>(g.item, 0x24), 4));
        out.push_back(r);
    }
    if (!out.empty()) {
        g_ser->WriteChunk(w, kTagKits, 1, out.data(), static_cast<uint32_t>(out.size() * sizeof(KitRecord)));
        LOGI("Loadout: %d swapped weapon(s) written to the co-save", static_cast<int>(out.size()));
    }
    if (!g_looks.empty())
        g_ser->WriteChunk(w, kTagLook, 1, g_looks.data(), static_cast<uint32_t>(g_looks.size() * sizeof(LookRecord)));
}
void PSE_CALL LoadKits(PSESaveReader* r, void*) {
    g_pendingKits.clear();
    g_pendingLooks.clear();
    uint32_t tag = 0, version = 0, len = 0;
    while (g_ser->NextChunk(r, &tag, &version, &len)) {
        if (tag == kTagLook && version == 1 && len % sizeof(LookRecord) == 0) {
            g_pendingLooks.resize(len / sizeof(LookRecord));
            g_ser->Read(r, g_pendingLooks.data(), len);
            continue;
        }
        if (tag != kTagKits || version != 1 || len % sizeof(KitRecord)) continue;
        g_pendingKits.resize(len / sizeof(KitRecord));
        g_ser->Read(r, g_pendingKits.data(), len);
    }
    g_pendingAt = 0;
    if (!g_pendingKits.empty())
        LOGI("Loadout: %d swapped weapon(s) in the co-save - put back after the load", static_cast<int>(g_pendingKits.size()));
}
void PSE_CALL RevertKits(void*) {
    g_pendingKits.clear();
    g_pendingLooks.clear();
}

void* DefById(const char* id) {
    void* found = nullptr;
    const uint32_t n = Rd<uint32_t>(0x635508);
    uint8_t* table = Rd<uint8_t*>(0x635F68);
    for (uint32_t i = 0; i < n && table && !found; ++i)
        if (_stricmp(Str(Rd<const char*>(table + i * 0x3BC, 4)).c_str(), id) == 0) found = table + i * 0x3BC;
    return found;
}

void ApplyPendingKits() {
    for (const KitRecord& k : g_pendingKits) {
        void* s = nullptr;
        ForEachSoldier([&](uint8_t* o, int, bool) {
            if (Rd<int>(o, 8) == k.soldierId) s = o;
        });
        char weapon[sizeof k.weapon + 1] = {};
        memcpy(weapon, k.weapon, sizeof k.weapon);
        void* def = DefById(weapon);
        if (!s || !def || SlotOfDef(def) != k.slot) {
            LOGE("[fail] Loadout: saved weapon %s for soldier %d not put back (%s)", weapon, k.soldierId,
                 !s ? "soldier gone" : !def ? "unknown weapon" : "not a kit weapon");
            continue;
        }
        if (CarriesDef(s, def)) continue;   // the game restored it after all
        g_swapping = true;
        void* item = CreateItem(s, def);
        g_swapping = false;
        if (!item || !Carries(s, item)) {
            LOGE("[fail] Loadout: saved weapon %s for soldier %d not given", weapon, k.soldierId);
            if (item) NotSaved(item);
            continue;
        }
        NotSaved(item);
        CallN(kSetReserve, item, 0);
        CallN(kAddReserve, item, k.reserve);
        Wr<int>(item, 0x84, k.fired);
        if (k.held) Call2(kHold, s, item, 0);
        g_given.push_back({k.soldierId, k.slot, item});
        LOGI("Loadout: soldier %d gets his saved %s back (%d rounds%s)", k.soldierId, weapon, k.reserve,
             k.held ? ", held" : "");
    }
    g_pendingKits.clear();
    for (const LookRecord& l : g_pendingLooks) {
        void* s = nullptr;
        ForEachSoldier([&](uint8_t* o, int, bool) {
            if (Rd<int>(o, 8) == l.soldierId) s = o;
        });
        if (!s || l.uniform < 0 || l.uniform > 1 || l.table < 0 || l.table > 3 || l.gear < 0 || l.gear > 2) continue;
        const std::wstring name = SoldierName(s);
        DressLook(s, name.c_str(), l.uniform, l.table, l.face, l.lookChanged != 0, l.gear, l.gearChanged != 0);
        g_looks.push_back(l);
    }
    g_pendingLooks.clear();
}

void ApplyChoices() {
    int changes = 0;
    for (Member& m : g_squad)
        for (int slot = 0; slot < kSlots; ++slot) {
            void* want = m.choices[slot].empty() ? nullptr : m.choices[slot][m.pick[slot]];
            if (want == Rd<void*>(m.item[slot], 0x24)) continue;
            if (Swap(m, slot, want)) ++changes;
        }
    for (Member& m : g_squad) {
        ApplyLook(m);
        if (m.uniform != m.uniform0 || m.face != m.face0 || m.gear != m.gear0) ++changes;
    }
    LOGI("Loadout: screen closed, %d change(s)", changes);
}

// ---- saves: the header's object counts taken again (see the top) ----------------------------------------------------
void RecountSaveObjects() {
    uint8_t* saver = Rd<uint8_t*>(kSaver);
    void* mgr = Rd<void*>(kObjects);
    if (!saver || !mgr) return;
    static const struct { uint32_t off; int type; } kCounts[] = {{0xC, 1}, {0xE, 0}, {0x10, 2}, {0x12, 3},
                                                                 {0x14, 5}, {0x16, 6}, {0x18, 7}, {0x1C, 9}};
    char diff[256] = {};
    for (const auto& c : kCounts) {
        int n = 0;
        __try {
            n = reinterpret_cast<int(__thiscall*)(void*, int, int, int, int)>(kCountObjects)(mgr, c.type, 3, -1,
                                                                                            kSaveNoFlag);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return;
        }
        const uint16_t before = Rd<uint16_t>(saver, c.off);
        if (before != static_cast<uint16_t>(n))
            snprintf(diff + strlen(diff), sizeof diff - strlen(diff), " type %d %u -> %d", c.type, before, n);
        Wr<uint16_t>(saver, c.off, static_cast<uint16_t>(n));
    }
    if (diff[0]) LOGI("Loadout: save object counts taken again:%s", diff);
#ifndef DS_DIST
    else LOGI("[dev] Loadout: save object counts checked - unchanged");
#endif
}

// Bullets / rockets in flight keep the weapon that fired them (projectile slot +0x88; manager [0x6800E4]: +0 slots of
// 0xA4, +8 count, +0x9C = 0x64 free) and the save stores it by script id - a weapon made here has none, so a loaded
// bullet's weapon was null and its hit test (callback 0x49F150 from the ray query 0x4BFD40 at 0x4E3589) faulted at
// 0x49F189. While the game saves, such a projectile points at a kit weapon of the same soldier (one with a script id);
// put back at PSE_MSG_SAVE_END.
struct ProjectileFix {
    uint8_t* slot;
    void* weapon;
};
std::vector<ProjectileFix> g_projectileFix;
bool IsGiven(void* item) {
    for (const Given& g : g_given)
        if (g.item == item) return true;
    return false;
}
void* KitWeaponWithId(void* soldier) {
    for (int i = 0; i < 25; ++i) {
        void* it = InvAt(soldier, i);
        if (it && Rd<int>(it, 4) == 6 && Rd<int>(it, 8) >= 0) return it;
    }
    return nullptr;
}
void PatchProjectilesForSave() {
    g_projectileFix.clear();
    uint8_t* mgr = Rd<uint8_t*>(0x6800E4);
    uint8_t* slots = Rd<uint8_t*>(mgr, 0);
    const uint32_t n = Rd<uint32_t>(mgr, 8);
    for (uint32_t i = 0; i < n && slots && i < 65536; ++i) {
        uint8_t* p = slots + i * 0xA4;
        if (Rd<uint32_t>(p, 0x9C) == 0x64) continue;
        void* w = Rd<void*>(p, 0x88);
        if (!w || !IsGiven(w)) continue;
        void* sub = KitWeaponWithId(Rd<void*>(w, 0x78));
        g_projectileFix.push_back({p, w});
        if (sub) Wr<void*>(p, 0x88, sub);
        else Wr<uint32_t>(p, 0x9C, 0x64);   // no kit weapon to name: the bullet isn't saved
    }
    if (!g_projectileFix.empty())
        LOGI("Loadout: %d projectile(s) of swapped weapons named after a kit weapon for the save",
             static_cast<int>(g_projectileFix.size()));
}
void UnpatchProjectiles() {   // (a projectile dropped for want of a kit weapon stays dropped)
    for (const ProjectileFix& f : g_projectileFix)
        if (Rd<void*>(f.slot, 0x88) != f.weapon) Wr<void*>(f.slot, 0x88, f.weapon);
    g_projectileFix.clear();
}

// ---- input ----------------------------------------------------------------------------------------------------------
enum Act { kNone = 0, kUp, kDown, kLeft, kRight, kAccept, kNextSoldier, kPrevSoldier };
struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
using XGetState = DWORD(WINAPI*)(DWORD, XState*);
XGetState g_xinput = nullptr;
void LoadXInput() {
    for (const char* dll : {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"})
        if (HMODULE h = LoadLibraryA(dll)) {
            g_xinput = reinterpret_cast<XGetState>(GetProcAddress(h, "XInputGetState"));
            if (g_xinput) return;
        }
}
// Whose input counts: -2 everybody (single player), -1 the keyboard only, j >= 0 the game's joystick j only (a co-op
// player's turn: his input block's device).
int g_inputSource = -2;
const Ds2FixPadsV1* g_pads = nullptr;
uint32_t PadBits(int j) {   // the game's DirectInput joystick j (DualSense / DualShock 4 / Xbox / others) via ds2fix
    const uint32_t b = g_pads ? g_pads->Buttons(j) : 0;
    uint32_t bits = 0;
    if (b & DS2FIX_PAD_UP) bits |= 1u << kUp;
    if (b & DS2FIX_PAD_DOWN) bits |= 1u << kDown;
    if (b & DS2FIX_PAD_LEFT) bits |= 1u << kLeft;
    if (b & DS2FIX_PAD_RIGHT) bits |= 1u << kRight;
    if (b & (DS2FIX_PAD_ACCEPT | DS2FIX_PAD_START)) bits |= 1u << kAccept;
    if (b & DS2FIX_PAD_R1) bits |= 1u << kNextSoldier;
    if (b & DS2FIX_PAD_L1) bits |= 1u << kPrevSoldier;
    return bits;
}
uint32_t RawInput() {   // bit per Act
    uint32_t bits = 0;
    HWND w = static_cast<HWND>(g_game->GameWindow());
    if (w && GetForegroundWindow() == w) {
        if (g_inputSource >= 0) return PadBits(g_inputSource);
        if (g_pads && g_inputSource == -2)
            for (int j = 0; j < g_pads->Count(); ++j) bits |= PadBits(j);
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        if (down(VK_UP) || down('W')) bits |= 1u << kUp;
        if (down(VK_DOWN) || down('S')) bits |= 1u << kDown;
        if (down(VK_LEFT) || down('A')) bits |= 1u << kLeft;
        if (down(VK_RIGHT) || down('D')) bits |= 1u << kRight;
        if (down(VK_RETURN) || down(VK_SPACE)) bits |= 1u << kAccept;
        if (down(VK_TAB) || down('E') || down(VK_NEXT)) bits |= 1u << kNextSoldier;
        if (down('Q') || down(VK_PRIOR)) bits |= 1u << kPrevSoldier;
        if (g_xinput && g_inputSource == -2)
            for (DWORD p = 0; p < 4; ++p) {
                XState x{};
                if (g_xinput(p, &x) != 0) continue;
                const WORD b = x.buttons;
                if ((b & 0x0001) || x.ly > 16000) bits |= 1u << kUp;
                if ((b & 0x0002) || x.ly < -16000) bits |= 1u << kDown;
                if ((b & 0x0004) || x.lx < -16000) bits |= 1u << kLeft;
                if ((b & 0x0008) || x.lx > 16000) bits |= 1u << kRight;
                if (b & 0x1010) bits |= 1u << kAccept;        // A / Start
                if (b & 0x0200) bits |= 1u << kNextSoldier;   // RB
                if (b & 0x0100) bits |= 1u << kPrevSoldier;   // LB
            }
    }
    return bits;
}
uint32_t g_prevBits = ~0u;   // everything counts as held when the screen opens
DWORD g_repeatAt = 0;
#ifndef DS_DIST
std::string g_devKeys;
size_t g_devKeyPos = 0;
DWORD g_devKeyAt = 0, g_devKeyMs = 700;
Act DevKey() {   // Dev\LoadoutKeys "R,D,L,T,S" (U D L R, T next soldier, P previous, S start, W wait)
    if (g_devKeyPos >= g_devKeys.size() || GetTickCount() < g_devKeyAt) return kNone;
    g_devKeyAt = GetTickCount() + g_devKeyMs;
    const char c = static_cast<char>(toupper(static_cast<unsigned char>(g_devKeys[g_devKeyPos])));
    while (g_devKeyPos < g_devKeys.size() && g_devKeys[g_devKeyPos] != ',') ++g_devKeyPos;
    if (g_devKeyPos < g_devKeys.size()) ++g_devKeyPos;
    LOGI("[dev] Loadout: key %c", c);
    switch (c) {
    case 'U': return kUp;
    case 'D': return kDown;
    case 'L': return kLeft;
    case 'R': return kRight;
    case 'S': return kAccept;
    case 'T': return kNextSoldier;
    case 'P': return kPrevSoldier;
    default: return kNone;
    }
}
#endif
Act NextAct() {
#ifndef DS_DIST
    if (Act a = DevKey()) return a;
#endif
    const uint32_t bits = RawInput();
    const uint32_t pressed = bits & ~g_prevBits;
    g_prevBits = bits;
    const DWORD now = GetTickCount();
    for (int a = kUp; a <= kPrevSoldier; ++a)
        if (pressed & (1u << a)) {
            g_repeatAt = now + 400;
            return static_cast<Act>(a);
        }
    // held Up / Down / Left / Right repeat
    if (bits && now >= g_repeatAt)
        for (int a = kUp; a <= kRight; ++a)
            if (bits & (1u << a)) {
                g_repeatAt = now + 120;
                return static_cast<Act>(a);
            }
    return kNone;
}

void Close();
int StepMember(int d) {   // the next / previous soldier of the player whose turn it is
    const int n = static_cast<int>(g_squad.size());
    for (int i = 1; i <= n; ++i) {
        const int k = ((g_member + d * i) % n + n) % n;
        if (Mine(g_squad[k])) return k;
    }
    return g_member;
}
int MineCount() {
    int c = 0;
    for (const Member& m : g_squad) c += Mine(m);
    return c;
}
int MineIndex() {
    int c = 0;
    for (int i = 0; i < g_member && i < static_cast<int>(g_squad.size()); ++i) c += Mine(g_squad[i]);
    return c;
}
int TurnDevice(int p) {   // the turn's player's joystick (-1 = keyboard): the co-op session's, else his input block's
    if (g_coop && g_coop->Players() > p) return g_coop->Joystick(p);
    return p < 4 ? g_savedJoy[p] : -1;
}
// From player p on: the first player with soldiers gets the turn; false = nobody left.
bool StartTurn(int p) {
    const int players = std::clamp(Rd<int>(0x626C1C), 1, 4);
    for (; p < players; ++p)
        for (int i = 0; i < static_cast<int>(g_squad.size()); ++i)
            if (g_squad[i].owner == p) {
                g_turn = p;
                g_member = i;
                g_row = kRowFirstSlot;
                g_inputSource = TurnDevice(p);
                g_prevBits = ~0u;   // the button that ended the last turn is still held
                g_dirty = true;
                LOGI("Loadout: player %d's turn (%s %d)", p + 1, g_inputSource < 0 ? "keyboard" : "pad",
                     g_inputSource);
                return true;
            }
    return false;
}
void NextTurnOrClose() {
    if (g_turn >= 0 && StartTurn(g_turn + 1)) return;
    Close();
}
void HandleInput() {
    const Act a = NextAct();
    if (a == kNone || g_squad.empty()) return;
    g_dirty = true;
    Member& m = g_squad[g_member];
    switch (a) {
    case kUp: g_row = (g_row + kRows - 1) % kRows; break;
    case kDown: g_row = (g_row + 1) % kRows; break;
    case kNextSoldier: g_member = StepMember(1); break;
    case kPrevSoldier: g_member = StepMember(-1); break;
    case kLeft:
    case kRight: {
        const int d = a == kRight ? 1 : -1;
        if (g_row == kRowSoldier) g_member = StepMember(d);
        else if (g_row >= kRowFirstSlot && g_row < kRowFirstSlot + kSlots) {
            const int slot = g_row - kRowFirstSlot;
            auto& c = m.choices[slot];
            if (!c.empty()) m.pick[slot] = (m.pick[slot] + static_cast<int>(c.size()) + d) % static_cast<int>(c.size());
        } else if (g_row == kRowUniform && m.skinTable >= 0) {
            const int other = 1 - m.uniform;
            if (SkinRows(other, m.skinTable) > 0) {
                m.uniform = other;
                m.face = std::min(m.face, SkinRows(other, m.skinTable) - 1);
            }
        } else if (g_row == kRowFace && m.skinTable >= 0) {
            const int rows = SkinRows(m.uniform, m.skinTable);
            if (rows > 1) m.face = (m.face + rows + d) % rows;
        } else if (g_row == kRowGear) {
            m.gear = (m.gear + 3 + d) % 3;
        }
        ApplyLook(m);   // uniform / face / gear show at once (nothing if unchanged)
        break;
    }
    case kAccept: NextTurnOrClose(); break;
    default: break;
    }
}

/// ---- drawing: the game's own menu pieces at EndScene ---------------------------------------------------------------
// The in-game 2D draws run immediately (FVF 0x144 quads through the renderer [0x68B18C]'s cached wrappers: SetTexture
// 0x5364E0 (cache +0x41A7C + stage x 4), SetFVF 0x536970, DrawPrimitiveUP 0x53D650), so they can be called inside the
// frame's last scene - the layer's PSE_EVENT_END_SCENE (one EndScene per Present in missions, measured):
//   menu row bar   0x43B990 cdecl(x, y, width, highlighted, 0, 0) -> 0x439C10: images 2 / 4 + the stretched middle
//                  (0x52C1E0) of the interface sheet [0x62F434] - what every DS2 menu row draws (item draw 0x42DC30)
//   text           0x529490 thiscall(font, text, x, y middle, 1); colour 0x526E20(font, argb), alpha 0x526E40(font, a);
//                  fonts [0x62F440] large (menus), [0x62F43C] small; font +0 sheet (+0x20 flags, +0x28 y scale = the
//                  HUD's k in missions), +0x18 line height
// Under them one plain dark quad (own device calls inside a D3DSBT_ALL state block, so the device - and with it the
// game's state cache - is as before when the game's pieces draw).
struct Line {
    std::wstring left, right;
    uint32_t colour;   // 0xRRGGBB
    bool selected, title, minor;
};
IDirect3DDevice9* g_dev = nullptr;
IDirect3DStateBlock9* g_state = nullptr;

void ReleaseGraphics() {
    if (g_state) g_state->Release();
    g_state = nullptr;
    g_dirty = true;
}

std::vector<Line> Lines() {
    std::vector<Line> out;
    const Member& m = g_squad[g_member];
    wchar_t title[64];
    if (g_turn >= 0) swprintf_s(title, L"SQUAD LOADOUT - PLAYER %d", g_turn + 1);
    else wcscpy_s(title, L"MISSION LOADOUT");
    out.push_back({title, L"", 0xE8D27A, false, true, false});
    wchar_t who[64];
    swprintf_s(who, L"<  %s  >   %d / %d", m.name.c_str(), MineIndex() + 1, MineCount());
    out.push_back({L"SOLDIER", who, 0xFFFFFF, g_row == kRowSoldier, false, false});
    const bool look = m.skinTable >= 0;
    const bool bothUniforms = look && SkinRows(0, m.skinTable) > 0 && SkinRows(1, m.skinTable) > 0;
    std::wstring uni = look ? (m.uniform ? L"US DELTA" : L"SAS") : L"-";
    if (bothUniforms) uni = L"<  " + uni + L"  >";
    out.push_back({L"UNIFORM", uni, m.uniform != m.uniform0 ? 0xFFD860u : 0xFFFFFFu, g_row == kRowUniform, false, false});
    std::wstring face = look ? FaceName(m.uniform, m.skinTable, m.face) : L"-";
    if (look && SkinRows(m.uniform, m.skinTable) > 1) face = L"<  " + face + L"  >";
    out.push_back({L"FACE", face, m.face != m.face0 || m.uniform != m.uniform0 ? 0xFFD860u : 0xFFFFFFu,
                   g_row == kRowFace, false, false});
    static const wchar_t* kGear[] = {L"SAS KIT", L"DELTA KIT", L"NONE"};
    out.push_back({L"GEAR", L"<  " + std::wstring(kGear[m.gear]) + L"  >", m.gear != m.gear0 ? 0xFFD860u : 0xFFFFFFu,
                   g_row == kRowGear, false, false});
    for (int slot = 0; slot < kSlots; ++slot) {
        const auto& c = m.choices[slot];
        void* def = c.empty() ? nullptr : c[m.pick[slot]];
        std::wstring v = WeaponName(def);
        if (c.size() > 1) v = L"<  " + v + L"  >";
        out.push_back({std::wstring(kSlotNames[slot], kSlotNames[slot] + strlen(kSlotNames[slot])), v,
                       m.pick[slot] ? 0xFFD860u : 0xFFFFFFu, g_row == slot + kRowFirstSlot, false, false});
    }
    bool more = false;   // another player's turn follows
    for (const Member& o : g_squad) more |= g_turn >= 0 && o.owner > g_turn;
    out.push_back({more ? L"DONE - NEXT PLAYER" : L"START MISSION", L"", 0xFFFFFF, g_row == kRowStart, false, false});
    out.push_back({L"UP / DOWN: ROW     LEFT / RIGHT: CHANGE", L"", 0xB0B0A0, false, false, true});
    out.push_back({L"TAB / LB / RB: SOLDIER     ENTER / A: START MISSION", L"", 0xB0B0A0, false, false, true});
    return out;
}

std::string Narrow(const std::wstring& w) {
    std::string out(w.size() * 2 + 1, '\0');
    const int n = WideCharToMultiByte(1252, 0, w.c_str(), -1, out.data(), static_cast<int>(out.size()), "?", nullptr);
    out.resize(n > 0 ? n - 1 : 0);
    return out;
}

void GameText(void* font, const char* text, int x, int yMiddle, uint32_t rgb) {
    __try {
        reinterpret_cast<void(__thiscall*)(void*, int)>(0x526E40)(font, 255);
        reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(0x526E20)(font, 0xFF000000u | rgb);
        reinterpret_cast<void(__thiscall*)(void*, const char*, int, int, int)>(0x529490)(font, text, x, yMiddle, 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void GameBar(int x, int y, int width, bool highlighted) {
    __try {
        reinterpret_cast<void(__cdecl*)(int, int, int, int, int, int)>(0x43B990)(x, y, width, highlighted ? 1 : 0, 0, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
float SheetScale(void* font) {
    void* sheet = Rd<void*>(font, 0);
    const float k = (Rd<uint32_t>(sheet, 0x20) & 1) ? Rd<float>(sheet, 0x28) : 1.0f;
    return k > 0.2f && k < 10.0f ? k : 1.0f;
}

void Backdrop(IDirect3DDevice9* dev, const D3DSURFACE_DESC& desc, float x0, float y0, float x1, float y1) {
    if (!g_state && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_state))) return;
    g_state->Capture();
    D3DVIEWPORT9 vp = {0, 0, desc.Width, desc.Height, 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetTexture(0, nullptr);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    struct V { float x, y, z, w; DWORD c; };
    auto quad = [&](float a, float b, float c, float d, DWORD col) {
        const V q[4] = {{a, b, 0, 1, col}, {c, b, 0, 1, col}, {a, d, 0, 1, col}, {c, d, 0, 1, col}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V));
    };
    const float e = std::max(2.0f, desc.Height / 480.0f);
    quad(x0 - e, y0 - e, x1 + e, y1 + e, 0xE0566238);   // olive frame, the menus' colour
    quad(x0, y0, x1, y1, 0xD80C100A);
    g_state->Apply();
}

void Draw(IDirect3DDevice9* dev) {
    if (dev != g_dev) {
        ReleaseGraphics();
        g_dev = dev;
    }
    // only onto the back buffer (a scene drawn into a texture ends with EndScene too)
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return;
    const bool onBackBuffer = SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb == rt;
    D3DSURFACE_DESC desc;
    rt->GetDesc(&desc);
    rt->Release();
    if (bb) bb->Release();
    if (!onBackBuffer) return;
    void* big = Rd<void*>(0x62F440);
    void* smallFont = Rd<void*>(0x62F43C);
    if (!big || !smallFont) return;
    const float k = SheetScale(big);
    const std::vector<Line> lines = Lines();
    const int W = static_cast<int>(desc.Width), H = static_cast<int>(desc.Height);
    // a row = the bar image's height (interface sheet record 2: +0x10 height) + a gap
    const int barH = static_cast<int>(Rd<uint16_t>(Rd<uint8_t*>(Rd<void*>(0x62F434), 4), 2 * 0x1C + 0x10) * k);
    const int rowH = std::max(barH + static_cast<int>(6 * k), static_cast<int>(26 * k));
    const int panelW = std::min(W - 40, static_cast<int>(560 * k)), pad = static_cast<int>(14 * k);
    int panelH = pad * 2;
    for (const Line& l : lines) panelH += l.title ? rowH + rowH / 4 : l.minor ? rowH * 2 / 3 : rowH;
    // left of the centre: the camera puts the selected soldier in the right half (SetViewHook)
    const int x0 = std::max(20, W / 2 - panelW - static_cast<int>(20 * k)), y0 = (H - panelH) / 2;
    Backdrop(dev, desc, static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x0 + panelW),
             static_cast<float>(y0 + panelH));
    const int valueX = x0 + pad + static_cast<int>(panelW * 0.30f);
    int y = y0 + pad;
    for (const Line& l : lines) {
        const std::string left = Narrow(l.left), right = Narrow(l.right);
        if (l.title) {
            GameText(big, left.c_str(), x0 + pad, y + rowH / 2, l.colour);
            y += rowH + rowH / 4;
            continue;
        }
        if (l.minor) {
            GameText(smallFont, left.c_str(), x0 + pad, y + rowH / 3, l.colour);
            y += rowH * 2 / 3;
            continue;
        }
        GameBar(x0 + pad, y + (rowH - barH) / 2, panelW - 2 * pad, l.selected);
        GameText(big, left.c_str(), x0 + pad + static_cast<int>(10 * k), y + rowH / 2, l.selected ? 0xFFFFFF : 0xC8C8B4);
        if (!right.empty()) GameText(big, right.c_str(), valueX, y + rowH / 2, l.colour);
        y += rowH;
    }
    g_dirty = false;
}

// ---- open / close ---------------------------------------------------------------------------------------------------
bool g_mission = false, g_doneThisLevel = false, g_fromSave = false, g_loadPending = false;
DWORD g_levelAt = 0, g_quietSince = 0;
float g_vehPos[3] = {};
DWORD g_closedAt = 0;

void Open() {
    BuildSquad();
    if (g_squad.empty()) {
        LOGI("Loadout: no squad soldiers found - no screen");
        return;
    }
    for (Member& m : g_squad) ApplyLook(m);   // remembered uniform / face / gear on at once
    g_member = 0;
    g_row = kRowFirstSlot;
    g_turn = -1;
    g_inputSource = -2;
    g_camStart = GetTickCount();
    g_camLogged = false;
    g_open = true;
    g_dirty = true;
    g_prevBits = ~0u;
    Freeze();
    HoldFrozen();
    if (Rd<int>(0x626C18) && Rd<int>(0x626C1C) >= 2 && !StartTurn(0)) g_turn = -1, g_inputSource = -2;
    int choices = 0;
    for (const Member& m : g_squad)
        for (int s = 0; s < kSlots; ++s) choices += static_cast<int>(m.choices[s].size());
    LOGI("Loadout: screen open, %d squad soldier(s), %d level weapon(s), %d choice(s)", static_cast<int>(g_squad.size()),
         static_cast<int>(g_levelDefs.size()), choices);
    for (void* d : g_levelDefs)
        LOGI("Loadout:   level weapon %s (%ls) slot %s%s", Str(Rd<const char*>(d, 4)).c_str(), WeaponName(d).c_str(),
             kSlotNames[SlotOfDef(d)], IsUnlocked(Mode(), Str(Rd<const char*>(d, 4))) ? "" : " - not unlocked yet");
    for (const Member& m : g_squad)
        for (int s = 0; s < kSlots; ++s)
            if (m.pick[s])
                LOGI("Loadout:   %ls %s preselected (remembered): %ls", m.name.c_str(), kSlotNames[s],
                     WeaponName(m.choices[s][m.pick[s]]).c_str());
#ifndef DS_DIST
    if (g_pads)
        for (int j = 0; j < g_pads->Count(); ++j)
            LOGI("[dev] Loadout: pad %d kind %d (1 PlayStation, 2 Xbox, 3 other), held 0x%X", j, g_pads->Kind(j),
                 g_pads->Buttons(j));
    g_devKeys = DevString("LoadoutKeys");
    g_devKeyPos = 0;
    g_devKeyMs = DevDword("LoadoutKeyMs", 700);
    g_devKeyAt = GetTickCount() + DevDword("LoadoutKeyDelayMs", 2500);
#endif
}

void Close() {
    if (!g_open) return;
    g_open = false;
    g_turn = -1;
    g_inputSource = -2;
    ApplyChoices();
    RememberChoices();
    Unfreeze(true);
    g_closedAt = GetTickCount();
    ReleaseGraphics();
}

// Intro over, player in control, his vehicle (if any) standing still.
bool PlainPlay() {
    if (g_game->FrontEndState() != 0x13 || g_game->DispatchMode() != 1 || g_game->InCutscene()) return false;
    uint8_t* b0 = Block(0);
    void* soldier = Rd<void*>(b0, 0x2EC);
    if (!Rd<int>(b0, 8) || !soldier) return false;
    if (Rd<int>(Rd<void*>(kViewCtrl), 8) != 0) return false;   // letterbox opening / on / closing
    return true;
}
// The vehicle moved less than 5 cm in the last second (sampled once a second: frame-rate independent).
DWORD g_vehSampleAt = 0;
bool g_vehStill = false;
bool VehicleStill(void* soldier) {
    void* veh = Rd<void*>(soldier, 0x690);
    if (!veh) return true;
    void* inst = Rd<void*>(veh, 0x30);
    const DWORD now = GetTickCount();
    if (g_vehSampleAt && now - g_vehSampleAt < 1000) return g_vehStill;
    float p[3] = {Rd<float>(inst, 0x108), Rd<float>(inst, 0x10C), Rd<float>(inst, 0x110)};
    const float d = std::fabs(p[0] - g_vehPos[0]) + std::fabs(p[1] - g_vehPos[1]) + std::fabs(p[2] - g_vehPos[2]);
    g_vehStill = g_vehSampleAt && d < 5.0f;   // cm
    memcpy(g_vehPos, p, sizeof p);
    g_vehSampleAt = now;
    return g_vehStill;
}

// ---- camera on the selected soldier (DS1 customise.cpp CameraView) ------------------------------------------------
// The camera manager [0x680120] applies camera n with 0x4E0860: (+8 cameras of 0x1B4: +0x10 rotation, +0x28 eye;
// manager +0x38 = 1 -> eye override +0x3C, +0x30 the view), at 0x4E08CB `call 0x544290` thiscall(view, eye*, rot*) ret 8
// (= DS1's SetView FUN_0054fdf0 at 0x4D672D). While the screen is up that call gets an eye orbiting the selected soldier
// (2.6 m away, 1.2 m up, a slow turn) and the rotation looking at his chest: degrees, x = pitch (> 0 looks down),
// y = yaw (forward = (sin, cos) in x / z) as in DS1 - checked against the game's own camera (dev log). A soldier in a
// vehicle has no position of his own (instance +0x108 stays 0): the camera orbits the vehicle then.
constexpr uint32_t kSetViewCall = 0x4E08CB, kSetView = 0x544290;
void* g_setViewNext = nullptr;
bool FocusPoint(void* soldier, float out[3]) {
    void* veh = Rd<void*>(soldier, 0x690);
    void* inst = Rd<void*>(veh ? veh : soldier, 0x30);
    out[0] = Rd<float>(inst, 0x108), out[1] = Rd<float>(inst, 0x10C), out[2] = Rd<float>(inst, 0x110);
    if (out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f) return false;
    out[1] += veh ? 170.0f : 120.0f;
    return true;
}
void __fastcall SetViewHook(void* view, void*, float* eye, float* rot) {
    using Next = void(__thiscall*)(void*, float*, float*);
    if (!g_open || g_squad.empty() || g_member >= static_cast<int>(g_squad.size())) {
        reinterpret_cast<Next>(g_setViewNext)(view, eye, rot);
        return;
    }
    float target[3];
    if (!FocusPoint(g_squad[g_member].soldier, target)) {
        reinterpret_cast<Next>(g_setViewNext)(view, eye, rot);
        return;
    }
#ifndef DS_DIST
    if (!g_camLogged) {   // the convention check: the game's own camera vs the yaw / pitch towards the soldier
        g_camLogged = true;
        const float dx = target[0] - eye[0], dy = target[1] - eye[1], dz = target[2] - eye[2];
        LOGI("[dev] Loadout: game camera eye %.0f %.0f %.0f rot %.1f %.1f %.1f; towards the soldier yaw %.1f pitch %.1f",
             eye[0], eye[1], eye[2], rot[0], rot[1], rot[2], std::atan2(dx, dz) * 57.29578f,
             std::atan2(-dy, std::sqrt(dx * dx + dz * dz)) * 57.29578f);
    }
#endif
    const bool inVehicle = Rd<void*>(g_squad[g_member].soldier, 0x690) != nullptr;
    const float dist = inVehicle ? 650.0f : 260.0f, height = inVehicle ? 180.0f : 40.0f;
    const float yaw = (GetTickCount() - g_camStart) * 0.012f;   // 12 degrees a second
    float myEye[3] = {target[0] - std::sin(yaw / 57.29578f) * dist, target[1] + height,
                      target[2] - std::cos(yaw / 57.29578f) * dist};
    const float dx = target[0] - myEye[0], dy = target[1] - myEye[1], dz = target[2] - myEye[2];
    // looking a little left of him puts him in the right half, beside the panel
    float myRot[3] = {std::atan2(-dy, std::sqrt(dx * dx + dz * dz)) * 57.29578f,
                      std::atan2(dx, dz) * 57.29578f - (inVehicle ? 12.0f : 16.0f), 0.0f};
    reinterpret_cast<Next>(g_setViewNext)(view, myEye, myRot);
}

#ifndef DS_DIST
// ---- dev: what the squad carries, and a scripted fire test ---------------------------------------------------------
void DumpSquad(const char* when) {
    ForEachSoldier([when](uint8_t* s, int team, bool active) {
        void* unit = Rd<void*>(s, 0x24);
        if (team != Rd<int8_t>(kPlayerTeam) || !active || Rd<int8_t>(unit, 0x1CF) < 0) return;
        std::string kit;
        void* held = Rd<void*>(s, 0x6CC);
        for (int i = 0; i < 25; ++i) {
            void* it = InvAt(s, i);
            if (!it || Rd<int>(it, 4) != 6 || SlotOfDef(Rd<void*>(it, 0x24)) < 0) continue;
            char b[160];
            snprintf(b, sizeof b, " %s%s(%d+%d #%d)", it == held ? "*" : "",
                     Str(Rd<const char*>(Rd<void*>(it, 0x24), 4)).c_str(), Rd<int>(it, 0x8C), Rd<int>(it, 0x58),
                     Rd<int>(it, 8));
            kit += b;
        }
        LOGI("[dev] Loadout: %s %s (id %d, vehicle %p):%s", when, Str(Rd<const char*>(unit, 4)).c_str(), Rd<int>(s, 8),
             Rd<void*>(s, 0x690), kit.c_str());
    });
}

// Dev\LoadoutFire = script ids "6,7": after the screen closes, those soldiers dismount (API 0x780), then pull the
// trigger of what they hold in bursts (API start_firing 0x694 / stop_firing 0x698) for Dev\LoadoutFireMs (20 s);
// every change of the reserve (+0x58) / rounds fired since the last reload (+0x84) is logged with the time - a reload
// that refills at once = no reload animation. Dev\LoadoutControl = script id: the player takes that soldier over
// (API take_control 0x63C) 1 s after the dismount, so the screenshots show him.
struct FireWatch {
    int id;
    void* lastHeld;
    int lastRes, lastFired;
};
std::vector<FireWatch> g_fire;
int g_controlId = -1;
DWORD g_fireStart = 0, g_fireLen = 20000, g_fireLast = 0;
bool g_firing = false, g_fireDone = true;
void Api(uint32_t slot, int arg) {
    __try {
        reinterpret_cast<void(__cdecl*)(int)>(Rd<uint32_t>(kApi + slot))(arg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGE("[dev] Loadout: API slot 0x%X faulted", slot);
    }
}
void* ById(int id) {
    __try {
        void* mgr = *reinterpret_cast<void**>(kObjects);
        return mgr ? reinterpret_cast<void*(__thiscall*)(void*, int)>(kById)(mgr, id) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
// Dev\LoadoutPickupTest = 1: 3 s after the screen closed, an enemy's hand-held weapon that isn't unlocked moves to
// Bradley (TakeItem from the enemy, then the game's AddItem - the wrapped call a real pick-up goes through).
bool g_pickupDone = false;
void PickupTest() {
    if (g_pickupDone || !g_closedAt || GetTickCount() - g_closedAt < 3000 || !DevDword("LoadoutPickupTest", 0)) return;
    g_pickupDone = true;
    void* bradley = ById(0);
    void* from = nullptr;
    void* item = nullptr;
    ForEachSoldier([&](uint8_t* s, int, bool) {
        if (item || IsSquadSoldier(s)) return;
        for (int i = 0; i < 25 && !item; ++i) {
            void* it = InvAt(s, i);
            void* def = Rd<void*>(it, 0x24);
            if (it && Rd<int>(it, 4) == 6 && SlotOfDef(def) >= 0 && !IsUnlocked(Mode(), Str(Rd<const char*>(def, 4))) &&
                !CarriesDef(bradley, def))
                from = s, item = it;
        }
    });
    if (!item || !bradley) {
        LOGI("[dev] Loadout: pick-up test - nothing to pick up");
        return;
    }
    Call3(kTakeItem, from, item, 0, 0);
    Call3(kAddItem, bradley, item, 0, 0);   // (through the wrapped entry: AddItemHook sees it)
    LOGI("[dev] Loadout: pick-up test - %s moved to Bradley", Str(Rd<const char*>(Rd<void*>(item, 0x24), 4)).c_str());
}

void FireTest() {
    if (g_fireDone || g_fire.empty() || !g_closedAt) return;
    const DWORD now = GetTickCount();
    if (!g_fireStart) {
        if (now - g_closedAt < 1500) return;
        g_fireStart = now;
        for (const FireWatch& f : g_fire) Api(0x780, f.id);   // dismount
        LOGI("[dev] Loadout: fire test on %d soldier(s) (dismount first)", static_cast<int>(g_fire.size()));
        return;
    }
    const DWORD t = now - g_fireStart;
    static bool controlled = false;
    if (t < 1000) controlled = false;
    if (!controlled && t > 1000 && g_controlId >= 0) {
        controlled = true;
        Api(0x63C, g_controlId);
        LOGI("[dev] Loadout: player takes over soldier %d", g_controlId);
    }
    if (t > g_fireLen + 3000) {
        for (const FireWatch& f : g_fire) Api(0x698, f.id);
        g_fireDone = true;
        LOGI("[dev] Loadout: fire test done");
        return;
    }
    for (FireWatch& f : g_fire) {
        void* held = Rd<void*>(ById(f.id), 0x6CC);
        const int res = Rd<int>(held, 0x58), fired = Rd<int>(held, 0x84);
        if (held != f.lastHeld || res != f.lastRes || (fired == 0) != (f.lastFired == 0) || fired < f.lastFired) {
            LOGI("[dev] Loadout: fire t %5u soldier %d held %s reserve %d fired since reload %d", t, f.id,
                 Str(Rd<const char*>(Rd<void*>(held, 0x24), 4)).c_str(), res, fired);
        }
        f.lastHeld = held, f.lastRes = res, f.lastFired = fired;
    }
    if (t < 3000) return;   // 3 s to get out of the vehicle
    if (now - g_fireLast > (g_firing ? 400u : 150u)) {
        g_fireLast = now;
        g_firing = !g_firing;
        for (const FireWatch& f : g_fire) Api(g_firing ? 0x694 : 0x698, f.id);
    }
}
// Dev\LoadoutLoadSave = save path without ".sav": at the main menu, LOAD GAME's own steps (its handler 0x45EC3F..
// 0x45ECAB: Save_Load, state 0x13, flow entry by the campaign's level name 0x430EC0, 0x40A3D0, 0x4CCB10, 0x4CCB90, then
// the front-end update answers 2 = load the flow's level) - a full load of that save, as the menu does it.
std::wstring g_devLoad;
bool g_devLoadDone = false, g_frontEndStart = false;
DWORD g_frontEndAt = 0;
void* g_feUpdateNext = nullptr;
int __cdecl FrontEndUpdate() {
    const int r = reinterpret_cast<int(__cdecl*)()>(g_feUpdateNext)();
    if (!g_frontEndStart) return r;
    g_frontEndStart = false;
    LOGI("[dev] Loadout: front end -> load the saved level");
    return 2;
}
void DevLoadSave() {
    if (g_devLoad.empty() || g_devLoadDone || !g_frontEndAt || GetTickCount() - g_frontEndAt < 3000) return;
    if (g_game->FrontEndState() != 2) return;
    g_devLoadDone = true;
    void* saver = Rd<void*>(kSaver);
    int ok = 0;
    __try {
        ok = reinterpret_cast<int(__thiscall*)(void*, const wchar_t*)>(0x41C7C0)(saver, g_devLoad.c_str());
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = -1;
    }
    LOGI("[dev] Loadout: Save_Load %ls -> %d", g_devLoad.c_str(), ok);
    if (ok != 1) return;
    uint8_t* camp = Rd<uint8_t*>(0x62F454);
    if (Rd<int>(camp, 8) == 2) {
        LOGE("[dev] Loadout: a mission-start save (campaign +8 = 2) - not handled by the dev load");
        return;
    }
    reinterpret_cast<void(__cdecl*)(int)>(0x43DFD0)(0x13);
    reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(0x430EC0)(reinterpret_cast<void*>(0x631608),
                                                                          reinterpret_cast<const char*>(camp + 0x18), 0);
    reinterpret_cast<void(__cdecl*)()>(0x40A3D0)();
    reinterpret_cast<void(__cdecl*)()>(0x4CCB10)();
    reinterpret_cast<void(__cdecl*)()>(0x4CCB90)();
    g_frontEndStart = true;
}
DWORD g_vehLogUntil = 0, g_vehLogLast = 0;
void VehicleLog() {   // Dev\LoadoutVehicleLog = script id (Mission 1's Land Rover: 195): its position every 2 s
    static int id = -2;
    if (id == -2) id = static_cast<int>(DevDword("LoadoutVehicleLog", 0xFFFFFFFF));
    if (id < 0 || !g_mission) return;
    const DWORD now = GetTickCount();
    const DWORD every = now - g_levelAt < 5000 ? 250 : 2000;   // close-up right after the load
    if (now - g_levelAt > 120000 || now - g_vehLogLast < every) return;
    g_vehLogLast = now;
    void* inst = Rd<void*>(ById(id), 0x30);
    char squad[400] = {};
    for (int sid : {0, 6, 7, 8}) {
        void* so = ById(sid);
        void* si = Rd<void*>(so, 0x30);
        snprintf(squad + strlen(squad), sizeof squad - strlen(squad), " | %d: %.0f %.0f %.0f veh %p held %p", sid,
                 Rd<float>(si, 0x108), Rd<float>(si, 0x10C), Rd<float>(si, 0x110), Rd<void*>(so, 0x690),
                 Rd<void*>(so, 0x6CC));
    }
    LOGI("[dev] Loadout: t %u object %d at %.1f %.1f %.1f%s", now - g_levelAt, id, Rd<float>(inst, 0x108),
         Rd<float>(inst, 0x10C), Rd<float>(inst, 0x110), squad);
}
#endif

// ---- events ---------------------------------------------------------------------------------------------------------
int PSE_CALL OnLevelEnd(PSEEventId, void* data, void*) {
    const char* name = data ? static_cast<const char*>(data) : "";
    g_mission = _strnicmp(name, "mission", 7) == 0;
    g_playFrames = 0;
    g_levelAt = GetTickCount();
    g_doneThisLevel = false;
    g_quietSince = 0;
    g_vehSampleAt = 0;
    g_vehStill = false;
    g_closedAt = 0;
    g_fromSave = g_loadPending;
    g_loadPending = false;
    g_given.clear();
    g_looks.clear();
    if (!g_fromSave) {
        g_pendingKits.clear();
        g_pendingLooks.clear();
    }
    if (g_open) {   // a level load with the screen up (shouldn't happen): give everything back
        g_open = false;
        Unfreeze();
    }
    if (g_inputBackAt) {
        g_inputBackAt = 0;
        GiveInputBack();
    }
    if (g_mission && g_fromSave) LOGI("Loadout: %s comes from a saved game - no screen", name);
#ifndef DS_DIST
    g_fire.clear();
    {
        const std::string ids = DevString("LoadoutFire");
        for (size_t i = 0; i < ids.size();) {
            const size_t c = ids.find(',', i);
            const std::string one = ids.substr(i, c == std::string::npos ? std::string::npos : c - i);
            if (!one.empty()) g_fire.push_back({atoi(one.c_str()), nullptr, -1, -1});
            i = c == std::string::npos ? ids.size() : c + 1;
        }
    }
    g_controlId = static_cast<int>(DevDword("LoadoutControl", 0xFFFFFFFF));
    g_fireDone = g_fire.empty();
    g_fireStart = 0;
    g_pickupDone = false;
    g_fireLen = DevDword("LoadoutFireMs", 20000);
#endif
    return 0;
}

int PSE_CALL OnLevelBegin(PSEEventId, void*, void*) {   // kits handed out during the load aren't pick-ups
    g_playFrames = 0;
    g_mission = false;
    return 0;
}

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    if (g_mission && g_game->DispatchMode() == 1) ++g_playFrames;
#ifndef DS_DIST
    DevLoadSave();
    PickupTest();
    VehicleLog();
    FireTest();
    static DWORD dumpAt = 0;
    if (g_mission && g_levelAt != dumpAt && GetTickCount() - g_levelAt > 4000 && DevDword("LoadoutLog", 0)) {
        dumpAt = g_levelAt;
        DumpSquad("level start");
    }
    static DWORD dumpedClose = 0;
    if (g_closedAt && g_closedAt != dumpedClose && GetTickCount() - g_closedAt > 1000 && DevDword("LoadoutLog", 0)) {
        dumpedClose = g_closedAt;
        DumpSquad("after the screen");
    }
#endif
    if (g_inputBackAt) {   // the screen closed: input back once nothing it reads is held (at most 1 s)
        if (!RawInput() || GetTickCount() >= g_inputBackAt || g_game->DispatchMode() != 1) {
            g_inputBackAt = 0;
            GiveInputBack();
        } else {
            for (int i = 0; i < BlockCount(); ++i) {
                Wr<int>(Block(i), 0x3A4, -1);
                Wr<int>(Block(i), 0xC, 0);
            }
        }
    }
    if (g_open) {
        if (g_game->FrontEndState() != 0x13 || g_game->DispatchMode() != 1) {   // left the mission (menu, load ...)
            LOGI("Loadout: mission left with the screen open - closed without changes");
            g_open = false;
            Unfreeze();
            return 0;
        }
        HoldFrozen();
        HandleInput();
#ifndef DS_DIST
        static DWORD freezeLog = 0;
        if (GetTickCount() - freezeLog > 3000) {
            freezeLog = GetTickCount();
            void* t = Rd<void*>(0x680098);
            LOGI("[dev] Loadout: frozen - mult %g, timer dt %g s, total %u ms", Rd<float>(kTimeMult), Rd<float>(t, 0xAC),
                 Rd<uint32_t>(t, 0x10));
        }
#endif
        return 0;
    }
    if (g_mission && (!g_pendingKits.empty() || !g_pendingLooks.empty()) && g_game->FrontEndState() == 0x13 && g_game->DispatchMode() == 1) {
        if (!g_pendingAt) g_pendingAt = GetTickCount();
        if (GetTickCount() - g_pendingAt > 500) {
#ifndef DS_DIST
            if (DevDword("LoadoutNoKitRestore", 0)) {
                LOGI("[dev] Loadout: co-save weapons NOT put back (Dev\\LoadoutNoKitRestore)");
                g_pendingKits.clear();
                g_pendingLooks.clear();
            } else
#endif
            ApplyPendingKits();
        }
    }
    if (!g_mission || g_doneThisLevel || g_fromSave || !Enabled()) return 0;
    const DWORD now = GetTickCount();
    if (now - g_levelAt > 300000) {   // 5 min without a quiet moment: not this level
        g_doneThisLevel = true;
        return 0;
    }
    if (!PlainPlay() || !VehicleStill(Rd<void*>(Block(0), 0x2EC))) {
        g_quietSince = 0;
        return 0;
    }
    if (!g_quietSince) g_quietSince = now;
    if (now - g_quietSince < 1000) return 0;
    g_doneThisLevel = true;
    LOGI("Loadout: intro over %u ms after the level load", now - g_levelAt);
    Open();
    return 0;
}

int PSE_CALL OnEndScene(PSEEventId, void* data, void*) {
    if (g_open && data && !g_squad.empty()) Draw(static_cast<IDirect3DDevice9*>(data));
    return 0;
}

void PSE_CALL OnMessage(const PSEMessage* msg, void*) {
    switch (msg->type) {
    case PSE_MSG_SAVE_BEGIN:
#ifndef DS_DIST
        if (DevDword("LoadoutNoRecount", 0)) {   // dev A/B: the game's own (stale) counts
            LOGI("[dev] Loadout: save object counts NOT taken again (Dev\\LoadoutNoRecount)");
            break;
        }
#endif
        PatchProjectilesForSave();
        RecountSaveObjects();
        break;
    case PSE_MSG_SAVE_END: UnpatchProjectiles(); break;
    case PSE_MSG_LOAD_END: g_loadPending = true; break;
#ifndef DS_DIST
    case PSE_MSG_FRONT_END_READY: g_frontEndAt = GetTickCount(); break;
#endif
    case PSE_MSG_DEVICE_LOST:
    case PSE_MSG_EXIT: ReleaseGraphics(); break;
    case PSE_MSG_POST_LOAD:
        g_pads = static_cast<const Ds2FixPadsV1*>(g_msg->QueryInterface(g_self, DS2FIX_PADS_INTERFACE, 1));
        if (g_pads && g_pads->size < sizeof(Ds2FixPadsV1)) g_pads = nullptr;
        g_coop = static_cast<const Ds2FixCoopV1*>(g_msg->QueryInterface(g_self, DS2FIX_COOP_INTERFACE, 1));
        if (g_coop && g_coop->size < sizeof(Ds2FixCoopV1)) g_coop = nullptr;
        g_skinsI = static_cast<const Ds2FixSkinsV1*>(g_msg->QueryInterface(g_self, DS2FIX_SKINS_INTERFACE, 1));
        if (g_skinsI && g_skinsI->size < sizeof(Ds2FixSkinsV1)) g_skinsI = nullptr;
        LOGI("Loadout: pads through ds2fix.pads %s, co-op session %s, skins %s", g_pads ? "yes" : "no (XInput + keyboard only)",
             g_coop ? "known" : "unknown", g_skinsI ? "yes" : "no (UNIFORM / FACE change names only)");
        g_options = static_cast<const Ds2FixOptionsV1*>(g_msg->QueryInterface(g_self, DS2FIX_OPTIONS_INTERFACE, 1));
        if (g_options && g_options->size >= sizeof(Ds2FixOptionsV1) && g_options->AddToggle(g_self, &kToggle))
            LOGI("Loadout: OPTIONS row LOADOUT SCREEN added");
        else
            LOGI("Loadout: no OPTIONS row (ds2fix.options missing or full) - setting %s stays as it is", kKeyFeat);
        break;
    default: break;
    }
}
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"ds2", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "ds2fix.loadout", "DesertStorm2Fix - loadout screen",
                                      "DesertStorm2Fix", PSE_MAKE_VERSION(0, 1, 0), 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_msg = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    g_settings = static_cast<const PSESettingsInterface*>(pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1));
    g_game = static_cast<const PSEGameDs2V1*>(pse->Query(g_self, PSE_GAME_DS2_INTERFACE, 1));
    if (!g_log) return 0;
    if (!g_msg || !g_settings || !g_game || g_game->size < sizeof(PSEGameDs2V1)) {
        LOGE("ds2fix.loadout: the loader lacks pse.messaging / pse.settings / pse.game.ds2 v1");
        return 0;
    }
    if (!g_game->ExeKnown()) LOGI("Loadout: not the known CDS II.exe 1.0 build - addresses may not fit");
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = kKeyFeat;
    d.type = PSE_SETTING_BOOL;
    d.flags = PSE_SETTING_LIVE;
    d.defaultInt = 1;
    d.label = "LOADOUT SCREEN";
    d.help = "Choose the squad's weapons after each mission's intro";
    if (g_settings->Declare(g_self, &d) != PSE_OK) LOGE("[fail] Loadout: setting %s not declared", kKeyFeat);
    for (int mode = 0; mode < 2; ++mode) {   // the remembered choices / unlocks (hidden strings, see Unlocked())
        PSESettingDesc t = {};
        t.size = sizeof t;
        t.type = PSE_SETTING_STRING;
        t.flags = PSE_SETTING_HIDDEN;
        t.defaultString = "";
        t.key = kUnlockKey[mode];
        g_settings->Declare(g_self, &t);
        for (int role = 0; role < 4; ++role) {
            t.key = kRememberKey[mode][role];
            g_settings->Declare(g_self, &t);
        }
    }
    LoadXInput();
    g_ser = static_cast<const PSESerializationInterface*>(pse->Query(g_self, PSE_SERIALIZATION_INTERFACE, 1));
    if (!g_ser || g_ser->SetCallbacks(g_self, &SaveKits, &LoadKits, &RevertKits, nullptr) != PSE_OK)
        LOGE("[fail] Loadout: no co-save - swapped weapons won't come back with a loaded game");
    g_hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    if (!g_hooks || g_hooks->HookCallSite(g_self, kSetViewCall, kSetView, reinterpret_cast<void*>(&SetViewHook), 0,
                                          &g_setViewNext, nullptr) != PSE_OK)
        LOGE("[fail] Loadout: camera call 0x%08X not hooked - the screen keeps the game's camera", kSetViewCall);
    if (!g_hooks || g_hooks->HookFunction(g_self, kAddItem, sizeof kAddItemEntry, kAddItemEntry,
                                          reinterpret_cast<void*>(&AddItemHook), 0, &g_addItemNext, nullptr) != PSE_OK)
        LOGE("[fail] Loadout: AddItem 0x%08X not hooked - picked-up weapons don't unlock", kAddItem);
#ifndef DS_DIST
    {
        const std::string load = DevString("LoadoutLoadSave");
        g_devLoad.assign(load.begin(), load.end());
        if (!g_devLoad.empty() && g_hooks &&
            g_hooks->HookCallSite(g_self, 0x40E73C, 0x43FB50, reinterpret_cast<void*>(&FrontEndUpdate), 100,
                                  &g_feUpdateNext, nullptr) != PSE_OK)
            LOGE("[dev] Loadout: front-end update call not hooked");
    }
#endif
    g_msg->RegisterListener(g_self, nullptr, &OnMessage, nullptr);
    g_msg->Subscribe(g_self, PSE_EVENT_LEVEL_LOAD_BEGIN, &OnLevelBegin, nullptr, 0);
    g_msg->Subscribe(g_self, PSE_EVENT_LEVEL_LOAD_END, &OnLevelEnd, nullptr, 0);
    g_msg->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    g_msg->Subscribe(g_self, PSE_EVENT_END_SCENE, &OnEndScene, nullptr, 0);
    LOGI("[ok]   Loadout: screen %s (setting %s); save object counts kept up to date", Enabled() ? "on" : "off", kKeyFeat);
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    if (g_options) g_options->RemoveToggle(g_self, &kToggle);
    if (g_inputBackAt) {
        g_inputBackAt = 0;
        GiveInputBack();
    }
    if (g_open) {
        g_open = false;
        Unfreeze();
    }
    ReleaseGraphics();
}
}
