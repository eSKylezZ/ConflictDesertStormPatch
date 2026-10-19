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

void StartLoader() {
    wchar_t path[MAX_PATH + 32];
    DWORD n = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return Say(L"can't find its own folder - the game runs without PSE");
    while (n > 0 && path[n - 1] != L'\\' && path[n - 1] != L'/') --n;
    path[n] = 0;
    wcscat_s(path, L"PSE\\pse_loader.dll");
    // Altered search path: anything pse_loader.dll imports is looked for next to it first.
    HMODULE loader = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    wchar_t msg[MAX_PATH + 128];
    if (!loader) {
        wsprintfW(msg, L"%s not loaded (error %lu) - the game runs without PSE", path, GetLastError());
        return Say(msg);
    }
    auto start = reinterpret_cast<PSELoaderStartFn>(GetProcAddress(loader, PSE_LOADER_START_EXPORT));
    if (!start) {
        FreeLibrary(loader);
        return Say(L"pse_loader.dll has no PSELoader_Start - the game runs without PSE");
    }
    wchar_t proxy[MAX_PATH];
    DWORD pn = GetModuleFileNameW(g_self, proxy, MAX_PATH);
    const wchar_t* name = proxy + (pn < MAX_PATH ? pn : 0);
    while (name > proxy && name[-1] != L'\\' && name[-1] != L'/') --name;
    PSEShimInfo info = {sizeof(PSEShimInfo), 1, g_self, name};
    int running = 0;
    DWORD code = 0;
    if (!GuardedStart(start, &info, &running, &code)) {
        wsprintfW(msg, L"PSELoader_Start crashed (exception 0x%08lX) - the game continues as far as it can", code);
        return Say(msg);
    }
    if (!running) Say(L"PSE is not active for this exe (the loader's log says why) - the game runs vanilla");
}

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
        // reserved != NULL: loaded with the process (static import) - the entry point hasn't run yet.
        if (reserved) HookEntryPoint();
    }
    return TRUE;
}
