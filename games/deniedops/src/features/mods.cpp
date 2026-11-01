// Mods folder (2026-10-25): a file with the name of a file the game reads, dropped into a folder under <game>\Mods\,
// is read instead of the archive's copy; a file only a mod has passes the game's exists checks. Same shape as
// GreatEscapeFix / DesertStormFix (games/tge/src/features/mods.cpp), at Denied Ops' own addresses (gamelib3 Files.cpp).
//
// Layout: <game>\Mods\<mod name>\...any sub folders...\<FILE.EXT>; mods in name order, a later mod wins a name (logged);
// folders starting with '_' or '.' are off. Index = upper-case file name, path stripped, built at start-up. Setting
// FeatMods (default on). What happened goes to Mods\DeniedOpsFix-report.txt (every build).
//
// How ConflictDeniedOps.exe reads files (Steamless copy, verified on the packed exe after the stub - unpack.cpp):
//  - flags [0xBE0674]: bit 0 = archive mode, always on in the shipped game (the start-up parser sets 'd'); bit 0x20
//    cleared by every open / close.
//  - two file streams, pointers at 0xBD4AE8[2] (0x40+ bytes): +0x08 buffers, +0x10 bytes in buffer, +0x14 buffer
//    index, +0x18 buffer size, +0x1C file position of the buffer, +0x20 base (archive entry offset), +0x24 file
//    position, +0x28 read position, +0x2C size (0 = stream free, 0x7FFFFFFF = ask the file), +0x30 FILE* used in
//    archive mode (= the current archive's FILE, archive +0x11C), +0x34 FILE* used on disk. EVERY read / seek / size /
//    close tests the global bit 0 to pick +0x30 or +0x34 - so a loose file can't be "opened from disk" for one call
//    the way DS1 does it; instead the stream is pointed at our own FILE as if it were an archive entry at offset 0.
//  - File_Open 0x5E24B0 thiscall(stream, name) ret 4: archive mode = upper-cased bare name hashed (0x5E21D0 +
//    0x6337A0) and looked up in the current archive's index (0x5E23B0), +0x20 / +0x2C from the entry; disk = fopen.
//    Only caller: the stream opener 0x5E2F40 (from the resolver path 0x5E3EE0, the package loader 0x637C10 ...).
//  - File_Exists 0x5E2900 cdecl(name) -> 1 / 0: archive index lookup (the resolver 0x5E3CA0 asks it first in archive
//    mode) - answers 1 for a mod's file.
//  - File_Close 0x5E3960 thiscall(stream): archive mode reads the rest, +0x2C = 0 (free); disk mode fcloses +0x34.
//    After it our FILE is closed and the stream's +0x30 handed back.
//  - FILE* belong to the game's CRT (MSVCR80.dll): opened / sized / closed through the exe's own import slots.
// Not covered: files the game fopens itself (films, saves, Operations.xml / profile writers), the async streamer.
//
// Octree for a mod's scene (2026-11-01, DS1 levels on the DO engine): Level_Init 0x441820 loads <scene>.PC.OCT unless
// the start-up value of flag 'd' (0x633960 cdecl(letter) -> byte, the table at 0xC45308) is '2' - then (0x4425E7) it
// builds every zone's octree in memory (0x82A860 thiscall([0xE558C4], 0)). The call at 0x4425DF answers '2' when the
// scene index (*.ENV) this level opened came from a mod: a shipped .PC.OCT indexes the shipped world, not the mod's.
// A mod's <SCENE>.level sidecar with a "--@script none" line (DS1 levels on a shipped level's slot) also clears the
// mission script object [0x9F2B3C] there - after the DLL's init has loaded the scene, before the first update: the
// shipped script would look its own objects up by id in a world that doesn't have them (every caller tests it for 0).
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
constexpr uint32_t kOpen = 0x5E24B0;
constexpr uint8_t kOpenEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x08, 0x01, 0x00, 0x00};   // mov eax,[esp+4]; sub esp,0x108
constexpr uint32_t kExists = 0x5E2900;
constexpr uint8_t kExistsEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x8D, 0x94, 0x24, 0xFC, 0xFE, 0xFF, 0xFF};
constexpr uint32_t kClose = 0x5E3960;
constexpr uint8_t kCloseEntry[] = {0x8B, 0x15, 0x74, 0x06, 0xBE, 0x00};                            // mov edx,[0xBE0674]
constexpr uint32_t kFileFlags = 0xBE0674;
constexpr uint32_t kFopenSlot = 0x90E388, kFcloseSlot = 0x90E38C, kFseekSlot = 0x90E1E0, kFtellSlot = 0x90E1E4;
constexpr uint32_t kOctreeFlagCall = 0x4425DF, kFlagValue = 0x633960;   // Level_Init: push 'd'; call
constexpr uint32_t kScriptObject = 0x9F2B3C;
constexpr size_t kNameMax = 0x100;   // File_Open copies the name into a 0x108-byte stack buffer

