// See ds1_saves.h. Moved from DesertStormFix's src/core/pse_saves.cpp (2026-10-19): the game layer owns the co-save, so a
// plugin's chunks are saved / loaded with or without DesertStormFix installed. Only the reader of DesertStormFix's old
// ".sav.dsfix" sidecar stays in dsfix (it knows that format) and comes in through SetLegacy.
#include "ds1_saves.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "loader.h"

namespace ds1_saves {
namespace {   // (inside ds1_saves: pse_tests compiles this file into one unit with ds1_events.cpp)
PSEPluginHandle g_self = 0;
PSEDs1LegacyCosaveFn g_legacy = nullptr;
bool g_loadPending = false;    // a save was loaded: the next level init restores it (no NEW_GAME)
bool g_fromFrontEnd = true;    // the last level was the front end (or none yet)

void Log(PSELogLevel level, const char* fmt, ...) {
    char line[768];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    pse::LogLine(g_self, level, line);
}

void Send(uint32_t type, const char* path) {
    const char* p = path ? path : "";
    pse::Broadcast(type, p, static_cast<uint32_t>(std::strlen(p) + 1));
}

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool EndsWithSav(const std::string& p) { return p.size() >= 4 && _stricmp(p.c_str() + p.size() - 4, ".sav") == 0; }
}  // namespace

void SetOwner(PSEPluginHandle self) { g_self = self; }
void SetLegacy(PSEDs1LegacyCosaveFn fn) { g_legacy = fn; }

std::string WithSav(const char* path) {
    std::string p = path ? path : "";
    if (!EndsWithSav(p)) p += ".sav";
    return p;
}

std::string CoSavePathFor(const std::string& savPath) {
    if (EndsWithSav(savPath)) return savPath.substr(0, savPath.size() - 4) + ".pse";
    return savPath + ".pse";
}

void SaveBegin(const char* path) { Send(PSE_MSG_SAVE_BEGIN, path); }

void SaveEnd(const char* path, bool written, bool existedBefore) {
    if (written && path) {
        const std::string sav = WithSav(path), co = CoSavePathFor(sav);
        if (!existedBefore && Exists(co)) {
            DeleteFileA(co.c_str());   // left over from a save deleted outside the game
            Log(PSE_LOG_INFO, "PSE saves: removed the stale co-save %s (its save was gone)", co.c_str());
        }
        const bool ok = pse::cosave::Save(co.c_str());
        if (ok) Log(PSE_LOG_INFO, "PSE saves: co-save %s written", co.c_str());
        else Log(PSE_LOG_ERROR, "[fail] PSE saves: co-save %s not written", co.c_str());
    }
    Send(PSE_MSG_SAVE_END, path);
}

void LoadBegin(const char* path) { Send(PSE_MSG_LOAD_BEGIN, path); }

void LoadEnd(const char* path, bool ok) {
    if (ok && path) {
        const std::string sav = WithSav(path), co = CoSavePathFor(sav);
        const uint32_t legacy = !Exists(co) && g_legacy ? g_legacy(sav.c_str(), nullptr, 0) : 0;
        if (Exists(co)) {
            if (pse::cosave::Load(co.c_str())) Log(PSE_LOG_INFO, "PSE saves: co-save %s loaded", co.c_str());
            else Log(PSE_LOG_ERROR, "[fail] PSE saves: co-save %s unreadable - plugins start fresh", co.c_str());
        } else if (legacy) {
            // A save made before the co-save (DesertStormFix's .dsfix sidecar): its data as the chunks the plugins write
            // now. The old file stays; the next save of this slot writes a .pse.
            std::vector<uint8_t> image(legacy);
            const uint32_t n = g_legacy(sav.c_str(), image.data(), legacy);
            image.resize(n < legacy ? n : legacy);
            const std::string label = sav + " (old sidecar)";
            pse::cosave::LoadBytes(image.data(), image.size(), label.c_str());
            Log(PSE_LOG_INFO, "PSE saves: %s has no co-save - read its old sidecar %s.dsfix", sav.c_str(), sav.c_str());
        } else {
            pse::cosave::Load(nullptr);   // no co-save: every plugin reverts
        }
        g_loadPending = true;
    }
    Send(PSE_MSG_LOAD_END, path);
}

void LevelStart(const char* level) {
    const bool frontEnd = level && _strnicmp(level, "frontend", 8) == 0;
    if (frontEnd) {
        g_fromFrontEnd = true;
        return;
    }
    if (g_loadPending) {
        g_loadPending = false;
    } else if (g_fromFrontEnd) {
        pse::cosave::NewGame();
        Send(PSE_MSG_NEW_GAME, level);
        Log(PSE_LOG_INFO, "PSE saves: new game (%s)", level ? level : "?");
    }
    g_fromFrontEnd = false;
}

void ResetForTests() {
    g_loadPending = false;
    g_fromFrontEnd = true;
    g_legacy = nullptr;
}
}  // namespace ds1_saves
