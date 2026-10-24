// See ds1_events.h. Moved here from DesertStormFix (2026-10-19): the frame call used to be driven by dsfix.framecap
// (pse::Frame inside features::OnFrame), the co-save by dsfix.loadout's save / load call-site hooks, NEW_GAME by
// dsfix.scripthost's level-init hook. DesertStormFix now chains on the same sites (its own priorities, below / above
// the layer's) and subscribes to the events / messages, keeping its order: framecap waits, runs its lockstep /
// determinism work, then calls the layer (tasks, PSE_EVENT_FRAME: other plugins first, DesertStormFix's per-frame work
// last), then the game's frame.
#include "ds1_events.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "ds1_saves.h"
#include "loader.h"
#include "pse/pse_game_ds1.h"

namespace ds1_events {
namespace {   // (inside ds1_events: pse_tests compiles this file into one unit with ds1_saves.cpp)
constexpr int32_t kInner = PSE_DS1_LAYER_PRIORITY_INNER, kOuter = PSE_DS1_LAYER_PRIORITY_OUTER;
constexpr uint32_t kDispatchMode = 0x60EC8C;   // u32: 0 running, 1 level load pending / running, 3 other

PSEPluginHandle g_self = 0;
bool g_installed = false;
int g_failed = 0;

void Log(PSELogLevel level, const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    pse::LogLine(g_self, level, line);
}

const PSEHooksInterface* H() { return pse::HooksInterface(); }

uint32_t Peek32(uint32_t va) {
    __try {
        return *reinterpret_cast<const volatile uint32_t*>(static_cast<uintptr_t>(va));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0xFFFFFFFF;
    }
}

bool IsFrontEnd(const char* level) { return level && _strnicmp(level, "frontend", 8) == 0; }

// ---- frame ---------------------------------------------------------------------------------------------------------
void* g_frameNext = nullptr;
std::string g_level;              // the level Level_Init loaded last ("" = none / unloaded)
bool g_frontEndPending = false;   // a front-end level finished loading: FRONT_END_READY on its first running frame

void __cdecl FrameHook() {
    if (g_frontEndPending && Peek32(kDispatchMode) == 0) {
        g_frontEndPending = false;
        pse::Broadcast(PSE_MSG_FRONT_END_READY, g_level.c_str(), static_cast<uint32_t>(g_level.size() + 1));
    }
    pse::Frame();   // pse.tasks, then PSE_EVENT_FRAME
    reinterpret_cast<void(__cdecl*)()>(g_frameNext)();
}

// ---- render --------------------------------------------------------------------------------------------------------
void* g_renderNext = nullptr;

int __cdecl RenderHook(int a) {
    pse::RaiseEvent(PSE_EVENT_PRE_RENDER, nullptr);
    const int r = reinterpret_cast<int(__cdecl*)(int)>(g_renderNext)(a);
    pse::RaiseEvent(PSE_EVENT_POST_RENDER, nullptr);
    return r;
}

// ---- level init / unload -------------------------------------------------------------------------------------------
void* g_levelInitNext = nullptr;
void* g_unloadNext[2] = {};

// ---- script messages (pse_game_ds1.h "ds1.script_message") ---------------------------------------------------------
// The level's script object [0x60F578] = 6 cdecl callbacks (+0 init, +4 update, +8 message(id, param*), +0xC save,
// +0x10 load, +0x14 size), set during level init by the game (mission DLL's LevelSetup) or DesertStormFix (Lua levels,
// translated missions). Right after level init the layer puts a copy in its place whose message callback raises the
// event and then calls the script's own - once per level, never again mid-level: online mode wraps the current object
// itself (and re-wraps when it changes), so it then wraps this copy and the chain stays one-way.
struct ScriptObject {
    void* fn[6];
};
ScriptObject g_scriptProxy{};
ScriptObject* g_scriptOriginal = nullptr;
PSEEventId g_evScriptMessage = 0;
constexpr uint32_t kScriptObject = 0x60F578;

int __cdecl ScriptMessageProxy(int id, uint32_t* param) {
    PSEDs1ScriptMessageEvent e{sizeof e, static_cast<uint32_t>(id), param};
    pse::RaiseEvent(g_evScriptMessage, &e);
    return reinterpret_cast<int(__cdecl*)(int, uint32_t*)>(g_scriptOriginal->fn[2])(id, param);
}

void WrapScriptObject() {
    auto& slot = *reinterpret_cast<ScriptObject**>(kScriptObject);
    if (!slot || slot == &g_scriptProxy || !g_evScriptMessage) return;
    g_scriptOriginal = slot;
    g_scriptProxy = *slot;
    g_scriptProxy.fn[2] = reinterpret_cast<void*>(&ScriptMessageProxy);
    slot = &g_scriptProxy;
}

int __cdecl LevelInitHook(const char* name) {
    ds1_saves::LevelStart(name);   // NEW_GAME (+ revert callbacks) when a game starts from the front end
    g_level = name ? name : "";
    g_frontEndPending = false;
    pse::RaiseEvent(PSE_EVENT_LEVEL_LOAD_BEGIN, const_cast<char*>(g_level.c_str()));
    const int r = reinterpret_cast<int(__cdecl*)(const char*)>(g_levelInitNext)(name);
    WrapScriptObject();
    pse::RaiseEvent(PSE_EVENT_LEVEL_LOAD_END, const_cast<char*>(g_level.c_str()));
    g_frontEndPending = IsFrontEnd(g_level.c_str());
    return r;
}

int Unload(void* next, bool exit) {
    if (!g_level.empty()) {
        const std::string level = g_level;   // (a subscriber may start something that changes g_level)
        pse::RaiseEvent(PSE_EVENT_LEVEL_UNLOAD, const_cast<char*>(level.c_str()));
    }
    // The game is closing: the plugins get PSE_MSG_EXIT and are unloaded (pse.host Shutdown - once, so a host plugin
    // calling it too is harmless), then the game's own clean-up - as the DS2 layer does
    if (exit)
        if (const PSEHostInterface* host = pse::HostInterface()) host->Shutdown();
    const int r = reinterpret_cast<int(__cdecl*)()>(next)();
    g_level.clear();
    g_frontEndPending = false;
    return r;
}
int __cdecl UnloadHook0() { return Unload(g_unloadNext[0], false); }   // level change (Level_LoadPending)
int __cdecl UnloadHook1() { return Unload(g_unloadNext[1], true); }    // window destroyed (WM_DESTROY 0x411404)

// ---- save / load ---------------------------------------------------------------------------------------------------
void* g_saveNext = nullptr;
void* g_loadNext = nullptr;

int __fastcall SaveHook(void* self, void*, const char* path, int flag) {
    const bool file = path && !flag;   // flag 1 ("DS_auto") / no path: nothing is written
    bool existed = false;
    if (file) {
        existed = GetFileAttributesA(ds1_saves::WithSav(path).c_str()) != INVALID_FILE_ATTRIBUTES;
        ds1_saves::SaveBegin(path);
    }
    const int r = reinterpret_cast<int(__thiscall*)(void*, const char*, int)>(g_saveNext)(self, path, flag);
    if (file) ds1_saves::SaveEnd(path, r != 0, existed);   // every plugin's save callback -> "<name>.pse", SAVE_END
    return r;
}

int __fastcall LoadHook(void* self, void*, const char* path) {
    if (path) ds1_saves::LoadBegin(path);
    const int r = reinterpret_cast<int(__thiscall*)(void*, const char*)>(g_loadNext)(self, path);
    if (path) ds1_saves::LoadEnd(path, r != 0);   // the co-save to the plugins, LOAD_END
    return r;
}

// ---- soldier damage (pse_game_ds1.h "game events") ------------------------------------------------------------------
// Soldier_TakeDamage thiscall(soldier, float amount, object* attacker) ret 8 - every damage of a soldier (the vtable's
// slot 15 = this function; online mode's wrapper on that slot calls it directly, so the entry hook sees what each PC
// really applies). It returns at once for a soldier that is already dead: those calls pass straight through.
void* g_damageNext = nullptr;
PSEEventId g_evDamage = 0, g_evDamaged = 0, g_evKilled = 0;

float Health(const void* soldier) {
    return *reinterpret_cast<const float*>(static_cast<const uint8_t*>(soldier) + 0x54);
}
int32_t ScriptId(const void* object) {
    return object ? *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(object) + 8) : -1;
}

void __fastcall DamageHook(void* soldier, void*, float amount, void* attacker) {
    using Fn = void(__thiscall*)(void*, float, void*);
    const float before = soldier ? Health(soldier) : 0.0f;
    if (!(before > 0.0f)) return reinterpret_cast<Fn>(g_damageNext)(soldier, amount, attacker);
    PSEDs1DamageEvent e{sizeof e, reinterpret_cast<uint32_t>(soldier), ScriptId(soldier),
                        reinterpret_cast<uint32_t>(attacker), ScriptId(attacker), amount, before, before};
    pse::RaiseEvent(g_evDamage, &e);
    if (!(e.amount > 0.0f)) return;   // a subscriber took it away (NaN too)
    const float applied = e.amount;
    reinterpret_cast<Fn>(g_damageNext)(soldier, applied, attacker);
    e = PSEDs1DamageEvent{sizeof e, reinterpret_cast<uint32_t>(soldier), ScriptId(soldier),
                          reinterpret_cast<uint32_t>(attacker), ScriptId(attacker), applied, before, Health(soldier)};
    pse::RaiseEvent(g_evDamaged, &e);
    if (!(e.healthAfter > 0.0f)) pse::RaiseEvent(g_evKilled, &e);
}

// ---- vehicle damage --------------------------------------------------------------------------------------------------
// Vehicle_ApplyDamage thiscall(vehicle, float amount, object* attacker, int, float, int finish) ret 0x14 - the common
// path of every vehicle damage (the vtable's damage slot 0x43F3B0, zone hits, scripts). It applies the damage
// (FUN_0049c630), and at health 0 makes the wreck (flag +0xC 0x400, script message 6). `finish` != 0 = the game's own
// clean-up of a wreck: no before-event (a subscriber must not cancel it).
void* g_vehicleNext = nullptr;
PSEEventId g_evVehDamage = 0, g_evVehDamaged = 0, g_evVehDestroyed = 0;

void __fastcall VehicleDamageHook(void* vehicle, void*, float amount, void* attacker, int a3, float a4, int finish) {
    using Fn = void(__thiscall*)(void*, float, void*, int, float, int);
    const float before = vehicle ? Health(vehicle) : 0.0f;
    if (!(before > 0.0f)) return reinterpret_cast<Fn>(g_vehicleNext)(vehicle, amount, attacker, a3, a4, finish);
    PSEDs1DamageEvent e{sizeof e, reinterpret_cast<uint32_t>(vehicle), ScriptId(vehicle),
                        reinterpret_cast<uint32_t>(attacker), ScriptId(attacker), amount, before, before};
    if (!finish) {
        pse::RaiseEvent(g_evVehDamage, &e);
        if (!(e.amount > 0.0f)) return;
    }
    const float applied = e.amount;
    reinterpret_cast<Fn>(g_vehicleNext)(vehicle, applied, attacker, a3, a4, finish);
    e = PSEDs1DamageEvent{sizeof e, reinterpret_cast<uint32_t>(vehicle), ScriptId(vehicle),
                          reinterpret_cast<uint32_t>(attacker), ScriptId(attacker), applied, before, Health(vehicle)};
    pse::RaiseEvent(g_evVehDamaged, &e);
    if (!(e.healthAfter > 0.0f)) pse::RaiseEvent(g_evVehDestroyed, &e);
}

// ---- pick-ups, objectives, mission end ------------------------------------------------------------------------------
// Soldier_PickUp thiscall(soldier, item) ret 4: the item is his after (item +0x6C = its owner) unless the game refused.
// Objective_Complete = the script API's +0x4C0 function, cdecl(index): the campaign's objective entry (FUN_004586e0)
// gets the done flag +0x14 & 4 (+4 = title hash). Campaign_SetResult thiscall(campaign [0x60EDC0], result) ret 4.
void* g_pickUpNext = nullptr;
void* g_objectiveNext = nullptr;
void* g_missionEndNext = nullptr;
PSEEventId g_evPickUp = 0, g_evObjective = 0, g_evMissionEnd = 0;
constexpr uint32_t kCampaign = 0x60EDC0, kObjectiveByIndex = 0x4586E0;

// Whether the game took the item = AddItem's answer (FUN_004314d0 thiscall(soldier, item, 0, 0) ret 0xC, called at
// 0x432082): non-zero = in the inventory (a stackable item - medikit, ammo - is merged into the stack it already has, so
// the picked object itself never gets him as owner). Recorded by a call-site hook, the event raised after the whole
// pick-up (text, sound, script message 3, held weapon) is done.
void* g_addItemNext = nullptr;
int g_pickUpDepth = 0;
bool g_pickUpTaken = false;
int __fastcall AddItemHook(void* soldier, void*, void* item, int a, int b) {
    const int r = reinterpret_cast<int(__thiscall*)(void*, void*, int, int)>(g_addItemNext)(soldier, item, a, b);
    if (g_pickUpDepth) g_pickUpTaken = r != 0;
    return r;
}

void __fastcall PickUpHook(void* soldier, void*, void* item) {
    ++g_pickUpDepth;
    g_pickUpTaken = false;
    reinterpret_cast<void(__thiscall*)(void*, void*)>(g_pickUpNext)(soldier, item);
    --g_pickUpDepth;
    if (!soldier || !item || !g_pickUpTaken) return;
    PSEDs1PickUpEvent e{sizeof e, reinterpret_cast<uint32_t>(soldier), ScriptId(soldier),
                        reinterpret_cast<uint32_t>(item), ScriptId(item)};
    pse::RaiseEvent(g_evPickUp, &e);
}

uint8_t* Objective(uint32_t index) {
    void* campaign = *reinterpret_cast<void**>(kCampaign);
    if (!campaign) return nullptr;
    return reinterpret_cast<uint8_t*(__thiscall*)(void*, uint32_t)>(kObjectiveByIndex)(campaign, index);
}
void __cdecl ObjectiveHook(uint32_t index) {
    const uint8_t* entry = Objective(index);
    const bool wasDone = entry && (entry[0x14] & 4);
    reinterpret_cast<void(__cdecl*)(uint32_t)>(g_objectiveNext)(index);
    if (!entry || wasDone || !(entry[0x14] & 4)) return;
    // (mission scripts pass the index with garbage in the upper bytes - the game reads the low byte)
    PSEDs1ObjectiveEvent e{sizeof e, index & 0xFF, *reinterpret_cast<const uint32_t*>(entry + 4)};
    pse::RaiseEvent(g_evObjective, &e);
}

void __fastcall MissionEndHook(void* campaign, void*, uint32_t result) {
    reinterpret_cast<void(__thiscall*)(void*, uint32_t)>(g_missionEndNext)(campaign, result);
    PSEDs1MissionEndEvent e{sizeof e, result};
    pse::RaiseEvent(g_evMissionEnd, &e);
}

// ---- D3D8 ----------------------------------------------------------------------------------------------------------
// COM vtables are per class: one broker pointer hook on &vtable[i] catches every object of the class. A class we don't
// know yet (the system d3d8 or DesertStormFix's D3D9 wrapper, src/render) gets its own record with its own `next`
// per slot, so each chain ends at its own original. (DesertStormFix's src/core/comhooks.h is the same idea.)
enum : int { kCreateDevice = 15, kTestCooperativeLevel = 3, kReset = 14, kPresent = 15, kEndScene = 35, kSlots = 40 };
struct ComClass {
    void** vtable = nullptr;
    void* next[kSlots] = {};
    bool hooked[kSlots] = {};
};
ComClass g_d3dClasses[4], g_devClasses[4];

void** Vtable(void* obj) { return *static_cast<void***>(obj); }

ComClass* ClassOf(ComClass (&table)[4], void* obj, bool add) {
    void** vt = Vtable(obj);
    for (ComClass& c : table)
        if (c.vtable == vt) return &c;
    if (!add) return nullptr;
    for (ComClass& c : table)
        if (!c.vtable) {
            c.vtable = vt;
            return &c;
        }
    return nullptr;
}
void* NextOf(ComClass (&table)[4], void* obj, int index) {
    const ComClass* c = ClassOf(table, obj, false);
    return c && c->next[index] ? c->next[index] : Vtable(obj)[index];
}
bool HookCom(ComClass (&table)[4], void* obj, int index, const void* fn, int32_t priority, const char* what) {
    ComClass* c = ClassOf(table, obj, true);
    if (!c) {
        Log(PSE_LOG_ERROR, "[fail] Game layer: too many D3D8 classes - %s not hooked", what);
        return false;
    }
    if (c->hooked[index]) return true;
    PSEHookId id = 0;
    const PSEResult r = H()->HookPointer(g_self, reinterpret_cast<uint32_t>(&c->vtable[index]), const_cast<void*>(fn),
                                         priority, &c->next[index], &id);
    c->hooked[index] = r == PSE_OK && c->next[index];
    if (!c->hooked[index]) Log(PSE_LOG_ERROR, "[fail] Game layer: %s hook refused (%d)", what, static_cast<int>(r));
    return c->hooked[index];
}

bool g_deviceLost = false;
constexpr HRESULT kDeviceLost = static_cast<HRESULT>(0x88760868), kDeviceNotReset = static_cast<HRESULT>(0x88760869);

HRESULT __stdcall TestCooperativeLevel(void* dev) {
    const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*)>(NextOf(g_devClasses, dev, kTestCooperativeLevel))(dev);
    if ((hr == kDeviceLost || hr == kDeviceNotReset) && !g_deviceLost) {
        g_deviceLost = true;
        pse::Broadcast(PSE_MSG_DEVICE_LOST, dev, 0);
    } else if (hr == 0) {
        g_deviceLost = false;
    }
    return hr;
}

