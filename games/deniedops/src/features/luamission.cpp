// Lua missions on ported levels: a mod level whose <scene stem>.level sidecar has "--@lua <bundle>.lua" runs that
// bundle (the source game's mission logic + a Denied Ops runtime, ModKit ds1todolua) in its own Lua state. Lifetime:
// armed when a new scene index opens (SceneGeneration), started on the first input update after the loading screen
// (SCR hash 0xC0647285) has gone, ended with the next scene. Per frame the bundle's global __do_update() runs.
//
// Natives (global table `dox`):
//   dox.call(slot, ...)  / dox.callf(slot, ...)   cdecl call of script API slot `slot` (byte offset into the table at
//                        0x991F90; e.g. 0x5DC = assign a soldier to a waypoint, 0xCB4 = end mission) with up to 12
//                        arguments (integers, booleans, floats as float32 bits) -> eax as an integer / st0 as a float
//   dox.r32(a) / dox.rf(a) / dox.w32(a, v)       guarded game memory (nil on a fault)
//   dox.obj(script id)   the scene object's address (nil = none)
//   dox.prompt(text)     a line on screen (prompt.cpp) while it keeps being set (UTF-8)
//   dox.time_ms()        milliseconds since the mission started (real time)
//   dox.log(text)        DeniedOpsFix.log line "Lua: ..."
// Script errors are logged once and stop the mission's Lua (the level keeps running).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace {
constexpr uint32_t kInputUpdate = 0x65BAC0, kApiTable = 0x991F90, kApiSize = 0x1000;
constexpr uint32_t kGui = 0xBD0C40, kTopScreen = 0x6FB2E0, kLoadingScreenHash = 0xC0647285;

lua_State* g_L = nullptr;
uint32_t g_generation = 0;
enum class Phase { Idle, Armed, Loading, Running, Stopped } g_phase = Phase::Idle;
LARGE_INTEGER g_start{}, g_freq{};

using UpdateFn = void(__cdecl*)();
UpdateFn g_nextUpdate = nullptr;

bool LoadingScreenUp() {
    void* gui = *reinterpret_cast<void**>(kGui);
    if (!gui) return false;
    void* top = reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(kTopScreen)(gui, nullptr, 4);
    return top && *reinterpret_cast<uint32_t*>(static_cast<char*>(top) + 0x14) == kLoadingScreenHash;
}

// --------------------------------------------------------------------------------------------------- natives
uint32_t Arg(lua_State* L, int i) {
    if (lua_isboolean(L, i)) return lua_toboolean(L, i) ? 1u : 0u;
    if (lua_isinteger(L, i)) return static_cast<uint32_t>(lua_tointeger(L, i));
    if (lua_isnumber(L, i)) {
        float f = static_cast<float>(lua_tonumber(L, i));
        uint32_t u;
        memcpy(&u, &f, 4);
        return u;
    }
    if (lua_isstring(L, i)) return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lua_tostring(L, i)));
    return 0;
}

uint32_t SlotTarget(lua_State* L) {
    const lua_Integer slot = luaL_checkinteger(L, 1);
    if (slot < 0 || slot >= kApiSize || slot % 4) luaL_error(L, "dox.call: bad slot 0x%X", static_cast<unsigned>(slot));
    return *reinterpret_cast<uint32_t*>(kApiTable + static_cast<uint32_t>(slot));
}

using Call12 = uint32_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                  uint32_t, uint32_t, uint32_t, uint32_t);
using Call12F = float(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                uint32_t, uint32_t, uint32_t, uint32_t);

void Args(lua_State* L, uint32_t (&a)[12]) {
    const int n = lua_gettop(L);
    if (n - 1 > 12) luaL_error(L, "dox.call: more than 12 arguments");
    for (int i = 0; i < 12; ++i) a[i] = i + 2 <= n ? Arg(L, i + 2) : 0;
}

int LCall(lua_State* L) {
    const uint32_t fn = SlotTarget(L);
    uint32_t a[12];
    Args(L, a);
    const uint32_t r = reinterpret_cast<Call12>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10],
                                                    a[11]);
    lua_pushinteger(L, static_cast<lua_Integer>(static_cast<int32_t>(r)));
    return 1;
}

int LCallF(lua_State* L) {
    const uint32_t fn = SlotTarget(L);
    uint32_t a[12];
    Args(L, a);
    const float r = reinterpret_cast<Call12F>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10],
                                                  a[11]);
    lua_pushnumber(L, r);
    return 1;
}

