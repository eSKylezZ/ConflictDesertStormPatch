// Lua mission scripts for Conflict: Desert Storm II: a level whose name ends in ".lua" is run by an embedded Lua 5.4
// instead of a MissionNN.dll - the port of DesertStormFix's scripthost.cpp (repo root games/ds1/src/features/; DS1
// addresses in brackets). Modder page: repo root docs/sdk/ds2-mods.md "Lua missions"; every address:
// docs/sdk/reference/game-ds2.md "Lua missions".
//
// How CDS II.exe runs a level script (level init FUN_00412690 [FUN_0040c090], cdecl(name), from Mission_Load 0x42CB64):
// the name's last 3 letters: "dll" -> FUN_00422020 (stdcall(name) ret 4 [FUN_00449260]: LoadLibraryA at 0x42202D,
// GetProcAddress "LevelSetup", LevelSetup(&api table 0x60D638) -> script object [0x62FE00], module [0x62FE04]); "env" ->
// FUN_004220c0 + LoadEnvironment FUN_00411970 (a bare scene); anything else -> the text manifest FUN_00421f40 (thiscall
// [0x62F420], name) at 0x412C51 [0x40C405] - hooked here: ".lua" names run the script instead.
// The API table: 636 cdecl functions (0x9F0 bytes; LevelSetup checks it with api[0](1, 7, 0x9F0)) - DS1's 566 with
// others in between (ModKit editor/games/ds2/api_slots.json + api_align_ds1.txt). The script object is DS1's 6 cdecl
// callbacks: +0 init (LevelSetup only), +4 update() (0x4104DD, every frame while the mission runs: campaign [0x62F454]
// +8 result 0), +8 message(id, param*) -> 1, +0xC save(buffer) -> buffer + n (0x41BEBC), +0x10 load(buffer) -> buffer +
// n (0x41C3D6), +0x14 () -> save block size (0 = SAVE GAME greyed, 0x46487F). (The shipped DLLs' objects carry 5 more
// dwords the game never reads.)
//
// Level data: DS2's level init names its archive / sound cache / music bank / .FAS after the level name exactly like
// DS1 - mods_levels.cpp redirects them (a .lua level's "--@archive mission01" header line = ScriptArchiveFor, a DLL
// level's <stem>.level). Octree / AI navigation for an edited scene: mods_levels.cpp too.
// No standard animation table: DS2 registers the standard states itself (the DLLs only add their scripted ones).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/menus.h"

#ifdef DS2_PSE_HOST
#include "core/pse_host.h"
#include "lua_bridge.h"   // pse/loader: plugin Lua functions (pse.lua, docs/sdk/reference/lua.md)
#endif

namespace {
constexpr uint32_t kTextLevelCall = 0x412C51, kTextLevelLoad = 0x421F40;   // thiscall(loader [0x62F420], name)
constexpr uint32_t kScriptObject = 0x62FE00;
constexpr uint32_t kApiTable = 0x60D638, kApiSize = 0x9F0;
constexpr uint32_t kReadFile = 0x4C49E0;   // cdecl(name, char** buf, int* size, heap, pool) -> 1 ok
constexpr uint32_t kFree = 0x4BE3B0;       // cdecl(memory)
constexpr uint32_t kLoadMissionDll = 0x422020;   // stdcall(name): LoadLibrary + LevelSetup -> [0x62FE00]
constexpr uint32_t kUnloadCall = 0x40D7A8, kUnloadScript = 0x422090;   // level unload: FreeLibrary + object 0 (module only)
constexpr uint32_t kCampaign = 0x62F454;   // CMission*: +8 result (0 running, 1 failed, 2 success)
constexpr uint32_t kCampaignResult = 0x430810;   // thiscall(campaign, result)
constexpr uint32_t kObjects = 0x6800E0, kObjectById = 0x4CF4F0;   // thiscall(object manager, script id)
constexpr uint32_t kGameTimer = 0x68009C;   // +0x10 = ms
constexpr uint32_t kShowMessage = 0x443820;   // cdecl(text hash, ms): the message bar (objectives' texts)
constexpr uint32_t kInstancePos = 0x4D2520;   // thiscall(render instance, float out[3])
// Campaign objective list (DS1's +0x4B0.. API functions, in DS2 no longer in the table - its .si files call them):
constexpr uint32_t kObjAdd = 0x440310, kObjDescription = 0x440330, kObjShow = 0x440390, kObjComplete = 0x440520,
                   kObjMarker = 0x440360, kObjAttach = 0x440560, kObjFind = 0x42ED20;
// Campaign flow (CCampaignFlow 0x631608): +8 one-mission flag (training), +0x10 entry index (-1 = the custom level
// named at +0x20), +0x14 state (2 = load the current level), +0x120 the custom name is set.
constexpr uint32_t kFlow = 0x631608;
constexpr uint32_t kFlowSelect = 0x430EC0;    // thiscall(name, 0) ret 8: 1 = campaign entry, 0 = custom (index -1)
constexpr uint32_t kFlowSetName = 0x40A890;   // thiscall(name) ret 4: +0x20 = name, +0x120 = 1
constexpr uint32_t kFlowFrontEnd = 0x430CF0;  // thiscall: the front end next
constexpr uint32_t kFlowAdvance = 0x430FE0;   // thiscall: after a mission / film (the end screen's Continue ...)
constexpr uint32_t kAdvanceCalls[] = {0x42CC60, 0x42CC76, 0x42CE3A, 0x4302D8, 0x43FBF1, 0x45EC8A,
                                      0x46153F, 0x461852, 0x464BD5, 0x464FA6, 0x46B4E6, 0x46D749};
constexpr uint32_t kCustomNameCall = 0x42CF3B, kCustomNameSource = 0x40C830;   // dispatcher: no custom name set
constexpr uint32_t kOptionsPtr = 0x60981C, kOptionsDefault = 0x5DA84C;   // -> "-fl -d" (devbg.cpp StartLevel)

lua_State* g_lua = nullptr;
std::string g_name;              // the running .lua level
bool g_failed = false;
bool g_luaLevel = false;
int g_updates = 0;
bool g_loadingSave = false;
std::string g_saveText;
bool g_saveTextValid = false;
std::string g_chainFrom;         // the last .lua level that ran (what the end screen's Continue continues from)
std::string g_nextLevel;         // game.set_next_level

template <class T>
T& At(uint32_t va) { return *reinterpret_cast<T*>(va); }
uint32_t ApiFunction(uint32_t slot) { return At<uint32_t>(kApiTable + slot); }
template <class R = int, class... A>
R Api(uint32_t slot, A... a) { return reinterpret_cast<R(__cdecl*)(A...)>(ApiFunction(slot))(a...); }

void Report(const char* what, lua_State* L) {
    features::ModLog("[fail] script %s: %s: %s", g_name.c_str(), what, lua_tostring(L, -1));
    lua_pop(L, 1);
}

int Traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}

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
        lua_pop(L, 1);
        g_failed = true;
        return false;
    }
    lua_remove(L, base);
    return true;
}

