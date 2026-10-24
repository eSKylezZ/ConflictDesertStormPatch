// VERY HARD and VERY EASY (2026-10-15): difficulty level 3 above Hard and level 4 below Easy (the game has 0-2; 4 is
// "below Easy" so every existing number keeps its meaning). The Japanese PS2 release ("Conflict Delta", Nov 2003) has
// a VERYHARD too, but it is only the old Hard renamed (its levels are 0.1 / 0.2 / 0.7 / 1.3) - this one is new.
// The game's difficulty (object [0x63C9E0]: +0 enabled, +4 level 0-2, +8/+0xC/+0x10 multiplier per level =
// 0.2 / 0.7 / 1.3 set at 0x40CAB6, +0x14 global 1.0) is read in these places:
//   - multiplier getter 0x4F0080 thiscall(which; 3 = current level) - damage to the squad x m (0x404243), damage to
//     enemies / m (0x4425AD), enemies' detection speed x m (0x407AE7). Index 3 would read the global, so the getter is
//     replaced: level 3 = kVeryHardMultiplier, level 4 = kVeryEasyMultiplier.
//   - experience awards 0x434190 (level getter at 0x4341A2: Easy x0.9, Hard x1.1, else x1) and the script API's
//     is_difficulty_easy +0x3A4 (0x481B50, getter at 0x481B56) / is_difficulty_hard +0x3A8 (0x481B70, getter at
//     0x481B76): level 3 answers as Hard, level 4 as Easy. Every mission sets the enemy skill level to "squad level -
//     3 / 2 / 1" (Easy / Normal / Hard) with set_team_skill_level +0x41C - wrapped: VERY HARD one level more (squad
//     level - 0, at most 9), VERY EASY one less (squad level - 4, at least 0).
//   - saves per mission 0x459190 (campaign +0x5A used, +0x5B + level allowed = 2 each; level >= 3 had none):
//     replaced, VERY HARD allows kVeryHardSaves, VERY EASY kVeryEasySaves.
// DIFFICULTY screen (state 5, vtable 0x5D85E8): create makes 3 item objects (push 3 at 0x41347D, mov edi,3 at
// 0x41348F) -> 5; set-up 0x4134E0 (vtable +0x0C, runs once per front-end load) wrapped: VERY EASY (item 0x807) first,
// VERY HARD (0x806) last - the list shows its item pool in order; the update's id range check (cmp si,0x806 at
// 0x41369F) and id -> level mapping (0x4136A6) take 0x806 -> 3, 0x807 -> 4.
// Optional (setting FeatVeryHard, restart): off = the menu keeps three items; the other hooks only matter at 3 / 4.
// Its own plugin since 2026-10-24 (PSE\Plugins\dsfix.difficulty.dll, dsfix kit): FeatVeryHard lives in its own
// pse.settings key; the two item texts reach the game's text lookup through dsfix.services' text provider.
#include <windows.h>

#include <algorithm>
#include <cstdint>

#include "core/log.h"
#include "core/patch.h"
#include "kit/kit.h"

namespace {
constexpr float kVeryHardMultiplier = 1.8f;  // Hard 1.3
constexpr int kVeryHardSaves = 1;            // Easy / Medium / Hard: 2 per mission
constexpr float kVeryEasyMultiplier = 0.1f;  // Easy 0.2
constexpr int kVeryEasySaves = 3;
constexpr int kVeryHard = 3, kVeryEasy = 4;

constexpr uint32_t kDifficulty = 0x63C9E0;
constexpr uint32_t kMultiplierGetter = 0x4F0080, kLevelGetter = 0x4F0070;
constexpr uint32_t kExperienceLevelCall = 0x4341A2, kApiIsEasyLevelCall = 0x481B56, kApiIsHardLevelCall = 0x481B76;
constexpr uint32_t kSavesLeft = 0x459190, kScriptObject = 0x60F578;
constexpr uint32_t kApiTable = 0x5ED238, kApiSetTeamSkill = kApiTable + 0x41C;
constexpr uint32_t kSquadTeam = 0x5F9B94;

constexpr uint32_t kCreateCapacity = 0x41347D, kCreateItems = 0x41348F;
constexpr uint32_t kSetupSlot = 0x5D85F4, kSetup = 0x4134E0;
constexpr uint32_t kSelectRange = 0x41369F, kSelectMap = 0x4136A6, kSelectSetLevel = 0x4136C0;
constexpr uint32_t kItemSet = 0x4E4FB0, kItemDrawCb = 0x413750, kListRelayout = 0x4E8130;
constexpr uint32_t kTitleDifficulty = 0x4EFFD929;
constexpr uint16_t kVeryHardItem = 0x806, kVeryEasyItem = 0x807;
constexpr uint32_t kTextVeryHard = 0xC0110001, kTextVeryEasy = 0xC0110002;

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}

