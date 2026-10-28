// Lua mission scripts (tgefix.scripthost): a level whose name ends in ".lua" is run by an embedded Lua 5.4 - port of
// DesertStorm2Fix's scripthost.cpp (games/ds2/src/features/scripthost.cpp; DS2 addresses in brackets), with
// campaigns (campaign.txt) and films (game.film). Modder page: docs/sdk/reference/game-tge.md "Lua missions".
//
// How GreatEscape.exe runs a level script (mapped 2026-10-25): Level_Init 0x5D1DE0 (cdecl(name)) picks by the name's
// last 3 letters: "dll" -> the loader FUN_00590b90 (stdcall(loader [0x7DC240], name) ret 8: LoadLibraryA, LevelSetup(&api
// table 0x65C1A8) -> script object [0x7DC0F4], module [0x7DC0F8]); "env" -> nothing; anything else -> the text manifest
// CLoader::Load FUN_00590e10 (esi = [0x7DC240], edx = name, plain ret, eax 0 = Level_Init fails) at 0x5D22B7 [0x412C51] -
// hooked here: ".lua" names run the script instead. Level_Init then goes on as for a DLL (octree of the scene the
// script's init loaded through API +0x14 - a level without a scene fails there).
// The API table: 698 cdecl functions (0xAE8 bytes, no version check). Known slots: +0x000 load_anim(state, .prs),
// +0x004 load_sound_bank, +0x008 load_music_bank, +0x014 load_scene (LoadEnvironment 0x5D44E0), +0x018 load_strings,
// +0x01C..+0x08C standard animation sets, +0x21C set_mission_result(result, now) (2 success, 1 failure), +0x278
// timer(id, 0, ms, repeat) -> message 0x18, +0x9AC TriggerEMovie(name, n) (in-engine cutscenes).
// The script object = 6 cdecl callbacks: +0 init (LevelSetup only), +4 update() (Game_Update 0x5D55C6), +8 message(id,
// param*) (~124 senders; TGE's own message numbering), +0xC save(buffer) -> buffer + n (0x5C7429), +0x10 load(buffer) ->
// buffer + n (0x5C8B78), +0x14 size (never called). Level_Unload frees the module inline (0x5CEE50) and clears the object
// only when there is a module - the call after it (0x5CEE6C) is hooked to clear ours.
// Starting a custom level: the level flow 0x7DDAE0 (+0xC index, -1 = the custom name at +0x1C; +0x11C custom-name flag):
// GoTo(-1) FUN_005c96c0 (eax = index, esi = flow, ecx = 0) + set custom name FUN_00401b00 (esi = flow, eax = name), as
// Level_LoadDispatch 0x5D5747..0x5D5763 does with Pivotal's level dialog. The flow moves on with Advance FUN_005c9b60
// (eax = flow) after a mission's end screen / a film: its 10 calls are hooked - a script's game.next_level / the next
// mission of a campaign is started instead.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"

