// Online co-op: the host's game settings for everyone. Every PC runs the whole mission (lockstep.cpp), so a setting
// that changes what happens in the world must be the same on every PC - a user's NIGHT time of day (AI sight x0.75)
// and squad voice accents (the voice-line picks draw from the random tables) desynced a 2-PC test at tick 360.
// The host puts its values into START (online.cpp) as one token; joiners use them - in memory only, never saved - for
// as long as the online mission runs (features::LockstepActive). The host uses the same parsed values, so both sides
// run the same code. Per-PC display / input settings (anti-aliasing, deadzone, frame cap, HUD, rumble, sounds while
// aiming) don't change the simulation and stay each player's own. Mods can't be sent: their names are compared and a
// difference is reported (log + the mod report).
//
// Token (no spaces - START is split by spaces):  H1.t<time>.f<flow>.T<0|1>.v<0|1>.c<voice role 0..3>.F<0|1>.C<hex>.M<hex>
//   t / f  TIME OF DAY / TIME FLOW choices        T  time of day rules the mission (featTimeOfDay && featLoadout)
//   v / c  squad voice accents on / each role's VOICE choice (co-op)    F  playable followers
//   C      cheats: 1 invulnerable, 2 infinite ammo, 4 enemy AI off, 8 enemy level 0, 0x10 infinite health,
//          trooper level << 8      M  hash of the enabled mods' folder names
//   J      Japanese-release fixes on (jpfixes.cpp; stuck tanks push free) - later field, older builds don't send it
//   S      the missions' co-op branches (coopscripts.cpp, slot 0x8A8) - later field
//   B      briefing + loadout screen on (customise.cpp / overview.cpp) - later field; a PC with the screen while the
//          other hasn't waits for that PC's "done" for ever (2-home session 2026-10-22)
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "core/settings.h"
#include "features/features.h"
#include "weather/dsfix_weather.h"

namespace {
features::HostSettings g_host;
bool g_parsed = false;

uint32_t ModsHash() {
    std::vector<std::string> names;
    for (std::string n : features::LoadedMods()) {
        std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        names.push_back(n);
    }
    std::sort(names.begin(), names.end());
    uint32_t h = 2166136261u;   // FNV-1a over "name\n" ...
    for (const std::string& n : names)
        for (char c : n + "\n") h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
    return names.empty() ? 0 : h;
}
}  // namespace

std::string features::HostSyncToken() {
    // the same token for a few seconds: lockstep's mission start (Arm) and online.cpp's START read it in the same frame
    // in either order - a host that applied its own settings after Arm ran with input delay 3 while its START told
    // the joiner 7, and both waited for an input the other never sent (2-PC test 2026-10-12)
    static std::string cached;
    static DWORD cachedAt = 0;
    if (!cached.empty() && GetTickCount() - cachedAt < 5000) return cached;
    const settings::Values& v = settings::Get();
    char out[192];
    const uint8_t* renderer = *reinterpret_cast<uint8_t* const*>(0x63C924);
    // input delay: half the worst round trip + 30 ms margin, in 1/60 s ticks (3..30) - shorter stalls the game on
    // every tick (2-PC test over the internet, 300 ms ping, delay 3: 30 fps and stutter)
    int ping = 0;
    for (const auto& p : net::Snap().peers) ping = std::max(ping, p.ping);
    const int delay = std::clamp((ping / 2 + 30 + 16) / 17, 3, 30);
    snprintf(out, sizeof out, "H1.t%d.f%d.T%d.v%d.c%d%d%d%d.F%d.C%X.M%X.D%d.L%d.P%d.J%d.W%d.S%d.B%d", TimeOfDay(), TimeFlow(),
             v.featTimeOfDay && v.featLoadout && TimeOfDayInstalled() ? 1 : 0, v.featVoices && VoicesInstalled() ? 1 : 0, CoopVoiceChoice(0), CoopVoiceChoice(1),
             CoopVoiceChoice(2), CoopVoiceChoice(3), PluginBool("dsfix.followers", "FeatFollowers", false) ? 1 : 0, CheatFlags(), ModsHash(),
             renderer ? renderer[0x40A9C] : 1, delay, ping,
             PluginBool("dsfix.jpfixes", "FeatJpFixes", false) ? 1 : 0,   // its own plugin's setting (no plugin: off)
             v.featWeather && v.featLoadout && WeatherPlugin() ? WeatherPlugin()->ForHost() : 0, v.featCoopScripts ? 1 : 0, v.featLoadout ? 1 : 0);
    cached = out;
    cachedAt = GetTickCount();
    return out;
}

bool features::ApplyHostSyncToken(const char* token, bool own) {
    HostSettings h;
    int t = 0, f = 0, T = 0, v = 0, F = 0;
    char c[8] = {};
    unsigned cheats = 0, mods = 0;
    int D = -1, L = -1, P = -1, J = -1, W = -1, Sc = -1, B = -1;   // (older builds don't send them)
    if (!token || sscanf_s(token, "H1.t%d.f%d.T%d.v%d.c%4[0-9].F%d.C%X.M%X.D%d.L%d.P%d.J%d.W%d.S%d.B%d", &t, &f, &T, &v, c, 8, &F,
                           &cheats, &mods, &D, &L, &P, &J, &W, &Sc, &B) < 8 ||
        strlen(c) != 4 || t < 0 || t >= kTimeOfDayCount || f < 0 || f >= kTimeFlowCount) {
        dslog::Write("[fail] Online: the host's settings (\"%s\") can't be read - this PC keeps its own (desync likely)",
                     token ? token : "");
        g_parsed = false;
        return false;
    }
    h.timeOfDay = t;
    h.timeFlow = f;
    h.timeOfDayOn = T != 0;
    h.voices = v != 0;
    for (int i = 0; i < 4; ++i) h.voice[i] = std::min(c[i] - '0', kVoiceCount - 1);
    h.followers = F != 0;
    h.cheats = cheats;
    h.modsHash = mods;
    h.drawFar = D;
    h.inputDelay = L;
    h.pingMs = P;
    h.jpFixes = J;
    h.weather = W >= 0 && W < kWeatherCount ? W : -1;
    h.coopScripts = Sc;
    h.loadout = B;
    g_host = h;
    g_parsed = true;
    SetCheatFlags(cheats);
    dslog::Write("Online: %s settings for this mission - time of day %s / %s%s, voice accents %s, followers %s, cheats %X, "
                 "draw distance %s, loadout screen %s", own ? "own (host)" : "the host's", TimeOfDayLabel(t), TimeFlowLabel(f),
                 h.timeOfDayOn ? "" : " (off)", h.voices ? "on" : "off", h.followers ? "on" : "off", cheats,
                 D < 0 ? "not sent" : D ? "far" : "near", B < 0 ? "not sent" : B ? "on" : "off");
    if (!own && mods != ModsHash()) {
        dslog::Write("[fail] Online: the host has other mods enabled than this PC - the mission will likely desync");
        ModLog("[fail] Online: the host's enabled mods differ from this PC's (Mods menu) - use the same mods");
    }
    return true;
}

// The loadout screen for the mission starting now: the host's choice while an online session runs (its START token
// arrives before the level loads, lockstep arms later), else this PC's setting.
bool features::LoadoutForMission() {
    if (g_parsed && g_host.loadout >= 0 && net::Snap().role != net::Snapshot::Role::None) return g_host.loadout != 0;
    return settings::Get().featLoadout;
}

const features::HostSettings* features::HostSync() { return g_parsed && LockstepActive() ? &g_host : nullptr; }

uint32_t features::OwnModsHash() { return ModsHash(); }
