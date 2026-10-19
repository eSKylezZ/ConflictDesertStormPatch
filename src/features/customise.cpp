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
// Controls: Up/Down option row, Left/Right change it, Q/E or L1/R1 previous/next soldier, Tab or L2/R2 the tabs
// (BRIEFING, MISSION, CHARACTER, LOADOUT, OVERVIEW), Enter / Cross-A start the mission.
// Uniform / face: skin table 0x619848[uniform*4 + role] (0 SAS, 1 US Delta, 2 Russian, 3 Iraqi - the last two are the
// old network multiplayer's factions, CSkins_RU/IR_*.txt: 7 men per role, textures + portraits in chardata.dat), entries
// 0x5C bytes: +4 texture, +0x27 portrait, +0x50/+0x54 name records (char[] first); applied with FUN_00435be0(soldier,
// entry) + the texture manager's pending-load pass FUN_0054a3e0 (else the new textures stay blank).
// Factions and weapons: the kit rows list the soldier's faction's weapons first (features::WeaponFactions); changing the
// UNIFORM to another faction hands out that faction's standard kit for the role (the multiplayer kits, MPSkinItems.txt)
// and changing back to the faction the soldier started the screen in gives back his kit from then.
// Gear (gear.cpp): HEADGEAR / BACKPACK / FACE rows pick the model on the hat, back and face connectors; a faction
// change first puts on that faction's standard gear (hat, pack, pouches - CONNOBJS.TXT). Remembered with the kit.
// Optional (settings featLoadout), decided when a level starts (so a change in the pause menu counts from the next
// level): off = nothing of this file runs - the game's briefing popup stays and sends its own message 0x3B, no
// screen, no carried kits; a Lua mission gets its game.kit kits without the screen (as game.loadout(false)).
// The VOICE row needs featVoices, the TIME OF DAY / TIME FLOW rows featTimeOfDay.
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"

namespace {
constexpr uint32_t kLevelName = 0x606880;
constexpr uint32_t kGameState = 0x63C948;       // +0x1B8C bit 0 = cutscene
constexpr uint32_t kTimeMultiplier = 0x5F7E64;  // float
constexpr uint32_t kDispatchMode = 0x60EC8C;    // 0 = playing
constexpr uint32_t kFrontEndState = 0x617C18;   // 0xE in play, 0x14 briefing
constexpr uint32_t kBriefingState = 0x14;
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
int g_mrow = 0;  // MISSION tab row: objectives, then TIME OF DAY, TIME FLOW
int g_textPage = 0, g_textPages = 1;  // page of the text shown (BRIEFING tab, MISSION tab descriptions)
// A level that continues the previous one (Mission4 -> 5A, 11B -> 12A -> 12B): the briefing opens with the carried kit,
// view-only, and the time continues (timeofday.cpp).
bool g_briefingClosed = false, g_briefingMessageSent = false;   // the game's briefing popup closed by us (per level)
bool g_linkedLevel = false;   // this level continues g_linkedFrom
bool g_scriptLevel = false;   // a Lua mission (scripthost.cpp)
bool g_loadoutOn = true;      // settings featLoadout, taken when the level started
std::string g_linkedFrom;     // "Mission4"
// CHARACTER (uniform, face, voice, gear) and LOADOUT (weapons) are the soldier tabs: the same screen, their own rows.
// VEHICLE (vehicles.cpp): missions where the squad has a vehicle - the vehicle in the window, its choice on the right.
enum Tab { kTabBriefing, kTabMission, kTabSquad, kTabLoadout, kTabVehicle, kTabOverview, kTabCount };
const char* const kTabNames[kTabCount] = {"BRIEFING", "MISSION", "CHARACTER", "LOADOUT", "VEHICLE", "OVERVIEW"};
int g_tab = kTabBriefing;
int g_vsel = 0;   // VEHICLE tab: which of the squad's vehicle kinds
bool MapTab() { return g_tab == kTabBriefing || g_tab == kTabMission || g_tab == kTabOverview; }
bool TabAvailable(int t) {
    if (t == kTabOverview) return features::OverviewAvailable();
    if (t == kTabVehicle) return features::VehicleSlots() > 0;
    return true;
}

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
constexpr float kOrbitSpeed = 0.45f;  // rad/s (auto spin)
float g_realDt = 0.0f;                // real seconds of the last frame (the world runs at 0.0001 while the screen is up)

void CameraView(float* eye, float* ang);   // the screen's camera (below, after the rows it follows)
void ResetCamera();

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
    if ((g_phase != Phase::Open && !keepOrbit) || g_sel >= g_squadCount || features::OverviewPlaying()) {
        reinterpret_cast<Fn>(kSetView)(view, pos, rot);
        return;
    }
    float eye[3], ang[3];
    CameraView(eye, ang);
    reinterpret_cast<Fn>(kSetView)(view, eye, ang);
    features::SceneFogBegin();
}

