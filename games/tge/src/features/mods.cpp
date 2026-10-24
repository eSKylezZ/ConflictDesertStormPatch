// Mods folder (basic resolver): any file the game opens by name can be replaced - or added - by dropping a file with
// that name into a folder under <game>\Mods\. Port of DesertStorm2Fix's mods.cpp resolver part (games/ds2/src/features/
// mods.cpp; DS2 addresses in brackets) - the engine's Files.cpp / Catalog.cpp is the same code. No table merging,
// .strings, level patches, sound packs or MODS menu yet.
//
// Layout: <game>\Mods\<mod name>\...any sub folders...\<FILE.EXT>. Mods are applied in name order; when two mods ship
// the same file name the later one wins (logged). Folders starting with '_' or '.' are switched off. The index (upper-
// case file name, path stripped) is built at start-up. Setting FeatMods (default on) switches the whole thing off.
//
// How GreatEscape.exe finds files (2026-10-23):
//  - resolver File_Resolve FUN_004c44d0(name, search paths, flags) cdecl -> path in the buffer 0x7AB6B8, or 0
//    [FUN_004c3c40, 0x67CFA8]. Flags [0x7ACDDC]: bit 0 = archive mode (the shipped game) - the resolver then only asks
//    File_InArchive and hands back the bare NAME; bit 0x40 = names pass through. Otherwise "<game>\<name>",
//    "<game>\Data\<name>" and the search paths on disk. Callers: the open wrapper 0x4C4860 (resolve + File_OpenStream
//    FUN_004c6430(path, "rb")) and the exists check FUN_004c5200 (texture loader's extension tries FUN_004481f0: .ddl,
//    .dds, ...). The entry is hooked: a name a mod has resolves to that file.
//  - opener File_Open FUN_004c60e0(file, path) thiscall ret 4 [FUN_004c2ea0]: copies the flags to file +0x140; archive
//    mode = the bare name looked up in the current archive [0x7ACDE0] - the path's folder is ignored. For a path inside
//    Mods\ bits 0 and 1 are cleared for the call, so it takes its disk branch (fopen "rb"; +0x134 FILE*, +0x12C size
//    0x7FFFFFFF = ask the file).
//  - "is it in the archive?" File_InArchive FUN_004c5df0(name) cdecl -> 1 / 0 [FUN_004c2810]: answers 1 for a mod's
//    file, so a file only a mod has passes the game's exists checks.
//  - per-texture load Texture_Load FUN_004489c0(texture) thiscall [FUN_00542f30]: textures a level archive hands over
//    as a memory buffer (+0x30 data, +0x34 size; name +8) never ask for a file - when a mod has the name (with or
//    without .dds / .tga / ...) the buffer is dropped for the call and the load takes the file path.
//  - Global Heap [0x7CDC14] (Heap_Ctor FUN_004d1290 thiscall(size, name), call 0x5D2C07, 1 MB): the texture readers
//    read whole files into it; a file that doesn't fit = no texture. The heap is made bigger by the largest mod file
//    (rounded up to 1 MB, capped at 64 MB) - counting switched-off folders too.
// Not covered: files the game fopens itself (Bink films Data\FMV, a few writers / streams), level DLLs, sound caches.
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kResolve = 0x4C44D0;
constexpr uint8_t kResolveEntry[] = {0xA0, 0xDC, 0xCD, 0x7A, 0x00};             // mov al, [0x7ACDDC]
constexpr uint32_t kOpen = 0x4C60E0;
constexpr uint8_t kOpenEntry[] = {0x81, 0xEC, 0x08, 0x02, 0x00, 0x00};           // sub esp, 0x208
constexpr uint32_t kInArchive = 0x4C5DF0;
constexpr uint8_t kInArchiveEntry[] = {0x81, 0xEC, 0x08, 0x01, 0x00, 0x00};      // sub esp, 0x108
constexpr uint32_t kTextureLoad = 0x4489C0;
constexpr uint8_t kTextureLoadEntry[] = {0x81, 0xEC, 0x24, 0x01, 0x00, 0x00};    // sub esp, 0x124
constexpr uint32_t kHeapCtorCall = 0x5D2C07, kHeapCtor = 0x4D1290;
constexpr uint32_t kFileFlags = 0x7ACDDC;   // bit 0 archive mode, bit 1 archive build
constexpr uint32_t kPathBuffer = 0x7AB6B8;
constexpr size_t kPathBufferSize = 0x200;
constexpr uint32_t kHeapSize = 0x100000;    // the game's Global Heap
constexpr uint32_t kHeapCap = 64u << 20;    // most we add to it

