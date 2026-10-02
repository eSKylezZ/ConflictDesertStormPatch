// Squad customise screen (V2 loadout): after a mission's intro and briefing the world is frozen and each soldier's
// uniform, face and kit can be changed on the live soldiers (game addresses and calls below).
//
// Trigger: armed when a MissionN level loads. Opens the first frame the mission is in plain play (front-end state
// [0x617C18] = 0xE and no cinematic bit [0x63C948]+0x1B8C) after an intro cutscene or briefing (0x14) - or within the
// first frames when the mission has neither - so the level is never seen before the screen.
// Freeze: time multiplier [0x5F7E64] = 0.0001 (exactly 0 turns the scene into fog colour); input blocks off by their
// device mask +0x38 = 0 (joystick +0x3C8 = -1 crashed on the first arrow key - some path indexes pads with it).
// Screen: opaque backdrop with a window in the middle where the soldier stands; squad list left, options right, drawn
// at the end of the frame (overlay late draw: our shapes, then text with the game's font).
// Controls: Up/Down option row, Left/Right change it, Q/E or L1/R1 (L2/R2) previous/next soldier, Enter / Cross-A
// start the mission.
// Uniform / face: skin table 0x619848[uniform*4 + role] (0 SAS, 1 US Delta, 2 Russian, 3 Iraqi - the last two are the
// old network multiplayer's factions, CSkins_RU/IR_*.txt: 7 men per role, textures + portraits in chardata.dat), entries
// 0x5C bytes: +4 texture, +0x27 portrait, +0x50/+0x54 name records (char[] first); applied with FUN_00435be0(soldier,
// entry) + the texture manager's pending-load pass FUN_0054a3e0 (else the new textures stay blank).
// Factions and weapons: the kit rows list the soldier's faction's weapons first (features::WeaponFactions); changing the
// UNIFORM to another faction hands out that faction's standard kit for the role (the multiplayer kits, MPSkinItems.txt)
// and changing back to the faction the soldier started the screen in gives back his kit from then.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kLevelName = 0x606880;
constexpr uint32_t kGameState = 0x63C948;       // +0x1B8C bit 0 = cutscene
constexpr uint32_t kTimeMultiplier = 0x5F7E64;  // float
constexpr uint32_t kDispatchMode = 0x60EC8C;    // 0 = playing
constexpr uint32_t kFrontEndState = 0x617C18;   // 0xE in play, 0x14 briefing
constexpr uint32_t kBlocks = 0x60F5B8, kBlockSize = 0x478, kBlockDevices = 0x38, kBlockCount = 0x610798;
constexpr uint32_t kRenderer = 0x63C924;
constexpr uint32_t kGameWindow = 0x606A60;
constexpr uint32_t kJoystickCount = 0x754BE8;
constexpr uint32_t kCharacters = 0x63C9A0, kSquadTeam = 0x5F9B94;
constexpr uint32_t kSkinTable = 0x619848, kSkinCount = 0x6193A8;  // [uniform * 4 + role]
constexpr uint32_t kTextureManager = 0x63C930;
constexpr int kSkinEntrySize = 0x5C;
constexpr int kSides = 4;  // skin table sides: 0 SAS, 1 US Delta, 2 Russian, 3 Iraqi
const char* const kSideLabels[kSides] = {"SAS", "US DELTA", "RUSSIAN", "IRAQI"};

// Armed -> (intro / briefing) Waiting -> Open -> Done. A level that goes straight into play for more than 10 frames
// is a loaded save (fresh missions always start with an intro or briefing): no screen, no carried kits - the save has
// its own. Carry / CarryWaiting: the same for a linked mission, whose kits come from the previous level.
enum class Phase { Idle, Armed, Waiting, Open, Done, Carry, CarryWaiting };
Phase g_phase = Phase::Idle;
char g_level[32] = {};
int g_armedFrames = 0;
float g_savedMultiplier = 1.0f;
int g_savedDevices[4] = {};

// ---- squad ----
struct Member {
    uint8_t* soldier;
    int role;  // squad slot: 0 Bradley (rifleman), 1 Foley (sniper), 2 Connors (heavy), 3 Jones (engineer)
    int openSide = -1;    // faction (skin table side) when the screen opened
    std::string openKit;  // his kit then (CurrentChoice), given back when he returns to that faction
};
Member g_squad[4];
int g_squadCount = 0;
int g_sel = 0;  // selected soldier
int g_row = 0;  // selected option row

// Co-op: players take turns (player 1 first); each edits only the soldiers they control (soldier +0x2B00 = owning
// player, set by the game's squad-to-player mapping FUN_00436f70) with their own device. Single player: one turn.
int g_turn = 0;
int Players() { return features::CoopPlayers(); }   // 0 = single player (or a dev split without a session)
int OwnerOf(uint8_t* soldier) { return *reinterpret_cast<int*>(soldier + 0x2B00); }
bool Mine(int i) { return Players() == 0 || OwnerOf(g_squad[i].soldier) == g_turn; }
int FirstMine() {
    for (int i = 0; i < g_squadCount; ++i)
        if (Mine(i)) return i;
    return -1;
}

const char* const kRoleNames[4] = {"RIFLEMAN", "SNIPER", "HEAVY WEAPONS", "COMBAT ENGINEER"};

int RoleOf(uint8_t* soldier) {
    auto* unit = *reinterpret_cast<uint8_t**>(soldier + 0x24);
    return unit ? *reinterpret_cast<int8_t*>(unit + 0x1C7) : -1;
}

void NoteSquadLaunchers();

void FindSquad() {
    g_squadCount = 0;
    auto mgr = *reinterpret_cast<const uint8_t* const*>(kCharacters);
    if (!mgr) return;
    const uint8_t team = *reinterpret_cast<const uint8_t*>(kSquadTeam);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return;
    auto lists = *reinterpret_cast<const uintptr_t* const*>(mgr + 4);
    if (!lists) return;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s && g_squadCount < 4; s = *reinterpret_cast<uintptr_t*>(s + 0x14)) {
        auto* soldier = reinterpret_cast<uint8_t*>(s);
        const int role = RoleOf(soldier);
        if (role < 0 || role > 3 || !*reinterpret_cast<void**>(soldier + 0x2A68)) continue;
        g_squad[g_squadCount++] = {soldier, role, -1, {}};
    }
    std::sort(g_squad, g_squad + g_squadCount, [](const Member& a, const Member& b) { return a.role < b.role; });
    NoteSquadLaunchers();
}

// ---- preview camera ----
// The frame's view is set by FUN_004d66c0 (camera manager [0x63C9D8], camera id per view at +0xEC0C) through
// FUN_0054fdf0(view, pos, rot) at 0x4D672D: pos = eye (cm, y up), rot = angles in degrees (x pitch, y yaw, z roll; the view is
// translate(-pos) * rotY * rotX * rotZ). While the screen is open the eye orbits the selected soldier and the fog
// (graphics.cpp) turns everything a few metres behind him into the backdrop colour.
constexpr uint32_t kSetViewSite = 0x4D672D, kSetView = 0x54FDF0;
constexpr uint32_t kFogColour = 0xFF283038;
constexpr float kFogStart = 300.0f, kFogEnd = 480.0f;  // cm from the eye; the soldier stands 2.8 m away
constexpr float kOrbitDistance = 280.0f, kTargetHeight = 95.0f, kEyeHeight = 125.0f, kOrbitSpeed = 0.45f;  // rad/s
LONGLONG g_orbitStart = 0;

