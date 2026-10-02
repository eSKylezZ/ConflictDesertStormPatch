#include "core/settings.h"

#include <windows.h>

#include <algorithm>

#include "core/log.h"

namespace {
constexpr char kKey[] = "SOFTWARE\\Pivotal Games\\Conflict Desert Storm\\Enhancements";
settings::Values g_values;

void Read(HKEY key, const char* name, uint32_t& value, uint32_t lo, uint32_t hi) {
    DWORD v, size = sizeof v, type;
    if (RegQueryValueExA(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) == ERROR_SUCCESS &&
        type == REG_DWORD)
        value = std::clamp<uint32_t>(v, lo, hi);
}

void Read(HKEY key, const char* name, bool& value) {
    uint32_t v = value;
    Read(key, name, v, 0, 1);
    value = v != 0;
}

bool Put(HKEY key, const char* name, uint32_t value) {
    DWORD v = value;
    return RegSetValueExA(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof v) == ERROR_SUCCESS;
}
}  // namespace

const settings::Values& settings::Get() { return g_values; }

void settings::Load() {
    HKEY key;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, KEY_READ, &key) == ERROR_SUCCESS) {
        Read(key, "HudScaling", g_values.hudScaling);
        Read(key, "HudScalePercent", g_values.hudScalePercent, 25, 400);
        Read(key, "Controller", g_values.controller);
        Read(key, "PadDeadzone", g_values.padDeadzone, 0, 90);
        Read(key, "FpsCap", g_values.fpsCap, 0, 1000);
        if (g_values.fpsCap == 0 || g_values.fpsCap > 240) g_values.fpsCap = 240;  // 240 is the most (older "unlimited")
        Read(key, "FpsCapToRefresh", g_values.fpsCapToRefresh);
        Read(key, "SplitScreenLayout", g_values.splitScreenLayout, 0, 1);
        Read(key, "DiscordPresence", g_values.discordPresence);
        Read(key, "DisplayMode", g_values.displayMode, 0, 2);
        Read(key, "SkipIntro", g_values.skipIntro);
        Read(key, "SkipCutscenes", g_values.skipCutscenes);
        Read(key, "Antialiasing", g_values.antialiasing, 0, 16);
        Read(key, "Anisotropy", g_values.anisotropy, 0, 16);
        Read(key, "MouseAcceleration", g_values.mouseAcceleration);
        Read(key, "VSync", g_values.vsync);
        Read(key, "PerfOverlay", g_values.perfOverlay);
        RegCloseKey(key);
    }
    const Values& v = g_values;
    dslog::Write("Settings: HudScaling=%d HudScalePercent=%u Controller=%d PadDeadzone=%u FpsCap=%u FpsCapToRefresh=%d "
                "SplitScreenLayout=%u DiscordPresence=%d DisplayMode=%u SkipIntro=%d SkipCutscenes=%d "
                "Antialiasing=%u Anisotropy=%u",
                v.hudScaling, v.hudScalePercent, v.controller, v.padDeadzone, v.fpsCap, v.fpsCapToRefresh,
                v.splitScreenLayout, v.discordPresence, v.displayMode, v.skipIntro, v.skipCutscenes,
                v.antialiasing, v.anisotropy);
}

bool settings::Save(const Values& v) {
    HKEY key;
    if (RegCreateKeyExA(HKEY_LOCAL_MACHINE, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        dslog::Write("Settings: cannot open the registry key for writing");
        return false;
    }
    bool ok = Put(key, "HudScaling", v.hudScaling) && Put(key, "HudScalePercent", v.hudScalePercent) &&
              Put(key, "Controller", v.controller) && Put(key, "PadDeadzone", v.padDeadzone) &&
              Put(key, "FpsCap", v.fpsCap) && Put(key, "FpsCapToRefresh", v.fpsCapToRefresh) &&
              Put(key, "SplitScreenLayout", v.splitScreenLayout) && Put(key, "DiscordPresence", v.discordPresence) &&
              Put(key, "DisplayMode", v.displayMode) && Put(key, "SkipIntro", v.skipIntro) &&
              Put(key, "SkipCutscenes", v.skipCutscenes) && Put(key, "Antialiasing", v.antialiasing) &&
              Put(key, "Anisotropy", v.anisotropy) && Put(key, "MouseAcceleration", v.mouseAcceleration) &&
              Put(key, "VSync", v.vsync) && Put(key, "PerfOverlay", v.perfOverlay);
    RegCloseKey(key);
    if (ok) g_values = v;
    return ok;
}
