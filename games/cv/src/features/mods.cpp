// Loose-file mods for Conflict: Vietnam: any file the game loads by name can be replaced (or added) by dropping it into
// a folder under <game>\Mods\<mod name>\ (any sub folders). Mods are applied in name order, a later mod wins; folders
// starting with '_' or '.' are switched off. The index (upper-case file name -> path) is built once at start-up.
//
// Vietnam.exe's file layer (mapped 2026-10-10, same design as Desert Storm 1 at other addresses):
//   Resolve    0x5308F5  cdecl(name, search paths, flags) -> path buffer 0x881238 (0x200) / the caller's name / 0.
//              Archive-index first, then <cwd>\name, <cwd>\Data\name, search paths. Callers: OpenPath 0x530C4B (and so
//              OpenFile 0x53145D, whole-file load 0x531474 - scenes, routes, tables, textures) and ResolveDefault
//              0x531446 (the texture finder trying extensions). Hooked first: a file a mod ships wins.
//   Open       0x52FA17  thiscall(file, path) ret 4, called by the pool opener 0x5300D9. With archive mode on (flags
//              [0x85DAEC] bit 0, set by the built-in start-up option "-d") it ignores the folder, hashes the bare name
//              and reads from the mounted .dat; the flags are copied into file +0x40 and decide reads / close later.
//              Hooked: a path inside Mods\ that exists on disk is opened with bit 0 cleared for that call (its own
//              fopen branch). Other paths - e.g. "<Mods dir>\mission01.spt", derived from a mod scene's folder - keep
//              the archive branch, which only uses the bare name.
//   Exists     0x53154E  cdecl(name): in archive mode it tail-jumps to the archive-index lookup 0x52FB81 (no resolver)
//              -> hooked: yes for mod files.
// Not covered: lightmap bundles (BundlesPC\*.pcr, CRT fopen 0x68C18F) and textures already in memory (texture +0x2C /
// +0x30, 0x4DD225).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kResolve = 0x5308F5, kResolveCont = 0x5308FD;
constexpr uint8_t kResolveEntry[] = {0x83, 0xEC, 0x0C, 0xA0, 0xEC, 0xDA, 0x85, 0x00};  // sub esp,0Ch; mov al,[0x85DAEC]
constexpr uint32_t kOpen = 0x52FA17, kOpenCont = 0x52FA20;
constexpr uint8_t kOpenEntry[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00};  // push ebp; mov ebp,esp; sub esp,104h
constexpr uint32_t kExists = 0x53154E, kExistsCont = 0x531555;
constexpr uint8_t kExistsEntry[] = {0xF6, 0x05, 0xEC, 0xDA, 0x85, 0x00, 0x01};           // test byte [0x85DAEC],1
constexpr uint32_t kFileFlags = 0x85DAEC;                                                  // bit 0 = archive mode

using ResolveFn = char*(__cdecl*)(const char*, void*, int);
using OpenFn = int(__thiscall*)(void*, const char*);
using ExistsFn = int(__cdecl*)(const char*);
ResolveFn g_resolve = nullptr;
OpenFn g_open = nullptr;
ExistsFn g_exists = nullptr;

struct ModFile {
    std::string path;
    int mod;
};
std::unordered_map<std::string, ModFile> g_files;  // upper-case file name -> file
std::vector<std::string> g_mods;  // enabled mod folder names, as indexed (BuildIndex)
std::string g_modsRoot;
char g_result[MAX_PATH * 2];  // what our resolver hands out (the game copies it into its own buffer straight away)

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string BaseName(const char* name) {
    const char* p = name;
    for (const char* q = name; *q; ++q)
        if (*q == '\\' || *q == '/' || *q == ':') p = q + 1;
    return p;
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

void ScanFolder(const std::string& dir, int mod, int& count) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        const std::string path = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanFolder(path, mod, count);
            continue;
        }
        const std::string key = Upper(fd.cFileName);
        auto it = g_files.find(key);
        if (it != g_files.end())
            dslog::Write("Mods: %s replaces %s's %s", g_mods[mod].c_str(), g_mods[it->second.mod].c_str(), fd.cFileName);
        g_files[key] = {path, mod};
        ++count;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void BuildIndex() {
    if (g_modsRoot.empty()) g_modsRoot = GameDir() + "\\Mods";
    g_files.clear();
    g_mods.clear();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_modsRoot + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> names;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && fd.cFileName[0] != '_')
            names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(),
              [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const std::string& name : names) {
        const int mod = static_cast<int>(g_mods.size());
        g_mods.push_back(name);
        int count = 0;
        ScanFolder(g_modsRoot + "\\" + name, mod, count);
        dslog::Write("Mods: %s - %d file(s)", name.c_str(), count);
    }
}

// Every folder under Mods\ regardless of the leading "_" (for a future menu to list) - name without the "_", on/off.
std::vector<std::pair<std::string, bool>> EnumerateAll() {
    std::vector<std::pair<std::string, bool>> all;
    if (g_modsRoot.empty()) g_modsRoot = GameDir() + "\\Mods";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_modsRoot + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return all;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        const bool off = fd.cFileName[0] == '_';
        all.emplace_back(off ? fd.cFileName + 1 : fd.cFileName, !off);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return _stricmp(a.first.c_str(), b.first.c_str()) < 0; });
    return all;
}

const ModFile* Find(const char* name) {
    if (!name || !*name || g_files.empty()) return nullptr;
    auto it = g_files.find(Upper(BaseName(name)));
    return it == g_files.end() ? nullptr : &it->second;
}

bool InModsFolder(const char* path) {
    return path && _strnicmp(path, g_modsRoot.c_str(), g_modsRoot.size()) == 0;
}

