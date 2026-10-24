// Interface dsfix.services v1 (core/dsfix_services.h): what DesertStormFix features in their own plugin DLLs need from
// dsfix.dll - the game's texts, control icons, the online state / the host's synced values, the mods' vehicle handling,
// and two hooks back into dsfix: text providers (asked by the Lookup hook before the catalog, ingame_input.cpp) and the
// trigger feel provider (padio.cpp's adaptive triggers / full pull, via features::PlayerTriggerFeel).
// Registered by dsfix's Init after its own features, before the loader loads the external plugins (round 2). The
// providers live in other DLLs, which the loader unloads after PSE_MSG_EXIT - dsfix forgets them at that message (the
// game still draws text while it shuts down).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/dsfix_services.h"
#include "core/log.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"
#include "render/water.h"
#include "timeofday/dsfix_timeofday.h"
#include "voices/dsfix_voices.h"
#include "weather/dsfix_weather.h"

namespace {
struct TextProvider {
    PSEPluginHandle plugin;
    DsfixTextProvider fn;
};
std::vector<TextProvider> g_text;
struct ScreenProvider {
    PSEPluginHandle plugin;
    DsfixScreenProvider fn;
};
std::vector<ScreenProvider> g_screens;
PSEPluginHandle g_feelPlugin = 0;
DsfixTriggerFeelProvider g_feel = nullptr;

const char* PSE_CALL GameText(uint32_t hash) { return features::GameText(hash); }
const char* PSE_CALL ControlIconText(int player, uint32_t action) { return features::ControlIconText(player, action); }
int PSE_CALL OnlineMission() { return features::LockstepActive() ? 1 : 0; }

int PSE_CALL HostValue(const char* name) {
    const features::HostSettings* host = features::HostSync();
    if (!host || !name) return -1;
    if (strcmp(name, "jpfixes") == 0) return host->jpFixes;
    if (strcmp(name, "followers") == 0) return host->followers ? 1 : 0;
    if (strcmp(name, "weather") == 0) return host->weather;
    if (strcmp(name, "timeofday_on") == 0) return host->timeOfDayOn ? 1 : 0;
    if (strcmp(name, "timeofday") == 0) return host->timeOfDay;
    if (strcmp(name, "timeflow") == 0) return host->timeFlow;
    if (strcmp(name, "voices") == 0) return host->voices ? 1 : 0;
    if (strncmp(name, "voice", 5) == 0 && name[5] >= '0' && name[5] <= '3' && !name[6]) return host->voice[name[5] - '0'];
    return -1;
}

int PSE_CALL ModsGeneration() { return features::ModsGeneration(); }
uint32_t PSE_CALL VehicleHandlingCount() { return static_cast<uint32_t>(features::ModVehicleHandling().size()); }
int PSE_CALL VehicleHandlingAt(uint32_t i, const char** model, float* grip, float* spin) {
    const auto& all = features::ModVehicleHandling();
    if (i >= all.size()) return 0;
    if (model) *model = all[i].model.c_str();
    if (grip) *grip = all[i].grip;
    if (spin) *spin = all[i].spin;
    return 1;
}

PSEResult PSE_CALL SetTextProvider(PSEPluginHandle plugin, DsfixTextProvider fn) {
    if (!plugin) return PSE_E_INVALID_ARG;
    for (auto it = g_text.begin(); it != g_text.end(); ++it)
        if (it->plugin == plugin) {
            if (fn) it->fn = fn;
            else g_text.erase(it);
            return PSE_OK;
        }
    if (fn) g_text.push_back({plugin, fn});
    return PSE_OK;
}

PSEResult PSE_CALL SetTriggerFeelProvider(PSEPluginHandle plugin, DsfixTriggerFeelProvider fn) {
    if (!plugin) return PSE_E_INVALID_ARG;
    if (!fn) {
        if (g_feelPlugin == plugin) g_feelPlugin = 0, g_feel = nullptr;
        return PSE_OK;
    }
    g_feelPlugin = plugin;
    g_feel = fn;
    return PSE_OK;
}

int PSE_CALL DsfixSetting(const char* key) {
    const PSESettingsInterface* s = pse::SettingsInterface();
    return s && key ? s->GetInt(pse_host::Core(), key) : 0;
}

int PSE_CALL ModsFolderEnabled() { return features::ModsFolderEnabled() ? 1 : 0; }
const char* PSE_CALL SkinBody(const char* texture) {
    static thread_local std::string s;
    s = texture ? features::SkinBody(texture) : std::string();
    return s.c_str();
}
const char* PSE_CALL SkinNormalMap(const char* texture) {
    static thread_local std::string s;
    s = texture ? features::SkinNormalMap(texture) : std::string();
    return s.c_str();
}
void PSE_CALL ModReport(const char* line) {
    if (line) features::ModLog("%s", line);
}
int PSE_CALL LoadoutScreenOpen() { return features::CustomiseOpen() ? 1 : 0; }
void* PSE_CALL GiveSpawnedWeapon(void* soldier, const char* weapon, int mags) {
    return soldier && weapon ? features::GiveSpawnedWeapon(soldier, weapon, mags) : nullptr;
}
int PSE_CALL GiveSpawnedMedikit(void* soldier) { return soldier && features::GiveSpawnedMedikit(soldier) ? 1 : 0; }

struct SettingsListener {
    PSEPluginHandle plugin;
    void(PSE_CALL* fn)(void);
};
std::vector<SettingsListener> g_settingsListeners;

PSEResult PSE_CALL SetSettingsListener(PSEPluginHandle plugin, void(PSE_CALL* fn)(void)) {
    if (!plugin) return PSE_E_INVALID_ARG;
    std::erase_if(g_settingsListeners, [plugin](const SettingsListener& l) { return l.plugin == plugin; });
    if (fn) g_settingsListeners.push_back({plugin, fn});
    return PSE_OK;
}

int PSE_CALL GameFocused() { return features::GameFocused() ? 1 : 0; }

static_assert(sizeof(DsfixPuddlePart) == sizeof(render::PuddlePart) && sizeof(DsfixWaterLook) == sizeof(render::WaterLook));
int PSE_CALL DrawPuddleReflections(void* dev8, const DsfixPuddlePart* parts, int count, const DsfixWaterLook* look) {
    if (!dev8 || !parts || count <= 0 || !look) return 0;
    return render::DrawPuddleReflections(dev8, reinterpret_cast<const render::PuddlePart*>(parts), count,
                                         *reinterpret_cast<const render::WaterLook*>(look))
               ? 1
               : 0;
}

const char* PSE_CALL LinkedFrom() {
    static std::string s;
    s = features::LinkedFrom();
    return s.c_str();
}

const char* PSE_CALL SkinVoice(const char* texture) {
    static thread_local std::string s;
    s = texture ? features::SkinVoice(texture) : std::string();
    return s.c_str();
}
int PSE_CALL SpeechLanguage() { return features::SpeechLanguage(); }

PSEResult PSE_CALL SetScreenProvider(PSEPluginHandle plugin, DsfixScreenProvider fn) {
    if (!plugin) return PSE_E_INVALID_ARG;
    std::erase_if(g_screens, [plugin](const ScreenProvider& p) { return p.plugin == plugin; });
    if (fn) g_screens.push_back({plugin, fn});
    return PSE_OK;
}

const DsfixServicesV1 kServices = {sizeof(DsfixServicesV1), 1,
                                   &GameText,               &ControlIconText,
                                   &OnlineMission,          &HostValue,
                                   &ModsGeneration,         &VehicleHandlingCount,
                                   &VehicleHandlingAt,      &SetTextProvider,
                                   &SetTriggerFeelProvider, &DsfixSetting,
                                   &ModsFolderEnabled,      &SkinBody,
                                   &SkinNormalMap,          &ModReport,
                                   &LoadoutScreenOpen,      &GiveSpawnedWeapon,
                                   &GiveSpawnedMedikit,     &SetSettingsListener,
                                   &GameFocused,            &DrawPuddleReflections,
                                   &LinkedFrom,             &SkinVoice,
                                   &SpeechLanguage,         &SetScreenProvider};

void PSE_CALL OnLoaderMessage(const PSEMessage* msg, void*) {
    if (msg && msg->type == PSE_MSG_EXIT) {   // the plugins are unloaded after this message
        g_text.clear();
        g_screens.clear();
        g_settingsListeners.clear();
        g_feel = nullptr;
        g_feelPlugin = 0;
    }
}
}  // namespace