namespace {
constexpr uint32_t kManifestCall = 0x5D22B7, kManifestLoad = 0x590E10;
constexpr uint32_t kDllLoader = 0x590B90, kLoaderObject = 0x7DC240;   // stdcall(loader, name) ret 8
constexpr uint32_t kScriptObject = 0x7DC0F4, kScriptModule = 0x7DC0F8;
constexpr uint32_t kApiTable = 0x65C1A8, kApiSize = 0xAE8;
constexpr uint32_t kUnloadCall = 0x5CEE6C, kUnloadNext = 0x4C5370;    // first call after the inline FreeLibrary
constexpr uint32_t kFlowAdvance = 0x5C9B60;   // (flow 0x7DDAE0, GoTo 0x5C96C0, set name 0x401B00: literals in the asm)
constexpr uint32_t kAdvanceCalls[] = {0x58F6D1, 0x58F758, 0x58FB37, 0x59025E, 0x5D1093,
                                      0x5D111D, 0x5D1158, 0x5D3D21, 0x5D3DDA, 0x5D4188};
constexpr int kMsgTimer = 0x18;

lua_State* g_lua = nullptr;
std::string g_name;          // the running .lua level
bool g_failed = false;
int g_updates = 0;
std::string g_nextLevel;     // game.next_level / the campaign's next mission
std::string g_pendingStart;  // a custom level to start at the next frame (+level, campaigns)

template <class T>
T& At(uint32_t va) { return *reinterpret_cast<T*>(va); }
uint32_t ApiFunction(uint32_t slot) { return At<uint32_t>(kApiTable + slot); }

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}
bool ReadAll(const std::string& path, std::string& out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    out.resize(size);
    DWORD got = 0;
    const bool ok = size == 0 || (ReadFile(f, out.data(), size, &got, nullptr) && got == size);
    CloseHandle(f);
    return ok;
}
bool EndsWith(const char* s, const char* tail) {
    const size_t a = strlen(s), b = strlen(tail);
    return a >= b && _stricmp(s + a - b, tail) == 0;
}
// A level script's file: a mod's (by name), else the game folder's.
bool ReadScript(const std::string& name, std::string& text, std::string* path = nullptr) {
    std::string p;
    if (const char* mod = features::ModFilePath(name.c_str())) p = mod;
    else p = GameDir() + "\\" + name;
    if (!ReadAll(p, text)) return false;
    if (path) *path = p;
    return true;
}
// "--@key value" in the script's first lines.
std::string Directive(const std::string& text, const char* key) {
    const std::string head = text.substr(0, std::min<size_t>(text.size(), 4096));
    const std::string tag = std::string("--@") + key;
    const size_t at = head.find(tag);
    if (at == std::string::npos) return {};
    const size_t b = head.find_first_not_of(" \t", at + tag.size());
    if (b == std::string::npos) return {};
    const size_t e = head.find_first_of(" \t\r\n", b);
    return head.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

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

// ---- base mission ("--@base LevelA.dll") ----
struct ScriptObject {
    void* fn[6];
};
ScriptObject* g_base = nullptr;
template <typename R, typename... A>
R BaseCall(int slot, A... args) {
    return reinterpret_cast<R(__cdecl*)(A...)>(g_base->fn[slot])(args...);
}

// game.timer(id, seconds[, repeating]) -> message 0x18 (on_message / _timer_fired). The event queue the timer goes into
// ([0x79ED94], add FUN_0048bd90) doesn't exist yet during the level's init (a timer there crashed at 0x48BD9F, as on
// DS1): timers set before the first update are armed by it.
struct PendingTimer {
    int id, ms, repeat;
};
std::vector<PendingTimer> g_pendingTimers;
void ArmTimer(const PendingTimer& t) {
    reinterpret_cast<int(__cdecl*)(int, float, int, int)>(ApiFunction(0x278))(t.id, 0.0f, t.ms, t.repeat);
}
// ---- script object ----
void __cdecl ScriptInit() {}
void __cdecl ScriptUpdate() {
    if (g_updates++ == 0) {
        dslog::Write("Script: %s - first update", g_name.c_str());
        for (const PendingTimer& t : g_pendingTimers) ArmTimer(t);
        g_pendingTimers.clear();
    }
    if (g_base) BaseCall<void>(1);
    if (g_lua && !g_failed) CallGlobal("update", 0, 0);
}
int __cdecl ScriptMessage(int id, uint32_t* param) {
    int result = g_base ? BaseCall<int>(2, id, param) : 1;
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
            if (lua_isinteger(L, -1)) result = static_cast<int>(lua_tointeger(L, -1));
            else if (lua_isboolean(L, -1)) result = lua_toboolean(L, -1);
            lua_pop(L, 1);
        }
    }
    return result;
}
// Saves: the base mission's block only (TGE never asks the script for its block size, so no Lua block is added).
void* __cdecl ScriptSave(void* buffer) { return g_base ? BaseCall<void*>(3, buffer) : buffer; }
void* __cdecl ScriptLoad(void* buffer) {
    if (g_base) buffer = BaseCall<void*>(4, buffer);
    if (g_lua && !g_failed) CallGlobal("on_save_loaded", 0, 0);
    return buffer;
}
int __cdecl ScriptSaveSize() { return g_base ? BaseCall<int>(5) : 0; }
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
uint32_t CheckSlot(lua_State* L, int i = 1) {
    const lua_Integer slot = luaL_checkinteger(L, i);
    if (slot < 0 || slot >= kApiSize || slot % 4) luaL_error(L, "bad API slot 0x%X", static_cast<unsigned>(slot));
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
int LuaLog(lua_State* L) {
    std::string line;
    for (int i = 1; i <= lua_gettop(L); ++i) {
        if (i > 1) line += "\t";
        line += luaL_tolstring(L, i, nullptr);
        lua_pop(L, 1);
    }
    dslog::Write("Script %s: %s", g_name.c_str(), line.c_str());
    return 0;
}
// game.timer(id, seconds[, repeating]) -> message 0x18 (on_message / _timer_fired); see ArmTimer.
int LuaTimer(lua_State* L) {
    const PendingTimer t{static_cast<int>(luaL_checkinteger(L, 1)), static_cast<int>(luaL_checknumber(L, 2) * 1000.0),
                         lua_toboolean(L, 3) ? 1 : 0};
    if (g_updates == 0) g_pendingTimers.push_back(t);
    else ArmTimer(t);
    return 0;
}
int LuaFilm(lua_State* L) {    // game.film(name): a Bink film now, the game frozen until it ends (films.cpp)
    features::PlayFilm(luaL_checkstring(L, 1));
    return 0;
}
int LuaEMovie(lua_State* L) {  // game.emovie(name[, n]): one of the level's in-engine cutscenes (API +0x9AC)
    lua_pushinteger(L, reinterpret_cast<int(__cdecl*)(const char*, int)>(ApiFunction(0x9AC))(luaL_checkstring(L, 1),
                                                                                         static_cast<int>(luaL_optinteger(L, 2, 6))));
    return 1;
}
int LuaEndMission(lua_State* L) {   // game.end_mission(2 success | 1 failure)
    reinterpret_cast<int(__cdecl*)(int, int)>(ApiFunction(0x21C))(static_cast<int>(luaL_checkinteger(L, 1)), 0);
    return 0;
}
int LuaNextLevel(lua_State* L) {    // game.next_level(name): what the end screen's Continue starts
    g_nextLevel = luaL_checkstring(L, 1);
    return 0;
}
int LuaUpdates(lua_State* L) {
    lua_pushinteger(L, g_updates);
    return 1;
}

const char kPrelude[] = R"(
local timers = {}
function _timer_fired(id) local f = timers[id]; if f then if not f.repeating then timers[id] = nil end f.fn(id) end end
function game.after(id, seconds, fn, repeating)
  timers[id] = {fn = fn, repeating = repeating}
  game.timer(id, seconds, repeating)
end
game.load_anim = function(state, file) return game.call(0x000, state, file) end
game.load_sound_bank = function(name) return game.call(0x004, name) end
game.load_music_bank = function(name) return game.call(0x008, name) end
game.load_scene = function(name) return game.call(0x014, name) end
game.load_strings = function(name) return game.call(0x018, name) end
game.state = {}
)";

