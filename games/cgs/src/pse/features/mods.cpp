// Loose-file mods for Conflict: Global Storm: a file in <game>\Mods\<mod name>\<anything>\<FILE> can replace or add a
// game file by NAME (case-insensitive basename match, any sub-folder). Mods apply in folder-name order, a later mod
// wins; folders starting with '_' or '.' are switched off.
//
// Two layers (2026-10-25):
//   1. Packed assets: the whole-file loader 0x52357C cdecl(name, void** buf, uint32_t* size, heap, flags) -> 1 / 0
//      is a plain wrapper (`push 1` + the five args) around 0x523447, whose first instruction jumps into the SecuROM
//      VM. Every asset the game reads whole goes through it by name (front end, live trace: 61 .dds textures, .fxo /
//      .vso shaders, Interface.gui, the 8 .gsc menu files, .txt tables; 53 call sites). Its entry is hooked: for a
//      name the mod index has, the loader is called with the mod file's FULL PATH while bit 0 of the file-system flags
//      byte 0x745D34 (archive mode, 1 in play - the DS1 / CV design) is cleared, so the game's own disk branch reads
//      the loose file and allocates the buffer exactly as for an archive entry (same heap argument, same free). The
//      bit is restored right after the call. Verified: a red C04_INT_opt0.dds (inside catalog.dat) turned the menu
//      bars / buttons red.
//   1b. Streamed assets: the stream open 0x52341B cdecl(name, flags) -> file object / 0 is a SecuROM thunk
//      (`jmp [0x117E678]` into the VM). Models open through it: the model loader 0x5E628E builds the .pi2 / .evo
//      names (0x5E4BB2) and opens them at 0x5E6346 - none reach the whole-file loader (Frida, 2026-10-28: a mod
//      C04_INTERFACE_GLOBE.evo was indexed but never served). The slot 0x117E678 is resolved on first use: the VM
//      stub it starts with writes the real target there after one call, 0x4599EB = plain code cdecl(name, flags) ->
//      0x522942(name, archive [0x74671C], flags). That function's entry is hooked (hooking the slot caught only the
//      first open): for a name the index has, the open runs with the mod file's full path and archive mode cleared,
//      as in 1.
//   2. Whole files opened by name with Win32 (archives, .sch sound caches, films, profile files): the kernel32
//      CreateFileA import slot 0x6C0138 redirects a READ open whose basename the index has. The VM calls real Win32,
//      so this catches what bypasses the loader.
//
// Not covered: a NEW file name the game first checks for with an exists-test inside the VM before loading (replacing
// an existing name always works); FindFirstFileA / FindNextFileA enumeration (left alone - duplicating the game's own
// directory merge risks files listed twice); any streamed read that opens an archive entry without the whole-file
// loader (none seen in the front end).
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kIatCreateFileA = 0x6C0138;
constexpr uint32_t kWholeFileLoad = 0x52357C;   // cdecl(name, void** buf, uint32_t* size, heap, flags) -> 1 / 0
constexpr uint8_t kWholeFileLoadBytes[] = {0x55, 0x8B, 0xEC, 0x6A, 0x01, 0xFF, 0x75, 0x18, 0xFF, 0x75, 0x14};
constexpr uint32_t kFileFlags = 0x745D34;       // byte, bit 0 = archive mode
constexpr uint32_t kStreamOpen = 0x4599EB;      // cdecl(name, flags) -> file object; the stream-open thunk's target
constexpr uint8_t kStreamOpenBytes[] = {0xFF, 0x74, 0x24, 0x08, 0xFF, 0x35, 0x1C, 0x67, 0x74, 0x00};

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_next = nullptr;
using LoadFn = int(__cdecl*)(const char*, void**, uint32_t*, void*, uint32_t);
LoadFn g_loadNext = nullptr;
using OpenFn = void*(__cdecl*)(const char*, int);
OpenFn g_openNext = nullptr;
std::unordered_set<std::string> g_logged;   // asset names already reported (shaders load many times)

struct ModFile {
    std::string path;
    int mod;
};
std::unordered_map<std::string, ModFile> g_files;  // upper-case base name -> file
std::vector<std::string> g_mods;
std::string g_modsRoot;
std::string g_reportPath;

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string BaseName(const char* path) {
    if (!path) return "";
    std::string s = path;
    size_t slash = s.find_last_of("\\/");
    return Upper(slash == std::string::npos ? s : s.substr(slash + 1));
}

void IndexFolder(const std::string& root, const std::string& dir, int mod) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        std::string path = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            IndexFolder(root, path, mod);
        } else {
            auto& slot = g_files[Upper(fd.cFileName)];
            if (mod >= slot.mod || slot.path.empty()) slot = {path, mod};
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

void BuildIndex() {
    g_files.clear();
    g_mods.clear();
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    size_t slash = dir.find_last_of("\\/");
    dir = slash == std::string::npos ? "." : dir.substr(0, slash);
    g_modsRoot = dir + "\\Mods";
    g_reportPath = g_modsRoot + "\\GlobalStormFix-report.txt";
    HANDLE f = CreateFileA(g_reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_modsRoot + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        features::ModLog("Mods: no Mods folder (%s)", g_modsRoot.c_str());
        return;
    }
    std::vector<std::string> names;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        if (fd.cFileName[0] == '_' || fd.cFileName[0] == '.') continue;
        names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end());
    for (size_t i = 0; i < names.size(); ++i) {
        g_mods.push_back(names[i]);
        IndexFolder(g_modsRoot, g_modsRoot + "\\" + names[i], static_cast<int>(i));
    }
    g_logged.clear();
    features::ModLog("Mods: %zu mod(s), %zu file(s) indexed by name (packed assets via the whole-file loader and the "
                     "stream open, other files via CreateFileA)", g_mods.size(), g_files.size());
}