// Tutorial / objective bar (FUN_00486d20, drawn after the HUD passes from the render loop): hidden on the screen.
constexpr uint32_t kObjectiveBarSite = 0x40F2AF, kObjectiveBar = 0x486D20;
void __cdecl ObjectiveBar() {
    if (!features::CustomiseHidesHud()) reinterpret_cast<void(__cdecl*)()>(kObjectiveBar)();
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

// Look ahead (setting featLoadoutLookAhead): a player who moved the view in the moment before the screen opened left
// their soldier looking at the ground / sky - soldier +0x328 = look pitch in degrees (+0x334 follows it at the clamp,
// -50 = up), the upper body and head follow it. Levelled for every player's soldier while the screen is up, with
// the block's AIM_PITCH values (+0x5C, +0x64) and its mouse look state (+0x44C pitch accumulator, which 0x44B466 keeps
// copying into +0x5C even with the devices off; +0x450 last dy, +0x454 / +0x458 flags), so nothing turns it back.
// RESET VIEW (C / L3) does the same, setting or not, and runs the settle again so a half-blended stance finishes too.
int BlockCount();
bool g_resetView = false;   // a RESET VIEW settle is running: level the look whatever the setting
void LookAhead() {
    if (!settings::Get().featLoadoutLookAhead && !g_resetView) return;
    for (int i = 0; i < BlockCount(); ++i) {
        auto* block = reinterpret_cast<uint8_t*>(kBlocks + i * kBlockSize);
        *reinterpret_cast<float*>(block + 0x5C) = 0.0f;
        *reinterpret_cast<float*>(block + 0x64) = 0.0f;
        for (uint32_t off = 0x44C; off <= 0x458; off += 4) *reinterpret_cast<uint32_t*>(block + off) = 0;
        auto* soldier = *reinterpret_cast<uint8_t**>(block + 0x310);
        if (!soldier) continue;
        *reinterpret_cast<float*>(soldier + 0x328) = 0.0f;
        *reinterpret_cast<float*>(soldier + 0x334) = 0.0f;
    }
}

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

// ---- remembered choices (settings key, settings::Key(): Loadout<SP|Coop><role> =
// "uniform;face;main;sidearm;launcher;grenades;headgear;backpack;face gear;7 pouch spots") ----
void SetChoiceString(const std::string& name, const std::string& value) { settings::WriteString(name.c_str(), value); }
bool GetChoiceString(const std::string& name, char* buf, DWORD size) {
    std::string v;
    if (!size || !settings::ReadString(name.c_str(), v)) return false;
    snprintf(buf, size, "%s", v.c_str());
    return true;
}

std::string ChoiceValue(int role) {
    char name[32];
    snprintf(name, sizeof name, "Loadout%s%d", *reinterpret_cast<int*>(0x606410) ? "Coop" : "SP", role);
    return name;
}

void HoldMain(uint8_t* soldier) {
    auto items = KitItems(soldier, Slot::Main);
    if (items.empty()) items = KitItems(soldier, Slot::Sidearm);   // a mission kit without a main weapon
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

// Left / Right on a gear row: the next model for that connector (none first); one that won't load is skipped.
void ChangeGear(uint8_t* soldier, int slot, int dir) {
    const std::vector<std::string> choices = features::GearChoices(slot);
    const int n = static_cast<int>(choices.size());
    if (n < 2) return;
    const std::string cur = features::GearOn(soldier, slot);
    int idx = -1;
    for (int i = 0; i < n; ++i)
        if (_stricmp(choices[i].c_str(), cur.c_str()) == 0) idx = i;
    int next = idx;
    for (int k = 0;; ++k) {
        next = next < 0 ? (dir > 0 ? 0 : n - 1) : (next + dir + n) % n;
        if (next == idx || k >= n) return;
        if (features::SetGear(soldier, slot, choices[next])) break;
    }
    dslog::Write("Customise: gear %d %s -> %s", slot, cur.empty() ? "(none)" : cur.c_str(),
                 choices[next].empty() ? "(none)" : choices[next].c_str());
}

// Left / Right on POUCHES: the next pouch set (a mix of his own counts as before the first).
void ChangePouches(uint8_t* soldier, int dir) {
    const int n = features::PouchSetCount();
    if (n < 2) return;
    const int cur = features::PouchSetOn(soldier);
    int next = cur;
    for (int k = 0;; ++k) {
        next = next < 0 ? (dir > 0 ? 0 : n - 1) : (next + dir + n) % n;
        if (next == cur || k >= n) return;
        if (features::SetPouchSet(soldier, next)) break;
    }
    dslog::Write("Customise: pouches %s -> %s", features::PouchSetLabel(cur).c_str(), features::PouchSetLabel(next).c_str());
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

// Gear rows' value in a kit string: model name, "-" = none, "" = unknown model (left alone when applied).
std::string GearField(uint8_t* soldier, int slot) {
    const std::string g = features::GearOn(soldier, slot);
    return g.empty() ? "-" : g == "?" ? "" : g;
}

// A soldier's kit as "uniform;face;main;sidearm;launcher;grenades;headgear;backpack;face gear;belt;belt left;belt right;
// hip left;hip right;chest left;chest right" (- = none).
std::string CurrentChoice(const Member& m) {
    int uniform = 0, index = 0;
    if (!CurrentSkin(m.soldier, m.role, uniform, index)) uniform = index = -1;
    std::string v = std::to_string(uniform) + ";" + std::to_string(index);
    for (Slot slot : {Slot::Main, Slot::Sidearm, Slot::Launcher}) {
        auto items = KitItems(m.soldier, slot);
        v += ";";
        v += items.empty() ? "-" : *reinterpret_cast<const char**>(items[0].second + 4);
    }
    v += ";" + GrenadeSet(m.soldier);
    for (int g = 0; g < features::kGearSlotCount; ++g) v += ";" + GearField(m.soldier, g);
    return v;
}

// The kit string's gear fields (6.., gear.cpp's slots in order), where the model can be loaded in this level. Older
// strings have fewer.
void ApplyGearFields(uint8_t* soldier, const std::vector<std::string>& part) {
    for (int g = 0; g < features::kGearSlotCount && 6 + g < static_cast<int>(part.size()); ++g) {
        const std::string& want = part[6 + g];
        if (want.empty() || _stricmp(want.c_str(), GearField(soldier, g).c_str()) == 0) continue;
        if (want == "-") {
            features::SetGear(soldier, g, "");
            continue;
        }
        const std::vector<std::string> choices = features::GearChoices(g);
        if (std::find_if(choices.begin(), choices.end(), [&](const std::string& c) { return _stricmp(c.c_str(), want.c_str()) == 0; }) != choices.end())
            features::SetGear(soldier, g, want);
    }
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
            if (entry != *reinterpret_cast<uint8_t**>(m.soldier + 0x2A68)) {
                const int side = SideOf(m.soldier);
                ApplySkin(m.soldier, entry);
                if (side != uniform) features::FactionGear(m.soldier, m.role, uniform);   // pouches etc. of the new faction
            }
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
    ApplyGearFields(m.soldier, part);
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
    features::FactionGear(m.soldier, m.role, to);   // the new faction's hat, pack and pouches
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
    for (int i = 0; i < g_squadCount; ++i) SetChoiceString(ChoiceValue(g_squad[i].role), CurrentChoice(g_squad[i]));
}

void ApplyChoices() {
    for (int i = 0; i < g_squadCount; ++i) {
        char buf[512] = {};
        if (!GetChoiceString(ChoiceValue(g_squad[i].role), buf, sizeof buf)) continue;
        ApplyChoice(g_squad[i], buf);
        dslog::Write("Customise: %s - last choices applied", kRoleNames[g_squad[i].role]);
    }
}

// ---- Lua missions (scripthost.cpp): a mission script's own loadout rules ----
// game.kit(id, {...}) forces a soldier's uniform (skin texture) and kit (weapon ids, "-" = none) and locks those rows;
// game.briefing(text) is the BRIEFING tab's text; game.loadout(false) skips the screen (the kits still apply). Rules
// belong to the running .lua level (features::ScriptLevelActive) and are cleared when the next one starts.
struct ScriptKit {
    std::string uniform;   // skin texture, "" = keep
    std::string slot[4];   // main, sidearm, launcher, grenades: weapon id ("a+b" grenade set), "-" none, "" keep
    bool lock = true;
};
std::map<int, ScriptKit> g_scriptKits;   // by script id (object +8)
bool g_scriptLoadout = true;
std::string g_scriptBriefing;

const ScriptKit* KitRule(uint8_t* soldier) {
    if (!features::ScriptLevelActive() || !soldier) return nullptr;
    auto it = g_scriptKits.find(*reinterpret_cast<int*>(soldier + 8));
    return it == g_scriptKits.end() ? nullptr : &it->second;
}

const char* ItemId(const std::pair<uint8_t*, uint8_t*>& item) { return *reinterpret_cast<const char**>(item.second + 4); }

void ApplyScriptKit(uint8_t* soldier, const ScriptKit& k) {
    const int role = RoleOf(soldier);
    if (!k.uniform.empty() && role >= 0 && role < 4) {
        [&] {
            for (int u = 0; u < kSides; ++u)
                for (int i = 0, n = SkinCount(u, role); i < n; ++i)
                    if (uint8_t* e = SkinEntry(u, role, i); e && _stricmp(reinterpret_cast<const char*>(e + 4), k.uniform.c_str()) == 0) {
                        if (e != *reinterpret_cast<uint8_t**>(soldier + 0x2A68)) {
                            const int side = SideOf(soldier);
                            ApplySkin(soldier, e);
                            if (side != u) features::FactionGear(soldier, role, u);
                        }
                        return;
                    }
            dslog::Write("[fail] Customise: mission uniform %s not in the skin tables", k.uniform.c_str());
        }();
    }
    const Slot slots[3] = {Slot::Main, Slot::Sidearm, Slot::Launcher};
    for (int s = 0; s < 3; ++s) {
        const std::string& want = k.slot[s];
        if (want.empty()) continue;
        auto items = KitItems(soldier, slots[s]);
        size_t keep = 0;
        if (want != "-") {
            if (items.empty() || _stricmp(ItemId(items[0]), want.c_str()) != 0) {
                if (!features::KitSwap(soldier, items.empty() ? nullptr : items[0].first, want, s)) continue;
                items = KitItems(soldier, slots[s]);
            }
            keep = 1;   // one of that slot; the rest go
        }
        for (size_t j = keep; j < items.size(); ++j) features::KitSwap(soldier, items[j].first, "", s);
    }
    if (!k.slot[3].empty()) SetGrenades(soldier, k.slot[3]);
    if (KitItems(soldier, Slot::Main).empty()) {   // no main weapon: the sidearm in his hands, not the knife
        auto side = KitItems(soldier, Slot::Sidearm);
        if (!side.empty()) reinterpret_cast<void(__thiscall*)(void*, void*, int)>(0x42EC10)(soldier, side[0].first, 1);
    }
    dslog::Write("Customise: mission kit for %d applied (%s / %s / %s / %s / %s)", *reinterpret_cast<int*>(soldier + 8),
                 k.uniform.empty() ? "-" : k.uniform.c_str(), k.slot[0].c_str(), k.slot[1].c_str(), k.slot[2].c_str(),
                 k.slot[3].c_str());
}

void ApplyScriptKits() {
    if (!features::ScriptLevelActive()) return;
    auto* mgr = *reinterpret_cast<uint8_t**>(0x63C9A0);
    auto* table = mgr ? *reinterpret_cast<uint8_t***>(mgr + 0x30) : nullptr;
    if (!table) return;
    for (const auto& [id, rule] : g_scriptKits)
        if (uint8_t* soldier = table[id]) ApplyScriptKit(soldier, rule);
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
    for (int i = 0; i < g_squadCount; ++i)
        SetChoiceString(CarryValue(std::to_string(g_squad[i].role).c_str()), CurrentChoice(g_squad[i]));
    SetChoiceString(CarryValue("From"), level);
}

std::string CarryFrom() {
    char buf[64] = {};
    if (!GetChoiceString(CarryValue("From"), buf, sizeof buf)) return "";
    return buf;
}

void ApplyCarry() {
    for (int i = 0; i < g_squadCount; ++i) {
        char buf[512] = {};
        if (GetChoiceString(CarryValue(std::to_string(g_squad[i].role).c_str()), buf, sizeof buf))
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

void Open(bool linked = false) {
#ifndef DS_DIST
    {   // dev A/B: Dev\CustomiseOff = 1 never opens the screen (vehicle swaps still happen at load)
        DWORD v = 0, size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "CustomiseOff", RRF_RT_REG_DWORD, nullptr, &v,
                         &size) == ERROR_SUCCESS && v) {
            g_phase = Phase::Done;
            dslog::Write("[dev] Customise: screen off (Dev\\CustomiseOff)");
            return;
        }
    }
#endif
    FindSquad();
    if (!g_squadCount) {
        g_phase = Phase::Done;
        dslog::Write("Customise: no squad soldiers in %s - skipped", g_level);
        return;
    }
    g_phase = Phase::Open;
    g_sel = g_row = g_mrow = 0;
    g_tab = kTabBriefing;
    g_vsel = 0;
    if (features::VehicleReloadOpened() && features::VehicleSlots()) g_tab = kTabVehicle;   // back after a vehicle change
    {   // soldiers seated in a vehicle stand beside it while the screen is up (a seated one isn't drawn)
        uint8_t* soldiers[4];
        for (int i = 0; i < g_squadCount; ++i) soldiers[i] = g_squad[i].soldier;
        features::StageSquadOutside(soldiers, g_squadCount);
    }
    g_textPage = 0;
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
    if (features::ScriptLevelActive()) ApplyScriptKits();   // a Lua mission's own kits, not the remembered choices
    else if (!linked) ApplyChoices();   // a linked level has the carried kit (ApplyCarry) instead
    for (int i = 0; i < g_squadCount; ++i) {
        HoldMain(g_squad[i].soldier);
        g_squad[i].openSide = SideOf(g_squad[i].soldier);
        g_squad[i].openKit = CurrentChoice(g_squad[i]);
    }
    Settle();
    LookAhead();
    g_primed = false;
    auto* mult = reinterpret_cast<float*>(kTimeMultiplier);
    g_savedMultiplier = *mult;
    *mult = 0.0001f;
    for (int i = 0; i < BlockCount(); ++i) {
        int* dev = reinterpret_cast<int*>(kBlocks + i * kBlockSize + kBlockDevices);
        g_savedDevices[i] = *dev;
        *dev = 0;
    }
    features::RuleTimeOfDay();   // the mission's start-up set-up (after the overview) may have set its own hour
    overlay::SetLateDraw(&LateDraw);
    ResetCamera();
    SingleView(true);
    features::SetSceneFog(true, kFogColour, kFogStart, kFogEnd);
    dslog::Write("Customise: opened in %s, %d soldiers", g_level, g_squadCount);
}

// forLevelLoad: a level load was requested while the screen is up (console / script level change) - put the game's
// state back before the load runs (closing after it restored the old level's split views into the new one, which then
// crashed creating objects at 0x4CCF82) and don't restart the mission for a changed vehicle.
void Close(bool forLevelLoad = false) {
    if (!g_linkedLevel && !features::ScriptLevelActive()) SaveChoices();   // (carried / mission kits aren't choices)
    SingleView(false);
    *reinterpret_cast<float*>(kTimeMultiplier) = g_savedMultiplier;
    for (int i = 0; i < BlockCount(); ++i)
        *reinterpret_cast<int*>(kBlocks + i * kBlockSize + kBlockDevices) = g_savedDevices[i];
    overlay::SetLateDraw(nullptr);
    features::SetSceneFog(false, 0, 0, 0);
    features::VehicleShowEnd();
    features::ReturnSquadInside();
    g_phase = Phase::Done;
    dslog::Write(forLevelLoad ? "Customise: closed (level load)" : "Customise: closed");
    if (!g_linkedLevel && !forLevelLoad) features::VehiclesCommit();   // a changed vehicle restarts the mission with it
}

// ---- input ----
enum : uint32_t { kUp = 1, kDown = 2, kLeft = 4, kRight = 8, kAccept = 16, kPrev = 32, kNext = 64, kTabPrev = 128, kTabNext = 256,
                  kPlay = 512, kSpin = 1024, kResetView = 2048 };
float g_turnInput = 0, g_zoomInput = 0;   // held camera input this frame: turn -1..1 (right +), zoom -1..1 (in +)
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
        if (edge & (1u << 6)) in |= kPrev;      // L1
        if (edge & (1u << 7)) in |= kNext;      // R1
        if (edge & (1u << 4)) in |= kTabPrev;   // L2
        if (edge & (1u << 5)) in |= kTabNext;   // R2
        if (edge & (1u << 3)) in |= kPlay;      // Square / X: play the overview
        if (edge & (1u << 10)) in |= kSpin;     // R3: camera spin on / off
        if (edge & (1u << 9)) in |= kResetView; // L3: soldiers look ahead again
    }
    static const int map[][2] = {{VK_UP, kUp},       {VK_DOWN, kDown}, {VK_LEFT, kLeft},  {VK_RIGHT, kRight},
                                 {VK_RETURN, kAccept}, {'Q', kPrev},    {'E', kNext},      {VK_PRIOR, kPrev},
                                 {VK_NEXT, kNext},     {VK_SPACE, kPlay},   {'R', kSpin},
                                 {'C', kResetView}};
    uint32_t keys = 0;
    if (onlyPad < 0) {
        for (auto& k : map)
            if (features::KeyHeld(k[0])) keys |= k[1];
        if (features::KeyHeld(VK_TAB)) keys |= features::KeyHeld(VK_SHIFT) ? kTabPrev : kTabNext;   // Shift+Tab: back
    }
    if (g_primed) in |= keys & ~g_prevKeys;
    g_prevKeys = keys;
    g_primed = true;
    // Held camera input: A / D turn, W / S zoom, the mouse wheel zooms; the right stick (game axes at 0x754270 +
    // j * 0x150, 3 floats per axis, value first: axis 2 right Y, 3 right X) turns and zooms.
    g_turnInput = g_zoomInput = 0;
    if (onlyPad < 0) {
        g_turnInput += (features::KeyHeld('D') ? 1.0f : 0.0f) - (features::KeyHeld('A') ? 1.0f : 0.0f);
        g_zoomInput += (features::KeyHeld('W') ? 1.0f : 0.0f) - (features::KeyHeld('S') ? 1.0f : 0.0f);
        const int wheel = *reinterpret_cast<int*>(0x754C24 + 8);   // DIMOUSESTATE lZ (120 per notch)
        g_zoomInput += std::clamp(wheel / 30.0f, -4.0f, 4.0f);
    }
    for (int j = 0; j < count; ++j) {
        if (onlyPad != -2 && onlyPad != j) continue;
        const auto* axes = reinterpret_cast<const float*>(0x754270 + j * 0x150);
        auto dz = [](float v) { return std::fabs(v) < 0.25f ? 0.0f : v; };
        g_turnInput += dz(axes[9]);
        g_zoomInput -= dz(axes[6]);
    }
    g_turnInput = std::clamp(g_turnInput, -1.0f, 1.0f);
    return in;
}

// ---- rows ----
// SQUAD tab rows (per soldier).
// POUCHES = a whole pouch set; with "free pouch choice" (featFreeGear) the 7 spot rows replace it.
enum Row { kRowUniform, kRowFace, kRowVoice, kRowHead, kRowBack, kRowFaceGear, kRowPouches, kRowBelt, kRowBeltL,
           kRowBeltR, kRowHipL, kRowHipR, kRowChestL, kRowChestR, kRowMain, kRowSidearm, kRowLauncher, kRowGrenades,
           kRowCount };
int GearSlotOf(int row) {   // gear.cpp's slot of a gear row, -1 for other rows
    if (row >= kRowBelt && row <= kRowChestR) return features::kGearFirstPouch + (row - kRowBelt);
    return row == kRowHead ? features::kGearHead : row == kRowBack ? features::kGearBack
                                                   : row == kRowFaceGear ? features::kGearFace : -1;
}

// A row a Lua mission fixed for this soldier (game.kit with lock): uniform / soldier rows when it set a uniform, the
// kit rows it named.
bool RowLocked(const Member& m, int row) {
    const ScriptKit* k = KitRule(m.soldier);
    if (!k || !k->lock) return false;
    if (row == kRowUniform || row == kRowFace) return !k->uniform.empty();
    if (row == kRowMain) return !k->slot[0].empty();
    if (row == kRowSidearm) return !k->slot[1].empty();
    if (row == kRowLauncher) return !k->slot[2].empty();
    if (row == kRowGrenades) return !k->slot[3].empty();
    return false;
}
const char* const kRowNames[kRowCount] = {"UNIFORM", "SOLDIER", "VOICE", "HEADGEAR", "BACKPACK", "FACE", "POUCHES",
                                          "BELT (BACK)", "BELT (LEFT)", "BELT (RIGHT)", "HIP (LEFT)", "HIP (RIGHT)",
                                          "CHEST (LEFT)", "CHEST (RIGHT)", "MAIN WEAPON", "SIDEARM", "LAUNCHER", "GRENADES"};
constexpr float kRowTop = 0.20f;  // fractions of the screen height
constexpr int kMaxRows = 10;      // rows on the panel at once; more scroll
bool RowShown(int r) {
    if ((r >= kRowMain) != (g_tab == kTabLoadout)) return false;   // weapons on LOADOUT, the rest on CHARACTER
    if (r == kRowVoice) return settings::Get().featVoices;
    if (r == kRowPouches) return !settings::Get().featFreeGear;
    if (r >= kRowBelt && r <= kRowChestR) return settings::Get().featFreeGear;
    return true;
}
int ShownBefore(int r) {
    int at = 0;
    for (int k = 0; k < r; ++k) at += RowShown(k) ? 1 : 0;
    return at;
}
int ShownRows() { return ShownBefore(kRowCount); }
float OptH() {   // row pitch: 0.075 of the screen height, less when the rows wouldn't fit above 0.80
    return std::min(0.075f, 0.60f / std::max(1, std::min(ShownRows(), kMaxRows)));
}
int g_rowScroll = 0;   // first row shown (counting shown rows only)
void FollowRow() {     // keeps the selected row in the window
    const int at = ShownBefore(g_row);
    if (at < g_rowScroll) g_rowScroll = at;
    if (at >= g_rowScroll + kMaxRows) g_rowScroll = at - kMaxRows + 1;
    g_rowScroll = std::clamp(g_rowScroll, 0, std::max(0, ShownRows() - kMaxRows));
}
bool RowVisible(int r) {
    const int at = ShownBefore(r);
    return RowShown(r) && at >= g_rowScroll && at < g_rowScroll + kMaxRows;
}
float RowY(int r, float H) {   // hidden and scrolled-off rows take no place
    return H * (kRowTop + (ShownBefore(r) - g_rowScroll) * OptH());
}
int StepRow(int r, int dir) {
    for (int k = 0; k < kRowCount; ++k) {
        r = (r + dir + kRowCount) % kRowCount;
        if (RowShown(r)) break;
    }
    return r;
}

// ---- preview camera ----
// Frames what the selected row changes: the hat for HEADGEAR, the face for FACE, the back for BACKPACK, each pouch
// spot from its side, the hands for weapons, the whole man for UNIFORM. Body points = the bone connectors' world
// positions: connector index FUN_00502c30 (thiscall instance, name hash 0x4B9C30), its world matrix FUN_00502ca0
// (thiscall instance, index; instance +0x22C, 0x44 bytes each), point through the matrix FUN_00540470 (thiscall
// matrix, in, out) - as the game finds a weapon's muzzle (FUN_004410d0). The side a part faces = from the body's
// middle at that height (between the belt's side points / chest and back) out to the part.
// Spin on (setting featLoadoutSpin, R / R3): the camera turns round the part by itself; off: it moves to the part's
// side and stays. A / D or the right stick turn it, W / S, the wheel or the right stick zoom.
struct Vec {
    float x = 0, y = 0, z = 0;
};
Vec Add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec Sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec Mid(Vec a, Vec b) { return {(a.x + b.x) / 2, (a.y + b.y) / 2, (a.z + b.z) / 2}; }
Vec Flat(Vec v) {   // horizontal unit vector, {} if too short
    const float l = std::sqrt(v.x * v.x + v.z * v.z);
    return l < 1.0f ? Vec{} : Vec{v.x / l, 0, v.z / l};
}
Vec Turn(Vec v, float deg) {   // about the up axis
    const float r = deg / 57.2957795f, c = std::cos(r), s2 = std::sin(r);
    return {v.x * c + v.z * s2, 0, -v.x * s2 + v.z * c};
}

// A connector's matrix is only kept up to date while something hangs on it (an empty one reads 0, 0, 0); the bones
// ("BIP01 HEAD", "BIP01 SPINE2", ...) always are - they pose the skin. Bones are found by name in the model's object
// list (instance +0xC8 first object, +0xE8 next, +0xE4 name hash) - the position in that list is the matrix index.
void* BoneMatrix(void* inst, uint32_t hash) {
    int index = 0;
    for (auto* o = *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(inst) + 0xC8); o && index < 256;
         o = *reinterpret_cast<uint8_t**>(o + 0xE8), ++index)
        if (*reinterpret_cast<uint32_t*>(o + 0xE4) == hash)
            return *reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(inst) + 0x22C) + index * 0x44;
    return nullptr;
}