void CloseLua() {
    if (g_lua) lua_close(g_lua);
    g_lua = nullptr;
    g_base = nullptr;
}

// A ".lua" level: run its chunk + init(), the script object becomes ours. false = Level_Init fails.
bool RunLuaLevel(const char* name) {
    CloseLua();
    g_name = name;
    g_failed = false;
    g_updates = 0;
    g_nextLevel.clear();
    g_pendingTimers.clear();
    std::string text, path;
    if (!ReadScript(name, text, &path)) {
        features::ModLog("[fail] script %s: file not found (Mods folder or game folder)", name);
        return false;
    }
    g_lua = luaL_newstate();
    lua_State* L = g_lua;
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(L, 0);
    const luaL_Reg game[] = {{"call", LuaCall},          {"callf", LuaCallF},       {"read32", LuaRead<int32_t>},
                             {"read8", LuaRead<uint8_t>}, {"readf", LuaRead<float>}, {"log", LuaLog},
                             {"timer", LuaTimer},         {"film", LuaFilm},         {"emovie", LuaEMovie},
                             {"end_mission", LuaEndMission}, {"next_level", LuaNextLevel}, {"updates", LuaUpdates},
                             {nullptr, nullptr}};
    luaL_newlib(L, game);
    lua_setglobal(L, "game");
    lua_pushcfunction(L, LuaLog);
    lua_setglobal(L, "print");
    if (luaL_dostring(L, kPrelude) != LUA_OK) {
        Report("prelude", L);
        CloseLua();
        return false;
    }
    if (luaL_loadbuffer(L, text.data(), text.size(), ("@" + std::string(name)).c_str()) != LUA_OK || lua_pcall(L, 0, 0, 0) != LUA_OK) {
        Report("load", L);
        CloseLua();
        return false;
    }
    const std::string base = Directive(text, "base");
    if (!base.empty()) {   // the game's mission DLL runs underneath: its LevelSetup loads the scene and sets everything up
        const int ok = reinterpret_cast<int(__stdcall*)(void*, const char*)>(kDllLoader)(At<void*>(kLoaderObject), base.c_str());
        g_base = ok ? At<ScriptObject*>(kScriptObject) : nullptr;
        if (!g_base) {
            features::ModLog("[fail] script %s: base mission %s didn't load", name, base.c_str());
            CloseLua();
            return false;
        }
    }
    At<void*>(kScriptObject) = &g_object;
    if (!CallGlobal("init", 0, 0) && g_failed) {
        At<void*>(kScriptObject) = g_base;
        CloseLua();
        return false;
    }
    features::ModLog("Script: %s running (%s)%s%s", name, path.c_str(), base.empty() ? "" : ", base mission ", base.c_str());
    return true;
}

