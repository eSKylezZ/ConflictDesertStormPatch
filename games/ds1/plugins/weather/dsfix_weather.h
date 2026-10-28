#pragma once
// Interface "dsfix.weather" (published by the plugin dsfix.weather, PSE\Plugins\dsfix.weather.dll): what dsfix.dll asks
// of the weather - the loadout screen's WEATHER row (customise.cpp), Lua game.weather (scripthost.cpp) and the host's
// weather for an online mission (hostsync.cpp). Without the plugin dsfix.dll shows no WEATHER row, game.weather errors
// and online sends "no weather". Version 1 (2026-10-24); later versions only add members at the end (check `size`).
#include <cstdint>

#include "pse/pse_api.h"

#define DSFIX_WEATHER_INTERFACE "dsfix.weather"
// Choices: 0 MISSION DEFAULT, CLEAR, OVERCAST, LIGHT RAIN, RAIN, THUNDERSTORM, LIGHT SNOW, SNOW, BLIZZARD, DUST,
// SANDSTORM, FOG, and the last = RANDOM (a choice only; kinds are 0 .. count - 2).
#define DSFIX_WEATHER_COUNT 13

typedef struct DsfixWeatherV1 {
    uint32_t size;      // sizeof(DsfixWeatherV1) of the publishing plugin
    uint32_t version;   // 1
    const char*(PSE_CALL* Label)(int choice);          // "RAIN"; any index (clamped)
    int(PSE_CALL* Choice)(void);                       // the remembered choice (may be RANDOM)
    void(PSE_CALL* SetChoice)(int choice);             // remembered, applied (changing over 3 s) in a running mission
    void(PSE_CALL* ScriptWeather)(int kind, float seconds);   // Lua: this level only
    int(PSE_CALL* ForHost)(void);                      // the host's weather for an online mission (RANDOM resolved)
    int(PSE_CALL* Kind)(void);                         // this level's weather now (0 = the mission's own)
} DsfixWeatherV1;
