// Translated missions in play (docs/internal/mission-translation.md "In the game", DESIGN.md §12.3): a shipped mission DLL
// (Mission*.dll / Training*.dll) runs as its stage-1 Lua translation instead of loading the DLL.
//   - Players: setting "TranslatedMissions" of plugin dsfix.missionsource (OPTIONS > DESERTSTORMFIX > PLUGINS,
//     MISSIONS FROM LUA, default OFF, restart). Dev builds: Dev\MissionSource = "lua" / "dll" overrides it.
//
//   - Generation: pse_build's C++ translator (pse/build/mission2lua, byte-identical with ModKit mission2lua.py) writes
//     <game>\PSE\Cache\missions\<Mission>.lua + .stamp (else %LOCALAPPDATA%\PSE\ds1-<hash>\missions) for every
//     mission DLL at start-up (TranslateAll, before the launcher); the stamp = the DLL's SHA-256 + the translator
//     build, so a changed DLL or a new translator regenerates it. Dev\PseRebuild (missing = 1 in dev builds): 1 =
//     regenerate every mission at each game start.
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
//   - Mod missions (any build, any setting): Mods\<mod>\<Stem>.lua with "--@replaces <Stem>.dll" in its header runs
//     in this host instead of that DLL (ModReplacement) - e.g. an edited readable mission. A mod's own DLL still wins.
//   - The state is closed at the next DLL level start (missionrt has detached the image at "pse.lua.level_end",
//     fired by scripthost's level init).
// Not covered: the loadout screen's OVERVIEW replay looks for the builder in the loaded mission module ([0x60F57C],
// overview.cpp) - with a translated mission there is none (the automatic overview after the briefing still runs).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
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
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"
#include "lua_bridge.h"
#include "mission2lua.h"