// At 0x5D22B7 (esi = loader, edx = level name): ".lua" names run here, everything else is the game's text manifest.
int __cdecl ManifestOrLua(const char* name) { return name && EndsWith(name, ".lua") ? (RunLuaLevel(name) ? 1 : 0) : -1; }
__declspec(naked) void ManifestStub() {
    __asm {
        push edx
        push esi
        push edx
        call ManifestOrLua
        add esp, 4
        pop esi
        pop edx
        cmp eax, -1
        je game
        ret
    game:
        mov eax, 0x590E10      // the game's manifest load (literals: a constexpr in __asm is a memory operand)
        jmp eax
    }
}

// Level_Unload, right after the inline FreeLibrary (0x5CEE6C): our object out, the Lua state closed.
void __cdecl UnloadScript() {
    if (!g_lua && !g_base) return;
    if (At<void*>(kScriptObject) == &g_object) At<void*>(kScriptObject) = nullptr;
    dslog::Write("Script: %s unloaded", g_name.c_str());
    CloseLua();
}
__declspec(naked) void UnloadStub() {
    __asm {
        pushad
        call UnloadScript
        popad
        mov eax, 0x4C5370
        jmp eax
    }
}

// ---- custom levels ----
void StartCustomLevel(const std::string& name) {
    static std::string keep;   // the flow copies the name; kept anyway
    keep = name;
    const char* n = keep.c_str();
    __asm {
        push esi
        mov esi, 0x7DDAE0      // flow; GoTo(-1) 0x5C96C0
        or eax, -1
        xor ecx, ecx
        mov edx, 0x5C96C0
        call edx
        mov eax, n             // set custom name 0x401B00
        mov esi, 0x7DDAE0
        mov edx, 0x401B00
        call edx
        pop esi
    }
    dslog::Write("Script: starting level %s", n);
}

// ---- campaigns: Mods\<mod>\campaign.txt ("name = ...", "mission = x.lua" lines) ----
struct Campaign {
    std::string name, mod;
    std::vector<std::string> missions;
};
std::vector<Campaign> g_campaigns;
const Campaign* g_campaign = nullptr;   // the one being played
const Campaign* g_pendingCampaign = nullptr;   // +campaign: becomes g_campaign when its first mission starts

void LoadCampaigns() {
    g_campaigns.clear();
    const std::string root = GameDir() + "\\Mods";
    for (const std::string& mod : features::LoadedMods()) {
        std::string text;
        if (!ReadAll(root + "\\" + mod + "\\campaign.txt", text)) continue;
        Campaign c;
        c.mod = mod;
        size_t pos = 0;
        while (pos < text.size()) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            auto trim = [](std::string s) {
                const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
                return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
            };
            const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (_stricmp(k.c_str(), "name") == 0) c.name = v;
            else if (_stricmp(k.c_str(), "mission") == 0) c.missions.push_back(v);
        }
        if (c.name.empty()) c.name = mod;
        if (c.missions.empty()) {
            features::ModLog("[fail] Campaigns: %s\\campaign.txt has no \"mission = ...\" lines", mod.c_str());
            continue;
        }
        features::ModLog("Campaigns: \"%s\" (%s) - %zu mission(s)", c.name.c_str(), mod.c_str(), c.missions.size());
        g_campaigns.push_back(std::move(c));
    }
}

// The campaign mission after `level`, or "".
std::string CampaignNext(const std::string& level) {
    if (!g_campaign) return {};
    for (size_t i = 0; i + 1 < g_campaign->missions.size(); ++i)
        if (_stricmp(g_campaign->missions[i].c_str(), level.c_str()) == 0) return g_campaign->missions[i + 1];
    return {};
}

// The flow to a level-table entry by name (FUN_005c98d0: eax = flow, (name, arg) ret 8).
void GoToEntry(const char* name) {
    __asm {
        push 0
        push name
        mov eax, 0x7DDAE0
        mov edx, 0x5C98D0
        call edx
    }
}

