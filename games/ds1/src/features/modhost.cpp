// What dsfix.dll keeps of the Mods folder since the plugin dsfix.mods (2026-10-25): the mod report and whether the Mods
// folder is read at all. Both are needed from dsfix.dll's own start-up on - the loader's report sink (pse_host.cpp)
// writes every plugin's "[fail]" lines here - while the mods themselves (index, resolver, merged tables, the MODS
// menu ...) are the plugin's, which reaches these through dsfix.host (BeginModReport, ModProblems) and dsfix.services
// (ModReport -> ModLog).
//
// Mod report: release builds have no log, so what the mods did (and what went wrong: "[fail]" lines) also goes to
// Mods\DesertStormFix-report.txt - started afresh at every build of the mod state (start-up, MODS menu APPLY: the
// plugin calls BeginModReport), later messages (weapon pictures at a level load) appended. A file at the top of Mods is
// not a mod (only folders are).
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/log.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
std::string g_reportPath;
int g_problems = 0;
}  // namespace

void features::BeginModReport(const std::string& modsRoot) {
    g_problems = 0;
    g_reportPath.clear();
    const DWORD attr = GetFileAttributesA(modsRoot.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;  // no Mods folder, no report
    g_reportPath = modsRoot + "\\DesertStormFix-report.txt";
    HANDLE f = CreateFileA(g_reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        g_reportPath.clear();
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[320];
    const int n = snprintf(head, sizeof head,
                           "DesertStormFix " DS_VERSION " - mod report, %04d-%02d-%02d %02d:%02d:%02d\r\n"
                           "Written when the game starts and when MODS > APPLY runs. Lines starting with [fail] are "
                           "problems, \"note\" lines are hints.\r\n\r\n",
                           t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    DWORD written = 0;
    WriteFile(f, head, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}

int features::ModProblems() { return g_problems; }

void features::ModLog(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    dslog::Write("%s", text);
    if (strncmp(text, "[fail]", 6) == 0) ++g_problems;
    if (g_reportPath.empty()) return;
    HANDLE f = CreateFileA(g_reportPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    WriteFile(f, "\r\n", 2, &written, nullptr);
    CloseHandle(f);
}

// The Mods folder is read when FeatMods is on, or for a "+level" start from the editor (its missions need their mod).
bool features::ModsFolderEnabled() {
    static const bool on = [] {
        if (settings::Get().featMods) return true;
        const char* cmd = GetCommandLineA();
        return cmd && strstr(cmd, "+level ") != nullptr;
    }();
    return on;
}
