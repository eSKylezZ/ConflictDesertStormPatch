// Translated missions in play - DEV ONLY for now (docs/sdk/mission-translation.md "In the game", DESIGN.md §12.3):
// Dev\MissionSource = "lua" runs a shipped mission DLL (Mission*.dll / Training*.dll) as its stage-1 Lua translation
// instead of loading the DLL. Default "dll" (= nothing changes); release builds never read it.
//
//   - Generation: pse_build's C++ translator (pse/build/mission2lua, byte-identical with ModKit mission2lua.py) writes
//     <game>\PSE\Cache\missions\<Mission>.lua + .stamp (else %LOCALAPPDATA%\PSE\ds1-<hash>\missions) the first time
//     the level starts; the stamp = the DLL's SHA-256 + the translator build, so a changed DLL or a new translator
//     regenerates it. Dev\PseRebuild (missing = 1 in dev builds): 1 = regenerate each mission once per game start.
//   - The hook: level init FUN_0040c090 calls FUN_00449260(name) (LoadLibrary + LevelSetup(&api) -> [0x60F578],
//     module [0x60F57C]) for ".dll" levels at 0x40C3EB. For a translated mission the hook runs the Lua instead, in its
//     own state: pse.lua namespaces (x86rt.install of dsfix.missionrt), the chunk, init() (the DLL image mapped by
//     missionrt, DllMain + LevelSetup emulated), then "pse.lua.level_start" - missionrt writes the DLL's own script
//     object to [0x60F578]. The level stays a ".dll" level for everything else (archives, loadout, scripthost):
//     the DLL's own init registers all its animations, the game calls the DLL's update / message / save / load
//     callbacks through their thunks, so saves are the DLL's bytes (old saves load either way).
//   - Fallback per mission to the DLL: a mod ships the DLL (Mods\ wins), the DLL's SHA-256 isn't in the verified list
//     (kVerified - passed the parity runs; Dev\MissionUnverified = 1 allows others), generation fails, or the Lua
//     fails to load / init. Logged "Mission source: ...".
//   - Dev\MissionSource = "both" (DLL runs, Lua shadowed, DESIGN §12.3) is not built: logged, the DLL runs.
//   - The state is closed at the next DLL level start (missionrt has detached the image at "pse.lua.level_end",
//     fired by scripthost's level init).
// Not covered: the loadout screen's OVERVIEW replay looks for the builder in the loaded mission module ([0x60F57C],
// overview.cpp) - with a translated mission there is none (the automatic overview after the briefing still runs).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "../../third_party/lua/lauxlib.h"
#include "../../third_party/lua/lua.h"
#include "../../third_party/lua/lualib.h"
}

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "lua_bridge.h"
#include "mission2lua.h"

#ifndef DS_DIST
namespace {

constexpr uint32_t kLoadMissionDllCall = 0x40C3EB;   // level init: call FUN_00449260 (".dll" level names only)
constexpr uint32_t kLoadMissionDll = 0x449260;       // bool stdcall(name)

// Mission DLLs whose translation passed the parity runs (docs/sdk/missions.md). SHA-256 of the shipped file
// (Steam = GOG).
const struct {
    const char* name;
    const char* sha256;
} kVerified[] = {
    {"Mission1.dll", "258e6d8a2e7e2a0079120fa8f5f770b759835985adce34bcb60f2934ce915f67"},
};

enum class Source { Dll, Lua, Both };
Source g_source = Source::Dll;
bool g_unverified = false;
int g_rebuild = 0;
std::set<std::string> g_rebuilt;   // missions regenerated in this run (Dev\PseRebuild)
std::wstring g_game, g_cacheDir;
lua_State* g_L = nullptr;          // the translated mission's state (closed at the next DLL level)

bool DevValue(const char* name, DWORD* dw, std::string* sz) {
    char buf[64] = {};
    DWORD size = sizeof buf, type = 0;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", name, RRF_RT_REG_DWORD | RRF_RT_REG_SZ, &type,
                     buf, &size) != ERROR_SUCCESS)
        return false;
    if (type == REG_DWORD) {
        std::memcpy(dw, buf, 4);
        if (sz) *sz = std::to_string(*dw);
    } else if (sz) {
        *sz = buf;
    }
    return true;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