// The flow's Advance (eax = flow): a script's next level / the campaign's next mission instead, while a .lua level ran;
// nothing next = the front end (Advance from the custom entry -1 would go on with entry 0: the legal screen and logos).
int __cdecl NextCustomLevel() {
    if (g_name.empty()) return 0;   // not after a .lua level
    std::string next = g_nextLevel.empty() ? CampaignNext(g_name) : g_nextLevel;
    g_nextLevel.clear();
    if (next.empty()) {
        if (g_campaign) dslog::Write("Campaigns: \"%s\" finished", g_campaign->name.c_str());
        g_campaign = nullptr;
        dslog::Write("Script: %s ended - back to the front end", g_name.c_str());
        g_name.clear();
        GoToEntry("FE");
        return 1;
    }
    StartCustomLevel(next);
    return 1;
}
__declspec(naked) void AdvanceStub() {
    __asm {
        push eax
        push ecx
        push edx
        call NextCustomLevel
        test eax, eax
        pop edx
        pop ecx
        pop eax
        jnz done
        mov ecx, 0x5C9B60      // Advance
        jmp ecx
    done:
        ret
    }
}

// "+level <name>" / "+campaign <name>" on the command line: started once the front end is up.
void ParseCommandLine() {
    const std::string cmd = GetCommandLineA();
    auto arg = [&](const char* key) -> std::string {
        const size_t at = cmd.find(key);
        if (at == std::string::npos) return {};
        size_t b = cmd.find_first_not_of(' ', at + strlen(key));
        if (b == std::string::npos) return {};
        const bool quoted = cmd[b] == '"';
        if (quoted) ++b;
        const size_t e = cmd.find(quoted ? '"' : ' ', b);
        return cmd.substr(b, e == std::string::npos ? std::string::npos : e - b);
    };
    if (const std::string level = arg("+level "); !level.empty()) g_pendingStart = level;
    if (const std::string camp = arg("+campaign "); !camp.empty()) {
        for (const Campaign& c : g_campaigns)
            if (_stricmp(c.name.c_str(), camp.c_str()) == 0 || _stricmp(c.mod.c_str(), camp.c_str()) == 0) g_campaign = &c;
        if (g_campaign) g_pendingStart = g_campaign->missions[0], g_pendingCampaign = g_campaign, g_campaign = nullptr;
        else features::ModLog("[fail] Campaigns: no campaign \"%s\" (+campaign)", camp.c_str());
    }
}

// A level that isn't a .lua one (the front end, a game mission): no campaign / next level any more.
int g_frontEndFrames = 0;   // frames since the front end loaded (a pending custom level starts at 30)
int PSE_CALL OnLevelLoad(PSEEventId, void* data, void*) {
    const char* level = static_cast<const char*>(data);
    g_frontEndFrames = level && _stricmp(level, "FE") == 0 ? 1 : 0;
    if (level && EndsWith(level, ".lua")) return 0;
    if (g_campaign && level && *level) dslog::Write("Campaigns: \"%s\" left (%s loads)", g_campaign->name.c_str(), level);
    g_campaign = nullptr;
    g_nextLevel.clear();
    g_name.clear();
    return 0;
}

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    if (!g_frontEndFrames || ++g_frontEndFrames < 30 || g_pendingStart.empty()) return 0;
    g_campaign = g_pendingCampaign;
    g_pendingCampaign = nullptr;
    StartCustomLevel(g_pendingStart);
    g_pendingStart.clear();
    return 0;
}
// A custom level's text table: level init's string set-up (0x58D8A0) loads "<level name minus extension>.BLF"
// (FUN_0047F040 thiscall(catalog, name, 0), call at 0x58D8B3) and, when that file isn't found, resets the catalog to
// GenStrs.loc + InvStrs.loc - which aren't shipped (every text then read "*NO STRING*", the pause menu included). A
// ".lua" level reads its archive's / base mission's table instead ("luatest.lua" + --@base LevelA.dll -> LevelA.BLF).
constexpr uint32_t kLevelStringsCall = 0x58D8B3, kLevelStrings = 0x47F040;
int __fastcall LevelStrings(void* catalog, void*, const char* name, int flag) {
    std::string own;
    if (name && EndsWith(name, ".lua")) {
        std::string stem = name;
        stem.resize(stem.size() - 4);
        own = features::ScriptArchiveFor(stem);
        if (!own.empty() && _stricmp(own.c_str(), "none") != 0) {
            own += ".dll";   // only the extension is replaced
            static std::string logged;
            if (logged != own) dslog::Write("Lua missions: %s uses %s's texts", name, own.c_str());
            logged = own;
        } else {
            own.clear();
        }
    }
    return reinterpret_cast<int(__thiscall*)(void*, const char*, int)>(kLevelStrings)(catalog, own.empty() ? name : own.c_str(), flag);
}
}  // namespace

