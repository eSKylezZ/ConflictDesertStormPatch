// Time of day: play any mission at dawn, by day, at dusk or at night, or with a running day / night cycle (TIME OF DAY
// and TIME FLOW rows on the Squad Loadout screen, remembered in the registry).
//
// The engine already has a full 24-hour atmosphere: every level's .env holds 4 colour keys (night, dawn 6 h, noon 12 h,
// dusk 18 h) for sun ambient / diffuse, fog, flare and sky, and the atmosphere object [[0x63C948]+0x1A58] blends them
// every frame from its hour (+0x3C), which its update FUN_004efc60 advances by dt * speed (+0x40, game seconds per
// real second) / 3600 until a "run until" hour (+0x44 flag, +0x48 hour). The sun / moon direction, stencil shadows
// (off at night), the sun flare and the AI's sight range (25 % less at night, FUN_004cc190) all follow it.
// The mission scripts set the hour in LevelSetup and some run the clock (Mission 1 dawn -> 11 h, Mission 3 day ->
// 23 h ...) through the script API table 0x5ED238: +0x78 SetTOD 0x485950, +0x7C GetTOD 0x485970, +0x80 SetSpeed
// 0x485980, +0x90 RunUntil 0x4859F0 (cdecl, float argument; GetTOD returns on the x87 stack).
// Those four entries are replaced: while our setting rules a level, the scripts' calls move a "script clock" of their
// own (so a script that waits for its hour still sees it) and the real atmosphere keeps our time.
#include <windows.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kEnvironment = 0x63C948, kAtmosphere = 0x1A58;
constexpr uint32_t kHour = 0x3C, kSpeed = 0x40, kRunFlag = 0x44, kRunUntil = 0x48;
constexpr uint32_t kApiSetTod = 0x5ED2B0, kApiGetTod = 0x5ED2B4, kApiSetSpeed = 0x5ED2B8, kApiRunUntil = 0x5ED2C8;
constexpr uint32_t kOrigSetTod = 0x485950, kOrigGetTod = 0x485970, kOrigSetSpeed = 0x485980, kOrigRunUntil = 0x4859F0;
constexpr uint32_t kLevelName = 0x606880, kDispatchMode = 0x60EC8C, kFrontEndState = 0x617C18;
constexpr uint32_t kTimer = 0x6406B8, kDtGetter = 0x4BA480;  // the atmosphere update's dt (seconds, game time)
constexpr char kRegKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";

// TIME OF DAY: 0 = the mission's own, then fixed start hours.
const float kStartHours[features::kTimeOfDayCount] = {-1.0f, 6.5f, 12.0f, 17.6f, 23.0f};
const char* const kTimeLabels[features::kTimeOfDayCount] = {"MISSION DEFAULT", "DAWN", "DAY", "DUSK", "NIGHT"};
// TIME FLOW: 0 = as the mission runs it (or stopped at a chosen hour), then one game hour per 10 / 5 / 2 / 1 real minutes.
const float kFlowSpeeds[features::kTimeFlowCount] = {0.0f, 6.0f, 12.0f, 30.0f, 60.0f};
const char* const kFlowLabels[features::kTimeFlowCount] = {"MISSION DEFAULT", "1 HOUR PER 10 MIN", "1 HOUR PER 5 MIN",
                                                           "1 HOUR PER 2 MIN", "1 HOUR PER MINUTE"};

int g_time = 0, g_flow = 0;  // the choices (registry TimeOfDay / TimeFlow)
bool g_loaded = false;

bool g_ruling = false;   // our setting rules the current level's atmosphere
bool g_applied = false;  // this level has had its first frame of plain play
// Linked levels (features::LinkedFrom): a chosen time / cycle continues from the hour the previous level ended at.
float g_lastHour = -1.0f;     // the ruled hour, each frame of plain play
bool g_lastRuled = false;
float g_prevHour = -1.0f;     // ... of the previous level
bool g_prevRuled = false;
bool g_continued = false;     // this level took the previous one's hour
char g_level[32] = {};
// the scripts' own clock while we rule
float g_scriptHour = 12.0f, g_scriptSpeed = 0.0f, g_scriptUntil = 0.0f;
bool g_scriptRun = false;