void __fastcall SetView(void* view, void* /*edx*/, float* pos, float* rot) {
    using Fn = void(__thiscall*)(void*, float*, float*);
    bool keepOrbit = false;
#ifndef DS_DIST
    {   // dev: Dev\KeepOrbit = 1 keeps the preview camera after the screen closes (to watch the soldier in play)
        DWORD v = 0, size = sizeof v;
        keepOrbit = g_squadCount && RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "KeepOrbit",
                                                 RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v;
    }
#endif
    if ((g_phase != Phase::Open && !keepOrbit) || g_sel >= g_squadCount) {
        reinterpret_cast<Fn>(kSetView)(view, pos, rot);
        return;
    }
    const float* at = reinterpret_cast<const float*>(g_squad[g_sel].soldier + 0x30);
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    if (!g_orbitStart) {
        g_orbitStart = now.QuadPart;
#ifndef DS_DIST
        dslog::Write("[dev] Customise camera: game eye %.0f %.0f %.0f rot %.3f %.3f %.3f, soldier %.0f %.0f %.0f", pos[0],
                     pos[1], pos[2], rot[0], rot[1], rot[2], at[0], at[1], at[2]);
#endif
    }
    const float t = static_cast<float>(now.QuadPart - g_orbitStart) / static_cast<float>(freq.QuadPart);
    const float a = t * kOrbitSpeed;
    float eye[3] = {at[0] + std::sin(a) * kOrbitDistance, at[1] + kEyeHeight, at[2] + std::cos(a) * kOrbitDistance};
    const float dx = at[0] - eye[0], dy = at[1] + kTargetHeight - eye[1], dz = at[2] - eye[2];
    constexpr float kDeg = 57.2957795f;  // the angles are in degrees (pitch > 0 looks down)
    float ang[3] = {-std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * kDeg, std::atan2(dx, dz) * kDeg, 0.0f};
    reinterpret_cast<Fn>(kSetView)(view, eye, ang);
    features::SceneFogBegin();
}

// Tutorial / objective bar (FUN_00486d20, drawn after the HUD passes from the render loop): hidden on the screen.
constexpr uint32_t kObjectiveBarSite = 0x40F2AF, kObjectiveBar = 0x486D20;
void __cdecl ObjectiveBar() {
    if (g_phase != Phase::Open) reinterpret_cast<void(__cdecl*)()>(kObjectiveBar)();
}

// ---- skins ----
uint8_t* SkinEntry(int uniform, int role, int index) {
    const int k = uniform * 4 + role;
    const int n = reinterpret_cast<int*>(kSkinCount)[k];
    auto* table = reinterpret_cast<uint8_t**>(kSkinTable)[k];
    if (!table || index < 0 || index >= n) return nullptr;
    return table + index * kSkinEntrySize;
}

// Faces offered: every row except the Training variants (texture name ending in a digit + 'T', e.g. Hero01_US_01T);
// mods append their rows after those (mods.cpp), so they are skipped wherever they are.
bool Selectable(int uniform, int role, int index) {
    const uint8_t* entry = SkinEntry(uniform, role, index);
    if (!entry) return false;
    const char* tex = reinterpret_cast<const char*>(entry + 4);
    const size_t len = strlen(tex);
    return !(len >= 2 && (tex[len - 1] == 'T' || tex[len - 1] == 't') && tex[len - 2] >= '0' && tex[len - 2] <= '9');
}

int SkinCount(int uniform, int role) { return reinterpret_cast<int*>(kSkinCount)[uniform * 4 + role]; }

// Uniforms: SAS, US Delta, Russian and Iraqi, plus the mods' uniform variants (.skin "uniform name", features::SkinVariants) - rows
// of the same tables, told apart by texture. A variant row is a uniform choice for its soldier, not another soldier.
std::string OptionOf(int uniform, int role, int index) {
    const uint8_t* entry = SkinEntry(uniform, role, index);
    if (!entry) return "";
    for (const features::SkinVariant& v : features::SkinVariants())
        if (v.uniform == uniform && v.role == role && _stricmp(v.texture.c_str(), reinterpret_cast<const char*>(entry + 4)) == 0)
            return v.name;
    return "";
}

bool InOption(int uniform, int role, int index, const std::string& option) {
    return Selectable(uniform, role, index) && OptionOf(uniform, role, index) == option;
}

struct UniformOption {
    int uniform;
    std::string name;  // "" = plain SAS / US Delta
};

std::vector<UniformOption> UniformOptions(int role) {
    std::vector<UniformOption> out;
    for (int u = 0; u < kSides; ++u) {
        std::vector<std::string> names;
        for (int i = 0, n = SkinCount(u, role); i < n; ++i)
            if (Selectable(u, role, i)) {
                const std::string o = OptionOf(u, role, i);
                if (std::find(names.begin(), names.end(), o) == names.end()) names.push_back(o);
            }
        std::stable_partition(names.begin(), names.end(), [](const std::string& s) { return s.empty(); });
        for (const std::string& s : names) out.push_back({u, s});
    }
    return out;
}

std::string UniformLabel(int uniform, int role, int index) {
    const std::string o = OptionOf(uniform, role, index);
    return !o.empty() ? o : uniform >= 0 && uniform < kSides ? kSideLabels[uniform] : "?";
}

// Next row of the same uniform from `index` in direction dir (dir 0: `index` itself if it fits, else the next one).
int StepSkin(int uniform, int role, int index, int dir, const std::string& option = "") {
    const int n = SkinCount(uniform, role);
    if (n <= 0) return -1;
    int i = ((index % n) + n) % n;
    if (dir == 0 && InOption(uniform, role, i, option)) return i;
    const int step = dir < 0 ? -1 : 1;
    for (int k = 0; k < n; ++k) {
        i = (i + step + n) % n;
        if (InOption(uniform, role, i, option)) return i;
    }
    return -1;
}

// The soldier's current skin as (uniform, index); false if not in the skin tables.
bool CurrentSkin(uint8_t* soldier, int role, int& uniform, int& index) {
    auto* entry = *reinterpret_cast<uint8_t**>(soldier + 0x2A68);
    for (int u = 0; u < kSides; ++u)
        for (int i = 0, n = reinterpret_cast<int*>(kSkinCount)[u * 4 + role]; i < n; ++i)
            if (SkinEntry(u, role, i) == entry) {
                uniform = u, index = i;
                return true;
            }
    return false;
}

const char* NamePart(uint8_t* entry, int offset) {
    auto* rec = entry ? *reinterpret_cast<const char**>(entry + offset) : nullptr;
    return rec ? rec : "";
}

void ApplySkin(uint8_t* soldier, uint8_t* entry) {
    using Apply = void(__thiscall*)(void*, void*);
    using LoadPending = void(__thiscall*)(void*, void*, int);  // (progress callback, its argument) - ret 8
    reinterpret_cast<Apply>(0x435BE0)(soldier, entry);
    reinterpret_cast<LoadPending>(0x54A3E0)(*reinterpret_cast<void**>(kTextureManager), nullptr, 0);
}

// ---- kit ----
enum class Slot { Main, Sidearm, Launcher, Grenades };

