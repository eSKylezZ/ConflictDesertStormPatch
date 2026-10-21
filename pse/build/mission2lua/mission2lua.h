#pragma once
// mission2lua - mission DLL -> Lua (stage 1: static recompilation), the C++ port of ModKit
// conflict_ds_tools/mission2lua.py (docs/internal/mission-translation.md, DESIGN.md §12). Used by pse_build.exe
// (`pse_build missions`) and by DesertStormFix's Dev\MissionSource (src/features/missionsource.cpp).
//
// The output is made from the player's own DLL and is game code: never commit it, never ship it.
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pse::m2l {

int TranslatorVersion();          // = mission2lua.py VERSION (the generated Lua names it; part of the cache stamp)
const char* TranslatorBuild();    // "1+cpp.1": translator version + C++ port revision (cache stamps)

struct Stats {
    uint32_t functions = 0, instructions = 0, thunks = 0, sehPairs = 0;
    std::map<std::string, int> unsupported;   // reason -> count (0 for every shipped DLL)
    std::vector<std::string> warnings;
    double ms = 0;
};

// Translates one mission DLL. `name` = its file name as on disk ("Mission1.dll": the header and the --@archive line
// use it). False + *error when the file isn't a usable 32-bit DLL (no LevelSetup export ...).
bool Translate(const std::vector<uint8_t>& dll, const std::string& name, std::string* lua, Stats* stats,
               std::string* error);

// ---- the missions cache (<game>\PSE\Cache\missions, docs/internal/asset-store.md "Missions") ---------------------------
// <stem>.lua (the generated mission) + <stem>.stamp ("dll <sha256> <size>\ntranslator <TranslatorBuild()>\n").
struct MissionFile {
    std::wstring dll;        // full path of the DLL
    std::string name;        // "Mission1.dll"
    std::string sha256;      // lower-case hex
};
std::string Sha256Hex(const std::vector<uint8_t>& data);
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out);
// Mission*.dll + Training*.dll of a game folder, name order (case-insensitive), like ds_mission2lua.py --all.
std::vector<std::wstring> MissionDlls(const std::wstring& gameDir);
// <game>\PSE\Cache\missions when it can be written, else %LOCALAPPDATA%\PSE\ds1-<install hash>\missions.
std::wstring DefaultMissionsDir(const std::wstring& gameDir);

struct CacheResult {
    bool ok = false;          // the .lua is there and matches the DLL + translator
    bool generated = false;   // it was (re)made now
    std::string sha256;       // the DLL's
    std::wstring luaPath;
    std::string error;
    Stats stats;
};
// Makes sure <dir>\<stem>.lua is the translation of `dllPath` by this translator (stamp check; force = always redo).
CacheResult EnsureCached(const std::wstring& dllPath, const std::wstring& dir, bool force);

// Optional syntax check of the generated Lua (pse_build links Lua; empty = no checker set). Returns "" or the error.
extern std::function<std::string(const std::string& lua, const std::string& chunk)> g_luaCheck;

}  // namespace pse::m2l