uint8_t* Atmosphere() {
    auto* env = *reinterpret_cast<uint8_t**>(kEnvironment);
    return env ? *reinterpret_cast<uint8_t**>(env + kAtmosphere) : nullptr;
}
template <class T>
T& At(uint8_t* p, uint32_t off) { return *reinterpret_cast<T*>(p + off); }

void Load() {
    if (g_loaded) return;
    g_loaded = true;
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kRegKey, "TimeOfDay", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS &&
        v < features::kTimeOfDayCount)
        g_time = static_cast<int>(v);
    size = sizeof v;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, kRegKey, "TimeFlow", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS &&
        v < features::kTimeFlowCount)
        g_flow = static_cast<int>(v);
}

void Save() {
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kRegKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    const DWORD t = g_time, f = g_flow;
    RegSetValueExA(key, "TimeOfDay", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&t), sizeof t);
    RegSetValueExA(key, "TimeFlow", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&f), sizeof f);
    RegCloseKey(key);
}

bool Cutscene() {
    auto* env = *reinterpret_cast<uint8_t**>(kEnvironment);
    return env && (*reinterpret_cast<uint32_t*>(env + 0x1B8C) & 1);
}

bool MissionLevel() { return _strnicmp(reinterpret_cast<const char*>(kLevelName), "Mission", 7) == 0; }

// Takes over the atmosphere with the current choices (keepHour: a running cycle from the mission's hour stays where it is).
void Rule() {
    uint8_t* a = Atmosphere();
    if (!a) return;
    if (!g_ruling) {   // the script clock starts where the mission had it
        g_scriptHour = At<float>(a, kHour);
        g_scriptSpeed = At<float>(a, kSpeed);
        g_scriptRun = At<int>(a, kRunFlag) != 0;
        g_scriptUntil = At<float>(a, kRunUntil);
    }
    if (g_time == 0 && g_flow == 0) {   // the mission's own time again: hand the script clock back
        if (g_ruling) {
            At<float>(a, kHour) = g_scriptHour;
            At<float>(a, kSpeed) = g_scriptSpeed;
            At<int>(a, kRunFlag) = g_scriptRun;
            At<float>(a, kRunUntil) = g_scriptUntil;
        }
        g_ruling = false;
        return;
    }
    g_ruling = true;
    At<float>(a, kHour) = g_time ? kStartHours[g_time] : g_scriptHour;
    if (g_continued && g_prevHour >= 0) At<float>(a, kHour) = g_prevHour;
    At<float>(a, kSpeed) = kFlowSpeeds[g_flow];
    At<int>(a, kRunFlag) = 0;
    dslog::Write("Time of day: %s, %s (hour %.2f)", kTimeLabels[g_time], kFlowLabels[g_flow], At<float>(a, kHour));
}

// ---- script API replacements (cdecl, float argument) ----
void __cdecl ScriptSetTod(float hour) {
    if (!g_ruling) return reinterpret_cast<void(__cdecl*)(float)>(kOrigSetTod)(hour);
    g_scriptHour = hour;
}
float __cdecl ScriptGetTod() {
    if (!g_ruling) return reinterpret_cast<float(__cdecl*)()>(kOrigGetTod)();
    return g_scriptHour;
}
void __cdecl ScriptSetSpeed(float speed) {
    if (!g_ruling) return reinterpret_cast<void(__cdecl*)(float)>(kOrigSetSpeed)(speed);
    g_scriptSpeed = speed;
}
void __cdecl ScriptRunUntil(float hour) {
    if (!g_ruling) return reinterpret_cast<void(__cdecl*)(float)>(kOrigRunUntil)(hour);
    g_scriptRun = true;
    g_scriptUntil = hour;
}

// The script clock runs like the engine's (FUN_004efc60): hour += dt * speed / 3600, wrapped, stopping at "run until".
void TickScriptClock() {
    auto* timer = *reinterpret_cast<void**>(kTimer);
    if (!timer || g_scriptSpeed == 0.0f) return;
    const float dt = reinterpret_cast<float(__thiscall*)(void*)>(kDtGetter)(timer);
    const float before = g_scriptHour;
    g_scriptHour += dt * g_scriptSpeed / 3600.0f;
    if (g_scriptHour >= 24.0f || g_scriptHour < 0.0f) g_scriptHour = 0.0f;
    if (g_scriptRun && before <= g_scriptUntil && g_scriptHour >= g_scriptUntil) {
        g_scriptSpeed = 0.0f;
        g_scriptRun = false;
    }
}

