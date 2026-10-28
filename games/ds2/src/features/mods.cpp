// Mods folder (basic resolver): any file the game opens by name can be replaced - or added - by dropping a file with
// that name into a folder under <game>\Mods\. Port of DesertStormFix's mods.cpp resolver part (repo root
// src/features/mods.cpp); no table merging, .strings or Lua yet (sounds: mods_sound.cpp).
//
// Layout: <game>\Mods\<mod name>\...any sub folders...\<FILE.EXT>. Mods are applied in name order; when two mods ship
// the same file name the later one wins (logged). Folders starting with '_' or '.' are switched off. The index (upper-
// case file name, path stripped) is built at start-up and again by the MODS menu's Apply (ReloadMods, modsmenu.cpp):
// later opens use the new index; what the game already read stays until it reads it again (ReloadMods tells which).
// Setting FeatMods (default on) switches the whole thing off.
//
// How CDS II.exe finds files (2026-10-22; DS1 addresses in brackets):
//  - resolver FUN_004c3c40(name, search paths, flags) cdecl -> path in the buffer 0x67CFA8 (0x200 bytes) or 0
//    [FUN_004be810, 0x63BF28]. Flags [0x665E4C]: bit 0 = archive mode (the shipped game) - the resolver then only asks
//    the archive index (FUN_004c2810) and hands back the bare NAME; bit 0x40 = names pass through; bit 1 = building an
//    archive (start-up option). Otherwise "<game>\<name>", "<game>\Data\<name>" and the search paths on disk.
//    Callers: open FUN_004c4000(name, paths, flags) (= resolve + FUN_004c3140(path, "rb")), whole-file reader
//    FUN_004c49e0(name, &buf, &size, heap, pool) [FUN_004bfb90], open wrappers FUN_004c49c0 (27 callers) and the
//    exists check FUN_004c49a0(name, flags) (the texture loader's extension tries 0x5427B0: .ddl, .dds, .png, .bmp,
//    .tga; model / sound loaders). The entry is hooked: a name a mod has resolves to that file.
//  - opener FUN_004c2ea0(file, path) thiscall ret 4 [FUN_004bd8a0]: copies the flags to file +0x140; archive mode =
//    hash of the bare name looked up in the current archive [0x665E54] (FUN_004c2320) - the path's folder is ignored.
//    One file object only ([0x665E48], picked by FUN_004c3140). For a path inside Mods\ bits 0 and 1 are cleared for
//    the call, so it takes its disk branch (fopen "rb"; +0x134 FILE*, +0x12C size 0x7FFFFFFF = ask the file).
//  - "is it in the archive?" FUN_004c2810(name) cdecl -> 1 / 0 (callers: the resolver, the character model preload
//    0x457411): answers 1 for a mod's file, so a file only a mod has passes the game's exists checks.
//  - per-texture load FUN_00542f30(texture) thiscall [FUN_00549a10]: textures a level archive hands over as a memory
//    buffer (+0x30 data, +0x34 size; name +8) never ask for a file - when a mod has the name (with or without .dds /
//    .tga / ...) the buffer is dropped for the call and the load takes the file path (resolver -> the mod's file).
//  - Global Heap [0x664380] (object from FUN_004c0650 thiscall(size, name), call 0x4136FC, 1 024 000 bytes; +0x48
//    size, +0x4C free, +0x50 used, +0x58 buffer): the texture readers 0x5421F0 / 0x5422A0 / 0x542E20 / 0x5427B0
//    read whole files into it (mark FUN_004c0810 / release FUN_004c0830), and a file that doesn't fit = no texture
//    ("Failed to allocate memory for file" goes to a stubbed print, nothing shows). The heap is made bigger by the largest mod file (rounded up to 1 MB,
//    capped at 64 MB) - counting switched-off folders too, so a mod switched on in the MODS menu fits without a
//    restart; a bigger file is reported as [fail]. With the plugin ds2fix.memory installed (PSE build,
//    games/ds2/plugins/memory) that plugin owns the heap's size instead: its hook on the same call runs in front of
//    ours, makes the heap itself (cap 64 MB / 256 MB with 4 GB) and reads texture files too big for it outside it; ours
//    then passes the call through. Without it a plugin may add bytes in front of us: we grow on top of what arrives.
// Not covered: files the game fopens itself (Bink films Data\EMovies, a few writers / streams), level DLLs.
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
#include "core/settings.h"
#include "features/features.h"
#ifdef DS2_PSE_HOST
#include "core/pse_host.h"
#include "loader.h"
#include "pse/ds2fix_memory.h"
#endif