using ResolveFn = char*(__cdecl*)(const char*, void*, unsigned);
using OpenFn = int(__thiscall*)(void*, const char*);
using InArchiveFn = int(__cdecl*)(const char*);
using TextureLoadFn = int(__thiscall*)(void*);
ResolveFn g_resolve = nullptr;
OpenFn g_open = nullptr;
InArchiveFn g_inArchive = nullptr;
TextureLoadFn g_textureLoad = nullptr;

struct ModFile {
    std::string path;
    int mod;
    uint32_t size;
};
std::unordered_map<std::string, ModFile> g_files;  // upper-case file name -> file
std::vector<std::string> g_mods;
std::string g_modsRoot;
uint32_t g_largestFile = 0;      // largest file the heap is sized for (every folder, on or off)
std::string g_largestName;
bool g_inMission = false;

std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// ---- mod report ----
// Release builds have no log, so what the mods did ("served" lines, which mod won a name, "[fail]" lines) goes to
// Mods\GreatEscapeFix-report.txt, written afresh at every start. A file at the top of Mods is not a mod.
std::string g_reportPath;
int g_problems = 0;

void BeginReport() {
    const DWORD attr = GetFileAttributesA(g_modsRoot.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;  // no Mods folder, no report
    g_reportPath = g_modsRoot + "\\GreatEscapeFix-report.txt";
    HANDLE f = CreateFileA(g_reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        g_reportPath.clear();
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[320];
    const int n = snprintf(head, sizeof head,
                           "GreatEscapeFix " DS_VERSION " - mod report, %04d-%02d-%02d %02d:%02d:%02d\r\n"
                           "Written when the game starts; \"served\" lines are added while it runs. Lines starting "
                           "with [fail] are problems.\r\n\r\n",
                           t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    DWORD written = 0;
    WriteFile(f, head, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

// Files the game never reads whole into its heap don't size it.
bool SizesHeap(const std::string& key) {
    static const char* const kSkip[] = {".DLL", ".WAV", ".BIK", ".BLEND", ".BLEND1", ".EXE", ".ZIP", ".7Z",
                                        ".MP4", ".JSON", ".MD", ".PY", ".TXT", ".SCH", ".SBK", ".PSF"};
    const size_t dot = key.find_last_of('.');
    if (dot == std::string::npos) return true;
    for (const char* e : kSkip)
        if (key.compare(dot, std::string::npos, e) == 0) return false;
    return true;
}

void NoteLargest(const std::string& key, const char* name, uint32_t size) {
    if (SizesHeap(key) && size > g_largestFile) {
        g_largestFile = size;
        g_largestName = name;
    }
}

// index = false: only the largest file (switched-off folders size the heap too)
void ScanFolder(const std::string& dir, int mod, int& count, bool index = true) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        const std::string path = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ScanFolder(path, mod, count, index);
            continue;
        }
        const std::string key = Upper(fd.cFileName);
        const uint32_t size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
        NoteLargest(key, fd.cFileName, size);
        if (!index) continue;
        if (path.size() >= kPathBufferSize) {
            features::ModLog("[fail] Mods: %s - path longer than %u characters, not used", path.c_str(),
                             static_cast<unsigned>(kPathBufferSize - 1));
            continue;
        }
        auto it = g_files.find(key);
        if (it != g_files.end())
            features::ModLog("Mods: %s replaces %s's %s", g_mods[mod].c_str(), g_mods[it->second.mod].c_str(), fd.cFileName);
        g_files[key] = {path, mod, size};
        ++count;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void BuildIndex() {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_modsRoot + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> names, off;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        (fd.cFileName[0] == '.' || fd.cFileName[0] == '_' ? off : names).push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    auto byName = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; };
    std::sort(names.begin(), names.end(), byName);
    std::sort(off.begin(), off.end(), byName);
    for (const std::string& name : names) {
        const int mod = static_cast<int>(g_mods.size());
        g_mods.push_back(name);
        int count = 0;
        ScanFolder(g_modsRoot + "\\" + name, mod, count);
        features::ModLog("Mods: %s - %d file(s)", name.c_str(), count);
    }
    for (const std::string& name : off) {
        features::ModLog("Mods: %s - switched off (folder name starts with %c)", name.c_str(), name[0]);
        int ignored = 0;
        ScanFolder(g_modsRoot + "\\" + name, -1, ignored, false);
    }
}

#ifndef DS_DIST
// Dev\ModsOpenLog = 1: every name the resolver is asked for, once.
void NoteOpened(const char* name) {
    static const bool logOpens = [] {
        DWORD v = 0, size = sizeof v;
        return RegGetValueA(HKEY_CURRENT_USER, "Software\\GreatEscapeFix\\Dev", "ModsOpenLog", RRF_RT_REG_DWORD,
                            nullptr, &v, &size) == ERROR_SUCCESS && v;
    }();
    if (!logOpens || !name || !*name) return;
    static std::unordered_map<std::string, bool> seen;
    const std::string key = Upper(name);
    if (seen[key]) return;
    seen[key] = true;
    dslog::Write("[dev] Mods: asked for %s%s", key.c_str(), g_inMission ? " (mission)" : "");
}
#else
void NoteOpened(const char*) {}
#endif

const ModFile* Find(const char* name) {
    if (!name || !*name || g_files.empty() || strchr(name, ':')) return nullptr;  // absolute paths stay as they are
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    auto it = g_files.find(Upper(base));
    return it == g_files.end() ? nullptr : &it->second;
}

// One "served" line per file and run (the report is for players: what their mods did).
void NoteServed(const char* name, const ModFile& f) {
    static std::unordered_map<std::string, bool> logged;
    if (logged[f.path]) return;
    logged[f.path] = true;
    features::ModLog("Mods: served %s from %s (%s)", name, g_mods[f.mod].c_str(), f.path.c_str());
}

bool StartsWith(const char* path, const std::string& root) {
    return !root.empty() && _strnicmp(path, root.c_str(), root.size()) == 0;
}

char* __cdecl Resolve(const char* name, void* paths, unsigned flags) {
    NoteOpened(name);
    if (const ModFile* f = Find(name)) {
        char* out = reinterpret_cast<char*>(kPathBuffer);
        memcpy(out, f->path.c_str(), f->path.size() + 1);
        NoteServed(name, *f);
        return out;
    }
    return g_resolve(name, paths, flags);
}

int __fastcall Open(void* file, void* /*edx*/, const char* path) {
    if (!path || !StartsWith(path, g_modsRoot)) return g_open(file, path);
    auto* flags = reinterpret_cast<uint32_t*>(kFileFlags);
    const uint32_t saved = *flags & 3u;
    *flags &= ~3u;  // this one from disk, and not into an archive being built
    const int result = g_open(file, path);
    *flags |= saved;
    if (!result) features::ModLog("[fail] Mods: can't open %s", path);
    return result;
}

int __cdecl InArchive(const char* name) {
    if (Find(name)) return 1;
    return g_inArchive(name);
}

int __fastcall TextureLoad(uint8_t* tex, void* /*edx*/) {
    if (tex && *reinterpret_cast<void**>(tex + 0x30)) {
        const char* name = *reinterpret_cast<const char**>(tex + 8);
        const ModFile* f = Find(name);
        for (const char* ext : {".dds", ".tga", ".png", ".bmp", ".ddl"})
            if (!f && name) f = Find((std::string(name) + ext).c_str());
        if (f) {
            *reinterpret_cast<void**>(tex + 0x30) = nullptr;
            *reinterpret_cast<uint32_t*>(tex + 0x34) = 0;
            static std::unordered_map<std::string, bool> logged;
            if (!logged[name]) {
                logged[name] = true;
                features::ModLog("Mods: texture %s - the level's own copy skipped, the mod's file is read", name);
            }
        }
    }
    return g_textureLoad(tex);
}

// Global Heap: 1 MB + the largest mod file, so a mod's big texture fits (see the top of the file).
void* __fastcall HeapCtor(void* self, void* /*edx*/, uint32_t size, const char* name) {
    uint32_t want = size;
    if (g_largestFile > kHeapCap)
        features::ModLog("[fail] Mods: %s is %u bytes - files the game reads whole (textures, models) can be at most %u MB",
                         g_largestName.c_str(), g_largestFile, kHeapCap >> 20);
    if (size >= kHeapSize && g_largestFile > 256 * 1024) {
        const uint32_t extra = static_cast<uint32_t>(
            std::min<uint64_t>((uint64_t{g_largestFile} + 0xFFFFF) & ~uint64_t{0xFFFFF}, kHeapCap));
        want = size + extra;
        features::ModLog("Mods: file heap %u -> %u bytes (largest mod file %s, %u bytes)", size, want,
                         g_largestName.c_str(), g_largestFile);
    }
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, const char*)>(kHeapCtor)(self, want, name);
}

template <class Fn>
bool HookEntry(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
}
}  // namespace

void features::ApplyMods() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!Get().mods) {
        dslog::Write("[off]  Mods: Mods folder ignored (FeatMods)");
        return;
    }
    g_modsRoot = GameDir() + "\\Mods";
    BeginReport();
    BuildIndex();
    if (g_files.empty()) dslog::Write("Mods: no mods switched on (%s)", g_modsRoot.c_str());
    if (!patch::Matches(kResolve, kResolveEntry, sizeof kResolveEntry) || !patch::Matches(kOpen, kOpenEntry, sizeof kOpenEntry)) {
        ModLog("[fail] Mods: unexpected game code (file resolver / opener) - mods not loaded");
        return;
    }
    if (!HookEntry(kResolve, reinterpret_cast<const void*>(&Resolve), g_resolve, kResolveEntry, sizeof kResolveEntry) ||
        !HookEntry(kOpen, reinterpret_cast<const void*>(&Open), g_open, kOpenEntry, sizeof kOpenEntry)) {
        ModLog("[fail] Mods: file resolver / opener not hooked - mods not loaded");
        return;
    }
    if (!HookEntry(kInArchive, reinterpret_cast<const void*>(&InArchive), g_inArchive, kInArchiveEntry, sizeof kInArchiveEntry))
        ModLog("[fail] Mods: exists check at 0x%08X not recognised - files only a mod has may be skipped", kInArchive);
    if (!HookEntry(kTextureLoad, reinterpret_cast<const void*>(&TextureLoad), g_textureLoad, kTextureLoadEntry,
                   sizeof kTextureLoadEntry))
        ModLog("[fail] Mods: texture load at 0x%08X not recognised - a level's own textures can't be replaced", kTextureLoad);
    if (!patch::HookCall(kHeapCtorCall, reinterpret_cast<const void*>(&HeapCtor), kHeapCtor))
        ModLog("[fail] Mods: file heap set-up at 0x%08X not recognised - mod files over ~1 MB may not load", kHeapCtorCall);
    ModLog("[ok]   Mods: %zu mod(s), %zu file(s)", g_mods.size(), g_files.size());
}

int features::ModCount() { return static_cast<int>(g_mods.size()); }
int features::ModProblems() { return g_problems; }
void features::SetInMission(bool inMission) { g_inMission = inMission; }

void features::ModLog(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    dslog::Write("%s", text);
    if (strncmp(text, "[fail]", 6) == 0) ++g_problems;
    if (g_reportPath.empty()) return;
    HANDLE f = CreateFileA(g_reportPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    WriteFile(f, "\r\n", 2, &written, nullptr);
    CloseHandle(f);
}