// ---- base mission ("--@base Mission01.dll") ----
struct ScriptObject {
    void* fn[6];
};
ScriptObject* g_base = nullptr;
std::string g_scene;       // the mission's own scene for the base DLL's load_scene ("" = the DLL's)
std::string g_baseScene;   // the scene the base DLL asked for

template <typename R, typename... A>
R BaseCall(int slot, A... args) {
    return reinterpret_cast<R(__cdecl*)(A...)>(g_base->fn[slot])(args...);
}

// ---- script object ----
void __cdecl ScriptInit() {}
void __cdecl ScriptUpdate() {
    if (g_updates++ == 0) dslog::Write("Script: %s - first update", g_name.c_str());
    if (g_base) BaseCall<void>(1);
#ifdef DS2_PSE_HOST
    pse::lua_bridge::Sync(g_lua);
#endif
    if (g_lua && !g_failed) CallGlobal("_frame", 0, 0);
    if (g_lua && !g_failed) CallGlobal("update", 0, 0);
}

// Messages that ask the script something (the game reads param[slot] back), from Mission01.dll's message handler:
// the answers it gives as constants are the defaults of a .lua level without a base mission; Lua's on_message result
// (integer / boolean) overrides. 0x2A / 0x2B answer with the callback's return value. DS2's message ids are NOT DS1's.
struct Query {
    int id, slot;   // slot -1 = the return value
    int answer;     // -1 = keep the game's default
};
constexpr Query kQueries[] = {{0x0F, 0, 0},  {0x1B, 2, -1}, {0x24, 1, 1},  {0x25, 2, 1},  {0x26, 2, 1},  {0x27, 2, -1},
                              {0x28, 0, -1}, {0x29, 1, 0},  {0x2A, -1, 0}, {0x2B, -1, 0}, {0x3B, 2, -1}, {0x3E, 1, -1},
                              {0x5A, 0, -1}};
constexpr int kMsgComplete = 0x11, kMsgFailed = 0x12, kMsgTimer = 0x16;

int __cdecl ScriptMessage(int id, uint32_t* param) {
    const Query* query = nullptr;
    for (const auto& q : kQueries)
        if (q.id == id) query = &q;
    int result = 1;
    if (g_base) {
        result = BaseCall<int>(2, id, param);
    } else if (query && query->answer >= 0) {
        if (query->slot < 0) result = query->answer;
        else if (param) param[query->slot] = static_cast<uint32_t>(query->answer);
    }
    bool answered = false, refused = false;
    if (g_lua && !g_failed) {
        lua_State* L = g_lua;
        if (id == kMsgTimer && param) {
            lua_pushinteger(L, static_cast<int32_t>(param[0]));
            CallGlobal("_timer_fired", 1, 0);
        }
        lua_pushinteger(L, id);
        for (int i = 0; i < 5; ++i) {
            if (param) lua_pushinteger(L, static_cast<int32_t>(param[i]));
            else lua_pushnil(L);
        }
        lua_pushinteger(L, reinterpret_cast<uintptr_t>(param));
        if (CallGlobal("on_message", 7, 1)) {
            if (lua_isinteger(L, -1) || lua_isboolean(L, -1)) {
                const int answer = lua_isboolean(L, -1) ? lua_toboolean(L, -1) : static_cast<int>(lua_tointeger(L, -1));
                answered = true;
                refused = answer == 0;
                if (query && query->slot >= 0 && param) param[query->slot] = static_cast<uint32_t>(answer);
                else if (query) result = answer;
            }
            lua_pop(L, 1);
        }
    }
    // Without a script object the game itself ends the mission on these (0x41EA10: success, 0x436CB0: failure) - a
    // base-less .lua level does the same unless its on_message answered false.
    if (!g_base && (id == kMsgComplete || id == kMsgFailed) && !(answered && refused)) {
        auto* campaign = At<void*>(kCampaign);
        if (campaign && At<int>(reinterpret_cast<uint32_t>(campaign) + 8) == 0) {
            dslog::Write("Script: %s - message 0x%X, mission %s", g_name.c_str(), id, id == kMsgComplete ? "complete" : "failed");
            reinterpret_cast<void(__thiscall*)(void*, int)>(kCampaignResult)(campaign, id == kMsgComplete ? 2 : 1);
        }
    }
    return result;
}

// Save games: the saver writes its sections into a buffer (saver [0x62F458] +0x88) and hands the cursor to save(buf) /
// load(buf), which return it advanced past the script's block. Block = "LUA1", u32 length, game.state as Lua text.
constexpr uint32_t kSaveMagic = 0x3141554C;   // "LUA1"
constexpr size_t kSaveMax = 64 * 1024;

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
const std::string& CachedSaveText() {
    if (!g_saveTextValid) {
        g_saveText = SaveText();
        g_saveTextValid = true;
    }
    return g_saveText;
}
void* __cdecl ScriptSave(void* buffer) {
    if (g_base) buffer = BaseCall<void*>(3, buffer);
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

// ---- the `game` table ----
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
    case LUA_TSTRING: return reinterpret_cast<uint32_t>(lua_tostring(L, i));
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

template <class R>
R CallAddress(lua_State* L, uint32_t fn, int first) {
    const int n = lua_gettop(L) - first + 1;
    if (n > 12) luaL_error(L, "too many arguments (%d, max 12)", n);
    uint32_t a[12] = {};
    for (int i = 0; i < n; ++i) a[i] = ArgOf(L, first + i);
    using Fn = R(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t);
    return reinterpret_cast<Fn>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9], a[10], a[11]);
}