namespace {
constexpr uint32_t kResolve = 0x4C3C40;
constexpr uint8_t kResolveEntry[] = {0xA0, 0x4C, 0x5E, 0x66, 0x00};             // mov al, [0x665E4C]
constexpr uint32_t kOpen = 0x4C2EA0;
constexpr uint8_t kOpenEntry[] = {0x81, 0xEC, 0x08, 0x02, 0x00, 0x00};           // sub esp, 0x208
constexpr uint32_t kInArchive = 0x4C2810;
constexpr uint8_t kInArchiveEntry[] = {0x81, 0xEC, 0x08, 0x01, 0x00, 0x00};      // sub esp, 0x108
constexpr uint32_t kTextureLoad = 0x542F30;
constexpr uint8_t kTextureLoadEntry[] = {0x81, 0xEC, 0x24, 0x01, 0x00, 0x00};    // sub esp, 0x124
constexpr uint32_t kHeapCtorCall = 0x4136FC, kHeapCtor = 0x4C0650;
constexpr uint32_t kFileFlags = 0x665E4C;   // bit 0 archive mode, bit 1 archive build
constexpr uint32_t kPathBuffer = 0x67CFA8;
constexpr size_t kPathBufferSize = 0x200;
constexpr uint32_t kHeapSize = 1024000;     // the game's Global Heap
constexpr uint32_t kHeapCap = 64u << 20;    // most we add to it
constexpr uint32_t kHeapGlobal = 0x664380;  // the heap object (+0x48 size)

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
std::string g_builtRoot;   // files built from the player's install (mods_levels.cpp, AddBuiltModFile)
std::vector<std::string> g_extraRoots;   // folders of files built by mods_tables.cpp (AddModRoot)
bool ExtraRoot(const char* path) {
    for (const std::string& r : g_extraRoots)
        if (_strnicmp(path, r.c_str(), r.size()) == 0) return true;
    return false;
}
uint32_t g_largestFile = 0;      // largest file the heap is sized for (every folder, on or off)
std::string g_largestName;
bool g_heapMade = false;
// Every name the resolver was asked for, with whether it was asked outside a mission (start-up / front-end: read once,
// kept - ReloadMods says "restart" for those) or by a mission (levels read their files again at every load).
std::unordered_map<std::string, bool> g_opened;   // upper-case file name -> asked outside a mission

std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// ---- mod report ----
// Release builds have no log, so what the mods did ("served" lines, which mod won a name, "[fail]" lines) goes to
// Mods\DesertStorm2Fix-report.txt, written afresh at every start. A file at the top of Mods is not a mod.
std::string g_reportPath;
int g_problems = 0;

void BeginReport() {
    const DWORD attr = GetFileAttributesA(g_modsRoot.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;  // no Mods folder, no report
    g_reportPath = g_modsRoot + "\\DesertStorm2Fix-report.txt";
    HANDLE f = CreateFileA(g_reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        g_reportPath.clear();
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[320];
    const int n = snprintf(head, sizeof head,
                           "DesertStorm2Fix " DS_VERSION " - mod report, %04d-%02d-%02d %02d:%02d:%02d\r\n"
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
        if (!index) {
            const uint32_t size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
            if (SizesHeap(key) && size > g_largestFile) {
                g_largestFile = size;
                g_largestName = fd.cFileName;
            }
            continue;
        }
        if (path.size() >= kPathBufferSize) {
            features::ModLog("[fail] Mods: %s - path longer than %u characters, not used", path.c_str(),
                             static_cast<unsigned>(kPathBufferSize - 1));
            continue;
        }
        const uint32_t size = fd.nFileSizeHigh ? 0xFFFFFFFFu : fd.nFileSizeLow;
        if (SizesHeap(key) && size > g_largestFile) {
            g_largestFile = size;
            g_largestName = fd.cFileName;
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
        if (!g_heapMade) ScanFolder(g_modsRoot + "\\" + name, -1, ignored, false);
    }
}

// In a mission = the world's scene name ([0x68008C] +0x1BBC) is a mission's: set early in mission init (0x41279C, the
// name buffer 0x62F7E0) before the level's files are read, "FrontEnd" (0x5DFD74) by the front-end init (0x42C94F),
// none during start-up. (The front-end state [0x63433C] reads 0x13 = "mission running" at start-up too.)
bool InMission() {
    const auto* world = *reinterpret_cast<const uint8_t* const*>(0x68008C);
    const char* scene = world ? *reinterpret_cast<const char* const*>(world + 0x1BBC) : nullptr;
    return scene && *scene && _stricmp(scene, "FrontEnd") != 0;
}

void NoteOpened(const char* name) {
    if (!name || !*name) return;
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    const std::string key = Upper(base);
    const bool outside = !InMission();
    auto it = g_opened.find(key);
#ifndef DS_DIST
    static const bool logOpens = [] {   // Dev\ModsOpenLog = 1: every name the resolver is asked for, once
        DWORD v = 0, size = sizeof v;
        return RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "ModsOpenLog", RRF_RT_REG_DWORD,
                            nullptr, &v, &size) == ERROR_SUCCESS && v;
    }();
    if (logOpens && it == g_opened.end())
        dslog::Write("[dev] Mods: asked for %s%s", key.c_str(), outside ? "" : " (mission)");
#endif
    if (it == g_opened.end()) g_opened.emplace(key, outside);
    else if (outside) it->second = true;
}

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
    if (!path || (!StartsWith(path, g_modsRoot) && !StartsWith(path, g_builtRoot) && !ExtraRoot(path)))
        return g_open(file, path);
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

// ds2fix.memory (games/ds2/plugins/memory) when it is installed and its heap hook went in - it then sizes the heap.
#ifdef DS2_PSE_HOST
const Ds2FixMemoryV1* MemoryPlugin() {
    const PSEMessagingInterface* m = pse::MessagingInterface();
    const auto* t = m ? static_cast<const Ds2FixMemoryV1*>(m->QueryInterface(pse_host::Core(), DS2FIX_MEMORY_INTERFACE, 1))
                      : nullptr;
    return t && t->size >= sizeof(Ds2FixMemoryV1) ? t : nullptr;
}
uint32_t MemoryCap() { return MemoryPlugin() ? MemoryPlugin()->HeapCap() : kHeapCap; }
bool MemoryLoadsBigTextures() { return MemoryPlugin() && MemoryPlugin()->LoadsBigTexturesOutsideHeap(); }
#else
bool MemoryPlugin() { return false; }
uint32_t MemoryCap() { return kHeapCap; }
bool MemoryLoadsBigTextures() { return false; }
#endif

// The heap's size now (0 before it is made), whoever sized it.
uint32_t HeapSizeNow() {
    const auto* heap = *reinterpret_cast<const uint8_t* const*>(kHeapGlobal);
    return g_heapMade && heap ? *reinterpret_cast<const uint32_t*>(heap + 0x48) : 0;
}

// Global Heap: 1 024 000 bytes + the largest mod file, so a mod's big texture fits (see the top of the file).
void* __fastcall HeapCtor(void* self, void* /*edx*/, uint32_t size, const char* name) {
    uint32_t want = size;
    g_heapMade = true;
    if (MemoryPlugin()) {   // ds2fix.memory made its own choice in front of us (it reports what it did)
        features::ModLog("Mods: file heap sized by ds2fix.memory");
        return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, const char*)>(kHeapCtor)(self, size, name);
    }
    if (g_largestFile > kHeapCap)
        features::ModLog("[fail] Mods: %s is %u bytes - files the game reads whole (textures, models) can be at most %u MB; "
                         "the game would show \"Failed to allocate memory\" for it", g_largestName.c_str(), g_largestFile,
                         kHeapCap >> 20);
    if (size >= kHeapSize && g_largestFile > 256 * 1024) {
        const uint32_t extra = static_cast<uint32_t>(
            std::min<uint64_t>((uint64_t{g_largestFile} + 0xFFFFF) & ~uint64_t{0xFFFFF}, kHeapCap));
        want = size + extra;
        features::ModLog("Mods: file heap %u -> %u bytes (largest mod file %s, %u bytes)", size, want,
                         g_largestName.c_str(), g_largestFile);
    }
    return reinterpret_cast<void*(__thiscall*)(void*, uint32_t, const char*)>(kHeapCtor)(self, want, name);
}

// Entry detour: the broker's HookFunction in the PSE host build; standalone, the prologue (whole instructions, no
// relative branches - checked by the expected bytes) moves into a small trampoline that jumps back after it.
template <class Fn>
bool HookEntry(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
#ifdef DS2_PSE_HOST
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
#else
    if (!patch::Matches(entry, prologue, len)) {
        dslog::Write("[fail] Mods: unexpected bytes at 0x%08X", entry);
        return false;
    }
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return false;
    memcpy(tramp, prologue, len);
    tramp[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(entry + len - (reinterpret_cast<uint32_t>(tramp) + len + 5));
    memcpy(tramp + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), tramp, len + 5);
    next = reinterpret_cast<Fn>(tramp);
    return patch::WriteJump(entry, fn);
#endif
}
}  // namespace

void features::ApplyMods() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!settings::Get().featMods) {
        dslog::Write("[off]  Mods: Mods folder ignored (FeatMods)");
        return;
    }
    g_modsRoot = GameDir() + "\\Mods";
    BeginReport();
    BuildIndex();
    BuildModReferences();   // mods_levels.cpp: .uses / .envpatch / .rmdpatch / .dllpatch from the player's install
    BuildModTables();       // mods_tables.cpp: merged .txt tables, .strings, .weapon / .vehicle / .unit
    // (the hooks go in with no mods too: the MODS menu can switch one on while the game runs)
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
const std::vector<std::string>& features::LoadedMods() { return g_mods; }
bool features::ModsFolderEnabled() { return settings::Get().featMods; }

