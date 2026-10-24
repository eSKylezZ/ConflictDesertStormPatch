// settings:: for ds2fix (the PSE host plugin build; it replaces src/core/settings.cpp there - the standalone dinput8.dll
// keeps that file and its HKLM\SOFTWARE\SCi Games\CDS II\Enhancements key).
//
// Every settings::Values field is a pse.settings value of plugin "ds2fix": HKCU\Software\PSE\ds2\Plugins\ds2fix (the
// loader's per-plugin key, docs/sdk/reference/settings.md), declared with type / range / flags like DesertStormFix's
// src/core/settings.cpp. The first start copies the old Enhancements key's values once (pse::MigrateRegistryValues,
// marker "MigratedFromEnhancements"); the old key is never changed, so the standalone build keeps working with it. The
// game's own Device Settings stay where the game keeps them; dev values stay in HKCU\Software\DesertStorm2Fix\Dev.
// A new settings::Values field goes into BOTH files (src/core/settings.cpp for the standalone build, Specs() here).
#include "core/settings.h"

#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "loader.h"

namespace {
constexpr char kFallbackKey[] = "Software\\PSE\\ds2\\Plugins\\ds2fix";   // = pse::PluginSettingsKey(Core())
// The standalone DesertStorm2Fix's key: HKLM, virtualized for this manifest-less 32-bit exe into the user's VirtualStore.
constexpr char kOldKey[] = "SOFTWARE\\SCi Games\\CDS II\\Enhancements";
constexpr char kOldVirtualStore[] =
    "Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\SCi Games\\CDS II\\Enhancements";
constexpr char kMigratedMarker[] = "MigratedFromEnhancements";

settings::Values g_values;
using settings::Values;

struct Spec {
    const char* key;
    PSESettingType type;
    uint32_t flags;
    int32_t minInt, maxInt;
    const char* label;
    const char* group;
    const char* const* choices;
    uint32_t (*get)(const Values&);
    void (*set)(Values&, uint32_t);
    const char* help = nullptr;
};

// Flags: when a change takes effect. ds2fix applies everything at start-up today, so all but the pure dev / overlay
// values are "restart"; LAUNCHER = a candidate for the launcher's Enhancements column once DS2 has one.
constexpr uint32_t kLive = PSE_SETTING_LIVE, kRestart = 0, kLauncher = PSE_SETTING_LAUNCHER;
const char* const kDisplayModes[] = {"FULLSCREEN", "WINDOWED", "BORDERLESS", nullptr};
const char* const kSplitLayouts[] = {"TOP / BOTTOM", "SIDE BY SIDE", nullptr};

#define DS_U(f) [](const Values& v) -> uint32_t { return v.f; }, [](Values& v, uint32_t x) { v.f = x; }
#define DS_B(f) [](const Values& v) -> uint32_t { return v.f ? 1u : 0u; }, [](Values& v, uint32_t x) { v.f = x != 0; }

// Ranges = the clamps of the standalone reader (src/core/settings.cpp).
const std::vector<Spec>& Specs() {
    static const std::vector<Spec> specs = {
        // Read by the ds2fix.widescreen plugin now (its own key, copied once from these); kept here so the migrated
        // values stay where it imports them from. Not offered to the launcher.
        {"HudScaling", PSE_SETTING_BOOL, kRestart, 0, 1, "HUD & MENU SCALING", "HUD", nullptr, DS_B(hudScaling)},
        {"HudScalePercent", PSE_SETTING_INT, kRestart, 25, 400, "HUD SIZE", "HUD", nullptr, DS_U(hudScalePercent),
         "percent, on top of the automatic min(W/800, H/600)"},
        {"Controller", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "CONTROLLER SUPPORT", "CONTROLS", nullptr, DS_B(controller)},
        {"PadDeadzone", PSE_SETTING_INT, kRestart | kLauncher, 0, 90, "STICK DEADZONE", "CONTROLS", nullptr, DS_U(padDeadzone),
         "percent"},
        {"FpsCap", PSE_SETTING_INT, kRestart | kLauncher, 0, 1000, "FRAME RATE LIMIT", "DISPLAY", nullptr, DS_U(fpsCap),
         "30..240; 0 or above 240 reads as 240"},
        {"FpsCapToRefresh", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "LIMIT TO MONITOR REFRESH", "DISPLAY", nullptr,
         DS_B(fpsCapToRefresh)},
        {"SplitScreenLayout", PSE_SETTING_CHOICE, kRestart | kLauncher, 0, 0, "SPLIT SCREEN LAYOUT", "DISPLAY", kSplitLayouts,
         DS_U(splitScreenLayout)},
        {"DiscordPresence", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "DISCORD STATUS", "SYSTEM", nullptr,
         DS_B(discordPresence)},
        {"DisplayMode", PSE_SETTING_CHOICE, kRestart | kLauncher, 0, 0, "DISPLAY MODE", "DISPLAY", kDisplayModes,
         DS_U(displayMode)},
        {"SkipIntro", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "SKIP INTRO", "SYSTEM", nullptr, DS_B(skipIntro)},
        {"SkipCutscenes", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "SKIP CUTSCENES", "SYSTEM", nullptr, DS_B(skipCutscenes)},
        {"Antialiasing", PSE_SETTING_INT, kRestart | kLauncher, 0, 16, "ANTI-ALIASING", "GRAPHICS", nullptr, DS_U(antialiasing),
         "MSAA samples (0 off)"},
        {"Anisotropy", PSE_SETTING_INT, kRestart | kLauncher, 0, 16, "TEXTURE FILTERING", "GRAPHICS", nullptr, DS_U(anisotropy),
         "anisotropic filtering level (0 = the game's own)"},
        {"MouseAcceleration", PSE_SETTING_BOOL, kRestart, 0, 1, "MOUSE ACCELERATION", "CONTROLS", nullptr,
         DS_B(mouseAcceleration)},
        {"VSync", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "V-SYNC", "DISPLAY", nullptr, DS_B(vsync)},
        {"PerfOverlay", PSE_SETTING_BOOL, kLive | kLauncher, 0, 1, "PERFORMANCE OVERLAY", "SYSTEM", nullptr, DS_B(perfOverlay)},
        {"MenuZoom", PSE_SETTING_BOOL, kLive | kLauncher, 0, 1, "MENU ZOOM", "HUD", nullptr, DS_B(menuZoom),
         "front end: the menu monitor fills the screen height (off = the whole 1280x1024 picture)"},
        {"FeatMods", PSE_SETTING_BOOL, kRestart, 0, 1, "MODS", "OPTIONAL FEATURES", nullptr, DS_B(featMods),
         "files in <game>\\Mods\\<mod> replace / add game files"},
    };
    return specs;
}
#undef DS_U
#undef DS_B

PSEPluginHandle Me() { return pse_host::Core(); }
const PSESettingsInterface* Api() { return pse::SettingsInterface(); }

void DeclareAll() {
    static bool declared = false;
    if (declared || !Api()) return;
    declared = true;
    const Values defaults;
    for (const Spec& s : Specs()) {
        PSESettingDesc d{};
        d.size = sizeof d;
        d.key = s.key;
        d.type = s.type;
        d.flags = s.flags;
        d.defaultInt = static_cast<int32_t>(s.get(defaults));
        d.minInt = s.minInt;
        d.maxInt = s.maxInt;
        d.label = s.label;
        d.help = s.help;
        d.choices = s.choices;
        d.group = s.group;
        const PSEResult r = Api()->Declare(Me(), &d);
        if (r != PSE_OK && r != PSE_E_EXISTS) dslog::Write("[fail] Settings: cannot declare %s (%d)", s.key, static_cast<int>(r));
    }
}

const char* Key() {
    static const std::string key = [] {
        std::string k = Me() ? pse::PluginSettingsKey(Me()) : std::string();
        return k.empty() ? std::string(kFallbackKey) : k;
    }();
    return key.c_str();
}

// One-time copy of the old Enhancements key (all values) into ours; the old key is never changed.
void Migrate() {
    const std::vector<pse::RegistrySource> sources = {
        {HKEY_LOCAL_MACHINE, kOldKey, 0},          // what the exe reads (virtualized: the VirtualStore copy + HKLM)
        {HKEY_CURRENT_USER, kOldVirtualStore, 0},  // the VirtualStore copy directly (if virtualization is off)
    };
    const pse::MigrationResult r = pse::MigrateRegistryValues(sources, HKEY_CURRENT_USER, Key(), kMigratedMarker);
    if (!r.ran) {
        if (!r.ok) dslog::Write("[fail] Settings: cannot open HKCU\\%s", Key());
        return;
    }
    std::string list;
    for (const std::string& n : r.copied) list += " " + n;
    dslog::Write("Settings: moved to HKCU\\%s - %zu value(s) copied from %d old key(s), %zu already there%s%s", Key(),
                 r.copied.size(), r.sourcesFound, r.kept.size(), list.empty() ? "" : ":", list.c_str());
    for (const std::string& n : r.failed) dslog::Write("[fail] Settings: couldn't copy %s (retried at the next start)", n.c_str());
}
}  // namespace