namespace {

constexpr uint32_t kLoadMissionDllCall = 0x40C3EB;   // level init: call FUN_00449260 (".dll" level names only)
constexpr uint32_t kLoadMissionDll = 0x449260;       // bool stdcall(name)

// Mission DLLs whose translation passed the parity runs (docs/internal/missions.md: API trace + world hashes identical to
// the DLL). SHA-256 of the shipped file (Steam = GOG).
const struct {
    const char* name;
    const char* sha256;
} kVerified[] = {
    {"Mission1.dll", "258e6d8a2e7e2a0079120fa8f5f770b759835985adce34bcb60f2934ce915f67"},
    {"Mission2.dll", "6b19033082315c188274b88f9c0f9402cbe80ba574e8850a69d0a825dcdc1b85"},
    {"Mission3.dll", "4bf673839c30e03533b04e313869e181e357453c74801315300eddec9092d13e"},
    {"Mission4.dll", "d0003a447123ab25ad02c261726333a7dfdf65793c2768bfda7507396b38f43f"},
    {"Mission5a.dll", "181f41c2c5d26ffc901106961529b2f19de8fe0c64cd4ba1af9bed31dbd993cf"},
    {"Mission5B.dll", "5e5e0ffcf3fc1b65a7d6668f04a8cb80f56dd4c00ba1698247b791f6fd699a08"},
    {"Mission6.dll", "7888576bbac62d53a4ce05b77cd2315c4068fd41fcf6d985a872dd7380bb86d8"},
    {"Mission7.dll", "01a6c88deb090c3bb0bf5bb073e1a84f585d6b802779c8f8e5119aec1f02d775"},
    {"Mission8.dll", "4deede471e3008efc525e83e9cd9088d13529e1b3ab3922504ad042ba90ec408"},
    {"Mission9.dll", "8c01434bf7c6eeb0dd08be2caaa2a8947197b4e74ea2cc3230517179310e59ff"},
    {"mission10.dll", "4fc333abbd17d588e9db5c801b0591e7cbf1c100eca1726fb785d676aa8d7850"},
    {"Mission11a.dll", "a641b6e9a09677b8ad831fa9ab4aaddbec61b094d27ddbf25a35676b4df51973"},
    {"Mission11b.dll", "9aa3d6fc663b743d6d36485e3f2dcb57ef24aee5479f366712f5b970fb97c1a7"},
    {"Mission12a.dll", "71c7e962ec21339b417cc2bd169c996a9f997f6c9b9059250169b3346ae7a6df"},
    {"Training1.dll", "2ffa8331271157a1576917f0433c6715d38315921c9d2407abef53b4b48d9fea"},
    {"Mission12B.dll", "7bb1fe2687ac4c026f811bd7621b5ce273d1f8710916dc69bd9b34a049924ce6"},
    {"Training2.dll", "b8ada149647608e1de16177b252c497da4e9f694c9fc1087a5b7a43d08f016c7"},
    {"Training3.dll", "1925cd81b1e5fbe40ecf314327197086cc364e1595a4a6bc2618726d8f8f9ba7"},
};

enum class Source { Dll, Lua, Both };
Source g_source = Source::Dll;
bool g_unverified = false;
int g_rebuild = 0;
std::set<std::string> g_rebuilt;   // missions regenerated in this run (Dev\PseRebuild)
std::map<std::string, pse::m2l::CacheResult> g_boot;   // lower-case DLL name -> the start-up translation
std::wstring g_game, g_cacheDir;
lua_State* g_L = nullptr;
const char* g_how = nullptr;   // the overlay's "mission script:" (features::MissionScriptText)
std::wstring g_lastLua;        // the .lua RunTranslated started last          // the translated mission's state (closed at the next DLL level)

bool DevValue(const char* name, DWORD* dw, std::string* sz) {   // release builds never read Dev\ values
#ifdef DS_DIST
    (void)name, (void)dw, (void)sz;
    return false;
#else
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
#endif
}

// ---- player setting (plugin dsfix.missionsource, shown in OPTIONS > DESERTSTORMFIX > PLUGINS) ----
constexpr const char* kSetOn = "TranslatedMissions";

PSEPluginHandle Self() {
    static PSEPluginHandle h = pse_host::Find("dsfix.missionsource");
    return h;
}

bool PlayerSetting() {
    const PSESettingsInterface* set = pse::SettingsInterface();
    if (!set || !Self()) return false;
    PSESettingDesc d{};
    d.size = sizeof d;
    d.key = kSetOn;
    d.type = PSE_SETTING_BOOL;
    d.flags = 0;        // restart
    d.defaultInt = 0;   // OFF: the translation behaves like the DLL (docs/internal/missions.md) but isn't play-tested yet
    d.minInt = 0;
    d.maxInt = 1;
    d.label = "MISSIONS FROM LUA";
    d.help = "runs the game's missions as Lua made from your own game files - same behaviour; an unchecked mission keeps its DLL";
    if (set->Declare(Self(), &d) != PSE_OK) dslog::Write("[fail] Mission source: setting %s not declared", kSetOn);
    return set->GetInt(Self(), kSetOn) != 0;
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
    g_lastLua = luaPath;
    dslog::Write("Mission source: %s runs from Lua (%s)", name, Narrow(luaPath).c_str());
    return true;
}

// A mod's mission Lua that takes the place of a shipped mission DLL: Mods\<mod>\<Stem>.lua whose header (the lines
// before the first line that isn't a "--" comment) has "--@replaces <Stem>.dll". Typically a readable mission
// (ModKit ds_mission2lua_readable) edited by a mod, rebuilt from the player's own DLL - see docs/sdk "Mission
// replacements". "" = none.
std::string ModReplacement(const char* name) {
    const std::string stem = std::string(name).substr(0, strlen(name) - 4);
    const std::string path = features::ModFilePath((stem + ".lua").c_str());
    if (path.empty()) return {};
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") != 0 || !f) return {};
    char buf[4096] = {};
    const size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    const std::string text(buf, n);
    size_t at = 0;
    while (at < text.size() && text.compare(at, 2, "--") == 0) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (Lower(line) == Lower("--@replaces " + std::string(name))) return path;
        at = end + 1;
    }
    return {};
}