void features::RegisterServices() {
    static bool done = false;
    if (done) return;
    done = true;
    const PSEMessagingInterface* m = pse::MessagingInterface();
    const bool ok = m && m->RegisterInterface(pse_host::Core(), DSFIX_SERVICES_INTERFACE, 1, &kServices) == PSE_OK;
    if (m) m->RegisterListener(pse_host::Core(), "pse", &OnLoaderMessage, nullptr);
    dslog::Write(ok ? "[ok]   " DSFIX_SERVICES_INTERFACE " 1: registered (texts, online state, mods, providers)"
                    : "[fail] " DSFIX_SERVICES_INTERFACE ": interface not registered");
}

void features::ServiceSettingsChanged() {
    for (const SettingsListener& l : std::vector<SettingsListener>(g_settingsListeners)) l.fn();
}

void* features::ServiceScreen(uint32_t state) {
    for (const ScreenProvider& p : g_screens)
        if (void* s = p.fn(state)) return s;
    return nullptr;
}

const char* features::ServiceText(uint32_t hash) {
    for (const TextProvider& p : g_text)
        if (const char* t = p.fn(hash)) return t;
    return nullptr;
}

features::TriggerFeel features::PlayerTriggerFeel(int player) {
    if (!g_feel || player < 0 || player > 3) return TriggerFeel::Default;
    const int f = g_feel(player);
    return f >= DSFIX_FEEL_DEFAULT && f <= DSFIX_FEEL_LAUNCHER ? static_cast<TriggerFeel>(f) : TriggerFeel::Default;
}

