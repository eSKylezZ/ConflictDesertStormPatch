#pragma once
// The weather plugin's own declarations (weather.cpp, weathersound.cpp, footprints.cpp - in features.h until they left
// dsfix.dll on 2026-10-24) plus the two things of dsfix.dll they use, through dsfix.services. The three files keep
// calling each other as features::... .
#include <cstdint>
#include <string>
#include <vector>

#include "kit/kit.h"

namespace features {
// Footprints (footprints.cpp): life per the FOOTPRINTS setting, everyone's prints, wiped by the weather.
constexpr int kFootprintModes = 4;     // OFF, GAME, LONG, PERMANENT
void ApplyFootprints();
void OnFrameFootprints();
const char* FootprintLabel(int mode);
void SetFootprintWipe(float rate);     // extra seconds of age per second for every print (0 = none)
// A puddle decal on the ground at y, growing from startSize x radius; the decal, or nullptr.
void* AddPuddle(float x, float y, float z, float radius, float yaw, float startSize);
bool DecalAlive(const void* decal);
void KillDecal(void* decal);
void SetPuddleRain(float rain);        // puddles stay while > 0.15, dry up after
int PuddleCount();                     // puddles alive
struct PuddleInfo {
    float x, y, z, radius, yaw, opacity;   // centre on the ground (cm), spread so far, rim turn, drying 1 -> 0
    const void* decal;
    const uint8_t* parts;                  // the decal's geometry: part list (+0x64 indices, +0x68 vertices, +0x6C
                                           // vertex data, +0x70 index data, +0x78 visible, +0x7C next)
};
const std::vector<PuddleInfo>& Puddles();   // alive, from the last decal walk
void* WeatherPuddleTexture();          // weather.cpp: the puddle picture (null until made)

// Weather (weather.cpp + weathersound.cpp): WEATHER row on the loadout screen's MISSION tab, Lua game.weather.
constexpr int kWeatherCount = 13;      // MISSION DEFAULT, CLEAR, OVERCAST, LIGHT RAIN, RAIN, THUNDERSTORM, LIGHT SNOW,
                                       // SNOW, BLIZZARD, DUST, SANDSTORM, FOG, RANDOM
constexpr int kWeatherLightSnow = 6, kWeatherBlizzard = 8, kWeatherDust = 9, kWeatherSandstorm = 10;
void ApplyWeather();
void OnFrameWeather();
void OnFrameWeatherSound();
void StopWeatherSound();                // plugin unload: worker joined, voices + XAudio2 released
const char* WeatherLabel(int i);
int WeatherChoice();                   // the remembered choice (may be RANDOM)
void SetWeather(int choice);           // remembered, applied (changing over 3 s) in a running mission
void ScriptWeather(int kind, float seconds);   // Lua: this level only
int WeatherForHost();                  // the host's weather for an online mission (RANDOM resolved)
int WeatherKind();                     // this level's weather now (0 = the mission's own)
float WeatherRain();                   // rain density shown (0..~1.4)
float WindStrength();                  // m/s
float CameraCover();                   // view 0's camera: 1 in a building interior, 0.6 under a roof outdoors, 0 open
void WeatherThunder(float delaySeconds, float power);   // weathersound.cpp

// dsfix.dll, through dsfix.services
inline bool GameFocused() { const DsfixServicesV1* s = kit::Services(); return s && s->GameFocused(); }
inline int DsfixSetting(const char* key) { const DsfixServicesV1* s = kit::Services(); return s ? s->DsfixSetting(key) : 0; }
}  // namespace features