int __stdcall LoadMissionLevelImpl(const char* name) {
    using LoadFn = int(__stdcall*)(const char*);
    const auto original = reinterpret_cast<LoadFn>(kLoadMissionDll);
    CloseState();   // the last translated mission's state (its level has ended)
    if (!IsMissionDll(name)) return original(name);
    if (features::ModFilePath(name).empty()) {   // a mod's DLL wins over everything
        const std::string mod = ModReplacement(name);
        if (!mod.empty()) {
            dslog::Write("Mission source: %s replaced by the mod mission %s", name, mod.c_str());
            features::ModLog("%s runs as the mod mission %s", name, mod.c_str());
            const std::wstring wmod(mod.begin(), mod.end());
            if (RunTranslated(name, wmod)) return 1;
            features::ModLog("[fail] %s: the mod mission %s failed to start (see DesertStormFix.log) - the game's DLL runs",
                             name, mod.c_str());
            return original(name);
        }
    }
    if (g_source == Source::Dll) return original(name);
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
    std::wstring dllDir = g_game;   // the game folder, else PSE\Originals (originals.cpp moved it there)
    HANDLE h = FindFirstFileW((dllDir + L"\\" + wname).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) h = FindFirstFileW(((dllDir = g_game + L"\\PSE\\Originals") + L"\\" + wname).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        // No DLL in the game folder: the cached translation carries the whole image, so it runs on its own when its
        // stamp names a verified DLL (by name + SHA-256) and this translator build.
        const std::string stem = std::string(name).substr(0, strlen(name) - 4);
        const std::wstring wstem(stem.begin(), stem.end());
        std::vector<uint8_t> stamp;
        if (!pse::m2l::ReadFileBytes(g_cacheDir + L"\\" + wstem + L".stamp", &stamp)) return original(name);
        const std::string text(stamp.begin(), stamp.end());
        char sha[65] = {}, build[64] = {};
        unsigned size = 0;
        const size_t t = text.find("translator ");
        if (sscanf_s(text.c_str(), "dll %64s %u", sha, unsigned(sizeof sha), &size) != 2 || t == std::string::npos ||
            sscanf_s(text.c_str() + t, "translator %63s", build, unsigned(sizeof build)) != 1 ||
            strcmp(build, pse::m2l::TranslatorBuild()) != 0) {
            dslog::Write("Mission source: %s isn't in the game folder and its cached translation is out of date", name);
            return original(name);
        }
        bool verified = false;
        for (const auto& v : kVerified)
            if (_stricmp(v.name, name) == 0 && strcmp(v.sha256, sha) == 0) verified = true;
        if (!verified && !g_unverified) return original(name);
        dslog::Write("Mission source: %s isn't in the game folder - running its cached translation", name);
        if (!RunTranslated(name, g_cacheDir + L"\\" + wstem + L".lua")) return original(name);
        return 1;
    }
    FindClose(h);
    const std::wstring dllPath = dllDir + L"\\" + fd.cFileName;
    const std::string disk = Narrow(fd.cFileName);
    // translated at start-up (TranslateAll); a DLL that appeared since is translated now
    const auto booted = g_boot.find(Lower(disk));
    const bool force = booted == g_boot.end() && g_rebuild >= 1 && !g_rebuilt.count(Lower(disk));
    const pse::m2l::CacheResult r =
        booted != g_boot.end() ? booted->second : pse::m2l::EnsureCached(dllPath, g_cacheDir, force);
    if (!r.ok) {
        dslog::Write("[fail] Mission source: %s: translation failed (%s) - the DLL runs", name, r.error.c_str());
        return original(name);
    }
    if (r.generated && booted == g_boot.end()) {   // (start-up translations were logged by TranslateAll)
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

// What the performance overlay shows for the running mission (features::MissionScriptText).
int __stdcall LoadMissionLevel(const char* name) {
    g_how = nullptr;
    g_lastLua.clear();
    const int result = LoadMissionLevelImpl(name);
    if (IsMissionDll(name)) {
        const bool cached = !g_lastLua.empty() && !g_cacheDir.empty() &&
                            _wcsnicmp(g_lastLua.c_str(), g_cacheDir.c_str(), g_cacheDir.size()) == 0;
        const std::wstring wname(name, name + strlen(name));
        const bool dllHere = GetFileAttributesW((g_game + L"\\" + wname).c_str()) != INVALID_FILE_ATTRIBUTES ||
                             !features::OriginalPath(name).empty();   // (moved to PSE\Originals still counts)
        g_how = !g_L ? "DLL" : !cached ? "Lua (mod mission)" : dllHere ? "Lua (translated)" : "Lua (translated, no DLL)";
    }
    return result;
}

// Every Mission*.dll / Training*.dll of the game folder translated during start-up (before the launcher), so levels
// only load the cached Lua. Unchanged stamps (DLL SHA-256 + translator build) are kept; dev builds regenerate all
// while Dev\PseRebuild >= 1. ~1 s for the 18 shipped DLLs the first time, ~0.1 s (hashing) after.
void TranslateAll() {
    const DWORD t0 = GetTickCount();
    int made = 0, kept = 0, failed = 0;
    for (const std::wstring& path : pse::m2l::MissionDlls(g_game)) {
        const std::string disk = Narrow(path.substr(path.find_last_of(L"\\/") + 1));
        const pse::m2l::CacheResult r = pse::m2l::EnsureCached(path, g_cacheDir, g_rebuild >= 1);
        if (!r.ok) {
            ++failed;
            dslog::Write("[fail] Mission source: %s: translation failed (%s) - it runs as the DLL", disk.c_str(),
                         r.error.c_str());
            continue;
        }
        g_boot[Lower(disk)] = r;
        g_rebuilt.insert(Lower(disk));
        r.generated ? ++made : ++kept;
    }
    dslog::Write("Mission source: start-up translation by mission2lua %s: %d translated, %d unchanged, %d failed (%u ms)",
                 pse::m2l::TranslatorBuild(), made, kept, failed, GetTickCount() - t0);
}

}  // namespace

const char* features::MissionScriptText() { return g_how; }

std::string features::MissionReplacement(const char* dllName) {
    return dllName && IsMissionDll(dllName) ? ModReplacement(dllName) : std::string();
}

void features::ApplyMissionSource() {
    static bool applied = false;   // restart setting: decided once
    if (applied) return;
    applied = true;
    DWORD dw = 0;
    std::string v;
    const bool player = PlayerSetting();   // declared always (the PLUGINS menu lists it)
    const bool dev = DevValue("MissionSource", &dw, &v);
    if (dev) {
        v = Lower(v);
        g_source = v == "lua" || v == "1" ? Source::Lua : v == "both" || v == "2" ? Source::Both : Source::Dll;
    } else {
        g_source = player ? Source::Lua : Source::Dll;
    }
    if (g_source == Source::Dll) {
        // mods may still replace a mission with their own Lua (ModReplacement); nothing is translated
        if (!patch::HookCall(kLoadMissionDllCall, reinterpret_cast<const void*>(&LoadMissionLevel), kLoadMissionDll))
            dslog::Write("[fail] Mission source: level-init hook not installed - mod mission replacements are off");
        return;
    }
    dw = 0;
    g_unverified = DevValue("MissionUnverified", &dw, nullptr) && dw != 0;
#ifdef DS_DIST
    g_rebuild = 0;   // players: regenerated only when the DLL or the translator changes (the stamp)
#else
    dw = 1;
    g_rebuild = DevValue("PseRebuild", &dw, nullptr) ? static_cast<int>(dw) : 1;
#endif
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_game = exe;
    g_game.resize(g_game.find_last_of(L"\\/"));
    g_cacheDir = pse::m2l::DefaultMissionsDir(g_game);
    if (!patch::HookCall(kLoadMissionDllCall, reinterpret_cast<const void*>(&LoadMissionLevel), kLoadMissionDll)) {
        dslog::Write("[fail] Mission source: level-init hook not installed - mission DLLs run as DLLs");
        return;
    }
    TranslateAll();
    dslog::Write("[ok]   Mission source: %s = %s (translator %s, cache %s%s%s)",
                 dev ? "Dev\\MissionSource" : "MISSIONS FROM LUA", g_source == Source::Lua ? "lua" : "both",
                 pse::m2l::TranslatorBuild(), Narrow(g_cacheDir).c_str(),
                 g_rebuild >= 1 ? ", all regenerated at start-up" : "", g_unverified ? ", unverified DLLs allowed" : "");
}
