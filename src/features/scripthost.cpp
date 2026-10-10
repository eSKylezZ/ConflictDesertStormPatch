// Lua mission scripts: a level whose name ends in ".lua" is run by an embedded Lua 5.4 instead of a MissionN.dll.
//
// How the game runs a level script: level init FUN_0040c090 checks the level name's last 3 letters (0x40C3D8): "dll"
// -> FUN_00449260 LoadLibrary + LevelSetup(&api table 0x5ED238) -> script object [0x60F578] (module [0x60F57C]);
// anything else -> FUN_00449190 (thiscall [0x60ED9C], name) = Pivotal's text level manifest (BEGIN, //, SOUND (stub),
// ENVIRONMENT -> FUN_0040b580, ANIM -> FUN_00448d20, INCLUDE, END; no script object). The data archive is the level
// name with its last 4 characters replaced by ".dat" - "mission1.lua" uses mission1.dat.
//
// The script API table (566 cdecl functions, 0x8D8 bytes; LevelSetup checks it with api[0](1, 7, 0x8D8)) is what
// MissionN.dll calls: e.g. +0x04 animation (state, .prs), +0x0C sound bank, +0x44 scene (.env), +0x4C map image,
// +0x50 ?, +0x54 level bounds (float[3] min, float[3] max), +0x58 strings (.loc). The script object is 6 cdecl
// callbacks: +0 init (only LevelSetup calls it), +4 update() per frame (0x40F95D), +8 message(id, param*) -> 1
// (71 senders, ids 0x00-0x53; 0x12 = mission start set-up, 0x3B = overview, 0x11 = mission complete), +0xC save(buffer)
// -> buffer + size, +0x10 load(buffer) -> buffer + size, +0x14 () -> save block size.
//
// The hook replaces the text-manifest call at 0x40C405: ".lua" levels are read through the game's file loader
// (FUN_004bfb90, so Mods\ files win), the standard animation table is registered (generated/standard_anims.h), the
// chunk runs and its init() is called (game.load_scene adds the ladder states like the DLLs: only when API +0x70 says
// the scene has ladders - their .prs files exist only in those levels' archives); the script object then forwards update / on_message into Lua. Lua gets base,
// string, table, math, utf8 and coroutine only (no io / os / package / debug).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <cmath>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
#include "../../third_party/lua/lualib.h"
}

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "generated/standard_anims.h"

