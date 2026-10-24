#pragma once
// PSE loader internals (NOT part of the public SDK). Used by the loader's own services, the game layer and the
// DesertStormFix features that are compiled into the same DLL during phase 1.
#include <cstdint>
#include <string>
#include <vector>

#include "pse/pse_api.h"
#include "host_api.h"   // private host ABI (phase 2 layout): shim / game layers / host plugins

struct HKEY__;  // <windows.h> HKEY

namespace pse {

struct Config {
    // The game layer's id ("ds1", "ds2" ...): plugins' `games` lists and the settings key. Required - Init refuses a
    // missing one (lower-case letters / digits, at most 16).
    const char* gameId = nullptr;
    // Registry key (under HKCU) that plugin settings live under: <registryRoot>\Plugins\<plugin name>. Empty =
    // "Software\PSE\<gameId>". Tests point this at a throw-away key.
    std::string registryRoot;
    // Folder for <plugin>.log files ("" = no files, lines only go to the sink).
    std::string logDir;
    // Receives every formatted log line (already prefixed) - DesertStormFix routes it into DesertStormFix.log.
    void (*logSink)(const char* line) = nullptr;
    // Receives Report() lines (mod report file). error != 0 = problem.
    void (*reportSink)(const char* line, int error) = nullptr;
};

// Start-up. Call once, on the game thread (it becomes the "game thread" for pse.tasks). Safe to call again after
// ResetForTests(). false = refused (no / bad game id; config.logSink gets the reason): nothing is initialised.
bool Init(const Config& config);
// Tests only: forget every plugin, listener, hook record, setting, task (does NOT restore patched memory).
void ResetForTests();

// Plugins compiled into the loader (phase 1: DesertStormFix features). The info struct must outlive the process.
PSEPluginHandle RegisterInternalPlugin(const PSEPluginInfo* info);
// The PSELoadInterface handed to a plugin (its Query resolves loader + registered interfaces).
const PSELoadInterface* LoadInterfaceFor(PSEPluginHandle plugin);
const char* PluginName(PSEPluginHandle plugin);       // "" for unknown handles
PSEPluginHandle FindPlugin(const char* name);         // 0 = none
uint32_t PluginCount();
const PSEPluginInfo* PluginInfo(PSEPluginHandle plugin);

// Loader-side log line (sender "pse").
void LogLine(PSEPluginHandle plugin, PSELogLevel level, const char* text);

// Game layer hooks into the loader:
void Frame();                                                    // once per game frame: tasks, then PSE_EVENT_FRAME
void Broadcast(uint32_t type, const void* data, uint32_t len);   // loader message ("pse") to every listener
int RaiseEvent(PSEEventId event, void* data);                    // as the loader

// Interface tables (also reachable through Query with the PSE_*_INTERFACE ids).
const PSELogInterface* LogInterface();
const PSEHooksInterface* HooksInterface();
const PSEMessagingInterface* MessagingInterface();
const PSETasksInterface* TasksInterface();
const PSESettingsInterface* SettingsInterface();
const PSESerializationInterface* SerializationInterface();

// pse.settings storage key of a plugin, under HKCU: "<registryRoot>\Plugins\<name>" ("" for unknown handles). Host
// code may keep its own undeclared values there (raw registry; Declare ignores names it wasn't given).
std::string PluginSettingsKey(PSEPluginHandle plugin);

// One-time copy of a host's old settings key into its new place (settings_migrate.cpp). Every value (all names, all
// registry types) of the sources is copied into dst unless dst already has that name; the first source that has a
// name wins. Then `marker` (REG_DWORD 1) is written into dst. Nothing happens when dst already has the marker; the
// sources are never changed. A failed write leaves the marker unset (the copy is retried at the next start).
struct RegistrySource {
    HKEY__* root;           // HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE ...
    std::string path;
    uint32_t extraAccess;   // ORed into KEY_READ (e.g. KEY_WOW64_32KEY), usually 0
};
struct MigrationResult {
    bool ran = false;                    // false: the marker was already there (or dst couldn't be opened: ok false)
    bool ok = false;                     // every copy + the marker written
    int sourcesFound = 0;                // sources that could be opened
    std::vector<std::string> copied;     // "Name (source n)"
    std::vector<std::string> kept;       // names dst already had (left alone)
    std::vector<std::string> failed;     // names whose write failed
};
MigrationResult MigrateRegistryValues(const std::vector<RegistrySource>& sources, HKEY__* dstRoot,
                                      const std::string& dstPath, const char* marker);

// Co-save, driven by the game layer around the game's own save / load.
namespace cosave {
bool Save(const char* coSavePath);    // runs every plugin's save callback, keeps chunks of plugins not loaded
bool Load(const char* coSavePath);    // missing file = every plugin's revert callback
// The same from a co-save image in memory (the file format; e.g. built from an older save format by the game layer).
// label = for the warning line. Returns false (and reverts everyone) if the image is damaged.
bool LoadBytes(const void* data, size_t len, const char* label);
void NewGame();                       // every plugin's revert callback
}  // namespace cosave

// Hooks broker environment (hooks.cpp is independent of the rest so it can be tested alone).
namespace hooks {
void SetEnvironment(const char* (*pluginName)(PSEPluginHandle), void (*log)(PSEPluginHandle, PSELogLevel, const char*));
void ResetForTests();   // forgets records + frees code memory; memory is NOT restored
}  // namespace hooks


// ---- External plugin DLLs (plugins.cpp, docs/sdk/reference/plugins.md) ---------------------------------------------

// Registry side. An external plugin's info strings are copied (its DLL can be freed while the record stays).
PSEPluginHandle RegisterExternalPlugin(const PSEPluginInfo* info);
// Failed / unloaded plugin: the handle stays taken (never handed out again) and keeps its name for logs, but
// FindPlugin, Query and every service treat it as gone.
void DisablePlugin(PSEPluginHandle plugin);
bool PluginDisabled(PSEPluginHandle plugin);
bool PluginExternal(PSEPluginHandle plugin);
bool ValidPluginName(const char* name);              // [a-z0-9_.-], 1..64 characters

// Manifest (<Name>.pse): "key = value" lines, '#' starts a comment.
struct PluginRequirement {
    std::string name;
    uint32_t minVersion = 0;                         // PSE_MAKE_VERSION; 0 = any version
};
struct PluginManifest {
    std::string name;
    std::vector<PluginRequirement> required;         // "requires = a >= 1.2, b"
    std::vector<std::string> optional, conflicts, after;
    bool host = false;                               // "host = true": wants pse.host (game layer's allow list decides)
    std::vector<std::string> warnings;               // unknown keys etc. (logged, not fatal)
};
// false + *error (with the line number) on a syntax error.
bool ParseManifest(const std::string& text, PluginManifest* out, std::string* error);
bool ParseVersion(const std::string& text, uint32_t* out);   // "1", "1.2", "1.2.3"
std::string VersionString(uint32_t version);                 // "1.2.3"

// Dependency resolution without loading anything (also used by the tests). `refusal` empty = still a candidate;
// the resolver fills it for every candidate it refuses. Requirements may also name plugins already registered
// (internal ones). Returns the load order as indices into `candidates`.
struct PluginCandidate {
    PluginManifest manifest;
    uint32_t version = 0;                            // from the DLL's PSEPlugin_Info
    std::string refusal;
};
std::vector<size_t> ResolvePluginOrder(std::vector<PluginCandidate>& candidates);

enum class PluginState { Loaded, Refused, Failed, Unloaded };
const char* PluginStateName(PluginState state);      // "loaded", "refused", "failed", "unloaded"
struct PluginLoadResult {
    std::string name, displayName, file;             // file = full DLL path (or the manifest when no DLL was read)
    uint32_t version = 0, flags = 0;
    PluginState state = PluginState::Refused;
    std::string reason;                              // why refused / failed ("" when loaded)
    PSEPluginHandle handle = 0;                      // 0 = never registered (refused before loading)
};

// Loads every plugin in <dir>\*.pse (+ the .dll of the same name). Main thread, once, after Init and after the
// internal plugins are registered. A missing folder = nothing to do. Ends with PSE_MSG_POST_LOAD and
// PSE_MSG_POST_POST_LOAD (also when no plugin was found). Returns the number of plugins loaded.
// hostPlugins: names the game layer allows as host plugins (manifest `host = true`): those load first, in a round of
// their own, and get "pse.host" through their Query; every other plugin is resolved after them (so it may require a
// host's sub-plugins). A `host = true` plugin that isn't on the list is refused.
uint32_t LoadPlugins(const std::wstring& dir, const std::vector<std::string>& hostPlugins = {});
// Exit: PSEPlugin_Unload in reverse load order, then each plugin's hooks / subscriptions are removed and its DLL
// freed.
void UnloadPlugins();
// One entry per manifest found, in load order (refused ones last, by name). A plugin that crashed in a callback after
// its Load (docs/sdk/reference/plugins.md "Crash guard") shows state Failed with the reason.
std::vector<PluginLoadResult> PluginResults();

// ---- Phase 2 layout: shim -> pse_loader.dll -> game layer -> plugins (startup.cpp, host.cpp, docs/sdk/layout.md) ----

// A plugin compiled into a host plugin / game layer (a DesertStormFix feature "dsfix.hud"): PSE_PLUGIN_INTERNAL, called
// without the crash guard, strings copied. `parent` must be a valid handle. Retired when the parent fails.
PSEPluginHandle RegisterHostedPlugin(const PSEPluginInfo* info, PSEPluginHandle parent);
PSEPluginHandle PluginParent(PSEPluginHandle plugin);              // 0 = none
std::vector<PSEPluginHandle> PluginChildren(PSEPluginHandle parent);
// Host plugins and the game layer: their PSELoadInterface.Query answers PSE_HOST_INTERFACE.
void SetPluginHost(PSEPluginHandle plugin);
bool PluginHost(PSEPluginHandle plugin);
// The pse.host table (host.cpp).
const PSEHostInterface* HostInterface();

// Start-up as pse_loader.dll runs it (PSELoader_Start) - also driven by pse_tests with scratch folders.
struct StartOptions {
    std::wstring pseDir;           // <game>\PSE: Games\pse_*.dll and Plugins\ are looked for under it
    std::wstring exePath;          // the running exe (hashed for detection)
    uint32_t imageBase = 0;        // mapped exe (detection info)
    uint32_t peTimestamp = 0;
    std::string registryRoot;      // "" = Software\PSE\<gameId> (tests: a throw-away key)
    // Output for log / report lines while no host plugin has set a sink (pse_loader.dll: OutputDebugString in dev
    // builds, nothing in release packages). Lines logged before a host sets its sink are kept and handed to it.
    void (*fallbackLog)(const char* line) = nullptr;
    void (*fallbackReport)(const char* line, int error) = nullptr;
};
struct StartResult {
    bool running = false;          // a game layer recognised the exe and initialised
    std::string gameId;
    std::wstring gameLayer;        // its DLL path
    std::string exeSha256;         // upper-case hex, LAA cleared
    std::string why;               // !running: the reason (also logged)
    uint32_t pluginsLoaded = 0;
};
// 1. hash the exe; 2. PSE\Games\pse_*.dll in name order: PSEGame_Detect - the first that answers 1 wins, the others are
// freed; none = vanilla, nothing else happens; 3. pse::Init (game id from the layer); 4. the layer registered as a
// built-in host plugin, PSEGame_Init; 5. LoadPlugins(PSE\Plugins, the layer's host list); 6. PSE_MSG_GAME_ENTRY.
// Once per process (tests: ResetForTests in between).
StartResult Start(const StartOptions& options);
// SHA-256 (upper-case hex) of a PE file with IMAGE_FILE_LARGE_ADDRESS_AWARE cleared (what DesertStormFix-Patcher
// toggles), "" if unreadable. Game-neutral: every game layer gets it in PSEGameDetectInfo.
std::string ExeSha256(const std::wstring& path);
// Log routing (startup.cpp): the pse::Config sinks Start installs end here.
void SetHostLogSink(PSEHostLogSink sink);
void SetHostReportSink(PSEHostReportSink sink);

// ---- Online plugin-set check (compat.cpp, docs/sdk/online-compat.md) -----------------------------------------------
namespace compat {
constexpr uint32_t kProtocol = 1;          // bump when the encoding or the comparison rules change
constexpr const char* kBuiltIn = "=";      // PluginEntry::hash of a plugin built into the host DLL
// PluginEntry::hash of an external SIMULATION plugin that exports PSEPlugin_SimFingerprint: "fp:" + SHA-256 (hex) of
// the string it returns - compared instead of the DLL's own hash (a dev and a release build of one source connect).
constexpr const char* kFingerprintPrefix = "fp:";
struct PluginEntry {
    std::string name;
    uint32_t version = 0;
    uint32_t flags = 0;    // PSE_PLUGIN_* as the loader has it (PSE_PLUGIN_INTERNAL set for built-in ones)
    std::string hash;      // external SIMULATION plugin: SHA-256 (hex) of its DLL or kFingerprintPrefix + the
                           // fingerprint's SHA-256; built in: kBuiltIn; client-only: ""
};
struct ModEntry {
    std::string name;      // folder name under Mods\ (compared without case)
    std::string hash;      // the host's content hash of the mod (onlinecompat.cpp: SHA-256 over its files)
};
struct Set {
    uint32_t protocol = kProtocol;
    uint32_t apiVersion = 0;       // PSE_API_VERSION
    std::string gameId;            // "ds1"
    std::string host;              // host DLL + version, "DesertStormFix 3.3.0-Alpha" (the game layer in phase 1)
    std::string hostFingerprint;   // simulation fingerprint of the host build: covers every built-in plugin
    std::vector<PluginEntry> plugins;
    std::vector<ModEntry> mods;
};
// Neither SIMULATION nor CLIENT_ONLY = simulation (DESIGN.md 4.1).
bool Simulation(uint32_t flags);
// Percent-encoding of a token (no spaces / ':' / '%'); "%" = the empty string.
std::string Escape(const std::string& s);
bool Unescape(const std::string& s, std::string* out);
// Text lines "PSEC <id> B|P|M|E ...", each at most maxLine characters (entries never split), sent as game commands.
std::vector<std::string> Encode(const Set& s, uint32_t id, size_t maxLine = 240);
// Collects one sender's lines. A new "B" line starts over (a resent list replaces the old one).
class Assembler {
public:
    enum Result { NotOurs, Pending, Done, Bad };
    Result Feed(const std::string& line);   // Done: `set` holds the list; Bad: `error` says why
    Set set;
    std::string error;
private:
    Set partial_;
    uint32_t id_ = 0;
    bool building_ = false;
};
// The differences that refuse a join, worded from this PC's side; `who` names the other PC ("the host", "player 2").
// Empty = compatible. Symmetric: Compare(a, b) is empty exactly when Compare(b, a) is.
//   - protocol / game id / loader API / host name + version / host simulation fingerprint must be equal;
//   - every plugin that is SIMULATION (or unflagged) on either side: same name + version + kind (built in vs DLL) and,
//     for DLLs, the same SHA-256 (or the same declared simulation fingerprint); CLIENT_ONLY plugins may differ or be
//     missing;
//   - every switched-on mod: same name (any case) and content hash.
std::vector<std::string> Compare(const Set& local, const Set& remote, const std::string& who);
// The running loader's plugins (disabled ones left out), by name.
std::vector<PluginEntry> LocalPlugins();
// SHA-256 (Windows CNG), lower-case hex.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void Add(const void* data, size_t len);
    std::string Hex();   // once; "" when CNG failed
private:
    struct Impl;
    Impl* impl_;
};
std::string Sha256Hex(const void* data, size_t len);
bool Sha256File(const std::wstring& path, std::string* hex);
}  // namespace compat

}  // namespace pse
