// Original game files moved out of the game folder once processed (user decision 2026-10-20): the archives (*.dat,
// not the uninstaller's), the level sound caches (*.sch) and the level script DLLs (Mission* / Training* / MP_MISSION*
// / FrontEnd / Outro .dll) go to
// <game>\PSE\Originals (pse_build OriginalsDir) at every start - the asset store and the mission translator read them
// there (they list the game folder first, then OriginalsDir):
//   - The game finds them through the DS1 game layer's redirect (pse/games/ds1/ds1_originals.cpp, since 2026-10-22 -
//     before, these hooks were here and a game without DesertStormFix couldn't open its own archives): the exe's file
//     imports open the PSE\Originals copy of a game-folder file that isn't there.
//   - DesertStormFix's own readers use features::GameFile / GameFiles / OriginalPath (both folders, game folder first).
//   - A file back in the game folder (Steam / GOG "verify files", a game update, the player moving it) wins, and the
//     next start processes and moves it again (same size + date as the PSE\Originals copy = dropped, else replaces it).
//   - A game folder that can't be written: nothing moves, the files are read where they are.
// Putting the files back = moving PSE\Originals\* into the game folder (readme). Dev builds: Dev\KeepOriginals = 1
// leaves them in place (test tools that read the DLLs / archives from the game folder).
#include <windows.h>

#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {

std::string g_game, g_originals;   // "<game>" and "<game>\PSE\Originals" (ANSI, as the game's paths are)
std::set<std::string> g_moved;     // lower-case names present in PSE\Originals
std::mutex g_mutex;

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

bool Exists(const std::string& path) { return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

void ScanMoved() {
    std::set<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_originals + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.insert(Lower(fd.cFileName));
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_moved.swap(names);
}

// The PSE\Originals path for a game-folder path (relative or absolute) whose file was moved; "" = use it as it is.
std::string Redirect(const char* path) {
    if (!path || !*path || g_game.empty() || strpbrk(path, "*?")) return {};
    char full[MAX_PATH];
    char* file = nullptr;
    const DWORD n = GetFullPathNameA(path, MAX_PATH, full, &file);
    if (!n || n >= MAX_PATH || !file || file == full) return {};
    const std::string dir(full, file - 1);
    if (_stricmp(dir.c_str(), g_game.c_str()) != 0) return {};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_moved.count(Lower(file))) return {};
    }
    if (GetFileAttributesA(full) != INVALID_FILE_ATTRIBUTES) return {};   // back in the game folder: it wins
    return g_originals + "\\" + file;
}

bool Processed(const char* name) {   // the files that move: archives, sound caches, the level script DLLs
    const size_t n = strlen(name);
    if (n < 5) return false;
    const char* ext = name + n - 4;
    if (_stricmp(ext, ".dat") == 0) return _strnicmp(name, "unins", 5) != 0;
    if (_stricmp(ext, ".sch") == 0) return true;
    if (_stricmp(ext, ".dll") == 0)   // every level's script DLL (all loaded through the LoadLibraryA redirect)
        return _strnicmp(name, "mission", 7) == 0 || _strnicmp(name, "training", 8) == 0 ||
               _strnicmp(name, "mp_mission", 10) == 0 || _stricmp(name, "frontend.dll") == 0 ||
               _stricmp(name, "outro.dll") == 0;
    return false;
}

bool KeepOriginals() {
#ifndef DS_DIST
    DWORD v = 0, size = sizeof v;
    return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "KeepOriginals", RRF_RT_REG_DWORD, nullptr,
                        &v, &size) == ERROR_SUCCESS && v != 0;
#else
    return false;
#endif
}

void MoveOriginals() {
    if (KeepOriginals()) {
        dslog::Write("Originals: Dev\\KeepOriginals = 1 - the game's files stay in the game folder");
        return;
    }
    std::vector<std::string> names;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_game + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Processed(fd.cFileName)) names.push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    if (names.empty()) return;
    if (!CreateDirectoryA(g_originals.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        dslog::Write("Originals: can't create %s (error %lu) - the game's files stay in the game folder",
                     g_originals.c_str(), GetLastError());
        return;
    }
    int moved = 0, dropped = 0, replaced = 0, failed = 0;
    const DWORD t0 = GetTickCount();
    for (const std::string& name : names) {
        const std::string from = g_game + "\\" + name, to = g_originals + "\\" + name;
        WIN32_FILE_ATTRIBUTE_DATA a{}, b{};
        const bool there = GetFileAttributesExA(to.c_str(), GetFileExInfoStandard, &b) != 0;
        if (there && GetFileAttributesExA(from.c_str(), GetFileExInfoStandard, &a) &&
            a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
            CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) == 0) {
            if (DeleteFileA(from.c_str())) ++dropped;
            else ++failed;
            continue;
        }
        if (MoveFileExA(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            there ? ++replaced : ++moved;
        } else {
            ++failed;
            dslog::Write("Originals: %s not moved (error %lu) - read from the game folder", name.c_str(), GetLastError());
            if (GetLastError() == ERROR_ACCESS_DENIED) break;   // a read-only install: leave the rest too
        }
    }
    dslog::Write("Originals: %d moved to %s, %d already there, %d replaced (game update), %d not moved (%lu ms)", moved,
                 g_originals.c_str(), dropped, replaced, failed, GetTickCount() - t0);
}

}  // namespace

std::string features::OriginalPath(const char* name) {
    return name ? Redirect((g_game + "\\" + name).c_str()) : std::string();
}

std::string features::GameFile(const std::string& name) {
    const std::string here = g_game + "\\" + name;
    if (g_game.empty() || Exists(here)) return here;
    const std::string there = g_originals + "\\" + name;
    return Exists(there) ? there : here;
}

std::vector<std::string> features::GameFiles(const char* pattern) {
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const std::string& dir : {g_game, g_originals}) {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && seen.insert(Lower(fd.cFileName)).second)
                out.push_back(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return out;
}

// Early (before ApplyMods): the folders + the redirect, so every later reader finds moved files.
void features::ApplyOriginalsRedirect() {
    if (!g_game.empty()) return;
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    g_game = exe;
    g_game.resize(g_game.find_last_of("\\/"));
    g_originals = g_game + "\\PSE\\Originals";
    ScanMoved();
    if (!g_moved.empty()) dslog::Write("Originals: %zu file(s) in %s", g_moved.size(), g_originals.c_str());
}

// Late (after the asset store and the mission translations have read them): move what's in the game folder.
void features::ApplyOriginalsMove() {
    if (g_game.empty()) ApplyOriginalsRedirect();
    MoveOriginals();
    ScanMoved();
}
