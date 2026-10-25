// What dsfix.coop needs of its own that dsfix.dll defined: the system's dinput8.dll (hotplug.cpp's scan thread makes
// its own IDirectInput8 - the game's dinput8.dll is the PSE shim).
#include <windows.h>

#include <cstring>

FARPROC RealDInput8(const char* name) {
    static HMODULE real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        const UINT n = GetSystemDirectoryA(path, MAX_PATH);   // SysWOW64 for this 32-bit process
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        strcat_s(path, "\\dinput8.dll");
        real = LoadLibraryA(path);
        if (!real) return nullptr;
    }
    return GetProcAddress(real, name);
}
