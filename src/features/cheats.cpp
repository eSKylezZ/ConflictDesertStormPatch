// Cheats menu (OPTIONS > CHEATS, front-end state 7 - shipped in every version, hidden).
//
// Unlock: the game's own check FUN_00402ef0 (main menu only) - hold Left Shift and type DESERTWATCH, or press
// Square Square Circle Circle L1 L1 R1 R1 L2 L2 R2 R2 on joystick 0. We also take that sequence from every pad
// (an Xbox pad: X X B B LB LB RB RB LT LT RT RT) and the Xbox version's code (X X Y Y, LS LS, RS RS, LT LT RT RT;
// Square Square Triangle Triangle L3 L3 R3 R3 L2 L2 R2 R2 on a PlayStation pad). The game kept "unlocked" in
// [0x606368], which also switches developer views (key assignment lists pad codes, ...): the five reads / writes of
// it in the unlock code and the Options create / set-up now use our own flag, remembered in the registry (CheatsUnlocked).
//
// Screen (vtable 0x5D83D8): the original rows TROOPER LEVEL 0-9 ([0x606364], 0x403110), ENEMY LEVEL = 0 ([0x606360],
// 0x4030F0) and MISSION LIST (state 8), plus INVULNERABLE (player damage [0x5E640C], setter 0x403050 marks the squad's
// soldiers), INFINITE HEALTH (ours: the squad's health is topped up), INFINITE AMMO ([0x60635C], 0x403090) and
// ENEMY AI ([0x5E6414], 0x4030B0 at level start + debug toggle 0x443E40 in a mission). The extra rows are the
// screen's own choice items (0x68 bytes, ctor 0x4E5010) added in create; MISSION LIST is moved to the end after
// every set-up (and back before the next one - the set-up fills the pool by position).
//
// Saves: the game already stores trooper level, enemy level, player damage, infinite ammo and enemy AI in the save
// header (+0x50 flags 1 damage / 2 ammo / 4 AI / 8 enemy level 0, +0x54 trooper level) and restores them on load.
// We add bit 0x100 (infinite health), name such saves "Cheats <name>" (coop.cpp SaveName) and unlock the menu when a
// save with cheats is loaded.
#include "features.h"

#include "../core/log.h"
#include "../core/patch.h"
#include "overlay.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <unordered_map>

namespace {

constexpr uint32_t kGameFlag = 0x606368;
constexpr uint32_t kFlagOperands[] = {0x402EC1, 0x402ED0, 0x402EFE, 0x4718CF, 0x471ABA};  // unlock, Options create + set-up
constexpr uint32_t kUnlock = 0x402EC0;                         // sets the flag + rebuilds the front-end screens
constexpr uint32_t kFrameCall = 0x40F90D, kFrameThunk = 0x40C080;  // per game frame: input poll + unlock check

constexpr uint32_t kVtable = 0x5D83D8;
constexpr uint32_t kCreate = 0x402A60, kSetup = 0x402B70, kUpdate = 0x402D50;
constexpr uint32_t kCapacityImm = 0x402AAE;                    // push 3 (list capacity)
constexpr uint32_t kAlloc = 0x4B83D0, kChoiceCtor = 0x4E5010, kPoolAdd = 0x4E58C0;
constexpr uint32_t kItemSet = 0x4E4FB0, kChoices = 0x4E5090, kChoiceDraw = 0x402E20;
constexpr uint32_t kListInput = 0x4E5D80, kItemById = 0x4E6EC0, kRelayout = 0x4E8130;
constexpr uint32_t kBackPressed = 0x478380, kSetState = 0x47EF20;
constexpr uint32_t kTitle = 0xEE1A0D13, kNo = 0x1F3F3, kYes = 0x17E33D4;

constexpr uint32_t kSetTrooper = 0x403110, kGetTrooper = 0x403100, kSetEnemyZero = 0x4030F0;
constexpr uint32_t kSetPlayerDamage = 0x403050, kSetInfAmmo = 0x403090, kSetEnemyAi = 0x4030B0;
constexpr uint32_t kToggleAiNow = 0x443E40, kAiManager = 0x60EDA0;
constexpr uint32_t kEnemyZero = 0x606360, kPlayerDamage = 0x5E640C, kInfAmmoFlag = 0x60635C, kEnemyAi = 0x5E6414;

constexpr uint32_t kHeaderWriteCall = 0x415858, kHeaderWrite = 0x415910;
constexpr uint32_t kHeaderApplyCall = 0x415E45, kHeaderApply = 0x415ED0;
constexpr uint32_t kSaveInfHealth = 0x100;

constexpr uint32_t kFrontEndState = 0x617C18, kDispatchMode = 0x60EC8C, kLevelName = 0x606880;
constexpr uint32_t kJoysticks = 0x754BC8, kPadStates = 0x7541B8, kPadStride = 0x150;
constexpr uint32_t kObjectManager = 0x63C9A0, kSquadTeam = 0x5F9B94;
constexpr char kRegKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";

// Our rows: id, text (own hashes answered by CheatsText), choice index read / written.
enum Row { kInvulnerable, kInfHealth, kInfAmmo, kEnemyAiRow, kRows };
constexpr uint16_t kFirstId = 0x810;
constexpr uint32_t kTextBase = 0xC00C0001;
const char* const kRowText[kRows + 1] = {"INVULNERABLE", "INFINITE HEALTH", "INFINITE AMMO", "ENEMY AI", "DEVELOPER"};
constexpr uint16_t kDeveloperId = kFirstId + kRows;   // DEVELOPER (developer.cpp, state 0x46)
constexpr uint32_t kStateDeveloper = 0x46, kPlainDraw = 0x402E60, kPlainCtor = 0x4E4F90;

uint32_t g_unlocked = 0;     // the game's unlock code and Options set-up read this (patched operands)
bool g_registryUnlocked = false;
bool g_infHealth = false;
DWORD g_noticeUntil = 0;
void* g_extra[kRows] = {};   // our choice items (pool positions 3.. after create)
void* g_mission = nullptr;   // MISSION LIST item (pool position 2 as the set-up expects it)
void* g_developer = nullptr; // DEVELOPER item (pool position 3 + kRows)

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}