uint8_t* Difficulty() { return *reinterpret_cast<uint8_t**>(kDifficulty); }
int Level() {
    const uint8_t* d = Difficulty();
    return d ? *reinterpret_cast<const int*>(d + 4) : 1;
}

// FUN_004f0080: 1.0 while disabled, else multiplier[which or the current level] x global.
float __fastcall Multiplier(const uint8_t* d, void*, int which) {
    if (!*reinterpret_cast<const int*>(d)) return 1.0f;
    const int level = which == 3 ? *reinterpret_cast<const int*>(d + 4) : which;
    const float value = level == kVeryHard   ? kVeryHardMultiplier
                        : level == kVeryEasy ? kVeryEasyMultiplier
                                             : reinterpret_cast<const float*>(d + 8)[std::clamp(level, 0, 2)];
    return value * *reinterpret_cast<const float*>(d + 0x14);
}

// The level for code that knows 0-2 only: VERY HARD counts as Hard there, VERY EASY as Easy.
int __fastcall LevelForGame(const uint8_t* d) {
    const int level = *reinterpret_cast<const int*>(d + 4);
    return level == kVeryEasy ? 0 : std::min(level, 2);
}

// FUN_00459190 (thiscall campaign, result in al): saves left in this mission; 1 without a mission script.
uint8_t __fastcall SavesLeft(const uint8_t* campaign) {
    if (!*reinterpret_cast<void**>(kScriptObject)) return 1;
    const int level = Level();
    int allowed;
    if (level == kVeryHard) allowed = std::min<int>(kVeryHardSaves, campaign[0x5D]);
    else if (level == kVeryEasy) allowed = std::max<int>(kVeryEasySaves, campaign[0x5B]);
    else if (level >= 0 && level < 3) allowed = campaign[0x5B + level];
    else allowed = 0;
    const int left = allowed - campaign[0x5A];
    return static_cast<uint8_t>(left > 0 ? left : 0);
}

// Script API +0x41C set_team_skill_level(team, level).
using SetTeamSkill = void(__cdecl*)(int, int);
SetTeamSkill g_setTeamSkill = nullptr;

void __cdecl SetTeamSkillHook(int team, int level) {
    const int difficulty = Level();
    if ((difficulty == kVeryHard || difficulty == kVeryEasy) && team != *reinterpret_cast<const uint8_t*>(kSquadTeam) &&
        level >= 0) {
        const int changed = difficulty == kVeryHard ? std::min(level + 1, 9) : std::max(level - 1, 0);
        dslog::Write("%s: team %d skill level %d -> %d", difficulty == kVeryHard ? "VERY HARD" : "VERY EASY", team, level,
                     changed);
        level = changed;
    }
    g_setTeamSkill(team, level);
}

// Update's id -> level: 0x803..0x807 -> 0..4 (replaces movsx eax,si; sub eax,0x803; the dec chain).
__declspec(naked) void SelectMapStub() {
    __asm {
        movsx eax, si
        sub eax, 0x803
        push eax
        push kSelectSetLevel
        ret
    }
}

bool g_menu = false;  // VERY EASY / VERY HARD items in the menu (FeatVeryHard at start-up)
void* g_setupNext = reinterpret_cast<void*>(kSetup);  // pointer hook chain (patch::HookSlot)

void __fastcall Setup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_setupNext)(screen);
    if (!g_menu) return;
    void* list = At<void*>(screen, 8);
    void** items = At<void**>(list, 0x2C);
    if (At<int16_t>(list, 0x26) != 3 || !items[3] || !items[4]) return;
    using SetFn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    using AddFn = void(__thiscall*)(void*, void*);
    void* veryHard = items[3];
    void* veryEasy = items[4];
    reinterpret_cast<SetFn>(kItemSet)(veryHard, kVeryHardItem, kTextVeryHard, 0, 0x24, 0, 0, kItemDrawCb, 0);
    reinterpret_cast<SetFn>(kItemSet)(veryEasy, kVeryEasyItem, kTextVeryEasy, 0, 0x24, 0, 0, kItemDrawCb, 0);
    reinterpret_cast<AddFn>((*reinterpret_cast<void***>(list))[1])(list, veryHard);
    reinterpret_cast<AddFn>((*reinterpret_cast<void***>(list))[1])(list, veryEasy);
    // list order = pool order: VERY EASY, EASY, MEDIUM, HARD, VERY HARD
    void* const order[5] = {veryEasy, items[0], items[1], items[2], veryHard};
    for (int i = 0; i < 5; ++i) items[i] = order[i];
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + 2 * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = 5;
    // the game starts on MEDIUM (index 1, now 2 below VERY EASY); the re-layout resets it
    const int16_t current = static_cast<int16_t>(At<int16_t>(list, 0x28) + 1);
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTitleDifficulty, 0, 0, 0x7FFF);
    reinterpret_cast<void(__thiscall*)(void*)>(kListRelayout)(list);
    At<int16_t>(list, 0x28) = current;
}