// The weather plugin's interface (queried until found: dsfix.dll's callers only run after the plugins loaded).
const DsfixWeatherV1* features::WeatherPlugin() {
    static const DsfixWeatherV1* w = nullptr;
    if (!w)
        if (const PSEMessagingInterface* m = pse::MessagingInterface())
            if (auto p = static_cast<const DsfixWeatherV1*>(m->QueryInterface(pse_host::Core(), DSFIX_WEATHER_INTERFACE, 1));
                p && p->size >= sizeof(DsfixWeatherV1))
                w = p;
    return w;
}

// The time-of-day plugin (dsfix.timeofday); the functions below keep the names customise.cpp / hostsync.cpp used.
namespace {
const DsfixTimeOfDayV1* TimeOfDayPlugin() {
    static const DsfixTimeOfDayV1* t = nullptr;
    if (!t)
        if (const PSEMessagingInterface* m = pse::MessagingInterface())
            if (auto p = static_cast<const DsfixTimeOfDayV1*>(
                    m->QueryInterface(pse_host::Core(), DSFIX_TIMEOFDAY_INTERFACE, 1));
                p && p->size >= sizeof(DsfixTimeOfDayV1))
                t = p;
    return t;
}
}  // namespace

bool features::TimeOfDayInstalled() { return TimeOfDayPlugin() != nullptr; }
int features::TimeOfDay() { return TimeOfDayPlugin() ? TimeOfDayPlugin()->Time() : 0; }
int features::TimeFlow() { return TimeOfDayPlugin() ? TimeOfDayPlugin()->Flow() : 0; }
void features::SetTimeOfDay(int time, int flow) {
    if (TimeOfDayPlugin()) TimeOfDayPlugin()->Set(time, flow);
}
bool features::TimeContinues() { return TimeOfDayPlugin() && TimeOfDayPlugin()->Continues(); }
void features::RuleTimeOfDay() {
    if (TimeOfDayPlugin()) TimeOfDayPlugin()->Rule();
}
const char* features::TimeOfDayLabel(int i) { return TimeOfDayPlugin() ? TimeOfDayPlugin()->TimeLabel(i) : "MISSION DEFAULT"; }
const char* features::TimeFlowLabel(int i) { return TimeOfDayPlugin() ? TimeOfDayPlugin()->FlowLabel(i) : "MISSION DEFAULT"; }

// The voices plugin (dsfix.voices); the functions below keep the names customise.cpp / hostsync.cpp used.
namespace {
const DsfixVoicesV1* VoicesPlugin() {
    static const DsfixVoicesV1* v = nullptr;
    if (!v)
        if (const PSEMessagingInterface* m = pse::MessagingInterface())
            if (auto p = static_cast<const DsfixVoicesV1*>(m->QueryInterface(pse_host::Core(), DSFIX_VOICES_INTERFACE, 1));
                p && p->size >= sizeof(DsfixVoicesV1))
                v = p;
    return v;
}
}  // namespace

bool features::VoicesInstalled() { return VoicesPlugin() != nullptr; }
int features::VoiceChoice(int role) { return VoicesPlugin() ? VoicesPlugin()->Choice(role) : 0; }
void features::SetVoiceChoice(int role, int v) {
    if (VoicesPlugin()) VoicesPlugin()->SetChoice(role, v);
}
int features::CoopVoiceChoice(int role) { return VoicesPlugin() ? VoicesPlugin()->CoopChoice(role) : 0; }
const char* features::VoiceLabel(int v) { return VoicesPlugin() ? VoicesPlugin()->Label(v) : "AUTO"; }
std::string features::VoiceText(uint8_t* soldier, int role) {
    if (!VoicesPlugin()) return "AUTO";
    char buf[64];
    VoicesPlugin()->Text(soldier, role, buf, sizeof buf);
    return buf;
}
bool features::SoldierHasOwnVoice(uint8_t* soldier) { return VoicesPlugin() && VoicesPlugin()->HasOwnVoice(soldier); }

const char* features::FootprintLabel(int mode) {   // OPTIONS > DESERTSTORMFIX > FOOTPRINTS
    static const char* const kLabels[kFootprintModes] = {"OFF", "GAME (12 S)", "LONG (2 MIN)", "PERMANENT"};
    return kLabels[mode < 0 ? 0 : mode >= kFootprintModes ? kFootprintModes - 1 : mode];
}

bool features::PluginBool(const char* plugin, const char* key, bool fallback) {
    const PSEPluginHandle h = pse_host::Find(plugin);
    const PSESettingsInterface* s = pse::SettingsInterface();
    return h && s ? s->GetInt(h, key) != 0 : fallback;
}
