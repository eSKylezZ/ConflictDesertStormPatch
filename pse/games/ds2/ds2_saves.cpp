// See ds2_saves.h. The DS1 layer's ds1_saves.cpp with DS2's path rules (wide paths, ".sav" appended on save / put in
// place of the extension on load) and without DesertStormFix's legacy sidecar reader.
#include "ds2_saves.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "loader.h"

namespace ds2_saves {
namespace {   // (inside ds2_saves: pse_tests compiles this file into one unit with ds2_events.cpp)
PSEPluginHandle g_self = 0;
bool g_loadPending = false;    // a save was loaded: the next level start restores it (no NEW_GAME)
bool g_fromFrontEnd = true;    // the last level was the front end (or none yet)

void Log(PSELogLevel level, const char* fmt, ...) {
    char line[768];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    pse::LogLine(g_self, level, line);
}

void Send(uint32_t type, const std::string& text) {
    pse::Broadcast(type, text.c_str(), static_cast<uint32_t>(text.size() + 1));
}

std::wstring Wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
    return w;
}
bool Exists(const std::string& utf8) { return GetFileAttributesW(Wide(utf8).c_str()) != INVALID_FILE_ATTRIBUTES; }
bool EndsWithSav(const std::string& p) { return p.size() >= 4 && _stricmp(p.c_str() + p.size() - 4, ".sav") == 0; }
}  // namespace

void SetOwner(PSEPluginHandle self) { g_self = self; }

std::string SaveFileFor(const std::string& path) { return path + ".sav"; }

std::string LoadFileFor(const std::string& path) {
    const size_t slash = path.find_last_of("\\/:");
    const size_t dot = path.find_last_of('.');
    const bool hasExt = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    return (hasExt ? path.substr(0, dot) : path) + ".sav";
}

std::string CoSavePathFor(const std::string& savPath) {
    if (EndsWithSav(savPath)) return savPath.substr(0, savPath.size() - 4) + ".pse";
    return savPath + ".pse";
}

void SaveBegin(const std::string& sav) { Send(PSE_MSG_SAVE_BEGIN, sav); }

void SaveEnd(const std::string& sav, bool written, bool existedBefore) {
    if (written) {
        const std::string co = CoSavePathFor(sav);
        if (!existedBefore && Exists(co)) {
            DeleteFileW(Wide(co).c_str());   // left over from a save deleted outside the game
            Log(PSE_LOG_INFO, "PSE saves: removed the stale co-save %s (its save was gone)", co.c_str());
        }
        if (pse::cosave::Save(co.c_str())) Log(PSE_LOG_INFO, "PSE saves: co-save %s written", co.c_str());
        else Log(PSE_LOG_ERROR, "[fail] PSE saves: co-save %s not written", co.c_str());
    }
    Send(PSE_MSG_SAVE_END, sav);
}

void LoadBegin(const std::string& sav) { Send(PSE_MSG_LOAD_BEGIN, sav); }

void LoadEnd(const std::string& sav, bool ok) {
    if (ok) {
        const std::string co = CoSavePathFor(sav);
        if (Exists(co)) {
            if (pse::cosave::Load(co.c_str())) Log(PSE_LOG_INFO, "PSE saves: co-save %s loaded", co.c_str());
            else Log(PSE_LOG_ERROR, "[fail] PSE saves: co-save %s unreadable - plugins start fresh", co.c_str());
        } else {
            pse::cosave::Load(nullptr);   // no co-save: every plugin reverts
        }
        g_loadPending = true;
    }
    Send(PSE_MSG_LOAD_END, sav);
}

void LevelStart(const char* level) {
    if (level && _stricmp(level, "frontend") == 0) {
        g_fromFrontEnd = true;
        return;
    }
    if (g_loadPending) {
        g_loadPending = false;
    } else if (g_fromFrontEnd) {
        pse::cosave::NewGame();
        Send(PSE_MSG_NEW_GAME, level ? level : "");
        Log(PSE_LOG_INFO, "PSE saves: new game (%s)", level ? level : "?");
    }
    g_fromFrontEnd = false;
}

void ResetForTests() {
    g_loadPending = false;
    g_fromFrontEnd = true;
}
}  // namespace ds2_saves