bool InstallLevel3() {
    static const uint8_t getter[] = {0x83, 0x39, 0x00, 0x75, 0x09};             // cmp [ecx],0; jne
    static const uint8_t saves[] = {0xA1, 0x78, 0xF5, 0x60, 0x00};              // mov eax,[0x60F578]
    if (!patch::Matches(kMultiplierGetter, getter, sizeof getter) || !patch::Matches(kSavesLeft, saves, sizeof saves)) {
        dslog::Write("[fail] VERY HARD: difficulty code not recognised - different exe? Not applied.");
        return false;
    }
    // Both getters are replaced (entry hooks that never run the original).
    return patch::HookFunction(kMultiplierGetter, reinterpret_cast<const void*>(&Multiplier), nullptr, getter,
                               sizeof getter) &&
           patch::HookFunction(kSavesLeft, reinterpret_cast<const void*>(&SavesLeft), nullptr, saves, sizeof saves) &&
           patch::HookCall(kExperienceLevelCall, reinterpret_cast<const void*>(&LevelForGame), kLevelGetter) &&
           patch::HookCall(kApiIsEasyLevelCall, reinterpret_cast<const void*>(&LevelForGame), kLevelGetter) &&
           patch::HookCall(kApiIsHardLevelCall, reinterpret_cast<const void*>(&LevelForGame), kLevelGetter) &&
           patch::HookSlot(kApiSetTeamSkill, reinterpret_cast<const void*>(&SetTeamSkillHook),
                           reinterpret_cast<void**>(&g_setTeamSkill));
}

bool InstallMenu() {
    static const uint8_t capacity[] = {0x6A, 0x03};
    static const uint8_t items[] = {0xBF, 0x03, 0x00, 0x00, 0x00};
    static const uint8_t range[] = {0x66, 0x81, 0xFE, 0x06, 0x08, 0x7D, 0x2F};  // cmp si,0x806; jge
    static const uint8_t map[] = {0x0F, 0xBF, 0xC6, 0x2D, 0x03, 0x08, 0x00, 0x00};  // movsx eax,si; sub eax,0x803
    const uint32_t setup = kSetup;
    if (!patch::Matches(kCreateCapacity, capacity, sizeof capacity) || !patch::Matches(kCreateItems, items, sizeof items) ||
        !patch::Matches(kSelectRange, range, sizeof range) || !patch::Matches(kSelectMap, map, sizeof map) ||
        !patch::Matches(kSetupSlot, &setup, 4)) {
        dslog::Write("[fail] VERY HARD: DIFFICULTY screen not recognised - menu item not added.");
        return false;
    }
    const uint8_t capacity4[] = {0x6A, 0x05};
    const uint8_t items4[] = {0xBF, 0x05, 0x00, 0x00, 0x00};
    const uint8_t range4[] = {0x66, 0x81, 0xFE, 0x08, 0x08};
    return patch::Write(kCreateCapacity, capacity4, sizeof capacity4) && patch::Write(kCreateItems, items4, sizeof items4) &&
           patch::Write(kSelectRange, range4, sizeof range4) &&
           patch::WriteJump(kSelectMap, reinterpret_cast<const void*>(&SelectMapStub), sizeof map, map) &&
           patch::HookSlot(kSetupSlot, reinterpret_cast<const void*>(&Setup), &g_setupNext, kSetup);
}
const char* PSE_CALL Text(uint32_t hash) {
    return hash == kTextVeryHard ? "VERY HARD" : hash == kTextVeryEasy ? "VERY EASY" : nullptr;
}

constexpr char kSetting[] = "FeatVeryHard";

bool Load() {
    const DsfixServicesV1* services = kit::Services();
    if (!services ||
        !kit::DeclareBool(kSetting, true, "VERY EASY & VERY HARD", "two more rows on the DIFFICULTY screen",
                          "GAMEPLAY", nullptr, 0))   // restart: the menu is built once per front-end load
        return false;
    services->SetTextProvider(kit::Self(), &Text);
    const bool level3 = InstallLevel3();
    g_menu = level3 && kit::GetBool(kSetting) && InstallMenu();
    dslog::Write(level3 ? (g_menu ? "[ok]   VERY HARD (x%.1f, enemies +1 skill, %d save per mission) / VERY EASY (x%.1f, "
                                    "enemies -1 skill, %d saves)"
                                  : "[off]  VERY HARD / VERY EASY: menu items off (rules installed; x%.1f, %d save / "
                                    "x%.1f, %d saves)")
                        : "[fail] VERY HARD / VERY EASY not applied",
                 kVeryHardMultiplier, kVeryHardSaves, kVeryEasyMultiplier, kVeryEasySaves);
    return true;
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.difficulty", "VERY HARD / VERY EASY", PSE_PLUGIN_SIMULATION, Load)