namespace {
constexpr uint32_t kTextLevelCall = 0x40C405;  // call FUN_00449190 (thiscall loader [0x60ED9C], name)
constexpr uint32_t kTextLevelLoad = 0x449190;
constexpr uint32_t kScriptObject = 0x60F578;
constexpr uint32_t kApiTable = 0x5ED238;
constexpr uint32_t kApiSize = 0x8D8;
constexpr uint32_t kReadFile = 0x4BFB90;  // cdecl(name, char** buf, int* size, int, int type) -> 1 ok
constexpr uint32_t kFree = 0x4B8590;      // cdecl(memory)

lua_State* g_lua = nullptr;
std::string g_name;
bool g_failed = false;  // a callback raised an error: the script stays loaded but is no longer called
// game.state as serialised by the last size() call, so save() writes exactly the block size() reported. Any other
// call into Lua may change the state, so CallGlobal drops it.
std::string g_saveText;
bool g_saveTextValid = false;

uint32_t ApiFunction(uint32_t slot) { return *reinterpret_cast<uint32_t*>(kApiTable + slot); }

void Report(const char* what, lua_State* L) {
    features::ModLog("[fail] script %s: %s: %s", g_name.c_str(), what, lua_tostring(L, -1));
    lua_pop(L, 1);
}

int Traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

// Calls global `name` with `nargs` arguments already pushed; leaves `nres` results. False on error (reported).
bool CallGlobal(const char* name, int nargs, int nres) {
    lua_State* L = g_lua;
    if (strcmp(name, "_save_state") != 0) g_saveTextValid = false;
    lua_getglobal(L, name);
    if (!lua_isfunction(L, -1)) {
        lua_pop(L, 1 + nargs);
        return false;
    }
    lua_insert(L, -1 - nargs);
    lua_pushcfunction(L, Traceback);
    lua_insert(L, -2 - nargs);
    const int base = lua_gettop(L) - nargs - 1;
    if (lua_pcall(L, nargs, nres, base) != LUA_OK) {
        Report(name, L);
        lua_pop(L, 1);  // traceback function
        g_failed = true;
        return false;
    }
    lua_remove(L, base);
    return true;
}

// --- base mission ("--@base Mission1.dll") ---------------------------------------------------------------------------
// An editor mission made from a shipped one keeps that mission's logic: the base DLL is loaded the game's own way
// (FUN_00449260: LoadLibrary + LevelSetup(&api) -> its script object, module in [0x60F57C] so the game frees it at the
// level's end) with its scene load (API +0x44) pointed at this mission's scene. Its object then gets every callback
// before Lua: Foley's cell, the cutscene actors, triggers, objectives and cutscenes all run as in the original, and the
// Lua script adds to it. The scene keeps the base objects' script ids (the editor gives new objects new ones).
struct ScriptObject {
    void* fn[6];
};
ScriptObject* g_base = nullptr;
std::string g_scene;  // the mission's own scene for the base DLL's load_scene

template <typename R, typename... A>
R BaseCall(int slot, A... args) {
    return reinterpret_cast<R(__cdecl*)(A...)>(g_base->fn[slot])(args...);
}

// --- script object (what the game calls) ----------------------------------------------------------------------------
void __cdecl ScriptInit() {}
int g_updates = 0;                // per level: the first call is logged
bool g_loadingSave = false;       // the level was started by LOAD GAME (API +0x3F4 at init)
void __cdecl ScriptUpdate() {
    if (g_updates++ == 0) dslog::Write("Script: %s - first update", g_name.c_str());
    if (g_base) BaseCall<void>(1);
    if (g_lua && !g_failed) CallGlobal("_frame", 0, 0);  // prelude: region ENTERED / LEFT events
    if (g_lua && !g_failed) CallGlobal("update", 0, 0);
}
// Messages that ask the script something: the game reads param[slot] back after the call (the sender pre-fills its
// own default). `permissive` = our default when the script doesn't answer - the game's own is "refused" for these,
// which made pick-ups, drops, vehicle inventory and squad switching impossible (Mission1.dll allows them).
// 0x2D (hacking kit allowed) and 0x53 (mission-end menu) use the callback's return value instead.
struct Query {
    int id, slot;
    int permissive;  // -1 = keep the game's default
};
constexpr Query kQueries[] = {
    {0x0F, 0, -1}, {0x1A, 2, -1}, {0x26, 1, -1}, {0x27, 1, -1}, {0x28, 2, 1},  {0x29, 2, 1},  {0x2A, 2, 1},
    {0x2B, 0, 1},  {0x2C, 1, -1}, {0x33, 3, -1}, {0x37, 0, -1}, {0x3D, 0, -1}, {0x43, 1, -1}, {0x44, 2, -1},
    {0x45, 2, -1}, {0x46, 1, -1}, {0x47, 1, -1}, {0x49, 1, -1}, {0x4C, 2, -1}, {0x4E, 1, -1}, {0x4F, 1, -1},
};

// message(id, param*): Lua on_message(id, a, b, c, d, e, param) gets the first five param dwords as integers (nil
// without a param; game.float(n) reinterprets one as a float). For a query its result (integer / boolean) is the
// answer; otherwise it is ignored.
int __cdecl ScriptMessage(int id, uint32_t* param) {
    const Query* query = nullptr;
    for (const auto& q : kQueries)
        if (q.id == id) query = &q;
    int result = id == 0x2D ? 1 : id == 0x53 ? 0 : 1;
    if (g_base) result = BaseCall<int>(2, id, param);      // the base mission answers its own queries
    else if (query && param && query->permissive >= 0) param[query->slot] = static_cast<uint32_t>(query->permissive);
    if (!g_lua || g_failed) return result;
    lua_State* L = g_lua;
    if (id == 0x16 && param) {                // re-arm a repeating Lua timer before the script hears about it
        lua_pushinteger(L, static_cast<int32_t>(param[0]));
        CallGlobal("_timer_fired", 1, 0);
    }
    lua_pushinteger(L, id);
    for (int i = 0; i < 5; ++i) {
        if (param) lua_pushinteger(L, static_cast<int32_t>(param[i]));  // the sender's stack: always readable
        else lua_pushnil(L);
    }
    lua_pushinteger(L, reinterpret_cast<uintptr_t>(param));
    if (!CallGlobal("on_message", 7, 1)) return result;
    if (lua_isinteger(L, -1) || lua_isboolean(L, -1)) {
        const int answer = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : static_cast<int>(lua_tointeger(L, -1));
        if (query && param) param[query->slot] = static_cast<uint32_t>(answer);
        else if (id == 0x2D || id == 0x53) result = answer;
    }
    lua_pop(L, 1);
    return result;
}
// Save games: the game writes its sections one after another into a fixed buffer (saver +0x98, 0x2AF53 + objects
// bytes, the whole buffer goes to the .sav – 176 715 B) and passes the cursor to save(buf) / load(buf), which return
// it advanced past the script's block; size() > 0 enables SAVE GAME at all (0x472008). Shipped levels leave >= 5.4 KB
// of the buffer unused (Mission 3; Mission 1: 49 KB), so the block is capped at 2 KB rather than growing the buffer
// (which would change the file layout). Block = "LUA1", u32 length, Lua text from game._save() (game.state).
constexpr uint32_t kSaveMagic = 0x3141554C;  // "LUA1"
constexpr size_t kSaveMax = 2048;

std::string SaveText() {
    if (!g_lua || g_failed) return {};
    if (!CallGlobal("_save_state", 0, 1)) return {};
    std::string text = lua_isstring(g_lua, -1) ? lua_tostring(g_lua, -1) : "";
    lua_pop(g_lua, 1);
    if (text.size() > kSaveMax) {
        features::ModLog("[fail] script %s: game.state is %u bytes when saved (max %u) - saved empty", g_name.c_str(),
                         static_cast<unsigned>(text.size()), static_cast<unsigned>(kSaveMax));
        text.clear();
    }
    return text;
}
// size() and save() must agree: size() serialises once and keeps the text, save() takes it (or serialises itself
// when no size() came first).
const std::string& CachedSaveText() {
    if (!g_saveTextValid) {
        g_saveText = SaveText();
        g_saveTextValid = true;
    }
    return g_saveText;
}
void* __cdecl ScriptSave(void* buffer) {
    if (g_base) buffer = BaseCall<void*>(3, buffer);       // the base mission's block first, then Lua's
    const std::string text = CachedSaveText();
    g_saveText.clear();
    g_saveTextValid = false;
    auto* p = static_cast<uint8_t*>(buffer);
    const uint32_t header[2] = {kSaveMagic, static_cast<uint32_t>(text.size())};
    memcpy(p, header, 8);
    memcpy(p + 8, text.data(), text.size());
    dslog::Write("Script: saved %u bytes of game.state", header[1]);
    return p + 8 + text.size();
}
void* __cdecl ScriptLoad(void* buffer) {
    if (g_base) buffer = BaseCall<void*>(4, buffer);
    auto* p = static_cast<uint8_t*>(buffer);
    uint32_t header[2];
    memcpy(header, p, 8);
    if (header[0] != kSaveMagic || header[1] > kSaveMax) {
        features::ModLog("[fail] script %s: save game has no Lua state block", g_name.c_str());
        return p;
    }
    if (g_lua && !g_failed) {
        lua_pushlstring(g_lua, reinterpret_cast<const char*>(p + 8), header[1]);
        CallGlobal("_load_state", 1, 0);
        dslog::Write("Script: loaded %u bytes of game.state", header[1]);
    }
    return p + 8 + header[1];
}
int __cdecl ScriptSaveSize() {
    return (g_base ? BaseCall<int>(5) : 0) + static_cast<int>(8 + CachedSaveText().size());
}

ScriptObject g_object = {{reinterpret_cast<void*>(&ScriptInit), reinterpret_cast<void*>(&ScriptUpdate),
               reinterpret_cast<void*>(&ScriptMessage), reinterpret_cast<void*>(&ScriptSave),
               reinterpret_cast<void*>(&ScriptLoad), reinterpret_cast<void*>(&ScriptSaveSize)}};

// --- the `game` table -----------------------------------------------------------------------------------------------
// Lua value -> one cdecl argument dword: integers as int32, floats as float bits, strings as char*, booleans 0/1.
uint32_t ArgOf(lua_State* L, int i) {
    switch (lua_type(L, i)) {
    case LUA_TNUMBER:
        if (lua_isinteger(L, i)) return static_cast<uint32_t>(lua_tointeger(L, i));
        {
            const float f = static_cast<float>(lua_tonumber(L, i));
            uint32_t bits;
            memcpy(&bits, &f, 4);
            return bits;
        }
    case LUA_TSTRING: return reinterpret_cast<uint32_t>(lua_tostring(L, i));  // valid while it stays on the stack
    case LUA_TBOOLEAN: return lua_toboolean(L, i) ? 1 : 0;
    case LUA_TNIL: return 0;
    default: return static_cast<uint32_t>(luaL_error(L, "argument %d: unsupported type %s", i, luaL_typename(L, i)));
    }
}

uint32_t CheckSlot(lua_State* L) {
    const lua_Integer slot = luaL_checkinteger(L, 1);
    if (slot < 4 || slot >= kApiSize || slot % 4) luaL_error(L, "bad API slot 0x%X", static_cast<unsigned>(slot));
    return static_cast<uint32_t>(slot);
}

// game.call(slot, ...) -> integer (eax); game.callf(slot, ...) -> number (st0). Up to 12 arguments; every API
// function is cdecl, so the extra dwords are harmless (the caller pops them).
template <class R>
R CallSlot(lua_State* L, uint32_t slot) {
    const int n = lua_gettop(L) - 1;
    if (n > 12) luaL_error(L, "too many arguments (%d, max 12)", n);
    uint32_t a[12] = {};
    for (int i = 0; i < n; ++i) a[i] = ArgOf(L, i + 2);
    using Fn = R(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t);
    return reinterpret_cast<Fn>(ApiFunction(slot))(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10],
                                                    a[11]);
}
// ---- texts, messages, prisoners (2026-10-09, the spy mission) ----
// Script texts: game.text(name, text) gives a catalog hash (0xC1xxxxxx, from the name - the same every run, so saved
// objectives find their text again) that the game's text lookup answers (ingame_input.cpp Lookup -> ScriptText).
std::unordered_map<uint32_t, std::string> g_texts;
// game.set_string(key, text): one of the LEVEL's own texts replaced by its key (e.g. the loading screen's SECTIONHDR /
// SECTIONDATE / SECTIONTEXT / SECTIONTEXT2, looked up by name through FUN_004f5ed0 on [0x60EDCC]: entries {+0 hash,
// +8 char* key} x [+0x4C] at [+0x40], loaded by the strings .loc). Keys are matched against the table whenever it
// changes, so they may be set before the table is loaded.
std::vector<std::pair<std::string, std::string>> g_namedTexts;
std::unordered_map<uint32_t, std::string> g_namedHashes;
const void* g_namedTable = nullptr;
uint32_t g_namedCount = 0;

