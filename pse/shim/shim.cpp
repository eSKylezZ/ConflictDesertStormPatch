// PSE shim (docs/sdk/layout.md): the proxy DLL a game loads before any of its own code runs (DS1: dinput8.dll,
// exports in proxy_dinput8.cpp). Game-neutral - it knows no game, no address and no plugin.
//
// DllMain (loader lock: nothing else may happen there) only redirects the exe's entry point to EntryStub, and only
// when the DLL is loaded with the process (a static import; a LoadLibrary later, after the entry point ran, does
// nothing). At the entry point, outside the loader lock and before the CRT / WinMain: the entry bytes are put back,
// <folder of this DLL>\PSE\pse_loader.dll is loaded and its PSELoader_Start runs (exe detection, game layer, plugins).
// No loader, no export, a failed or crashed start = one OutputDebugString line and the game runs vanilla.
#include <windows.h>

#include <cstring>

#include "host_api.h"

namespace {
HMODULE g_self = nullptr;
uint8_t g_entrySaved[5];
uint32_t g_entry = 0;

void Say(const wchar_t* text) {
    wchar_t line[600];
    wsprintfW(line, L"[PSE shim] %s\n", text);
    OutputDebugStringW(line);
}

// PSE didn't start but a host plugin had moved the game's own files to <game>\PSE\Originals (DesertStormFix, every
// boot): the game can't find them now - tell the player once what happened and how to get them back.
void OriginalsWarning(const wchar_t* folder, const wchar_t* why) {
    wchar_t dir[MAX_PATH + 32], pattern[MAX_PATH + 40];
    wsprintfW(dir, L"%sPSE\\Originals", folder);
    wsprintfW(pattern, L"%s\\*.*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    bool any = false;
    do any = any || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
    while (!any && FindNextFileW(h, &fd));
    FindClose(h);
    if (!any) return;
    wchar_t text[2 * MAX_PATH + 600];
    wsprintfW(text, L"The game's own files (archives, sound caches, mission DLLs) were moved to\n%s\nby the PSE plugin, "
                    L"but PSE didn't start this time:\n%s\n\nThe game can't run without those files. Reinstall the "
                    L"patch, or move every file from that folder back into the game folder.", dir, why);
    MessageBoxW(nullptr, text, L"PSE: game files are in PSE\\Originals", MB_OK | MB_ICONWARNING);
}

int StartFilter(EXCEPTION_POINTERS* ep, DWORD* code) {
    *code = ep->ExceptionRecord->ExceptionCode;
    return EXCEPTION_EXECUTE_HANDLER;
}
int GuardedStart(PSELoaderStartFn start, const PSEShimInfo* info, int* result, DWORD* code) {
    __try {
        *result = start(info);
        return 1;
    } __except (StartFilter(GetExceptionInformation(), code)) {
        return 0;
    }
}

void StartLoaderImpl(wchar_t* why, wchar_t* advice);

// PSE didn't start: the player sees why once (a release build writes no other log, and the game otherwise just
// looks unpatched - 2026-10-22: "it's not showing the menu changes"). Shown again only when the reason changes
// (HKCU\Software\PSE ShimNotice), so a player who keeps the files on purpose isn't asked every start.
void Notice(const wchar_t* why, const wchar_t* advice) {
    HKEY key = nullptr;
    wchar_t last[600] = L"";
    DWORD size = sizeof last;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\PSE", 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"ShimNotice", nullptr, nullptr, reinterpret_cast<BYTE*>(last), &size);
        if (lstrcmpW(last, why) == 0) {
            RegCloseKey(key);
            return;
        }
        RegSetValueExW(key, L"ShimNotice", 0, REG_SZ, reinterpret_cast<const BYTE*>(why),
                       static_cast<DWORD>((lstrlenW(why) + 1) * sizeof(wchar_t)));
        RegCloseKey(key);
    }
    wchar_t text[2048];
    wsprintfW(text, L"The patch (Pivotal Script Extender) isn't running, so the game starts unpatched.\n\n%s\n\n%s\n\n"
                    L"(This message shows once per reason. Details: PSE\\Logs\\loader.log in the game folder.)", why, advice);
    MessageBoxW(nullptr, text, L"PSE: the patch isn't active", MB_OK | MB_ICONWARNING);
}

void StartLoader() {
    wchar_t why[MAX_PATH + 300] = L"", advice[600] = L"";
    StartLoaderImpl(why, advice);
    if (!why[0]) {
        HKEY key = nullptr;   // running: a later failure is news again
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PSE", 0, KEY_WRITE, &key) == ERROR_SUCCESS) {
            RegDeleteValueW(key, L"ShimNotice");
            RegCloseKey(key);
        }
        return;
    }
    Say(why);
    wchar_t folder[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(g_self, folder, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    while (n > 0 && folder[n - 1] != L'\\' && folder[n - 1] != L'/') --n;
    folder[n] = 0;
    Notice(why, advice);
    OriginalsWarning(folder, why);
}

// fills `why` (and what to do about it) when PSE isn't running afterwards
void StartLoaderImpl(wchar_t* why, wchar_t* advice) {
    wchar_t path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return (void)lstrcpyW(why, L"can't find its own folder - the game runs without PSE");
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    path[n] = 0;
    wcscat_s(path, L"PSE\\pse_loader.dll");
    // Altered search path: anything pse_loader.dll imports is looked for next to it first.
    HMODULE loader = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loader) {
        const DWORD error = GetLastError();
        wsprintfW(why, L"%s not loaded (error %lu) - the game runs without PSE", path, error);
        lstrcpyW(advice, error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_MOD_NOT_FOUND
                             ? L"dinput8.dll is only the starter: copy EVERYTHING from the download into the game "
                               L"folder (next to DesertStorm.exe) - dinput8.dll and the whole PSE folder."
                             : L"Reinstall the patch (all files from the download, next to DesertStorm.exe).");
        return;
    }
    auto start = reinterpret_cast<PSELoaderStartFn>(GetProcAddress(loader, PSE_LOADER_START_EXPORT));
    if (!start) {
        FreeLibrary(loader);
        return (void)lstrcpyW(why, L"pse_loader.dll has no PSELoader_Start - the game runs without PSE");
    }
    wchar_t proxy[MAX_PATH];
    DWORD pn = GetModuleFileNameW(g_self, proxy, MAX_PATH);
    const wchar_t* name = proxy + (pn < MAX_PATH ? pn : 0);
    while (name > proxy && name[-1] != L'\\' && name[-1] != L'/') --name;
    PSEShimInfo info = {sizeof(PSEShimInfo), 1, g_self, name};
    int running = 0;
    DWORD code = 0;
    if (!GuardedStart(start, &info, &running, &code)) {
        wsprintfW(why, L"PSELoader_Start crashed (exception 0x%08lX) - the game continues as far as it can", code);
        return;
    }
    if (!running) {
        const char* reason = "";
        if (auto fn = reinterpret_cast<PSELoaderReasonFn>(GetProcAddress(loader, PSE_LOADER_REASON_EXPORT))) reason = fn();
        wsprintfW(why, L"PSE is not active: %S", reason && reason[0] ? reason : "see PSE\\Logs\\loader.log");
        // The shim is game-neutral: the advice names the exe that was started and, for the games PSE knows, the
        // versions it supports.
        wchar_t exe[MAX_PATH];
        DWORD en = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const wchar_t* exeName = exe + (en < MAX_PATH ? en : 0);
        while (exeName > exe && exeName[-1] != L'\\' && exeName[-1] != L'/') --exeName;
        if (!*exeName) exeName = L"game exe";
        const bool ds1 = lstrcmpiW(exeName, L"DesertStorm.exe") == 0;
        const bool ds2 = !ds1 && CompareStringOrdinal(exeName, 6, L"CDS II", 6, TRUE) == CSTR_EQUAL;
        const bool tge = lstrcmpiW(exeName, L"GreatEscape.exe") == 0;
        if (reason && strstr(reason, "recognises this exe")) {
            wsprintfW(advice, L"This %s isn't a version the patch supports%s", exeName,
                      ds1   ? L" (Conflict: Desert Storm: the Steam / GOG release). Other versions (CD, other regions, "
                              L"changed exes) aren't supported yet."
                      : ds2 ? L" (Conflict: Desert Storm II: CDS II.exe 1.0 without the disc check). The CD exe with "
                              L"SecuROM's disc check isn't supported; other versions aren't supported yet."
                      : tge ? L" (The Great Escape: GreatEscape.exe 1.0 without the disc check). Other versions aren't "
                              L"supported yet."
                            : L".");
        } else {
            wsprintfW(advice, L"Reinstall the patch (all files from the download, next to %s).", exeName);
        }
    }
}

}  // namespace

