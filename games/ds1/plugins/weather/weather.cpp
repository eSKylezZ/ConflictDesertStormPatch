// Weather: clear / overcast / rain / thunderstorm / snow / blizzard / dust / sandstorm / fog for any mission (WEATHER row
// on the Squad Loadout screen's MISSION tab, remembered in the registry; online the host's choice). Research and the
// designs it follows (Conflict: Vietnam's CWeather, Global Storm, Denied Ops): research/weather.md.
//
// - Mood: the atmosphere [[0x63C948]+0x1A58] holds 7 colour-key arrays (SUNAMB +0x144, SUNDIF, FOG, FLARE, EAST, SKY,
//   WEST, each 4 x RGBA float for night / 6 / 12 / 18 h) that its per-view blend FUN_004eec30 turns into sun light, fog
//   colour, sky and flare, and fog start / stop (+4 / +0xC, base +8 / +0x10; the AI's sight range follows the fog stop,
//   0x40F896). After the game's atmosphere update (call 0x40FD8F -> FUN_004efc60, which also switches stencil shadows,
//   renderer [0x6406A4] +0x406B8) the keys are rewritten from a snapshot of the level's own, greyed / tinted / darkened
//   by the weather, and the fog pulled in (FUN_004efc10 thiscall(atm, start, stop) -> renderer fog states + far clip).
//   A change the game makes itself (a mission's set_sky / fog fades) is taken into the snapshot.
// - Wind: heading and strength wander round the preset's (Vietnam's model) with gusts; it slants rain / snow / sand,
//   leans the shrubs (sway channels 0x643C90, 16 x 0x28: phase, speed, amplitude, offset x / y / z - after the game's
//   update, call 0x40FC02 -> FUN_0050a690), drifts smoke / dust particles ([0x63C974] slots) and wipes footprints.
// - Precipitation: own batched draw after each view's transparents (call 0x40E356 -> FUN_004e2d20, thiscall world):
//   a field of drops / flakes / grains tiled through the world (one simulation for every split view), shown in a box
//   round the view's camera (from the device's view matrix). Each one is hidden below the top surface of its 2 m cell -
//   a downward ray (FUN_004de610 thiscall(world, 0, start, end, result 0xC0, flags, out zone) ret 0x18, hit point
//   result +0x28) cached per cell, a few per frame nearest first - so nothing falls under roofs or through floors;
//   rain splashes where a drop meets it. The ray only finds geometry of the zones round the camera, so a miss from
//   60 m up is tried again from 15 m and 3 m above the camera; then a ray up from the ground finds roofs, canopies and
//   bridges, whose underside becomes the cell's top (covered: no rain, no puddle).
// - Indoors: building interiors are their own zones - the scene's ENVI records under "Root" of type 0 (outdoor ground
//   is type 5, world pieces), e.g. MISSION1_HUT_INTERIOR1410. The level loader's scene read (call 0x40B606 ->
//   FUN_004bfb90) is shared to note their names; a zone root (scene object, parent +0xBC = 0, zone data +0x20C) keeps
//   the name's lower-case LFSR hash at +0x210. A view whose camera (camera manager [0x63C9D8], record +8 + id*0x1D8,
//   eye +0x68, zone +0x4C) is in an interior draws no precipitation or splashes, places no puddles, casts no rays, gets
//   a dimmer lightning flash; weathersound.cpp switches to the inside rain.
// - Sky: drawn first in each view (call 0x40E1EE -> FUN_004e2cc0 thiscall(world, a, b) ret 8) from its own dome and
//   cloud textures, which the colour keys only partly reach - right after it the whole view is blended towards the
//   weather's sky colour x daylight, so only the sky changes (the world is drawn over it).
// - Thunderstorms: lightning = every view's screen flash (view controllers [0x63C990 + i*4], FUN_004d2c30 thiscall(vc,
//   ms, r, g, b)) + a short burst of sky / ambient light.
// - Puddles: while it rains, about one a second on a flat, low 2 m cell near view 0's camera (its top and its four
//   neighbours known, none more than 12 cm off, it no higher than the lowest + 4 cm) - footprints.cpp's AddPuddle decals
//   (they grow, stay while it rains, dry up after). Wetness: the view darkens a little more the longer it rains, and
//   dries back over two minutes.
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "weather_local.h"

namespace {
constexpr uint32_t kEnvironment = 0x63C948, kAtmosphere = 0x1A58, kKeys = 0x144, kKeyFloats = 7 * 4 * 4;
constexpr uint32_t kAtmosUpdateCall = 0x40FD8F, kAtmosUpdate = 0x4EFC60, kSetFog = 0x4EFC10;
constexpr uint32_t kSwayCall = 0x40FC02, kSwayUpdate = 0x50A690, kSway = 0x643C90;
constexpr uint32_t kTransparentsCall = 0x40E356, kTransparents = 0x4E2D20;
constexpr uint32_t kSkyCall = 0x40E1EE, kSky = 0x4E2CC0;   // per view, before any of the world
constexpr uint32_t kRay = 0x4DE610, kWorldMinY = 0x1A80;
constexpr uint32_t kShadowRenderer = 0x6406A4, kShadowMode = 0x406B8;
constexpr uint32_t kViewControllers = 0x63C990, kFlash = 0x4D2C30;
constexpr uint32_t kParticles = 0x63C974;
constexpr uint32_t kGameTimer = 0x63C958, kRenderer = 0x63C924, kDevice = 0x40530;
constexpr uint32_t kLevelName = 0x606880, kDispatchMode = 0x60EC8C;
constexpr uint32_t kSceneRead = 0x40B606, kReadFile = 0x4BFB90;   // level loader: cdecl(name, &buf, &size, 0, type)
constexpr uint32_t kCameras = 0x63C9D8;

enum class Precip { None, Rain, Snow, Sand };
struct Preset {
    const char* label;
    float cloud;        // 0..1: sun light greyed / dimmed, sky towards the fog colour, no sun flare, no shadows (> 0.55)
    float light;        // direct sun light x
    float fogMix;       // 0..1 fog / sky colour towards fogColour (brightness kept)
    float fogColour[3];
    float fogBright;    // fog / sky brightness x
    float fogNear, fogFar;   // x the level's fog start / stop (0 = keep)
    float fogFarMax;    // cm, 0 = none
    Precip precip;
    float density;      // x the base count
    float wind;         // m/s
    bool lightning;
    float wipe;         // footprint wipe rate at full strength (extra seconds of age per second)
    float tint[3];      // the whole view x this (the world is lit ahead of time - the sun keys only light models)
    float sky[4];       // sky colour at full daylight (rgb) + how far the sky goes to it
};
// MISSION DEFAULT (0) and RANDOM (last) aren't presets.
const Preset kPresets[] = {
    {"MISSION DEFAULT", 0, 1, 0, {0, 0, 0}, 1, 1, 1, 0, Precip::None, 0, 0, false, 0, {1, 1, 1}, {0, 0, 0, 0}},
    {"CLEAR", 0, 1, 0, {0, 0, 0}, 1, 1, 1, 0, Precip::None, 0, 1.5f, false, 0, {1, 1, 1}, {0, 0, 0, 0}},
    {"OVERCAST", 0.7f, 0.7f, 0.55f, {0.64f, 0.65f, 0.67f}, 0.9f, 0.6f, 0.85f, 0, Precip::None, 0, 3, false, 0.3f, {0.84f, 0.84f, 0.86f}, {0.64f, 0.65f, 0.67f, 0.8f}},
    {"LIGHT RAIN", 0.75f, 0.65f, 0.65f, {0.55f, 0.56f, 0.58f}, 0.8f, 0.4f, 0.75f, 0, Precip::Rain, 0.35f, 3, false, 2, {0.78f, 0.79f, 0.81f}, {0.52f, 0.53f, 0.55f, 0.88f}},
    {"RAIN", 0.85f, 0.55f, 0.75f, {0.48f, 0.49f, 0.51f}, 0.7f, 0.25f, 0.6f, 0, Precip::Rain, 1, 5, false, 2.5f, {0.66f, 0.67f, 0.70f}, {0.42f, 0.43f, 0.45f, 0.93f}},
    {"THUNDERSTORM", 1, 0.4f, 0.8f, {0.38f, 0.39f, 0.42f}, 0.55f, 0.2f, 0.5f, 0, Precip::Rain, 1.4f, 9, true, 3.5f, {0.52f, 0.53f, 0.58f}, {0.28f, 0.29f, 0.32f, 0.96f}},
    {"LIGHT SNOW", 0.6f, 0.85f, 0.75f, {0.86f, 0.87f, 0.89f}, 1.2f, 0.5f, 0.75f, 0, Precip::Snow, 0.4f, 1.5f, false, 0.7f, {0.92f, 0.93f, 0.96f}, {0.80f, 0.81f, 0.83f, 0.85f}},
    {"SNOW", 0.75f, 0.75f, 0.88f, {0.86f, 0.87f, 0.89f}, 1.25f, 0.3f, 0.55f, 0, Precip::Snow, 1, 3, false, 1.5f, {0.86f, 0.88f, 0.92f}, {0.84f, 0.85f, 0.86f, 0.93f}},
    {"BLIZZARD", 0.9f, 0.6f, 0.95f, {0.86f, 0.87f, 0.89f}, 1.25f, 0, 0.3f, 3500, Precip::Snow, 2, 13, false, 4, {0.84f, 0.86f, 0.90f}, {0.88f, 0.88f, 0.89f, 0.97f}},
    {"DUST", 0.3f, 0.85f, 0.6f, {0.78f, 0.62f, 0.42f}, 1, 0.5f, 0.65f, 0, Precip::Sand, 0.4f, 7, false, 2, {0.97f, 0.88f, 0.74f}, {0.78f, 0.66f, 0.50f, 0.8f}},
    {"SANDSTORM", 0.7f, 0.55f, 0.95f, {0.80f, 0.58f, 0.34f}, 0.9f, 0, 0.3f, 3000, Precip::Sand, 1.5f, 15, false, 5, {0.92f, 0.72f, 0.50f}, {0.76f, 0.52f, 0.30f, 0.97f}},
    {"FOG", 0.5f, 0.75f, 0.85f, {0.74f, 0.75f, 0.76f}, 1, 0, 0.35f, 2500, Precip::None, 0, 0.5f, false, 0, {0.90f, 0.91f, 0.93f}, {0.76f, 0.77f, 0.78f, 0.95f}},
    {"RANDOM", 0, 1, 0, {0, 0, 0}, 1, 1, 1, 0, Precip::None, 0, 0, false, 0, {1, 1, 1}, {0, 0, 0, 0}},
};
constexpr int kKinds = static_cast<int>(sizeof kPresets / sizeof kPresets[0]);
static_assert(kKinds == features::kWeatherCount, "kWeatherCount");
constexpr int kRandom = kKinds - 1;

template <class T>
T& At(void* p, uint32_t off) { return *reinterpret_cast<T*>(static_cast<uint8_t*>(p) + off); }
uint8_t* Atmosphere() {
    auto* env = *reinterpret_cast<uint8_t**>(kEnvironment);
    return env ? *reinterpret_cast<uint8_t**>(env + kAtmosphere) : nullptr;
}
float Rand01() { return static_cast<float>(rand()) / RAND_MAX; }   // CRT rand: not the game's random tables
float RandRange(float a, float b) { return a + (b - a) * Rand01(); }

int g_drawn = 0, g_drawCalls = 0, g_rays = 0, g_rayHits = 0;   // dev log
double g_msUpdate = 0, g_msDraw = 0, g_msSky = 0;                 // dev log: time spent (sum over the log period)
double NowMs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return t.QuadPart * 1000.0 / f.QuadPart;
}