bool BodyPoint(uint8_t* soldier, const char* connector, Vec& out) {
    void* inst = *reinterpret_cast<void**>(soldier + 0x2C);
    if (!inst) return false;
    const uint32_t hash = reinterpret_cast<uint32_t(__cdecl*)(const char*)>(0x4B9C30)(connector);
    void* m = nullptr;
    if (strstr(connector, "CONNECTOR")) {
        const int index = reinterpret_cast<int(__thiscall*)(void*, uint32_t)>(0x502C30)(inst, hash);
        if (index >= 0) m = reinterpret_cast<void*(__thiscall*)(void*, int)>(0x502CA0)(inst, index);
    } else {
        m = BoneMatrix(inst, hash);
    }
    if (!m) return false;
    const float in[3] = {0, 0, 0};
    float o[3] = {};
    reinterpret_cast<void(__thiscall*)(void*, const float*, float*)>(0x540470)(m, in, o);
    const float* feet = reinterpret_cast<const float*>(soldier + 0x30);
    const float dx = o[0] - feet[0], dy = o[1] - feet[1], dz = o[2] - feet[2];
    if (!std::isfinite(o[0]) || dx * dx + dy * dy + dz * dz > 300.0f * 300.0f) {   // not a body point
#ifndef DS_DIST
        static std::map<std::string, int> logged;
        if (logged[connector]++ == 0)
            dslog::Write("[dev] Customise camera: %s at %.0f %.0f %.0f, %.0f cm from the feet - not used", connector, o[0], o[1],
                         o[2], std::sqrt(dx * dx + dy * dy + dz * dz));
#endif
        return false;
    }
    out = {o[0], o[1], o[2]};
    return true;
}

struct Focus {
    Vec target;
    Vec side;         // where the camera looks from (horizontal unit vector from the target), {} = anywhere
    float dist = 280; // cm
    float lift = 30;  // eye above the target, cm
};