// Weapons by class (definition +0xC0: 0 AK, 1 SMG, 2 sniper, 3 pistol, 4 MG, 5 rockets, 7 frag, 10 shotgun, 11 smoke,
// 12 M16A2 - see rumble.cpp).
bool InSlot(uint8_t* def, Slot slot) {
    const int cls = *reinterpret_cast<int*>(def + 0xC0);
    return slot == Slot::Main       ? (cls == 0 || cls == 1 || cls == 2 || cls == 4 || cls == 10 || cls == 12)
           : slot == Slot::Sidearm  ? cls == 3
           : slot == Slot::Launcher ? cls == 5
                                    : (cls == 7 || cls == 11);
}

// The soldier's weapons in a slot: (item, definition).
std::vector<std::pair<uint8_t*, uint8_t*>> KitItems(uint8_t* soldier, Slot slot) {
    std::vector<std::pair<uint8_t*, uint8_t*>> out;
    auto* inv = *reinterpret_cast<uint8_t***>(soldier + 0x26B8);
    if (!inv) return out;
    for (int i = 0; i < 25; ++i) {
        uint8_t* it = inv[i];
        if (!it || *reinterpret_cast<int*>(it + 4) != 6) continue;
        auto* def = *reinterpret_cast<uint8_t**>(it + 0x24);
        // (and a kit weapon of that slot by its Weaps.txt row: the airstrike designator is class 5 like the LAWs)
        if (def && *reinterpret_cast<int*>(def) == 5 && *reinterpret_cast<const char**>(def + 4) && InSlot(def, slot) &&
            features::KitSlotOf(*reinterpret_cast<const char**>(def + 4)) == static_cast<int>(slot))
            out.push_back({it, def});
    }
    return out;
}

// Mission objectives that need a launcher (tanks, SCUDs): the squad never loses its last one on this screen or by a
// carried kit if the mission handed one out. g_squadLaunchers = the squad had one when FindSquad ran (before any
// choice was applied).
bool g_squadLaunchers = false;

bool LauncherElsewhere(uint8_t* soldier) {
    for (int i = 0; i < g_squadCount; ++i)
        if (g_squad[i].soldier != soldier && !KitItems(g_squad[i].soldier, Slot::Launcher).empty()) return true;
    return false;
}

void NoteSquadLaunchers() { g_squadLaunchers = g_squadCount && LauncherElsewhere(nullptr); }

bool MayDropLauncher(uint8_t* soldier) { return !g_squadLaunchers || LauncherElsewhere(soldier); }

// The launcher slot: all launcher items go and the new type comes with as many rockets. The game keeps a stack of
// launchers as one item per launcher with every rocket on the first (Connors' three LAWs in Missions 4, 6, 8: 3, 0,
// 0 - a launcher holds 1, so 3 can't be set on one item), so the new stack is built the same way: one give per
// rocket, each merged onto the first by AddItem. "" = none.
bool SwapLauncher(uint8_t* soldier, const std::string& want) {
    auto items = KitItems(soldier, Slot::Launcher);
    if (want.empty() && !items.empty() && !MayDropLauncher(soldier)) return false;
    if (items.empty()) return want.empty() || features::KitSwap(soldier, nullptr, want, 2);
    int rockets = 0;
    for (auto& [item, def] : items) rockets += features::KitRounds(item);
    rockets = std::clamp(rockets, 1, 9);
    if (!features::KitSwap(soldier, items[0].first, want, 2, want.empty() ? 0 : 1)) return false;
    for (size_t k = 1; k < items.size(); ++k) features::KitSwap(soldier, items[k].first, "", 2);
    if (!want.empty())
        for (int k = 1; k < rockets; ++k) features::KitSwap(soldier, nullptr, want, 2, 1);
    return true;
}

// A weapon's name as the game shows it (catalog text by its id, a mod's display name), else the id.
const char* WeaponLabel(uint8_t* def) {
    const uint32_t hash = *reinterpret_cast<const uint32_t*>(def + 0x80);
    const char* text = features::ModText(hash);
    if (!text) text = features::GameText(hash);
    if (text && *text) return text;
    const char* name = *reinterpret_cast<const char**>(def + 4);
    return strncmp(name, "US_WPN_", 7) ? name : name + 7;
}

void KitText(uint8_t* soldier, Slot slot, char* out, size_t n) {
    out[0] = 0;
    for (auto& [item, def] : KitItems(soldier, slot)) {
        if (out[0]) strncat_s(out, n, ", ", _TRUNCATE);
        strncat_s(out, n, WeaponLabel(def), _TRUNCATE);
    }
    if (!out[0]) strcpy_s(out, n, "-");
}

// The preview shows each soldier with their main weapon in hand: held item set at once (FUN_0042ec10(soldier, item,
// 1) - the 1 skips the switching animation) and the world run at normal speed for a moment (g_settleUntil) so the
// upper-body pose follows (frozen time left a soldier who started the mission with his knife out still holding it).
DWORD g_settleUntil = 0;

void Settle() { g_settleUntil = GetTickCount() + 3000; }

void HoldMain(uint8_t* soldier);

// The soldier's faction: the side of the skin table his skin is in, -1 if none.
int SideOf(uint8_t* soldier) {
    int uniform = -1, index = 0;
    const int role = RoleOf(soldier);
    return role >= 0 && role < 4 && CurrentSkin(soldier, role, uniform, index) ? uniform : -1;
}

// Left / Right on a kit row: the next weapon this level can hand out for the slot (launcher: or none) - the soldier's
// own faction's weapons first, then the others (each group in the usual order).
void ChangeKit(uint8_t* soldier, Slot slot, int dir) {
    const int s = static_cast<int>(slot);
    std::vector<std::string> choices = features::KitChoices(s);
    if (const int side = SideOf(soldier); side >= 0)
        std::stable_partition(choices.begin(), choices.end(),
                              [&](const std::string& w) { return (features::WeaponFactions(w.c_str()) >> side) & 1; });
    if (slot == Slot::Launcher && (KitItems(soldier, slot).empty() || MayDropLauncher(soldier))) choices.insert(choices.begin(), "");
    if (choices.size() < 2 && !(choices.size() == 1 && KitItems(soldier, slot).empty())) return;
    auto items = KitItems(soldier, slot);
    uint8_t* cur = items.empty() ? nullptr : items[0].first;
    const std::string curName = items.empty() ? "" : *reinterpret_cast<const char**>(items[0].second + 4);
    const int n = static_cast<int>(choices.size());
    int idx = -1;
    for (int i = 0; i < n; ++i)
        if (_stricmp(choices[i].c_str(), curName.c_str()) == 0) idx = i;
    const int next = idx < 0 ? (dir > 0 ? 0 : n - 1) : (idx + dir + n) % n;
    if (next == idx) return;
    if (slot == Slot::Launcher ? !SwapLauncher(soldier, choices[next])
                               : !features::KitSwap(soldier, cur, choices[next], s))
        return;   // (no settle: running time let the game switch a freshly made gun back to the knife)
    dslog::Write("Customise: kit %d %s -> %s", s, curName.empty() ? "(none)" : curName.c_str(),
                 choices[next].empty() ? "(none)" : choices[next].c_str());
}

// ---- remembered choices (registry Enhancements\Loadout<SP|Coop><role> = "uniform;face;main;sidearm;launcher") ----
constexpr char kRegKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";