// ---- choice ----
int g_choice = 0;      // registry Weather (0 = the mission's own)
bool g_loaded = false;
int g_kind = 0;        // the weather this level has (RANDOM resolved), 0 = none
char g_level[32] = {};
bool g_levelOn = false;   // a level weather can apply to is running

void Load() {   // read every time: a pse.settings value (cheap), so a change from elsewhere counts too
    g_loaded = true;
    // the plugin's own hidden setting "Weather" (plugin.cpp declares it; copied once from dsfix's key)
    const int v = kit::GetInt("Weather");
    if (v >= 0 && v < kKinds) g_choice = v;
}
void Save() { kit::SetInt("Weather", g_choice); }

// The host's weather for this online mission (RANDOM resolved), else -1 (dsfix.dll's hostsync, field .W).
int HostWeather() {
    const DsfixServicesV1* s = kit::Services();
    return s ? s->HostValue("weather") : -1;
}

int Resolve(int choice) {
    if (choice != kRandom) return choice;
    return 1 + rand() % (kRandom - 1);
}

bool On() {
    if (HostWeather() >= 0) return true;
    return features::DsfixSetting("FeatWeather") != 0;
}

// ---- transition between the level's own look and the weather ----
const Preset* g_from = &kPresets[0];
const Preset* g_to = &kPresets[0];
float g_blend = 1.0f;          // 0 = g_from, 1 = g_to
float g_blendRate = 1.0f;      // per second
struct Mix {
    float cloud, light, fogMix, fogColour[3], fogBright, fogNear, fogFar, fogFarMax, density, wind, wipe, tint[3];
    float sky[3], skyMix;
    Precip precip;
    float precipFrom, precipTo;   // density of each side's precipitation (they may differ in kind)
    Precip kindFrom, kindTo;
    bool lightning;
};
float Lerp(float a, float b, float t) { return a + (b - a) * t; }
Mix Current() {
    const Preset& a = *g_from;
    const Preset& b = *g_to;
    const float t = g_blend;
    Mix m{};
    m.cloud = Lerp(a.cloud, b.cloud, t);
    m.light = Lerp(a.light, b.light, t);
    // the fog colour comes from whichever side has one
    const float ma = a.fogMix * (1 - t), mb = b.fogMix * t;
    m.fogMix = ma + mb;
    for (int i = 0; i < 3; ++i)
        m.fogColour[i] = m.fogMix > 0 ? (a.fogColour[i] * ma + b.fogColour[i] * mb) / m.fogMix : 0;
    m.fogBright = Lerp(a.fogBright, b.fogBright, t);
    m.fogNear = Lerp(a.fogNear, b.fogNear, t);
    m.fogFar = Lerp(a.fogFar, b.fogFar, t);
    const float fa = a.fogFarMax > 0 ? a.fogFarMax : 1e9f, fb = b.fogFarMax > 0 ? b.fogFarMax : 1e9f;
    m.fogFarMax = Lerp(std::min(fa, 1e6f), std::min(fb, 1e6f), t);
    m.wind = Lerp(a.wind, b.wind, t);
    for (int i = 0; i < 3; ++i) m.tint[i] = Lerp(a.tint[i], b.tint[i], t);
    // the sky colour from whichever side has one (as the fog colour)
    const float sa = a.sky[3] * (1 - t), sb = b.sky[3] * t;
    m.skyMix = sa + sb;
    for (int i = 0; i < 3; ++i) m.sky[i] = m.skyMix > 0 ? (a.sky[i] * sa + b.sky[i] * sb) / m.skyMix : 0;
    m.wipe = Lerp(a.wipe, b.wipe, t);
    m.kindFrom = a.precip, m.kindTo = b.precip;
    m.precipFrom = a.density * (1 - t), m.precipTo = b.density * t;
    m.lightning = b.lightning && t > 0.5f;
    return m;
}

void StartWeather(int kind, float seconds) {
    g_from = g_blend >= 0.5f ? g_to : g_from;   // a change mid-transition starts from the nearer side
    g_to = &kPresets[std::clamp(kind, 0, kKinds - 2)];
    g_blend = seconds > 0 ? 0.0f : 1.0f;
    g_blendRate = seconds > 0 ? 1.0f / seconds : 1.0f;
    g_kind = kind;
    dslog::Write("Weather: %s%s", g_to->label, seconds > 0 ? " (changing)" : "");
}

// ---- wind ----
float g_windHeading = 0.7f, g_windBase = 0.7f;   // radians (x = sin, z = cos)
float g_windStrength = 0.0f;                     // m/s, current
float g_gust = 0.0f, g_gustTarget = 0.0f, g_gustTimer = 5.0f;
float g_windVec[3] = {};                         // cm/s

void UpdateWind(float dt, float base) {
    g_windHeading += RandRange(-1.0f, 1.0f) * 0.25f * dt;
    g_windHeading = std::clamp(g_windHeading, g_windBase - 0.6f, g_windBase + 0.6f);
    g_gustTimer -= dt;
    if (g_gustTimer <= 0) {
        g_gustTarget = g_gustTarget > 0 ? 0.0f : RandRange(0.3f, 0.8f);
        g_gustTimer = g_gustTarget > 0 ? RandRange(1.0f, 2.5f) : RandRange(4.0f, 12.0f);
    }
    g_gust += (g_gustTarget - g_gust) * std::min(1.0f, dt * 1.5f);
    const float target = base * (1.0f + g_gust) * (1.0f + 0.15f * std::sin(GetTickCount() * 0.0007f));
    g_windStrength += (target - g_windStrength) * std::min(1.0f, dt * 0.8f);
    g_windVec[0] = std::sin(g_windHeading) * g_windStrength * 100.0f;
    g_windVec[1] = 0.0f;
    g_windVec[2] = std::cos(g_windHeading) * g_windStrength * 100.0f;
}

// ---- mood ----
float g_orig[kKeyFloats];       // the level's own keys
float g_written[kKeyFloats];    // what we last wrote
bool g_haveOrig = false;
float g_origFog[2] = {}, g_writtenFog[2] = {};   // start, stop
bool g_haveFog = false;
float g_flash = 0.0f;           // lightning light, decays

float Lum(const float* c) { return 0.3f * c[0] + 0.59f * c[1] + 0.11f * c[2]; }

void ApplyMood(uint8_t* atm, const Mix& m) {
    float* keys = &At<float>(atm, kKeys);
    if (!g_haveOrig || std::memcmp(keys, g_written, sizeof g_written) != 0) {   // first time, or the game set its own
        std::memcpy(g_orig, keys, sizeof g_orig);
        g_haveOrig = true;
    }
    float out[kKeyFloats];
    std::memcpy(out, g_orig, sizeof out);
    for (int arr = 0; arr < 7; ++arr)
        for (int k = 0; k < 4; ++k) {
            float* c = out + (arr * 4 + k) * 4;
            const float lum = Lum(c);
            switch (arr) {
                case 0: {   // SUNAMB: ambient keeps its level, loses colour; a little brighter under cloud
                    for (int i = 0; i < 3; ++i) c[i] = Lerp(c[i], lum, m.cloud * 0.7f) * (1.0f + m.cloud * 0.15f);
                    break;
                }
                case 1: {   // SUNDIF: direct sun dimmed and greyed
                    for (int i = 0; i < 3; ++i) c[i] = Lerp(c[i], lum, m.cloud * 0.8f) * m.light;
                    break;
                }
                case 3: {   // FLARE: no sun disc behind cloud
                    for (int i = 0; i < 3; ++i) c[i] *= std::max(0.0f, 1.0f - m.cloud * 1.3f);
                    break;
                }
                default: {  // FOG, EAST, SKY, WEST: towards the weather's colour, the key's brightness kept
                    if (m.fogMix <= 0) break;
                    const float target = std::max(Lum(m.fogColour), 0.01f);
                    for (int i = 0; i < 3; ++i)
                        c[i] = Lerp(c[i], m.fogColour[i] * lum / target, m.fogMix) * Lerp(1.0f, m.fogBright, m.fogMix);
                    break;
                }
            }
            if (g_flash > 0 && (arr == 0 || arr == 2 || arr == 5 || arr == 4 || arr == 6)) {
                const float add = g_flash * (arr == 0 ? 0.5f : 0.35f);
                c[0] += add * 0.85f, c[1] += add * 0.9f, c[2] += add;
            }
            for (int i = 0; i < 3; ++i) c[i] = std::clamp(c[i], 0.0f, 1.0f);   // the game turns them into bytes unclamped (1.1 wraps)
        }
    std::memcpy(keys, out, sizeof out);
    std::memcpy(g_written, out, sizeof out);

    // fog distances (cm)
    float& start = At<float>(atm, 4);
    float& stop = At<float>(atm, 0xC);
    if (!g_haveFog || start != g_writtenFog[0] || stop != g_writtenFog[1]) {
        if (g_haveFog && std::fabs(start - g_writtenFog[0]) < 0.05f * std::max(1.0f, g_writtenFog[0]) &&
            std::fabs(stop - g_writtenFog[1]) < 0.05f * std::max(1.0f, g_writtenFog[1])) {
            g_origFog[0] += start - g_writtenFog[0];   // a script's fog fade: move the original by the same step
            g_origFog[1] += stop - g_writtenFog[1];
        } else {
            g_origFog[0] = start, g_origFog[1] = stop;
        }
        g_haveFog = true;
    }
    float newStop = std::min(g_origFog[1] * m.fogFar, m.fogFarMax);
    newStop = std::max(newStop, 1500.0f);
    float newStart = g_origFog[0] * m.fogNear;
    if (newStart > newStop * 0.8f && (m.fogFar < 1.0f || m.fogFarMax < 1e6f)) newStart = newStop * 0.8f;
    if (newStart != start || newStop != stop) {
        reinterpret_cast<void(__thiscall*)(void*, float, float)>(kSetFog)(atm, newStart, newStop);
        At<float>(atm, 8) = newStart;   // base = current: the AI's sight follows the fog without the extra 10 m cut
        At<float>(atm, 0x10) = newStop;
    }
    g_writtenFog[0] = newStart, g_writtenFog[1] = newStop;
}