Focus FocusFor(uint8_t* s, int row) {
    const float* p = reinterpret_cast<const float*>(s + 0x30);
    const Vec feet{p[0], p[1], p[2]};
    Vec hat, face, back, belt, beltL, beltR, hipL, hipR, chestL, chestR, hand;
    // Each part takes its connector if something hangs on it now, else a point made from the bones.
    Vec head, spine2, pelvis, thighL, thighR, clavL, clavR;
    const bool okHead = BodyPoint(s, "BIP01 HEAD", head), okSpine = BodyPoint(s, "BIP01 SPINE2", spine2);
    const bool okPelvis = BodyPoint(s, "BIP01 PELVIS", pelvis);
    const bool okThighs = BodyPoint(s, "BIP01 L THIGH", thighL) && BodyPoint(s, "BIP01 R THIGH", thighR);
    const bool okClav = BodyPoint(s, "BIP01 L CLAVICLE", clavL) && BodyPoint(s, "BIP01 R CLAVICLE", clavR);
    bool okHat = BodyPoint(s, "BIP01 HAT CONNECTOR", hat), okFace = BodyPoint(s, "BIP01 FACE CONNECTOR", face);
    bool okBack = BodyPoint(s, "BIP01 SPINE1 CONNECTOR", back), okBelt = BodyPoint(s, "BIP01 SPINE2 CONNECTOR", belt);
    bool okBelts = BodyPoint(s, "BIP01 SPINE2L CONNECTOR", beltL) && BodyPoint(s, "BIP01 SPINE2R CONNECTOR", beltR);
    bool okHips = BodyPoint(s, "BIP01 HIPL CONNECTOR", hipL) && BodyPoint(s, "BIP01 HIPR CONNECTOR", hipR);
    bool okChest = BodyPoint(s, "BIP01 CHESTL CONNECTOR", chestL) && BodyPoint(s, "BIP01 CHESTR CONNECTOR", chestR);
    const bool okHand = BodyPoint(s, "BIP01 R HAND", hand);
    // facing: chest front minus back (both worn), else square to the clavicles, pointing away from the back of the
    // spine (the pack side): clavicle line turned a quarter, sign chosen so the head is in front of the upper back
    Vec fwd = okChest && okBack ? Flat(Sub(Mid(chestL, chestR), back)) : Vec{};
    if (fwd.x == 0 && fwd.z == 0 && okClav) {
        const Vec lr = Flat(Sub(clavR, clavL));
        fwd = {-lr.z, 0, lr.x};
        // the head sits a little in front of the upper spine: flip if this points the other way
        if (okHead && okSpine) {
            const Vec hs = Sub(head, spine2);
            if (hs.x * fwd.x + hs.z * fwd.z < 0) fwd = {-fwd.x, 0, -fwd.z};
        }
    }
    if (fwd.x == 0 && fwd.z == 0) fwd = {0, 0, 1};
    // parts with nothing on them now: points made from the bones (cm)
    auto ahead = [&](Vec p, float f, float up) { return Vec{p.x + fwd.x * f, p.y + up, p.z + fwd.z * f}; };
    const Vec hipMid = okThighs ? Mid(thighL, thighR) : pelvis;
    auto outward = [&](Vec t, float k) { return Add(t, Vec{(t.x - hipMid.x) * k, 0, (t.z - hipMid.z) * k}); };
    if (!okHat && okHead) hat = ahead(head, 0, 14), okHat = true;
    if (!okFace && okHead) face = ahead(head, 10, 2), okFace = true;
    if (!okBack && okSpine) back = ahead(spine2, -14, 0), okBack = true;
    if (!okBelt && okPelvis) belt = ahead(pelvis, -13, 8), okBelt = true;
    if (!okBelts && okThighs) beltL = Add(outward(thighL, 0.6f), Vec{0, 14, 0}), beltR = Add(outward(thighR, 0.6f), Vec{0, 14, 0}), okBelts = true;
    if (!okHips && okThighs) hipL = outward(thighL, 1.0f), hipR = outward(thighR, 1.0f), okHips = true;
    if (!okChest && okClav && okSpine)
        chestL = ahead(Mid(clavL, spine2), 12, 0), chestR = ahead(Mid(clavR, spine2), 12, 0), okChest = true;
    const Vec back3 = {-fwd.x, 0, -fwd.z};
    const Vec waist = okBelts ? Mid(beltL, beltR) : Add(feet, Vec{0, 95, 0});
    const Vec chest = okChest && okBack ? Mid(Mid(chestL, chestR), back) : Add(waist, Vec{0, 30, 0});
    auto outFrom = [&](Vec part, Vec centre) {   // the side a part faces
        Vec d = Flat(Sub(part, centre));
        return d.x == 0 && d.z == 0 ? fwd : d;
    };
    Focus f;
    const float top = okHat ? hat.y : feet.y + 170;
    f.target = {feet.x, (feet.y + top) / 2, feet.z};   // the whole man
    f.side = Turn(fwd, 30);
    f.dist = 280, f.lift = 30;
    switch (row) {
    case kRowHead:
        if (okHat) f.target = Add(hat, Vec{0, -8, 0}), f.side = Turn(fwd, 25), f.dist = 85, f.lift = 18;
        break;
    case kRowFaceGear:
        if (okFace) f.target = face, f.side = Turn(fwd, 15), f.dist = 70, f.lift = 6;
        break;
    case kRowFace:
    case kRowVoice:
        if (okFace) f.target = Add(face, Vec{0, -10, 0}), f.side = Turn(fwd, 20), f.dist = 120, f.lift = 10;
        break;
    case kRowBack:
        if (okBack) f.target = back, f.side = Turn(back3, 20), f.dist = 140, f.lift = 25;
        break;
    case kRowPouches:
        f.target = waist, f.side = Turn(back3, 45), f.dist = 150, f.lift = 35;
        break;
    case kRowBelt:
        if (okBelt) f.target = belt, f.side = outFrom(belt, waist), f.dist = 100, f.lift = 20;
        break;
    case kRowBeltL:
    case kRowBeltR:
        if (okBelts) {
            const Vec part = row == kRowBeltL ? beltL : beltR;
            f.target = part, f.side = outFrom(part, waist), f.dist = 100, f.lift = 20;
        }
        break;
    case kRowHipL:
    case kRowHipR:
        if (okHips) {
            const Vec part = row == kRowHipL ? hipL : hipR;
            f.target = part, f.side = outFrom(part, Add(waist, Vec{0, part.y - waist.y, 0})), f.dist = 100, f.lift = 20;
        }
        break;
    case kRowChestL:
    case kRowChestR:
        if (okChest) {
            const Vec part = row == kRowChestL ? chestL : chestR;
            f.target = part, f.side = outFrom(part, chest), f.dist = 100, f.lift = 15;
        }
        break;
    case kRowMain:
    case kRowSidearm:
        if (okHand) f.target = hand, f.side = Turn(fwd, 40), f.dist = 150, f.lift = 20;
        break;
    case kRowLauncher:
        if (okBack) f.target = back, f.side = Turn(back3, 35), f.dist = 190, f.lift = 30;
        break;
    case kRowGrenades:
        f.target = waist, f.side = Turn(fwd, -35), f.dist = 160, f.lift = 30;
        break;
    default:
        break;   // UNIFORM: the whole man
    }
    return f;
}

struct Camera {
    bool set = false;
    Vec target;
    float yaw = 0, dist = 280, lift = 30;
} g_cam;
float g_zoom = 1.0f, g_manualYaw = 0.0f;   // the player's zoom (kept across rows) and turn (until the row changes)
int g_focusKey = -1;

void ResetCamera() {
    g_cam = {};
    g_zoom = 1.0f, g_manualYaw = 0.0f, g_focusKey = -1;
}

float AngleTo(float from, float to) {   // shortest turn
    float d = std::fmod(to - from, 6.2831853f);
    if (d > 3.14159265f) d -= 6.2831853f;
    if (d < -3.14159265f) d += 6.2831853f;
    return d;
}

void CameraView(float* eyeOut, float* angOut) {
    static LARGE_INTEGER last{};
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = last.QuadPart ? std::min(0.1f, static_cast<float>(now.QuadPart - last.QuadPart) / freq.QuadPart) : 0.0f;
    last = now;
    uint8_t* soldier = g_squad[g_sel].soldier;
    const int row = MapTab() ? kRowUniform : g_row;
    uint8_t* vehicle = g_tab == kTabVehicle ? features::VehicleObject(g_vsel) : nullptr;
    const int key = vehicle ? 900000 + g_vsel : g_sel * 1000 + g_tab * 100 + row;
    if (key != g_focusKey) g_focusKey = key, g_manualYaw = 0.0f;   // a new part: back to its own view
    Focus f;
    if (vehicle) {   // the VEHICLE tab: the whole vehicle from a few metres out, a little above
        const float* p = reinterpret_cast<const float*>(vehicle + 0x30);
        f.target = {p[0], p[1] + 130.0f, p[2]};
        f.dist = 900.0f, f.lift = 260.0f;
    } else {
        f = FocusFor(soldier, row);
    }
    const bool spin = settings::Get().featLoadoutSpin;
    g_zoom = std::clamp(g_zoom * std::exp(-g_zoomInput * dt * 1.4f), 0.45f, 2.2f);
    const float turn = g_turnInput * 1.8f * dt;
    const float partYaw = f.side.x == 0 && f.side.z == 0 ? g_cam.yaw : std::atan2(f.side.x, f.side.z);
    if (!g_cam.set) {   // first frame: straight to the view
        g_cam.set = true;
        g_cam.target = f.target, g_cam.yaw = partYaw, g_cam.dist = f.dist * g_zoom, g_cam.lift = f.lift;
    }
    const float k = 1.0f - std::exp(-dt * 5.0f);   // ease towards the wanted view
    g_cam.target = Add(g_cam.target, Vec{(f.target.x - g_cam.target.x) * k, (f.target.y - g_cam.target.y) * k,
                                         (f.target.z - g_cam.target.z) * k});
    g_cam.dist += (f.dist * g_zoom - g_cam.dist) * k;
    g_cam.lift += (f.lift - g_cam.lift) * k;
    if (spin) {
        g_cam.yaw += kOrbitSpeed * dt + turn;
    } else {
        g_manualYaw += turn;
        g_cam.yaw += AngleTo(g_cam.yaw, partYaw + g_manualYaw) * k;
    }
    const Vec eye = {g_cam.target.x + std::sin(g_cam.yaw) * g_cam.dist, g_cam.target.y + g_cam.lift,
                     g_cam.target.z + std::cos(g_cam.yaw) * g_cam.dist};
    const float dx = g_cam.target.x - eye.x, dy = g_cam.target.y - eye.y, dz = g_cam.target.z - eye.z;
    constexpr float kDeg = 57.2957795f;  // the angles are in degrees (pitch > 0 looks down)
    eyeOut[0] = eye.x, eyeOut[1] = eye.y, eyeOut[2] = eye.z;
    angOut[0] = -std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * kDeg, angOut[1] = std::atan2(dx, dz) * kDeg, angOut[2] = 0.0f;
    // the fog wall stays a little behind the soldier at any zoom
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (vehicle) features::SetSceneFog(true, kFogColour, d + 450.0f, d + 1100.0f);   // (a tank is 7 m long)
    else features::SetSceneFog(true, kFogColour, d + 40.0f, d + 220.0f);
}

// ---- the squad's animations keep playing while the world is paused ----
// Every render instance's animation channels advance in FUN_005022a0 (called per instance from the scene update
// FUN_004ddee0 at 0x4DDF3D) by the animation timer's frame time, [0x63C954] +0xA8 (read by FUN_005022e0 through
// FUN_004ba480) - which the paused world (time multiplier 0.0001) makes nearly 0. For the squad's instances it gets the
// real frame time.
constexpr uint32_t kAnimateSite = 0x4DDF3D, kAnimate = 0x5022A0;
void __fastcall Animate(void* inst) {
    using Fn = void(__thiscall*)(void*);
    auto* timer = *reinterpret_cast<uint8_t**>(0x63C954);
    float photoDt = 0.0f;   // photo mode: a posed soldier held / scrubbed
    if (timer && features::PhotoAnimate(inst, photoDt)) {
        float& dt = *reinterpret_cast<float*>(timer + 0xA8);
        const float saved = dt;
        dt = photoDt;
        reinterpret_cast<Fn>(kAnimate)(inst);
        dt = saved;
        return;
    }
    bool squad = false;
    if (g_phase == Phase::Open && timer && *reinterpret_cast<float*>(kTimeMultiplier) < 0.01f && !features::OverviewPlaying())
        for (int i = 0; i < g_squadCount && !squad; ++i) squad = *reinterpret_cast<void**>(g_squad[i].soldier + 0x2C) == inst;
    if (!squad) return reinterpret_cast<Fn>(kAnimate)(inst);
    float& dt = *reinterpret_cast<float*>(timer + 0xA8);
    const float saved = dt;
#ifndef DS_DIST
    static int calls = 0;
    if (++calls == 60 || calls == 600) {   // channel 0 / 1 states (instance +0x248 + ch * 0x34: +0 anim, +0xC time, +0x14 loops)
        auto* ch = static_cast<uint8_t*>(inst) + 0x248;
        dslog::Write("[dev] Customise: squad animation (call %d, dt %.4f): ch0 anim %p time %.2f loops %d, ch1 anim %p time %.2f",
                     calls, g_realDt, *reinterpret_cast<void**>(ch), *reinterpret_cast<float*>(ch + 0xC),
                     *reinterpret_cast<int*>(ch + 0x14), *reinterpret_cast<void**>(ch + 0x34), *reinterpret_cast<float*>(ch + 0x40));
    }
#endif
    dt = g_realDt;
    reinterpret_cast<Fn>(kAnimate)(inst);
    dt = saved;
}

// ---- tabs ----
// MISSION (map, objectives, time of day), SQUAD (the soldiers' rows) and OVERVIEW (the mission's night-vision
// fly-over, only where it has one). L2 / R2 (Tab) switch.