int LuaCall(lua_State* L) {
    const uint32_t slot = CheckSlot(L);
    lua_pushinteger(L, static_cast<int32_t>(CallAddress<uint32_t>(L, ApiFunction(slot), 2)));
    return 1;
}
int LuaCallF(lua_State* L) {
    const uint32_t slot = CheckSlot(L);
    lua_pushnumber(L, CallAddress<float>(L, ApiFunction(slot), 2));
    return 1;
}

// The campaign objective list (cdecl functions of the exe the .si files use; DS1's API +0x4B0 / 4B4 / 4B8 / 4C0 /
// 4C8): game._objective(op, ...) - "add" (title hash) -> index | nil, "describe" (i, hash), "show" (i), "complete" (i),
// "hide" (i), "reopen" (i), "attach" (i, script id), "marker" (i, x, y, z).
uint8_t* ObjectiveEntry(int index) {
    auto* campaign = At<void*>(kCampaign);
    return campaign ? reinterpret_cast<uint8_t*(__thiscall*)(void*, char)>(kObjFind)(campaign, static_cast<char>(index))
                    : nullptr;
}
int LuaObjective(lua_State* L) {
    const std::string op = luaL_checkstring(L, 1);
    if (op == "add") {
        const int i = reinterpret_cast<int(__cdecl*)(uint32_t)>(kObjAdd)(static_cast<uint32_t>(luaL_checkinteger(L, 2))) & 0xFF;
        if (i == 0xFF) lua_pushnil(L);
        else lua_pushinteger(L, i);
        return 1;
    }
    const int i = static_cast<int>(luaL_checkinteger(L, 2));
    if (op == "describe") reinterpret_cast<void(__cdecl*)(int, uint32_t)>(kObjDescription)(i, static_cast<uint32_t>(luaL_checkinteger(L, 3)));
    else if (op == "show") reinterpret_cast<void(__cdecl*)(int)>(kObjShow)(i);
    else if (op == "complete") reinterpret_cast<void(__cdecl*)(int)>(kObjComplete)(i);
    else if (op == "attach") reinterpret_cast<void(__cdecl*)(int, int)>(kObjAttach)(i, static_cast<int>(luaL_checkinteger(L, 3)));
    else if (op == "marker") {
        float p[3] = {static_cast<float>(luaL_checknumber(L, 3)), static_cast<float>(luaL_checknumber(L, 4)),
                      static_cast<float>(luaL_checknumber(L, 5))};
        reinterpret_cast<void(__cdecl*)(int, float*)>(kObjMarker)(i, p);
    } else if (op == "hide" || op == "reopen" || op == "flags") {
        uint8_t* e = ObjectiveEntry(i);   // entry +0x14 flags: 2 shown, 4 done, 8 tracked (compass)
        if (!e) return 0;
        if (op == "hide") e[0x14] = static_cast<uint8_t>(e[0x14] & ~0x0A);
        else if (op == "reopen") e[0x14] = static_cast<uint8_t>((e[0x14] & ~0x04) | 0x0A);
        else {
            lua_pushinteger(L, e[0x14]);
            return 1;
        }
    } else {
        return luaL_error(L, "game._objective: unknown operation %s", op.c_str());
    }
    return 0;
}

