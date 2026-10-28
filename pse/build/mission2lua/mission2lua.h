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

// ---- stage 2: readable Lua (C++ port of ModKit conflict_ds_tools/mission2lua_readable.py; gate scripts/m2r_gate.py) -----
// The same DLL gives the same text as the Python, byte for byte, so a mod's patch written against the ModKit's output
// applies to the player's (docs/sdk/reference/mission-patches.md).
int ReadableVersion();   // = mission2lua_readable.VERSION
struct SceneRefs {       // mission2lua_readable.refs_from_env: number -> editor ID
    std::map<int64_t, std::string> objects, waypoints, regions;
};
// pse::records' scene reader (pse/records/placed.cpp) - the same names the ModKit's refs_from_env gives
bool SceneRefsFromEnv(const std::string& envBytes, SceneRefs* out);
// One file out of the install's archives by name: the first *.dat (PSE\Originals + the game folder, lower-case name
// order) that has it - the ModKit's archive.GameFiles.
bool ReadArchiveFile(const std::wstring& gameDir, const std::string& name, std::string* out);
// TranslateReadable's sceneRefs for an install: the scene out of its archives -> SceneRefsFromEnv
std::function<bool(const std::string&, SceneRefs*)> SceneRefsFromGame(const std::wstring& gameDir);
// Lua source "return {objects = {[EDITOR_ID] = n}, waypoints, regions, cameras}" with EVERY editor ID of the scene (the
// readable mission's obj / wp / rgn only hold the ones its own logic names) - the SCENE table of mission patches.
std::string SceneTableLua(const std::string& envBytes);

// The readable translation cache (<missions cache>\readable): <stem>.lua (the bundle) + <stem>.stamp
// ("dll <sha256> <size>\nreadable <ReadableBuild()>\nscene <scene>\n"); regenerated when the DLL or the translator changes.
const char* ReadableBuild();   // "<mission2lua_readable VERSION>+cpp.<port revision>"
struct ReadableCache {
    bool ok = false, generated = false;
    std::string sha256, bundle, scene, error;
    std::wstring luaPath;
    double ms = 0;
};
ReadableCache EnsureReadableCached(const std::wstring& dllPath, const std::wstring& dir, const std::wstring& gameDir,
                                   bool force);

// ---- mission patches (docs/sdk/reference/mission-patches.md; m2r_patch.cpp) --------------------------------------------
// A <Stem>.patch.lua: header lines "--@patches <Stem>.dll" + "--@readable <ReadableVersion()>", edit blocks
// "--[[@edit / <lines of the readable mission> / --@to / <their replacement> / ]]" (any long-bracket level), and Lua that
// runs in the mission's environment before its init().
struct PatchEdit {
    std::vector<std::string> from, to;
    int line = 0;   // the block's first line in the patch
};
struct MissionPatch {
    std::string path, label;   // full path, a short name for messages ("<mod>\<file>")
    std::string text;          // the file, LF line ends
    std::vector<PatchEdit> edits;
};
std::string NormalizePatchText(const std::vector<uint8_t>& raw);   // CRLF -> LF, BOM off
// Reads the header + edit blocks into p->edits. False + *why: not for this DLL / another version / a broken block.
bool ParseMissionPatch(const std::string& dllName, MissionPatch* p, std::string* why);
// Applies every edit of p (each must match exactly one place) - all of them or none (*bundle unchanged on false).
bool ApplyMissionPatchEdits(std::string* bundle, const MissionPatch& p, std::string* why);
// Adds "_G.__pse_mission_env = _ENV" before start.lua's part of the bundle (the patches' environment, read by the host).
bool HandOutMissionEnv(std::string* bundle);
// Lua run in the mission's environment before the patches: TEXT / MOVIE (text key hashes), X (= X86RT).
const char* MissionPatchPrelude();
struct ReadableParts {
    std::string mission, refs, events, runtime, start, bundle;   // mission.lua, refs.lua, scripts/*.lua, <Mission>.lua
    std::string scene;                                           // the scene file LevelSetup loads ("" = none found)
};
// sceneRefs(scene, &refs): the level's editor IDs for the scene the DLL loads (false / unset = numbers stay numbers).
// level = the --@archive name ("" = the DLL name's stem, lower case).
bool TranslateReadable(const std::vector<uint8_t>& dll, const std::string& name, const std::string& level,
                       const std::function<bool(const std::string& scene, SceneRefs* refs)>& sceneRefs,
                       ReadableParts* out, std::string* error);

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
