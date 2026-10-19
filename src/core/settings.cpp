#include "core/settings.h"

#include <windows.h>

#include <algorithm>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/pse_host.h"
#include "loader.h"

// Every settings::Values field is a pse.settings value of plugin "dsfix" (HKCU\Software\PSE\ds1\Plugins\dsfix);
// docs/sdk/ds1-settings.md lists them. The game's own "Device Settings" key stays where the game keeps it.
namespace {
constexpr char kGameKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Device Settings";
constexpr char kFallbackKey[] = "Software\\PSE\\ds1\\Plugins\\dsfix";   // = pse::PluginSettingsKey(Core())
// Before 2026-10-19: HKLM (virtualized for this manifest-less 32-bit exe into the user's VirtualStore).
constexpr char kOldKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";
constexpr char kOldVirtualStore[] =
    "Software\\Classes\\VirtualStore\\MACHINE\\SOFTWARE\\WOW6432Node\\Pivotal Games\\Conflict Desert Storm\\Enhancements";
constexpr char kMigratedMarker[] = "MigratedFromEnhancements";

settings::Values g_values;
using settings::Values;

// restart: set per feature by whoever made it switchable (true until a feature can follow a change at once)
settings::Toggle g_toggles[] = {
    {"FeatLoadout", &Values::featLoadout, false, "BRIEFING & LOADOUT"},
    {"FeatUniformSelect", &Values::featUniformSelect, false, "UNIFORM SELECT SCREEN"},
    {"FeatTimeOfDay", &Values::featTimeOfDay, false, "TIME OF DAY OPTIONS"},
    {"FeatFreeGear", &Values::featFreeGear, false, "FREE POUCH CHOICE"},
    {"FeatLoadoutSpin", &Values::featLoadoutSpin, false, "LOADOUT CAMERA SPIN"},
    {"FeatLoadoutLookAhead", &Values::featLoadoutLookAhead, false, "LOADOUT: LOOK AHEAD"},
    {"FeatVoices", &Values::featVoices, false, "SQUAD VOICE ACCENTS"},
    {"FeatPlayerVoice", &Values::featPlayerVoice, false, "YOUR SOLDIER TALKS"},
    {"FeatFollowers", &Values::featFollowers, false, "PLAYABLE FOLLOWERS"},
    {"FeatVeryHard", &Values::featVeryHard, true, "VERY EASY & VERY HARD"},
    {"FeatJpFixes", &Values::featJpFixes, false, "JAPANESE RELEASE FIXES"},
    {"FeatRumble", &Values::featRumble, false, "ENHANCED RUMBLE"},
    {"FeatTooltips", &Values::featTooltips, false, "INVENTORY HINTS"},
    {"FeatAimAudio", &Values::featAimAudio, false, "AIMING AUDIO FIX"},
    {"FeatShadowFix", &Values::featShadowFix, false, "SHADOW FIX"},
    {"FeatSharpText", &Values::featSharpText, false, "SHARPER TEXT"},
    {"FeatMods", &Values::featMods, true, "MODS"},
    {"FeatCustomMissions", &Values::featCustomMissions, false, "CUSTOM CAMPAIGNS"},
    {"FeatVersus", &Values::featVersus, true, "VERSUS MENU"},
    {"FeatCheats", &Values::featCheats, true, "EXTENDED CHEATS"},
    {"FeatWeather", &Values::featWeather, false, "WEATHER OPTIONS"},
    {"FeatFootprintsAll", &Values::featFootprintsAll, false, "FOOTPRINTS: EVERYONE"},
    {"FeatFootprintsWeather", &Values::featFootprintsWeather, false, "WEATHER WIPES FOOTPRINTS"},
    {"FeatRunInBackground", &Values::featRunInBackground, false, "RUN IN BACKGROUND"},
    {"FeatDevConsole", &Values::featDevConsole, false, "DEVELOPER CONSOLE"},
};

// One settings::Values field as a pse.settings value.
struct Spec {
    std::string key;
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

constexpr uint32_t kLive = PSE_SETTING_LIVE, kRestart = 0, kNextLevel = PSE_SETTING_NEXT_LEVEL,
                   kLauncher = PSE_SETTING_LAUNCHER, kHidden = PSE_SETTING_HIDDEN;

const char* const kDisplayModes[] = {"FULLSCREEN", "WINDOWED", "BORDERLESS", nullptr};
const char* const kSplitLayouts[] = {"TOP / BOTTOM", "SIDE BY SIDE", nullptr};
const char* const kRenderers[] = {"DIRECT3D 8 (ORIGINAL)", "DIRECT3D 9", "DIRECT3D 12 (9ON12)", "DIRECT3D 12 + FLIP",
                                  nullptr};
const char* const kBloom[] = {"OFF", "SUBTLE", "STRONG", nullptr};
const char* const kFootprints[] = {"OFF", "GAME (12 S)", "LONG (2 MIN)", "PERMANENT", nullptr};

#define DS_U(f) [](const Values& v) -> uint32_t { return v.f; }, [](Values& v, uint32_t x) { v.f = x; }
#define DS_B(f) [](const Values& v) -> uint32_t { return v.f ? 1u : 0u; }, [](Values& v, uint32_t x) { v.f = x != 0; }
#define DS_FOV(i) [](const Values& v) -> uint32_t { return v.fov[i]; }, [](Values& v, uint32_t x) { v.fov[i] = x; }

// Ranges = the clamps the old registry reader used (settings::Load before 2026-10-19).
const std::vector<Spec>& Specs() {
    static const std::vector<Spec> specs = [] {
        std::vector<Spec> s = {
            {"HudScaling", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "HUD & MENU SCALING", "HUD", nullptr, DS_B(hudScaling)},
            {"HudScalePercent", PSE_SETTING_INT, kRestart | kLauncher, 25, 400, "HUD SIZE", "HUD", nullptr, DS_U(hudScalePercent),
             "percent, on top of the automatic min(W/800, H/600)"},
            {"Controller", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "CONTROLLER SUPPORT", "CONTROLS", nullptr, DS_B(controller)},
            {"PadDeadzone", PSE_SETTING_INT, kLive | kLauncher, 0, 90, "STICK DEADZONE", "CONTROLS", nullptr, DS_U(padDeadzone),
             "percent"},
            {"FpsCap", PSE_SETTING_INT, kLive | kLauncher, 0, 1000, "FRAME RATE LIMIT", "DISPLAY", nullptr, DS_U(fpsCap),
             "30..240; 0 or above 240 reads as 240"},
            {"FpsCapToRefresh", PSE_SETTING_BOOL, kLive | kLauncher, 0, 1, "LIMIT TO MONITOR REFRESH", "DISPLAY", nullptr,
             DS_B(fpsCapToRefresh)},
            {"SplitScreenLayout", PSE_SETTING_CHOICE, kNextLevel | kLauncher, 0, 0, "SPLIT SCREEN LAYOUT", "DISPLAY",
             kSplitLayouts, DS_U(splitScreenLayout)},
            // DiscordPresence: setting of the external plugin dsfix.discord since 2026-10-19 (plugins/discord)
            {"DisplayMode", PSE_SETTING_CHOICE, kLive | kLauncher, 0, 0, "DISPLAY MODE", "DISPLAY", kDisplayModes,
             DS_U(displayMode), "applied with OPTIONS > VIDEO > APPLY NOW in the main menu"},
            {"SkipIntro", PSE_SETTING_BOOL, kRestart | kLauncher, 0, 1, "SKIP INTRO", "SYSTEM", nullptr, DS_B(skipIntro)},
            {"SkipCutscenes", PSE_SETTING_BOOL, kLive | kLauncher, 0, 1, "SKIP CUTSCENES", "SYSTEM", nullptr, DS_B(skipCutscenes)},
            {"Antialiasing", PSE_SETTING_INT, kLive | kLauncher, 0, 16, "ANTI-ALIASING", "GRAPHICS", nullptr, DS_U(antialiasing),
             "MSAA samples (0 off); applied with APPLY NOW"},
            {"Anisotropy", PSE_SETTING_INT, kLive | kLauncher, 0, 16, "TEXTURE FILTERING", "GRAPHICS", nullptr, DS_U(anisotropy),
             "anisotropic filtering level (0 = the game's trilinear)"},
            {"MouseAcceleration", PSE_SETTING_BOOL, kLive, 0, 1, "MOUSE ACCELERATION", "CONTROLS", nullptr, DS_B(mouseAcceleration),
             "the game's MOUSE OPTIONS > ACCELERATION"},
            {"VSync", PSE_SETTING_BOOL, kLive | kLauncher, 0, 1, "V-SYNC", "DISPLAY", nullptr, DS_B(vsync)},
            {"PerfOverlay", PSE_SETTING_BOOL, kLive | kLauncher, 0, 1, "PERFORMANCE OVERLAY", "SYSTEM", nullptr, DS_B(perfOverlay)},
            {"SkipLauncher", PSE_SETTING_BOOL, kLive | kLauncher, 0, 1, "LAUNCHER AT START", "SYSTEM", nullptr, DS_B(skipLauncher),
             "1 = start without the launcher (Shift shows it)"},
            {"Renderer", PSE_SETTING_CHOICE, kRestart | kLauncher, 0, 0, "RENDERER", "GRAPHICS", kRenderers, DS_U(renderer),
             "release builds stop at 2"},
            {"Hdr", PSE_SETTING_BOOL, kLive, 0, 1, "HDR", "GRAPHICS", nullptr, DS_B(hdr), "Renderer 3 only"},
            {"HdrPaperWhite", PSE_SETTING_INT, kLive, 80, 500, "HDR PAPER WHITE", "GRAPHICS", nullptr, DS_U(hdrPaperWhite), "nits"},
            {"HdrPeak", PSE_SETTING_INT, kLive, 0, 10000, "HDR PEAK BRIGHTNESS", "GRAPHICS", nullptr, DS_U(hdrPeak),
             "nits (0 = the display's own)"},
            {"HdrHighlights", PSE_SETTING_INT, kLive, 0, 100, "HDR HIGHLIGHTS", "GRAPHICS", nullptr, DS_U(hdrHighlights), "percent"},
            {"Bloom", PSE_SETTING_CHOICE, kLive, 0, 0, "BLOOM", "GRAPHICS", kBloom, DS_U(bloom)},
            {"Fov1", PSE_SETTING_INT, kLive, 50, 110, "FIELD OF VIEW - PLAYER 1", "GAMEPLAY", nullptr, DS_FOV(0), "degrees"},
            {"Fov2", PSE_SETTING_INT, kLive, 50, 110, "FIELD OF VIEW - PLAYER 2", "GAMEPLAY", nullptr, DS_FOV(1), "degrees"},
            {"Fov3", PSE_SETTING_INT, kLive, 50, 110, "FIELD OF VIEW - PLAYER 3", "GAMEPLAY", nullptr, DS_FOV(2), "degrees"},
            {"Fov4", PSE_SETTING_INT, kLive, 50, 110, "FIELD OF VIEW - PLAYER 4", "GAMEPLAY", nullptr, DS_FOV(3), "degrees"},
            {"MenuBarCustom", PSE_SETTING_BOOL, kLive, 0, 1, "MENU BAR COLOUR", "HUD", nullptr, DS_B(menuBarCustom),
             "1 = MenuBarColour instead of the game's green"},
            {"MenuBarColour", PSE_SETTING_COLOUR, kLive | kHidden, 0, 0, "MENU BAR COLOUR (RGB)", "HUD", nullptr,
             DS_U(menuBarColour)},
            {"HudColourCustom", PSE_SETTING_BOOL, kLive, 0, 1, "HUD COLOUR", "HUD", nullptr, DS_B(hudColourCustom),
             "1 = HudColour instead of the game's green"},
            {"HudColour", PSE_SETTING_COLOUR, kLive | kHidden, 0, 0, "HUD COLOUR (RGB)", "HUD", nullptr, DS_U(hudColour)},
            {"Footprints", PSE_SETTING_CHOICE, kLive, 0, 0, "FOOTPRINTS", "GAMEPLAY", kFootprints, DS_U(footprints)},
        };
        // Optional features (OPTIONS > DESERTSTORMFIX): field pointers, so built from g_toggles at run time.
        return s;
    }();
    return specs;
}
#undef DS_U
#undef DS_B
#undef DS_FOV

PSEPluginHandle Me() { return pse_host::Core(); }
const PSESettingsInterface* Api() { return pse::SettingsInterface(); }

bool g_declared = false;

void Declare(const char* key, PSESettingType type, uint32_t flags, int32_t def, int32_t mn, int32_t mx, const char* label,
             const char* group, const char* const* choices, const char* help) {
    PSESettingDesc d{};
    d.size = sizeof d;
    d.key = key;
    d.type = type;
    d.flags = flags;
    d.defaultInt = def;
    d.minInt = mn;
    d.maxInt = mx;
    d.label = label;
    d.help = help;
    d.choices = choices;
    d.group = group;
    const PSEResult r = Api()->Declare(Me(), &d);
    if (r != PSE_OK && r != PSE_E_EXISTS) dslog::Write("[fail] Settings: cannot declare %s (%d)", key, static_cast<int>(r));
}

void DeclareAll() {
    if (g_declared) return;
    g_declared = true;
    const Values defaults;
    for (const Spec& s : Specs())
        Declare(s.key.c_str(), s.type, s.flags, static_cast<int32_t>(s.get(defaults)), s.minInt, s.maxInt, s.label, s.group,
                s.choices, s.help);
    for (const settings::Toggle& t : g_toggles)
        Declare(t.name, PSE_SETTING_BOOL, t.restart ? kRestart : kLive, defaults.*t.field ? 1 : 0, 0, 1, t.label,
                "OPTIONAL FEATURES", nullptr, nullptr);
}

// One-time copy of the old Enhancements key (all values, all types) into ours; the old key is never changed.
void Migrate() {
    const std::vector<pse::RegistrySource> sources = {
        {HKEY_LOCAL_MACHINE, kOldKey, 0},          // what this exe reads (virtualized: the VirtualStore copy + HKLM)
        {HKEY_CURRENT_USER, kOldVirtualStore, 0},  // the VirtualStore copy directly (if virtualization is off)
        {HKEY_CURRENT_USER, kOldKey, 0},           // the HKCU fallback customise.cpp / vehicles.cpp wrote to
    };
    const pse::MigrationResult r = pse::MigrateRegistryValues(sources, HKEY_CURRENT_USER, settings::Key(), kMigratedMarker);
    if (!r.ran) {
        if (!r.ok) dslog::Write("[fail] Settings: cannot open HKCU\\%s", settings::Key());
        return;
    }
    std::string list;
    for (const std::string& n : r.copied) list += " " + n;
    dslog::Write("Settings: moved to HKCU\\%s - %zu value(s) copied from %d old key(s), %zu already there%s%s",
                 settings::Key(), r.copied.size(), r.sourcesFound, r.kept.size(), list.empty() ? "" : ":", list.c_str());
    for (const std::string& n : r.failed) dslog::Write("[fail] Settings: couldn't copy %s (retried at the next start)", n.c_str());
}

bool PutRaw(const char* name, DWORD type, const void* data, DWORD size) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, settings::Key(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
        return false;
    const bool ok = RegSetValueExA(key, name, 0, type, static_cast<const BYTE*>(data), size) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}
}  // namespace

const settings::Values& settings::Get() { return g_values; }

const settings::Toggle* settings::Toggles(int& count) {
    count = static_cast<int>(sizeof g_toggles / sizeof g_toggles[0]);
    return g_toggles;
}

bool settings::SetToggle(bool Values::*field, bool on) {
    Values v = g_values;
    v.*field = on;
    return Save(v);
}

bool settings::GetByName(const char* name, uint32_t& value) {
    if (!name) return false;
    for (const Spec& s : Specs())
        if (_stricmp(s.key.c_str(), name) == 0) return value = s.get(g_values), true;
    for (const Toggle& t : g_toggles)
        if (_stricmp(t.name, name) == 0) return value = g_values.*t.field ? 1 : 0, true;
    return false;
}

bool settings::SetByName(const char* name, uint32_t value) {
    if (!name) return false;
    Values v = g_values;
    for (const Spec& s : Specs())
        if (_stricmp(s.key.c_str(), name) == 0) return s.set(v, value), Save(v);
    for (const Toggle& t : g_toggles)
        if (_stricmp(t.name, name) == 0) return v.*t.field = value != 0, Save(v);
    return false;
}

const char* settings::Key() {
    static const std::string key = [] {
        std::string k = Me() ? pse::PluginSettingsKey(Me()) : std::string();
        return k.empty() ? std::string(kFallbackKey) : k;
    }();
    return key.c_str();
}

void settings::Load() {
    static bool migrated = false;
    if (!migrated) {
        migrated = true;
        Migrate();
    }
    if (Me()) {
        DeclareAll();
        for (const Spec& s : Specs()) s.set(g_values, static_cast<uint32_t>(Api()->GetInt(Me(), s.key.c_str())));
        for (const Toggle& t : g_toggles) g_values.*t.field = Api()->GetInt(Me(), t.name) != 0;
    } else {
        dslog::Write("[fail] Settings: no PSE plugin \"dsfix\" - defaults used");
    }
    if (g_values.fpsCap == 0 || g_values.fpsCap > 240) g_values.fpsCap = 240;  // 240 is the most (older "unlimited")
    g_values.renderer = std::min(g_values.renderer, Values::kMaxRenderer);
    const Values& v = g_values;
    dslog::Write("Settings: HudScaling=%d HudScalePercent=%u Controller=%d PadDeadzone=%u FpsCap=%u FpsCapToRefresh=%d "
                "SplitScreenLayout=%u DisplayMode=%u SkipIntro=%d SkipCutscenes=%d "
                "Antialiasing=%u Anisotropy=%u SkipLauncher=%d Renderer=%u",
                v.hudScaling, v.hudScalePercent, v.controller, v.padDeadzone, v.fpsCap, v.fpsCapToRefresh,
                v.splitScreenLayout, v.displayMode, v.skipIntro, v.skipCutscenes,
                v.antialiasing, v.anisotropy, v.skipLauncher, v.renderer);
    std::string feats;
    for (const Toggle& t : g_toggles) feats += std::string(" ") + t.name + "=" + (v.*t.field ? "1" : "0");
    dslog::Write("Settings: optional features%s", feats.c_str());
}

bool settings::Save(const Values& v) {
    if (!Me()) {
        dslog::Write("Settings: cannot save (no PSE plugin \"dsfix\")");
        return false;
    }
    DeclareAll();
    bool ok = true;
    for (const Spec& s : Specs())
        ok = Api()->SetInt(Me(), s.key.c_str(), static_cast<int32_t>(s.get(v))) == PSE_OK && ok;
    for (const Toggle& t : g_toggles) ok = Api()->SetInt(Me(), t.name, v.*t.field ? 1 : 0) == PSE_OK && ok;
    if (ok) g_values = v;
    else dslog::Write("Settings: cannot write HKCU\\%s", Key());
    return ok;
}

bool settings::ReadDword(bool game, const char* name, uint32_t& value) {
    DWORD v, size = sizeof v;
    // The game's key stays in (virtualized) HKLM where the game itself keeps it.
    if (RegGetValueA(game ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, game ? kGameKey : Key(), name, RRF_RT_REG_DWORD,
                     nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    value = v;
    return true;
}

bool settings::WriteDword(bool game, const char* name, uint32_t value) {
    const DWORD v = value;
    if (!game) return PutRaw(name, REG_DWORD, &v, sizeof v);
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kGameKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    const bool ok = RegSetValueExA(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

bool settings::ReadString(const char* name, std::string& value) {
    DWORD size = 0;
    if (RegGetValueA(HKEY_CURRENT_USER, Key(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return false;
    std::string buf(size + 1, '\0');
    size = static_cast<DWORD>(buf.size());
    if (RegGetValueA(HKEY_CURRENT_USER, Key(), name, RRF_RT_REG_SZ, nullptr, buf.data(), &size) != ERROR_SUCCESS)
        return false;
    buf.resize(strnlen(buf.c_str(), buf.size()));
    value = buf;
    return true;
}

bool settings::WriteString(const char* name, const std::string& value) {
    return PutRaw(name, REG_SZ, value.c_str(), static_cast<DWORD>(value.size() + 1));
}

bool settings::ReadBinary(const char* name, void* data, uint32_t size) {
    DWORD n = size;
    return RegGetValueA(HKEY_CURRENT_USER, Key(), name, RRF_RT_REG_BINARY, nullptr, data, &n) == ERROR_SUCCESS &&
           n == size;
}

bool settings::WriteBinary(const char* name, const void* data, uint32_t size) {
    return PutRaw(name, REG_BINARY, data, size);
}

bool settings::DeleteValue(const char* name) {
    const LSTATUS s = RegDeleteKeyValueA(HKEY_CURRENT_USER, Key(), name);
    return s == ERROR_SUCCESS || s == ERROR_FILE_NOT_FOUND;
}