void Replace(uint32_t slot, uint32_t original, const void* fn) {
    if (*reinterpret_cast<uint32_t*>(slot) != original) {
        dslog::Write("[fail] Time of day: script API slot 0x%08X not recognised", slot);
        return;
    }
    const uint32_t v = reinterpret_cast<uint32_t>(fn);
    patch::Write(slot, &v, 4);
}
}  // namespace

const char* features::TimeOfDayLabel(int i) { return kTimeLabels[i]; }
const char* features::TimeFlowLabel(int i) { return kFlowLabels[i]; }
int features::TimeOfDay() { Load(); return g_time; }
bool features::TimeContinues() { return g_continued; }
int features::TimeFlow() { Load(); return g_flow; }

// From the loadout screen: a new choice, applied at once (the preview soldier is lit by it) and remembered.
void features::SetTimeOfDay(int time, int flow) {
    Load();
    if (!features::LinkedFrom().empty()) return;   // a continuing level keeps the running clock
    g_time = time;
    g_flow = flow;
    Save();
    if (MissionLevel()) Rule();
}

// The briefing screen opened: rule (again) now - the mission script's start-up set-up runs after the overview and
// sets its hour without the SetTOD entry.
void features::RuleTimeOfDay() {
    Load();
    if (!MissionLevel()) return;
    if (!g_applied) g_applied = true, g_continued = !features::LinkedFrom().empty() && g_prevRuled;
    Rule();
}

void features::ApplyTimeOfDay() {
    Replace(kApiSetTod, kOrigSetTod, reinterpret_cast<const void*>(&ScriptSetTod));
    Replace(kApiGetTod, kOrigGetTod, reinterpret_cast<const void*>(&ScriptGetTod));
    Replace(kApiSetSpeed, kOrigSetSpeed, reinterpret_cast<const void*>(&ScriptSetSpeed));
    Replace(kApiRunUntil, kOrigRunUntil, reinterpret_cast<const void*>(&ScriptRunUntil));
}

// A mission is ruled from its first moment of plain play (the loadout screen opens then too; loaded saves and linked
// missions, which have no screen, get the remembered choice the same way). Before that - intro, briefing - the
// mission's own time is shown.
void features::OnFrameTimeOfDay() {
    Load();
    const char* level = reinterpret_cast<const char*>(kLevelName);
    if (strncmp(level, g_level, sizeof g_level - 1) != 0) {   // new level: the scripts' calls go through again
        strncpy_s(g_level, level, sizeof g_level - 1);
        g_prevHour = g_lastHour, g_prevRuled = g_lastRuled;
        g_lastHour = -1.0f, g_lastRuled = false;
        g_continued = false;
        g_ruling = false;
        g_scriptRun = false;
        g_scriptSpeed = 0.0f;
        g_applied = false;
    }
    if (*reinterpret_cast<int8_t*>(kDispatchMode) != 0) return;
    if (g_ruling) TickScriptClock();
    if (g_ruling && MissionLevel() && *reinterpret_cast<uint32_t*>(kFrontEndState) == 0xE)
        if (uint8_t* a = Atmosphere()) g_lastHour = At<float>(a, kHour), g_lastRuled = true;
    // The engine's blend has a gap at exactly 6:00 and 18:00 (its sunrise / sunset fades meet there and neither
    // applies): the soldiers' bump lighting turns a glaring yellow for that instant. Never rest on those hours.
    if (uint8_t* a = Atmosphere(); a && MissionLevel()) {
        float& hour = At<float>(a, kHour);
        if (fabsf(hour - 6.0f) < 0.002f || fabsf(hour - 18.0f) < 0.002f) hour += 0.004f;
    }
    if (!g_applied && MissionLevel() && *reinterpret_cast<uint32_t*>(kFrontEndState) == 0xE && !Cutscene()) {
        g_applied = true;
        g_continued = !features::LinkedFrom().empty() && g_prevRuled;
        Rule();
    }
}