// ---- MISSION tab: the level's map and objectives, as the game's OBJECTIVES screen (state 0xD) has them ----
// Objectives: list [[0x60EDC0]+0x12E4] (next +0xD8), entry +4 text hashes [0] title / [1] description, +0x14 flags (2
// shown, 4 complete, 8 tracked), +0x15 marker index, +0x18 positions (vec3 cm, y up). Map: image [0x6107C0] (-1 =
// none) of sheet [0x60EE20] (records +4: 24 B, +0 page, +0xE w, +0x10 h; UVs +8: 16 B; page textures sheet +0xC
// [page] +0x10); world bounds origin 0x6107D0 / 0x6107D8 (x / z), extents 0x6107C4 / 0x6107CC: u = (x - ox) / ex,
// v = |(z - oz) / ez - 1| (the game's 0x454CF0).
constexpr uint32_t kCampaign = 0x60EDC0, kStrings = 0x60EDCC, kLookup = 0x4F6730;
constexpr uint32_t kMapSheet = 0x60EE20, kMapImage = 0x6107C0;
struct Objective {
    std::string title, text;
    bool done = false, hasPos = false;
    float x = 0, z = 0;
};
std::vector<Objective> g_objectives;   // the shown objectives
std::string g_briefing;                // the game's briefing popup text (BRIEFING tab)
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};
Rect g_map;                            // where the map was drawn this frame (w = 0: no map)