const settings::Values& settings::Get() { return g_values; }

void settings::Load() {
    static bool migrated = false;
    if (!migrated) {
        migrated = true;
        Migrate();
    }
    if (Me() && Api()) {
        DeclareAll();
        for (const Spec& s : Specs()) s.set(g_values, static_cast<uint32_t>(Api()->GetInt(Me(), s.key)));
    } else {
        dslog::Write("[fail] Settings: no PSE plugin \"ds2fix\" - defaults used");
    }
    if (g_values.fpsCap == 0 || g_values.fpsCap > 240) g_values.fpsCap = 240;  // 240 is the most (older "unlimited")
    std::string line;
    for (const Spec& s : Specs()) line += std::string(" ") + s.key + "=" + std::to_string(s.get(g_values));
    dslog::Write("Settings (HKCU\\%s):%s", Key(), line.c_str());
}

bool settings::Save(const Values& v) {
    if (!Me() || !Api()) {
        dslog::Write("Settings: cannot save (no PSE plugin \"ds2fix\")");
        return false;
    }
    DeclareAll();
    bool ok = true;
    for (const Spec& s : Specs()) ok = Api()->SetInt(Me(), s.key, static_cast<int32_t>(s.get(v))) == PSE_OK && ok;
    if (ok) g_values = v;
    else dslog::Write("Settings: cannot write HKCU\\%s", Key());
    return ok;
}