bool InMission() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    return Global<int>(kDispatchMode) == 0 && level[0] && _strnicmp(level, "frontend", 8) != 0;
}

int Trooper() { return reinterpret_cast<uint16_t(__cdecl*)()>(kGetTrooper)(); }

int RowValue(int row) {
    switch (row) {
        case kInvulnerable: return Global<int>(kPlayerDamage) == 0;
        case kInfHealth: return g_infHealth;
        case kInfAmmo: return Global<int>(kInfAmmoFlag) != 0;
        case kEnemyAiRow: return Global<int>(kEnemyAi) != 0;
    }
    return 0;
}

bool AiRunning() {
    void* ai = Global<void*>(kAiManager);
    return ai && At<int>(ai, 0xC) != 0;
}

void SetRow(int row, bool on) {
    switch (row) {
        case kInvulnerable: reinterpret_cast<void(__cdecl*)(int)>(kSetPlayerDamage)(on ? 0 : 1); break;
        case kInfHealth: g_infHealth = on; break;
        case kInfAmmo: reinterpret_cast<void(__cdecl*)(int)>(kSetInfAmmo)(on ? 1 : 0); break;
        case kEnemyAiRow:
            reinterpret_cast<void(__cdecl*)(int)>(kSetEnemyAi)(on ? 1 : 0);
            if (InMission() && Global<void*>(kAiManager) && AiRunning() != on)
                reinterpret_cast<void(__cdecl*)()>(kToggleAiNow)();
            break;
    }
    dslog::Write("Cheats: %s %s", kRowText[row], on ? "on" : "off");
}

void SaveUnlocked() {
    if (g_registryUnlocked) return;
    g_registryUnlocked = true;
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        const DWORD v = 1;
        RegSetValueExA(key, "CheatsUnlocked", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v);
        RegCloseKey(key);
    }
}

// --- screen -----------------------------------------------------------------------------------------------------------