// MODS menu Apply: the folders as they are now (the menu renamed them) -> new index + a fresh report. Files whose
// source changed (added / removed / another mod's copy) that the game already asked for outside a mission were read
// at start-up / by the front-end and stay as they are until the game starts again; mission files are read again at
// the next level load.
features::ModReload features::ReloadMods() {
    ModReload r;
    if (!settings::Get().featMods || g_modsRoot.empty()) return r;
    std::unordered_map<std::string, std::string> before;
    for (const auto& [key, f] : g_files) before.emplace(key, f.path);
    g_files.clear();
    g_mods.clear();
    g_problems = 0;
    const uint32_t heapLargest = g_largestFile;
    g_largestFile = 0;
    g_largestName.clear();
    BeginReport();
    ModLog("Mods: index rebuilt from the MODS menu");
    BuildIndex();
    BuildModReferences();   // mods_levels.cpp: .uses / .envpatch / .rmdpatch / .dllpatch from the player's install
    BuildModTables();       // mods_tables.cpp: merged .txt tables, .strings, .weapon / .vehicle / .unit
    r.ok = true;
    std::vector<std::string> changed;
    for (const auto& [key, path] : before) {
        auto it = g_files.find(key);
        if (it == g_files.end() || _stricmp(it->second.path.c_str(), path.c_str()) != 0) changed.push_back(key);
    }
    for (const auto& [key, f] : g_files)
        if (!before.count(key)) changed.push_back(key);
    r.changed = static_cast<int>(changed.size());
    for (const std::string& key : changed) {
        auto it = g_opened.find(key);
        if (it == g_opened.end()) continue;
        if (it->second) {
            if (!r.restart++) r.firstRestart = key;
            ModLog("Mods: %s was read at start-up / by the menus - the change shows after a restart", key.c_str());
        } else {
            ++r.nextLevel;
        }
    }
    // the heap was sized at start-up (all folders); a bigger file now needs a restart - with ds2fix.memory only a file
    // that isn't a texture (its texture reads go outside the heap when they don't fit)
    uint32_t largest = 0;
    std::string largestKey;
    for (const auto& [key, f] : g_files)
        if (SizesHeap(key) && f.size > largest) largest = f.size, largestKey = key;
    const uint32_t cap = MemoryCap();
    const uint32_t heapExtra = HeapSizeNow() > kHeapSize ? HeapSizeNow() - kHeapSize : 0;
    static const char* const kTextures[] = {".DDS", ".TGA", ".PNG", ".BMP", ".DDL"};
    bool texture = false;
    for (const char* e : kTextures)
        texture = texture || (largestKey.size() > 4 && largestKey.compare(largestKey.size() - 4, 4, e) == 0);
    if (MemoryLoadsBigTextures() && texture) {
        // fine at any size the address space allows
    } else if (largest > cap) {
        ModLog("[fail] Mods: a %u-byte file - files the game reads whole (textures, models) can be at most %u MB", largest,
               cap >> 20);
    } else if (g_heapMade && largest > 256 * 1024 && largest > heapExtra) {
        ++r.restart;
        if (r.firstRestart.empty()) r.firstRestart = "FILE HEAP";
        ModLog("Mods: a %u-byte file needs a bigger file heap (%u extra at start-up) - restart the game", largest, heapExtra);
    }
    g_largestFile = std::max(heapLargest, g_largestFile);
    ModLog("[ok]   Mods: %zu mod(s), %zu file(s) - %d file(s) changed, %d after a restart", g_mods.size(), g_files.size(),
           r.changed, r.restart);
    ReloadSkins();
    ReloadModSounds();
    return r;
}
int features::ModProblems() { return g_problems; }

