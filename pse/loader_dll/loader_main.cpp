// pse_loader.dll (docs/sdk/layout.md): the one copy of the PSE loader services in the process (pse_core, linked in
// statically) + the start-up the shim calls at the exe's entry point. Game-neutral.
#include <windows.h>

#include <string>

#include "loader.h"

namespace {
HMODULE g_self = nullptr;

#ifndef DS_DIST
// Dev builds: loader lines that no host plugin took (no dsfix.dll, unknown exe ...) go to the debugger output.
void DebugOut(const char* line) {
    OutputDebugStringA(line);
    OutputDebugStringA("\n");
}
void DebugReport(const char* line, int) { DebugOut(line); }
#endif

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
#ifndef DS_DIST
    o.fallbackLog = &DebugOut;
    o.fallbackReport = &DebugReport;
#endif
    (void)shim;   // version 1 carries nothing the start-up needs yet (the proxy's name is in the shim's own log)
    return pse::Start(o).running ? 1 : 0;
}