void __fastcall AtmosUpdate(uint8_t* atm) {
    reinterpret_cast<void(__fastcall*)(void*)>(kAtmosUpdate)(atm);
    if (!g_levelOn || g_kind == 0 && g_blend >= 1.0f && g_from == g_to) return;
    const Mix m = Current();
    ApplyMood(atm, m);
    if (m.cloud > 0.55f)
        if (auto* r = *reinterpret_cast<uint8_t**>(kShadowRenderer)) At<int>(r, kShadowMode) = 0;
}

// ---- shrubs lean with the wind ----
float g_swayAmp[16];
bool g_haveSway = false;
void __fastcall SwayUpdate(void* self) {
    auto* ch = reinterpret_cast<float*>(kSway);
    if (!g_haveSway) {
        for (int i = 0; i < 16; ++i) g_swayAmp[i] = ch[i * 10 + 2];
        g_haveSway = true;
    }
    const float w = g_levelOn ? std::min(g_windStrength / 12.0f, 1.5f) : 0.0f;
    for (int i = 0; i < 16; ++i) ch[i * 10 + 2] = g_swayAmp[i] * (1.0f + w * 1.5f);
    reinterpret_cast<void(__thiscall*)(void*)>(kSwayUpdate)(self);
    if (w <= 0) return;
    const float lean = 0.05f * w;
    const float dx = std::sin(g_windHeading), dz = std::cos(g_windHeading);
    for (int i = 0; i < 16; ++i) ch[i * 10 + 3] += dx * lean, ch[i * 10 + 5] += dz * lean;
}

// ---- smoke / dust drift ----
void DriftParticles(float dt) {
    auto* mgr = *reinterpret_cast<uint8_t**>(kParticles);
    if (!mgr || g_windStrength < 0.5f) return;
    auto* slots = At<uint8_t*>(mgr, 4);
    const int cap = At<int>(mgr, 0xC);
    if (!slots || cap <= 0 || cap > 100000) return;
    for (int i = 0; i < cap; ++i) {
        uint8_t* p = slots + i * 200;
        if (At<int>(p, 0xB0) != 0) continue;
        const int type = At<int>(p, 0xAC);
        if (type != 6 && type != 7 && type != 8 && type != 18 && type != 9 && type != 10) continue;
        float* pos = &At<float>(p, 0x44);
        pos[0] += g_windVec[0] * dt * 0.5f;
        pos[2] += g_windVec[2] * dt * 0.5f;
    }
}

// ---- interiors ----
// Zone name hash as the game keeps it (+0x210): LFSR over the lower-cased name (texture / catalog hash).
uint32_t NameHash(const char* s, size_t n) {
    uint32_t x = 1;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t c = static_cast<uint8_t>(tolower(static_cast<uint8_t>(s[i])));
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) & 1) ^ (x & 1) ^ ((x >> 1) & 1) ^ ((x >> 21) & 1) ^ ((x >> 31) & 1);
            x = (x << 1) | in;
        }
    }
    return x;
}
std::vector<uint32_t> g_interiors;   // this level's interior zones (name hashes)
int g_sceneZones = 0;                // root zones seen in the scene (dev log)

// ENVI <flags> <n> "<name>" "Root" <type> ... -> interiors = type != 5
void NoteScene(const char* text, int size) {
    g_interiors.clear();
    g_sceneZones = 0;
    for (int pos = 0; pos < size;) {
        int end = pos;
        while (end < size && text[end] != '\n') ++end;
        const char* l = text + pos;
        const int len = end - pos;
        pos = end + 1;
        if (len < 20 || strncmp(l, "ENVI ", 5) != 0) continue;
        const char* q[4] = {};
        int nq = 0;
        for (int i = 0; i < len && nq < 4; ++i)
            if (l[i] == '"') q[nq++] = l + i;
        if (nq < 4 || q[3] - q[2] != 5 || strncmp(q[2] + 1, "Root", 4) != 0) continue;
        ++g_sceneZones;
        if (atoi(q[3] + 1) != 5) g_interiors.push_back(NameHash(q[0] + 1, static_cast<size_t>(q[1] - q[0] - 1)));
    }
}
using ReadFileFn = int(__cdecl*)(const char*, char**, int*, int, int);
void* g_readNext = reinterpret_cast<void*>(kReadFile);
int __cdecl ReadScene(const char* name, char** buf, int* size, int a4, int type) {
    const int r = reinterpret_cast<ReadFileFn>(g_readNext)(name, buf, size, a4, type);
    if (r == 1 && buf && *buf && size && *size > 8 && memcmp(*buf, "Version", 7) == 0) {
        __try {
            NoteScene(*buf, *size);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_interiors.clear();
        }
    }
    return r;
}