std::vector<features::ModFileRef> features::ModFilesWithExtension(const char* ext) {
    std::vector<ModFileRef> out;
    const std::string want = Upper(ext);
    for (const auto& [key, f] : g_files)
        if (key.size() > want.size() && key.compare(key.size() - want.size(), want.size(), want) == 0)
            out.push_back({f.path, f.mod >= 0 && f.mod < static_cast<int>(g_mods.size()) ? g_mods[f.mod] : std::string()});
    std::sort(out.begin(), out.end(), [](const ModFileRef& a, const ModFileRef& b) {
        return _stricmp(a.mod.c_str(), b.mod.c_str()) != 0 ? _stricmp(a.mod.c_str(), b.mod.c_str()) < 0
                                                          : _stricmp(a.path.c_str(), b.path.c_str()) < 0;
    });
    return out;
}

// mods_levels.cpp: a file built from the player's own install for a mod (a .uses / .envpatch / .dllpatch result) -
// indexed like that mod's file unless a mod ships the name itself.
bool features::AddBuiltModFile(const std::string& name, const std::string& path, const std::string& mod) {
    const std::string key = Upper(name);
    if (g_files.count(key)) return false;
    int index = -1;
    for (size_t i = 0; i < g_mods.size(); ++i)
        if (_stricmp(g_mods[i].c_str(), mod.c_str()) == 0) index = static_cast<int>(i);
    if (index < 0 || path.size() >= kPathBufferSize) return false;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const uint32_t size = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) ? a.nFileSizeLow : 0;
    g_files[key] = {path, index, size};
    g_builtRoot = path.substr(0, path.find_last_of("\\/") + 1);
    if (SizesHeap(key) && size > g_largestFile) {
        g_largestFile = size;
        g_largestName = name;
    }
    return true;
}

// mods_tables.cpp: a file it built (merged table, retextured model) - indexed for `name`, replacing a mod's own file of
// that name (a mod's Weaps.txt is one part of the merged Weaps.txt); `mod` = index in LoadedMods().
void features::SetModFile(const std::string& name, const std::string& path, int mod) {
    if (path.size() >= kPathBufferSize || g_mods.empty()) return;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const uint32_t size = GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a) ? a.nFileSizeLow : 0;
    g_files[Upper(name)] = {path, std::clamp(mod, 0, static_cast<int>(g_mods.size()) - 1), size};
}
void features::AddModRoot(const std::string& folder) {
    for (const std::string& r : g_extraRoots)
        if (_stricmp(r.c_str(), folder.c_str()) == 0) return;
    g_extraRoots.push_back(folder);
}

std::string features::ModFilePath(const char* name) {
    const ModFile* f = Find(name);
    return f ? f->path : std::string();
}

bool features::ModHasTexture(const std::string& name) {
    if (Find(name.c_str())) return true;
    for (const char* ext : {".dds", ".tga", ".png", ".bmp", ".ddl"})
        if (Find((name + ext).c_str())) return true;
    return false;
}

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