bool __fastcall Create(void* screen, void*) {
    if (!reinterpret_cast<bool(__thiscall*)(void*)>(kCreate)(screen)) return false;
    void* list = At<void*>(screen, 8);
    for (int i = 0; i < kRows; ++i) {
        void* item = reinterpret_cast<void*(__cdecl*)(int, int)>(kAlloc)(0x68, 0x1E);
        if (item) reinterpret_cast<void(__thiscall*)(void*)>(kChoiceCtor)(item);
        g_extra[i] = item;
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kPoolAdd)(list, item);
    }
    void* dev = reinterpret_cast<void*(__cdecl*)(int, int)>(kAlloc)(0x24, 0x1E);
    if (dev) reinterpret_cast<void(__thiscall*)(void*)>(kPlainCtor)(dev);
    g_developer = dev;
    reinterpret_cast<void(__thiscall*)(void*, void*)>(kPoolAdd)(list, dev);
    g_mission = At<void**>(list, 0x2C)[2];
    return true;
}

void __fastcall Setup(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    void** pool = At<void**>(list, 0x2C);
    // Pool back to the order the original set-up fills by position: trooper, enemy, mission list, ours.
    if (g_mission) {
        pool[2] = g_mission;
        for (int i = 0; i < kRows; ++i) pool[3 + i] = g_extra[i];
        pool[3 + kRows] = g_developer;
    }
    reinterpret_cast<void(__thiscall*)(void*)>(kSetup)(screen);
    if (!g_mission || !g_extra[kRows - 1] || !g_developer) return;
    using SetFn = void(__thiscall*)(void*, int, int, int, int, uint32_t, int, uint32_t, int);
    using ChoicesFn = void(__thiscall*)(void*, uint32_t, uint32_t, int, int, int, int, int, int);
    for (int i = 0; i < kRows; ++i) {
        void* item = g_extra[i];
        reinterpret_cast<SetFn>(kItemSet)(item, kFirstId + i, -1, 0, 0x24, kTextBase + i, 0, kChoiceDraw, 0);
        reinterpret_cast<ChoicesFn>(kChoices)(item, kNo, kYes, -1, -1, -1, -1, -1, -1);
        At<uint8_t>(item, 0x24) = static_cast<uint8_t>(RowValue(i));
        reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
    }
    reinterpret_cast<SetFn>(kItemSet)(g_developer, kDeveloperId, kTextBase + kRows, 0, 0x24, kTextBase + kRows, 0,
                                      kPlainDraw, 0);
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, g_developer);
    // Shown in pool order: trooper, enemy, ours, mission list, developer.
    for (int i = 0; i < kRows; ++i) pool[2 + i] = g_extra[i];
    pool[2 + kRows] = g_mission;
    pool[3 + kRows] = g_developer;
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (kRows + 1) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = static_cast<int16_t>(4 + kRows);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTitle, 0, 0, 0x7FFF);
    reinterpret_cast<void(__thiscall*)(void*)>(kRelayout)(list);
}

// The original update, plus our rows (the list's input call can't run twice in a frame, so this replaces it).
int __fastcall Update(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    for (int i = 0; i < kRows; ++i)  // values changed elsewhere (a loaded save, a debug key) show up at once
        if (g_extra[i]) At<uint8_t>(g_extra[i], 0x24) = static_cast<uint8_t>(RowValue(i));
    const int16_t id = reinterpret_cast<int16_t(__thiscall*)(void*)>(kListInput)(list);
    if (id == kDeveloperId) reinterpret_cast<void(__cdecl*)(int)>(kSetState)(kStateDeveloper);
    if (id >= 0x80D && id < kFirstId + kRows) {
        void* item = reinterpret_cast<void*(__thiscall*)(void*, int)>(kItemById)(list, id);
        if (item) {
            if (id == 0x80D) reinterpret_cast<void(__cdecl*)(int)>(kSetTrooper)(At<uint16_t>(item, 0x26));
            else if (id == 0x80E) reinterpret_cast<void(__cdecl*)(int)>(kSetEnemyZero)(At<int8_t>(item, 0x24) > 0);
            else if (id == 0x80F) reinterpret_cast<void(__cdecl*)(int)>(kSetState)(8);
            else SetRow(id - kFirstId, At<uint8_t>(item, 0x24) != 0);
        }
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) reinterpret_cast<void(__cdecl*)(int)>(kSetState)(4);
    return 0;
}

// --- unlock from any pad ----------------------------------------------------------------------------------------------

constexpr int kSeqLen = 12;
const int kSequences[][kSeqLen] = {
    {3, 3, 1, 1, 6, 6, 7, 7, 4, 4, 5, 5},     // PC / PS2: Square Square Circle Circle L1 L1 R1 R1 L2 L2 R2 R2
    {3, 3, 0, 0, 9, 9, 10, 10, 4, 4, 5, 5},   // Xbox: X X Y Y, LS LS, RS RS, LT LT RT RT
};
int g_seq[8][2] = {};

