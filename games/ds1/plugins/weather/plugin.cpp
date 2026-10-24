// Plugin dsfix.weather (PSE\Plugins\dsfix.weather.dll, dsfix kit, 2026-10-24): weather.cpp + weathersound.cpp +
// footprints.cpp, which were dsfix.dll's features dsfix.weather / dsfix.footprints. Their settings stay DesertStormFix's
// (FeatWeather, Footprints, FeatFootprintsAll, FeatFootprintsWeather - OPTIONS > DESERTSTORMFIX), read through
// dsfix.services; the remembered WEATHER choice is this plugin's hidden setting "Weather" (copied once from dsfix's
// key). Publishes dsfix.weather v1 (dsfix_weather.h) for dsfix.dll's loadout row, Lua game.weather and online sync.
#include "dsfix_weather.h"
#include "weather_local.h"

static_assert(DSFIX_WEATHER_COUNT == features::kWeatherCount);

namespace {
const char* PSE_CALL Label(int choice) { return features::WeatherLabel(choice); }
int PSE_CALL Choice() { return features::WeatherChoice(); }
void PSE_CALL SetChoice(int choice) { features::SetWeather(choice); }
void PSE_CALL ScriptWeather(int kind, float seconds) { features::ScriptWeather(kind, seconds); }
int PSE_CALL ForHost() { return features::WeatherForHost(); }
int PSE_CALL Kind() { return features::WeatherKind(); }

const DsfixWeatherV1 kWeather = {sizeof(DsfixWeatherV1), 1, &Label, &Choice, &SetChoice, &ScriptWeather, &ForHost, &Kind};

bool Load() {
    if (!kit::Services() || !kit::DeclareInt("Weather", 0, 0, features::kWeatherCount - 1)) return false;
    features::ApplyFootprints();
    features::ApplyWeather();
    // dsfix.dll's old per-frame order: weather, its sound, then the footprints
    if (!kit::OnFrame([] {
            features::OnFrameWeather();
            features::OnFrameWeatherSound();
            features::OnFrameFootprints();
        }))
        return false;
    return kit::Messaging()->RegisterInterface(kit::Self(), DSFIX_WEATHER_INTERFACE, 1, &kWeather) == PSE_OK;
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.weather", "Weather + footprints", PSE_PLUGIN_SIMULATION, Load)

// The loader frees this DLL at exit: the sound worker must be gone first (it ran on in freed code - crash on quit).
extern "C" __declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) { features::StopWeatherSound(); }