HRESULT __stdcall Reset(void* dev, void* pp) {
    const HRESULT hr = reinterpret_cast<HRESULT(__stdcall*)(void*, void*)>(NextOf(g_devClasses, dev, kReset))(dev, pp);
    if (SUCCEEDED(hr)) {
        g_deviceLost = false;
        pse::Broadcast(PSE_MSG_DEVICE_RESET, dev, 0);
    }
    return hr;
}

HRESULT __stdcall EndScene(void* dev) {
    pse::RaiseEvent(PSE_EVENT_END_SCENE, dev);
    return reinterpret_cast<HRESULT(__stdcall*)(void*)>(NextOf(g_devClasses, dev, kEndScene))(dev);
}

HRESULT __stdcall Present(void* dev, const RECT* src, const RECT* dst, HWND wnd, const void* dirty) {
    pse::RaiseEvent(PSE_EVENT_PRESENT, dev);
    using Fn = HRESULT(__stdcall*)(void*, const RECT*, const RECT*, HWND, const void*);
    return reinterpret_cast<Fn>(NextOf(g_devClasses, dev, kPresent))(dev, src, dst, wnd, dirty);
}

HRESULT __stdcall CreateDevice(void* d3d, UINT adapter, uint32_t type, HWND wnd, DWORD flags, void* pp, void** out) {
    using Fn = HRESULT(__stdcall*)(void*, UINT, uint32_t, HWND, DWORD, void*, void**);
    const HRESULT hr = reinterpret_cast<Fn>(NextOf(g_d3dClasses, d3d, kCreateDevice))(d3d, adapter, type, wnd, flags, pp, out);
    if (FAILED(hr) || !out || !*out) return hr;
    void* dev = *out;
    bool ok = HookCom(g_devClasses, dev, kTestCooperativeLevel, &TestCooperativeLevel, kInner, "TestCooperativeLevel");
    ok = HookCom(g_devClasses, dev, kReset, &Reset, kOuter, "Reset") && ok;
    ok = HookCom(g_devClasses, dev, kEndScene, &EndScene, kInner, "EndScene") && ok;
    ok = HookCom(g_devClasses, dev, kPresent, &Present, kInner, "Present") && ok;
    if (!ok) Log(PSE_LOG_ERROR, "[fail] Game layer: some D3D device events are missing");
    g_deviceLost = false;
    pse::Broadcast(PSE_MSG_DEVICE_CREATED, dev, 0);
    return hr;
}

