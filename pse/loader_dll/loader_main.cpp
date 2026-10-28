// pse_loader.dll (docs/sdk/layout.md): the one copy of the PSE loader services in the process (pse_core, linked in
// statically) + the start-up the shim calls at the exe's entry point. Game-neutral.
#include <windows.h>

#include <cstdio>
#include <string>

#include "loader.h"

namespace {
HMODULE g_self = nullptr;
std::string g_reason;   // the last start's "why" when PSE stayed off (PSELoader_Reason)

// Every build: the loader's own lines (start-up, before any host plugin took them - all of them when PSE stays off)
// go to <PSE folder>\Logs\loader.log, so a player whose game ran vanilla can see why (release builds write no other
// log); dev builds also send them to the debugger output.
FILE* g_file = nullptr;
std::wstring g_logPath;
void FileLog(const char* line) {
    if (!g_file && !g_logPath.empty()) {
        CreateDirectoryW(g_logPath.substr(0, g_logPath.find_last_of(L'\\')).c_str(), nullptr);
        _wfopen_s(&g_file, g_logPath.c_str(), L"w");
        g_logPath.clear();   // one try
    }
    if (g_file) {
        fputs(line, g_file);
        fputc('\n', g_file);
        fflush(g_file);
    }
#ifndef DS_DIST
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
#endif
}
void FileReport(const char* line, int) { FileLog(line); }

std::wstring FolderOf(HMODULE m) {
    wchar_t path[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(m, path, MAX_PATH);
    std::wstring s(path, n < MAX_PATH ? n : 0);
    const size_t slash = s.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash);
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

extern "C" __declspec(dllexport) int PSE_CALL PSELoader_Start(const PSEShimInfo* shim) {
    pse::StartOptions o;
    o.pseDir = FolderOf(g_self);
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    o.exePath.assign(exe, n < MAX_PATH ? n : 0);
    auto base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
    o.imageBase = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(base));
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) o.peTimestamp = nt->FileHeader.TimeDateStamp;
    }
    g_logPath = o.pseDir + L"\\Logs\\loader.log";
    o.fallbackLog = &FileLog;
    o.fallbackReport = &FileReport;
    (void)shim;   // version 1 carries nothing the start-up needs yet (the proxy's name is in the shim's own log)
    const pse::StartResult r = pse::Start(o);
    g_reason = r.running ? std::string() : r.why;
    return r.running ? 1 : 0;
}

extern "C" __declspec(dllexport) const char* PSE_CALL PSELoader_Reason() { return g_reason.c_str(); }
