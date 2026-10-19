// Skip the start-up logo films (setting SkipIntro). Films play from a playlist (table 0x60C454, 0x20-byte entries
// {name, type, ...}: "Logo" type 6 = Logo1..5PAL.bik numbered by the player object 0x631608, "blank" type 0 = front-end,
// "AMODE" type 1 = attract film, then the mission films). FUN_0042ccd0 builds "data\FMV\<name>PAL[UK|US].bik" and plays
// it with FUN_0042cb80 (call at 0x42CE0F). The per-frame film update FUN_0042cbf0 asks the cancel query FUN_004426b0
// (call at 0x42CC51; any player's MOVIE_CANCEL - what a click does) for films of type 4 / 5 / 6 and then moves the
// playlist on. That query answers yes while a logo film plays. (Refusing to open the film instead stalled the playlist:
// nothing then reports the film as finished.)
#include <cstring>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kPlayFilm = 0x42CB80, kPlayFilmCall = 0x42CE0F;
constexpr uint32_t kCancelQuery = 0x4426B0, kCancelQueryCall = 0x42CC51;

bool g_logoPlaying = false;
int g_logoFrames = 0;  // cancelled a few frames in, like a click (the film is set up by then)

int __stdcall PlayFilm(const char* path) {  // FUN_0042cb80 is stdcall (ret 4)
    const char* name = path ? std::strrchr(path, '\\') : nullptr;
    name = name ? name + 1 : path;
    g_logoPlaying = name && settings::Get().skipIntro && !_strnicmp(name, "Logo", 4);
    g_logoFrames = 0;
    if (g_logoPlaying) dslog::Write("Intro: %s skipped", name);
    return reinterpret_cast<int(__stdcall*)(const char*)>(kPlayFilm)(path);
}

int __cdecl CancelQuery() {
    if (g_logoPlaying && ++g_logoFrames >= 5) {
        g_logoPlaying = false;
        return 1;
    }
    return reinterpret_cast<int(__cdecl*)()>(kCancelQuery)();
}
}  // namespace

void features::ApplyCinematics() {
    if (patch::HookCall(kPlayFilmCall, reinterpret_cast<const void*>(&PlayFilm), kPlayFilm) &&
        patch::HookCall(kCancelQueryCall, reinterpret_cast<const void*>(&CancelQuery), kCancelQuery))
        dslog::Write("[ok]   Intro: logo films %s", settings::Get().skipIntro ? "skipped" : "kept");
    else
        dslog::Write("[fail] Intro: film player calls not recognised");
}