using OpenFn = int(__thiscall*)(void*, const char*);
using ExistsFn = int(__cdecl*)(const char*);
using CloseFn = void(__thiscall*)(void*);
OpenFn g_open = nullptr;
ExistsFn g_exists = nullptr;
CloseFn g_close = nullptr;

struct ModFile {
    std::string path;
    int mod;
};
std::unordered_map<std::string, ModFile> g_files;
std::vector<std::string> g_mods;
std::string g_modsRoot;
std::string g_reportPath;

struct Swapped {
    void* file;     // our FILE*
    uint32_t prev;  // the stream's +0x30 before
};
std::unordered_map<uint8_t*, Swapped> g_swapped;

template <class T>
T Crt(uint32_t slot) {
    return reinterpret_cast<T>(*reinterpret_cast<void**>(slot));
}
void* CrtOpen(const char* path) { return Crt<void*(__cdecl*)(const char*, const char*)>(kFopenSlot)(path, "rb"); }
int CrtClose(void* f) { return Crt<int(__cdecl*)(void*)>(kFcloseSlot)(f); }
int CrtSeek(void* f, long off, int origin) { return Crt<int(__cdecl*)(void*, long, int)>(kFseekSlot)(f, off, origin); }
long CrtTell(void* f) { return Crt<long(__cdecl*)(void*)>(kFtellSlot)(f); }

std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