bool ZoneIsInterior(const uint8_t* zone) {
    if (!zone || g_interiors.empty()) return false;
    __try {
        for (int i = 0; i < 64 && *reinterpret_cast<const uint8_t* const*>(zone + 0xBC); ++i)
            zone = *reinterpret_cast<const uint8_t* const*>(zone + 0xBC);
        const uint32_t h = *reinterpret_cast<const uint32_t*>(zone + 0x210);
        return std::find(g_interiors.begin(), g_interiors.end(), h) != g_interiors.end();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every view's camera this frame: eye and whether it is in an interior.
struct ViewCam {
    float eye[3];
    bool indoor;
};
ViewCam g_viewCams[4];
int g_viewCamCount = 0;
bool g_indoor0 = false;   // view 0's camera is in an interior
// The camera a view was drawn from (matched by eye): indoors?
bool IndoorView(const float* eye) {
    for (int i = 0; i < g_viewCamCount; ++i) {
        const float dx = eye[0] - g_viewCams[i].eye[0], dy = eye[1] - g_viewCams[i].eye[1], dz = eye[2] - g_viewCams[i].eye[2];
        if (dx * dx + dy * dy + dz * dz < 100.0f * 100.0f) return g_viewCams[i].indoor;
    }
    return false;
}

// ---- top surface per 2 m cell ----
constexpr float kCell = 200.0f;
std::unordered_map<uint64_t, float> g_tops;   // cell -> top surface y (cm), -1e9 = nothing below
uint64_t CellKey(int x, int z) { return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z); }

// A copy of the cells round view 0's camera in a plain array, made once a frame: every drop looks its cell up each frame
// (a hash lookup per drop per view was a large part of the weather's cost).
constexpr int kGrid = 48;
constexpr float kUnknown = 3.0e9f;
float g_grid[kGrid * kGrid];
int g_gridX0 = 0, g_gridZ0 = 0;
bool g_gridOn = false;

void BuildGrid(const float* cam) {
    g_gridX0 = static_cast<int>(std::floor(cam[0] / kCell)) - kGrid / 2;
    g_gridZ0 = static_cast<int>(std::floor(cam[2] / kCell)) - kGrid / 2;
    for (int z = 0; z < kGrid; ++z)
        for (int x = 0; x < kGrid; ++x) {
            const auto it = g_tops.find(CellKey(g_gridX0 + x, g_gridZ0 + z));
            g_grid[z * kGrid + x] = it == g_tops.end() ? kUnknown : it->second;
        }
    g_gridOn = true;
}

float TopAt(float x, float z, bool& known) {
    const int cx = static_cast<int>(std::floor(x / kCell)), cz = static_cast<int>(std::floor(z / kCell));
    const int gx = cx - g_gridX0, gz = cz - g_gridZ0;
    if (g_gridOn && gx >= 0 && gz >= 0 && gx < kGrid && gz < kGrid) {
        const float t = g_grid[gz * kGrid + gx];
        known = t != kUnknown;
        return known ? t : 0.0f;
    }
    const auto it = g_tops.find(CellKey(cx, cz));
    known = it != g_tops.end();
    return known ? it->second : 0.0f;
}

// The game's world ray faulted at 0x4CCF82 on a level's first frames (front end -> Mission 1 with rain / thunder,
// 2026-10-19: the zone data it walks wasn't set up yet, or the view eye still lay outside every zone). So rays wait
// kRayWarmupFrames played frames, only start from points inside the world box (world +0x1A7C min, +0x1A88 max), and
// run fault-guarded: a fault turns the rays off for the rest of the level (one log line) instead of killing the game.
constexpr uint32_t kWorldMin = 0x1A7C, kWorldMax = 0x1A88;
constexpr int kRayWarmupFrames = 30;
int g_rayFrames = 0;        // played frames of this level (OnFrameWeatherBody)
bool g_raysFaulted = false;
using RayFn = int(__thiscall*)(void*, void*, float*, float*, void*, uint32_t, void*);
int GuardedRay(void* world, float* start, float* end, uint8_t* result) {
    __try {
        return reinterpret_cast<RayFn>(kRay)(world, nullptr, start, end, result, 0x1C88, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// NaN = "can't ask now" (warm-up, outside the world box, rays off): the caller must not cache it as "no ground".
constexpr float kRayNotNow = std::numeric_limits<float>::quiet_NaN();
// Vertical ray from fromY to toY (NaN = down to the world floor): the hit's y, -1e9 = nothing.
float Cast(float x, float z, float fromY, float toY) {
    void* world = *reinterpret_cast<void**>(kEnvironment);
    if (!world || g_raysFaulted || g_rayFrames < kRayWarmupFrames) return kRayNotNow;
    const float* lo = reinterpret_cast<const float*>(static_cast<uint8_t*>(world) + kWorldMin);
    const float* hi = reinterpret_cast<const float*>(static_cast<uint8_t*>(world) + kWorldMax);
    if (!(lo[0] < hi[0]) || x < lo[0] || x > hi[0] || z < lo[2] || z > hi[2]) return kRayNotNow;
    if (std::isnan(toY)) toY = At<float>(world, kWorldMinY) - 100.0f;
    float start[3] = {x, fromY, z}, end[3] = {x, toY, z};
    alignas(16) uint8_t result[0xC0] = {};
    const int hit = GuardedRay(world, start, end, result);
    if (hit < 0) {
        g_raysFaulted = true;
        dslog::Write("[warn] Weather: the game's ground ray faulted on %s - weather rays off for this level", g_level);
        return kRayNotNow;
    }
    return hit ? At<float>(result, 0x2C) : -1e9f;
}

// A cell's top seen from a camera at height camY: the ground (from 60 m up, else from 15 m / 3 m above the camera -
// the ray only knows the zones round it), or what covers it (a ray up from the ground). Counts its rays.
float CellTop(float x, float z, float camY, int& rays) {
    float ground = -1e9f;
    for (const float above : {6000.0f, 1500.0f, 300.0f}) {
        ground = Cast(x, z, camY + above, kRayNotNow);
        ++rays;
        if (std::isnan(ground)) return ground;
        if (ground > -1e8f) break;
    }
    if (ground < -1e8f) return ground;
    const float roof = Cast(x, z, ground + 60.0f, ground + 6000.0f);
    ++rays;
    if (std::isnan(roof)) return roof;
    return roof > -1e8f ? roof : ground;
}

// Fills cells round (cx, cz) nearest first, at most `budget` rays.
void FillTops(float cx, float cy, float cz, int radius, int budget) {
    const int ox = static_cast<int>(std::floor(cx / kCell)), oz = static_cast<int>(std::floor(cz / kCell));
    for (int r = 0; r <= radius && budget > 0; ++r)
        for (int dx = -r; dx <= r && budget > 0; ++dx)
            for (int dz = -r; dz <= r && budget > 0; ++dz) {
                if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
                const uint64_t key = CellKey(ox + dx, oz + dz);
                if (g_tops.count(key)) continue;
                int rays = 0;
                const float top = CellTop((ox + dx + 0.5f) * kCell, (oz + dz + 0.5f) * kCell, cy, rays);
                budget -= rays;
                g_rays += rays;
                if (std::isnan(top)) continue;   // not now - ask again on a later frame
                g_tops[key] = top;
                if (top > -1e8f) ++g_rayHits;
            }
}

// ---- the precipitation field ----
// Positions are kept in [0, size) and shown in the box round each camera (tiled through the world).
constexpr float kRainBox[3] = {3200, 2400, 3200}, kSnowBox[3] = {2600, 1800, 2600}, kSandBox[3] = {3000, 1200, 3000};
struct Drop {
    float p[3];
    float speed;   // fall cm/s (rain / snow), drift factor (sand)
    float phase;
    float size;
};
struct Field {
    Precip kind = Precip::None;
    std::vector<Drop> drops;
    float box[3] = {};
    int active = 0;   // how many are shown (density)
};
Field g_fields[2];   // [0] = the weather fading out, [1] = fading in
struct Splash {
    float p[3];
    float age;
};
std::vector<Splash> g_splashes;
float g_cam0[3] = {};   // view 0's camera (cm) - splashes, tops
bool g_haveCam = false;

void InitField(Field& f, Precip kind) {
    if (f.kind == kind && !f.drops.empty()) return;
    f.kind = kind;
    f.drops.clear();
    if (kind == Precip::None) return;
    const float* box = kind == Precip::Rain ? kRainBox : kind == Precip::Snow ? kSnowBox : kSandBox;
    std::memcpy(f.box, box, sizeof f.box);
    const int n = kind == Precip::Rain ? 8000 : kind == Precip::Snow ? 7000 : 3500;
    f.drops.resize(n);
    for (Drop& d : f.drops) {
        for (int i = 0; i < 3; ++i) d.p[i] = Rand01() * box[i];
        d.speed = kind == Precip::Rain ? RandRange(900, 1200) : kind == Precip::Snow ? RandRange(70, 140) : RandRange(0.7f, 1.3f);
        d.phase = Rand01() * 6.2831853f;
        d.size = kind == Precip::Snow ? RandRange(3.5f, 7.5f) : kind == Precip::Sand ? RandRange(1.0f, 2.2f) : 1.0f;
    }
}

float Wrap(float v, float size) {
    v = std::fmod(v, size);
    return v < 0 ? v + size : v;
}

// World position of a drop in the box round `cam`.
void Place(const Field& f, const Drop& d, const float* cam, float* w) {
    for (int i = 0; i < 3; ++i) {
        const float lo = cam[i] - f.box[i] * (i == 1 ? 0.45f : 0.5f);
        w[i] = lo + Wrap(d.p[i] - lo, f.box[i]);
    }
}

void StepField(Field& f, float dt, float density) {
    f.active = std::min(static_cast<int>(f.drops.size()), static_cast<int>(f.drops.size() * std::min(density, 2.0f) / 2.0f));
    if (f.kind == Precip::None || f.active == 0) return;
    const float t = GetTickCount() * 0.001f;
    for (int i = 0; i < f.active; ++i) {
        Drop& d = f.drops[i];
        float before[3] = {};
        if (f.kind == Precip::Rain && g_haveCam) Place(f, d, g_cam0, before);
        switch (f.kind) {
            case Precip::Rain:
                d.p[0] += g_windVec[0] * 0.8f * dt, d.p[2] += g_windVec[2] * 0.8f * dt, d.p[1] -= d.speed * dt;
                break;
            case Precip::Snow: {
                const float swirl = 25.0f + g_windStrength * 4.0f;
                d.p[0] += (g_windVec[0] + std::sin(t * 1.3f + d.phase) * swirl) * dt;
                d.p[2] += (g_windVec[2] + std::cos(t * 1.1f + d.phase) * swirl) * dt;
                d.p[1] -= d.speed * (1.0f + g_windStrength * 0.05f) * dt;
                break;
            }
            default: {   // sand: carried by the wind, rising and falling a little
                d.p[0] += g_windVec[0] * d.speed * dt;
                d.p[2] += g_windVec[2] * d.speed * dt;
                d.p[1] += std::sin(t * 2.0f + d.phase) * 60.0f * dt - 20.0f * dt;
                break;
            }
        }
        for (int k = 0; k < 3; ++k) d.p[k] = Wrap(d.p[k], f.box[k]);
        if (f.kind == Precip::Rain && g_haveCam && g_splashes.size() < 400) {   // splash where it meets the top surface
            float after[3];
            Place(f, d, g_cam0, after);
            bool known = false;
            const float top = TopAt(after[0], after[2], known);
            if (known && before[1] >= top && after[1] < top && top > -1e8f) {
                const float dx = after[0] - g_cam0[0], dz = after[2] - g_cam0[2];
                if (dx * dx + dz * dz < 1200.0f * 1200.0f) g_splashes.push_back({{after[0], top + 2.0f, after[2]}, 0});
            }
        }
    }
}

// ---- drawing ----
enum Dev : int {
    kCreateTexture = 20, kSetTransform = 37, kGetTransform = 38, kSetRenderState = 50, kGetRenderState = 51,
    kGetTexture = 60, kSetTexture = 61, kGetTss = 62, kSetTss = 63, kDrawPrimitiveUP = 72, kSetVertexShader = 76,
    kGetVertexShader = 77, kSetPixelShader = 88, kGetPixelShader = 89
};
void** Vt(void* o) { return *static_cast<void***>(o); }
template <class Fn>
Fn M(void* o, int i) { return reinterpret_cast<Fn>(Vt(o)[i]); }
using SetRs = HRESULT(__stdcall*)(void*, DWORD, DWORD);
using GetRs = HRESULT(__stdcall*)(void*, DWORD, DWORD*);
using SetTss = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD);
using GetTss = HRESULT(__stdcall*)(void*, DWORD, DWORD, DWORD*);
using SetTex = HRESULT(__stdcall*)(void*, DWORD, void*);
using GetTex = HRESULT(__stdcall*)(void*, DWORD, void**);
using SetDw = HRESULT(__stdcall*)(void*, DWORD);
using GetDw = HRESULT(__stdcall*)(void*, DWORD*);
using Xform = HRESULT(__stdcall*)(void*, DWORD, float*);
using DrawUP = HRESULT(__stdcall*)(void*, uint32_t, UINT, const void*, UINT);

struct LineVertex {
    float x, y, z;
    DWORD colour;
};
struct QuadVertex {
    float x, y, z;
    DWORD colour;
    float u, v;
};
void* g_device = nullptr;
void* g_flake = nullptr;    // soft round dot
void* g_puff = nullptr;     // soft cloud for dust sheets
void* g_streak = nullptr;   // rain streak: soft sides, fading ends
void* g_puddleTex = nullptr;   // puddle: dark, slightly blue, ragged edge
float g_pixel = 0.0012f;    // ~a pixel per cm of distance in the current view (from the projection)

void* MakeTexture(void* dev, int n, bool cloud) {
    using Create = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, uint32_t, uint32_t, void**);
    struct Locked {
        INT pitch;
        void* bits;
    };
    using Lock = HRESULT(__stdcall*)(void*, UINT, Locked*, const RECT*, DWORD);
    using Unlock = HRESULT(__stdcall*)(void*, UINT);
    void* tex = nullptr;
    if (FAILED(M<Create>(dev, kCreateTexture)(dev, n, n, 1, 0, 21 /*A8R8G8B8*/, 1 /*MANAGED*/, &tex)) || !tex) return nullptr;
    Locked lr{};
    if (SUCCEEDED(M<Lock>(tex, 16)(tex, 0, &lr, nullptr, 0))) {
        for (int y = 0; y < n; ++y) {
            auto* row = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.bits) + y * lr.pitch);
            for (int x = 0; x < n; ++x) {
                const float dx = (x + 0.5f) / n * 2 - 1, dy = (y + 0.5f) / n * 2 - 1;
                float a = std::max(0.0f, 1.0f - std::sqrt(dx * dx + dy * dy));
                if (n == 48) {   // puddle (48 x 48): dark water, a ragged rim
                    const float ang = std::atan2(dy, dx);
                    const float rim = 0.78f + 0.12f * std::sin(ang * 3.0f + 0.7f) + 0.07f * std::sin(ang * 7.0f + 2.1f);
                    const float r = std::sqrt(dx * dx + dy * dy) / rim;
                    a = std::clamp((1.0f - r) * 6.0f, 0.0f, 1.0f) * 0.62f;
                    const float sheen = 0.05f * std::max(0.0f, 1.0f - std::fabs(dx + dy + 0.3f) * 3.0f);
                    const int lum = static_cast<int>(std::clamp(0.14f + sheen, 0.0f, 1.0f) * 255.0f);
                    row[x] = static_cast<uint32_t>(a * 255.0f) << 24 | static_cast<uint32_t>(lum * 0.96f) << 16 |
                             static_cast<uint32_t>(lum * 0.98f) << 8 | static_cast<uint32_t>(std::min(255, lum + 4));
                    continue;
                }
                if (n == 16) {   // streak (16 x 16): soft sides, fades towards both ends
                    a = std::max(0.0f, 1.0f - std::fabs(dx)) * std::min(1.0f, (1.0f - std::fabs(dy)) * 3.0f);
                } else if (cloud) {   // lumpy: a few sines over the falloff
                    const float noise = 0.65f + 0.35f * std::sin(x * 0.45f + std::sin(y * 0.3f) * 2.0f) *
                                                    std::cos(y * 0.37f + std::sin(x * 0.21f) * 2.0f);
                    a = a * a * noise;
                } else {
                    a = std::pow(a, 1.5f);
                }
                row[x] = static_cast<uint32_t>(std::clamp(a, 0.0f, 1.0f) * 255.0f) << 24 | 0xFFFFFF;
            }
        }
        M<Unlock>(tex, 17)(tex, 0);
    }
    return tex;
}

struct View {
    float m[16];      // view matrix (row-vector convention)
    float eye[3], right[3], up[3], fwd[3];
};
bool ReadView(void* dev, View& v) {
    if (FAILED(M<Xform>(dev, kGetTransform)(dev, 2 /*VIEW*/, v.m))) return false;
    const float* m = v.m;
    for (int j = 0; j < 3; ++j) v.eye[j] = -(m[12] * m[j * 4 + 0] + m[13] * m[j * 4 + 1] + m[14] * m[j * 4 + 2]);
    for (int j = 0; j < 3; ++j) v.right[j] = m[j * 4 + 0], v.up[j] = m[j * 4 + 1], v.fwd[j] = m[j * 4 + 2];
    return true;
}

