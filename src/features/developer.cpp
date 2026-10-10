// DEVELOPER page (OPTIONS > CHEATS > DEVELOPER, front-end state 0x46): Pivotal's debug tools that only the debug key
// map reached (start flag M, tools/env/debugkeys.py), as menu rows that work with a pad.
//
// Displays are plain flags the game's draw code checks: route markers [0x60EE10], quad tree (AI walk map)
// [0x60EE00], routing debug [0x610924], AI senses [[0x60EDA0]+0], health bars [0x60EE04], engine stat bars
// [0x60ED50] (its handler 0x443A90 also clears the counters first), wireframe = world [0x63C948] +0x19DC bit 0x40
// (handler 0x443EE0 also swaps the clear colour). TIME SPEED = the global time multiplier [0x5F7E64]. Actions:
// COMPLETE MISSION = "Successful completion of mission" (0x444DC0: script message 0x11), RESURRECT ALL = "Resurrect
// people" (0x444B60). Screen: Options sub-screen class with a copied vtable (like versus.cpp's MATCH SETUP), option
// rows (FUN_004e5090 choices, draw callback 0x45DCD0), served by coop.cpp's state -> screen hook.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "features/features.h"

namespace {
constexpr uint32_t kSetState = 0x47EF20, kBackPressed = 0x478380, kListSelected = 0x4E5D80;
constexpr uint32_t kAlloc = 0x4B83D0, kListCtor = 0x4E5B10, kListReserve = 0x4E56C0, kListAddToPool = 0x4E58C0;
constexpr uint32_t kItemCtor = 0x4E4F90, kItemSet = 0x4E4FB0, kItemOptions = 0x4E5090, kListCallbacks = 0x4E59E0;
constexpr uint32_t kOptionRowDraw = 0x45DCD0;
constexpr uint32_t kDrawPanel = 0x456EB0, kDrawTitle = 0x456F40;
constexpr uint32_t kLargeFont = 0x60EDB8, kHudSheet = 0x60EE18, kMainMenuScreen = 0x617A00;
constexpr uint32_t kMainMenuEnter = 0x4153B0, kOptCtor = 0x409B90, kOptVtable = 0x5D8510, kOptSetup = 0x409C90;
constexpr uint32_t kOptItemDrawCb = 0x409E90;
constexpr uint32_t kStateCheats = 7;
constexpr int kVtableEntries = 21;

constexpr uint32_t kRouteMarkers = 0x60EE10, kQuadTree = 0x60EE00, kRoutingDebug = 0x610924, kAiManager = 0x60EDA0;
constexpr uint32_t kHealthBars = 0x60EE04, kStatBars = 0x60ED50, kStatBarsToggle = 0x443A90;
constexpr uint32_t kWorld = 0x63C948, kWireframeToggle = 0x443EE0, kTimeScale = 0x5F7E64;
constexpr uint32_t kCompleteMission = 0x444DC0, kResurrect = 0x444B60, kScriptObject = 0x60F578;
constexpr uint32_t kDispatchMode = 0x60EC8C, kLevelName = 0x606880;

enum : uint32_t {
    kTextTitle = 0xC00D0001, kTextRoutes, kTextQuad, kTextRouting, kTextSenses, kTextHealth, kTextStats, kTextWire,
    kTextTime, kTextComplete, kTextResurrect, kTextT025, kTextT05, kTextT1, kTextT2, kTextT4, kTextEnd
};
const char* const kTexts[kTextEnd - kTextTitle] = {
    "DEVELOPER", "ROUTE MARKERS", "NAVIGATION MAP", "ROUTING DEBUG", "AI SENSES", "HEALTH BARS", "ENGINE STATS",
    "WIREFRAME", "TIME SPEED", "COMPLETE MISSION", "RESURRECT ALL", "0.25x", "0.5x", "1x", "2x", "4x"};
constexpr uint32_t kYesNo[] = {0x0001F3F3, 0x017E33D4};  // NO, YES
constexpr uint32_t kTimeTexts[] = {kTextT025, kTextT05, kTextT1, kTextT2, kTextT4};
constexpr float kTimeValues[] = {0.25f, 0.5f, 1.0f, 2.0f, 4.0f};

enum Row { kRoutes, kQuad, kRouting, kSenses, kHealth, kStats, kWire, kTime, kComplete, kResurrectRow, kRows };
constexpr uint16_t kFirstId = 0x870;

uint8_t g_screen[0x20];
void* g_vtable[kVtableEntries];
bool g_built = false;

template <class T>
T& At(void* base, uint32_t offset) {
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(base) + offset);
}
template <class T>
T& Global(uint32_t va) {
    return *reinterpret_cast<T*>(va);
}
void SetState(uint32_t s) { reinterpret_cast<void(__cdecl*)(uint32_t)>(kSetState)(s); }
void* ItemAt(void* list, int i) { return At<void**>(list, 0x2C)[i]; }
void AddToList(void* list, void* item) {
    reinterpret_cast<void(__thiscall*)(void*, void*)>((*reinterpret_cast<void***>(list))[1])(list, item);
}
bool InMission() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    return Global<int>(kDispatchMode) == 0 && level[0] && _strnicmp(level, "frontend", 8) != 0;
}

