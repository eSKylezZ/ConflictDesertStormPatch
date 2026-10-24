// Loose-file mods for Conflict: Global Storm: a file in <game>\Mods\<mod name>\<anything>\<FILE> can replace or add a
// game file by NAME (case-insensitive basename match, any sub-folder). Mods apply in folder-name order, a later mod
// wins; folders starting with '_' or '.' are switched off.
//
// Unlike DS1/DS2/CV/TGE, this resolver is NOT a hook on the game's own file-resolve function. ConflictGlobal.exe
// (RELOADED NoCD) runs almost all of its logic - including, as far as could be checked without a full RE pass, its
// archive index lookup and path building - inside a SecuROM virtual machine with no static call sites to hook
// (games/cgs/CLAUDE.md "Environment"). Finding the VM'd resolver/opener/exists-check the way CV's were found (CV's
// mods.cpp header comment) would need its own RE session; that work was NOT done here (scope note in the hand-off).
//
// Instead this hooks the plain (non-VM'd) kernel32 import slot CreateFileA (0x6C0138, confirmed present in the
// exe's import table 2026-10-24: ReadFile 0x6C0088, SetFilePointer 0x6C008C, DeleteFileA 0x6C0114, CreateFileA
// 0x6C0138, FindNextFileA 0x6C01DC, FindFirstFileA 0x6C01E0 - all plain kernel32 thunks, since the VM has to call
// real Win32 eventually). Every CreateFileA the game makes is checked against the loose-file index by the
// requested path's basename; a match for a WRITE-less open (GENERIC_READ, OPEN_EXISTING-ish) is redirected to the
// mod's file.
//
// What this does and does not cover:
//   - DOES let a mod replace or add any file the game opens by a real Win32 CreateFileA call with a literal path -
//     a whole .dat archive, a loose config/text file, anything not packed inside another file's byte range.
//   - Does NOT let a mod override one asset packed inside a .dat archive (a single texture/model id inside e.g.
//     GLOBAL1.dat) - that needs the VM'd archive-index lookup itself, not yet reverse engineered. A modder who wants
//     to change one packed asset must currently replace the whole archive file that contains it.
//   - FindFirstFileA/FindNextFileA (directory enumeration, e.g. a save/profile browser) are left alone: redirecting
//     enumeration results without duplicating the game's own directory-merge logic risks showing files twice or not
//     at all, and nothing observed so far needs it for the archive path.
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kIatCreateFileA = 0x6C0138;

using CreateFileAFn = HANDLE(WINAPI*)(LPCSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileAFn g_next = nullptr;

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
    features::ModLog("Mods: %zu mod(s), %zu file(s) indexed by name (basename match, CreateFileA redirect only - see "
                      "mods.cpp header: archives are replaced whole, not per asset)",
                      g_mods.size(), g_files.size());
}

HANDLE WINAPI ModCreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition,
                             DWORD flags, HANDLE tmpl) {
    if (path && disposition == OPEN_EXISTING && !(access & GENERIC_WRITE)) {
        auto it = g_files.find(BaseName(path));
        if (it != g_files.end()) {
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
    patch::HookSlot(kIatCreateFileA, reinterpret_cast<void*>(&ModCreateFileA),
                    reinterpret_cast<void**>(&g_next), nullptr);
}

int features::ModCount() { return static_cast<int>(g_mods.size()); }

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