int __cdecl ModLoad(const char* name, void** buf, uint32_t* size, void* heap, uint32_t flags) {
    if (name) {
        std::string base = BaseName(name);
        auto it = g_files.find(base);
        if (it != g_files.end()) {
            volatile uint8_t* archiveMode = reinterpret_cast<volatile uint8_t*>(kFileFlags);
            uint8_t old = *archiveMode;
            *archiveMode = static_cast<uint8_t>(old & ~1u);
            int ok = g_loadNext(it->second.path.c_str(), buf, size, heap, flags);
            *archiveMode = old;
            if (ok) {
                if (g_logged.insert(base).second) features::ModLog("Mods: %s -> %s", name, it->second.path.c_str());
                return ok;
            }
            features::ModLog("[fail] Mods: %s could not be read (%s) - the game's own copy is used", name,
                             it->second.path.c_str());
        }
    }
    return g_loadNext(name, buf, size, heap, flags);
}

void* __cdecl ModOpen(const char* name, int flags) {
    if (name) {
        std::string base = BaseName(name);
        auto it = g_files.find(base);
        if (it != g_files.end()) {
            volatile uint8_t* archiveMode = reinterpret_cast<volatile uint8_t*>(kFileFlags);
            uint8_t old = *archiveMode;
            *archiveMode = static_cast<uint8_t>(old & ~1u);
            void* file = g_openNext(it->second.path.c_str(), flags);
            *archiveMode = old;
            if (file) {
                if (g_logged.insert(base).second) features::ModLog("Mods: %s -> %s", name, it->second.path.c_str());
                return file;
            }
            features::ModLog("[fail] Mods: %s could not be opened (%s) - the game's own copy is used", name,
                             it->second.path.c_str());
        }
    }
    return g_openNext(name, flags);
}

HANDLE WINAPI ModCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                             DWORD flags, HANDLE tmpl) {
    if (path && disposition == OPEN_EXISTING && !(access & GENERIC_WRITE)) {
        auto it = g_files.find(BaseName(path));
        if (it != g_files.end() && _stricmp(path, it->second.path.c_str()) != 0) {   // not the mod file itself
            HANDLE h = g_next(it->second.path.c_str(), access, share, sa, disposition, flags, tmpl);
            if (h != INVALID_HANDLE_VALUE) {
                features::ModLog("Mods: %s -> %s", path, it->second.path.c_str());
                return h;
            }
            // fall through to the original path (the mod's copy is missing/unreadable)
        }
    }
    return g_next(path, access, share, sa, disposition, flags, tmpl);
}
}  // namespace

void features::ApplyMods() {
    if (!features::Get().mods) return;
    BuildIndex();
    patch::HookSlot(kIatCreateFileA, reinterpret_cast<void*>(&ModCreateFileA), reinterpret_cast<void**>(&g_next));
    if (!patch::HookFunction(kWholeFileLoad, reinterpret_cast<void*>(&ModLoad), reinterpret_cast<void**>(&g_loadNext),
                             kWholeFileLoadBytes, sizeof kWholeFileLoadBytes))
        features::ModLog("[fail] Mods: whole-file loader not hooked - packed assets can't be replaced (whole files "
                         "still can)");
    if (!patch::HookFunction(kStreamOpen, reinterpret_cast<void*>(&ModOpen), reinterpret_cast<void**>(&g_openNext),
                             kStreamOpenBytes, sizeof kStreamOpenBytes))
        features::ModLog("[fail] Mods: stream open not hooked - models (.evo / .pi2) can't be replaced");
}

int features::ModCount() { return static_cast<int>(g_mods.size()); }

// For the mods menu (modsmenu.cpp): every folder directly under Mods\, enabled or not (a '_'/'.'-prefixed folder is
// off). BuildIndex() itself only ever sees enabled folders, so this is a second, unfiltered scan - the menu is the
// only caller, kept here because it's the same root path g_modsRoot already resolved.
std::vector<std::pair<std::string, bool>> features::ModList() {
    std::vector<std::pair<std::string, bool>> out;
    if (g_modsRoot.empty()) return out;   // ApplyMods() hasn't run (FeatMods off) - nothing to list
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((g_modsRoot + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
        bool off = fd.cFileName[0] == '_' || fd.cFileName[0] == '.';
        out.emplace_back(fd.cFileName, !off);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

// Renames <root>\<folderName> on (strips a leading '_') or off (adds one) and re-scans, so the CreateFileA redirect
// reflects it without a relaunch. folderName is exactly what ModList() returned (may already carry the '_'/'.').
bool features::SetModEnabled(const std::string& folderName, bool enable) {
    if (g_modsRoot.empty() || folderName.empty()) return false;
    std::string from = g_modsRoot + "\\" + folderName;
    std::string bare = folderName;
    while (!bare.empty() && (bare[0] == '_' || bare[0] == '.')) bare.erase(0, 1);
    std::string to = g_modsRoot + "\\" + (enable ? bare : ("_" + bare));
    bool ok = true;
    if (from != to) {
        ok = MoveFileA(from.c_str(), to.c_str()) != 0;
        if (!ok) features::ModLog("Mods: could not rename %s -> %s (error %lu)", from.c_str(), to.c_str(), GetLastError());
    }
    BuildIndex();   // re-scan so ModCreateFileA sees the new state immediately
    return ok;
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
