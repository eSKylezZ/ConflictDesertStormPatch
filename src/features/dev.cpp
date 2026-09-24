// Development-only hooks (compiled out of DS_DIST builds), driven by HKCU\Software\DesertStormFix\Dev.
//
// StartLevel (REG_SZ, e.g. "mission1.dll"): boot straight into that level instead of the front-end. The game's
// start-up options come from a built-in "command line" string (pointer at 0x5e7050 -> "-f -d"), parsed by
// FUN_0040fdc0: -f = front-end, a plain word = level to load (copied into the buffer at 0x5e6f28), otherwise the
// game starts in state 0xe (mission). The pointer is aimed at our own "-d <level>" string.
#include <windows.h>

#include <cstdio>
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

void features::ApplyDevHooks() {
#ifndef DS_DIST
    constexpr uint32_t kOptionsPtr = 0x5E7050, kOptionsDefault = 0x5E708C;  // -> "-f -d"
    static char options[64];
    char level[32] = {};
    DWORD size = sizeof level;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "StartLevel", RRF_RT_REG_SZ, nullptr, level,
                     &size) != ERROR_SUCCESS || !level[0] || strlen(level) > 20)
        return;
    const uint32_t expected = kOptionsDefault;
    if (!patch::Matches(kOptionsPtr, &expected, 4)) return;
    snprintf(options, sizeof options, "-d %s", level);
    patch::WriteValue(kOptionsPtr, reinterpret_cast<uint32_t>(options));
    dslog::Write("[dev]  Start level: %s", level);
#endif
}
