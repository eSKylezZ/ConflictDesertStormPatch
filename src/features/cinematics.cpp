// Skip the start-up intro (logos + intro film) and / or the mission cutscenes.
//
// Logos: the first front-end load (level init FUN_0040c090) shows a slideshow (Take2 / SCi, Pivotal, legal
// notice; SlideShow.cpp object 0x606954, ~17 s, driven by the load-progress callback 0x40AB50 and waited for at
// the end of the load) while the first-boot flag 0x5E6C80 is 1; the flag is cleared afterwards. Clearing it
// before that load shows the normal loading screen instead.
// Intro film: FrontEnd.dll (environment intro.env, in-engine, ~40 s) asks once at start-up through script API
// slot +0x8BC (0x48A4D0: returns and clears the one-shot flag 0x5ED230, initially 1) whether to open with the
// film; with 0 it goes straight to the main menu. The same film still runs as the menu's idle attract loop
// (ended by any input) - that is left alone.
//
// Cutscenes are run by the mission scripts (MissionN.dll) through the script function table at 0x5ED700..:
// MovieStart 0x486A00 sets the cinematic bit ([0x63C948]+0x1B8C bit 0), MovieEnd 0x486F10 clears it, and the
// script polls FUN_00486710 (cancel: fades out and returns 1 once MOVIE_CANCEL - Esc / pad button 0 / left mouse
// - is pressed by any player) or FUN_004867B0 (the same query without side effects). Both also cancel when the
// developer start-up flag 'u' is set (FUN_004B9CD0(0x75) at 0x48676E / 0x4867ED); those two reads go through
// MovieSkipFlag, which also answers yes while a mission cutscene runs and the setting is on.
#include <windows.h>

#include <cstring>
#include <initializer_list>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kFirstBoot = 0x5E6C80;                // 1 = logo slideshow on the first front-end load
constexpr uint32_t kPlayIntroFilm = 0x5ED230;            // 1 = front-end opens with the intro film
constexpr uint32_t kOptionFlag = 0x4B9CD0;               // int (char option): start-up option set?
constexpr uint32_t kFlagReads[] = {0x48676E, 0x4867ED};  // 'u' reads in the two movie-cancel queries
constexpr uint32_t kGameState = 0x63C948;                // +0x1B8C bit 0 = cutscene running
constexpr uint32_t kCurrentLevel = 0x606880;             // environment file of the loaded level

using OptionFlagFn = int(__cdecl*)(int);

bool InMissionCutscene() {
    auto state = *reinterpret_cast<const uint8_t**>(kGameState);
    if (!state || !(state[0x1B8C] & 1)) return false;
    const auto level = reinterpret_cast<const char*>(kCurrentLevel);
    return _strnicmp(level, "frontend", 8) != 0 && _strnicmp(level, "intro", 5) != 0;
}

int __cdecl MovieSkipFlag(int option) {
    if (reinterpret_cast<OptionFlagFn>(kOptionFlag)(option)) return 1;
    return settings::Get().skipCutscenes && InMissionCutscene() ? 1 : 0;
}
}  // namespace

void features::ApplyCinematics() {
    static bool hooked = false;
    if (!hooked) {
        hooked = true;
        for (uint32_t site : kFlagReads) hooked = patch::HookCall(site, &MovieSkipFlag, kOptionFlag) && hooked;
        if (hooked) dslog::Write("[ok]   Cutscene skip: movie-cancel hooks");
    }
    // Both are only read by the first front-end load, which comes after the launcher; 0 / 1 are the valid values.
    const uint32_t value = settings::Get().skipIntro ? 0 : 1;
    for (uint32_t flag : {kFirstBoot, kPlayIntroFilm}) {
        const uint32_t current = *reinterpret_cast<volatile uint32_t*>(flag);
        if (current <= 1 && current != value) patch::WriteValue(flag, value);
    }
}
