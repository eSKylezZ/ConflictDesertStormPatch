// PSE\Games\pse_ds2.dll: the Conflict: Desert Storm II game layer (docs/sdk/reference/game-ds2.md, layout.md).
//   PSEGame_Detect - the exe is CDS II.exe 1.0 disc-free (SHA-256 E9B46420..., LAA bit cleared) at 0x400000. The
//                    retail SecuROM exe (44EF1E16...) is logged and refused: its imports go through SecuROM's own
//                    thunks, so the import / call-site hooks of this layer don't fit it;
//   PSEGame_Init   - registers the ID database (pse.ids, signatures checked against the ORIGINAL bytes - before any
//                    plugin patches), pse.game.ds2 v1, a log file (PSE\Logs\loader.log - DS2 has no host plugin yet to
//                    take the lines), then installs the hooks that drive the core game events and the co-save
//                    (ds2_events.cpp: frame, render, level, save / load, D3D9, exit - owner pse.game.ds2);
//   PSEGame_Info   - game id "ds2", the layer's plugin, the host plugins it allows ("ds2fix", the planned DesertStorm2Fix
//                    host plugin - docs/sdk/reference/game-ds2.md "DesertStorm2Fix as a host plugin").
// Loader services through pse/hostlink (the layer gets pse.host like a host plugin).
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "ds2_events.h"
#include "generated/pse_ids_ds2.h"   // src/generated (scripts/gen_ids.py)
#include "hostlink.h"
#include "ids.h"
#include "pse_game_ds2.h"

namespace {

const char* const kHostPlugins[] = {"ds2fix", nullptr};

// ---- PSE\Logs\loader.log: every loader / plugin line while no host plugin has a sink of its own ----
std::mutex g_logMutex;
HANDLE g_logFile = INVALID_HANDLE_VALUE;

void WriteLine(const char* prefix, const char* line) {
    std::lock_guard lock(g_logMutex);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[32];
    const int n = std::snprintf(stamp, sizeof stamp, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond,
                                t.wMilliseconds);
    DWORD done = 0;
    WriteFile(g_logFile, stamp, static_cast<DWORD>(n), &done, nullptr);
    if (prefix) WriteFile(g_logFile, prefix, static_cast<DWORD>(std::strlen(prefix)), &done, nullptr);
    WriteFile(g_logFile, line, static_cast<DWORD>(std::strlen(line)), &done, nullptr);
    WriteFile(g_logFile, "\r\n", 2, &done, nullptr);
}
void PSE_CALL LogSink(const char* line) { WriteLine(nullptr, line); }
void PSE_CALL ReportSink(const char* line, int error) { WriteLine(error ? "[report] [fail] " : "[report] ", line); }

void OpenLog(const wchar_t* pseDir) {
    if (!pseDir || !*pseDir) return;
    const std::wstring dir = std::wstring(pseDir) + L"\\Logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    g_logFile = CreateFileW((dir + L"\\loader.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_logFile == INVALID_HANDLE_VALUE) return;
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);         // the lines kept since start-up are written at once
    host->SetReportSink(&ReportSink);
}

}  // namespace

// The log file is closed when the DLL goes (process exit; tests free the layer between runs).
BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && !reserved) {   // FreeLibrary only (at process exit Windows closes it)
        std::lock_guard lock(g_logMutex);
        if (g_logFile != INVALID_HANDLE_VALUE) CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    return TRUE;
}

extern "C" {
__declspec(dllexport) extern const PSEGameLayerInfo PSEGame_Info;
const PSEGameLayerInfo PSEGame_Info = {sizeof(PSEGameLayerInfo), 1, "ds2", &pse_game_ds2::kPluginInfo, kHostPlugins};

__declspec(dllexport) int PSE_CALL PSEGame_Detect(const PSEGameDetectInfo* d) {
    if (!d || d->size < sizeof(PSEGameDetectInfo) || !d->exeSha256) return 0;
    if (_stricmp(d->exeSha256, pse_game_ds2::SecuromSha256()) == 0) {
        // Detect has no log yet: the loader logs "doesn't recognise this exe"; a debugger sees why.
        OutputDebugStringA("[pse_ds2] CDS II - Original.exe (SecuROM) is not supported - use the disc-free CDS II.exe\n");
        return 0;
    }
    return _stricmp(d->exeSha256, pse_game_ds2::KnownSha256()) == 0 && d->imageBase == 0x400000 ? 1 : 0;
}

__declspec(dllexport) int PSE_CALL PSEGame_Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return 0;
    OpenLog(pse::hostlink::Host()->pseDir);
    // ID database first: the loader calls this before any plugin, so the game's bytes are still the original ones.
    pse::ids::RegisterGameForProcess(pse_ids_ds2::kGame, pse->plugin);
    if (!pse_game_ds2::Register()) return 0;
    ds2_events::Install(pse->plugin);
    return 1;
}
}
