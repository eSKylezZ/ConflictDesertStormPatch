// dinput8.dll proxy exports (dinput8.def): every call forwards to the real system dinput8.dll (SysWOW64 for a 32-bit
// process), loaded on first use. The shim never imports DINPUT8.dll itself (dumpbin /imports must not list it: it
// would load this very file again).
#include <windows.h>
#include <unknwn.h>

namespace {
HMODULE g_real = nullptr;

FARPROC Real(const char* name) {
    if (!g_real) {
        wchar_t path[MAX_PATH];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        wcscat_s(path, L"\\dinput8.dll");
        g_real = LoadLibraryW(path);
        if (!g_real) return nullptr;
    }
    return GetProcAddress(g_real, name);
}
}  // namespace

extern "C" {
extern "C" void ShimLateStart();   // shim.cpp: a late-loaded proxy starts PSE here

HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE inst, DWORD version, REFIID riid, LPVOID* out, IUnknown* outer) {
    ShimLateStart();
    using Fn = HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, IUnknown*);
    auto fn = reinterpret_cast<Fn>(Real("DirectInput8Create"));
    return fn ? fn(inst, version, riid, out, outer) : E_FAIL;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllCanUnloadNow"));
    return fn ? fn() : S_FALSE;
}
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID clsid, REFIID riid, LPVOID* out) {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*)>(Real("DllGetClassObject"));
    return fn ? fn(clsid, riid, out) : CLASS_E_CLASSNOTAVAILABLE;
}
HRESULT WINAPI Proxy_DllRegisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllRegisterServer"));
    return fn ? fn() : E_FAIL;
}
HRESULT WINAPI Proxy_DllUnregisterServer() {
    auto fn = reinterpret_cast<HRESULT(WINAPI*)()>(Real("DllUnregisterServer"));
    return fn ? fn() : E_FAIL;
}
}
