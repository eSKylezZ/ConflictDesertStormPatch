#pragma once
// pse.ui's pure parts (no game, no Windows UI): the settings page of a plugin built from pse.settings' Describe, value
// stepping / texts, the PLUGINS list built from the loader's load results, UTF-8 -> the game fonts' 8-bit text, and
// the per-plugin text-hash allocator. Used by the in-game menus (features/pluginsmenu.cpp), the launcher's plugin rows
// (features/launcher.cpp) and pse.ui (features/pseui.cpp); unit-tested by pse/tests/test_ui_model.cpp.
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "loader.h"
#include "pse/pse_api.h"

namespace pse_ui {

// ---- text ----
// Does the font draw byte c (0x20..0xFF)? Null = every byte.
using HasGlyph = bool (*)(unsigned char c, void* ctx);
// UTF-8 -> Windows-1252 bytes the game's fonts draw. A character outside 1252, or one whose byte the font lacks, is
// replaced by its plain form (accents dropped, ligatures spelt out, typographic quotes / dashes / ellipsis made ASCII)
// or '?'. Control characters (the fonts' icon slots 1-31) become spaces; a malformed sequence is one '?'. upper = to
// upper case (the menus are all capitals). Stops at maxBytes output bytes (0 = no limit).
std::string ToGameText(const char* utf8, bool upper = false, HasGlyph has = nullptr, void* ctx = nullptr,
                       size_t maxBytes = 0);

// ---- a plugin's settings page ----
enum class RowKind { Heading, Bool, Int, Choice, Colour, String };
struct Row {
    RowKind kind = RowKind::Heading;
    PSEPluginHandle owner = 0;
    std::string key;                     // "" for a heading
    std::string label;                   // UTF-8 as declared (heading: the group name)
    std::string help;
    uint32_t flags = 0;                  // PSE_SETTING_*
    int32_t min = 0, max = 0, step = 1;  // Int; Choice: 0 .. count - 1
    int32_t def = 0;
    std::vector<std::string> choices;    // Choice labels (UTF-8)
};
// Every non-HIDDEN setting `owner` declared, in declaration order; a heading row before each run of settings whose
// group differs from the previous one (none for settings without a group). Unknown types are left out.
std::vector<Row> BuildPage(const PSESettingsInterface* settings, PSEPluginHandle owner);
// Settings `owner` declared that a page would show (= BuildPage without headings).
size_t VisibleSettings(const PSESettingsInterface* settings, PSEPluginHandle owner);
bool Editable(const Row& row);           // Bool / Int / Choice / Colour (String is shown read-only, headings do nothing)
bool Spinner(const Row& row);            // changes with Left / Right (Bool / Int / Choice with more than one value)
// The next value one step `dir` (-1 / +1) away: Bool flips, Int moves by step and stops at min / max, Choice wraps.
// Others: value unchanged.
int32_t StepValue(const Row& row, int32_t value, int dir);
// "ON" / "OFF", the number, the choice's label, "#RRGGBB", the string ("" when empty). UTF-8.
std::string ValueText(const Row& row, int32_t value, const std::string& text = std::string());
// " *" = after a restart, " +" = from the next level, "" = at once (LIVE).
const char* WhenMarker(uint32_t flags);
bool NeedsRestart(uint32_t flags);       // neither LIVE nor NEXT_LEVEL

// ---- the PLUGINS list ----
struct PluginEntry {
    std::string name, displayName;       // displayName falls back to the name
    std::string version;                 // "1.2.3"
    pse::PluginState state = pse::PluginState::Loaded;
    std::string reason;                  // refused / failed: why
    PSEPluginHandle handle = 0;          // 0 = never loaded
    bool host = false;                   // a host plugin (DesertStormFix): its settings have their own menus
    size_t settings = 0;                 // rows its page would have (loaded, non-host plugins)
};
// The load results in their order (loaded first, refused last), host plugins marked; `settingsOf` counts a loaded
// plugin's visible settings (null = none).
std::vector<PluginEntry> BuildPluginList(const std::vector<pse::PluginLoadResult>& results,
                                         const std::vector<std::string>& hostNames,
                                         size_t (*settingsOf)(PSEPluginHandle, void*), void* ctx);
const char* StateText(pse::PluginState state);   // "LOADED", "REFUSED", "FAILED", "UNLOADED"

// ---- the launcher ----
// A setting the launcher shows for an external plugin: PSE_SETTING_LAUNCHER, not HIDDEN, bool / int / choice.
bool LauncherSetting(const PSESettingDesc& desc);

// ---- text hashes ----
// Hashes handed out to plugins for their texts: 0xC4000000 | (plugin & 0xFFF) << 12 | (n & 0xFFF), n = 1.. per
// plugin. Same plugin + key = same hash; RegisterText again replaces the text.
class TextTable {
public:
    static constexpr uint32_t kBase = 0xC4000000u, kMask = 0xFF000000u;
    static constexpr uint32_t kPerPlugin = 4095;
    uint32_t Register(PSEPluginHandle plugin, const std::string& key, const std::string& text);   // 0 = refused
    uint32_t Hash(PSEPluginHandle plugin, const std::string& key) const;                           // 0 = none
    const std::string* Find(uint32_t hash) const;                                                  // UTF-8
    static bool Ours(uint32_t hash) { return (hash & kMask) == kBase; }

private:
    std::map<std::pair<PSEPluginHandle, std::string>, uint32_t> keys_;
    std::map<uint32_t, std::string> texts_;
    std::map<PSEPluginHandle, uint32_t> next_;
};

}  // namespace pse_ui