void* g_create8Next = nullptr;

void* WINAPI Direct3DCreate8Hook(UINT sdk) {
    void* d3d = reinterpret_cast<void*(WINAPI*)(UINT)>(g_create8Next)(sdk);
    if (d3d) HookCom(g_d3dClasses, d3d, kCreateDevice, &CreateDevice, kOuter, "IDirect3D8::CreateDevice");
    return d3d;
}

// ---- installation --------------------------------------------------------------------------------------------------
bool Check(PSEResult r, const char* what, uint32_t va) {
    if (r == PSE_OK) return true;
    Log(PSE_LOG_ERROR, "[fail] Game layer: %s hook at 0x%08X refused (%d) - that event is missing", what, va,
        static_cast<int>(r));
    ++g_failed;
    return false;
}
}  // namespace

int Install(PSEPluginHandle self) {
    if (g_installed) return g_failed;
    g_installed = true;
    g_self = self;
    g_failed = 0;
    ds1_saves::SetOwner(self);
    const PSEHooksInterface* h = H();
    PSEHookId id = 0;
    Check(h->HookCallSite(self, kFrameCall, kFrameDispatch, reinterpret_cast<void*>(&FrameHook), kInner, &g_frameNext,
                          &id), "frame", kFrameCall);
    Check(h->HookCallSite(self, kRenderCall, kRenderFrame, reinterpret_cast<void*>(&RenderHook), kInner, &g_renderNext,
                          &id), "render", kRenderCall);
    Check(h->HookCallSite(self, kLevelInitCall, kLevelInit, reinterpret_cast<void*>(&LevelInitHook), kOuter,
                          &g_levelInitNext, &id), "level init", kLevelInitCall);
    void* const unloadHooks[] = {reinterpret_cast<void*>(&UnloadHook0), reinterpret_cast<void*>(&UnloadHook1)};
    for (int i = 0; i < 2; ++i)
        Check(h->HookCallSite(self, kUnloadCalls[i], kLevelUnload, unloadHooks[i], kOuter, &g_unloadNext[i], &id),
              "level unload", kUnloadCalls[i]);
    Check(h->HookFunction(self, kSaveGame, sizeof kSavePrologue, kSavePrologue, reinterpret_cast<void*>(&SaveHook),
                          kOuter, &g_saveNext, &id), "save", kSaveGame);
    Check(h->HookFunction(self, kLoadGame, sizeof kLoadPrologue, kLoadPrologue, reinterpret_cast<void*>(&LoadHook),
                          kOuter, &g_loadNext, &id), "load", kLoadGame);
    const PSEMessagingInterface* m = pse::MessagingInterface();
    g_evDamage = m->EventId(PSE_DS1_EVENT_SOLDIER_DAMAGE);
    g_evDamaged = m->EventId(PSE_DS1_EVENT_SOLDIER_DAMAGED);
    g_evKilled = m->EventId(PSE_DS1_EVENT_SOLDIER_KILLED);
    Check(h->HookFunction(self, kTakeDamage, sizeof kTakeDamagePrologue, kTakeDamagePrologue,
                          reinterpret_cast<void*>(&DamageHook), kOuter, &g_damageNext, &id), "soldier damage",
          kTakeDamage);
    g_evVehDamage = m->EventId(PSE_DS1_EVENT_VEHICLE_DAMAGE);
    g_evVehDamaged = m->EventId(PSE_DS1_EVENT_VEHICLE_DAMAGED);
    g_evVehDestroyed = m->EventId(PSE_DS1_EVENT_VEHICLE_DESTROYED);
    g_evPickUp = m->EventId(PSE_DS1_EVENT_ITEM_PICKED_UP);
    g_evObjective = m->EventId(PSE_DS1_EVENT_OBJECTIVE_COMPLETED);
    g_evMissionEnd = m->EventId(PSE_DS1_EVENT_MISSION_END);
    g_evScriptMessage = m->EventId(PSE_DS1_EVENT_SCRIPT_MESSAGE);
    Check(h->HookFunction(self, kVehicleDamage, sizeof kVehicleDamagePrologue, kVehicleDamagePrologue,
                          reinterpret_cast<void*>(&VehicleDamageHook), kOuter, &g_vehicleNext, &id), "vehicle damage",
          kVehicleDamage);
    Check(h->HookFunction(self, kPickUp, sizeof kPickUpPrologue, kPickUpPrologue, reinterpret_cast<void*>(&PickUpHook),
                          kOuter, &g_pickUpNext, &id), "pick-up", kPickUp);
    Check(h->HookCallSite(self, kPickUpAddItemCall, kAddItem, reinterpret_cast<void*>(&AddItemHook), kOuter,
                          &g_addItemNext, &id), "pick-up AddItem", kPickUpAddItemCall);
    Check(h->HookFunction(self, kObjectiveComplete, sizeof kObjectivePrologue, kObjectivePrologue,
                          reinterpret_cast<void*>(&ObjectiveHook), kOuter, &g_objectiveNext, &id), "objective",
          kObjectiveComplete);
    Check(h->HookFunction(self, kMissionEnd, sizeof kMissionEndPrologue, kMissionEndPrologue,
                          reinterpret_cast<void*>(&MissionEndHook), kOuter, &g_missionEndNext, &id), "mission end",
          kMissionEnd);
    // The game's Direct3DCreate8 import (called through the thunk 0x59B246); DesertStormFix's graphics hook chains
    // behind this one (its wrapper object for Renderer 1-3 is what the layer then sees).
    if (Peek32(kIatDirect3DCreate8) == 0 || Peek32(kIatDirect3DCreate8) == 0xFFFFFFFF) {
        Log(PSE_LOG_ERROR, "[fail] Game layer: Direct3DCreate8 import slot 0x%08X is empty - no D3D events",
            kIatDirect3DCreate8);
        ++g_failed;
    } else {
        Check(h->HookPointer(self, kIatDirect3DCreate8, reinterpret_cast<void*>(&Direct3DCreate8Hook), kOuter,
                             &g_create8Next, &id), "Direct3DCreate8", kIatDirect3DCreate8);
    }
    Log(g_failed ? PSE_LOG_ERROR : PSE_LOG_INFO, "Game layer: %s (frame, render, level, save / load, game events, D3D)",
        g_failed ? "some event hooks failed" : "event hooks installed");
    return g_failed;
}

void ResetForTests() {
    g_installed = false;
    g_failed = 0;
    g_level.clear();
    g_frontEndPending = false;
    g_deviceLost = false;
    for (ComClass& c : g_d3dClasses) c = ComClass{};
    for (ComClass& c : g_devClasses) c = ComClass{};
}
}  // namespace ds1_events