void BeginReport() {
    const DWORD attr = GetFileAttributesA(g_modsRoot.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    g_reportPath = g_modsRoot + "\\DeniedOpsFix-report.txt";
    HANDLE f = CreateFileA(g_reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        g_reportPath.clear();
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[320];
    const int n = snprintf(head, sizeof head,
                           "DeniedOpsFix " DS_VERSION " - mod report, %04d-%02d-%02d %02d:%02d:%02d\r\n"
                           "Written when the game starts; \"served\" lines are added while it runs. Lines starting "
                           "with [fail] are problems.\r\n\r\n",
                           t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    DWORD written = 0;
    WriteFile(f, head, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
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
        if (fd.nFileSizeHigh || !fd.nFileSizeLow) {
            features::ModLog("[fail] Mods: %s - empty or over 4 GB, not used", path.c_str());
            continue;
        }
        const std::string key = Upper(fd.cFileName);
        if (key.size() >= kNameMax) {
            features::ModLog("[fail] Mods: %s - file name too long, not used", path.c_str());
            continue;
        }
        if (key == "DENIEDOPSFIX-REPORT.TXT" || key == "CAMPAIGN.TXT") continue;   // campaign.txt: campaigns.cpp
        auto it = g_files.find(key);
        if (it != g_files.end())
            features::ModLog("Mods: %s replaces %s's %s", g_mods[mod].c_str(), g_mods[it->second.mod].c_str(), fd.cFileName);
        g_files[key] = {path, mod};
        ++count;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void BuildIndex() {
    g_files.clear();
    g_mods.clear();
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
    for (const std::string& name : names) {
        const int mod = static_cast<int>(g_mods.size());
        g_mods.push_back(name);
        int count = 0;
        ScanFolder(g_modsRoot + "\\" + name, mod, count);
        features::ModLog("Mods: %s - %d file(s)", name.c_str(), count);
    }
    for (const std::string& name : off) features::ModLog("Mods: %s - switched off (folder name starts with %c)", name.c_str(), name[0]);
}

const ModFile* Find(const char* name) {
    if (!name || !*name || g_files.empty() || strchr(name, ':')) return nullptr;
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    auto it = g_files.find(Upper(base));
    return it == g_files.end() ? nullptr : &it->second;
}

void NoteServed(const char* name, const ModFile& f) {
    static std::unordered_map<std::string, bool> logged;
    if (logged[f.path]) return;
    logged[f.path] = true;
    features::ModLog("Mods: served %s from %s (%s)", name, g_mods[f.mod].c_str(), f.path.c_str());
}

template <class T>
T& At(uint8_t* p, uint32_t off) {
    return *reinterpret_cast<T*>(p + off);
}

#ifndef DS_DIST
// Dev\ModsOpenLog = 1: every name File_Open is asked for, once (what a mod can replace).
void NoteOpened(const char* name) {
    static const bool on = [] {
        DWORD v = 0, size = sizeof v;
        return RegGetValueA(HKEY_CURRENT_USER, "Software\\DeniedOpsFix\\Dev", "ModsOpenLog", RRF_RT_REG_DWORD, nullptr, &v,
                            &size) == ERROR_SUCCESS && v;
    }();
    if (!on || !name) return;
    static std::unordered_map<std::string, bool> seen;
    const std::string key = Upper(name);
    if (seen[key]) return;
    seen[key] = true;
    dslog::Write("[dev] Mods: open %s", key.c_str());
}
#else
void NoteOpened(const char*) {}
#endif

bool g_sceneFromMod = false;   // the last scene index (*.ENV) opened was a mod's
std::string g_sceneName;       // ... and its bare name ("M_BAS.ENV")

uint32_t g_sceneGeneration = 0;   // + 1 per scene index opened (a level load)

bool SceneWithoutScript() { return !_stricmp(features::SceneLevelOption("script").c_str(), "none"); }

bool IsSceneIndex(const char* name) {
    const size_t n = name ? strlen(name) : 0;
    return n > 4 && _stricmp(name + n - 4, ".ENV") == 0;
}

// Lighting of a mod's scene (research DeniedOps_tools\lighting\LIGHTING.md): an instance gets lights only from the
// .LGT, HLO records or a copy of its zone's list; with lighting on and an EMPTY list the light-constant upload 0x627DC0
// builds one directional light of diffuse 0 and ambient 0 (the three fldz at 0x62812B / 0x628133 / 0x62813B) - black.
// Ported levels (DS1 ...) bake their lighting into vertex colours: for a mod's scene that default becomes ambient
// (1,1,1) = texture x vertex colour, the source game's own look; shipped scenes keep the game's (every shipped instance
// has a light, so nothing there changes either way).
const uint8_t kFldz[] = {0xD9, 0xEE};
const uint8_t kFld1[] = {0xD9, 0xE8};
const BytePatch kSceneLight[] = {
    {DS_PATCH_NAME("empty light list: ambient r = 1"), 0x62812B, kFldz, kFld1, 2, false},
    {DS_PATCH_NAME("empty light list: ambient g = 1"), 0x628133, kFldz, kFld1, 2, false},
    {DS_PATCH_NAME("empty light list: ambient b = 1"), 0x62813B, kFldz, kFld1, 2, false},
};

void SceneLight(bool modScene) {
    static int state = -1;
    if (state == static_cast<int>(modScene)) return;
    state = modScene;
    if (modScene ? patch::ApplyGroup("SCENE_LIGHT", kSceneLight) : patch::RevertGroup("SCENE_LIGHT", kSceneLight))
        dslog::Write("Mods: unlit instances %s", modScene ? "take white ambient (a mod's scene)" : "back to the game's black");
    else
        dslog::Write("[fail] Mods: light constant code at 0x%08X not recognised", kSceneLight[0].va);
}

int __cdecl OctreeFlag(int letter) {
    const char v = reinterpret_cast<char(__cdecl*)(int)>(kFlagValue)(letter);
    SceneLight(g_sceneFromMod);
    if (!g_sceneFromMod) return v;
    dslog::Write("Mods: the scene comes from a mod - its octree is built at load (not the shipped .PC.OCT)");
    if (SceneWithoutScript() && *reinterpret_cast<void**>(kScriptObject)) {
        *reinterpret_cast<void**>(kScriptObject) = nullptr;
        features::ModLog("Mods: %s runs without its mission script (--@script none)", g_sceneName.c_str());
    }
    return '2';
}

int __fastcall Open(uint8_t* stream, void* /*edx*/, const char* name) {
    NoteOpened(name);
    const ModFile* m = Find(name);
    if (IsSceneIndex(name)) {
        g_sceneFromMod = m != nullptr;
        const char* base = name;
        for (const char* p = name; *p; ++p)
            if (*p == '\\' || *p == '/') base = p + 1;
        g_sceneName = Upper(base);
        ++g_sceneGeneration;
    }
    if (!m) return g_open(stream, name);
    auto& flags = *reinterpret_cast<uint32_t*>(kFileFlags);
    if (!(flags & 1)) {   // disk mode (never in the shipped game): the game's own fopen of the mod's path
        const int r = g_open(stream, m->path.c_str());
        if (r) NoteServed(name, *m);
        return r;
    }
    void* f = CrtOpen(m->path.c_str());
    if (!f) {
        features::ModLog("[fail] Mods: can't open %s - the game's copy is used", m->path.c_str());
        return g_open(stream, name);
    }
    long size = 0;
    if (CrtSeek(f, 0, SEEK_END) == 0) size = CrtTell(f);
    CrtSeek(f, 0, SEEK_SET);
    if (size <= 0) {
        CrtClose(f);
        return g_open(stream, name);
    }
    flags &= ~0x20u;
    auto it = g_swapped.find(stream);   // a stream left swapped (the game reused it without closing): close ours
    if (it != g_swapped.end()) {
        CrtClose(it->second.file);
        At<uint32_t>(stream, 0x30) = it->second.prev;
        g_swapped.erase(it);
    }
    g_swapped[stream] = {f, At<uint32_t>(stream, 0x30)};
    At<void*>(stream, 0x30) = f;            // archive-mode reads use this FILE ...
    At<uint32_t>(stream, 0x20) = 0;         // ... from offset 0
    At<uint32_t>(stream, 0x28) = 0;
    At<uint32_t>(stream, 0x2C) = static_cast<uint32_t>(size);
    At<uint32_t>(stream, 0x10) = 0;         // nothing buffered: the first read seeks and fills
    At<uint32_t>(stream, 0x14) = 0;
    At<uint32_t>(stream, 0x1C) = 0x7FFFFFFF;
    At<uint32_t>(stream, 0x24) = 0x7FFFFFFF;
    NoteServed(name, *m);
    return 1;
}

void __fastcall Close(uint8_t* stream, void* /*edx*/) {
    g_close(stream);
    auto it = g_swapped.find(stream);
    if (it == g_swapped.end()) return;
    const Swapped s = it->second;
    g_swapped.erase(it);
    if (At<void*>(stream, 0x30) == s.file) At<uint32_t>(stream, 0x30) = s.prev;   // an archive switch may have reset it
    At<uint32_t>(stream, 0x1C) = 0x7FFFFFFF;   // the archive's FILE position is unknown now: next read seeks
    At<uint32_t>(stream, 0x24) = 0x7FFFFFFF;
    At<uint32_t>(stream, 0x10) = 0;
    CrtClose(s.file);
}

int __cdecl Exists(const char* name) {
    if (Find(name)) return 1;
    return g_exists(name);
}

template <class Fn>
bool HookEntry(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
}

void Apply() {
    if (!HookEntry(kOpen, reinterpret_cast<const void*>(&Open), g_open, kOpenEntry, sizeof kOpenEntry) ||
        !HookEntry(kClose, reinterpret_cast<const void*>(&Close), g_close, kCloseEntry, sizeof kCloseEntry)) {
        features::ModLog("[fail] Mods: unexpected game code (file open / close) - mods not loaded");
        return;
    }
    if (!HookEntry(kExists, reinterpret_cast<const void*>(&Exists), g_exists, kExistsEntry, sizeof kExistsEntry))
        features::ModLog("[fail] Mods: exists check at 0x%08X not recognised - files only a mod has may be skipped", kExists);
    if (!patch::HookCall(kOctreeFlagCall, reinterpret_cast<const void*>(&OctreeFlag), kFlagValue))
        features::ModLog("[fail] Mods: Level_Init octree check at 0x%08X not recognised - a mod's scene uses the shipped "
                         ".PC.OCT", kOctreeFlagCall);
    features::ModLog("[ok]   Mods: %zu mod(s), %zu file(s)", g_mods.size(), g_files.size());
}
}  // namespace

void features::ApplyMods() {
    if (!Get().mods) {
        dslog::Write("[off]  Mods: Mods folder ignored (FeatMods)");
        return;
    }
    g_modsRoot = GameDir() + "\\Mods";
    BeginReport();
    BuildIndex();
    const DWORD attr = GetFileAttributesA(g_modsRoot.c_str());
    const bool folder = attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
    // With no Mods folder there is nothing to serve, now or after a MODS menu switch: the file code stays untouched.
    // A folder with every mod switched off still gets the hooks - the MODS menu can switch one on while the game runs.
    if (!folder) {
        dslog::Write("Mods: no Mods folder (%s)", g_modsRoot.c_str());
        return;
    }
    if (g_files.empty()) dslog::Write("Mods: no mods switched on (%s)", g_modsRoot.c_str());
    AfterUnpack(&Apply);
}

// The full path of the switched-on mods' file with this bare name ("" = no mod has it).
std::string features::ModFilePath(const char* name) {
    const ModFile* f = Find(name);
    return f ? f->path : std::string();
}

// A "--@<key> <value>" line of the current scene's <scene stem>.level in a mod ("" = none / the scene isn't a mod's).
std::string features::SceneLevelOption(const char* key) {
    if (!g_sceneFromMod || g_sceneName.empty()) return {};
    const std::string level = ModFilePath((g_sceneName.substr(0, g_sceneName.find_last_of('.')) + ".level").c_str());
    FILE* f = nullptr;
    if (level.empty() || fopen_s(&f, level.c_str(), "rb") || !f) return {};
    const std::string tag = std::string("--@") + key;
    std::string value;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        const char* p = line;
        while (*p == ' ' || *p == '\t') ++p;
        if (_strnicmp(p, tag.c_str(), tag.size()) || (p[tag.size()] != ' ' && p[tag.size()] != '\t')) continue;
        char word[64] = {};
        if (sscanf_s(p + tag.size(), " %63s", word, static_cast<unsigned>(sizeof word)) == 1) value = word;
    }
    fclose(f);
    return value;
}

uint32_t features::SceneGeneration() { return g_sceneGeneration; }

std::vector<features::ModFolder> features::ModList() {
    std::vector<ModFolder> out;
    if (g_modsRoot.empty()) return out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_modsRoot + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, ".."))
            continue;
        ModFolder m;
        m.folder = fd.cFileName;
        m.enabled = fd.cFileName[0] != '_' && fd.cFileName[0] != '.';
        m.name = m.folder;
        while (!m.name.empty() && (m.name[0] == '_' || m.name[0] == '.')) m.name.erase(0, 1);
        out.push_back(m);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    // By the bare name, so a folder keeps its place when the '_' prefix comes and goes.
    std::sort(out.begin(), out.end(), [](const ModFolder& a, const ModFolder& b) {
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

bool features::SetModEnabled(const std::string& folder, bool on) {
    if (g_modsRoot.empty() || folder.empty()) return false;
    std::string bare = folder;
    while (!bare.empty() && (bare[0] == '_' || bare[0] == '.')) bare.erase(0, 1);
    const std::string to = on ? bare : "_" + bare;
    if (to == folder) return true;
    const std::string from = g_modsRoot + "\\" + folder, dest = g_modsRoot + "\\" + to;
    if (!MoveFileA(from.c_str(), dest.c_str())) {
        features::ModLog("[fail] Mods: can't rename %s to %s (error %lu)", folder.c_str(), to.c_str(), GetLastError());
        return false;
    }
    features::ModLog("Mods: %s switched %s in the MODS menu", bare.c_str(), on ? "on" : "off");
    BuildIndex();   // the hooks read the index on every open: the change counts from the next file the game reads
    features::ModLog("Mods: %zu mod(s), %zu file(s) after the change", g_mods.size(), g_files.size());
    return true;
}

void features::ModLog(const char* fmt, ...) {
    char text[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof text, fmt, args);
    va_end(args);
    dslog::Write("%s", text);
    if (g_reportPath.empty()) return;
    HANDLE f = CreateFileA(g_reportPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    WriteFile(f, "\r\n", 2, &written, nullptr);
    CloseHandle(f);
}