char* __cdecl ResolveHook(const char* name, void* paths, int flags) {
#ifndef DS_DIST
    if (name) {   // dev log: level-level files the game asks for (scene, routes, scripts), each name once
        const std::string up = Upper(BaseName(name));
        const size_t dot = up.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : up.substr(dot);
        static std::unordered_map<std::string, bool> seen;
        if ((ext == ".ENV" || ext == ".RMD" || ext == ".QTD" || ext == ".LUA" || ext == ".OCT") && !seen[up]) {
            seen[up] = true;
            dslog::Write("[dev] resolve %s (flags %d)", name, flags);
        }
    }
#endif
    if (const ModFile* f = Find(name)) {
        static std::unordered_map<std::string, bool> logged;
        const std::string key = Upper(BaseName(name));
        if (!logged[key]) {
            logged[key] = true;
            dslog::Write("Mods: %s -> %s", name, f->path.c_str());
        }
        strncpy_s(g_result, f->path.c_str(), _TRUNCATE);
        return g_result;
    }
    return g_resolve(name, paths, flags);
}

int __fastcall OpenHook(void* file, void* /*edx*/, const char* path) {
    if (InModsFolder(path) && GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
        auto flags = reinterpret_cast<volatile uint32_t*>(kFileFlags);
        const uint32_t saved = *flags;
        *flags = saved & ~1u;                // disk branch for this file (copied into file +0x40)
        const int ok = g_open(file, path);
        *flags = (*flags & ~1u) | (saved & 1u);
        return ok;
    }
    return g_open(file, path);
}

int __cdecl ExistsHook(const char* name) {
    return Find(name) ? 1 : g_exists(name);
}

// Copies `len` entry bytes to an executable stub followed by `jmp cont`; returns the stub.
void* Trampoline(const uint8_t* entry, size_t len, uint32_t cont) {
    auto stub = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!stub) return nullptr;
    memcpy(stub, entry, len);
    stub[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(cont - (reinterpret_cast<uint32_t>(stub) + len + 5));
    memcpy(stub + len + 1, &rel, 4);
    return stub;
}

bool Hook(uint32_t site, const uint8_t* entry, size_t len, uint32_t cont, const void* fn, void** original) {
    if (!patch::Matches(site, entry, len)) return false;
    *original = Trampoline(entry, len, cont);
    return *original && patch::WriteJump(site, fn);
}
}  // namespace

void features::ApplyMods() {
    BuildIndex();
    // Hooked even with no mods: the MODS menu (modsmenu.cpp) can switch one on while the game runs.
    if (g_files.empty()) dslog::Write("Mods: no mods in %s", g_modsRoot.c_str());
    const bool ok = patch::Matches(kResolve, kResolveEntry, sizeof kResolveEntry) &&
                    patch::Matches(kOpen, kOpenEntry, sizeof kOpenEntry) &&
                    patch::Matches(kExists, kExistsEntry, sizeof kExistsEntry);
    if (!ok) {
        dslog::Write("[fail] Mods: unexpected bytes at the file layer - different exe version? Mods not loaded.");
        return;
    }
    Hook(kResolve, kResolveEntry, sizeof kResolveEntry, kResolveCont, reinterpret_cast<const void*>(&ResolveHook),
         reinterpret_cast<void**>(&g_resolve));
    Hook(kOpen, kOpenEntry, sizeof kOpenEntry, kOpenCont, reinterpret_cast<const void*>(&OpenHook),
         reinterpret_cast<void**>(&g_open));
    Hook(kExists, kExistsEntry, sizeof kExistsEntry, kExistsCont, reinterpret_cast<const void*>(&ExistsHook),
         reinterpret_cast<void**>(&g_exists));
    dslog::Write("Mods: %zu mod(s), %zu file(s) - resolver, opener and exists check hooked", g_mods.size(), g_files.size());
}

// ---- backend for a future in-game MODS menu (no screen built yet for Vietnam - see games/cv/CLAUDE.md) ----
// There is no RE'd main-menu list / state-dispatch / text-draw machinery for Vietnam.exe yet (unlike DS1, where
// overlay.cpp / hud.cpp / coop.cpp already existed before modsmenu.cpp was written); these three functions are the
// same mechanical operations DS1's modsmenu.cpp calls into mods.cpp for (scan, toggle, rescan), exposed so that
// whichever session does the menu RE doesn't also have to touch the file layer again.

void features::ModsRescan() { BuildIndex(); }  // re-hooks nothing (the 3 hooks stay installed); re-reads Mods\ only

std::vector<std::pair<std::string, bool>> features::ModsList() { return EnumerateAll(); }

bool features::ModsToggle(const std::string& name) {
    if (g_modsRoot.empty()) g_modsRoot = GameDir() + "\\Mods";
    bool on = false, found = false;
    for (const auto& m : EnumerateAll())
        if (_stricmp(m.first.c_str(), name.c_str()) == 0) { on = m.second; found = true; break; }
    if (!found) {
        dslog::Write("[fail] Mods: toggle %s - no such folder", name.c_str());
        return false;
    }
    const std::string dir = g_modsRoot + "\\";
    const std::string from = dir + (on ? "" : "_") + name, to = dir + (on ? "_" : "") + name;
    if (!MoveFileA(from.c_str(), to.c_str())) {
        dslog::Write("[fail] Mods: toggle %s -> %s (error %lu)", from.c_str(), to.c_str(), GetLastError());
        return false;
    }
    dslog::Write("Mods: %s switched %s", name.c_str(), on ? "off" : "on");
    BuildIndex();  // the hooks are already installed; only the index (which files/folders they serve) changes
    return true;
}

int features::ModsInstalledCount() { return static_cast<int>(EnumerateAll().size()); }
int features::ModsActiveCount() { return static_cast<int>(g_mods.size()); }