void* World() { return Global<void*>(kWorld); }
int* Senses() {
    void* ai = Global<void*>(kAiManager);
    return ai ? static_cast<int*>(ai) : nullptr;
}

int TimeChoice() {
    const float t = Global<float>(kTimeScale);
    int best = 2;
    for (int i = 0; i < 5; ++i)
        if (std::fabs(kTimeValues[i] - t) < std::fabs(kTimeValues[best] - t)) best = i;
    return best;
}

int Value(int row) {
    switch (row) {
        case kRoutes: return Global<int>(kRouteMarkers) != 0;
        case kQuad: return Global<int>(kQuadTree) != 0;
        case kRouting: return Global<int>(kRoutingDebug) != 0;
        case kSenses: return Senses() && Senses()[0] != 0;
        case kHealth: return Global<int>(kHealthBars) != 0;
        case kStats: return Global<int>(kStatBars) != 0;
        case kWire: return World() && (At<uint32_t>(World(), 0x19DC) & 0x40) != 0;
        case kTime: return TimeChoice();
    }
    return 0;
}

void Set(int row, int v) {
    if (Value(row) == v) return;
    switch (row) {
        case kRoutes: Global<int>(kRouteMarkers) = v; break;
        case kQuad: Global<int>(kQuadTree) = v; break;
        case kRouting: Global<int>(kRoutingDebug) = v; break;
        case kSenses: if (Senses()) Senses()[0] = v; break;
        case kHealth: Global<int>(kHealthBars) = v; break;
        case kStats: reinterpret_cast<void(__cdecl*)()>(kStatBarsToggle)(); break;
        case kWire: if (World()) reinterpret_cast<void(__cdecl*)()>(kWireframeToggle)(); break;
        case kTime: Global<float>(kTimeScale) = kTimeValues[v]; break;
    }
    dslog::Write("Developer: %s -> %d", kTexts[kTextRoutes - kTextTitle + row], v);
}

void SetOptionRow(void* item, uint16_t id, uint32_t label, const uint32_t* options, int count, int choice) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, 0xFFFFFFFF, 0, 0x24, static_cast<int>(label), 0, kOptionRowDraw, 0);
    uint32_t h[8];
    for (int i = 0; i < 8; ++i) h[i] = i < count ? options[i] : 0xFFFFFFFF;
    using Opts = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    reinterpret_cast<Opts>(kItemOptions)(item, h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
    At<int32_t>(item, 0x20) = 1;  // value from the option list
    At<int8_t>(item, 0x24) = static_cast<int8_t>(choice);
}
void SetButtonRow(void* item, uint16_t id, uint32_t text) {
    using Fn = void(__thiscall*)(void*, int, uint32_t, int, int, int, int, uint32_t, int);
    reinterpret_cast<Fn>(kItemSet)(item, id, text, 0, 0x24, 0, 0, kOptItemDrawCb, 0);
}

bool __fastcall Create(void* screen, void*) {
    using Alloc = void*(__cdecl*)(uint32_t, uint32_t);
    void* list = reinterpret_cast<Alloc>(kAlloc)(0xA4, 0x1E);
    if (!list) return false;
    reinterpret_cast<void*(__thiscall*)(void*, void*, void*, int)>(kListCtor)(
        list, Global<void*>(kLargeFont), Global<void*>(kHudSheet), 1);
    At<void*>(screen, 8) = list;
    if (!reinterpret_cast<int(__thiscall*)(void*, int)>(kListReserve)(list, kRows)) return false;
    for (int i = 0; i < kRows; ++i) {
        void* item = reinterpret_cast<Alloc>(kAlloc)(0x24, 0x1E);
        if (item) reinterpret_cast<void*(__thiscall*)(void*)>(kItemCtor)(item);
        reinterpret_cast<void(__thiscall*)(void*, void*)>(kListAddToPool)(list, item);
    }
    return true;
}