void CheckPads() {
    if (g_unlocked || Global<int>(kFrontEndState) != 1) {
        memset(g_seq, 0, sizeof g_seq);
        return;
    }
    for (int j = 0; j < 8; ++j) {
        if (!Global<void*>(kJoysticks + j * 4)) continue;
        int pressed = -1;
        for (int b = 0; b < 28 && pressed < 0; ++b)
            if (Global<uint8_t>(kPadStates + j * kPadStride + b * 4) & 2) pressed = b;
        if (pressed < 0) continue;
        for (int s = 0; s < 2; ++s) {
            int& at = g_seq[j][s];
            at = pressed == kSequences[s][at] ? at + 1 : (pressed == kSequences[s][0] ? 1 : 0);
            if (at == kSeqLen) {
                dslog::Write("Cheats: unlocked with pad %d (%s code)", j, s ? "Xbox" : "PC / PS2");
                reinterpret_cast<void(__cdecl*)()>(kUnlock)();
                return;
            }
        }
    }
}

// --- in a mission -----------------------------------------------------------------------------------------------------

template <class F>
void ForEachSquadSoldier(F f) {
    auto mgr = Global<uint8_t*>(kObjectManager);
    if (!mgr) return;
    const uint8_t team = Global<uint8_t>(kSquadTeam);
    if (team >= mgr[0x10] || 1 >= mgr[0x11]) return;
    auto lists = *reinterpret_cast<uintptr_t**>(mgr + 4);
    if (!lists) return;
    for (uintptr_t s = lists[team * mgr[0x11] + 1]; s; s = *reinterpret_cast<uintptr_t*>(s + 0x14))
        f(reinterpret_cast<uint8_t*>(s));
}

std::unordered_map<uint8_t*, float> g_fullHealth;  // highest health seen per soldier this level
void* g_healthLevel = nullptr;

void KeepHealth() {
    void* mgr = Global<void*>(kObjectManager);
    if (mgr != g_healthLevel) g_fullHealth.clear(), g_healthLevel = mgr;
    ForEachSquadSoldier([](uint8_t* s) {
        float& health = *reinterpret_cast<float*>(s + 0x54);
        float& full = g_fullHealth[s];
        if (health > full) full = health;
        if (health > 0.0f && health < full) health = full;   // the dead stay dead
    });
}

void Upkeep() {
    static int frames = 0;
    if (!InMission()) return;
    if (g_infHealth) KeepHealth();
    // Invulnerability is a flag on each soldier: mark the ones that joined the squad since (Foley rescued, ...).
    if (Global<int>(kPlayerDamage) == 0 && ++frames % 60 == 0)
        reinterpret_cast<void(__cdecl*)(int)>(kSetPlayerDamage)(0);
}

void DrawNotice() {
    if (!g_noticeUntil || GetTickCount() > g_noticeUntil) return;
    void* font = Global<void*>(0x60EDB8);
    void* renderer = Global<void*>(0x63C924);
    if (!font || !renderer) return;
    const int w = At<int>(renderer, 0x40688), h = At<int>(renderer, 0x4068C);
    const char* text = "CHEATS UNLOCKED - OPTIONS > CHEATS";
    const int tw = overlay::TextWidth(font, text);
    const float boxW = tw + h * 0.08f, boxH = h * 0.085f, top = h * 0.015f;
    const float box[1][4] = {{(w - boxW) / 2, top, boxW, boxH}};
    overlay::FillRects(box, 1, 0xC0000000);
    overlay::DrawLabel(font, text, (w - tw) / 2, static_cast<int>(top + h * 0.0575f), 0xFF80FF80);
}

void __cdecl Frame() {
    reinterpret_cast<void(__cdecl*)()>(kFrameThunk)();  // input poll + the game's own unlock check
    if (!g_unlocked && Global<int>(kGameFlag)) g_unlocked = 1;   // start-up option X
    CheckPads();
    static uint32_t seen = g_unlocked;
    if (g_unlocked && !seen) {
        SaveUnlocked();
        g_noticeUntil = GetTickCount() + 4000;
    }
    seen = g_unlocked;
    Upkeep();
    features::OnFrameFreeCam();   // after the input poll: its pad look adds to this frame's mouse state
}

