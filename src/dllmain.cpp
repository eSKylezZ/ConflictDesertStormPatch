// dinput8.dll proxy: DesertStorm.exe imports DINPUT8.DLL, so a dinput8.dll in the game folder is loaded
// before any game code runs. Exports forward to the real system dinput8.dll.
//
// Patching is not done inside DllMain (loader lock): DllMain only redirects the exe's entry point to
// EntryStub, which restores it, applies all features, then continues into the CRT startup / WinMain.
#include <windows.h>
#include <unknwn.h>

#include <cstring>

#include "core/log.h"
#include "core/proxy.h"
#include "core/settings.h"
#include "features/features.h"

// ---- forwarding to the real dinput8.dll ----
namespace {
HMODULE g_real = nullptr;
}

FARPROC RealDInput8(const char* name) {
    if (!g_real) {
        char path[MAX_PATH];
        UINT n = GetSystemDirectoryA(path, MAX_PATH);  // SysWOW64 for this 32-bit process
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        strcat_s(path, "\\dinput8.dll");
        g_real = LoadLibraryA(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}

namespace {

// ---- entry point hook ----
uint8_t g_entrySaved[5];
uint32_t g_entry = 0;

void Init() {
    dslog::Open();
    dslog::Write("DesertStormFix " DS_VERSION " loaded");
    settings::Load();
    features::ApplyWidescreen();
    features::ApplyHud();
    features::ApplyController();
    features::ApplyTiming();
    features::ApplyFrameCap();
    features::ApplyLauncher();
    features::ApplyInGameInput();
    features::ApplySplitScreen();
    features::ApplyDiscord();
    features::ApplyDevHooks();
}

extern "C" void __cdecl InitOnEntry() {
    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_entry), 5, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(reinterpret_cast<void*>(g_entry), g_entrySaved, 5);
    VirtualProtect(reinterpret_cast<void*>(g_entry), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_entry), 5);
    Init();
}

__declspec(naked) void EntryStub() {
    __asm {
        pushad
        call InitOnEntry
        popad
        jmp dword ptr [g_entry]
    }
}

bool IsGameExe() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    const char* name = path + n;
    while (name > path && name[-1] != '\\') --name;
    return _stricmp(name, "DesertStorm.exe") == 0;
}

void HookEntryPoint() {
    auto base = reinterpret_cast<uint8_t*>(GetModuleHandleA(nullptr));
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    g_entry = reinterpret_cast<uint32_t>(base) + nt->OptionalHeader.AddressOfEntryPoint;

    uint8_t jmp[5] = {0xE9};
    int32_t rel = static_cast<int32_t>(reinterpret_cast<uint32_t>(&EntryStub) - (g_entry + 5));
    std::memcpy(jmp + 1, &rel, 4);

    DWORD old;
    VirtualProtect(reinterpret_cast<void*>(g_entry), 5, PAGE_EXECUTE_READWRITE, &old);
    std::memcpy(g_entrySaved, reinterpret_cast<void*>(g_entry), 5);
    std::memcpy(reinterpret_cast<void*>(g_entry), jmp, 5);
    VirtualProtect(reinterpret_cast<void*>(g_entry), 5, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(g_entry), 5);
}
}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        if (IsGameExe()) HookEntryPoint();
    }
    return TRUE;
}

extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, IUnknown* outer) {
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto fn = reinterpret_cast<Fn>(RealDInput8("DirectInput8Create"));
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(RealDInput8("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(RealDInput8("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(RealDInput8("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(RealDInput8("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}
}