void __fastcall Setup(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptSetup)(screen);
    void* list = At<void*>(screen, 8);
    using Callbacks = void(__thiscall*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, int, int, int, int, int);
    reinterpret_cast<Callbacks>(kListCallbacks)(list, 0x478440, 0x478480, 0x478530, 0x4785E0, 0x478690, 0, 0, 0, 0, 0);
    At<void*>(list, 0x90) = screen;
    for (int r = kRoutes; r <= kWire; ++r)
        SetOptionRow(ItemAt(list, r), static_cast<uint16_t>(kFirstId + r), kTextRoutes + r, kYesNo, 2, Value(r));
    SetOptionRow(ItemAt(list, kTime), kFirstId + kTime, kTextTime, kTimeTexts, 5, Value(kTime));
    SetButtonRow(ItemAt(list, kComplete), kFirstId + kComplete, kTextComplete);
    SetButtonRow(ItemAt(list, kResurrectRow), kFirstId + kResurrectRow, kTextResurrect);
    for (int i = 2; i < kRows; ++i) AddToList(list, ItemAt(list, i));  // the class set-up added the first two
    At<int16_t>(list, 0x40) = static_cast<int16_t>(At<int16_t>(list, 0x40) + (kRows - 2) * At<int16_t>(list, 0x38));
    At<int16_t>(list, 0x42) = kRows;
    using Layout = void(__thiscall*)(void*, uint32_t, int, int, int);
    reinterpret_cast<Layout>((*reinterpret_cast<void***>(screen))[20])(screen, kTextTitle, 0, 0, 0x7FFF);
}

int __fastcall Update(void* screen, void*) {
    void* list = At<void*>(screen, 8);
    const short id = reinterpret_cast<short(__thiscall*)(void*)>(kListSelected)(list);
    for (int r = kRoutes; r <= kTime; ++r) {  // left / right changed a row: apply it; else show the live state
        int8_t& choice = At<int8_t>(ItemAt(list, r), 0x24);
        static int8_t shown[kRows] = {};
        if (choice != shown[r]) Set(r, choice);
        choice = static_cast<int8_t>(Value(r));
        shown[r] = choice;
    }
    if (id == kFirstId + kComplete && InMission() && Global<void*>(kScriptObject)) {
        dslog::Write("Developer: complete mission");
        reinterpret_cast<void(__cdecl*)()>(kCompleteMission)();
    }
    if (id == kFirstId + kResurrectRow && InMission()) {
        dslog::Write("Developer: resurrect all");
        reinterpret_cast<void(__cdecl*)()>(kResurrect)();
    }
    if (reinterpret_cast<int(__cdecl*)(int)>(kBackPressed)(0)) SetState(kStateCheats);
    return 0;
}

void __fastcall Draw(void* screen, void*) {
    reinterpret_cast<void(__thiscall*)(void*, int, int)>(kDrawPanel)(screen, 6, 0);
    reinterpret_cast<void(__thiscall*)(void*, uint32_t, int)>(kDrawTitle)(screen, kTextTitle, 1);
    void* list = At<void*>(screen, 8);
    reinterpret_cast<void(__thiscall*)(void*)>((*reinterpret_cast<void***>(list))[2])(list);
}

void Build() {
    reinterpret_cast<void(__thiscall*)(void*)>(kOptCtor)(g_screen);
    std::memcpy(g_vtable, reinterpret_cast<void*>(kOptVtable), sizeof g_vtable);
    At<void**>(g_screen, 0) = g_vtable;
    g_vtable[6] = reinterpret_cast<void*>(kMainMenuEnter);  // enter runs the set-up every time
    g_vtable[1] = reinterpret_cast<void*>(&Create);
    g_vtable[3] = reinterpret_cast<void*>(&Setup);
    g_vtable[4] = reinterpret_cast<void*>(&Update);
    g_vtable[5] = reinterpret_cast<void*>(&Draw);
    g_built = Create(g_screen, nullptr);
    dslog::Write(g_built ? "Developer: screen built" : "[fail] Developer: screen not built");
}
}  // namespace

void* features::DeveloperScreen() {
    if (!g_built && Global<void*>(kMainMenuScreen)) Build();
    return g_built ? g_screen : nullptr;
}

const char* features::DeveloperText(uint32_t hash) {
    return hash >= kTextTitle && hash < kTextEnd ? kTexts[hash - kTextTitle] : nullptr;
}