// The data a ".lua" level uses ("--@archive level01" / "none", else its "--@base" mission's): "" = its own name's.
std::string features::ScriptArchiveFor(const std::string& stem) {
    std::string text;
    if (!ReadScript(stem + ".lua", text)) return {};
    std::string archive = Directive(text, "archive");
    if (archive.empty()) {
        archive = Directive(text, "base");
        if (const size_t dot = archive.find_last_of('.'); dot != std::string::npos) archive.resize(dot);
    }
    if (archive.size() > 4 && _stricmp(archive.c_str() + archive.size() - 4, ".dat") == 0) archive.resize(archive.size() - 4);
    return archive;
}

std::vector<features::CustomMission> features::CustomMissions() {
    std::vector<CustomMission> out;
    for (size_t i = 0; i < g_campaigns.size(); ++i) {
        const Campaign& c = g_campaigns[i];
        out.push_back({c.name, "campaign, " + std::to_string(c.missions.size()) + " mission" + (c.missions.size() == 1 ? "" : "s"),
                       static_cast<int>(i), {}});
    }
    const std::string root = GameDir() + "\\Mods";
    for (const std::string& mod : LoadedMods()) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((root + "\\" + mod + "\\*.lua").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            bool listed = false;
            for (const Campaign& c : g_campaigns)
                for (const std::string& m : c.missions) listed = listed || _stricmp(m.c_str(), fd.cFileName) == 0;
            if (listed) continue;
            std::string name = fd.cFileName;
            name.resize(name.size() - 4);
            out.push_back({name, "mission (" + mod + ")", -1, fd.cFileName});
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return out;
}

void features::StartCustomMission(const CustomMission& m) {
    if (m.campaign >= 0) StartCampaign(m.campaign);
    else if (!m.level.empty()) StartCustomLevel(m.level);
}

void features::StartCampaign(int index) {
    if (index < 0 || index >= static_cast<int>(g_campaigns.size())) return;
    g_campaign = &g_campaigns[index];
    StartCustomLevel(g_campaign->missions[0]);
}

void features::ApplyScriptHost() {
    if (!patch::HookCall(kManifestCall, reinterpret_cast<const void*>(&ManifestStub), kManifestLoad)) {
        ModLog("[fail] Lua missions: level init's manifest call at 0x%08X not recognised - .lua levels can't run", kManifestCall);
        return;
    }
    if (!patch::HookCall(kLevelStringsCall, reinterpret_cast<const void*>(&LevelStrings), kLevelStrings))
        ModLog("[fail] Lua missions: level text set-up at 0x%08X not recognised - .lua levels may show *NO STRING*", kLevelStringsCall);
    if (!patch::HookCall(kUnloadCall, reinterpret_cast<const void*>(&UnloadStub), kUnloadNext))
        ModLog("[fail] Lua missions: level unload at 0x%08X not recognised", kUnloadCall);
    int advance = 0;
    for (uint32_t site : kAdvanceCalls) advance += patch::HookCall(site, reinterpret_cast<const void*>(&AdvanceStub), kFlowAdvance);
    if (advance != static_cast<int>(std::size(kAdvanceCalls)))
        ModLog("[fail] Lua missions: %d of %zu level-flow calls hooked - next levels / campaigns may not chain", advance,
               std::size(kAdvanceCalls));
    if (Get().mods) LoadCampaigns();
    ParseCommandLine();
    if (const PSEMessagingInterface* m = pse::MessagingInterface()) {
        m->Subscribe(pse_host::Find("tgefix.scripthost"), PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
        m->Subscribe(pse_host::Find("tgefix.scripthost"), PSE_EVENT_LEVEL_LOAD_BEGIN, &OnLevelLoad, nullptr, 0);
    }
    dslog::Write("[ok]   Lua missions: .lua levels, %zu campaign(s)%s%s", g_campaigns.size(),
                 g_pendingStart.empty() ? "" : ", starting ", g_pendingStart.c_str());
}