// --- saves ------------------------------------------------------------------------------------------------------------

int __fastcall WriteHeader(void* self, void*, uint8_t* header) {
    const int r = reinterpret_cast<int(__thiscall*)(void*, uint8_t*)>(kHeaderWrite)(self, header);
    if (g_infHealth) At<uint32_t>(header, 0x50) |= kSaveInfHealth;
    return r;
}

void __cdecl NoteLoadedHeader(const uint8_t* header) {
    const uint32_t flags = *reinterpret_cast<const uint32_t*>(header + 0x50);
    const int trooper = *reinterpret_cast<const uint16_t*>(header + 0x54);
    g_infHealth = (flags & kSaveInfHealth) != 0;
    const bool cheats = !(flags & 1) || (flags & 2) || !(flags & 4) || (flags & 8) || trooper || g_infHealth;
    if (cheats && !g_unlocked) {
        g_unlocked = 1;
        SaveUnlocked();
    }
    if (cheats) dslog::Write("Cheats: save with cheats loaded (flags 0x%X, trooper level %d)", flags, trooper);
}

// At 0x415E45 (call FUN_00415ed0, ecx = the saver, esi = the header read from the file).
__declspec(naked) void ApplyHeaderStub() {
    __asm {
        push ecx
        push esi
        call NoteLoadedHeader
        add esp, 4
        pop ecx
        mov eax, kHeaderApply
        jmp eax
    }
}

}  // namespace

bool features::CheatsActive() {
    return Trooper() > 0 || Global<int>(kEnemyZero) != 0 || Global<int>(kPlayerDamage) == 0 ||
           Global<int>(kInfAmmoFlag) != 0 || Global<int>(kEnemyAi) == 0 || g_infHealth;
}

bool features::CheatsUnlocked() { return g_unlocked != 0; }

const char* features::CheatsText(uint32_t hash) {
    return hash >= kTextBase && hash <= kTextBase + kRows ? kRowText[hash - kTextBase] : nullptr;
}

void features::ApplyCheats() {
    static bool done = false;
    if (done) return;
    done = true;
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kRegKey, "CheatsUnlocked", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v)
        g_unlocked = 1, g_registryUnlocked = true;

    for (uint32_t site : kFlagOperands) {
        const uint32_t game = kGameFlag;
        if (!patch::Matches(site, &game, 4)) {
            dslog::Write("[fail] Cheats: unexpected bytes at 0x%08X - not applied", site);
            return;
        }
    }
    const uint32_t create = kCreate, setup = kSetup, update = kUpdate;
    const uint8_t capacity = 3;
    if (!patch::Matches(kVtable + 4, &create, 4) || !patch::Matches(kVtable + 0xC, &setup, 4) ||
        !patch::Matches(kVtable + 0x10, &update, 4) || !patch::Matches(kCapacityImm, &capacity, 1)) {
        dslog::Write("[fail] Cheats: cheat screen not recognised - not applied");
        return;
    }
    const uint32_t ours = reinterpret_cast<uint32_t>(&g_unlocked);
    for (uint32_t site : kFlagOperands) patch::Write(site, &ours, 4);
    patch::WriteValue<uint8_t>(kCapacityImm, static_cast<uint8_t>(4 + kRows));
    patch::WriteValue(kVtable + 4, reinterpret_cast<uint32_t>(&Create));
    patch::WriteValue(kVtable + 0xC, reinterpret_cast<uint32_t>(&Setup));
    patch::WriteValue(kVtable + 0x10, reinterpret_cast<uint32_t>(&Update));
    const bool frame = patch::HookCall(kFrameCall, reinterpret_cast<const void*>(&Frame), kFrameThunk);
    const bool write = patch::HookCall(kHeaderWriteCall, reinterpret_cast<const void*>(&WriteHeader), kHeaderWrite);
    const bool load = patch::HookCall(kHeaderApplyCall, reinterpret_cast<const void*>(&ApplyHeaderStub), kHeaderApply);
    overlay::AddLateDraw(&DrawNotice);
    dslog::Write("Cheats: menu installed (unlocked %d, frame %d, save %d/%d)", g_unlocked, frame, write, load);
}