std::string ChoiceValue(int role) {
    char name[32];
    snprintf(name, sizeof name, "Loadout%s%d", *reinterpret_cast<int*>(0x606410) ? "Coop" : "SP", role);
    return name;
}

void HoldMain(uint8_t* soldier) {
    auto items = KitItems(soldier, Slot::Main);
    if (items.empty()) return;
    using Hold = void(__thiscall*)(void*, void*, int);
    // Once when the screen opens: the game's own animated switch (argument 3 = 0, as the inventory does), so a soldier
    // left in the intro's knife stance (the main weapon already counts as held) really draws it - the held item is
    // cleared first (instant, the game falls back to the knife) so the switch runs. Weapons changed on the screen then
    // go straight into the hands (KitSwap) - an animated switch to a freshly made weapon left it slung on the back.
    if (*reinterpret_cast<uint8_t**>(soldier + 0x26B0) == items[0].first) reinterpret_cast<Hold>(0x42EC10)(soldier, nullptr, 1);
    reinterpret_cast<Hold>(0x42EC10)(soldier, items[0].first, 0);
}

// Grenades: every grenade item the soldier has, as "id+id" (sorted), "-" for none.
std::string GrenadeSet(uint8_t* soldier) {
    std::vector<std::string> ids;
    for (auto& [item, def] : KitItems(soldier, Slot::Grenades)) ids.push_back(*reinterpret_cast<const char**>(def + 4));
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    std::string out;
    for (const std::string& id : ids) out += (out.empty() ? "" : "+") + id;
    return out.empty() ? "-" : out;
}

// Replaces all grenades with a set: 2 of each type in a mixed set, 4 of a single type.
void SetGrenades(uint8_t* soldier, const std::string& set) {
    for (auto& [item, def] : KitItems(soldier, Slot::Grenades)) features::KitSwap(soldier, item, "", 3);
    if (set == "-") return;
    std::vector<std::string> ids;
    for (size_t a = 0; a < set.size();) {
        size_t b = set.find('+', a);
        if (b == std::string::npos) b = set.size();
        ids.push_back(set.substr(a, b - a));
        a = b + 1;
    }
    for (const std::string& id : ids)
        for (int k = 0; k < (ids.size() == 1 ? 2 : 1); ++k) features::KitSwap(soldier, nullptr, id, 3);  // 2 per give
}

// The grenade sets on offer: every unlocked type together, then each type alone.
std::vector<std::string> GrenadeChoices() {
    std::vector<std::string> types = features::KitChoices(3);
    std::sort(types.begin(), types.end());
    std::vector<std::string> out;
    if (types.size() > 1) {
        std::string all;
        for (const std::string& t : types) all += (all.empty() ? "" : "+") + t;
        out.push_back(all);
    }
    for (const std::string& t : types) out.push_back(t);
    return out;
}

void ChangeGrenades(uint8_t* soldier, int dir) {
    const std::vector<std::string> choices = GrenadeChoices();
    if (choices.empty()) return;
    const std::string cur = GrenadeSet(soldier);
    const int n = static_cast<int>(choices.size());
    int idx = -1;
    for (int i = 0; i < n; ++i)
        if (_stricmp(choices[i].c_str(), cur.c_str()) == 0) idx = i;
    const int next = idx < 0 ? (dir > 0 ? 0 : n - 1) : (idx + dir + n) % n;
    if (next == idx) return;
    SetGrenades(soldier, choices[next]);
    dslog::Write("Customise: grenades %s -> %s", cur.c_str(), choices[next].c_str());
}

// A soldier's kit as "uniform;face;main;sidearm;launcher;grenades" (- = none).
std::string CurrentChoice(const Member& m) {
    int uniform = 0, index = 0;
    if (!CurrentSkin(m.soldier, m.role, uniform, index)) uniform = index = -1;
    std::string v = std::to_string(uniform) + ";" + std::to_string(index);
    for (Slot slot : {Slot::Main, Slot::Sidearm, Slot::Launcher}) {
        auto items = KitItems(m.soldier, slot);
        v += ";";
        v += items.empty() ? "-" : *reinterpret_cast<const char**>(items[0].second + 4);
    }
    return v + ";" + GrenadeSet(m.soldier);
}

// Applies a kit string where it still fits (weapons found in this mode and loadable in this level).
void ApplyChoice(const Member& m, const std::string& value) {
    std::vector<std::string> part;
    for (size_t a = 0; a <= value.size();) {
        size_t b = value.find(';', a);
        if (b == std::string::npos) b = value.size();
        part.push_back(value.substr(a, b - a));
        a = b + 1;
    }
    if (part.size() < 5) return;
    const int uniform = atoi(part[0].c_str()), index = atoi(part[1].c_str());
    if (uniform >= 0 && uniform < kSides && index >= 0 && index < SkinCount(uniform, m.role) && Selectable(uniform, m.role, index))
        if (uint8_t* entry = SkinEntry(uniform, m.role, index))
            if (entry != *reinterpret_cast<uint8_t**>(m.soldier + 0x2A68)) ApplySkin(m.soldier, entry);
    const Slot slots[3] = {Slot::Main, Slot::Sidearm, Slot::Launcher};
    for (int k = 0; k < 3; ++k) {
        const std::string& want = part[2 + k];
        auto items = KitItems(m.soldier, slots[k]);
        const std::string cur = items.empty() ? "-" : *reinterpret_cast<const char**>(items[0].second + 4);
        if (_stricmp(cur.c_str(), want.c_str()) == 0) continue;
        if (want == "-") {
            if (slots[k] == Slot::Launcher) SwapLauncher(m.soldier, "");   // (kept if it is the squad's last one)
            continue;
        }
        const std::vector<std::string> choices = features::KitChoices(k);
        if (std::find_if(choices.begin(), choices.end(), [&](const std::string& c) { return _stricmp(c.c_str(), want.c_str()) == 0; }) == choices.end())
            continue;   // not found in this mode yet / not loadable here
        if (slots[k] == Slot::Launcher)
            SwapLauncher(m.soldier, want);
        else
            features::KitSwap(m.soldier, items.empty() ? nullptr : items[0].first, want, k);
    }
    if (part.size() > 5 && _stricmp(part[5].c_str(), GrenadeSet(m.soldier).c_str()) != 0) {
        const std::vector<std::string> g = GrenadeChoices();
        if (part[5] == "-" || std::find_if(g.begin(), g.end(), [&](const std::string& c) { return _stricmp(c.c_str(), part[5].c_str()) == 0; }) != g.end())
            SetGrenades(m.soldier, part[5]);
    }
}

std::vector<std::string> SplitChoice(const std::string& value) {
    std::vector<std::string> part;
    for (size_t a = 0; a <= value.size();) {
        size_t b = value.find(';', a);
        if (b == std::string::npos) b = value.size();
        part.push_back(value.substr(a, b - a));
        a = b + 1;
    }
    return part;
}