bool IsMissionDll(const char* name) {
    const std::string n = Lower(name ? name : "");
    if (n.size() < 5 || n.compare(n.size() - 4, 4, ".dll") != 0 || n.find_first_of("\\/") != std::string::npos) return false;
    return n.rfind("mission", 0) == 0 || n.rfind("training", 0) == 0;
}

int Traceback(lua_State* L) {
    luaL_traceback(L, L, lua_tostring(L, 1), 1);
    return 1;
}
int LuaLog(lua_State* L) {
    std::string line;
    for (int k = 1; k <= lua_gettop(L); ++k) {
        if (k > 1) line += ' ';
        line += luaL_tolstring(L, k, nullptr);
        lua_pop(L, 1);
    }
    dslog::Write("Mission source: [lua] %s", line.c_str());
    return 0;
}

void CloseState() {
    if (!g_L) return;
    if (pse::lua_bridge::g_state == g_L) {   // not ended yet (shouldn't happen: level init ended it)
        pse::lua_bridge::LevelEnded("translated mission");
        pse::lua_bridge::SetState(nullptr);
    }
    lua_close(g_L);
    g_L = nullptr;
}

// Runs the translated mission. False = nothing installed (the caller falls back to the DLL).
bool RunTranslated(const char* name, const std::wstring& luaPath) {
    std::vector<uint8_t> src;
    if (!pse::m2l::ReadFileBytes(luaPath, &src)) {
        dslog::Write("[fail] Mission source: %s: can't read %s", name, Narrow(luaPath).c_str());
        return false;
    }
    lua_State* L = luaL_newstate();
    static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base},          {LUA_TABLIBNAME, luaopen_table},
                                    {LUA_STRLIBNAME, luaopen_string},   {LUA_MATHLIBNAME, luaopen_math},
                                    {LUA_UTF8LIBNAME, luaopen_utf8},    {LUA_COLIBNAME, luaopen_coroutine},
                                    {nullptr, nullptr}};
    for (const luaL_Reg* l = libs; l->func; ++l) {
        luaL_requiref(L, l->name, l->func, 1);
        lua_pop(L, 1);
    }
    lua_newtable(L);   // game.log: the generated init() reports through it
    lua_pushcfunction(L, LuaLog);
    lua_setfield(L, -2, "log");
    lua_setglobal(L, "game");
    lua_pushcfunction(L, LuaLog);
    lua_setglobal(L, "print");
    pse::lua_bridge::Install(L);   // x86rt.install (dsfix.missionrt) + every plugin namespace
    pse::lua_bridge::SetState(L);
    g_L = L;
    auto fail = [&](const char* what) {
        dslog::Write("[fail] Mission source: %s: %s: %s - the DLL runs instead", name, what,
                     lua_tostring(L, -1) ? lua_tostring(L, -1) : "?");
        pse::lua_bridge::LevelEnded(name);   // missionrt frees an image init() may have mapped
        pse::lua_bridge::SetState(nullptr);
        lua_close(L);
        g_L = nullptr;
        return false;
    };
    const std::string chunk = "@" + Narrow(luaPath);
    lua_pushcfunction(L, Traceback);
    if (luaL_loadbuffer(L, reinterpret_cast<const char*>(src.data()), src.size(), chunk.c_str()) != LUA_OK)
        return fail("load");
    if (lua_pcall(L, 0, 0, 1) != LUA_OK) return fail("run");
    lua_getglobal(L, "init");
    if (lua_pcall(L, 0, 0, 1) != LUA_OK) return fail("init");
    lua_settop(L, 0);
    pse::lua_bridge::LevelStarted(name);   // missionrt: the DLL's script object -> [0x60F578]
    if (*reinterpret_cast<uint32_t*>(0x60F578) == 0) {
        lua_pushstring(L, "no script object after level start");
        return fail("start");
    }
    dslog::Write("Mission source: %s runs from Lua (%s)", name, Narrow(luaPath).c_str());
    return true;
}