std::string Upper(std::string s) {
    for (char& ch : s) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string GameText(uint32_t hash) {
    void* table = *reinterpret_cast<void**>(kStrings);
    if (hash == 0xFFFFFFFF || !table) return "";
    const char* t = reinterpret_cast<const char*(__thiscall*)(void*, uint32_t)>(kLookup)(table, hash);
    return t ? t : "";
}

void ReadObjectives() {
    g_objectives.clear();
    auto* campaign = *reinterpret_cast<uint8_t**>(kCampaign);
    if (!campaign) return;
    {   // the briefing (FUN_00458800: campaign +0x12EC title, +0x12F0 date, +0x12F4 place, pages +0x12F8..+0x130C)
        std::string story;
        for (uint32_t off = 0x12F8; off <= 0x130C; off += 4) {
            const std::string page = GameText(*reinterpret_cast<uint32_t*>(campaign + off));
            if (!page.empty()) story += (story.empty() ? "" : " ") + page;
        }
        const std::string date = GameText(*reinterpret_cast<uint32_t*>(campaign + 0x12F0));
        const std::string place = GameText(*reinterpret_cast<uint32_t*>(campaign + 0x12F4));
        const std::string head = date + (date.empty() || place.empty() ? "" : ", ") + place;
        g_briefing = head.empty() ? story : story.empty() ? head : head + "\n" + story;
        if (features::ScriptLevelActive() && !g_scriptBriefing.empty()) g_briefing = g_scriptBriefing;
    }
    int guard = 0;
    for (auto* e = *reinterpret_cast<uint8_t**>(campaign + 0x12E4); e && guard < 32; e = *reinterpret_cast<uint8_t**>(e + 0xD8), ++guard) {
        const uint8_t flags = e[0x14];
        if (!(flags & 2)) continue;
        Objective o;
        o.title = GameText(*reinterpret_cast<uint32_t*>(e + 4));
        o.text = GameText(*reinterpret_cast<uint32_t*>(e + 8));
        o.done = (flags & 4) != 0;
        float pos[3] = {};   // the game's marker position: the set position, else the attached object's (FUN_00457270)
        reinterpret_cast<void(__thiscall*)(void*, float*)>(0x457270)(e, pos);
        o.hasPos = pos[0] != 0.0f || pos[2] != 0.0f;
        o.x = pos[0], o.z = pos[2];
        g_objectives.push_back(o);
    }
}

bool TimeRows() { return settings::Get().featTimeOfDay; }   // TIME OF DAY / TIME FLOW (timeofday.cpp)
bool WeatherRow() { return settings::Get().featWeather; }    // WEATHER (weather.cpp)
int SettingRows() { return (TimeRows() ? 2 : 0) + (WeatherRow() ? 1 : 0); }   // rows under the objectives
// Setting row r (0-based under the objectives): 0 TIME OF DAY, 1 TIME FLOW, 2 WEATHER.
int SettingKind(int r) { return TimeRows() ? r : r + 2; }
int MissionRows() { return std::max(1, static_cast<int>(g_objectives.size()) + SettingRows()); }

// World (cm) -> point on the drawn map; false if outside it.
bool MapPoint(float x, float z, float& sx, float& sy) {
    const float ox = *reinterpret_cast<float*>(0x6107D0), oz = *reinterpret_cast<float*>(0x6107D8);
    const float ex = *reinterpret_cast<float*>(0x6107C4), ez = *reinterpret_cast<float*>(0x6107CC);
    if (g_map.w <= 0 || ex == 0 || ez == 0) return false;
    const float u = (x - ox) / ex, v = std::fabs((z - oz) / ez - 1.0f);
    if (u < 0 || u > 1 || v < 0 || v > 1) return false;
    sx = g_map.x + u * g_map.w, sy = g_map.y + v * g_map.h;
    return true;
}

void Ring(float cx, float cy, float r, float width, uint32_t argb) {
    constexpr int kSegments = 24;
    for (int i = 0; i < kSegments; ++i) {
        const float a0 = 6.2831853f * i / kSegments, a1 = 6.2831853f * (i + 1) / kSegments;
        overlay::QueueLine(cx + r * std::cos(a0), cy + r * std::sin(a0), cx + r * std::cos(a1), cy + r * std::sin(a1), width, argb);
    }
}

// The map with the objectives and the squad on it, fitted into the area (queued with the frame's shapes).
void DrawMap(const Rect& area, float H) {
    g_map = {};
    auto* sheet = *reinterpret_cast<uint8_t**>(kMapSheet);
    const int img = *reinterpret_cast<int*>(kMapImage);
    if (!sheet || img < 0) return;
    const uint8_t* header = *reinterpret_cast<uint8_t**>(sheet);
    const uint8_t* records = *reinterpret_cast<uint8_t**>(sheet + 4);
    const float* uvs = *reinterpret_cast<float**>(sheet + 8);
    auto* const* pages = *reinterpret_cast<uint8_t* const* const*>(sheet + 0xC);
    if (!header || !records || !uvs || !pages || img >= *reinterpret_cast<const uint16_t*>(header + 6)) return;
    const uint8_t* rec = records + img * 24;
    const float* uv = uvs + img * 4;
    const uint8_t* page = pages[*reinterpret_cast<const uint16_t*>(rec)];
    void* texture = page ? *reinterpret_cast<void* const*>(page + 0x10) : nullptr;
    const float iw = *reinterpret_cast<const uint16_t*>(rec + 0xE), ih = *reinterpret_cast<const uint16_t*>(rec + 0x10);
    if (!texture || iw <= 0 || ih <= 0) return;
    const float scale = std::min(area.w / iw, area.h / ih);
    g_map = {area.x + (area.w - iw * scale) / 2, area.y + (area.h - ih * scale) / 2, iw * scale, ih * scale};
    const float c[4][2] = {{g_map.x, g_map.y}, {g_map.x + g_map.w, g_map.y}, {g_map.x, g_map.y + g_map.h},
                           {g_map.x + g_map.w, g_map.y + g_map.h}};
    overlay::QueueScreenQuad(texture, c, uv[0], uv[1], uv[2], uv[3]);
    const float r = H * 0.022f, line = std::max(2.0f, H / 360.0f);
    for (int i = 0; i < static_cast<int>(g_objectives.size()); ++i) {
        const Objective& o = g_objectives[i];
        float sx, sy;
        if (!o.hasPos || !MapPoint(o.x, o.z, sx, sy)) continue;
        const bool sel = i == g_mrow && g_tab == kTabMission;
        Ring(sx, sy, sel ? r * 1.3f : r, sel ? line * 1.6f : line, o.done ? 0xFF40C040 : sel ? 0xFFE8C060 : 0xFF2050E0);
    }
    for (int i = 0; i < g_squadCount; ++i) {   // the squad: small squares (the soldier being looked at in gold)
        const float* pos = reinterpret_cast<const float*>(g_squad[i].soldier + 0x30);
        float sx, sy;
        if (!MapPoint(pos[0], pos[2], sx, sy)) continue;
        const float d = H * 0.008f;
        overlay::QueueScreenRect(sx - d - 1, sy - d - 1, 2 * d + 2, 2 * d + 2, 0xFF000000);
        overlay::QueueScreenRect(sx - d, sy - d, 2 * d, 2 * d, i == 0 ? 0xFF40D040 : 0xFF90E090);
    }
}

void ChangeMission(int dir) {
    const int n = static_cast<int>(g_objectives.size());
    if (g_mrow < n) {   // briefing / objective rows: Left / Right turn the text's pages
        g_textPage = std::max(0, std::min(g_textPages - 1, g_textPage + dir));
        return;
    }
    const int kind = SettingKind(g_mrow - n);
    if (kind == 2) {
        if (g_linkedLevel) return;   // a continuing level keeps its weather
        features::SetWeather((features::WeatherChoice() + dir + features::kWeatherCount) % features::kWeatherCount);
        return;
    }
    if (!TimeRows()) return;
    int t = features::TimeOfDay(), f = features::TimeFlow();
    if (kind == 0) t = (t + dir + features::kTimeOfDayCount) % features::kTimeOfDayCount;
    else f = (f + dir + features::kTimeFlowCount) % features::kTimeFlowCount;
    features::SetTimeOfDay(t, f);
}

void Fill(float x, float y, float w, float h, uint32_t argb) {   // drawn at once (late draw: under the text that follows)
    const float r[1][4] = {{x, y, w, h}};
    overlay::FillRects(r, 1, argb);
}

// Text cut into lines of at most maxW pixels (at spaces).
std::vector<std::string> Wrap(void* font, const std::string& text, float maxW) {
    std::vector<std::string> lines;
    std::string line, word;
    auto flush = [&] {
        if (!word.empty()) {
            const std::string tryLine = line.empty() ? word : line + " " + word;
            if (!line.empty() && overlay::TextWidth(font, tryLine.c_str()) > maxW) lines.push_back(line), line = word;
            else line = tryLine;
            word.clear();
        }
    };
    for (char c : text) {
        if (c == ' ' || c == '\n' || c == '\r') {
            flush();
            if (c == '\n' && !line.empty()) lines.push_back(line), line.clear();
        } else {
            word += c;
        }
    }
    flush();
    if (!line.empty()) lines.push_back(line);
    return lines;
}

void Change(int dir) {
    if (g_linkedLevel) return;   // carried kit and continuing time: view only
    if (g_tab == kTabMission) return ChangeMission(dir);
    if (g_tab == kTabVehicle) {   // co-op: player 1 picks the vehicles
        if (g_turn == 0) features::StepVehicle(g_vsel, dir);
        return;
    }
    if (g_tab == kTabBriefing) {
        g_textPage = std::max(0, std::min(g_textPages - 1, g_textPage + dir));
        return;
    }
    Member& m = g_squad[g_sel];
    if (RowLocked(m, g_row)) return;
    if (g_row == kRowVoice) {   // AUTO, SAS, DELTA (English game only), OWN (when his skin brings a voice)
        const bool own = features::SoldierHasOwnVoice(m.soldier), accents = features::SpeechLanguage() >= 0;
        int v = features::VoiceChoice(m.role);
        for (int k = 0; k < features::kVoiceCount; ++k) {
            v = (v + dir + features::kVoiceCount) % features::kVoiceCount;
            if ((v == 3 && !own) || ((v == 1 || v == 2) && !accents)) continue;
            break;
        }
        features::SetVoiceChoice(m.role, v);
        dslog::Write("Customise: %s voice -> %s", kRoleNames[m.role], features::VoiceLabel(v));
        return;
    }
    if (g_row == kRowGrenades) {
        ChangeGrenades(m.soldier, dir);
        return;
    }
    if (const int g = GearSlotOf(g_row); g >= 0) {
        ChangeGear(m.soldier, g, dir);
        return;
    }
    if (g_row == kRowPouches) {
        ChangePouches(m.soldier, dir);
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
#ifndef DS_DIST
    // Dev: Enter presses on this screen (user: several needed on a real screen) - seen or not, and who had the focus.
    static bool enterWas = false;
    const bool enterNow = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    if (enterNow && !enterWas) {
        HWND fg = GetForegroundWindow();
        char cls[64] = "-";
        if (fg) GetClassNameA(fg, cls, sizeof cls);
        dslog::Write("[dev] Customise: Enter down - foreground %p (%s), game window %p, focused %d, KeyHeld %d, primed %d",
                     static_cast<void*>(fg), cls, static_cast<void*>(wnd), features::GameFocused(),
                     features::KeyHeld(VK_RETURN), g_primed);
    }
    enterWas = enterNow;
#endif
    if (!wnd || !features::GameFocused()) return;
    const uint32_t in = ReadInput();
#ifndef DS_DIST
    if (in & kAccept) dslog::Write("[dev] Customise: Accept (tab %d)", g_tab);
#endif
    for (int step : {in & kPrev ? -1 : 0, in & kNext ? 1 : 0}) {
        if (!step) continue;
        if (g_tab == kTabVehicle) {   // Q / E: the squad's next kind of vehicle
            const int n = std::max(1, features::VehicleSlots());
            g_vsel = ((g_vsel + step) % n + n) % n;
            continue;
        }
        for (int k = 1; k <= g_squadCount; ++k) {   // the next of this player's soldiers
            const int i = ((g_sel + step * k) % g_squadCount + g_squadCount) % g_squadCount;
            if (Mine(i)) {
                g_sel = i;
                break;
            }
        }
    }
    for (int step : {in & kTabPrev ? -1 : 0, in & kTabNext ? 1 : 0}) {
        if (!step) continue;
        for (int k = 1; k < kTabCount; ++k) {
            const int t = ((g_tab + step * k) % kTabCount + kTabCount) % kTabCount;
            if (TabAvailable(t)) {
                g_tab = t;
                break;
            }
        }
        g_textPage = 0;
        if (!MapTab() && g_tab != kTabVehicle) g_row = StepRow(kRowCount - 1, 1), g_rowScroll = 0;   // the tab's first row
    }
    if (g_vsel >= features::VehicleSlots()) g_vsel = 0;
    // the squad's vehicles: the selected one (or its preview) on the VEHICLE tab, all hidden on the others
    features::VehicleShow(g_tab == kTabVehicle ? g_vsel : -1);
    if (MapTab()) ReadObjectives();
    if (g_tab == kTabOverview && (in & kPlay)) features::StartOverview();
    if (g_tab == kTabBriefing) {
        if (in & kUp) g_textPage = std::max(0, g_textPage - 1);
        if (in & kDown) g_textPage = std::min(g_textPages - 1, g_textPage + 1);
    } else if (g_tab == kTabMission) {
        const int n = MissionRows();
        if (g_mrow >= n) g_mrow = n - 1;
        if (in & kUp) g_mrow = (g_mrow + n - 1) % n, g_textPage = 0;
        if (in & kDown) g_mrow = (g_mrow + 1) % n, g_textPage = 0;
    } else if (g_tab == kTabVehicle) {
        // one row (VEHICLE); Up / Down do nothing
    } else {
        if (!RowShown(g_row)) g_row = StepRow(g_row, 1);
        if (in & kUp) g_row = StepRow(g_row, -1);
        if (in & kDown) g_row = StepRow(g_row, 1);
        FollowRow();
    }
    if (in & kLeft) Change(-1);
    if (in & kRight) Change(1);
    if (in & kSpin) {
        settings::SetToggle(&settings::Values::featLoadoutSpin, !settings::Get().featLoadoutSpin);
        dslog::Write("Customise: camera spin %s", settings::Get().featLoadoutSpin ? "on" : "off");
    }
    if (in & kResetView) {
        g_resetView = true;
        Settle();
        LookAhead();
        dslog::Write("Customise: view reset");
    }
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
    if (MapTab()) {   // cover the soldier window: map left, mission text right
        band(wx0 - t, wy0 - t, wx1 - wx0 + 2 * t, wy1 - wy0 + 2 * t, kBack);
        band(W * 0.04f, H * 0.14f, W * 0.50f, H * 0.72f, kPanel);
        band(W * 0.56f, H * 0.14f, W * 0.40f, H * 0.72f, kPanel);
        const float m = H * 0.025f;
        DrawMap({W * 0.04f + m, H * 0.14f + m, W * 0.50f - 2 * m, H * 0.72f - 2 * m}, H);
        return;
    }
    const float rowH = H * 0.09f;
    if (g_tab == kTabVehicle) {   // the vehicle list left, its one row right
        band(W * 0.04f, H * 0.20f + g_vsel * rowH, W * 0.30f, rowH, kSel);
        band(W * 0.66f, H * kRowTop, W * 0.30f, H * 0.075f, kSel);
        return;
    }
    band(W * 0.04f, H * 0.20f + g_sel * rowH, W * 0.30f, rowH, kSel);
    FollowRow();   // (also after the free pouch setting changed the rows)
    if (RowVisible(g_row)) band(W * 0.66f, RowY(g_row, H), W * 0.30f, H * OptH(), kSel);
    {   // more rows above / below: small chevrons at the panel's right edge
        const float x = W * 0.935f, d = H * 0.012f, line = std::max(2.0f, H / 400.0f);
        constexpr uint32_t kArrow = 0xFFC8A040;
        if (g_rowScroll > 0) {
            const float y = H * kRowTop - H * 0.01f;
            overlay::QueueLine(x - d, y + d * 0.6f, x, y - d * 0.4f, line, kArrow);
            overlay::QueueLine(x, y - d * 0.4f, x + d, y + d * 0.6f, line, kArrow);
        }
        if (g_rowScroll + kMaxRows < ShownRows()) {
            const float y = H * (kRowTop + kMaxRows * OptH()) + H * 0.01f;
            overlay::QueueLine(x - d, y - d * 0.6f, x, y + d * 0.4f, line, kArrow);
            overlay::QueueLine(x, y + d * 0.4f, x + d, y - d * 0.6f, line, kArrow);
        }
    }
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
// Q / E (L1 / R1): the soldier, or on the VEHICLE tab the vehicle (when there is more than one).
bool Cycle() { return !MapTab() && (g_tab != kTabVehicle || features::VehicleSlots() > 1); }
std::string CycleWhat() { return g_tab == kTabVehicle ? " VEHICLE     " : " SOLDIER     "; }

std::string PromptLine(bool last) {
    using overlay::Icon;
    int style = Players() ? features::CoopPromptStyle(g_turn) : -1;  // 0 keyboard, 1 PlayStation, 2 Xbox
    if (style < 0) style = features::KeyboardInUse() ? 0 : features::PadStyleXbox() ? 2 : 1;
    const char* action = last ? "START MISSION" : "NEXT PLAYER";
    if (!overlay::GlyphsReady()) {
        return std::string(style ? "D-PAD  CHOOSE / CHANGE     L1/R1  SOLDIER     L2/R2  TAB     CROSS  "
                                 : "UP/DOWN  CHOOSE     LEFT/RIGHT  CHANGE     Q/E  SOLDIER     TAB  NEXT TAB     ENTER  ") + action;
    }
    auto icon = [](Icon i) { return std::string(overlay::IconChar(i)); };
    auto pad = [&](int button) {  // game button numbering (0 Triangle .. 7 R1)
        return static_cast<Icon>(static_cast<int>(style == 2 ? Icon::XbTriangle : Icon::PsTriangle) + button);
    };
    if (!style)
        return icon(Icon::KbArrowUp) + icon(Icon::KbArrowDown) + " CHOOSE     " +
               (g_linkedLevel ? std::string() : icon(Icon::KbArrowLeft) + icon(Icon::KbArrowRight) + " CHANGE     ") +
               (Cycle() ? icon(Icon::KbQ) + icon(Icon::KbE) + CycleWhat() : std::string()) +
               icon(Icon::KbShift) + icon(Icon::KbTab) + icon(Icon::KbTab) + " TAB     " + icon(Icon::KbEnter) + " " + action;
    const Icon vertical = style == 2 ? Icon::XbDpadVertical : Icon::PsDpadVertical;
    const Icon horizontal = style == 2 ? Icon::XbDpadHorizontal : Icon::PsDpadHorizontal;
    return icon(vertical) + " CHOOSE     " + (g_linkedLevel ? std::string() : icon(horizontal) + " CHANGE     ") +
           (Cycle() ? icon(pad(6)) + icon(pad(7)) + CycleWhat() : std::string()) + icon(pad(4)) + icon(pad(5)) +
           " TAB     " + icon(pad(2)) + " " + action;
}

// Camera hints (soldier tabs): turn, zoom, spin on / off, reset view.
std::string CameraLine() {
    using overlay::Icon;
    int style = Players() ? features::CoopPromptStyle(g_turn) : -1;
    if (style < 0) style = features::KeyboardInUse() ? 0 : features::PadStyleXbox() ? 2 : 1;
    const std::string spin = settings::Get().featLoadoutSpin ? "SPIN: ON" : "SPIN: OFF";
    if (!overlay::GlyphsReady())
        return style ? "RIGHT STICK  TURN / ZOOM     R3  " + spin + "     L3  RESET VIEW"
                     : "A/D  TURN     W/S  ZOOM     R  " + spin + "     C  RESET VIEW";
    auto icon = [](Icon i) { return std::string(overlay::IconChar(i)); };
    if (!style)
        return icon(Icon::KbA) + icon(Icon::KbD) + " TURN     " + icon(Icon::KbW) + icon(Icon::KbS) + " ZOOM     " +
               icon(Icon::KbR) + " " + spin + "     " + icon(Icon::KbC) + " RESET VIEW";
    const Icon stick = style == 2 ? Icon::XbStickR : Icon::PsStickR;
    const Icon r3 = static_cast<Icon>(static_cast<int>(style == 2 ? Icon::XbTriangle : Icon::PsTriangle) + 10);
    const Icon l3 = static_cast<Icon>(static_cast<int>(style == 2 ? Icon::XbTriangle : Icon::PsTriangle) + 9);
    return icon(stick) + " TURN / ZOOM     " + icon(r3) + " " + spin + "     " + icon(l3) + " RESET VIEW";
}

// Whether Left / Right on a row has anything else to switch to for this soldier.
bool HasOptions(const Member& m, int row) {
    int u = 0, idx = 0;
    if (g_linkedLevel || RowLocked(m, row)) return false;
    if (row == kRowVoice) return features::SpeechLanguage() >= 0 || features::SoldierHasOwnVoice(m.soldier);
    if (row == kRowPouches) return features::PouchSetCount() > 1;
    if (const int g = GearSlotOf(row); g >= 0) {
        const std::string cur = features::GearOn(m.soldier, g);
        for (const std::string& c : features::GearChoices(g))
            if (_stricmp(c.c_str(), cur.c_str()) != 0) return true;
        return false;
    }
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

// VEHICLE tab text: the squad's kinds of vehicle left, the chosen one right (with its side, seats and what starting
// the mission does with a change).
void VehicleLateDraw(float W, float H) {
    constexpr uint32_t kWhite = 0xFFFFFFFF, kDim = 0xFFA0A8B0, kGoldText = 0xFFE8C060;
    const float rowH = H * 0.09f, maxW = W * 0.27f;
    Text("VEHICLES", W * 0.06f, H * 0.185f, kGoldText, true);
    Text("VEHICLE", W * 0.68f, H * 0.185f, kGoldText, true);
    const int n = features::VehicleSlots();
    for (int i = 0; i < n; ++i) {
        const float y = H * 0.20f + i * rowH;
        Text(features::VehicleSlotName(i).c_str(), W * 0.06f, y + rowH * 0.45f, i == g_vsel ? kWhite : kDim, true, false, maxW);
        Text(features::VehicleSlotNote(i).c_str(), W * 0.06f, y + rowH * 0.80f, kDim, false, false, maxW);
    }
    const float optH = H * 0.075f, y = H * kRowTop;
    Text("VEHICLE", W * 0.68f, y + optH * 0.40f, kDim);
    const std::string label = features::VehicleChoiceLabel(g_vsel);
    const bool open = !g_linkedLevel && g_turn == 0 && features::VehicleHasChoices(g_vsel);
    const std::string shown =
        open ? "< " + Fit(overlay::MenuFont(), label, maxW - overlay::TextWidth(overlay::MenuFont(), "<  >")) + " >" : label;
    Text(shown.c_str(), W * 0.68f, y + optH * 0.85f, kWhite, false, false, maxW);
    float iy = y + optH * 1.6f;
    for (const std::string& line : features::VehicleChoiceInfo(g_vsel)) {
        Text(line.c_str(), W * 0.68f, iy, kDim, false, false, maxW);
        iy += H * 0.032f;
    }
    if (features::VehiclesChanged()) {
        iy += H * 0.02f;
        for (const std::string& l : Wrap(overlay::MenuFont(), "STARTING THE MISSION RELOADS IT WITH THE NEW VEHICLES "
                                                              "(THE INTRO IS SKIPPED, THIS SCREEN COMES BACK).", maxW)) {
            Text(l.c_str(), W * 0.68f, iy, kGoldText);
            iy += H * 0.032f;
        }
    }
    if (g_linkedLevel) Text("VEHICLES CARRY ON AS THEY ARE", W * 0.68f, H * 0.83f, kGoldText, false, false, maxW);
}

// MISSION tab text: the mission's title, its objectives (the selected one's description below), the time rows.
void MissionLateDraw(float W, float H) {
    constexpr uint32_t kWhite = 0xFFFFFFFF, kDim = 0xFFA0A8B0, kGoldText = 0xFFE8C060, kDone = 0xFF70C070;
    const char* level = reinterpret_cast<const char*>(kLevelName);
    std::string name = features::LevelDisplayName(level), title = features::LevelTitleText(level);
    for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    const float x = W * 0.58f, maxW = W * 0.36f;
    Text((name + (title.empty() ? "" : " - " + title)).c_str(), x, H * 0.185f, kGoldText, true, false, maxW);
    if (g_map.w <= 0) Text("NO MAP AVAILABLE", W * 0.29f, H * 0.5f, kDim, true, true);
    for (int i = 0; i < static_cast<int>(g_objectives.size()); ++i) {   // numbers next to the rings on the map
        float sx, sy;
        if (g_objectives[i].hasPos && MapPoint(g_objectives[i].x, g_objectives[i].z, sx, sy))
            Text(std::to_string(i + 1).c_str(), sx + H * 0.03f, sy + H * 0.01f,
                 i == g_mrow && g_tab == kTabMission ? kGoldText : kWhite);
    }
    float y = H * 0.25f;
    if (g_tab == kTabOverview) {   // what the tab plays, and how
        Text("NIGHT-VISION OVERVIEW", x, y, kGoldText);
        y += H * 0.05f;
        for (const std::string& l : Wrap(overlay::MenuFont(), "A fly-over of the mission area in night vision, showing each objective, with the "
                                                              "radio briefing. Skip it any time with Esc, Enter, Cross or Circle.", maxW)) {
            Text(l.c_str(), x, y, kWhite);
            y += H * 0.032f;
        }
        y += H * 0.04f;
        int style = Players() ? features::CoopPromptStyle(g_turn) : -1;
        if (style < 0) style = features::KeyboardInUse() ? 0 : features::PadStyleXbox() ? 2 : 1;
        const std::string key = !overlay::GlyphsReady() ? (style ? "SQUARE" : "SPACE")
                                : overlay::IconChar(style == 0 ? overlay::Icon::KbSpace
                                                               : style == 2 ? overlay::Icon::XbSquare : overlay::Icon::PsSquare);
        Text((key + "  PLAY OVERVIEW").c_str(), x, y, kGoldText, true);
        return;
    }
    if (g_tab == kTabBriefing) {   // the story, in pages of the panel's height
        const float lineH = H * 0.032f, bottom = H * 0.80f;
        const std::vector<std::string> lines = Wrap(overlay::MenuFont(), g_briefing, maxW);
        const int perPage = std::max(1, static_cast<int>((bottom - y) / lineH));
        g_textPages = std::max(1, (static_cast<int>(lines.size()) + perPage - 1) / perPage);
        g_textPage = std::min(g_textPage, g_textPages - 1);
        for (int k = g_textPage * perPage; k < std::min<int>(static_cast<int>(lines.size()), (g_textPage + 1) * perPage); ++k) {
            Text(lines[k].c_str(), x, y, k == 0 ? kGoldText : kWhite);   // the first line: date and place
            y += lineH;
        }
        if (lines.empty()) Text("NO BRIEFING", x, y, kDim);
        if (g_textPages > 1) {
            char page[32];
            snprintf(page, sizeof page, "< PAGE %d/%d >", g_textPage + 1, g_textPages);
            Text(page, x, H * 0.835f, kGoldText);
        }
        return;
    }
    Text("OBJECTIVES", x, y, kDim);
    y += H * 0.045f;
    const float rowH = H * 0.045f;
    for (int i = 0; i < static_cast<int>(g_objectives.size()); ++i) {
        const Objective& o = g_objectives[i];
        if (i == g_mrow) Fill(W * 0.56f, y - rowH * 0.72f, W * 0.40f, rowH, 0xFF3A4652);
        std::string line = std::to_string(i + 1) + "  " + (o.title.empty() ? "OBJECTIVE" : o.title) + (o.done ? "  (DONE)" : "");
        Text(line.c_str(), x, y, o.done ? kDone : i == g_mrow ? kWhite : kDim, false, false, maxW);
        y += rowH;
    }

    const int n = static_cast<int>(g_objectives.size());
    const int timeRows = SettingRows();
    const float rowPitch = H * 0.07f;
    const float ty = H * 0.86f - timeRows * rowPitch;   // time rows at the bottom of the panel
    g_textPages = 1;
    if (g_mrow < n && !g_objectives[g_mrow].text.empty()) {   // the selected row's text, in pages that fit above them
        y += H * 0.01f;
        const float lineH = H * 0.03f;
        const std::vector<std::string> lines = Wrap(overlay::MenuFont(), g_objectives[g_mrow].text, maxW);
        const int perPage = std::max(1, static_cast<int>((ty - H * 0.06f - y) / lineH));
        g_textPages = (static_cast<int>(lines.size()) + perPage - 1) / perPage;
        g_textPage = std::min(g_textPage, g_textPages - 1);
        for (int k = g_textPage * perPage; k < std::min<int>(static_cast<int>(lines.size()), (g_textPage + 1) * perPage); ++k) {
            Text(lines[k].c_str(), x, y, kWhite);
            y += lineH;
        }
        if (g_textPages > 1) {
            char page[32];
            snprintf(page, sizeof page, "< PAGE %d/%d >", g_textPage + 1, g_textPages);
            Text(page, x, ty - H * 0.045f, kGoldText);
        }
    }
    for (int r = 0; r < timeRows; ++r) {
        const float ry = ty + r * rowPitch;
        const bool sel = g_mrow == n + r;
        if (sel) Fill(W * 0.56f, ry - H * 0.03f, W * 0.40f, H * 0.068f, 0xFF3A4652);
        const int kind = SettingKind(r);
        Text(kind == 2 ? "WEATHER" : kind ? "TIME FLOW" : "TIME OF DAY", x, ry, kDim);
        const std::string label = kind == 2 ? features::WeatherLabel(features::WeatherChoice())
                                  : kind    ? features::TimeFlowLabel(features::TimeFlow())
                                            : features::TimeOfDayLabel(features::TimeOfDay());
        std::string value = "< " + label + " >";
        if (g_linkedLevel)
            value = features::TimeContinues() && kind == 0 ? "CONTINUES FROM " + Upper(features::LevelDisplayName(g_linkedFrom.c_str())) : label;
        Text(value.c_str(), x, ry + H * 0.032f, sel ? kWhite : kDim);
    }
}

void LateDraw() {
    if (g_phase == Phase::Armed || g_phase == Phase::Waiting || g_phase == Phase::Carry || g_phase == Phase::CarryWaiting) {
        // From the end of the intro until the screen opens - the game's own briefing (closed at once), the skipped
        // overview, the frame play starts (rendered before OnFrameCustomise sees it) - the backdrop covers the frame.
        const auto* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
        const uint32_t fe = *reinterpret_cast<uint32_t*>(kFrontEndState);
        if (r && *reinterpret_cast<int8_t*>(kDispatchMode) == 0 &&
            (Playing() || fe == kBriefingState || features::OverviewSkipPending())) {
            const float rects[1][4] = {{0, 0, static_cast<float>(*reinterpret_cast<const int*>(r + 0x40688)),
                                        static_cast<float>(*reinterpret_cast<const int*>(r + 0x4068C))}};
            overlay::FillRects(rects, 1, 0xFF12161A);
        }
        return;
    }
    if (g_phase != Phase::Open || features::OverviewPlaying()) return;
    const float W = g_W, H = g_H;
    if (W <= 0) return;
    constexpr uint32_t kWhite = 0xFFFFFFFF, kDim = 0xFFA0A8B0, kGoldText = 0xFFE8C060;
    char title[64];
    if (Players()) snprintf(title, sizeof title, "MISSION BRIEFING - PLAYER %d", g_turn + 1);
    else strcpy_s(title, "MISSION BRIEFING");
    Text(title, W * 0.5f, H * 0.065f, kGoldText, true, true);
    {   // tab bar
        std::string bar;
        float x = W * 0.5f;
        std::vector<int> tabs;
        for (int tb = 0; tb < kTabCount; ++tb)
            if (TabAvailable(tb)) tabs.push_back(tb);
        void* font = overlay::MenuFont();
        float total = 0;
        for (int tb : tabs) total += overlay::TextWidth(font, kTabNames[tb]) + W * 0.04f;
        x -= total / 2;
        for (int tb : tabs) {
            Text(kTabNames[tb], x, H * 0.115f, tb == g_tab ? kGoldText : kDim);
            const float w = static_cast<float>(overlay::TextWidth(font, kTabNames[tb]));
            if (tb == g_tab) Fill(x, H * 0.122f, w, std::max(2.0f, H / 360.0f), kGoldText);
            x += w + W * 0.04f;
        }
    }
    if (MapTab()) {
        // A level load in progress (scripted level change with the screen up) frees the text tables it reads.
        if (*reinterpret_cast<int8_t*>(kDispatchMode) != 0) return;
        MissionLateDraw(W, H);
        const bool lastTurn = !Players() || g_turn >= Players() - 1;
        Text(PromptLine(lastTurn).c_str(), W * 0.5f, H * 0.93f, kDim, false, true);
        return;
    }
    const float rowH = H * 0.09f;
    if (g_tab == kTabVehicle) {
        VehicleLateDraw(W, H);
        const bool last = !Players() || g_turn >= Players() - 1;
        Text(CameraLine().c_str(), W * 0.5f, H * 0.885f, kDim, false, true);
        Text(PromptLine(last).c_str(), W * 0.5f, H * 0.93f, kDim, false, true);
        return;
    }
    Text("SQUAD", W * 0.06f, H * 0.185f, kGoldText, true);
    Text(g_tab == kTabLoadout ? "WEAPONS" : "UNIFORM & GEAR", W * 0.68f, H * 0.185f, kGoldText, true);
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
    const float optH = H * OptH();
    for (int r = 0; r < kRowCount; ++r) {
        if (!RowVisible(r)) continue;
        const float y = RowY(r, H);
        Text(kRowNames[r], W * 0.68f, y + optH * 0.40f, kDim);
        char value[160] = {};
        int u = 0, idx = 0;
        const bool known = CurrentSkin(m.soldier, m.role, u, idx);
        auto* entry = *reinterpret_cast<uint8_t**>(m.soldier + 0x2A68);
        switch (r) {
        case kRowUniform: snprintf(value, sizeof value, "%s", known ? UniformLabel(u, m.role, idx).c_str() : "?"); break;
        case kRowFace: snprintf(value, sizeof value, "%s %s", NamePart(entry, 0x50), NamePart(entry, 0x54)); break;
        case kRowVoice: snprintf(value, sizeof value, "%s", features::VoiceText(m.soldier, m.role).c_str()); break;
        case kRowPouches: snprintf(value, sizeof value, "%s", features::PouchSetLabel(features::PouchSetOn(m.soldier)).c_str()); break;
        default:
            if (GearSlotOf(r) >= 0)
                snprintf(value, sizeof value, "%s", features::GearLabel(features::GearOn(m.soldier, GearSlotOf(r))).c_str());
            break;
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
    if (g_linkedLevel)
        Text(("KIT CARRIED OVER FROM " + Upper(features::LevelDisplayName(g_linkedFrom.c_str()))).c_str(), W * 0.68f,
             H * 0.83f, kGoldText, false, false, W * 0.27f);
    const bool last = !Players() || g_turn >= Players() - 1;
    Text(CameraLine().c_str(), W * 0.5f, H * 0.885f, kDim, false, true);
    Text(PromptLine(last).c_str(), W * 0.5f, H * 0.93f, kDim, false, true);
}

bool Playing() {
    return *reinterpret_cast<uint32_t*>(kFrontEndState) == 0xE && !Cutscene() &&
           *reinterpret_cast<int8_t*>(kDispatchMode) == 0;
}
}  // namespace

bool features::CustomiseOpen() { return g_phase == Phase::Open; }
// Dev / tests (devserver dev.loadout_close): closes the screen as its last Accept would. Game thread.
bool features::CustomiseClose() {
    if (g_phase != Phase::Open) return false;
    Close();
    return true;
}
bool features::CustomisePending() { return g_phase != Phase::Idle && g_phase != Phase::Done && g_phase != Phase::Open; }

void features::ResetScriptLoadout() {
    g_scriptKits.clear();
    g_scriptLoadout = true;
    g_scriptBriefing.clear();
}
void features::SetScriptLoadout(bool on) { g_scriptLoadout = on; }
void features::SetScriptBriefing(const std::string& text) { g_scriptBriefing = text; }
void features::SetScriptKit(int id, const std::string& uniform, const std::string (&slots)[4], bool lock) {
    ScriptKit& k = g_scriptKits[id];
    k.uniform = uniform;
    for (int i = 0; i < 4; ++i) k.slot[i] = slots[i];
    k.lock = lock;
}
void features::ApplyScriptKitNow(int id) {   // game.kit after the start: at once
    auto* mgr = *reinterpret_cast<uint8_t**>(0x63C9A0);
    auto* table = mgr ? *reinterpret_cast<uint8_t***>(mgr + 0x30) : nullptr;
    auto it = g_scriptKits.find(id);
    if (table && table[id] && it != g_scriptKits.end()) ApplyScriptKit(table[id], it->second);
}
bool features::CustomiseHidesHud() {
    return (g_phase == Phase::Open && !features::OverviewPlaying()) || features::ScriptFilmPlaying();
}

// The level this one continues ("Mission4"), "" if it doesn't continue another.
std::string features::LinkedFrom() { return g_linkedLevel ? g_linkedFrom : std::string(); }

void features::OnFrameCustomise() {
    features::OnFrameVehicles();
    if (features::LockstepActive()) return;   // online co-op: no loadout screen yet (its input isn't synced)
    // features::OnFrame runs before the frame's dispatcher, so this sees a requested load before it happens.
    if (g_phase == Phase::Open && *reinterpret_cast<int8_t*>(kDispatchMode) == 1) Close(true);
    {
        static LARGE_INTEGER last{};
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        g_realDt = last.QuadPart ? std::clamp(static_cast<float>(now.QuadPart - last.QuadPart) / freq.QuadPart, 0.0f, 0.1f) : 0.0f;
        last = now;
    }
    char level[32] = {};
    memcpy(level, reinterpret_cast<const char*>(kLevelName), sizeof level - 1);
    if (strcmp(level, g_level) != 0) {  // new level
        if (g_phase == Phase::Open) Close();
        // Carried kits only when this level follows its chain predecessor in this session (Mission 4's end ->
        // 5A), or restarts a level that did - CarryFrom stays in the registry for good, so 5A started from the
        // mission list / a new campaign / another session got the old Mission 4 kits, view-only.
        // (the last named level: the scene name is cleared between levels for a reload / restart)
        static std::string lastLevel;
        static bool lastLinked = false;
        if (g_level[0]) lastLevel = LevelBase(g_level), lastLinked = g_linkedLevel;
        const std::string prevBase = lastLevel;
        const bool prevLinked = lastLinked;
        memcpy(g_level, level, sizeof g_level);
        g_scriptLevel = features::ScriptLevelActive();
        g_loadoutOn = settings::Get().featLoadout;
        {
            static int logged = -1;
            if (logged != static_cast<int>(g_loadoutOn)) {
                logged = g_loadoutOn;
                dslog::Write(g_loadoutOn ? "[ok]   Loadout: mission briefing + squad loadout screen"
                                         : "[off]  Loadout: the game's briefing, no loadout screen / carried kits");
            }
        }
        g_phase = (_strnicmp(level, "Mission", 7) == 0 && g_loadoutOn) || g_scriptLevel ? Phase::Armed : Phase::Idle;
        g_linkedLevel = false;
        g_briefingClosed = g_briefingMessageSent = false;
        const std::string from = CarryFrom(), base = LevelBase(level);
        const bool follows = _stricmp(prevBase.c_str(), from.c_str()) == 0 ||
                             ((prevBase.empty() || _stricmp(prevBase.c_str(), base.c_str()) == 0) && prevLinked);
        if (g_phase == Phase::Armed && g_loadoutOn && ChainNext(from, base) && !follows)
            dslog::Write("Customise: %s not entered from %s - own kits, no carry-over", level, from.c_str());
        if (g_phase == Phase::Armed && g_loadoutOn && ChainNext(from, base) && follows) {
            g_phase = Phase::Carry;
            g_linkedLevel = true;
            g_linkedFrom = CarryFrom();
        }
        g_armedFrames = 0;
        // (the backdrop over the intro's last frames; a Lua mission has no intro - its opening film must stay visible)
        overlay::SetLateDraw((g_phase == Phase::Armed || g_phase == Phase::Carry) && !g_scriptLevel ? &LateDraw : nullptr);
    }
    if (*reinterpret_cast<int8_t*>(kDispatchMode) != 0) return;  // loading / other
    if (g_loadoutOn &&
        (g_phase == Phase::Armed || g_phase == Phase::Waiting || g_phase == Phase::Carry || g_phase == Phase::CarryWaiting) &&
        *reinterpret_cast<uint32_t*>(kFrontEndState) == kBriefingState) {
        // The game's briefing popup (map + story): closed as its Esc does (state 0xE; its leave sets [0x61079C] and its
        // update then starts the overview, which overview.cpp skips). The setter ignores calls during a transition.
        reinterpret_cast<void(__cdecl*)(uint32_t)>(0x47EF20)(0xE);
        if (!g_briefingClosed) dslog::Write("Customise: game briefing closed");
        g_briefingClosed = true;
    }
    // The briefing's own update sends the mission script message 0x3B once the state is 0xE (0x4024F4: [0x60F578]+8
    // (0x3B, 0)) - the script's step from its intro state to the overview and then its mission set-up (state 2 of
    // MissionN.dll; without it no trigger or AI logic ever runs). Closed by us, the briefing's update doesn't run
    // again, so the message is sent here, once.
    if (g_briefingClosed && !g_briefingMessageSent && *reinterpret_cast<uint32_t*>(kFrontEndState) == 0xE) {
        g_briefingMessageSent = true;
        if (auto* scripts = *reinterpret_cast<uint8_t**>(0x60F578)) {
            reinterpret_cast<void(__cdecl*)(int, int)>(*reinterpret_cast<uint32_t*>(scripts + 8))(0x3B, 0);
            dslog::Write("Customise: briefing message 0x3B sent to the mission script");
        }
    }
    const bool playing = Playing();
    switch (g_phase) {
    case Phase::Armed:  // intro / briefing first
    case Phase::Carry:
        if (g_scriptLevel) {   // a Lua mission goes straight into play: its update() runs a few frames first
            if (!playing) break;
            if (features::ScriptFilmPlaying()) {   // its opening film first (movie.cpp)
                g_armedFrames = 0;
                break;
            }
            if (features::ScriptLoadingSave()) {
                g_phase = Phase::Done;
                overlay::SetLateDraw(nullptr);
            } else if (++g_armedFrames > 15) {
                if (g_scriptLoadout && g_loadoutOn) {
                    Open();
                } else {
                    ApplyScriptKits();
                    g_phase = Phase::Done;
                    overlay::SetLateDraw(nullptr);
                }
            }
            break;
        }
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
        {   // the overview (OVERVIEW tab) plays in the running world, unfogged, with the screen out of the way
            static bool overview = false;
            const bool now = features::OverviewPlaying();
            if (now != overview) {
                overview = now;
                if (now) features::SetSceneFog(false, 0, 0, 0);
                else features::SetSceneFog(true, kFogColour, kFogStart, kFogEnd), g_primed = false;   // back on OVERVIEW
            }
            if (now) {
                *reinterpret_cast<float*>(kTimeMultiplier) = g_savedMultiplier;
                break;
            }
        }
        {
            const bool settling = GetTickCount() < g_settleUntil;
            *reinterpret_cast<float*>(kTimeMultiplier) = settling ? g_savedMultiplier : 0.0001f;
            if (!settling) g_resetView = false;
            LookAhead();   // every frame the screen is up: nothing else may tilt the frozen soldiers
        }
        Update();
        if (g_phase == Phase::Open) DrawShapes();
        break;
    case Phase::CarryWaiting:   // the previous linked mission's kits, once the intro / briefing is over
        if (playing) {
            FindSquad();
            ApplyCarry();
            dslog::Write("Customise: %s continues from %s - kits carried over", g_level, CarryFrom().c_str());
            Open(true);
        }
        break;
    default:
        break;
    }
    // In a linked mission (one with a next level in its chain): keep a snapshot of the kits for the next level.
    static DWORD lastSnap = 0;
    if (g_loadoutOn && playing && g_phase != Phase::Open && ChainHasNext(LevelBase(g_level)) && GetTickCount() - lastSnap > 2000) {
        lastSnap = GetTickCount();
        FindSquad();
        if (g_squadCount) SnapshotCarry(LevelBase(g_level));
    }
}

void features::ApplyCustomise() {
    dslog::Write(settings::Get().featLoadout ? "[ok]   Loadout: on at start-up (taken per level)"
                                             : "[off]  Loadout: off at start-up (taken per level)");
    if (!patch::HookCall(kSetViewSite, reinterpret_cast<const void*>(&SetView), kSetView))
        dslog::Write("[fail] Customise: camera hook");
    if (!patch::HookCall(kObjectiveBarSite, reinterpret_cast<const void*>(&ObjectiveBar), kObjectiveBar))
        dslog::Write("[fail] Customise: objective bar hook");
    if (!patch::HookCall(kAnimateSite, reinterpret_cast<const void*>(&Animate), kAnimate))
        dslog::Write("[fail] Customise: squad animation hook");
}