// A faction's standard issue for a role: the old network multiplayer's kits (MPSkinItems.txt rows "role, item, type,
// side, magazines"; sides SAS / US / RUSSIAN / IRAQI) - the first main weapon and the first sidearm listed.
void FactionKit(int side, int role, std::string& main, std::string& sidearm) {
    static const char* const kTableSides[kSides] = {"SAS", "US", "RUSSIAN", "IRAQI"};
    std::string text;
    if (side < 0 || side >= kSides || role < 0 || role > 3 || !features::ReadGameTable("MPSkinItems.txt", text)) return;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::vector<std::string> col;
        for (size_t a = pos; a <= nl;) {
            size_t e = text.find(',', a);
            if (e == std::string::npos || e > nl) e = nl;
            std::string c = text.substr(a, e - a);
            while (!c.empty() && (c.back() == '\r' || c.back() == ' ')) c.pop_back();
            while (!c.empty() && c.front() == ' ') c.erase(c.begin());
            col.push_back(c);
            a = e + 1;
        }
        pos = nl + 1;
        if (col.size() < 4 || _stricmp(col[0].c_str(), kRoleNames[role]) != 0 || _stricmp(col[3].c_str(), kTableSides[side]) != 0)
            continue;
        const int slot = features::KitSlotOf(col[1].c_str());
        if (slot == 0 && main.empty()) main = col[1];
        if (slot == 1 && sidearm.empty()) sidearm = col[1];
    }
}

// After a UNIFORM change to another faction: back to the screen's opening kit in his own faction, else the new
// faction's standard issue (main weapon + sidearm, where this level can hand them out).
void FollowFaction(Member& m, int from, int to) {
    if (from == to || to < 0) return;
    std::vector<std::string> cur = SplitChoice(CurrentChoice(m));
    if (to == m.openSide && !m.openKit.empty()) {
        std::vector<std::string> kit = SplitChoice(m.openKit);
        for (size_t k = 2; k < kit.size() && k < cur.size(); ++k) cur[k] = kit[k];
    } else {
        std::string main, sidearm;
        FactionKit(to, m.role, main, sidearm);
        if (cur.size() > 3) {
            if (!main.empty()) cur[2] = main;
            if (!sidearm.empty()) cur[3] = sidearm;
        }
    }
    std::string value;
    for (size_t k = 0; k < cur.size(); ++k) value += (k ? ";" : "") + cur[k];
    ApplyChoice(m, value);  // (KitSwap puts a new main weapon straight into the hands)
    dslog::Write("Customise: %s now %s - kit %s", kRoleNames[m.role], kSideLabels[to], value.c_str());
}

void SaveChoices() {
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    for (int i = 0; i < g_squadCount; ++i) {
        const std::string v = CurrentChoice(g_squad[i]);
        RegSetValueExA(key, ChoiceValue(g_squad[i].role).c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>(v.size() + 1));
    }
    RegCloseKey(key);
}

void ApplyChoices() {
    for (int i = 0; i < g_squadCount; ++i) {
        char buf[512] = {};
        DWORD size = sizeof buf;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, kRegKey, ChoiceValue(g_squad[i].role).c_str(), RRF_RT_REG_SZ, nullptr, buf,
                         &size) != ERROR_SUCCESS)
            continue;
        ApplyChoice(g_squad[i], buf);
        dslog::Write("Customise: %s - last choices applied", kRoleNames[g_squad[i].role]);
    }
}

// ---- linked missions: the squad's kit carries on between levels that continue the same operation (Mission4 ->
// Mission5A, Mission11B -> Mission12A -> Mission12B). While a linked level is played the kits are
// snapshotted (every 2 s, registry Carry<role> + CarryFrom, per mode); the next level of the chain applies them instead
// of its own starting kits and skips the customise screen.
const char* const kChains[][4] = {{"Mission4", "Mission5A", nullptr}, {"Mission11B", "Mission12A", "Mission12B", nullptr}};

std::string LevelBase(const char* level) {
    std::string b = level;
    const size_t dot = b.find('.');
    return dot == std::string::npos ? b : b.substr(0, dot);
}

bool ChainNext(const std::string& from, const std::string& to) {
    for (auto& chain : kChains)
        for (int i = 0; chain[i] && chain[i + 1]; ++i)
            if (_stricmp(chain[i], from.c_str()) == 0 && _stricmp(chain[i + 1], to.c_str()) == 0) return true;
    return false;
}

bool ChainHasNext(const std::string& level) {
    for (auto& chain : kChains)
        for (int i = 0; chain[i] && chain[i + 1]; ++i)
            if (_stricmp(chain[i], level.c_str()) == 0) return true;
    return false;
}

std::string CarryValue(const char* what) {
    return std::string("Carry") + (*reinterpret_cast<int*>(0x606410) ? "Coop" : "SP") + what;
}

void SnapshotCarry(const std::string& level) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    for (int i = 0; i < g_squadCount; ++i) {
        const std::string v = CurrentChoice(g_squad[i]);
        RegSetValueExA(key, CarryValue(std::to_string(g_squad[i].role).c_str()).c_str(), 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(v.c_str()), static_cast<DWORD>(v.size() + 1));
    }
    RegSetValueExA(key, CarryValue("From").c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(level.c_str()),
                   static_cast<DWORD>(level.size() + 1));
    RegCloseKey(key);
}

std::string CarryFrom() {
    char buf[64] = {};
    DWORD size = sizeof buf;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kRegKey, CarryValue("From").c_str(), RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS)
        return "";
    return buf;
}