// Script texts: game.text(name, text) -> a catalog hash (0xC1xxxxxx, from the name) the game's text lookup answers
// (menus.cpp's lookup hook, own texts first).
std::map<uint32_t, std::unique_ptr<std::wstring>> g_texts;
uint32_t TextHash(const char* name) {
    uint32_t h = 2166136261u;
    for (const char* c = name; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    return 0xC1000000u | (h & 0x00FFFFFFu);
}
int LuaText(lua_State* L) {
    const uint32_t hash = TextHash(luaL_checkstring(L, 1));
    size_t n = 0;
    const char* utf8 = luaL_checklstring(L, 2, &n);
    auto text = std::make_unique<std::wstring>(n + 1, L'\0');
    const int w = MultiByteToWideChar(CP_UTF8, 0, utf8, static_cast<int>(n), text->data(), static_cast<int>(n + 1));
    text->resize(w > 0 ? static_cast<size_t>(w) : 0);
    features::menus::AddText(hash, text->c_str());
    g_texts[hash] = std::move(text);   // the old text (if any) stays alive until the next level: the game may hold it
    lua_pushinteger(L, hash);
    return 1;
}
std::vector<std::unique_ptr<std::wstring>> g_oldTexts;

// game._message(hash, ms): the message bar (FUN_00443820, as an objective's activation text)
int LuaMessage(lua_State* L) {
    reinterpret_cast<void(__cdecl*)(uint32_t, int)>(kShowMessage)(static_cast<uint32_t>(luaL_checkinteger(L, 1)),
                                                               static_cast<int>(luaL_optinteger(L, 2, 4000)));
    return 0;
}

// game.set_level_bounds(min_x, min_y, min_z, max_x, max_y, max_z): API +0x7C set_map_bounds(float[3]* max, float[3]* min)
int LuaLevelBounds(lua_State* L) {
    float lo[3], hi[3];
    for (int i = 0; i < 3; ++i) {
        lo[i] = static_cast<float>(luaL_checknumber(L, 1 + i));
        hi[i] = static_cast<float>(luaL_checknumber(L, 4 + i));
    }
    Api<int>(0x7C, hi, lo);
    return 0;
}

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

// game.object(id): the object for a script id (FUN_004cf4f0 on [0x6800E0]) or 0; game.position(id) / player_position(n):
// the object's render instance (+0x30) position (+0x108, or FUN_004d2520 when it hangs on something: +0x164), cm.
uint8_t* ObjectById(int id) {
    void* mgr = At<void*>(kObjects);
    if (!mgr) return nullptr;
    __try {
        return reinterpret_cast<uint8_t*(__thiscall*)(void*, int)>(kObjectById)(mgr, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool PositionOf(const uint8_t* obj, float out[3]) {
    __try {
        auto* inst = obj ? *reinterpret_cast<uint8_t* const*>(obj + 0x30) : nullptr;
        if (!inst) return false;
        if (*reinterpret_cast<const int*>(inst + 0x164) == 0) memcpy(out, inst + 0x108, 12);
        else reinterpret_cast<void(__thiscall*)(void*, float*)>(kInstancePos)(inst, out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int PushPosition(lua_State* L, const uint8_t* obj) {
    float p[3];
    if (!PositionOf(obj, p)) return 0;
    lua_pushnumber(L, p[0]);
    lua_pushnumber(L, p[1]);
    lua_pushnumber(L, p[2]);
    return 3;
}
int LuaObject(lua_State* L) {
    lua_pushinteger(L, reinterpret_cast<uintptr_t>(ObjectById(static_cast<int>(luaL_checkinteger(L, 1)))));
    return 1;
}
int LuaPosition(lua_State* L) { return PushPosition(L, ObjectById(static_cast<int>(luaL_checkinteger(L, 1)))); }
// player n's soldier: input block 0x62FF7C + n * 0x58C, soldier +0x2EC
uint8_t* PlayerSoldier(int n) {
    if (n < 0 || n > 3) return nullptr;
    return At<uint8_t*>(0x62FF7C + n * 0x58C + 0x2EC);
}
int LuaPlayerSoldier(lua_State* L) {
    lua_pushinteger(L, reinterpret_cast<uintptr_t>(PlayerSoldier(static_cast<int>(luaL_optinteger(L, 1, 0)))));
    return 1;
}
int LuaPlayerPosition(lua_State* L) { return PushPosition(L, PlayerSoldier(static_cast<int>(luaL_optinteger(L, 1, 0)))); }
// health: object +0x58 (float; soldiers, vehicles - as API +0x... "get health" FUN_00446610)
int LuaHealth(lua_State* L) {
    const uint8_t* o = ObjectById(static_cast<int>(luaL_checkinteger(L, 1)));
    if (!o) return 0;
    lua_pushnumber(L, *reinterpret_cast<const float*>(o + 0x58));
    return 1;
}
int LuaTimeMs(lua_State* L) {
    const uint8_t* t = At<uint8_t*>(kGameTimer);
    lua_pushinteger(L, t ? *reinterpret_cast<const int*>(t + 0x10) : 0);
    return 1;
}

int LuaSetNextLevel(lua_State* L) {
    g_nextLevel = luaL_checkstring(L, 1);
    return 0;
}
int LuaNextMission(lua_State* L) {
    lua_pushstring(L, features::CampaignNext(g_name.c_str()));
    return 1;
}
int LuaReady(lua_State* L) {   // the event queue exists (timers can be armed): after the level's first update
    lua_pushboolean(L, g_updates > 0);
    return 1;
}

#ifndef DS_DIST
// dev builds: game._dev_save(name) = the pause menu's SAVE GAME into Documents\SCi\CDSII\<name>.sav (Save_Game
// FUN_0041c600 thiscall(saver [0x62F458], const wchar_t* path without .sav, 0)) - for scripted save / load tests.
int LuaDevSave(lua_State* L) {
    wchar_t docs[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", docs, MAX_PATH);
    std::wstring path = std::wstring(docs) + L"\\Documents\\SCi\\CDSII";
    CreateDirectoryW((std::wstring(docs) + L"\\Documents\\SCi").c_str(), nullptr);
    CreateDirectoryW(path.c_str(), nullptr);
    const std::string name = luaL_checkstring(L, 1);
    path += L"\\" + std::wstring(name.begin(), name.end());
    void* saver = At<void*>(0x62F458);
    const int r = saver ? reinterpret_cast<int(__thiscall*)(void*, const wchar_t*, int)>(0x41C600)(saver, path.c_str(), 0) : 0;
    dslog::Write("[dev] Script: saved game %s -> %d", name.c_str(), r);
    lua_pushinteger(L, r);
    return 1;
}
#endif

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

// DS1's prelude (games/ds1/src/features/scripthost.cpp) with DS2's slots.
constexpr const char* kPrelude = R"lua(
local call = game.call
game.GAME = "ds2"
function game.anim(state, file) return call(0x04, state, file) end            -- a scripted animation state
function game.load_sound_bank(file) return call(0x34, file) end
function game.load_scene(file) return call(0x70, file) end                    -- the level's scene (.env)
function game.load_map_image(file, image) call(0x74, file); return call(0x78, image or 0) end
function game.load_strings(file) return call(0x80, file) end                  -- the mission's texts (M01Strs.loc)
function game.set_time_of_day(hour) return call(0xA4, hour + 0.0) end
function game.get_time_of_day() return game.callf(0xA8) end
function game.set_mission_index(i) return call(0x590, i) end                  -- campaign +4 (Mission01 = 0)
-- briefing texts (catalog hashes): slot 0 title, 1 date, 2 place, 3 and up the next free story page
function game.set_briefing_text(text_hash, slot) return call(0x5D8, text_hash, slot) end
-- objectives (the campaign's list, as the objectives screen and the compass show it): add_objective(title[,
-- description]) -> index (nil if refused); title / description = text hashes (game.text) - a string is turned into one
local function hash_of(t, tag)
    if type(t) == "string" then return game.text("objective." .. tag .. "." .. t, t) end
    return t
end
function game.add_objective(title, description)
    local i = game._objective("add", hash_of(title, "title"))
    if i == nil then return nil end
    if description then game._objective("describe", i, hash_of(description, "text")) end
    return i
end
function game.show_objective(index) return game._objective("show", index) end      -- shown + tracked
function game.hide_objective(index) return game._objective("hide", index) end
function game.complete_objective(index) return game._objective("complete", index) end  -- sound, done, untracked
function game.reopen_objective(index) return game._objective("reopen", index) end
function game.objective_flags(index) return game._objective("flags", index) end   -- 2 shown, 4 done, 8 tracked
function game.attach_objective(index, id) return game._objective("attach", index, id) end
function game.objective_marker(index, x, y, z) return game._objective("marker", index, x, y, z) end
-- mission end: 2 = success, 1 = failure (API +0x58C). After a success the end screen's Continue goes to
-- set_next_level's level, else the next mission of the level's custom campaign (campaign.txt), else the front end.
function game.set_next_level(name) return game._set_next_level(name) end
function game.next_mission() return game._next_mission() end
function game.end_mission(result) return call(0x58C, result) end
function game.debug_print(text) return call(0x5DC, "%s", text) end
function game.activate(id) return call(0xFC, id) end
function game.deactivate(id) return call(0x100, id) end
game.layers = {}
function game.show_layer(name) for _, id in ipairs(game.layers[name] or {}) do call(0xFC, id) end end
function game.hide_layer(name) for _, id in ipairs(game.layers[name] or {}) do call(0x100, id) end end
function game.set_team(id, team) return call(0x118, id, team) end
-- regions: spheres {x, y, z, radius = r} and boxes {x, y, z, yaw = deg, min = {x, y, z}, max = {x, y, z}}; _frame()
-- sends on_message(game.EVENT.ENTERED / LEFT, region name, player 0-3)
game.regions = {}
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
-- timers (API +0x6CC (id, float ms, 0, repeat) -> on_message(game.EVENT.TIMER, id)); set during init() they are armed
-- on the level's first frame (the event queue isn't there yet). A repeating timer is re-armed each time it fires.
local pending_timers, repeating_timers = {}, {}
local function arm(id, seconds) return call(0x6CC, id, seconds * 1000.0, 0, 0) end
function game.timer(id, seconds, repeating)
    repeating_timers[id] = repeating and seconds or nil
    if not game._ready() then pending_timers[#pending_timers + 1] = { id, seconds }; return 0 end
    return arm(id, seconds)
end
function game.stop_timer(id) repeating_timers[id] = nil end
function _timer_fired(id) local s = repeating_timers[id]; if s then arm(id, s) end end
function _frame()
    if #pending_timers > 0 then
        local list = pending_timers
        pending_timers = {}
        for _, t in ipairs(list) do arm(t[1], t[2]) end
    end
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
-- persistent script state (saved games): nil, booleans, numbers, strings and tables of those; max 64 KB as text
game.state = {}
function game.loading_save() return call(0x4D0) ~= 0 end
local function serialize(v, depth)
    local t = type(v)
    if t == "number" or t == "string" then return string.format("%q", v) end
    if t == "boolean" or t == "nil" then return tostring(v) end
    if t ~= "table" then error("game.state can't hold a " .. t) end
    if depth > 16 then error("game.state is nested too deeply") end
    local parts = {}
    for k, val in pairs(v) do parts[#parts + 1] = "[" .. serialize(k, depth + 1) .. "]=" .. serialize(val, depth + 1) end
    return "{" .. table.concat(parts, ",") .. "}"
end
function _save_state() return "return " .. serialize(game.state, 0) end
function _load_state(text)
    local chunk = load(text, "=save", "t", {})
    game.state = chunk and chunk() or {}
end
-- the pause menu (and SAVE / LOAD GAME from it): API +0x560 ([0x6315B4]); on after init() for a .lua level
function game.allow_pause(on) return call(0x560, on == false and 0 or 1) end
function game.float(bits) return string.unpack("<f", string.pack("<i4", bits)) end
function game.distance(id, x, y, z)
    local ax, ay, az = game.position(id); if not ax then return math.huge end
    return math.sqrt((ax - x) ^ 2 + (ay - y) ^ 2 + (az - z) ^ 2)
end
function game.text(name, text) return game._text(name, text) end
function game.popup(text, seconds)
    local h = type(text) == "string" and game._text("popup." .. text, text) or text
    return game._message(h, math.floor((seconds or 4) * 1000))
end
-- message ids (on_message(id, a, b, c, d, e, param)) known in DS2 - they are not DS1's numbers
game.EVENT = {
    COMPLETE = 0x11,          -- the mission is complete (.si MISSION_COMPLETE result, debug key): default = success
    FAILED = 0x12,            -- the mission failed (.si MISSION_FAILED, squad dead): default = failure
    TIMER = 0x16,             -- a = timer id (game.timer)
    OBJECTIVE_DONE = 0x4F,    -- a = .si objective index (a base mission's objectives)
    ENTERED = 0x100,          -- a region name, b player 0-3 (the host's own, see game.regions)
    LEFT = 0x101,
}
game.EVENT_ENTERED, game.EVENT_LEFT = 0x100, 0x101
)lua";

void OpenLibs(lua_State* L) {
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
                                    {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_COLIBNAME, luaopen_coroutine}};
    for (const auto& lib : libs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    for (const char* unsafe : {"dofile", "loadfile"}) {
        lua_pushnil(L);
        lua_setglobal(L, unsafe);
    }
    static const luaL_Reg game[] = {{"call", LuaCall},
                                    {"callf", LuaCallF},
                                    {"set_level_bounds", LuaLevelBounds},
                                    {"read32", LuaRead<uint32_t>},
                                    {"readf", LuaRead<float>},
                                    {"read8", LuaRead<uint8_t>},
                                    {"log", LuaLog},
                                    {"_objective", LuaObjective},
                                    {"_text", LuaText},
                                    {"_message", LuaMessage},
                                    {"_set_next_level", LuaSetNextLevel},
                                    {"_next_mission", LuaNextMission},
                                    {"_ready", LuaReady},
                                    {"object", LuaObject},
                                    {"position", LuaPosition},
                                    {"health", LuaHealth},
                                    {"player_soldier", LuaPlayerSoldier},
                                    {"player_position", LuaPlayerPosition},
                                    {"time_ms", LuaTimeMs},
#ifndef DS_DIST
                                    {"_dev_save", LuaDevSave},
#endif
                                    {nullptr, nullptr}};
    luaL_newlib(L, game);
    lua_setglobal(L, "game");
    lua_pushcfunction(L, LuaLog);
    lua_setglobal(L, "print");
}

bool EndsWith(const char* s, const char* tail) {
    const size_t n = strlen(s), m = strlen(tail);
    return n >= m && _stricmp(s + n - m, tail) == 0;
}

std::string Directive(const std::string& source, const char* key) {
    const std::string head = source.substr(0, 2048);
    const size_t at = head.find(key);
    if (at == std::string::npos) return {};
    const size_t b = head.find_first_not_of(" \t", at + strlen(key));
    if (b == std::string::npos) return {};
    const size_t e = head.find_first_of(" \t\r\n", b);
    return head.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    return dir.substr(0, dir.find_last_of("\\/"));
}

// A .lua level's text: the mod's file, else one in the game folder.
bool ReadScriptFile(const char* name, std::string& out) {
    std::string path = features::ModFilePath(name);
    if (path.empty()) path = GameDir() + "\\" + name;
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    out.resize(GetFileSize(f, nullptr));
    DWORD got = 0;
    const bool ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    CloseHandle(f);
    return ok;
}

// API +0x70 (load scene) while the base DLL sets itself up: this mission's scene instead of the DLL's own. The base's
// level information (<scene>.si: areas, objectives - its script calls them by index) is loaded by LoadEnvironment only
// for levels in the campaign flow; a .lua level isn't, so the base scene's .si is read here (FUN_00432cc0 on 0x631848).
constexpr uint32_t kApiLoadScene = kApiTable + 0x70;
constexpr uint32_t kLevelInfo = 0x631848, kLevelInfoRead = 0x432CC0;
uint32_t g_loadScene = 0;
int __cdecl BaseLoadScene(const char* scene) {
    g_baseScene = scene ? scene : "";
    const std::string use = g_scene.empty() ? g_baseScene : g_scene;
    dslog::Write("Script: base mission loads %s -> %s", g_baseScene.c_str(), use.c_str());
    const int r = reinterpret_cast<int(__cdecl*)(const char*)>(g_loadScene)(use.c_str());
    if (At<uint32_t>(kLevelInfo) == 0 && At<uint32_t>(kLevelInfo + 0x58) == 0 && !g_baseScene.empty()) {
        std::string si = g_baseScene.substr(0, g_baseScene.find_last_of('.')) + ".si";
        const int ok = reinterpret_cast<int(__thiscall*)(uint32_t, const char*)>(kLevelInfoRead)(kLevelInfo, si.c_str());
        dslog::Write("Script: level information %s %s (%u objectives)", si.c_str(), ok ? "loaded" : "NOT loaded",
                     At<uint32_t>(kLevelInfo + 0x58));
    }
    return r;
}

bool LoadBase(const std::string& dll) {
#ifdef DS2_PSE_HOST
    patch::OwnerScope owner(pse_host::Find("ds2fix.scripthost"));
    g_loadScene = At<uint32_t>(kApiLoadScene);
    patch::HookSlot(kApiLoadScene, reinterpret_cast<const void*>(&BaseLoadScene), reinterpret_cast<void**>(&g_loadScene));
#else
    g_loadScene = At<uint32_t>(kApiLoadScene);
    patch::WriteValue(kApiLoadScene, reinterpret_cast<uint32_t>(&BaseLoadScene));
#endif
    At<ScriptObject*>(kScriptObject) = nullptr;
    const bool ok = reinterpret_cast<bool(__stdcall*)(const char*)>(kLoadMissionDll)(dll.c_str());
#ifdef DS2_PSE_HOST
    patch::UnhookSlot(kApiLoadScene, reinterpret_cast<const void*>(&BaseLoadScene));
#else
    patch::WriteValue(kApiLoadScene, g_loadScene);
#endif
    g_base = ok ? At<ScriptObject*>(kScriptObject) : nullptr;
    if (!g_base) features::ModLog("[fail] script %s: base mission %s didn't load", g_name.c_str(), dll.c_str());
    else dslog::Write("Script: base mission %s runs under %s", dll.c_str(), g_name.c_str());
    return g_base != nullptr;
}

void DetachPluginLua() {
#ifdef DS2_PSE_HOST
    if (!pse::lua_bridge::g_state) return;
    pse::lua_bridge::LevelEnded(g_name.c_str());
    pse::lua_bridge::SetState(nullptr);
#endif
}

void CloseLua() {
    DetachPluginLua();
    if (g_lua) lua_close(g_lua);
    g_lua = nullptr;
}

int RunLuaLevel(const char* name) {
    CloseLua();
    g_name = name;
    g_failed = false;
    g_saveText.clear();
    g_saveTextValid = false;
    g_base = nullptr;
    g_scene.clear();
    g_baseScene.clear();
    g_updates = 0;
    g_loadingSave = false;
    g_luaLevel = true;
    g_nextLevel.clear();
    g_chainFrom = name;

    std::string source;
    if (!ReadScriptFile(name, source)) {
        features::ModLog("[fail] script %s: file not found", name);
        return 0;
    }
    lua_State* L = luaL_newstate();
    g_lua = L;
    OpenLibs(L);
#ifdef DS2_PSE_HOST
    const size_t pluginFunctions = pse::lua_bridge::Install(L);
    pse::lua_bridge::SetState(L);
    if (pluginFunctions) dslog::Write("Script: %u plugin Lua function(s) installed", static_cast<unsigned>(pluginFunctions));
#endif
    lua_getglobal(L, "math");
    lua_getfield(L, -1, "randomseed");
    lua_pushinteger(L, static_cast<lua_Integer>(GetTickCount()));
    if (lua_pcall(L, 1, 0, 0) != LUA_OK) lua_pop(L, 1);
    lua_pop(L, 1);
    if (luaL_loadbuffer(L, kPrelude, strlen(kPrelude), "=prelude") != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        Report("prelude", L);
        return 0;
    }
    const std::string base = Directive(source, "--@base");
    if (!base.empty()) {
        g_scene = Directive(source, "--@scene");
        if (g_scene.empty()) {   // the mission's own <name>.ENV when a mod has it, else the base's scene
            const std::string own = g_name.substr(0, g_name.size() - 4) + ".ENV";
            if (!features::ModFilePath(own.c_str()).empty()) g_scene = own;
        }
        if (!LoadBase(base)) return 0;
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
    g_loadingSave = Api<int>(0x4D0) != 0;
    if (!g_base) At<ScriptObject*>(kScriptObject) = nullptr;   // init()'s set-up messages go nowhere (as LevelSetup)
    if (!CallGlobal("init", 0, 0) && g_failed) return 0;
    if (!g_base) Api<int>(0x560, 1);   // pause menu allowed (game.allow_pause)
    At<ScriptObject*>(kScriptObject) = &g_object;
    dslog::Write("Script: %s running (%s)%s", name, g_base ? ("on " + base).c_str() : "no base mission",
                 g_loadingSave ? " - loading a save" : "");
#ifdef DS2_PSE_HOST
    pse::lua_bridge::LevelStarted(name);
#endif
    return 1;
}

int __fastcall LoadTextLevel(void* loader, void*, const char* name) {
    if (name && EndsWith(name, ".lua")) return RunLuaLevel(name);
    return reinterpret_cast<int(__thiscall*)(void*, const char*)>(kTextLevelLoad)(loader, name);
}

// Level unload (FUN_00422090, from 0x40D7A8): the game clears the script object only with a module handle - a
// .lua level without a base has none, so the object is taken back here (the next level, or the front end, must not
// call into a finished script) and the Lua state closed.
void __fastcall UnloadScript(void* self, void*) {
    reinterpret_cast<void(__thiscall*)(void*)>(kUnloadScript)(self);
    if (At<ScriptObject*>(kScriptObject) == &g_object) At<ScriptObject*>(kScriptObject) = nullptr;
    if (g_luaLevel) {
        dslog::Write("Script: %s unloaded", g_name.c_str());
        CloseLua();
        g_luaLevel = false;
        g_base = nullptr;
        for (auto& [hash, text] : g_texts) g_oldTexts.push_back(std::move(text));
        g_texts.clear();
    }
}

// ---- the next level after a .lua mission ----
// The end screen's Continue (and the films) advance the campaign flow (FUN_00430fe0). A custom level (flow index -1)
// would go to flow entry 0 (the start-up films); after a .lua level the flow goes to set_next_level's level, else the
// level's custom campaign's next mission, else the front end. A failed mission also returns to the front end.
uint8_t* Flow() { return reinterpret_cast<uint8_t*>(kFlow); }
int FlowIndex() { return *reinterpret_cast<int*>(Flow() + 0x10); }

void StartLevelByName(const std::string& name) {
    if (!reinterpret_cast<int(__thiscall*)(void*, const char*, int)>(kFlowSelect)(Flow(), name.c_str(), 0))
        reinterpret_cast<void(__thiscall*)(void*, const char*)>(kFlowSetName)(Flow(), name.c_str());
    *reinterpret_cast<int*>(Flow() + 8) = 0;   // not a one-mission (training) run
    *reinterpret_cast<int*>(Flow() + 0x14) = 2;
}

void __fastcall FlowAdvance(void* flow, void*) {
    if (FlowIndex() == -1 && !g_chainFrom.empty()) {
        const std::string from = g_chainFrom;
        g_chainFrom.clear();
        auto* campaign = At<uint8_t*>(kCampaign);
        const int result = campaign ? *reinterpret_cast<int*>(campaign + 8) : 0;
        std::string next = g_nextLevel.empty() ? features::CampaignNext(from.c_str()) : g_nextLevel;
        g_nextLevel.clear();
        if (result == 2 && !next.empty() && _stricmp(next.c_str(), "FrontEnd") != 0 &&
            _stricmp(next.c_str(), "FrontEnd.dll") != 0) {
            dslog::Write("Script: %s done - next level %s", from.c_str(), next.c_str());
            StartLevelByName(next);
            return;
        }
        dslog::Write("Script: %s %s - back to the front end", from.c_str(), result == 2 ? "done" : "ended");
        reinterpret_cast<void(__thiscall*)(void*)>(kFlowFrontEnd)(flow);
        return;
    }
    reinterpret_cast<void(__thiscall*)(void*)>(kFlowAdvance)(flow);
}

// The level dispatcher (0x42CF3B) asks FUN_0040c830 for a level name when the flow has no entry and no custom name
// (+0x120 = 0) - its level-select dialog of Pivotal's tools. A restart of a custom level (the end screen / pause menu:
// FUN_00430e70 -> FUN_00430ab0(-1) clears +0x120) ends up there: the custom name the flow still holds is used again.
// LOAD GAME (0x45EC51 Save_Load, then FUN_00430ec0(campaign +0x18 = the saved level's name)) of a custom level's save
// ends up there too: the campaign's level name (restored from the save) comes first, then the flow's.
const char* __cdecl CustomLevelName() {
    auto* campaign = At<char*>(kCampaign);
    const char* saved = campaign ? campaign + 0x18 : nullptr;
    const char* own = reinterpret_cast<const char*>(Flow() + 0x20);
    const char* use = saved && saved[0] ? saved : own;
    if (use[0]) {
        dslog::Write("Script: level %s again (restart / loaded save; flow name %s)", use, own);
        return use;
    }
    return reinterpret_cast<const char*(__cdecl*)()>(kCustomNameSource)();
}

// The front-end frame (0x40E6E0) stores what the current screen's update returned in the flow's state [0x63161C]
// (0x40E73C: call FUN_0043fb50 = state -> screen -> vtable +0x10); 2 = load the flow's level (as the START GAME path's
// last screen returns). A level started from our menus (custom campaigns) answers 2 once.
constexpr uint32_t kFrontEndUpdateCall = 0x40E73C, kFrontEndUpdate = 0x43FB50;
bool g_frontEndStart = false;
int __cdecl FrontEndUpdate() {
    const int r = reinterpret_cast<int(__cdecl*)()>(kFrontEndUpdate)();
    if (!g_frontEndStart) return r;
    g_frontEndStart = false;
    dslog::Write("Script: front end -> load %s", reinterpret_cast<const char*>(Flow() + 0x20));
    return 2;
}

// ---- "+level <name>": straight into a level (the ModKit editor's Play) ----
char g_quickLevel[64] = "";
char g_quickOptions[96] = "";
DWORD WINAPI PressPlay(LPVOID) {   // the launcher (dialog 0x6E) is OK'd once it is up (Play = IDOK)
    for (int i = 0; i < 400; ++i) {
        Sleep(50);
        HWND found = nullptr;
        EnumWindows([](HWND w, LPARAM lp) -> BOOL {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            char cls[16] = {};
            GetClassNameA(w, cls, sizeof cls);
            if (pid == GetCurrentProcessId() && !strcmp(cls, "#32770") && IsWindowVisible(w)) {
                *reinterpret_cast<HWND*>(lp) = w;
                return FALSE;
            }
            return TRUE;
        }, reinterpret_cast<LPARAM>(&found));
        if (found) {
            PostMessageA(found, WM_COMMAND, IDOK, 0);
            return 0;
        }
    }
    return 0;
}
// The start-up options parser FUN_0040b090 (cdecl(command line), called at 0x414AEE with WinMain's command line) takes
// the built-in "-fl -d" (pointer 0x60981C) only when the command line is empty - "+level x" would be read as the
// options themselves (no 'd': the locale loader then checks its source files and crashed at 0x4FC4B0). With +level the
// parser gets "-d <name>" instead.
constexpr uint32_t kOptionsParseCall = 0x414AEE, kOptionsParse = 0x40B090;
int __cdecl ParseOptions(const char* commandLine) {
    return reinterpret_cast<int(__cdecl*)(const char*)>(kOptionsParse)(g_quickOptions[0] ? g_quickOptions : commandLine);
}

void ParseCommandLine() {
    const char* cmd = GetCommandLineA();
    const char* at = cmd ? strstr(cmd, "+level ") : nullptr;
    if (!at) return;
    at += 7;
    while (*at == ' ') ++at;
    size_t n = 0;
    while (at[n] && at[n] != ' ' && at[n] != '"' && n < sizeof g_quickLevel - 1) ++n;
    const std::string name(at, n);
    const bool ok = n > 4 && (EndsWith(name.c_str(), ".lua") || EndsWith(name.c_str(), ".dll")) &&
                    name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") ==
                        std::string::npos;
    if (!ok) {
        dslog::Write("[fail] +level %s: not a level name (letters, digits, _ . ending in .lua or .dll)", name.c_str());
        return;
    }
    const uint32_t expected = kOptionsDefault;
    if (!patch::Matches(kOptionsPtr, &expected, 4)) {
        dslog::Write("[fail] +level %s: the start-up options are already changed", name.c_str());
        return;
    }
    snprintf(g_quickOptions, sizeof g_quickOptions, "-d %s", name.c_str());
    const char* opts = g_quickOptions;
    patch::WriteValue(kOptionsPtr, opts);
    strcpy_s(g_quickLevel, name.c_str());
    if (!patch::HookCall(kOptionsParseCall, reinterpret_cast<const void*>(&ParseOptions), kOptionsParse))
        dslog::Write("[fail] +level: start-up options parser at 0x%08X not recognised", kOptionsParseCall);
    DWORD background = 0, size = sizeof background;   // dev background test runs OK the launcher themselves (devbg.cpp)
    RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "Background", RRF_RT_REG_DWORD, nullptr, &background,
                 &size);
    if (!background)
        if (HANDLE t = CreateThread(nullptr, 0, PressPlay, nullptr, 0, nullptr)) CloseHandle(t);
    dslog::Write("Script: +level %s - straight into the level", g_quickLevel);
}
}  // namespace

// mods_levels.cpp at each level's first archive open: a .lua level's "--@archive <level>" (`isLua` = the level is a
// .lua one; "" = no line: then "none" when the level has no archive of its own). Also resets the end-screen chain for
// any other level.
std::string features::ScriptArchiveFor(const char* level, bool& isLua) {
    isLua = level && EndsWith(level, ".lua");
    if (!isLua) {
        g_chainFrom.clear();
        return {};
    }
    std::string source;
    if (!ReadScriptFile(level, source)) return {};
    std::string value = Directive(source, "--@archive");
    if (value.size() > 4 && _stricmp(value.c_str() + value.size() - 4, ".dat") == 0) value.resize(value.size() - 4);
    return value;
}

bool features::ScriptLevelActive() { return g_luaLevel && g_lua && !g_failed; }
bool features::ScriptHasBase() { return g_luaLevel && g_base != nullptr; }
const char* features::QuickStartLevel() { return g_quickLevel[0] ? g_quickLevel : nullptr; }

void features::StartCustomLevel(const char* name) {
    if (!name || !*name) return;
    StartLevelByName(name);
    g_frontEndStart = true;
}

void features::ApplyScriptHost() {
    if (!settings::Get().featMods) {
        dslog::Write("[off]  Script host: .lua levels need the Mods folder (FeatMods)");
        return;
    }
    ParseCommandLine();
#ifdef DS2_PSE_HOST
    if (!pse::lua_bridge::g_api && !pse::lua_bridge::Attach(pse::LoadInterfaceFor(pse_host::Core())))
        dslog::Write("Script host: no pse.lua.host from the loader - plugin Lua functions unavailable");
#endif
    int ok = 0, total = 0;
    auto hook = [&](uint32_t site, const void* fn, uint32_t target) {
        ++total;
        if (patch::HookCall(site, fn, target)) ++ok;
        else dslog::Write("[fail] Script host: call at 0x%08X not recognised", site);
    };
    hook(kTextLevelCall, reinterpret_cast<const void*>(&LoadTextLevel), kTextLevelLoad);
    hook(kUnloadCall, reinterpret_cast<const void*>(&UnloadScript), kUnloadScript);
    hook(kCustomNameCall, reinterpret_cast<const void*>(&CustomLevelName), kCustomNameSource);
    hook(kFrontEndUpdateCall, reinterpret_cast<const void*>(&FrontEndUpdate), kFrontEndUpdate);
    for (uint32_t site : kAdvanceCalls) hook(site, reinterpret_cast<const void*>(&FlowAdvance), kFlowAdvance);
    dslog::Write("%s Script host: .lua levels - %d of %d hooks", ok == total ? "[ok]  " : "[fail]", ok, total);
    ApplyCampaigns();
}