int __stdcall LoadMissionLevel(const char* name) {
    using LoadFn = int(__stdcall*)(const char*);
    const auto original = reinterpret_cast<LoadFn>(kLoadMissionDll);
    CloseState();   // the last translated mission's state (its level has ended)
    if (g_source == Source::Dll || !IsMissionDll(name)) return original(name);
    if (g_source == Source::Both) {
        dslog::Write("[warn] Mission source: Dev\\MissionSource = both isn't built yet - %s runs as the DLL", name);
        return original(name);
    }
    if (!features::ModFilePath(name).empty()) {
        dslog::Write("Mission source: %s comes from a mod - the DLL runs", name);
        return original(name);
    }
    // the file as on disk (its name goes into the generated header)
    WIN32_FIND_DATAW fd;
    std::wstring wname(name, name + strlen(name));
    HANDLE h = FindFirstFileW((g_game + L"\\" + wname).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return original(name);
    FindClose(h);
    const std::wstring dllPath = g_game + L"\\" + fd.cFileName;
    const std::string disk = Narrow(fd.cFileName);
    const bool force = g_rebuild >= 1 && !g_rebuilt.count(Lower(disk));
    const pse::m2l::CacheResult r = pse::m2l::EnsureCached(dllPath, g_cacheDir, force);
    if (!r.ok) {
        dslog::Write("[fail] Mission source: %s: translation failed (%s) - the DLL runs", name, r.error.c_str());
        return original(name);
    }
    if (r.generated) {
        g_rebuilt.insert(Lower(disk));
        dslog::Write("Mission source: %s translated by mission2lua %s in %.0f ms (%u functions, %u instructions)%s",
                     disk.c_str(), pse::m2l::TranslatorBuild(), r.stats.ms, r.stats.functions, r.stats.instructions,
                     force ? " (Dev\\PseRebuild)" : "");
    }
    bool verified = false;
    for (const auto& v : kVerified)
        if (_stricmp(v.name, disk.c_str()) == 0 && r.sha256 == v.sha256) verified = true;
    if (!verified && !g_unverified) {
        dslog::Write("Mission source: %s (SHA-256 %s) isn't in the verified list - the DLL runs (Dev\\MissionUnverified = 1 "
                     "runs it anyway)", disk.c_str(), r.sha256.substr(0, 16).c_str());
        return original(name);
    }
    if (!RunTranslated(name, r.luaPath)) return original(name);
    return 1;
}

}  // namespace
#endif  // DS_DIST

void features::ApplyMissionSource() {
#ifndef DS_DIST
    DWORD dw = 0;
    std::string v;
    if (DevValue("MissionSource", &dw, &v)) {
        v = Lower(v);
        g_source = v == "lua" || v == "1" ? Source::Lua : v == "both" || v == "2" ? Source::Both : Source::Dll;
    }
    if (g_source == Source::Dll) return;
    dw = 0;
    g_unverified = DevValue("MissionUnverified", &dw, nullptr) && dw != 0;
    dw = 1;
    g_rebuild = DevValue("PseRebuild", &dw, nullptr) ? static_cast<int>(dw) : 1;
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_game = exe;
    g_game.resize(g_game.find_last_of(L"\\/"));
    g_cacheDir = pse::m2l::DefaultMissionsDir(g_game);
    if (!patch::HookCall(kLoadMissionDllCall, reinterpret_cast<const void*>(&LoadMissionLevel), kLoadMissionDll)) {
        dslog::Write("[fail] Mission source: level-init hook not installed - mission DLLs run as DLLs");
        return;
    }
    dslog::Write("[ok]   Mission source: Dev\\MissionSource = %s (translator %s, cache %s%s%s)",
                 g_source == Source::Lua ? "lua" : "both", pse::m2l::TranslatorBuild(), Narrow(g_cacheDir).c_str(),
                 g_rebuild >= 1 ? ", regenerated once per start" : "", g_unverified ? ", unverified DLLs allowed" : "");
#endif
}