void ApplyCarry() {
    for (int i = 0; i < g_squadCount; ++i) {
        char buf[512] = {};
        DWORD size = sizeof buf;
        if (RegGetValueA(HKEY_LOCAL_MACHINE, kRegKey, CarryValue(std::to_string(g_squad[i].role).c_str()).c_str(),
                         RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            ApplyChoice(g_squad[i], buf);
    }
}

// Split screen: one full-screen view while the screen is up (renderer viewport count +0x20, viewports +0x24 x 0x18,
// full-screen viewport +0x40650) - the other views would show the level unfogged.
uint8_t g_savedViews[4 + 0x18] = {};
bool g_singleView = false;

void SingleView(bool on) {
    auto* r = *reinterpret_cast<uint8_t**>(kRenderer);
    if (!r || on == g_singleView) return;
    if (on) {
        if (*reinterpret_cast<uint32_t*>(r + 0x20) < 2) return;
        memcpy(g_savedViews, r + 0x20, sizeof g_savedViews);
        *reinterpret_cast<uint32_t*>(r + 0x20) = 1;
        memcpy(r + 0x24, r + 0x40650, 0x18);
    } else {
        memcpy(r + 0x20, g_savedViews, sizeof g_savedViews);
    }
    g_singleView = on;
}

// ---- open / close ----
bool Cutscene() {
    const auto* state = *reinterpret_cast<uint8_t* const*>(kGameState);
    return state && (state[0x1B8C] & 1);
}

int BlockCount() {
    const int n = *reinterpret_cast<int*>(kBlockCount);
    return n < 0 ? 0 : n > 4 ? 4 : n;
}

void LateDraw();
bool g_primed = false;  // input: ignore whatever is held when the screen opens

void Open() {
    FindSquad();
    if (!g_squadCount) {
        g_phase = Phase::Done;
        dslog::Write("Customise: no squad soldiers in %s - skipped", g_level);
        return;
    }
    g_phase = Phase::Open;
    g_sel = g_row = 0;
    g_turn = 0;
    if (Players()) {   // player 1 may control none of the soldiers listed first
        for (int p = 0; p < Players(); ++p) {
            g_turn = p;
            if (FirstMine() >= 0) break;
        }
        g_sel = std::max(0, FirstMine());
    }
    for (int i = 0; i < g_squadCount; ++i)   // the mission's own kits count as found
        for (Slot slot : {Slot::Main, Slot::Sidearm, Slot::Launcher, Slot::Grenades})
            for (auto& [item, def] : KitItems(g_squad[i].soldier, slot))
                features::KitUnlock(*reinterpret_cast<const char**>(def + 4));
    ApplyChoices();
    for (int i = 0; i < g_squadCount; ++i) {
        HoldMain(g_squad[i].soldier);
        g_squad[i].openSide = SideOf(g_squad[i].soldier);
        g_squad[i].openKit = CurrentChoice(g_squad[i]);
    }
    Settle();
    g_primed = false;
    auto* mult = reinterpret_cast<float*>(kTimeMultiplier);
    g_savedMultiplier = *mult;
    *mult = 0.0001f;
    for (int i = 0; i < BlockCount(); ++i) {
        int* dev = reinterpret_cast<int*>(kBlocks + i * kBlockSize + kBlockDevices);
        g_savedDevices[i] = *dev;
        *dev = 0;
    }
    overlay::SetLateDraw(&LateDraw);
    g_orbitStart = 0;
    SingleView(true);
    features::SetSceneFog(true, kFogColour, kFogStart, kFogEnd);
    dslog::Write("Customise: opened in %s, %d soldiers", g_level, g_squadCount);
}

void Close() {
    SaveChoices();
    SingleView(false);
    *reinterpret_cast<float*>(kTimeMultiplier) = g_savedMultiplier;
    for (int i = 0; i < BlockCount(); ++i)
        *reinterpret_cast<int*>(kBlocks + i * kBlockSize + kBlockDevices) = g_savedDevices[i];
    overlay::SetLateDraw(nullptr);
    features::SetSceneFog(false, 0, 0, 0);
    g_phase = Phase::Done;
    dslog::Write("Customise: closed");
}

// ---- input ----
enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kAccept = 16, kPrev = 32, kNext = 64 };
uint32_t g_prevPad[8] = {}, g_prevKeys = 0;

uint32_t ReadInput() {
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), 8);
    int onlyPad = Players() ? features::CoopJoystick(g_turn) : -2;   // -1 = keyboard player, -2 = anyone
    if (onlyPad >= 8) onlyPad = -2;  // no game joystick (dev CoopFakeJoin test players): anyone
    uint32_t in = 0;
    for (int j = 0; j < count; ++j) {
        const uint32_t now = (onlyPad == -2 || onlyPad == j) ? features::ReadJoystick(j) : 0;
        const uint32_t edge = g_primed ? now & ~g_prevPad[j] : 0;
        g_prevPad[j] = now;
        if (edge & (1u << 12 | 1u << 16)) in |= kUp;
        if (edge & (1u << 14 | 1u << 18)) in |= kDown;
        if (edge & (1u << 15 | 1u << 19)) in |= kLeft;
        if (edge & (1u << 13 | 1u << 17)) in |= kRight;
        if (edge & (1u << 2)) in |= kAccept;
        if (edge & (1u << 4 | 1u << 6)) in |= kPrev;  // L2 / L1
        if (edge & (1u << 5 | 1u << 7)) in |= kNext;  // R2 / R1
    }
    static const int map[][2] = {{VK_UP, kUp},       {VK_DOWN, kDown}, {VK_LEFT, kLeft},  {VK_RIGHT, kRight},
                                 {VK_RETURN, kAccept}, {'Q', kPrev},    {'E', kNext},      {VK_PRIOR, kPrev},
                                 {VK_NEXT, kNext}};
    uint32_t keys = 0;
    if (onlyPad < 0)
        for (auto& k : map)
            if (features::KeyHeld(k[0])) keys |= k[1];
    if (g_primed) in |= keys & ~g_prevKeys;
    g_prevKeys = keys;
    g_primed = true;
    return in;
}

// ---- rows ----
enum Row { kRowUniform, kRowFace, kRowMain, kRowSidearm, kRowLauncher, kRowGrenades, kRowCount };
const char* const kRowNames[kRowCount] = {"UNIFORM", "SOLDIER", "MAIN WEAPON", "SIDEARM", "LAUNCHER", "GRENADES"};

void Change(int dir) {
    Member& m = g_squad[g_sel];
    if (g_row == kRowGrenades) {
        ChangeGrenades(m.soldier, dir);
        return;
    }
    if (g_row == kRowMain || g_row == kRowSidearm || g_row == kRowLauncher) {
        ChangeKit(m.soldier, g_row == kRowMain ? Slot::Main : g_row == kRowSidearm ? Slot::Sidearm : Slot::Launcher, dir);
        return;
    }
    int uniform = 0, index = 0;
    if (!CurrentSkin(m.soldier, m.role, uniform, index)) return;
    const int fromSide = uniform;
    if (g_row == kRowUniform) {
        const std::vector<UniformOption> opts = UniformOptions(m.role);
        const std::string cur = OptionOf(uniform, m.role, index);
        int at = 0;
        for (int k = 0; k < static_cast<int>(opts.size()); ++k)
            if (opts[k].uniform == uniform && opts[k].name == cur) at = k;
        if (opts.size() < 2) return;
        const UniformOption to = opts[(at + (dir < 0 ? -1 : 1) + opts.size()) % opts.size()];
        // the same soldier if he has that uniform (a variant of his, or SAS <-> Delta for the hero), else the row at
        // the same place (plain uniforms) or the uniform's first soldier
        uint8_t* was = SkinEntry(uniform, m.role, index);
        int pick = -1;
        for (int i = 0, n = SkinCount(to.uniform, m.role); i < n && pick < 0; ++i)
            if (InOption(to.uniform, m.role, i, to.name) &&
                strcmp(NamePart(SkinEntry(to.uniform, m.role, i), 0x50), NamePart(was, 0x50)) == 0 &&
                strcmp(NamePart(SkinEntry(to.uniform, m.role, i), 0x54), NamePart(was, 0x54)) == 0)
                pick = i;
        if (pick < 0)
            pick = StepSkin(to.uniform, m.role, to.name.empty() ? std::min(index, SkinCount(to.uniform, m.role) - 1) : 0, 0,
                            to.name);
        uniform = to.uniform;
        index = pick;
    } else if (g_row == kRowFace) {
        index = StepSkin(uniform, m.role, index, dir, OptionOf(uniform, m.role, index));
    } else {
        return;  // grenades: shown only
    }
    if (index < 0) return;
    if (uint8_t* entry = SkinEntry(uniform, m.role, index)) {
        ApplySkin(m.soldier, entry);
        dslog::Write("Customise: %s -> %s %s (%s)", kRoleNames[m.role], NamePart(entry, 0x50), NamePart(entry, 0x54),
                     reinterpret_cast<char*>(entry + 4));
        FollowFaction(m, fromSide, uniform);
    }
}

// Accept: the next player who controls a soldier gets the screen; after the last one the mission starts.
void NextTurn() {
    for (int p = g_turn + 1; p < Players(); ++p) {
        g_turn = p;
        const int first = FirstMine();
        if (first >= 0) {
            g_sel = first;
            g_row = 0;
            g_primed = false;   // the new player's held buttons don't count
            dslog::Write("Customise: player %d's turn", p + 1);
            return;
        }
    }
    Close();
}