// Loaded after the exe started (a protected / wrapped exe that loads its DLLs itself): the entry point has already
// run, so PSE starts at the game's first DirectInput call instead (proxy_dinput8.cpp) - outside the loader lock, before
// the game reads any input. Late, but every patch checks its bytes, so the game is patched from then on.
bool g_late = false;
volatile LONG g_lateStarted = 0;
extern "C" void ShimLateStart() {
    if (!g_late || InterlockedExchange(&g_lateStarted, 1)) return;
    Say(L"loaded after the exe started - PSE starts at the first DirectInput call");
    StartLoader();
}

namespace {

extern "C" void __cdecl InitOnEntry() {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_entry), 5, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(g_entry), g_entrySaved, 5);
    VirtualProtect(reinterpret_cast<void*>(g_entry), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_entry), 5);
    StartLoader();
}

__declspec(naked) void EntryStub() {
    __asm {
        pushad
        call InitOnEntry
        popad
        jmp dword ptr [g_entry]
    }
}

void HookEntryPoint() {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) return;
    g_entry = reinterpret_cast<uint32_t>(base) + nt->OptionalHeader.AddressOfEntryPoint;

    uint8_t jmp[5] = {0xE9};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&EntryStub) - (g_entry + 5));
    std::memcpy(jmp + 1, &rel, 4);

    DWORD old;
    if (!VirtualProtect(reinterpret_cast<void*>(g_entry), 5, PAGE_EXECUTE_READWRITE, &old)) return;
    std::memcpy(g_entrySaved, reinterpret_cast<void*>(g_entry), 5);
    std::memcpy(reinterpret_cast<void*>(g_entry), jmp, 5);
    VirtualProtect(reinterpret_cast<void*>(g_entry), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_entry), 5);
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        // reserved != NULL: loaded with the process (static import) - the entry point hasn't run yet; else the first
        // DirectInput call starts PSE (ShimLateStart).
        if (reserved) HookEntryPoint();
        else g_late = true;
    }
    return TRUE;
}