// Saves every state it changes and puts it back (the game keeps its own cache of them).
class Scope {
public:
    explicit Scope(void* dev) : dev_(dev) {
        for (size_t i = 0; i < kN; ++i) M<GetRs>(dev, kGetRenderState)(dev, kStates[i], &rs_[i]);
        for (size_t i = 0; i < kT; ++i) M<GetTss>(dev, kGetTss)(dev, 0, kStage[i], &tss_[i]);
        M<GetTss>(dev, kGetTss)(dev, 1, 1, &tss1_);
        M<GetTex>(dev, kGetTexture)(dev, 0, &tex_);
        M<GetDw>(dev, kGetVertexShader)(dev, &vs_);
        M<GetDw>(dev, kGetPixelShader)(dev, &ps_);
        M<Xform>(dev, kGetTransform)(dev, 256 /*WORLD*/, world_);
        static const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        M<Xform>(dev, kSetTransform)(dev, 256, const_cast<float*>(identity));
        const DWORD set[][2] = {{7, 1}, {14, 0}, {15, 0}, {19, 5}, {20, 6}, {22, 1}, {27, 1}, {28, 0}, {52, 0}, {137, 0}};
        for (auto& s : set) M<SetRs>(dev, kSetRenderState)(dev, s[0], s[1]);
        M<SetTss>(dev, kSetTss)(dev, 1, 1, 1 /*DISABLE*/);
        M<SetDw>(dev, kSetPixelShader)(dev, 0);
    }
    ~Scope() {
        M<Xform>(dev_, kSetTransform)(dev_, 256, world_);
        M<SetTex>(dev_, kSetTexture)(dev_, 0, tex_);
        if (tex_) M<ULONG(__stdcall*)(void*)>(tex_, 2)(tex_);
        M<SetDw>(dev_, kSetVertexShader)(dev_, vs_);
        M<SetDw>(dev_, kSetPixelShader)(dev_, ps_);
        M<SetTss>(dev_, kSetTss)(dev_, 1, 1, tss1_);
        for (size_t i = 0; i < kT; ++i) M<SetTss>(dev_, kSetTss)(dev_, 0, kStage[i], tss_[i]);
        for (size_t i = 0; i < kN; ++i) M<SetRs>(dev_, kSetRenderState)(dev_, kStates[i], rs_[i]);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    void Textured(void* tex) {
        const DWORD op = tex ? 4 /*MODULATE*/ : 3 /*SELECTARG2*/;
        const DWORD stage[][2] = {{1, op}, {2, 2}, {3, 0}, {4, op}, {5, 2}, {6, 0}, {13, 3}, {14, 3}, {16, 2}, {17, 2}, {18, 0}};
        for (auto& s : stage) M<SetTss>(dev_, kSetTss)(dev_, 0, s[0], s[1]);
        M<SetTex>(dev_, kSetTexture)(dev_, 0, tex);
        M<SetDw>(dev_, kSetVertexShader)(dev_, tex ? 0x142u : 0x42u);   // XYZ | DIFFUSE [| TEX1]
    }

private:
    static constexpr DWORD kStates[] = {7, 14, 15, 19, 20, 22, 27, 28, 52, 137};
    static constexpr size_t kN = sizeof kStates / sizeof kStates[0];
    static constexpr DWORD kStage[] = {1, 2, 3, 4, 5, 6, 13, 14, 16, 17, 18};
    static constexpr size_t kT = sizeof kStage / sizeof kStage[0];
    void* dev_;
    DWORD rs_[kN], tss_[kT], tss1_ = 0, vs_ = 0, ps_ = 0;
    float world_[16];
    void* tex_ = nullptr;
};

DWORD Argb(float a, float r, float g, float b) {
    auto c = [](float v) { return static_cast<DWORD>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return c(a) << 24 | c(r) << 16 | c(g) << 8 | c(b);
}

// Fades drops out near the box's sides (no popping as the camera moves) and hides those under a roof / the ground.
float Visible(const Field& f, const float* w, const float* cam) {
    bool known = false;
    const float top = TopAt(w[0], w[2], known);
    if (!known || w[1] < top) return 0.0f;
    const float ex = 1.0f - std::fabs(w[0] - cam[0]) / (f.box[0] * 0.5f), ez = 1.0f - std::fabs(w[2] - cam[2]) / (f.box[2] * 0.5f);
    return std::clamp(std::min(ex, ez) * 4.0f, 0.0f, 1.0f);
}

std::vector<LineVertex> g_lines;
std::vector<QuadVertex> g_quads;

void FlushQuads(void* dev) {
    for (size_t i = 0; i < g_quads.size(); i += 3 * 1800) {
        const UINT tris = static_cast<UINT>(std::min<size_t>(g_quads.size() - i, 3 * 1800) / 3);
        M<DrawUP>(dev, kDrawPrimitiveUP)(dev, 4 /*TRIANGLELIST*/, tris, g_quads.data() + i, sizeof(QuadVertex));
    }
    g_quads.clear();
}
void Billboard(const View& v, const float* p, float sx, float sy, DWORD colour) {
    QuadVertex q[4];
    const float c[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (int k = 0; k < 4; ++k) {
        for (int i = 0; i < 3; ++i) (&q[k].x)[i] = p[i] + v.right[i] * c[k][0] * sx + v.up[i] * c[k][1] * sy;
        q[k].colour = colour;
        q[k].u = c[k][0] * 0.5f + 0.5f, q[k].v = c[k][1] * 0.5f + 0.5f;
    }
    const int order[6] = {0, 1, 2, 0, 2, 3};
    for (int k : order) g_quads.push_back(q[k]);
}

float g_brightness = 1.0f;   // the scene's light (sun ambient + diffuse at the current hour): drops follow it
// The level's fog (atmosphere +0 colour, +4 start, +0xC stop - after the weather pulled it in): our drops / flakes /
// sheets / splashes fade into it by distance like the world does (the fixed-function fog isn't used for them).
float g_fogStart = 2000.0f, g_fogEnd = 8000.0f, g_fogColour[3] = {0.5f, 0.5f, 0.5f};
float FogAt(float dist) { return std::clamp((dist - g_fogStart) / std::max(1.0f, g_fogEnd - g_fogStart), 0.0f, 1.0f); }
float Dist(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// colour (rgb 0..1) faded into the fog, alpha thinned with it
DWORD Fogged(float a, float r, float g, float b, float dist) {
    const float f = FogAt(dist);
    return Argb(a * (1.0f - f), Lerp(r, g_fogColour[0], f), Lerp(g, g_fogColour[1], f), Lerp(b, g_fogColour[2], f));
}
// in front of the view (behind it nothing is drawn: half the drops skipped)
bool Ahead(const View& v, const float* w) {
    return (w[0] - v.eye[0]) * v.fwd[0] + (w[1] - v.eye[1]) * v.fwd[1] + (w[2] - v.eye[2]) * v.fwd[2] > -60.0f;
}

void DrawField(void* dev, Scope& s, const Field& f, const View& v) {
    if (f.kind == Precip::None || f.active == 0) return;
    const float light = std::clamp(g_brightness, 0.15f, 1.0f);
    if (f.kind == Precip::Rain) {
        // streaks: quads along the fall, facing the camera, at least ~1.5 pixels wide at any distance
        s.Textured(g_streak);
        const float len = 0.07f;   // seconds of fall shown as the streak
        const float vx = g_windVec[0] * 0.8f, vz = g_windVec[2] * 0.8f;
        for (int i = 0; i < f.active; ++i) {
            const Drop& d = f.drops[i];
            float w[3];
            Place(f, d, v.eye, w);
            if (!Ahead(v, w)) continue;
            const float vis = Visible(f, w, v.eye);
            if (vis <= 0) continue;
            const float top[3] = {w[0] - vx * len, w[1] + d.speed * len, w[2] - vz * len};
            const float axis[3] = {top[0] - w[0], top[1] - w[1], top[2] - w[2]};
            const float to[3] = {v.eye[0] - w[0], v.eye[1] - w[1], v.eye[2] - w[2]};
            const float dist = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
            float side[3] = {axis[1] * to[2] - axis[2] * to[1], axis[2] * to[0] - axis[0] * to[2], axis[0] * to[1] - axis[1] * to[0]};
            const float sl = std::sqrt(side[0] * side[0] + side[1] * side[1] + side[2] * side[2]);
            if (sl < 1e-3f) continue;
            const float half = 0.25f + dist * g_pixel;   // cm
            for (float& c : side) c *= half / sl;
            const DWORD colour = Fogged(0.6f * vis, 0.82f * light, 0.85f * light, 0.92f * light, dist);
            const QuadVertex q[4] = {{w[0] - side[0], w[1] - side[1], w[2] - side[2], colour, 0, 1},
                                     {w[0] + side[0], w[1] + side[1], w[2] + side[2], colour, 1, 1},
                                     {top[0] + side[0], top[1] + side[1], top[2] + side[2], colour, 1, 0},
                                     {top[0] - side[0], top[1] - side[1], top[2] - side[2], colour, 0, 0}};
            const int order[6] = {0, 1, 2, 0, 2, 3};
            for (int k : order) g_quads.push_back(q[k]);
            ++g_drawn;
        }
        FlushQuads(dev);
        return;
    }
    if (f.kind == Precip::Snow) {
        s.Textured(g_flake);
        for (int i = 0; i < f.active; ++i) {
            const Drop& d = f.drops[i];
            float w[3];
            Place(f, d, v.eye, w);
            if (!Ahead(v, w)) continue;
            const float vis = Visible(f, w, v.eye);
            if (vis <= 0) continue;
            Billboard(v, w, d.size, d.size, Fogged(0.85f * vis, 0.95f * light, 0.96f * light, light, Dist(w, v.eye)));
        }
        FlushQuads(dev);
        return;
    }
    // sand: streaked grains along the wind, then big soft dust sheets
    s.Textured(g_flake);
    const float wl = std::max(1.0f, std::sqrt(g_windVec[0] * g_windVec[0] + g_windVec[2] * g_windVec[2]));
    const float wx = g_windVec[0] / wl, wz = g_windVec[2] / wl;
    for (int i = 0; i < f.active; ++i) {
        const Drop& d = f.drops[i];
        float w[3];
        Place(f, d, v.eye, w);
        if (!Ahead(v, w)) continue;
        const float vis = Visible(f, w, v.eye);
        if (vis <= 0) continue;
        // a short streak: a quad stretched along the wind (built from the wind and camera up)
        const float half = std::min(30.0f, wl * 0.02f) + 2.0f;
        QuadVertex q[4];
        const float a[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
        const DWORD colour = Fogged(0.5f * vis, 0.82f * light, 0.66f * light, 0.45f * light, Dist(w, v.eye));
        for (int k = 0; k < 4; ++k) {
            q[k].x = w[0] + wx * half * a[k][0] + v.up[0] * d.size * a[k][1];
            q[k].y = w[1] + v.up[1] * d.size * a[k][1];
            q[k].z = w[2] + wz * half * a[k][0] + v.up[2] * d.size * a[k][1];
            q[k].colour = colour;
            q[k].u = a[k][0] * 0.5f + 0.5f, q[k].v = a[k][1] * 0.5f + 0.5f;
        }
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k : order) g_quads.push_back(q[k]);
    }
    FlushQuads(dev);
    s.Textured(g_puff);
    const int sheets = std::min(f.active / 40, 90);
    for (int i = 0; i < sheets; ++i) {
        const Drop& d = f.drops[i * 37 % f.drops.size()];
        float w[3];
        Place(f, d, v.eye, w);
        if (!Ahead(v, w)) continue;
        const float vis = Visible(f, w, v.eye);
        if (vis <= 0) continue;
        const float size = 250.0f + d.size * 150.0f;
        Billboard(v, w, size, size * 0.6f, Fogged(0.10f * vis, 0.80f * light, 0.62f * light, 0.40f * light, Dist(w, v.eye)));
    }
    FlushQuads(dev);
}

void DrawSplashes(void* dev, Scope& s, const View& v) {
    if (g_splashes.empty()) return;
    s.Textured(g_flake);
    const float light = std::clamp(g_brightness, 0.15f, 1.0f);
    for (const Splash& sp : g_splashes) {
        const float t = sp.age / 0.18f;
        const float size = 3.0f + 7.0f * t;
        Billboard(v, sp.p, size, size * 0.45f,
                  Fogged(0.45f * (1.0f - t), 0.8f * light, 0.82f * light, 0.88f * light, Dist(sp.p, v.eye)));
    }
    FlushQuads(dev);
}

// The whole view x the weather's tint (multiplied: DEST x SRC colour), lifted by a lightning flash.
float g_tint[3] = {1, 1, 1};
float g_wet = 0.0f;   // 0 dry .. 1 soaked
void Tint(void* dev, Scope& s, const DWORD* vp, bool indoor) {   // indoors: no wet look, a dimmer flash
    float c[3];
    const float wetness = indoor ? 0.0f : g_wet, flash = g_flash * (indoor ? 0.25f : 0.6f);
    const float wet[3] = {1.0f - 0.14f * wetness, 1.0f - 0.13f * wetness, 1.0f - 0.10f * wetness};
    for (int i = 0; i < 3; ++i) c[i] = std::min(1.0f, g_tint[i] * wet[i] + flash);
    if ((c[0] > 0.995f && c[1] > 0.995f && c[2] > 0.995f) || vp[2] == 0) return;
    struct V {
        float x, y, z, rhw;
        DWORD colour;
    };
    const float x0 = static_cast<float>(vp[0]) - 0.5f, y0 = static_cast<float>(vp[1]) - 0.5f;
    const float x1 = x0 + vp[2], y1 = y0 + vp[3];
    const DWORD colour = Argb(1, c[0], c[1], c[2]);
    const V quad[4] = {{x0, y0, 0, 1, colour}, {x1, y0, 0, 1, colour}, {x0, y1, 0, 1, colour}, {x1, y1, 0, 1, colour}};
    s.Textured(nullptr);
    DWORD fog = 0;
    M<GetRs>(dev, kGetRenderState)(dev, 28 /*FOGENABLE*/, &fog);
    M<SetRs>(dev, kSetRenderState)(dev, 28, 0);
    M<SetRs>(dev, kSetRenderState)(dev, 7, 0);
    M<SetRs>(dev, kSetRenderState)(dev, 19, 1 /*ZERO*/);
    M<SetRs>(dev, kSetRenderState)(dev, 20, 3 /*SRCCOLOR*/);
    M<SetDw>(dev, kSetVertexShader)(dev, 0x44);   // XYZRHW | DIFFUSE
    M<DrawUP>(dev, kDrawPrimitiveUP)(dev, 5 /*TRIANGLESTRIP*/, 2, quad, sizeof(V));
    M<SetRs>(dev, kSetRenderState)(dev, 19, 5);
    M<SetRs>(dev, kSetRenderState)(dev, 20, 6);
    M<SetRs>(dev, kSetRenderState)(dev, 7, 1);
    M<SetRs>(dev, kSetRenderState)(dev, 28, fog);
}

// Blends the view (holding only the sky so far) towards the weather's sky colour, fading into the fog colour lower
// down - where the fogged world meets it.
float g_skyMix = 0.0f, g_skyColour[3] = {};
float g_rainNow = 0.0f;   // rain shown (puddle ripples)
void __fastcall Sky(void* world, void*, void* a, void* b) {
    reinterpret_cast<void(__thiscall*)(void*, void*, void*)>(kSky)(world, a, b);
    if (!g_levelOn || g_skyMix < 0.01f) return;
    struct Timer {
        double t0 = NowMs();
        ~Timer() { g_msSky += NowMs() - t0; }
    } timer;
    const auto* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    void* dev = r ? *reinterpret_cast<void* const*>(r + kDevice) : nullptr;
    uint8_t* atm = Atmosphere();
    if (!dev || !atm) return;
    DWORD vp[6] = {};
    using Viewport = HRESULT(__stdcall*)(void*, DWORD*);
    if (FAILED(M<Viewport>(dev, 41 /*GetViewport*/)(dev, vp)) || !vp[2] || !vp[3]) return;
    const DWORD top = Argb(g_skyMix, g_skyColour[0], g_skyColour[1], g_skyColour[2]);
    const DWORD bottom = Argb(g_skyMix, g_fogColour[0], g_fogColour[1], g_fogColour[2]);
    struct V {
        float x, y, z, rhw;
        DWORD colour;
    };
    const float x0 = static_cast<float>(vp[0]) - 0.5f, y0 = static_cast<float>(vp[1]) - 0.5f;
    const float x1 = x0 + vp[2], y1 = y0 + vp[3];
    const V quad[4] = {{x0, y0, 0, 1, top}, {x1, y0, 0, 1, top}, {x0, y1, 0, 1, bottom}, {x1, y1, 0, 1, bottom}};
    Scope s(dev);
    s.Textured(nullptr);
    DWORD fogOn = 0;
    M<GetRs>(dev, kGetRenderState)(dev, 28 /*FOGENABLE*/, &fogOn);
    M<SetRs>(dev, kSetRenderState)(dev, 28, 0);
    M<SetRs>(dev, kSetRenderState)(dev, 7, 0);
    M<SetDw>(dev, kSetVertexShader)(dev, 0x44);   // XYZRHW | DIFFUSE
    M<DrawUP>(dev, kDrawPrimitiveUP)(dev, 5 /*TRIANGLESTRIP*/, 2, quad, sizeof(V));
    M<SetRs>(dev, kSetRenderState)(dev, 28, fogOn);
}

void DrawWeather(void* world);
// a shared call (broker chain): nameplates.cpp reads each view's camera there too
void* g_transparentsNext = reinterpret_cast<void*>(kTransparents);
void __fastcall Transparents(void* world) {
    reinterpret_cast<void(__thiscall*)(void*)>(g_transparentsNext)(world);
    if (!g_levelOn) return;
    const double t0 = NowMs();
    DrawWeather(world);
    g_msDraw += NowMs() - t0;
}
void DrawWeather(void*) {
    const bool any = g_fields[0].active > 0 || g_fields[1].active > 0 || !g_splashes.empty() || g_tint[0] < 0.995f ||
                     g_tint[1] < 0.995f || g_tint[2] < 0.995f || g_flash > 0 || g_wet > 0.01f ||
                     !features::Puddles().empty();
    if (!any) return;
    const auto* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    void* dev = r ? *reinterpret_cast<void* const*>(r + kDevice) : nullptr;
    if (!dev) return;
    if (dev != g_device) {   // first use or a new device: our textures with it
        g_device = dev;
        g_flake = MakeTexture(dev, 32, false);
        g_puff = MakeTexture(dev, 64, true);
        g_streak = MakeTexture(dev, 16, false);
        g_puddleTex = MakeTexture(dev, 48, false);
    }
    View v;
    if (!ReadView(dev, v)) return;
    // pixel size: projection _22 = cot(fov / 2) -> one pixel per cm of distance = 2 / (_22 * viewport height)
    float proj[16];
    DWORD vp[6] = {};
    using Viewport = HRESULT(__stdcall*)(void*, DWORD*);
    if (SUCCEEDED(M<Xform>(dev, kGetTransform)(dev, 3 /*PROJECTION*/, proj)) &&
        SUCCEEDED(M<Viewport>(dev, 41 /*GetViewport*/)(dev, vp)) && proj[5] > 0 && vp[3] > 0)
        g_pixel = 1.6f / (proj[5] * vp[3]);
    if (!features::Puddles().empty()) {   // D3D9 renderers: the puddles mirror the scene (no-op on Renderer 0)
        // in front of the view and within 40 m, nearest first, at most 12 (the scene copy only when one is in view)
        static std::vector<std::pair<float, const features::PuddleInfo*>> close;
        static std::vector<DsfixPuddlePart> parts;
        close.clear();
        parts.clear();
        for (const features::PuddleInfo& p : features::Puddles()) {
            const float c[3] = {p.x, p.y, p.z};
            const float d = Dist(c, v.eye);
            const float ahead = (p.x - v.eye[0]) * v.fwd[0] + (p.y - v.eye[1]) * v.fwd[1] + (p.z - v.eye[2]) * v.fwd[2];
            if (d < 4000.0f && ahead > -p.radius && d < g_fogEnd) close.push_back({d, &p});
        }
        std::sort(close.begin(), close.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        if (close.size() > 12) close.resize(12);
        for (const auto& n : close)
            for (const uint8_t* part = n.second->parts; part && parts.size() < 64;
                 part = *reinterpret_cast<const uint8_t* const*>(part + 0x7C)) {
                if (!*reinterpret_cast<const int*>(part + 0x78)) continue;
                parts.push_back({*reinterpret_cast<void* const*>(part + 0x6C), *reinterpret_cast<const int*>(part + 0x68),
                                 *reinterpret_cast<const uint16_t* const*>(part + 0x70), *reinterpret_cast<const int*>(part + 0x64),
                                 n.second->opacity});
            }
        if (!parts.empty()) {
            DsfixWaterLook look{};
            std::memcpy(look.sky, g_skyColour, sizeof look.sky);
            std::memcpy(look.fogColour, g_fogColour, sizeof look.fogColour);
            look.fogStart = g_fogStart, look.fogEnd = g_fogEnd;
            look.timeSeconds = static_cast<float>(GetTickCount() % 1000000) * 0.001f;
            look.rain = g_rainNow;
            look.mask = g_puddleTex;
            if (const DsfixServicesV1* s = kit::Services()) s->DrawPuddleReflections(dev, parts.data(), static_cast<int>(parts.size()), &look);
        }
    }
    Scope s(dev);
    ++g_drawCalls;
    const bool indoor = IndoorView(v.eye);
    if (!indoor) {
        for (const Field& f : g_fields) DrawField(dev, s, f, v);
        DrawSplashes(dev, s, v);
    }
    Tint(dev, s, vp, indoor);
}

// ---- lightning ----
float g_strikeTimer = 8.0f;
void Lightning(float dt, bool on) {
    g_flash = std::max(0.0f, g_flash - dt * 5.0f);
    if (!on) return;
    g_strikeTimer -= dt;
    if (g_strikeTimer > 0) return;
    g_strikeTimer = RandRange(6.0f, 22.0f);
    g_flash = RandRange(0.6f, 1.0f);
    const auto* r = *reinterpret_cast<uint8_t* const*>(kRenderer);
    const int views = r ? std::clamp(*reinterpret_cast<const int*>(r + 0x20), 1, 4) : 1;
    for (int i = 0; i < views; ++i)
        if (void* vc = reinterpret_cast<void**>(kViewControllers)[i]) {
            const bool in = i < g_viewCamCount && g_viewCams[i].indoor;   // indoors: only what comes through windows
            reinterpret_cast<void(__thiscall*)(void*, int, uint8_t, uint8_t, uint8_t)>(kFlash)(vc, 140, in ? 18 : 70,
                                                                                         in ? 18 : 72, in ? 20 : 80);
        }
    features::WeatherThunder(RandRange(0.4f, 3.5f), g_flash);
#ifndef DS_DIST
    dslog::Write("[dev] Weather: lightning (%.2f)", g_flash);
#endif
}

bool LevelFits(const char* level) {
    if (_strnicmp(level, "FrontEnd", 8) == 0 || _strnicmp(level, "intro", 5) == 0 || !level[0]) return false;
    return true;
}

uint32_t g_lastMs = 0;

// ---- puddles ----
// Each puddle is a spot (a hollow 2 m cell): its decal is taken away when every camera is more than 45 m off and put
// back (already spread) when one comes within 35 m again, while it rains or the ground is still wet - so only puddles
// round the players exist. New spots: about one a second while it rains, at least 5 m from others, at most 10 x rain
// shown at once near the camera.
struct Spot {
    float x, y, z, radius, yaw;
    void* decal;
};
std::unordered_map<uint64_t, Spot> g_spots;
float g_puddleTimer = 0.0f, g_spotTimer = 0.0f, g_dryFor = 0.0f;   // dryFor: seconds since the rain stopped
float g_eyes[4][3];
int g_eyeCount = 0;

// Water gathers in hollows: the cell's 8 neighbours all known and within 8 cm of it (flat over 6 x 6 m), and it no
// higher than the mean of the 5 x 5 cells round it (not a mound or a flat top). maxRadius: 160 cm when the whole 10 x
// 10 m is within 15 cm, else 100 cm.
bool PuddleSpot(int cx, int cz, float& top, float& maxRadius) {
    const auto it = g_tops.find(CellKey(cx, cz));
    if (it == g_tops.end() || it->second < -1e8f) return false;
    top = it->second;
    float sum = 0.0f, spread = 0.0f;
    int n = 0;
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            if (!dx && !dz) continue;
            const auto c = g_tops.find(CellKey(cx + dx, cz + dz));
            const bool inner = std::abs(dx) <= 1 && std::abs(dz) <= 1;
            if (c == g_tops.end() || c->second < -1e8f) {
                if (inner) return false;
                continue;
            }
            const float d = std::fabs(c->second - top);
            if (inner && d > 8.0f) return false;
            spread = std::max(spread, d);
            sum += c->second, ++n;
        }
    if (n < 12 || top > sum / n + 1.0f) return false;
    maxRadius = spread <= 15.0f ? 160.0f : 100.0f;
    return true;
}

float NearestEye(float x, float z) {
    float best = 1e9f;
    for (int i = 0; i < g_eyeCount; ++i) {
        const float dx = x - g_eyes[i][0], dz = z - g_eyes[i][2];
        best = std::min(best, dx * dx + dz * dz);
    }
    return std::sqrt(best);
}

bool AddSpot(int cx, int cz, float x, float top, float z, float radius, float yaw) {
    for (const auto& kv : g_spots) {   // 5 m apart
        const float dx = kv.second.x - x, dz = kv.second.z - z;
        if (dx * dx + dz * dz < 500.0f * 500.0f) return false;
    }
    Spot spot{x, top, z, radius, yaw, nullptr};
    spot.decal = features::AddPuddle(x, top, z, radius, yaw, 0.35f);
    if (!spot.decal) return false;
    g_spots[CellKey(cx, cz)] = spot;
    return true;
}

// Twice a second: spots out of every camera's range lose their decal, spots back in range get theirs again; dried-up
// spots (no rain for 100 s) go.
void TendSpots(float rain) {
    for (auto it = g_spots.begin(); it != g_spots.end();) {
        Spot& sp = it->second;
        const float d = NearestEye(sp.x, sp.z);
        const bool alive = features::DecalAlive(sp.decal);
        if (!alive) sp.decal = nullptr;
        if (alive && d > 4500.0f) {
            features::KillDecal(sp.decal);
            sp.decal = nullptr;
        } else if (!alive && d < 3500.0f && rain > 0.15f) {   // back in range while it rains: already spread
            sp.decal = features::AddPuddle(sp.x, sp.y, sp.z, sp.radius, sp.yaw, 1.0f);
        }
        // dried up: faded out in view after the rain, or out of range for 100 s of dry weather
        if (!sp.decal && rain <= 0.15f && (d < 3500.0f || g_dryFor > 100.0f)) it = g_spots.erase(it);
        else ++it;
    }
}

int SpotsShown() {
    int n = 0;
    for (const auto& kv : g_spots) n += kv.second.decal != nullptr;
    return n;
}

void PlacePuddles(float dt, float rain) {
    g_dryFor = rain > 0.15f ? 0.0f : g_dryFor + dt;
    g_spotTimer -= dt;
    if (g_spotTimer <= 0) {
        g_spotTimer = 0.5f;
        TendSpots(rain);
    }
#ifndef DS_DIST
    {   // Dev\PuddleNear = 1: one puddle in the hollow nearest 4 m ahead of player 1 (reflection tests)
        static bool done = false;
        DWORD v = 0, size = sizeof v;
        static const bool want = RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "PuddleNear",
                                              RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v;
        const auto* soldier = *reinterpret_cast<const uint8_t* const*>(0x60F5B8 + 0x310);
        if (want && !done && soldier && g_haveCam && rain > 0.2f) {
            const float* s = reinterpret_cast<const float*>(soldier + 0x30);
            float fx = s[0] - g_cam0[0], fz = s[2] - g_cam0[2];
            const float l = std::sqrt(fx * fx + fz * fz);
            if (l > 1.0f) {
                fx /= l, fz /= l;
                const int ax = static_cast<int>(std::floor((s[0] + fx * 400.0f) / kCell));
                const int az = static_cast<int>(std::floor((s[2] + fz * 400.0f) / kCell));
                static int tries = 0;
                for (int r = 0; r <= 4 && !done; ++r)
                    for (int dz = -r; dz <= r && !done; ++dz)
                        for (int dx = -r; dx <= r && !done; ++dx) {
                            float top, maxRadius;
                            if (std::max(std::abs(dx), std::abs(dz)) != r || !PuddleSpot(ax + dx, az + dz, top, maxRadius)) continue;
                            const float x = (ax + dx + 0.5f) * kCell, z = (az + dz + 0.5f) * kCell;
                            done = AddSpot(ax + dx, az + dz, x, top, z, maxRadius, 0.4f);
                            if (done) dslog::Write("[dev] Weather: test puddle at (%.0f %.0f %.0f) soldier (%.0f %.0f %.0f)", x, top, z,
                                                   s[0], s[1], s[2]);
                        }
                if (!done && ++tries > 600) done = true;
            }
        }
    }
#endif
    if (rain < 0.2f || !g_haveCam || g_indoor0) return;
    g_puddleTimer -= dt;
    if (g_puddleTimer > 0) return;
    g_puddleTimer = 1.0f / rain;
    if (SpotsShown() >= std::min(14, static_cast<int>(10 * rain + 0.5f)) || g_spots.size() > 200) return;
    const int ox = static_cast<int>(std::floor(g_cam0[0] / kCell)), oz = static_cast<int>(std::floor(g_cam0[2] / kCell));
    for (int tries = 0; tries < 8; ++tries) {
        const int cx = ox + static_cast<int>(RandRange(-9.0f, 9.99f)), cz = oz + static_cast<int>(RandRange(-9.0f, 9.99f));
        float top = 0.0f, maxRadius = 0.0f;
        if (g_spots.count(CellKey(cx, cz)) || !PuddleSpot(cx, cz, top, maxRadius)) continue;
        const float x = (cx + RandRange(0.35f, 0.65f)) * kCell, z = (cz + RandRange(0.35f, 0.65f)) * kCell;
        if (AddSpot(cx, cz, x, top, z, RandRange(60.0f, maxRadius), RandRange(0.0f, 6.2831853f))) {
#ifndef DS_DIST
            static int logged = 0;
            if (logged++ < 3) dslog::Write("[dev] Weather: puddle at (%.0f %.0f %.0f)", x, top, z);
#endif
            return;
        }
    }
}
}  // namespace

void* features::WeatherPuddleTexture() { return g_puddleTex; }

const char* features::WeatherLabel(int i) { return kPresets[std::clamp(i, 0, kKinds - 1)].label; }
int features::WeatherChoice() { Load(); return g_choice; }
float features::WindStrength() { return g_levelOn ? g_windStrength : 0.0f; }   // 0 in the front end (sound off)
int features::WeatherKind() { return g_levelOn ? g_kind : 0; }
float features::WeatherRain() {
    float r = 0;
    if (!g_levelOn) return r;   // the front end: the last mission's drops are still in the fields
    for (const Field& f : g_fields)
        if (f.kind == Precip::Rain && !f.drops.empty()) r += static_cast<float>(f.active) / (f.drops.size() / 2.0f);
    return r;
}
float features::CameraCover() {
    if (!g_haveCam) return 0.0f;
    if (g_indoor0) return 1.0f;
    bool known = false;
    const float top = TopAt(g_cam0[0], g_cam0[2], known);
    return known && top > g_cam0[1] + 50.0f ? 0.6f : 0.0f;   // outdoors under a roof / canopy / bridge
}

void features::SetWeather(int choice) {
    Load();
    g_choice = std::clamp(choice, 0, kKinds - 1);
    Save();
    if (g_levelOn && On()) StartWeather(Resolve(g_choice), 3.0f);
}

// Lua / editor: weather kind by index, changing over `seconds`.
void features::ScriptWeather(int kind, float seconds) {
    if (g_levelOn) StartWeather(std::clamp(kind, 0, kKinds - 2), seconds);
}

int features::WeatherForHost() {
    Load();
    return Resolve(g_choice);
}

void OnFrameWeatherBody();
void features::OnFrameWeather() {
    const double t0 = NowMs();
    OnFrameWeatherBody();
    g_msUpdate += NowMs() - t0;
}
void OnFrameWeatherBody() {
    Load();
    const char* level = reinterpret_cast<const char*>(kLevelName);
    static bool firstFrame = false;   // the new level's first played frame: only its clock is taken (below)
    if (strncmp(level, g_level, sizeof g_level - 1) != 0) {   // a new level
        strncpy_s(g_level, level, sizeof g_level - 1);
        firstFrame = true;
        g_tops.clear();
        g_splashes.clear();
        g_haveOrig = g_haveFog = g_haveSway = false;
        g_wet = 0.0f;
        g_spots.clear();
        g_gridOn = false;
        g_flash = 0;
        g_rayFrames = 0;
        g_raysFaulted = false;
        g_levelOn = LevelFits(level) && On();
        const int host = HostWeather();
        const int kind = !g_levelOn ? 0 : host >= 0 ? host : Resolve(g_choice);
        g_from = g_to = &kPresets[std::clamp(kind, 0, kKinds - 2)];
        g_blend = 1.0f;
        g_kind = kind;
        g_windBase = g_windHeading = RandRange(0.0f, 6.2831853f);
        g_windStrength = g_to->wind;
        if (g_levelOn && kind) dslog::Write("Weather: %s on %s", g_to->label, level);
    }
    const auto* timer = *reinterpret_cast<const uint8_t* const*>(kGameTimer);
    if (!timer || *reinterpret_cast<int8_t*>(kDispatchMode) != 0) return;
    const uint32_t ms = *reinterpret_cast<const uint32_t*>(timer + 0x10);
    const float dt = (ms - g_lastMs) * 0.001f;
    g_lastMs = ms;
    // The game timer starts again at every level, so the last level's last ms could give a small "dt" here - and the
    // view cameras may still hold the last level's eye on this frame: rays from there (CastDown) hit no zone and the
    // game's ray faulted at 0x4CCF82 (Mission 1 -> front end -> Mission 1 after a short front-end stay, 2026-10-19).
    if (firstFrame) {
        firstFrame = false;
        return;
    }
    if (g_rayFrames < kRayWarmupFrames) ++g_rayFrames;
    if (!g_levelOn || dt <= 0 || dt > 0.25f) {
        if (!g_levelOn) features::SetFootprintWipe(0);
        return;
    }
    g_blend = std::min(1.0f, g_blend + dt * g_blendRate);
    const Mix m = Current();
    std::memcpy(g_tint, m.tint, sizeof g_tint);
    g_skyMix = m.skyMix;
    UpdateWind(dt, m.wind);
    Lightning(dt, m.lightning);
    DriftParticles(dt);
    features::SetFootprintWipe(m.wipe * std::max(0.3f, g_windStrength / std::max(1.0f, m.wind)));

    // scene light for the drops: the sun ambient + diffuse at noon key scaled by the hour's darkness (rough)
    if (uint8_t* atm = Atmosphere()) {
        const uint32_t fog = At<uint32_t>(atm, 0);
        g_fogColour[0] = (fog >> 16 & 0xFF) / 255.0f, g_fogColour[1] = (fog >> 8 & 0xFF) / 255.0f, g_fogColour[2] = (fog & 0xFF) / 255.0f;
        g_fogStart = At<float>(atm, 4), g_fogEnd = std::max(g_fogStart + 100.0f, At<float>(atm, 0xC));
        const float hour = At<float>(atm, 0x3C);
        const float day = std::clamp(std::sin((hour - 6.0f) / 12.0f * 3.14159265f) * 1.5f + 0.3f, 0.2f, 1.0f);
        g_brightness = day * (1.0f - m.cloud * 0.35f) + g_flash * 0.5f;
        // the preset's sky hue at the fog colour's brightness: fully fogged scenery (rock spires, distant cliffs) has
        // the fog colour, so a sky darker than it (an own day curve did that at dusk / night) showed them as pale cut-outs
        const float skyLum = std::max(Lum(m.sky), 0.01f);
        const float sky = Lum(g_fogColour) / skyLum;
        for (int i = 0; i < 3; ++i) g_skyColour[i] = std::min(1.0f, m.sky[i] * sky + g_flash * 0.5f);
    }

    // precipitation: the outgoing kind in field 0, the incoming in field 1
    InitField(g_fields[0], m.precipFrom > 0 ? m.kindFrom : Precip::None);
    InitField(g_fields[1], m.precipTo > 0 ? m.kindTo : Precip::None);
    const bool precip = g_fields[0].kind != Precip::None || g_fields[1].kind != Precip::None;
    // every view's camera (puddles are kept round all of them); tops filled round them, the grid round view 0's
    g_eyeCount = g_viewCamCount = 0;
    g_indoor0 = false;
    if (const auto* cams = *reinterpret_cast<const uint8_t* const*>(kCameras)) {
        const auto* r = *reinterpret_cast<const uint8_t* const*>(kRenderer);
        const int views = r ? std::clamp(*reinterpret_cast<const int*>(r + 0x20), 1, 4) : 1;
        for (int v = 0; v < views; ++v) {
            const uint32_t id = *reinterpret_cast<const uint32_t*>(cams + 0xEC0C + v * 4);
            if (id >= 128) continue;
            const uint8_t* cam = cams + 8 + id * 0x1D8;
            ViewCam& vc = g_viewCams[g_viewCamCount++];
            std::memcpy(vc.eye, cam + 0x68, sizeof vc.eye);
            vc.indoor = ZoneIsInterior(*reinterpret_cast<const uint8_t* const*>(cam + 0x4C));
            std::memcpy(g_eyes[g_eyeCount++], vc.eye, sizeof g_eyes[0]);
        }
        if (g_eyeCount) std::memcpy(g_cam0, g_eyes[0], sizeof g_cam0), g_haveCam = true, g_indoor0 = g_viewCams[0].indoor;
    }
    if ((precip || !g_spots.empty()) && g_haveCam) {
        // not round a camera indoors: its rays only see the interior, and cells keep what they first found
        for (int i = 0; i < g_eyeCount; ++i)
            if (!g_viewCams[i].indoor) FillTops(g_eyes[i][0], g_eyes[i][1], g_eyes[i][2], 9, i ? 30 : 60);
        if (g_tops.size() > 60000) g_tops.clear();
        BuildGrid(g_cam0);
    }
    // rain now: wetness and puddles follow it
    const float rainNow = (m.kindFrom == Precip::Rain ? m.precipFrom : 0.0f) + (m.kindTo == Precip::Rain ? m.precipTo : 0.0f);
    g_wet = rainNow > 0.05f ? std::min(1.0f, g_wet + dt * rainNow / 45.0f) : std::max(0.0f, g_wet - dt / 120.0f);
    features::SetPuddleRain(rainNow);
    g_rainNow = rainNow;
    PlacePuddles(dt, rainNow);
    StepField(g_fields[0], dt, m.precipFrom);
    StepField(g_fields[1], dt, m.precipTo);
#ifndef DS_DIST
    static uint32_t lastLog = 0, frames = 0;
    ++frames;
    if (GetTickCount() - lastLog > 5000) {
        dslog::Write("[dev] Weather: %.0f fps, per frame: update %.2f ms, draw %.2f ms, sky %.2f ms", frames * 1000.0 /
                     std::max<uint32_t>(1, GetTickCount() - lastLog), g_msUpdate / std::max(1u, frames),
                     g_msDraw / std::max(1u, frames), g_msSky / std::max(1u, frames));
        g_msUpdate = g_msDraw = g_msSky = 0;
        frames = 0;
        lastLog = GetTickCount();
        bool known = false;
        const float top = g_haveCam ? TopAt(g_cam0[0], g_cam0[2], known) : 0;
        const float* sky = g_haveOrig ? g_orig + (5 * 4 + 2) * 4 : nullptr;
        const float* skyNow = g_haveOrig ? g_written + (5 * 4 + 2) * 4 : nullptr;
        dslog::Write("[dev] Weather: camera %s (interiors %u of %d root zones), cover %.1f", g_indoor0 ? "INDOORS" : "outdoors",
                     static_cast<unsigned>(g_interiors.size()), g_sceneZones, features::CameraCover());
        dslog::Write("[dev] Weather: %s blend %.2f wind %.1f m/s, fields %d/%d, cam (%.0f %.0f %.0f) top %s%.0f, rays %d "
                     "hits %d cells %u, puddles %d wet %.2f, drawn %d in %d passes, sky noon %.2f %.2f %.2f -> %.2f %.2f %.2f",
                     g_to->label, g_blend, g_windStrength, g_fields[0].active, g_fields[1].active, g_cam0[0], g_cam0[1],
                     g_cam0[2], known ? "" : "? ", top, g_rays, g_rayHits, static_cast<unsigned>(g_tops.size()), SpotsShown(), g_wet, g_drawn,
                     g_drawCalls, sky ? sky[0] : 0, sky ? sky[1] : 0, sky ? sky[2] : 0, skyNow ? skyNow[0] : 0,
                     skyNow ? skyNow[1] : 0, skyNow ? skyNow[2] : 0);
        g_drawn = g_drawCalls = g_rays = g_rayHits = 0;
    }
#endif
    for (Splash& s : g_splashes) s.age += dt;
    g_splashes.erase(std::remove_if(g_splashes.begin(), g_splashes.end(), [](const Splash& s) { return s.age > 0.18f; }),
                     g_splashes.end());
}

void features::ApplyWeather() {
    const bool ok = patch::HookCall(kAtmosUpdateCall, reinterpret_cast<const void*>(&AtmosUpdate), kAtmosUpdate) &&
                    patch::HookCall(kSwayCall, reinterpret_cast<const void*>(&SwayUpdate), kSwayUpdate) &&
                    patch::SubscribeCall(kTransparentsCall, reinterpret_cast<const void*>(&Transparents), kTransparents, 100,
                                         &g_transparentsNext) &&
                    patch::HookCall(kSkyCall, reinterpret_cast<const void*>(&Sky), kSky);
    // the scene read is shared: loadout.cpp's hook (HLOI kit rewrites) calls the game's reader itself, so this one runs
    // in front of it and only looks at what comes back (ENVI lines aren't changed)
    if (!patch::SubscribeCall(kSceneRead, reinterpret_cast<const void*>(&ReadScene), kReadFile, 100, &g_readNext))
        dslog::Write("[warn] Weather: scene read not shared - rain and snow can't tell indoors from outdoors");
    dslog::Write(ok ? "[ok]   Weather: atmosphere / wind / precipitation hooks" : "[fail] Weather: unexpected code - no weather");
}