void Update() {
    const HWND wnd = *reinterpret_cast<HWND*>(kGameWindow);
    if (!wnd || !features::GameFocused()) return;
    const uint32_t in = ReadInput();
    for (int step : {in & kPrev ? -1 : 0, in & kNext ? 1 : 0}) {
        if (!step) continue;
        for (int k = 1; k <= g_squadCount; ++k) {   // the next of this player's soldiers
            const int i = ((g_sel + step * k) % g_squadCount + g_squadCount) % g_squadCount;
            if (Mine(i)) {
                g_sel = i;
                break;
            }
        }
    }
    if (in & kUp) g_row = (g_row + kRowCount - 1) % kRowCount;
    if (in & kDown) g_row = (g_row + 1) % kRowCount;
    if (in & kLeft) Change(-1);
    if (in & kRight) Change(1);
    if (in & kAccept) NextTurn();
}

// ---- drawing ----
float g_W = 0, g_H = 0;

void DrawShapes() {
    const auto* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    if (!r) return;
    const float W = g_W = static_cast<float>(*reinterpret_cast<const int*>(r + 0x40688));
    const float H = g_H = static_cast<float>(*reinterpret_cast<const int*>(r + 0x4068C));
    constexpr uint32_t kBack = 0xFF12161A, kPanel = 0xFF1C2228, kGold = 0xFFC8A040, kSel = 0xFF3A4652;
    auto band = [](float x, float y, float w, float h, uint32_t c) { overlay::QueueScreenRect(x, y, w, h, c); };
    // backdrop with a window on the soldier (x 0.37..0.63, y 0.14..0.86)
    const float wx0 = W * 0.37f, wx1 = W * 0.63f, wy0 = H * 0.14f, wy1 = H * 0.86f;
    band(0, 0, W, wy0, kBack);
    band(0, wy1, W, H - wy1, kBack);
    band(0, wy0, wx0, wy1 - wy0, kBack);
    band(wx1, wy0, W - wx1, wy1 - wy0, kBack);
    const float t = std::max(2.0f, H / 360.0f);  // window frame
    band(wx0 - t, wy0 - t, wx1 - wx0 + 2 * t, t, kGold);
    band(wx0 - t, wy1, wx1 - wx0 + 2 * t, t, kGold);
    band(wx0 - t, wy0, t, wy1 - wy0, kGold);
    band(wx1, wy0, t, wy1 - wy0, kGold);
    // panels + selection bars
    band(W * 0.04f, H * 0.14f, W * 0.30f, H * 0.72f, kPanel);
    band(W * 0.66f, H * 0.14f, W * 0.30f, H * 0.72f, kPanel);
    const float rowH = H * 0.09f, optH = H * 0.075f;
    band(W * 0.04f, H * 0.20f + g_sel * rowH, W * 0.30f, rowH, kSel);
    band(W * 0.66f, H * 0.20f + g_row * optH, W * 0.30f, optH, kSel);
}

// Text shortened with "..." to fit maxW pixels (the value, not the arrows around it, gets cut).
std::string Fit(void* font, const std::string& text, float maxW) {
    if (!font || maxW <= 0 || overlay::TextWidth(font, text.c_str()) <= maxW) return text;
    std::string cut = text;
    while (cut.size() > 1) {
        cut.pop_back();
        if (overlay::TextWidth(font, (cut + "...").c_str()) <= maxW) return cut + "...";
    }
    return cut;
}

void Text(const char* s, float x, float y, uint32_t argb, bool large = false, bool centre = false, float maxW = 0) {
    void* font = large ? *reinterpret_cast<void**>(0x60EDB8) : overlay::MenuFont();
    if (!font) return;
    const std::string fitted = Fit(font, s, maxW);
    s = fitted.c_str();
    int xi = static_cast<int>(x);
    if (centre) xi -= overlay::TextWidth(font, s) / 2;
    overlay::DrawLabel(font, s, xi, static_cast<int>(y), argb);
}

bool Playing();

// Prompt line with button icons (icon glyphs inside the game's text): the keys, or the pad of whoever edits now -
// in co-op the player whose turn it is (their own icon style), else the device in use.
std::string PromptLine(bool last) {
    using overlay::Icon;
    int style = Players() ? features::CoopPromptStyle(g_turn) : -1;  // 0 keyboard, 1 PlayStation, 2 Xbox
    if (style < 0) style = features::KeyboardInUse() ? 0 : features::PadStyleXbox() ? 2 : 1;
    const char* action = last ? "START MISSION" : "NEXT PLAYER";
    if (!overlay::GlyphsReady()) {
        return std::string(style ? "D-PAD  CHOOSE / CHANGE     L1/R1  SOLDIER     CROSS  " : "UP/DOWN  CHOOSE     LEFT/RIGHT  CHANGE     Q/E  SOLDIER     ENTER  ") + action;
    }
    auto icon = [](Icon i) { return std::string(overlay::IconChar(i)); };
    auto pad = [&](int button) {  // game button numbering (0 Triangle .. 7 R1)
        return static_cast<Icon>(static_cast<int>(style == 2 ? Icon::XbTriangle : Icon::PsTriangle) + button);
    };
    if (!style)
        return icon(Icon::KbArrowUp) + icon(Icon::KbArrowDown) + " CHOOSE     " + icon(Icon::KbArrowLeft) +
               icon(Icon::KbArrowRight) + " CHANGE     " +
               icon(Icon::KbQ) + icon(Icon::KbE) + " SOLDIER     " + icon(Icon::KbEnter) + " " + action;
    const Icon vertical = style == 2 ? Icon::XbDpadVertical : Icon::PsDpadVertical;
    const Icon horizontal = style == 2 ? Icon::XbDpadHorizontal : Icon::PsDpadHorizontal;
    return icon(vertical) + " CHOOSE     " + icon(horizontal) + " CHANGE     " + icon(pad(6)) + icon(pad(7)) +
           " SOLDIER     " + icon(pad(2)) + " " + action;
}

// Whether Left / Right on a row has anything else to switch to for this soldier.
bool HasOptions(const Member& m, int row) {
    int u = 0, idx = 0;
    const bool known = CurrentSkin(m.soldier, m.role, u, idx);
    if (row == kRowUniform) return known && UniformOptions(m.role).size() > 1;
    if (row == kRowFace) return known && StepSkin(u, m.role, idx, 1, OptionOf(u, m.role, idx)) != idx;
    if (row == kRowGrenades) {
        const std::string cur = GrenadeSet(m.soldier);
        for (const std::string& c : GrenadeChoices())
            if (_stricmp(c.c_str(), cur.c_str()) != 0) return true;
        return false;
    }
    const Slot slot = row == kRowMain ? Slot::Main : row == kRowSidearm ? Slot::Sidearm : Slot::Launcher;
    auto items = KitItems(m.soldier, slot);
    const std::string cur = items.empty() ? "" : *reinterpret_cast<const char**>(items[0].second + 4);
    std::vector<std::string> choices = features::KitChoices(static_cast<int>(slot));
    if (slot == Slot::Launcher && (items.empty() || MayDropLauncher(m.soldier))) choices.push_back("");
    for (const std::string& c : choices)
        if (_stricmp(c.c_str(), cur.c_str()) != 0) return true;
    return false;
}