void ResolveNamedTexts() {
    auto* strings = *reinterpret_cast<uint8_t**>(0x60EDCC);
    const auto* table = strings ? *reinterpret_cast<uint8_t**>(strings + 0x40) : nullptr;
    const uint32_t count = strings ? *reinterpret_cast<uint32_t*>(strings + 0x4C) : 0;
    if (table == g_namedTable && count == g_namedCount) return;
    g_namedTable = table, g_namedCount = count;
    g_namedHashes.clear();
    if (!table) return;
    for (uint32_t i = 0; i < count; ++i) {
        const char* key = *reinterpret_cast<const char* const*>(table + i * 12 + 8);
        if (!key) continue;
        for (const auto& [name, text] : g_namedTexts)
            if (_stricmp(key, name.c_str()) == 0) g_namedHashes[*reinterpret_cast<const uint32_t*>(table + i * 12)] = text;
    }
}
uint32_t TextHash(const char* name) {
    uint32_t h = 2166136261u;
    for (const char* c = name; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    return 0xC1000000u | (h & 0x00FFFFFFu);
}
int LuaSetString(lua_State* L) {
    const std::string key = luaL_checkstring(L, 1), text = luaL_checkstring(L, 2);
    for (auto& entry : g_namedTexts)
        if (_stricmp(entry.first.c_str(), key.c_str()) == 0) {
            entry.second = text;
            g_namedTable = nullptr;   // re-resolve
            return 0;
        }
    g_namedTexts.emplace_back(key, text);
    g_namedTable = nullptr;
    return 0;
}

int LuaText(lua_State* L) {
    const uint32_t hash = TextHash(luaL_checkstring(L, 1));
    g_texts[hash] = luaL_checkstring(L, 2);
    lua_pushinteger(L, hash);
    return 1;
}
// game._popup(text, ms): the pop-up bar at the bottom (FUN_004d33a0 thiscall(view controller [0x63C98C], text, ms) -
// picked-up items, warnings). The game keeps the pointer: texts live in a ring.
int LuaPopup(lua_State* L) {
    static std::string ring[16];
    static int next = 0;
    std::string& slot = ring[next++ % 16];
    slot = luaL_checkstring(L, 1);
    void* controller = *reinterpret_cast<void**>(0x63C98C);
    if (controller)
        reinterpret_cast<void(__thiscall*)(void*, const char*, int)>(0x4D33A0)(controller, slot.c_str(),
                                                                              static_cast<int>(luaL_optinteger(L, 2, 4000)));
    return 0;
}
// game._clear_skin(id): drop a soldier's skin texture override (render instance +0x2B0 via FUN_00503290(inst, 0)) -
// API +0x258 sets one (the prisoner uniform "AlliedPOW.bmp").
int LuaClearSkin(lua_State* L) {
    void* mgr = *reinterpret_cast<void**>(0x63C9A0);
    void* obj = mgr ? reinterpret_cast<void*(__thiscall*)(void*, int)>(0x4CA660)(mgr, static_cast<int>(luaL_checkinteger(L, 1)))
                    : nullptr;
    void* inst = obj ? *reinterpret_cast<void**>(static_cast<char*>(obj) + 0x2C) : nullptr;
    if (inst) reinterpret_cast<void(__thiscall*)(void*, void*)>(0x503290)(inst, nullptr);
    return 0;
}

// game.keep_awake(id, on): enemy soldiers the player hasn't seen go dormant. Each AI turn FUN_00428110 (fastcall
// soldier, one caller 0x42BBD8 in FUN_0042bb80, non-player teams only) sets soldier +0x280 when +0x17C != 0 and the
// soldier was never drawn / not for 60 s / is beyond fog + 10 m; the gate FUN_00428250 then skips its brain and mover,
// so a guard's patrol route stands still until the player comes near (verified: Mission 3 guards +0x280 = 1, a walking
// goatherd 0). +0x17C can't be cleared from outside (the engine sets it again within seconds), so the call is hooked:
// soldiers on this list are always awake. The list is per level (soldier pointers; cleared when a Lua level starts).
constexpr uint32_t kDormancyCall = 0x42BBD8, kDormancy = 0x428110;
std::vector<uint8_t*> g_awake;

int __fastcall Dormancy(uint8_t* soldier) {
    for (uint8_t* s : g_awake)
        if (s == soldier) {
            *reinterpret_cast<int*>(soldier + 0x280) = 0;
            return 0;
        }
    return reinterpret_cast<int(__fastcall*)(uint8_t*)>(kDormancy)(soldier);
}

bool IsSoldier(const uint8_t* obj) {
    __try {
        return obj && *reinterpret_cast<const int*>(obj + 4) == 1;   // object type 1 = soldier
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int LuaKeepAwake(lua_State* L) {
    static const bool hooked = patch::HookCall(kDormancyCall, reinterpret_cast<const void*>(&Dormancy), kDormancy);
    void* mgr = *reinterpret_cast<void**>(0x63C9A0);
    auto* obj = mgr ? static_cast<uint8_t*>(reinterpret_cast<void*(__thiscall*)(void*, int)>(0x4CA660)(
                          mgr, static_cast<int>(luaL_checkinteger(L, 1))))
                    : nullptr;
    const bool on = lua_isnoneornil(L, 2) || lua_toboolean(L, 2);
    const bool soldier = IsSoldier(obj);
    if (hooked && soldier) {
        auto it = std::find(g_awake.begin(), g_awake.end(), obj);
        if (on && it == g_awake.end()) g_awake.push_back(obj);
        if (!on && it != g_awake.end()) g_awake.erase(it);
    }
    lua_pushboolean(L, hooked && soldier);
    return 1;
}

int LuaCall(lua_State* L) {
    const uint32_t slot = CheckSlot(L);
    lua_pushinteger(L, static_cast<int32_t>(CallSlot<uint32_t>(L, slot)));
    return 1;
}
int LuaCallF(lua_State* L) {
    const uint32_t slot = CheckSlot(L);
    lua_pushnumber(L, CallSlot<float>(L, slot));
    return 1;
}

// game.set_level_bounds(min_x, min_y, min_z, max_x, max_y, max_z): API +0x54 (float[3]* min, float[3]* max).
int LuaLevelBounds(lua_State* L) {
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        lo[i] = static_cast<float>(luaL_checknumber(L, 1 + i));
        hi[i] = static_cast<float>(luaL_checknumber(L, 4 + i));
    }
    reinterpret_cast<void(__cdecl*)(float*, float*)>(ApiFunction(0x54))(lo, hi);
    return 0;
}

// game.read32(address) / game.readf(address): reads game memory (message parameters); nil if unreadable.
template <class T>
int LuaRead(lua_State* L) {
    const auto address = static_cast<uintptr_t>(luaL_checkinteger(L, 1));
    T value{};
    __try {
        value = *reinterpret_cast<const T*>(address);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    if constexpr (sizeof(T) == 4 && static_cast<T>(0.5) != 0) lua_pushnumber(L, value);
    else lua_pushinteger(L, static_cast<lua_Integer>(value));
    return 1;
}

// game._next_mission(): the mission after this level in its custom campaign (campaigns.cpp), else "FrontEnd.dll".
// ---- loadout rules (customise.cpp) ----
// game.kit(id, {uniform = "TEXTURE", main = "-", sidearm = "US_WPN_SIGP228", launcher = "-", grenades = "-",
//               lock = true}) - the soldier's uniform / kit at the start (the loadout screen shows it, locked rows
// can't be changed); after the start it applies at once. game.briefing(text) - the BRIEFING tab's text.
// game.loadout(false) - no loadout screen (the kits still apply).
std::string Field(lua_State* L, int t, const char* name) {
    lua_getfield(L, t, name);
    std::string v = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
    lua_pop(L, 1);
    return v;
}
int LuaKit(lua_State* L) {
    const int id = static_cast<int>(luaL_checkinteger(L, 1));
    luaL_checktype(L, 2, LUA_TTABLE);
    const std::string slots[4] = {Field(L, 2, "main"), Field(L, 2, "sidearm"), Field(L, 2, "launcher"), Field(L, 2, "grenades")};
    lua_getfield(L, 2, "lock");
    const bool lock = lua_isnil(L, -1) || lua_toboolean(L, -1);
    lua_pop(L, 1);
    features::SetScriptKit(id, Field(L, 2, "uniform"), slots, lock);
    if (g_updates > 0) features::ApplyScriptKitNow(id);
    return 0;
}
int LuaBriefing(lua_State* L) {
    features::SetScriptBriefing(luaL_checkstring(L, 1));
    return 0;
}
int LuaLoadout(lua_State* L) {
    features::SetScriptLoadout(lua_toboolean(L, 1) != 0);
    return 0;
}

int LuaNextMission(lua_State* L) {
    lua_pushstring(L, features::CampaignNext(g_name.c_str()));
    return 1;
}

// game._ladder_anims(): the ladder states MissionN.dll registers after its scene when API +0x70 is non-zero.
int LuaLadderAnims(lua_State*) {
    for (const auto& anim : generated::kLadderAnims)
        reinterpret_cast<int(__cdecl*)(const char*, const char*)>(ApiFunction(0x04))(anim.state, anim.file);
    return 0;
}

int LuaLog(lua_State* L) {
    std::string line;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        if (i > 1) line += ' ';
        line += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    dslog::Write("[script] %s: %s", g_name.c_str(), line.c_str());
    return 0;
}

// Named helpers over game.call (the API slots known so far).
constexpr const char* kPrelude = R"lua(
local call = game.call
function game.anim(state, file) return call(0x04, state, file) end
function game.load_sound_bank(file) return call(0x0C, file) end
function game.load_scene(file)                -- + the ladder animations when the scene has ladders (API +0x70)
    local ok = call(0x44, file)
    if call(0x70) ~= 0 then game._ladder_anims() end
    return ok
end
function game.load_map_image(file) return call(0x4C, file) end
function game.load_strings(file) return call(0x58, file) end
function game.set_time_of_day(hour) return call(0x78, hour + 0.0) end
function game.get_time_of_day() return game.callf(0x7C) end
-- briefing texts (catalog hashes): slot 0 title, 1 date, 2 place, 3 and up the next free story page (API +0x4E0 =
-- FUN_00458770 -> campaign +0x12EC..; the mission DLLs' set-up calls it)
function game.set_briefing_text(text_hash, slot) return call(0x4E0, text_hash, slot) end
-- objectives: add_objective(title hash[, description hash]) adds one (API +0x4B0 = FUN_00458640) and returns the
-- game's index for it - 0, 1, ... in the order objectives are added (a base mission's DLL adds its own too, and
-- re-adds them when a save is loaded, so keep the returned index, don't assume one); nil if the game refused.
-- The old form add_objective(hash, small index) was really +0x4E0 (briefing text) and still does that.
local warned_old_objective = false
function game.add_objective(text_hash, description_hash)
    if description_hash and description_hash >= 0 and description_hash < 0x10000 then
        if not warned_old_objective then
            warned_old_objective = true
            game.log("game.add_objective(hash, index) is the old form - it sets briefing text (game.set_briefing_text);"
                     .. " game.add_objective(hash) adds an objective and returns its index")
        end
        return game.set_briefing_text(text_hash, description_hash)
    end
    local i = call(0x4B0, text_hash) & 0xFF      -- the index comes back in al only (0xFF = failed)
    if i == 0xFF then return nil end
    if description_hash then call(0x4B4, i, description_hash) end   -- description (the objectives screen)
    return i
end
function game.show_objective(index) return call(0x4B8, index) end      -- shown + tracked
function game.hide_objective(index) return call(0x4BC, index) end
function game.complete_objective(index) return call(0x4C0, index) end  -- objective sound, done, untracked
function game.reopen_objective(index) return call(0x4C4, index) end
-- mission end: 2 = success, 1 = failure (MissionN.dll: +0x48C(2) after setting the next level)
-- Without set_next_level, a success continues with the campaign's next mission (campaign.txt), the front end after the
-- last one (a level with no next level quit the game after its end screen).
local next_set = false
function game.set_next_level(name) next_set = true; return call(0x490, name) end
function game.next_mission() return game._next_mission() end
function game.end_mission(result)
    if result == 2 and not next_set then call(0x490, game._next_mission()) end
    return call(0x48C, result)
end
function game.debug_print(text) return call(0x30C, text) end
-- objects by script id: activate puts a waiting one (scene field active = 0) into play (API +0xC4: SetActive(1)
-- unless already active), deactivate takes one out (+0xC8: SetActive(0)). Layers = named groups of script ids (the
-- editor writes game.layers into the mission script) shown or hidden together.
function game.activate(id) return call(0xC4, id) end
function game.deactivate(id) return call(0xC8, id) end
game.layers = {}
function game.show_layer(name)
    for _, id in ipairs(game.layers[name] or {}) do call(0xC4, id) end
end
function game.hide_layer(name)
    for _, id in ipairs(game.layers[name] or {}) do call(0xC8, id) end
end
-- trigger regions (the scene's REGN records; the editor writes them into game.regions): spheres {x, y, z, radius =
-- r} and boxes {x, y, z, yaw = deg, min = {x, y, z}, max = {x, y, z}} (offsets around the position, turned by yaw like
-- the scene's objects). The game has no "entered" event, so _frame() (the host calls it before update) checks every
-- player each frame and sends on_message(game.EVENT.ENTERED / LEFT, region name, player 0-3).
game.regions = {}
game.EVENT_ENTERED, game.EVENT_LEFT = 0x100, 0x101
function game.in_region(name, x, y, z)
    local r = game.regions[name]
    if not r or not x then return false end
    local dx, dy, dz = x - r[1], y - r[2], z - r[3]
    if r.radius then return dx * dx + dy * dy + dz * dz <= r.radius * r.radius end
    local c, s = math.cos(math.rad(r.yaw or 0)), math.sin(math.rad(r.yaw or 0))
    local lx, lz = dx * c - dz * s, dx * s + dz * c
    return lx >= r.min[1] and lx <= r.max[1] and dy >= r.min[2] and dy <= r.max[2] and lz >= r.min[3] and lz <= r.max[3]
end
function game.player_in_region(name, n) return game.in_region(name, game.player_position(n or 0)) end
local inside = {}
-- Timers go into the game's event queue [0x63C978] (API +0x5A4 -> FUN_004db6b0), whose array (+8) doesn't exist yet
-- while the level is initialising - a timer set in init() crashed the game at 0x4DB6C1. game.timer keeps those back
-- and _frame arms them on the first frame the queue is there.
local pending_timers = {}
function game._timer_queue_ready()
    local q = game.read32(0x63C978)
    return q ~= nil and q ~= 0 and (game.read32(q + 8) or 0) ~= 0
end
local function arm_pending_timers()
    if next(pending_timers) == nil or not game._timer_queue_ready() then return end
    local list = pending_timers
    pending_timers = {}
    for _, t in ipairs(list) do game.call(0x5A4, t[1], t[2] * 1000.0, 0, 0) end
end
function _frame()
    arm_pending_timers()
    if next(game.regions) == nil or type(on_message) ~= "function" then return end
    for n = 0, 3 do
        local x, y, z = game.player_position(n)
        if x then
            for name in pairs(game.regions) do
                local key = name .. "#" .. n
                local now = game.in_region(name, x, y, z)
                if now ~= (inside[key] or false) then
                    inside[key] = now
                    on_message(now and game.EVENT_ENTERED or game.EVENT_LEFT, name, n)
                end
            end
        end
    end
end
-- persistent script state: everything in game.state is stored in save games (nil, booleans, numbers, strings and
-- tables of those; max 2 KB as text). After a load, game.state holds the saved values and on_message(EVENT.SAVE_LOADED)
-- follows; init() runs before that - game.loading_save() tells it to skip intros.
game.state = {}
function game.loading_save() return call(0x3F4) ~= 0 end
local function serialize(v, depth)
    local t = type(v)
    if t == "number" or t == "string" then return string.format("%q", v) end
    if t == "boolean" or t == "nil" then return tostring(v) end
    if t ~= "table" then error("game.state can't hold a " .. t) end
    if depth > 16 then error("game.state is nested too deeply") end
    local parts = {}
    for k, val in pairs(v) do
        parts[#parts + 1] = "[" .. serialize(k, depth + 1) .. "]=" .. serialize(val, depth + 1)
    end
    return "{" .. table.concat(parts, ",") .. "}"
end
function _save_state() return "return " .. serialize(game.state, 0) end
function _load_state(text)
    local chunk = load(text, "=save", "t", {})
    game.state = chunk and chunk() or {}
end
-- script timer: on_message(game.EVENT.TIMER, id) after `seconds` (API +0x5A4(id, float ms, 0, repeat); verified:
-- 3000.0 fired after 3.0 s of game time)
-- The API's own repeat flag fires once more at once and then stops (measured 2026-10-09: two ticks 3 ms apart), so a
-- repeating timer is a one-shot that the host re-arms each time it fires (_timer_fired).
local repeating_timers = {}
function game.timer(id, seconds, repeating)
    repeating_timers[id] = repeating and seconds or nil
    if not game._timer_queue_ready() then      -- level still initialising: armed on the first frame (_frame)
        pending_timers[#pending_timers + 1] = { id, seconds }
        return 0
    end
    return call(0x5A4, id, seconds * 1000.0, 0, 0)
end
function game.stop_timer(id) repeating_timers[id] = nil end
function _timer_fired(id)
    local s = repeating_timers[id]
    if s then call(0x5A4, id, s * 1000.0, 0, 0) end
end
-- the pause menu (and so SAVE GAME / LOAD GAME from it): API +0x460 sets the flag the pause key checks ([0x61079C];
-- every mission DLL sets it once its intro is over). Lua levels have it on after init(); turn it off around your own
-- cutscenes with game.allow_pause(false).
function game.allow_pause(on) return call(0x460, on == false and 0 or 1) end
function game.float(bits) return string.unpack("<f", string.pack("<i4", bits)) end
-- message ids (on_message(id, a, b, c, d, e, param)); ? = meaning not certain
game.EVENT = {
    KILLED = 0x00,             -- a victim, b killer (-1), c time
    REMOVED = 0x01,            -- a dead soldier deactivated
    DROPPED = 0x02,            -- a soldier, b item
    PICKED_UP = 0x03,          -- a soldier, b item
    COMBAT = 0x04,             -- a enemy unit reached combat alert
    DESTROYED = 0x07,          -- a object, b attacker (-1)
    WAYPOINT = 0x08,           -- a unit, b waypoint index
    PATH_DONE = 0x09,          -- a unit
    ARRIVED = 0x0A,            -- a unit (only with its notify flag)
    HEALED = 0x0B,             -- a unit
    DAMAGED = 0x0C,            -- a unit
    SWITCH = 0x0D,             -- a hash of the switch name as written (FUN_004b9a10, signed; switches.cpp +0x44), b 1.0f, c soldier
    RADIO_CALL = 0x0E,         -- a soldier, b c d target x y z (floats), e near
    LASER = 0x0F,              -- query: answer 1 = designate b
    SPAWN = 0x10,              -- a spawn point
    COMPLETE = 0x11,           -- mission complete (debug key / dev flag)
    START = 0x12,              -- mission start set-up
    ACTION = 0x13,             -- a soldier, b action type offered via ACTION_QUERY
    DOOR = 0x15,               -- a door, b 0 closing / 1 opening / 2 closed / 3 open
    TIMER = 0x16,              -- a timer id (game.timer)
    PLANTED = 0x17,            -- a soldier
    EXPLOSION = 0x18,          -- a b c position (floats)
    MOUNT_QUERY = 0x1A,        -- query: a soldier, b vehicle / gun -> allow
    MOUNTED = 0x1B,            -- a soldier, b vehicle
    DISMOUNTED = 0x1C,         -- a soldier, b vehicle
    SEAT = 0x1D,               -- a soldier, b vehicle, c seat
    LAUNCHER_DROPPED = 0x1E,   -- a soldier, b launcher
    VEHICLE_DAMAGED = 0x1F,    -- a vehicle, b attacker
    OBJECTIVES_SCREEN = 0x20,  -- a 1 opened / 0 closed
    ACTION_QUERY = 0x26,       -- query per frame: a soldier -> action type (0 none)
    DROP_QUERY = 0x28,         -- query: a soldier, b item -> allow (default allowed)
    PICKUP_QUERY = 0x29,       -- query: a soldier, b item -> allow (default allowed)
    VEHICLE_ITEMS_QUERY = 0x2A,-- query: a soldier, b vehicle -> allow (default allowed)
    SWITCH_SOLDIER_QUERY = 0x2B, -- query -> allow (default allowed)
    GIVE_QUERY = 0x33,         -- query: a giver, b receiver, c item -> allow
    SPOTTED = 0x34,            -- a observer, b target
    SHOT = 0x3C,               -- a weapon, b shooter
    FIRE_MODE = 0x40,          -- a soldier
    ZOOM = 0x41,               -- a soldier
    TRIGGER = 0x44,            -- a soldier, b weapon (query: c ? consume ammo)
    LADDER_QUERY = 0x46,       -- query: a soldier -> allow
    ORDERS_QUERY = 0x47,       -- query: a soldier -> allowed orders mask (0xFF)
    UNLOCK = 0x4A,             -- a soldier, b held item
    INVENTORY_QUERY = 0x4C,    -- query: a soldier, b held item -> allow
    SAVE_LOADED = 0x52,
    ENTERED = 0x100,           -- a region name, b player 0-3 (the host's own, see game.regions)
    LEFT = 0x101,              -- a region name, b player 0-3
}
function game.time_ms() local t = game.read32(0x63C958); return t and game.read32(t + 0x10) or 0 end
-- texts of your own: a hash for add_objective, popup and anything that shows catalog text
function game.text(name, text) return game._text(name, text) end
-- a message in the pop-up bar at the bottom of the screen
function game.popup(text, seconds) return game._popup(text, math.floor((seconds or 4) * 1000)) end
-- objects by script id: the object, its position (cm)
function game.object(id)
    local mgr = game.read32(0x63C9A0); if not mgr or mgr == 0 then return 0 end
    local t = game.read32(mgr + 0x30); return t and game.read32(t + id * 4) or 0
end
function game.position(id)
    local o = game.object(id); if o == 0 then return nil end
    return game.readf(o + 0x30), game.readf(o + 0x34), game.readf(o + 0x38)
end
function game.distance(id, x, y, z)
    local ax, ay, az = game.position(id); if not ax then return math.huge end
    return math.sqrt((ax - x) ^ 2 + (ay - y) ^ 2 + (az - z) ^ 2)
end
-- a soldier's side (API +0xE0): 0 = the squad, 2 = what Mission 1 uses for its prisoner
function game.set_team(id, team) return call(0xE0, id, team) end
-- prisoners the way Mission1.dll holds Foley: prisoner uniform (+0x258, animations kept), kit taken (+0x2A8, unless keep_kit), the
-- captive AI bit (+0x1D8 -> soldier +0x284 bit 0x200), another side (+0xE0 team 2) and AI bit 30 (+0x180).
-- game.captive(id, false) frees him: back on the squad's side, bits cleared, his own uniform.
function game.captive(id, on, keep_kit)
    if on ~= false then
        call(0x258, id, "AlliedPOW.bmp", 0)   -- 1 would also stop his animations (FUN_00502bb0) for good: crash
                                              -- at 0x42DF97 once the AI moves him after release
        if not keep_kit then call(0x2A8, id) end
        call(0x1D8, id, 0); call(0xE0, id, 2); call(0x180, id, 0)
    else
        call(0xE0, id, 0); call(0x1D8, id, 1); call(0x180, id, 1); game._clear_skin(id)
    end
end
-- player n's soldier (input block 0x60F5B8 + n*0x478, soldier +0x310) and its position (+0x30, cm)
function game.player_soldier(n) return game.read32(0x60F5B8 + (n or 0) * 0x478 + 0x310) or 0 end
function game.player_position(n)
    local s = game.player_soldier(n)
    if s == 0 then return nil end
    return game.readf(s + 0x30), game.readf(s + 0x34), game.readf(s + 0x38)
end
)lua";

void OpenLibs(lua_State* L) {
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
                                    {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_COLIBNAME, luaopen_coroutine}};
    for (const auto& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    for (const char* unsafe : {"dofile", "loadfile"}) {  // file access only through the game's loader
        lua_pushnil(L);
        lua_setglobal(L, unsafe);
    }
    static const luaL_Reg game[] = {{"call", LuaCall},
                                    {"callf", LuaCallF},
                                    {"set_level_bounds", LuaLevelBounds},
                                    {"read32", LuaRead<uint32_t>},
                                    {"readf", LuaRead<float>},
                                    {"log", LuaLog},
                                    {"_ladder_anims", LuaLadderAnims},
                                    {"_next_mission", LuaNextMission},
                                    {"_text", LuaText},
                                    {"set_string", LuaSetString},
                                    {"_popup", LuaPopup},
                                    {"_clear_skin", LuaClearSkin},
                                    {"keep_awake", LuaKeepAwake},
                                    {"kit", LuaKit},
                                    {"briefing", LuaBriefing},
                                    {"loadout", LuaLoadout},
                                    {nullptr, nullptr}};
    luaL_newlib(L, game);
    features::RegisterFilmFunctions(L);   // movie.cpp: images, shapes, music, film mode
    lua_setglobal(L, "game");
    lua_pushcfunction(L, LuaLog);
    lua_setglobal(L, "print");
}

bool EndsWith(const char* s, const char* tail) {
    const size_t n = strlen(s), m = strlen(tail);
    return n >= m && _stricmp(s + n - m, tail) == 0;
}

// "--@base Mission1.dll" / "--@scene NAME.ENV" header lines (first 2 KB).
std::string Directive(const std::string& source, const char* key) {
    const std::string head = source.substr(0, 2048);
    const size_t at = head.find(key);
    if (at == std::string::npos) return {};
    size_t b = head.find_first_not_of(" \t", at + strlen(key)), e = head.find_first_of(" \t\r\n", b);
    return b == std::string::npos ? std::string() : head.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

// API +0x44 (load scene) while the base DLL sets itself up: this mission's scene instead of the DLL's own.
constexpr uint32_t kApiLoadScene = kApiTable + 0x44;
uint32_t g_loadScene = 0;
int __cdecl BaseLoadScene(const char* scene) {
    dslog::Write("Script: base mission loads %s -> %s", scene ? scene : "(null)", g_scene.c_str());
    return reinterpret_cast<int(__cdecl*)(const char*)>(g_loadScene)(g_scene.c_str());
}

bool LoadBase(const std::string& dll) {
    constexpr uint32_t kLoadMissionDll = 0x449260;   // stdcall(name) (ret 4) -> LevelSetup -> [0x60F578], module [0x60F57C]
    g_loadScene = *reinterpret_cast<uint32_t*>(kApiLoadScene);
    patch::WriteValue(kApiLoadScene, reinterpret_cast<uint32_t>(&BaseLoadScene));
    *reinterpret_cast<ScriptObject**>(kScriptObject) = nullptr;
    const bool ok = reinterpret_cast<bool(__stdcall*)(const char*)>(kLoadMissionDll)(dll.c_str());
    patch::WriteValue(kApiLoadScene, g_loadScene);
    g_base = ok ? *reinterpret_cast<ScriptObject**>(kScriptObject) : nullptr;
    if (!g_base) features::ModLog("[fail] script %s: base mission %s didn't load", g_name.c_str(), dll.c_str());
    else dslog::Write("Script: base mission %s runs under %s", dll.c_str(), g_scene.c_str());
    return g_base != nullptr;
}

int RunLuaLevel(const char* name) {
    if (g_lua) lua_close(g_lua);
    g_lua = nullptr;
    g_awake.clear();   // the previous level's soldiers are gone
    g_name = name;
    g_failed = false;
    g_saveText.clear();
    g_saveTextValid = false;
    g_base = nullptr;
    g_updates = 0;
    g_loadingSave = false;
    features::ResetScriptLoadout();
    g_namedTexts.clear();
    g_namedHashes.clear();
    g_namedTable = nullptr;

    char* buf = nullptr;
    int size = 0;
    if (!reinterpret_cast<int(__cdecl*)(const char*, char**, int*, int, int)>(kReadFile)(name, &buf, &size, 0, 6) || !buf) {
        features::ModLog("[fail] script %s: file not found", name);
        return 0;
    }
    std::string source(buf, static_cast<size_t>(size));
    reinterpret_cast<void(__cdecl*)(void*)>(kFree)(buf);

    lua_State* L = luaL_newstate();
    g_lua = L;
    OpenLibs(L);
    if (luaL_loadbuffer(L, kPrelude, strlen(kPrelude), "=prelude") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        Report("prelude", L);
        return 0;
    }
    const std::string base = Directive(source, "--@base");
    if (!base.empty()) {                       // the base DLL registers its animations and sets the map up itself
        g_scene = Directive(source, "--@scene");
        if (g_scene.empty()) g_scene = g_name.substr(0, g_name.size() - 4) + ".ENV";
        if (!LoadBase(base)) return 0;
    } else {
        for (const auto& anim : generated::kStandardAnims)
            reinterpret_cast<int(__cdecl*)(const char*, const char*)>(ApiFunction(0x04))(anim.state, anim.file);
    }
    const std::string chunk = "@" + g_name;
    if (luaL_loadbuffer(L, source.data(), source.size(), chunk.c_str()) != LUA_OK) {
        Report("load", L);
        return 0;
    }
    lua_pushcfunction(L, Traceback);
    lua_insert(L, -2);
    if (lua_pcall(L, 0, 0, -2) != LUA_OK) {
        Report("run", L);
        return 0;
    }
    lua_pop(L, 1);
    g_loadingSave = reinterpret_cast<int(__cdecl*)()>(ApiFunction(0x3F4))() != 0;
    if (!CallGlobal("init", 0, 0) && g_failed) return 0;
    if (!g_base) reinterpret_cast<int(__cdecl*)(int)>(ApiFunction(0x460))(1);   // pause menu allowed (see allow_pause)
    *reinterpret_cast<ScriptObject**>(kScriptObject) = &g_object;
    dslog::Write("Script: %s running (%s)", name, g_base ? ("on " + base).c_str() : "standard animations");
    return 1;
}

int __fastcall LoadTextLevel(void* loader, void*, const char* name) {
    if (name && EndsWith(name, ".lua")) return RunLuaLevel(name);
    return reinterpret_cast<int(__thiscall*)(void*, const char*)>(kTextLevelLoad)(loader, name);
}

// --- level data for .lua levels -------------------------------------------------------------------------------------
// Level init (cdecl(name), one caller 0x40E05D) opens the level archive "<name minus 4 chars>.dat" into slot 1
// (FUN_004bcf70(1, path, 0, 0) at 0x40C19C, after FUN_004bd000 closed the previous one), loads the sound cache
// "<name minus 4>.sch" (FUN_0055f540 thiscall(sound manager, path) at 0x40C2E6 - without it every sound of the level
// is "not in the cache") and the music bank "<name minus 4>Music.sbk" (FUN_0055dd20 thiscall(manager, path, flag) at
// 0x40C350) - all before the script runs.
// A .lua level names its data in its header instead:  --@archive mission1  (mission1.dat + mission1.sch: reuse a
// shipped map) or  --@archive none  (no level archive: the map's files are loose in the mod folder). Without the line
// the game's own names are used when that archive exists.
constexpr uint32_t kLevelInitCall = 0x40E05D, kLevelInit = 0x40C090;
constexpr uint32_t kArchiveOpenCall = 0x40C19C, kArchiveOpen = 0x4BCF70;
constexpr uint32_t kSoundCacheCall = 0x40C2E6, kSoundCacheLoad = 0x55F540;
constexpr uint32_t kMusicBankCall = 0x40C350, kMusicBankLoad = 0x55DD20;
bool g_luaLevel = false;
std::string g_archive;  // declared archive base name, "none", or empty

std::string ReadArchiveDirective(const char* name) {
    char* buf = nullptr;
    int size = 0;
    if (!reinterpret_cast<int(__cdecl*)(const char*, char**, int*, int, int)>(kReadFile)(name, &buf, &size, 0, 6) || !buf)
        return {};
    std::string head(buf, static_cast<size_t>(size < 2048 ? size : 2048));
    reinterpret_cast<void(__cdecl*)(void*)>(kFree)(buf);
    const size_t at = head.find("--@archive");
    if (at == std::string::npos) return {};
    size_t b = head.find_first_not_of(" \t", at + 10), e = head.find_first_of(" \t\r\n", b);
    if (b == std::string::npos) return {};
    std::string value = head.substr(b, e == std::string::npos ? std::string::npos : e - b);
    if (value.size() > 4 && _stricmp(value.c_str() + value.size() - 4, ".dat") == 0) value.resize(value.size() - 4);
    return value;
}

int __cdecl LevelInit(const char* name) {
    features::ResetScriptFilm();   // a film left running by the last level
    g_awake.clear();               // keep_awake soldiers of the last level (pointers are reused)
    g_luaLevel = name && EndsWith(name, ".lua");
    g_archive = g_luaLevel ? ReadArchiveDirective(name) : std::string();
    if (g_luaLevel) dslog::Write("Script: level %s, archive %s", name, g_archive.empty() ? "(default)" : g_archive.c_str());
    return reinterpret_cast<int(__cdecl*)(const char*)>(kLevelInit)(name);
}

int __cdecl OpenLevelArchive(int slot, const char* path, int a, int b) {
    using Open = int(__cdecl*)(int, const char*, int, int);
    if (g_luaLevel) {
        if (_stricmp(g_archive.c_str(), "none") == 0) return 0;
        if (!g_archive.empty()) {
            const std::string own = g_archive + ".dat";
            return reinterpret_cast<Open>(kArchiveOpen)(slot, own.c_str(), a, b);
        }
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) {
            dslog::Write("Script: no %s - the level has no archive", path);
            return 0;
        }
    }
    return reinterpret_cast<Open>(kArchiveOpen)(slot, path, a, b);
}

// The loading-bar file "<base>.FAS" (FUN_004fb880 at 0x40C1B1) takes its base from FUN_004be620(level name) at
// 0x40C1A2 (the only use of that result). Routes / quadtree / octree are named after the scene (.env) instead.
constexpr uint32_t kBaseNameCall = 0x40C1A2, kBaseName = 0x4BE620;
const char* __cdecl LevelBaseName(const char* name) {
    if (g_luaLevel && !g_archive.empty() && _stricmp(g_archive.c_str(), "none") != 0) return g_archive.c_str();
    return reinterpret_cast<const char* (__cdecl*)(const char*)>(kBaseName)(name);
}

bool UsesOwnArchive() { return g_luaLevel && !g_archive.empty() && _stricmp(g_archive.c_str(), "none") != 0; }

void* __fastcall LoadSoundCache(void* manager, void*, const char* path) {
    using Load = void*(__thiscall*)(void*, const char*);
    const std::string own = g_archive + ".sch";
    return reinterpret_cast<Load>(kSoundCacheLoad)(manager, UsesOwnArchive() ? own.c_str() : path);
}

void* __fastcall LoadMusicBank(void* manager, void*, const char* path, int flag) {
    using Load = void*(__thiscall*)(void*, const char*, int);
    const std::string own = g_archive + "Music.sbk";
    return reinterpret_cast<Load>(kMusicBankLoad)(manager, UsesOwnArchive() ? own.c_str() : path, flag);
}

// Octree for .lua levels: the scene's zone props are culled through "<scene>.OCT", which indexes the props by their
// place in the scene - an edited scene (props moved, added, deleted) with the base level's octree draws the wrong ones
// or none. Level init only builds it with start-up flag 'c' (0x40C633: FUN_004e3560(scene, 1) builds every zone's
// octree and writes "<scene>.OCT" with the game's fopen at 0x4E35C8, then FUN_004e3730 loads it back through the
// resolver). For .lua levels the flag check answers yes and the file goes over the mod's copy (else the game folder).
constexpr uint32_t kOctreeFlagCall = 0x40C635, kStartFlag = 0x4B9CD0;
constexpr uint32_t kOctreeOpenCall = 0x4E35C8, kGameFopen = 0x56732D;
DWORD g_octreeStart = 0;

int __cdecl OctreeFlag(int letter) {
    // Only for a scene the mission ships (its .OCT placeholder is in the mod / built from the player's install): a
    // .lua level on a shipped scene keeps the shipped octree - a rebuild would write <scene>.OCT into the game folder.
    std::string scene = reinterpret_cast<const char*>(0x606880);
    scene = scene.substr(0, scene.find_last_of('.')) + ".OCT";
    if (g_luaLevel && letter == 'c' && !features::ModFilePath(scene.c_str()).empty()) {
        g_octreeStart = GetTickCount();
        return 1;
    }
    return reinterpret_cast<int(__cdecl*)(int)>(kStartFlag)(letter);
}

void* __cdecl OctreeOpen(const char* name, const char* mode) {
    using Fopen = void*(__cdecl*)(const char*, const char*);
    const std::string own = g_luaLevel ? features::ModFilePath(name) : std::string();
    void* f = reinterpret_cast<Fopen>(kGameFopen)(own.empty() ? name : own.c_str(), mode);
    if (g_luaLevel)
        dslog::Write("Script: octree %s rebuilt from the scene in %lu ms -> %s%s", name,
                     GetTickCount() - g_octreeStart, own.empty() ? name : own.c_str(), f ? "" : " (can't write)");
    return f;
}
}  // namespace

// --- AI navigation for edited scenes ---------------------------------------------------------------------------------
// A level's AI walk map (<scene>.qtd, quadtree of walkable cells) and route network (<scene>.rmd) were made from the
// shipped scene, so they ignore what an editor mission added (a tank in the road, a new wall). When a level has no
// .qtd the game builds both from the scene (level init 0x40C731: Routing_BuildFromScene FUN_00507470 -> quadtree
// FUN_005062e0 over the collision, one marker per walkable cell FUN_00506470, router FUN_00509a60) - the generated part
// of the shipped network exactly (Mission 1: the same 1673 markers). What it can't make are the hand-placed markers the
// shipped file starts with (Mission 1: 339, flags without bit 0) - mission scripts address them by index (API +0x60 /
// +0x64, Mission1.dll 106 calls) - so for .lua levels they are put back from the level's own .rmd at their indices,
// the generated ones (and every walkable cell's marker index, node +2) move up behind them, the hand-placed markers'
// links into the generated part are matched by position, and the islands (FUN_00504670) and the router's per-marker
// table are redone. Marker = 32 B: x, y, z, u16 links[5] (0xFFFF none), u16 island, u16 flags, u16 filler, f32 weight.
constexpr uint32_t kBuildNavCall = 0x40C731, kBuildNav = 0x507470;
constexpr uint32_t kMarkerSet = 0x63C9C0, kQuadRoot = 0x63C9BC, kRouter = 0x63C9A8;
struct NavMarker {
    float x, y, z;
    uint16_t links[5];
    uint16_t island, flags, filler;
    float weight;
};
static_assert(sizeof(NavMarker) == 32, "marker layout");

void ShiftCells(uint8_t* node, uint16_t by, int& cells) {
    for (int depth = 0; node && depth < 64; ++depth) {
        uint8_t* children = *reinterpret_cast<uint8_t**>(node + 4);
        if (!children) {
            if (node[0] & 2) { *reinterpret_cast<uint16_t*>(node + 2) += by; ++cells; }
            return;
        }
        for (int k = 0; k < 3; ++k) ShiftCells(children + k * 0x14, by, cells);
        node = children + 3 * 0x14;
    }
}

void __cdecl BuildNav(void* scene) {
    reinterpret_cast<void(__cdecl*)(void*)>(kBuildNav)(scene);
    if (!g_luaLevel) return;
    const DWORD t0 = GetTickCount();
    // the level's own route file (an editor mission's: the base network + its changes, mods.cpp BuildReferences)
    std::string name = reinterpret_cast<const char*>(0x606880);
    name = name.substr(0, name.find_last_of('.')) + ".RMD";
    std::string file;
    const std::string path = features::ModFilePath(name.c_str());
    HANDLE f = path.empty() ? INVALID_HANDLE_VALUE
                            : CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) {
        file.resize(GetFileSize(f, nullptr));
        DWORD got = 0;
        ReadFile(f, file.data(), static_cast<DWORD>(file.size()), &got, nullptr);
        CloseHandle(f);
    }
    auto* set = *reinterpret_cast<uint32_t**>(kMarkerSet);
    if (file.size() < 12 || !set) {
        dslog::Write("Script: AI navigation built from the scene (%s: no hand-placed markers)", name.c_str());
        return;
    }
    const auto* old = reinterpret_cast<const NavMarker*>(file.data() + 12);
    const uint32_t oldCount = static_cast<uint32_t>((file.size() - 12) / 32);
    uint32_t hand = 0;
    while (hand < oldCount && !(old[hand].flags & 1)) ++hand;
    const auto* built = reinterpret_cast<NavMarker*>(set[0]);
    const uint32_t n = set[1];
    if (!hand || !built || hand + n > 0xFFF0) return;

    // generated markers by cell position (x, z): old generated index -> new one
    auto key = [](float x, float z) { return (static_cast<uint64_t>(static_cast<int32_t>(lroundf(x))) << 32) ^
                                             static_cast<uint32_t>(static_cast<int32_t>(lroundf(z))); };
    std::unordered_map<uint64_t, uint16_t> byPos;
    for (uint32_t i = 0; i < n; ++i) byPos[key(built[i].x, built[i].z)] = static_cast<uint16_t>(i);
    auto mapOld = [&](uint16_t l) -> uint16_t {          // a link of the old file -> index in the new set
        if (l == 0xFFFF || l >= oldCount) return 0xFFFF;
        if (l < hand) return l;
        auto it = byPos.find(key(old[l].x, old[l].z));
        if (it != byPos.end()) return static_cast<uint16_t>(it->second + hand);
        float best = 600.0f * 600.0f; uint16_t at = 0xFFFF;  // the cell changed: nearest generated marker within 6 m
        for (uint32_t i = 0; i < n; ++i) {
            const float dx = built[i].x - old[l].x, dz = built[i].z - old[l].z, d = dx * dx + dz * dz;
            if (d < best) { best = d; at = static_cast<uint16_t>(i + hand); }
        }
        return at;
    };

    uint32_t tmp[2] = {};
    reinterpret_cast<int(__thiscall*)(uint32_t*, int)>(0x504110)(tmp, static_cast<int>(hand + n));
    auto* merged = reinterpret_cast<NavMarker*>(tmp[0]);
    if (!merged) return;
    for (uint32_t i = 0; i < n; ++i) {
        merged[hand + i] = built[i];
        for (uint16_t& l : merged[hand + i].links)
            if (l != 0xFFFF) l = static_cast<uint16_t>(l + hand);
    }
    // Links between generated markers that the shipped network has and the builder doesn't make (Mission 1: 16, without
    // them the network fell apart into 75 islands instead of 2) - kept wherever both cells still exist.
    int kept = 0;
    for (uint32_t i = hand; i < oldCount; ++i) {
        const uint16_t a = mapOld(static_cast<uint16_t>(i));
        if (a == 0xFFFF || byPos.find(key(old[i].x, old[i].z)) == byPos.end()) continue;
        for (uint16_t l : old[i].links) {
            if (l == 0xFFFF || l < hand || l >= oldCount || byPos.find(key(old[l].x, old[l].z)) == byPos.end()) continue;
            const uint16_t b = mapOld(l);
            bool has = false;
            for (uint16_t x : merged[a].links) has |= x == b;
            for (uint16_t& x : merged[a].links)
                if (!has && x == 0xFFFF) { x = b; has = true; ++kept; }
        }
    }
    int lost = 0;
    for (uint32_t m = 0; m < hand; ++m) {
        merged[m] = old[m];
        for (uint16_t& l : merged[m].links) {
            const uint16_t was = l;
            l = mapOld(l);
            if (was != 0xFFFF && l == 0xFFFF) ++lost;
            if (l == 0xFFFF || l < hand || was >= oldCount) continue;
            bool back = false;                                // the old generated marker linked back: so does the new
            for (uint16_t b : old[was].links) back |= b == m;
            if (!back) continue;
            NavMarker& q = merged[l];
            bool has = false;
            for (uint16_t b : q.links) has |= b == m;
            for (uint16_t& b : q.links)
                if (!has && b == 0xFFFF) { b = static_cast<uint16_t>(m); has = true; }
            for (uint16_t& b : q.links) {                    // all 5 used: drop one the original marker didn't have
                if (has) break;
                bool original = false;
                for (uint16_t o : old[was].links) original |= o != 0xFFFF && mapOld(o) == b;
                if (!original) { b = static_cast<uint16_t>(m); has = true; }
            }
        }
    }
    reinterpret_cast<void(__fastcall*)(uint32_t*)>(0x504210)(set);   // the built array
    set[0] = tmp[0];
    set[1] = hand + n;
    int cells = 0;
    ShiftCells(*reinterpret_cast<uint8_t**>(kQuadRoot), static_cast<uint16_t>(hand), cells);
    for (uint32_t i = 0; i < hand + n; ++i) merged[i].island = 0;   // the flood fill only visits island 0
    reinterpret_cast<void(__fastcall*)(uint32_t*)>(0x504670)(set);   // islands
    if (void* router = *reinterpret_cast<void**>(kRouter))
        reinterpret_cast<void*(__thiscall*)(void*, int)>(0x509A60)(router, static_cast<int>(hand + n));
    dslog::Write("Script: AI navigation built from the scene: %u markers + %u hand-placed from %s (%d links lost), "
                 "%d cells, %d shipped links kept, %u islands, %lu ms", n, hand, name.c_str(), lost, cells, kept, set[2],
                 GetTickCount() - t0);
}

// "DesertStorm.exe +level ambush.lua": boot straight into a level (the editor's Play) - the game's built-in start-up
// options (pointer 0x5E7050 -> "-f -d", parsed by FUN_0040fdc0: a plain word = the level) are aimed at "-d <name>",
// and the launcher presses Play by itself (launcher.cpp, features::QuickStartLevel).
namespace {
char g_quickLevel[64] = "";
void ParseCommandLine() {
    const char* cmd = GetCommandLineA();
    const char* at = cmd ? strstr(cmd, "+level ") : nullptr;
    if (!at) return;
    at += 7;
    while (*at == ' ') ++at;
    size_t n = 0;
    while (at[n] && at[n] != ' ' && at[n] != '"' && n < sizeof g_quickLevel - 1) ++n;
    std::string name(at, n);
    const bool ok = n > 4 && (EndsWith(name.c_str(), ".lua") || EndsWith(name.c_str(), ".dll")) &&
                    name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") == std::string::npos;
    if (!ok) {
        dslog::Write("[fail] +level %s: not a level name (letters, digits, _ . ending in .lua or .dll)", name.c_str());
        return;
    }
    constexpr uint32_t kOptionsPtr = 0x5E7050, kOptionsDefault = 0x5E708C;  // -> "-f -d"
    static char options[80];
    const uint32_t expected = kOptionsDefault;
    if (!patch::Matches(kOptionsPtr, &expected, 4)) return;
    snprintf(options, sizeof options, "-d %s", name.c_str());
    patch::WriteValue(kOptionsPtr, reinterpret_cast<uint32_t>(options));
    strcpy_s(g_quickLevel, name.c_str());
    dslog::Write("Script: +level %s - straight into the level", g_quickLevel);
}
}  // namespace

const char* features::ScriptText(uint32_t hash) {
    auto it = g_texts.find(hash);
    if (it != g_texts.end()) return it->second.c_str();
    if (g_namedTexts.empty() || !g_luaLevel) return nullptr;
    ResolveNamedTexts();
    auto named = g_namedHashes.find(hash);
    return named == g_namedHashes.end() ? nullptr : named->second.c_str();
}

bool features::ScriptLevelActive() { return g_luaLevel && g_lua && !g_failed; }
bool features::ScriptLoadingSave() { return g_loadingSave; }

const char* features::QuickStartLevel() { return g_quickLevel[0] ? g_quickLevel : nullptr; }

void features::ApplyScriptHost() {
    ParseCommandLine();
    if (patch::HookCall(kTextLevelCall, reinterpret_cast<const void*>(&LoadTextLevel), kTextLevelLoad) &&
        patch::HookCall(kLevelInitCall, reinterpret_cast<const void*>(&LevelInit), kLevelInit) &&
        patch::HookCall(kArchiveOpenCall, reinterpret_cast<const void*>(&OpenLevelArchive), kArchiveOpen) &&
        patch::HookCall(kSoundCacheCall, reinterpret_cast<const void*>(&LoadSoundCache), kSoundCacheLoad) &&
        patch::HookCall(kMusicBankCall, reinterpret_cast<const void*>(&LoadMusicBank), kMusicBankLoad) &&
        patch::HookCall(kBaseNameCall, reinterpret_cast<const void*>(&LevelBaseName), kBaseName))
        dslog::Write("Script host: .lua levels enabled");
    // Separate from the chain above: the dev BuildOctree option may own 0x40C635 already (it forces the same branch).
    if (patch::HookCall(kBuildNavCall, reinterpret_cast<const void*>(&BuildNav), kBuildNav))
        dslog::Write("Script host: .lua levels without a walk map build their AI navigation from the scene");
    if (patch::HookCall(kOctreeFlagCall, reinterpret_cast<const void*>(&OctreeFlag), kStartFlag) &&
        patch::HookCall(kOctreeOpenCall, reinterpret_cast<const void*>(&OctreeOpen), kGameFopen))
        dslog::Write("Script host: .lua levels rebuild their octree");
}