bool Readable(uint32_t a, size_t n) {
    MEMORY_BASIC_INFORMATION m{};
    if (!VirtualQuery(reinterpret_cast<void*>(a), &m, sizeof m) || m.State != MEM_COMMIT) return false;
    if (m.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return a + n <= reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize;
}

int LR32(lua_State* L) {
    const uint32_t a = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    if (!Readable(a, 4)) return lua_pushnil(L), 1;
    lua_pushinteger(L, static_cast<lua_Integer>(*reinterpret_cast<int32_t*>(a)));
    return 1;
}

int LRF(lua_State* L) {
    const uint32_t a = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    if (!Readable(a, 4)) return lua_pushnil(L), 1;
    lua_pushnumber(L, *reinterpret_cast<float*>(a));
    return 1;
}

int LW32(lua_State* L) {
    const uint32_t a = static_cast<uint32_t>(luaL_checkinteger(L, 1));
    if (!Readable(a, 4)) return lua_pushboolean(L, 0), 1;
    *reinterpret_cast<uint32_t*>(a) = Arg(L, 2);
    return lua_pushboolean(L, 1), 1;
}

// The scene object with that script id (the lookup every script API slot starts with: thiscall 0x818000 on the
// object manager [0xE55900]) -> address, nil = none
int LObj(lua_State* L) {
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    void* mgr = *reinterpret_cast<void**>(0xE55900);
    void* o = mgr ? reinterpret_cast<void*(__fastcall*)(void*, void*, int)>(0x818000)(mgr, nullptr, id) : nullptr;
    if (!o) return lua_pushnil(L), 1;
    lua_pushinteger(L, static_cast<lua_Integer>(reinterpret_cast<uintptr_t>(o)));
    return 1;
}

int LPrompt(lua_State* L) {
    features::SetPrompt(luaL_checkstring(L, 1));
    return 0;
}

int LTime(lua_State* L) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    lua_pushinteger(L, static_cast<lua_Integer>((now.QuadPart - g_start.QuadPart) * 1000 / g_freq.QuadPart));
    return 1;
}

int LLog(lua_State* L) {
    dslog::Write("Lua: %s", luaL_checkstring(L, 1));
    return 0;
}

// --------------------------------------------------------------------------------------------------- lifetime
void Stop(const char* why) {
    if (g_L) {
        lua_close(g_L);
        g_L = nullptr;
        dslog::Write("Lua mission: stopped (%s)", why);
    }
    g_phase = Phase::Stopped;
}

bool Protected(int nargs, const char* what) {   // function + nargs on the stack
    if (lua_pcall(g_L, nargs, 0, 0) == LUA_OK) return true;
    features::ModLog("[fail] Lua mission: %s: %s", what, lua_tostring(g_L, -1));
    Stop("script error");
    return false;
}

void Start() {
    const std::string bundle = features::SceneLevelOption("lua");
    const std::string path = features::ModFilePath(bundle.c_str());
    if (path.empty()) {
        features::ModLog("[fail] Lua mission: %s not found in the Mods folder", bundle.c_str());
        g_phase = Phase::Stopped;
        return;
    }
    g_L = luaL_newstate();
    luaL_openlibs(g_L);
    static const luaL_Reg kDox[] = {{"call", LCall}, {"callf", LCallF}, {"r32", LR32}, {"rf", LRF}, {"w32", LW32},
                                    {"obj", LObj}, {"prompt", LPrompt}, {"time_ms", LTime}, {"log", LLog}, {nullptr, nullptr}};
    luaL_newlib(g_L, kDox);
    lua_setglobal(g_L, "dox");
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    g_phase = Phase::Running;
    dslog::Write("Lua mission: %s", path.c_str());
    if (luaL_loadfile(g_L, path.c_str()) != LUA_OK) {
        features::ModLog("[fail] Lua mission: %s", lua_tostring(g_L, -1));
        Stop("load error");
        return;
    }
    if (!Protected(0, "start")) return;
    lua_getglobal(g_L, "__do_start");
    if (lua_isfunction(g_L, -1)) Protected(0, "__do_start");
    else lua_pop(g_L, 1);
}

void __cdecl InputUpdate() {
    g_nextUpdate();
    const uint32_t gen = features::SceneGeneration();
    if (gen != g_generation) {   // a new scene: whatever ran belongs to the old level
        g_generation = gen;
        if (g_L) Stop("new scene");
        g_phase = features::SceneLevelOption("lua").empty() ? Phase::Idle : Phase::Armed;
    }
    if (g_phase == Phase::Armed && LoadingScreenUp()) g_phase = Phase::Loading;
    else if (g_phase == Phase::Loading && !LoadingScreenUp()) Start();
    else if (g_phase == Phase::Running && g_L) {
        lua_getglobal(g_L, "__do_update");
        if (lua_isfunction(g_L, -1)) Protected(0, "__do_update");
        else lua_pop(g_L, 1);
    }
}

void Apply() {
    constexpr uint8_t kEntry[] = {0xE8, 0xBB, 0xFD, 0xFF, 0xFF};   // call 0x65B880
    if (!patch::HookFunction(kInputUpdate, reinterpret_cast<void*>(&InputUpdate),
                             reinterpret_cast<void**>(&g_nextUpdate), kEntry, sizeof kEntry))
        dslog::Write("Lua mission: [fail] frame hook");
}
}  // namespace

void features::ApplyLuaMission() { AfterUnpack(&Apply); }
