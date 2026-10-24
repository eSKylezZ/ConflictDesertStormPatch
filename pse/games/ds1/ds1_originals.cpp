// See ds1_originals.h. Moved here from DesertStormFix (2026-10-22, src/features/originals.cpp keeps the moving): with
// the redirect in DesertStormFix, a game without it couldn't open its own archives ("Couldn't open GenStrs.loc").
// No list of moved files is kept: DesertStormFix moves files after this is installed (same start), so a missing
// game-folder file is looked up in PSE\Originals when it is opened.
#include "ds1_originals.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#include "loader.h"
#include "pse/pse_game_ds1.h"

namespace ds1_originals {
namespace {

PSEPluginHandle g_self = 0;
bool g_installed = false;
std::string g_game, g_originals;   // "<game>" and "<game>\PSE\Originals" (ANSI, as the game's paths are)

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using GetFileAttributesAFn = DWORD(WINAPI*)(LPCSTR);
using FindFirstFileAFn = HANDLE(WINAPI*)(LPCSTR, LPWIN32_FIND_DATAA);
using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
void* g_createFileA = nullptr;
void* g_createFileW = nullptr;
void* g_getFileAttributesA = nullptr;
void* g_findFirstFileA = nullptr;
void* g_loadLibraryA = nullptr;

void Log(PSELogLevel level, const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    pse::LogLine(g_self, level, line);
}

HANDLE WINAPI RedirectCreateFileA(LPCSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                                  DWORD flags, HANDLE templ) {
    const std::string to = Redirect(name);
    return reinterpret_cast<CreateFileAFn>(g_createFileA)(to.empty() ? name : to.c_str(), access, share, sa,
                                                          disposition, flags, templ);
}
HANDLE WINAPI RedirectCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa,
                                  DWORD disposition, DWORD flags, HANDLE templ) {
    if (name) {   // the game's own paths are ANSI; only a plain ANSI name can be one of them
        char a[MAX_PATH];
        BOOL lossy = FALSE;
        if (WideCharToMultiByte(CP_ACP, 0, name, -1, a, MAX_PATH, nullptr, &lossy) && !lossy) {
            const std::string to = Redirect(a);
            if (!to.empty())
                return reinterpret_cast<CreateFileAFn>(g_createFileA)(to.c_str(), access, share, sa, disposition, flags,
                                                                      templ);
        }
    }
    return reinterpret_cast<CreateFileWFn>(g_createFileW)(name, access, share, sa, disposition, flags, templ);
}
DWORD WINAPI RedirectGetFileAttributesA(LPCSTR name) {
    const std::string to = Redirect(name);
    return reinterpret_cast<GetFileAttributesAFn>(g_getFileAttributesA)(to.empty() ? name : to.c_str());
}
HANDLE WINAPI RedirectFindFirstFileA(LPCSTR name, LPWIN32_FIND_DATAA data) {
    const std::string to = Redirect(name);
    return reinterpret_cast<FindFirstFileAFn>(g_findFirstFileA)(to.empty() ? name : to.c_str(), data);
}
HMODULE WINAPI RedirectLoadLibraryA(LPCSTR name) {
    // a bare name ("Mission3.dll") is looked for in the game folder first by Windows: same rule
    const std::string to =
        name && !std::strpbrk(name, "\\/") ? Redirect((g_game + "\\" + name).c_str()) : Redirect(name);
    if (!to.empty()) return LoadLibraryExA(to.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    return reinterpret_cast<LoadLibraryAFn>(g_loadLibraryA)(name);
}

}  // namespace

std::string Redirect(const char* path) {
    if (!path || !*path || g_game.empty() || std::strpbrk(path, "*?")) return {};
    char full[MAX_PATH];
    char* file = nullptr;
    const DWORD n = GetFullPathNameA(path, MAX_PATH, full, &file);
    if (!n || n >= MAX_PATH || !file || file == full) return {};
    const std::string dir(full, file - 1);
    if (_stricmp(dir.c_str(), g_game.c_str()) != 0) return {};
    if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) return {};   // in the game folder: it wins
    std::string moved = g_originals + "\\" + file;
    return GetFileAttributesA(moved.c_str()) != INVALID_FILE_ATTRIBUTES ? moved : std::string();
}

int Install(PSEPluginHandle self) {
    if (g_installed) return 0;
    g_installed = true;
    g_self = self;
    if (g_game.empty()) {
        char exe[MAX_PATH];
        const DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
        g_game.assign(exe, n < MAX_PATH ? n : 0);
        g_game.resize(g_game.find_last_of("\\/") == std::string::npos ? 0 : g_game.find_last_of("\\/"));
        g_originals = g_game + "\\PSE\\Originals";
    }
    const PSEHooksInterface* h = pse::HooksInterface();
    const struct {
        uint32_t slot;
        void* fn;
        void** next;
        const char* name;
    } hooks[] = {
        {kIatCreateFileA, reinterpret_cast<void*>(&RedirectCreateFileA), &g_createFileA, "CreateFileA"},
        {kIatCreateFileW, reinterpret_cast<void*>(&RedirectCreateFileW), &g_createFileW, "CreateFileW"},
        {kIatGetFileAttributesA, reinterpret_cast<void*>(&RedirectGetFileAttributesA), &g_getFileAttributesA,
         "GetFileAttributesA"},
        {kIatFindFirstFileA, reinterpret_cast<void*>(&RedirectFindFirstFileA), &g_findFirstFileA, "FindFirstFileA"},
        {kIatLoadLibraryA, reinterpret_cast<void*>(&RedirectLoadLibraryA), &g_loadLibraryA, "LoadLibraryA"},
    };
    int failed = 0;
    for (const auto& k : hooks) {
        PSEHookId id = 0;
        const PSEResult r = h->HookPointer(self, k.slot, k.fn, PSE_DS1_LAYER_PRIORITY_OUTER, k.next, &id);
        if (r != PSE_OK) {
            Log(PSE_LOG_ERROR, "[fail] Game layer: %s import 0x%08X not hooked (%d) - moved game files can't be found",
                k.name, k.slot, static_cast<int>(r));
            ++failed;
        }
    }
    if (!failed) Log(PSE_LOG_INFO, "Game layer: game files found in %s too", g_originals.c_str());
    return failed;
}

void SetFoldersForTests(const std::string& game) {
    g_game = game;
    g_originals = game + "\\PSE\\Originals";
}
void ResetForTests() {
    g_installed = false;
    g_game.clear();
    g_originals.clear();
}

}  // namespace ds1_originals