void LateDraw() {
    if (g_phase == Phase::Armed || g_phase == Phase::Waiting) {
        // The frame the briefing / intro ends is rendered before OnFrameCustomise sees it: cover it at render time.
        const auto* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
        if (r && Playing()) {
            const float rects[1][4] = {{0, 0, static_cast<float>(*reinterpret_cast<const int*>(r + 0x40688)),
                                        static_cast<float>(*reinterpret_cast<const int*>(r + 0x4068C))}};
            overlay::FillRects(rects, 1, 0xFF12161A);
        }
        return;
    }
    if (g_phase != Phase::Open) return;
    const float W = g_W, H = g_H;
    if (W <= 0) return;
    constexpr uint32_t kWhite = 0xFFFFFFFF, kDim = 0xFFA0A8B0, kGoldText = 0xFFE8C060;
    char title[64];
    if (Players()) snprintf(title, sizeof title, "SQUAD LOADOUT - PLAYER %d", g_turn + 1);
    else strcpy_s(title, "SQUAD LOADOUT");
    Text(title, W * 0.5f, H * 0.09f, kGoldText, true, true);
    Text("SQUAD", W * 0.06f, H * 0.185f, kGoldText, true);
    Text("EQUIPMENT", W * 0.68f, H * 0.185f, kGoldText, true);
    const float rowH = H * 0.09f;
    for (int i = 0; i < g_squadCount; ++i) {
        const Member& m = g_squad[i];
        const float y = H * 0.20f + i * rowH;
        auto* entry = *reinterpret_cast<uint8_t**>(m.soldier + 0x2A68);
        int u = 0, idx = 0;
        char name[80], sub[80];
        snprintf(name, sizeof name, "%s %s", NamePart(entry, 0x50), NamePart(entry, 0x54));
        snprintf(sub, sizeof sub, "%s - %s", kRoleNames[m.role],
                 CurrentSkin(m.soldier, m.role, u, idx) ? UniformLabel(u, m.role, idx).c_str() : "?");
        if (Players()) {   // whose soldier: "P1" ... ; other players' soldiers dimmed during this turn
            char tag[8];
            snprintf(tag, sizeof tag, "P%d", OwnerOf(m.soldier) + 1);
            Text(tag, W * 0.315f, y + rowH * 0.45f, Mine(i) ? kGoldText : 0xFF606870, true);
        }
        Text(name, W * 0.06f, y + rowH * 0.45f, i == g_sel ? kWhite : Mine(i) ? kDim : 0xFF606870, true);
        Text(sub, W * 0.06f, y + rowH * 0.80f, Mine(i) ? kDim : 0xFF606870);
    }
    const Member& m = g_squad[g_sel];
    const float optH = H * 0.075f;
    for (int r = 0; r < kRowCount; ++r) {
        const float y = H * 0.20f + r * optH;
        Text(kRowNames[r], W * 0.68f, y + optH * 0.40f, kDim);
        char value[160] = {};
        int u = 0, idx = 0;
        const bool known = CurrentSkin(m.soldier, m.role, u, idx);
        auto* entry = *reinterpret_cast<uint8_t**>(m.soldier + 0x2A68);
        switch (r) {
        case kRowUniform: snprintf(value, sizeof value, "%s", known ? UniformLabel(u, m.role, idx).c_str() : "?"); break;
        case kRowFace: snprintf(value, sizeof value, "%s %s", NamePart(entry, 0x50), NamePart(entry, 0x54)); break;
        case kRowMain: KitText(m.soldier, Slot::Main, value, sizeof value); break;
        case kRowSidearm: KitText(m.soldier, Slot::Sidearm, value, sizeof value); break;
        case kRowLauncher: KitText(m.soldier, Slot::Launcher, value, sizeof value); break;
        case kRowGrenades: KitText(m.soldier, Slot::Grenades, value, sizeof value); break;
        }
        const float maxW = W * 0.27f;
        std::string shown = value;
        if (HasOptions(m, r)) {   // "< value >" only when Left / Right would change something
            shown = "< " + Fit(overlay::MenuFont(), value, maxW - overlay::TextWidth(overlay::MenuFont(), "<  >")) + " >";
        }
        Text(shown.c_str(), W * 0.68f, y + optH * 0.85f, r == g_row ? kWhite : kDim, false, false, maxW);
    }
    const bool last = !Players() || g_turn >= Players() - 1;
    Text(PromptLine(last).c_str(), W * 0.5f, H * 0.93f, kDim, false, true);
}

bool Playing() {
    return *reinterpret_cast<uint32_t*>(kFrontEndState) == 0xE && !Cutscene() &&
           *reinterpret_cast<int8_t*>(kDispatchMode) == 0;
}
}  // namespace

bool features::CustomiseOpen() { return g_phase == Phase::Open; }

void features::OnFrameCustomise() {
    char level[32] = {};
    memcpy(level, reinterpret_cast<const char*>(kLevelName), sizeof level - 1);
    if (strcmp(level, g_level) != 0) {  // new level
        if (g_phase == Phase::Open) Close();
        memcpy(g_level, level, sizeof g_level);
        g_phase = _strnicmp(level, "Mission", 7) == 0 ? Phase::Armed : Phase::Idle;
        if (g_phase == Phase::Armed && ChainNext(CarryFrom(), LevelBase(level))) g_phase = Phase::Carry;
        g_armedFrames = 0;
        overlay::SetLateDraw(g_phase == Phase::Armed ? &LateDraw : nullptr);
    }
    if (*reinterpret_cast<int8_t*>(kDispatchMode) != 0) return;  // loading / other
    const bool playing = Playing();
    switch (g_phase) {
    case Phase::Armed:  // intro / briefing first
    case Phase::Carry:
        if (!playing) {
            g_phase = g_phase == Phase::Carry ? Phase::CarryWaiting : Phase::Waiting;
        } else if (++g_armedFrames > 10) {
            dslog::Write("Customise: %s went straight into play (a loaded save) - no loadout screen / carried kits", g_level);
            g_phase = Phase::Done;
            overlay::SetLateDraw(nullptr);
        }
        break;
    case Phase::Waiting:
        if (playing) Open();
        break;
    case Phase::Open:
        {
            const bool settling = GetTickCount() < g_settleUntil;
            *reinterpret_cast<float*>(kTimeMultiplier) = settling ? g_savedMultiplier : 0.0001f;
        }
        Update();
        if (g_phase == Phase::Open) DrawShapes();
        break;
    case Phase::CarryWaiting:   // the previous linked mission's kits, once the intro / briefing is over
        if (playing) {
            FindSquad();
            ApplyCarry();
            g_phase = Phase::Done;
            dslog::Write("Customise: %s continues from %s - kits carried over", g_level, CarryFrom().c_str());
        }
        break;
    default:
        break;
    }
    // In a linked mission (one with a next level in its chain): keep a snapshot of the kits for the next level.
    static DWORD lastSnap = 0;
    if (playing && g_phase != Phase::Open && ChainHasNext(LevelBase(g_level)) && GetTickCount() - lastSnap > 2000) {
        lastSnap = GetTickCount();
        FindSquad();
        if (g_squadCount) SnapshotCarry(LevelBase(g_level));
    }
}

void features::ApplyCustomise() {
    if (!patch::HookCall(kSetViewSite, reinterpret_cast<const void*>(&SetView), kSetView))
        dslog::Write("[fail] Customise: camera hook");
    if (!patch::HookCall(kObjectiveBarSite, reinterpret_cast<const void*>(&ObjectiveBar), kObjectiveBar))
        dslog::Write("[fail] Customise: objective bar hook");
}
