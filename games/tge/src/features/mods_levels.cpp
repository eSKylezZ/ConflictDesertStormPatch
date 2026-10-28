// Levels from mods (part of tgefix.modtables): port of DesertStorm2Fix's mods_levels.cpp (games/ds2/src/features/
// mods_levels.cpp; DS2 addresses in brackets) to GreatEscape.exe (mapped 2026-10-25).
//
//  1. Mission DLLs from mods: Level_Init 0x5D1DE0 loads a "*.dll" level through the loader FUN_00590b90 (stdcall(loader,
//     name) ret 8, from 0x5D227A) [FUN_00422020]: `call [LoadLibraryA]` at 0x590B9D with the bare name (-> game folder /
//     system paths, never Mods\), handle [0x7DC0F8], then GetProcAddress "LevelSetup" and LevelSetup(&api 0x65C1A8)
//     -> script object [0x7DC0F4]. The call is hooked: a name the mod index has (Mods\<mod>\X.dll, or one built from a
//     .dllpatch) loads by its full path. The game frees it as before (FreeLibrary inlined in Level_Unload 0x5CEE50).
//  2. "<stem>.level" next to a mod's mission DLL: "--@archive level01" (or "--@archive none") = that level's data.
//     Level_Init names everything after the level name minus its extension (lower-cased at 0x5D1E79):
//       archive "<stem>.dat"    FUN_004c5e90(slot 1, name, a, b) at 0x5D1ECA (0, 0) and 0x5D20FB (1, 0) [0x4C3400]
//       sound cache "<stem>.sch" FUN_004c55a0(name, out) at 0x5D205D, loaded by FUN_0041f950 (thiscall [0x79EDC8])
//       music "<stem>Music.sbk"  FUN_0041db60 thiscall(sound mgr [0x79EDC8], name, 0) at 0x5D20C8 [FUN_004b33c0]
//       pools "<STEM>.FAS"       sprintf("%s.FAS", world [0x79ED6C] +0x1BC0) at 0x4829D8 inside FUN_004829b0
//     All four are redirected.
//  3. Game data by reference (mods carry no game files): <name>.uses ("file X", "file A = B", "piece KIT = MODEL.EVO n"),
//     <name>.envpatch ("base GE_LEVEL01.ENV" + hunks), <name>.dllpatch (byte changes against a shipped DLL, every
//     original byte checked) -> built from the player's own archives into %TEMP%\GreatEscapeFix\merged\game and indexed
//     like mod files. No .rmdpatch: TGE's route files (.rmd: u32 count + 80-byte markers) aren't DS1 / DS2's.
//  Edited scenes: TGE only loads "<scene>.OCT" (FUN_00467f10 at 0x5D2404 - no octree builder; a missing one makes
//  Level_Init fail), so an .envpatch must not change the order of zone props, or the mod ships its own .OCT.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
std::string Stem(const std::string& name) {
    const std::string base = BaseName(name);
    const size_t dot = base.find_last_of('.');
    return dot == std::string::npos ? base : base.substr(0, dot);
}
std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}
bool ReadAll(const std::string& path, std::string& out) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const DWORD size = GetFileSize(f, nullptr);
    out.resize(size);
    DWORD got = 0;
    const bool ok = size == 0 || (ReadFile(f, out.data(), size, &got, nullptr) && got == size);
    CloseHandle(f);
    return ok;
}
bool WriteAll(const std::string& path, const std::string& data) {
    HANDLE f = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(f);
    return ok;
}
std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t a = 0;
    while (a < text.size()) {
        size_t b = text.find('\n', a);
        if (b == std::string::npos) b = text.size();
        std::string line = text.substr(a, b - a);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        a = b + 1;
    }
    return out;
}
std::string BuiltDir() {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "GreatEscapeFix";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\merged";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\game";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

// ---- the player's own game files (archive hash: LFSR over the LOWER-cased bare name) ----
uint32_t ArchiveHash(const std::string& name) {
    uint32_t x = 1;
    for (char ch : BaseName(name)) {
        const unsigned char c = static_cast<unsigned char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        for (int b = 0; b < 8; ++b) x = x << 1 | (((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1);
    }
    return x;
}

std::vector<std::string> g_dats;   // catalog.dat first, then the rest of <game>\*.dat

bool ReadGameFile(const std::string& name, std::string& out) {
    if (g_dats.empty()) {
        const std::string dir = GameDir();
        g_dats.push_back(dir + "\\catalog.dat");
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dir + "\\*.dat").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (_stricmp(fd.cFileName, "catalog.dat") && _strnicmp(fd.cFileName, "unins", 5))
                    g_dats.push_back(dir + "\\" + fd.cFileName);
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    const uint32_t hash = ArchiveHash(name);
    for (const std::string& dat : g_dats) {
        HANDLE f = CreateFileA(dat.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        const DWORD fileSize = GetFileSize(f, nullptr);
        uint32_t first[3] = {};
        DWORD got = 0;
        bool found = false;
        if (ReadFile(f, first, 12, &got, nullptr) && got == 12 && first[1] >= 12 && first[1] <= (4u << 20) &&
            first[1] <= fileSize) {
            std::vector<uint32_t> index(first[1] / 4);
            SetFilePointer(f, 0, nullptr, FILE_BEGIN);
            ReadFile(f, index.data(), first[1] / 12 * 12, &got, nullptr);
            for (size_t i = 0; i + 2 < got / 4; i += 3) {
                const uint32_t eh = index[i], offset = index[i + 1], size = index[i + 2];
                if (!eh && !offset && !size) break;
                if (eh != hash || offset + size > fileSize) continue;
                out.resize(size);
                SetFilePointer(f, static_cast<LONG>(offset), nullptr, FILE_BEGIN);
                found = ReadFile(f, out.data(), size, &got, nullptr) && got == size;
                break;
            }
        }
        CloseHandle(f);
        if (found) return true;
    }
    return false;
}

// ---- references: built from the player's install ----
bool AddBuilt(const std::string& name, const std::string& data, int mod, const std::string& label, const char* what) {
    const std::string key = Upper(name);
    if (features::ModFilePath(key.c_str())) return false;   // the mod ships its own (or another built it)
    const std::string path = BuiltDir() + "\\" + key;
    if (!WriteAll(path, data)) {
        features::ModLog("[fail] Mods: %s: can't write %s (%s)", label.c_str(), path.c_str(), what);
        return false;
    }
    features::SetModFile(key, path, mod);
    return true;
}

bool ApplyEnvPatch(const std::string& patch, std::string& out, std::string& base, std::string& error) {
    const std::vector<std::string> p = SplitLines(patch);
    if (p.empty() || p[0].compare(0, 5, "base ") != 0) {
        error = "no \"base <scene>\" line";
        return false;
    }
    base = p[0].substr(5);
    while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();
    std::string text;
    if (!ReadGameFile(base, text)) {
        error = base + " isn't in this game's archives";
        return false;
    }
    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::vector<std::string> lines = SplitLines(text);
    std::vector<std::string> result;
    size_t at = 0;   // next base line to copy
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i].empty() || p[i][0] != '@') continue;
        unsigned start = 0, del = 0, ins = 0;
        if (sscanf_s(p[i].c_str(), "@ %u %u %u", &start, &del, &ins) != 3 || start < at || start + del > lines.size() ||
            i + ins >= p.size() + 1) {
            error = "hunk \"" + p[i] + "\" doesn't fit " + base;
            return false;
        }
        while (at < start) result.push_back(lines[at++]);
        at += del;
        for (unsigned k = 0; k < ins; ++k) result.push_back(p[++i]);
    }
    while (at < lines.size()) result.push_back(lines[at++]);
    out.clear();
    for (const std::string& l : result) out += l + (crlf ? "\r\n" : "\n");
    return true;
}

int HexNibble(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool HexBytes(const std::string& hex, std::string& out) {
    out.clear();
    if (hex.size() % 2) return false;
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexNibble(hex[i]), lo = HexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi << 4 | lo);
    }
    return true;
}

// .dllpatch: "base Level01.dll" (a file in the game folder), "base-size <bytes>", optional "size <bytes>", then
// "@ <file offset hex> <original hex | -> <new hex>" (as DS1 / DS2, ModKit gamerefs.make_dll_patch).
bool ApplyDllPatch(const std::string& patch, std::string& out, std::string& base, std::string& error) {
    size_t baseSize = 0, size = 0;
    bool haveBaseSize = false, haveSize = false;
    std::vector<std::string> hunks;
    for (std::string line : SplitLines(patch)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        char key[16] = {}, value[MAX_PATH] = {};
        if (sscanf_s(line.c_str(), "%15s %259s", key, 16, value, MAX_PATH) < 1) continue;
        if (!strcmp(key, "base")) base = value;
        else if (!strcmp(key, "base-size")) haveBaseSize = sscanf_s(value, "%zu", &baseSize) == 1;
        else if (!strcmp(key, "size")) haveSize = sscanf_s(value, "%zu", &size) == 1;
        else if (!strcmp(key, "@")) hunks.push_back(line);
    }
    if (base.empty() || base.find_first_of("\\/:") != std::string::npos || !haveBaseSize) {
        error = "no \"base <file>\" / \"base-size\" line";
        return false;
    }
    std::string file;
    if (!ReadAll(GameDir() + "\\" + base, file)) {
        error = base + " isn't in the game folder";
        return false;
    }
    if (file.size() != baseSize) {
        error = base + " is another version (size differs)";
        return false;
    }
    out = file;
    out.resize(haveSize ? size : file.size(), '\0');
    for (const std::string& h : hunks) {
        char offsetText[16] = {};
        std::string origText(h.size(), '\0'), newText(h.size(), '\0');
        size_t offset = 0;
        std::string orig, now;
        if (sscanf_s(h.c_str(), "@ %15s %s %s", offsetText, 16, origText.data(), static_cast<unsigned>(origText.size()),
                     newText.data(), static_cast<unsigned>(newText.size())) != 3 ||
            sscanf_s(offsetText, "%zx", &offset) != 1) {
            error = "bad line \"" + h + "\"";
            return false;
        }
        origText.resize(strlen(origText.c_str()));
        newText.resize(strlen(newText.c_str()));
        if (!HexBytes(newText, now) || (origText != "-" && !HexBytes(origText, orig)) ||
            (!orig.empty() && orig.size() != now.size()) || offset + now.size() > out.size()) {
            error = "bad line \"" + h + "\"";
            return false;
        }
        if (!orig.empty() && (offset + orig.size() > file.size() || file.compare(offset, orig.size(), orig))) {
            error = base + " is another version (bytes at " + offsetText + " differ)";
            return false;
        }
        if (orig.empty() && offset < file.size()) {
            error = "bytes at " + std::string(offsetText) + " are in the base but have no original (\"-\")";
            return false;
        }
        out.replace(offset, now.size(), now);
    }
    return true;
}

// Kit piece: object `index` of a world-piece model as a one-object static prop (ModKit kits.py rules): the bytes from
// its EOBJ to its NEXT / DONE, header patched (model name +8, node type +0x8C = 0, parent = "ROOT", last tag DONE).
// The parent field's place follows the EOBJ version as on DS1 (0x2F: +0xB0) / DS2 (0x35: +0xAC); other versions refused.
bool CutPiece(const std::string& model, int index, const std::string& name, std::string& out, std::string& error) {
    std::string data;
    if (!ReadGameFile(model, data)) {
        error = model + " isn't in this game's archives";
        return false;
    }
    if (data.size() < 0x100 || data.compare(0, 4, "EOBJ") != 0) {
        error = model + " isn't a model";
        return false;
    }
    uint32_t version = 0;
    memcpy(&version, data.data() + 4, 4);
    size_t parentAt = 0;
    if ((version & 0xFFFF) == 0x35) parentAt = 0xAC;
    else if ((version & 0xFFFF) == 0x2F) parentAt = 0xB0;
    else {
        char v[16];
        snprintf(v, sizeof v, "0x%X", version & 0xFFFF);
        error = model + " has a model version (" + v + ") kit pieces don't know yet";
        return false;
    }
    const std::string next = std::string("NEXTEOBJ") + std::string(reinterpret_cast<const char*>(&version), 4);
    std::vector<size_t> starts = {0};
    for (size_t p = data.find(next); p != std::string::npos; p = data.find(next, p + 4)) starts.push_back(p + 4);
    if (index < 0 || index >= static_cast<int>(starts.size())) {
        error = model + " has " + std::to_string(starts.size()) + " objects";
        return false;
    }
    const size_t a = starts[index], b = index + 1 < static_cast<int>(starts.size()) ? starts[index + 1] : data.size();
    out = data.substr(a, b - a);
    if (out.size() < parentAt + 64 || (out.compare(out.size() - 4, 4, "NEXT") && out.compare(out.size() - 4, 4, "DONE"))) {
        error = "object " + std::to_string(index) + " doesn't end in NEXT / DONE";
        return false;
    }
    const std::string n = name.substr(0, 63);
    memset(&out[8], 0, 64);
    memcpy(&out[8], n.data(), n.size());
    const uint32_t staticType = 0;
    memcpy(&out[0x8C], &staticType, 4);
    memset(&out[parentAt], 0, 64);
    memcpy(&out[parentAt], "ROOT", 4);
    memcpy(&out[out.size() - 4], "DONE", 4);
    return true;
}

struct RefFile {
    std::string path, label;
    int mod;
};

void BuildUses(const RefFile& u) {
    std::string text;
    if (!ReadAll(u.path, text)) return;
    int files = 0, failed = 0;
    for (std::string line : SplitLines(text)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        char kind[8] = {}, a[128] = {}, eq[4] = {}, b[128] = {};
        const int n = sscanf_s(line.c_str(), "%7s %127s %3s %127s", kind, 8, a, 128, eq, 4, b, 128);
        if (n < 2) continue;
        if (!_stricmp(kind, "piece")) {   // piece KIT_X = MODEL.EVO <index>
            char model[128] = {};
            int index = -1;
            if (sscanf_s(line.c_str(), "%*s %*s = %127s %d", model, 128, &index) != 2) {
                features::ModLog("[fail] Mods: %s - bad line \"%s\"", u.label.c_str(), line.c_str());
                ++failed;
                continue;
            }
            const std::string target = std::string(a) + ".EVO";
            if (features::ModFilePath(target.c_str())) { ++files; continue; }
            std::string data, error;
            if (!CutPiece(model, index, a, data, error)) {
                features::ModLog("[fail] Mods: %s uses piece %d of %s: %s", u.label.c_str(), index, model, error.c_str());
                ++failed;
                continue;
            }
            if (AddBuilt(target, data, u.mod, u.label, "kit piece")) ++files;
        } else if (!_stricmp(kind, "file")) {
            const std::string source = n == 4 && !strcmp(eq, "=") ? b : a;
            if (features::ModFilePath(a)) { ++files; continue; }
            std::string data;
            if (!ReadGameFile(source, data)) {
                features::ModLog("[fail] Mods: %s uses %s, which isn't in this game's data", u.label.c_str(), source.c_str());
                ++failed;
                continue;
            }
            if (AddBuilt(a, data, u.mod, u.label, "file")) ++files;
        } else {
            features::ModLog("[fail] Mods: %s - unknown line \"%s\" (file / piece)", u.label.c_str(), line.c_str());
            ++failed;
        }
    }
    features::ModLog("Mods: %s uses %d game file(s) from your install%s", u.label.c_str(), files,
                     failed ? " - see the [fail] lines" : "");
}

void BuildPatch(const RefFile& r, bool dll) {
    std::string patch, data, base, error;
    if (!ReadAll(r.path, patch)) return;
    const bool ok = dll ? ApplyDllPatch(patch, data, base, error) : ApplyEnvPatch(patch, data, base, error);
    if (!ok) {
        features::ModLog("[fail] Mods: %s - %s (the game's own %s is used)", r.label.c_str(), error.c_str(),
                         dll ? "mission DLL" : "scene");
        return;
    }
    // the patch is named like the file it makes ("MyOp.dllpatch" -> MYOP.DLL, "GE_Level01.envpatch" -> GE_LEVEL01.ENV)
    const std::string target = Upper(Stem(r.path)) + (dll ? ".DLL" : ".ENV");
    if (AddBuilt(target, data, r.mod, r.label, dll ? "mission DLL" : "scene"))
        features::ModLog("Mods: %s built from %s + %s%s", target.c_str(), base.c_str(), r.label.c_str(),
                         dll ? "" : " (scene: keep the zone props' order - The Great Escape can't rebuild <scene>.OCT)");
}

void Collect(const std::string& dir, int mod, const std::string& modName, std::vector<RefFile>& uses,
             std::vector<RefFile>& envs, std::vector<RefFile>& dlls) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == '.') continue;
        const std::string path = dir + "\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            Collect(path, mod, modName, uses, envs, dlls);
            continue;
        }
        const std::string key = Upper(fd.cFileName);
        const RefFile r{path, modName + "\\" + fd.cFileName, mod};
        auto ends = [&](const char* t) { return key.size() > strlen(t) && key.compare(key.size() - strlen(t), strlen(t), t) == 0; };
        if (ends(".USES")) uses.push_back(r);
        else if (ends(".ENVPATCH")) envs.push_back(r);
        else if (ends(".DLLPATCH")) dlls.push_back(r);
        else if (ends(".RMDPATCH"))
            features::ModLog("[fail] Mods: %s - route patches aren't supported for The Great Escape (its .rmd format differs)",
                             r.label.c_str());
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

// ---- .level sidecars: the level data a mission DLL uses ----
std::string ArchiveDirectiveIn(const std::string& head) {
    const size_t at = head.find("--@archive");
    if (at == std::string::npos) return {};
    const size_t b = head.find_first_not_of(" \t", at + 10);
    if (b == std::string::npos) return {};
    const size_t e = head.find_first_of(" \t\r\n", b);
    std::string value = head.substr(b, e == std::string::npos ? std::string::npos : e - b);
    if (value.size() > 4 && _stricmp(value.c_str() + value.size() - 4, ".dat") == 0) value.resize(value.size() - 4);
    return value;
}

bool g_redirect = false;
std::string g_archive;   // "level01", "none", or empty
std::string g_levelStem;

bool UsesOwnArchive() { return g_redirect && !g_archive.empty() && _stricmp(g_archive.c_str(), "none") != 0; }

// The level's stem from the archive name Level_Init built ("<stem>.dat"): a mod's "<stem>.level" decides.
void StartLevel(const char* archivePath) {
    g_redirect = false;
    g_archive.clear();
    g_levelStem = archivePath ? Stem(archivePath) : std::string();
    if (g_levelStem.empty()) return;
    std::string luaName = g_levelStem + ".lua";
    const char* path = features::ModFilePath((g_levelStem + ".level").c_str());
    if (path) {
        std::string head;
        if (!ReadAll(path, head)) return;
        head.resize(std::min<size_t>(head.size(), 2048));
        g_archive = ArchiveDirectiveIn(head);
    } else {   // a .lua level (scripthost.cpp): its "--@archive" / "--@base" line; none = its own <stem>.dat if any
        g_archive = features::ScriptArchiveFor(g_levelStem);
        if (g_archive.empty()) return;
        path = luaName.c_str();
    }
    if (g_archive.empty()) {
        features::ModLog("[fail] Mods: %s has no \"--@archive <level>\" line - the level's own data names are used", path);
        return;
    }
    if (_stricmp(g_archive.c_str(), "none") != 0 &&
        GetFileAttributesA((GameDir() + "\\" + g_archive + ".dat").c_str()) == INVALID_FILE_ATTRIBUTES &&
        !features::ModFilePath((g_archive + ".dat").c_str())) {
        features::ModLog("[fail] Mods: %s: --@archive %s - there is no %s.dat; the level's own data names are used", path,
                         g_archive.c_str(), g_archive.c_str());
        g_archive.clear();
        return;
    }
    g_redirect = true;
    dslog::Write("Mods: level %s - data from %s (%s)", g_levelStem.c_str(), g_archive.c_str(), path);
}

// ---- hooks ----
constexpr uint32_t kMissionDllLoadCall = 0x590B9D, kLoadLibraryIat = 0x5F50FC;
constexpr uint32_t kArchiveOpen = 0x4C5E90;
constexpr uint32_t kArchiveOpenCalls[] = {0x5D1ECA, 0x5D20FB};
constexpr uint32_t kSchNameCall = 0x5D205D, kSchName = 0x4C55A0;
constexpr uint32_t kMusicCall = 0x5D20C8, kMusicLoad = 0x41DB60;
constexpr uint32_t kFasNameCall = 0x4829D8, kSprintf = 0x51EE86;

HMODULE WINAPI LoadMissionDll(const char* name) {
    const char* own = name ? features::ModFilePath(name) : nullptr;
    if (!own) return LoadLibraryA(name);
    HMODULE m = LoadLibraryExA(own, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD error = m ? 0 : GetLastError();
    if (m) {
        static std::unordered_set<std::string> logged;
        if (logged.insert(Upper(own)).second) features::ModLog("Mods: mission DLL %s from %s", name, own);
    } else {
        features::ModLog("[fail] Mods: mission DLL %s from %s didn't load (error %lu)", name, own, error);
    }
    return m;
}

using ArchiveOpenFn = int(__cdecl*)(int, const char*, int, int);
int __cdecl OpenLevelArchive(int slot, const char* path, int a, int b) {
    const auto open = reinterpret_cast<ArchiveOpenFn>(kArchiveOpen);
    if (a == 0 && b == 0) StartLevel(path);   // 0x5D1ECA = the first open of a new level; 0x5D20FB reopens with (1, 0)
    if (!g_redirect) return open(slot, path, a, b);
    if (_stricmp(g_archive.c_str(), "none") == 0) return 0;
    const std::string own = g_archive + ".dat";
    return open(slot, own.c_str(), a, b);
}

int __cdecl SchName(const char* name, char* out) {
    const std::string own = g_archive + ".sch";
    return reinterpret_cast<int(__cdecl*)(const char*, char*)>(kSchName)(UsesOwnArchive() ? own.c_str() : name, out);
}

int __fastcall MusicLoad(void* mgr, void*, const char* name, int flag) {
    const std::string own = g_archive + "Music.sbk";
    return reinterpret_cast<int(__thiscall*)(void*, const char*, int)>(kMusicLoad)(mgr, UsesOwnArchive() ? own.c_str() : name, flag);
}

int __cdecl FasName(char* out, const char* fmt, const char* stem) {
    const std::string own = Upper(g_archive);
    return reinterpret_cast<int(__cdecl*)(char*, const char*, const char*)>(kSprintf)(out, fmt, UsesOwnArchive() ? own.c_str() : stem);
}
}  // namespace

// Mods folder state -> files built from the player's install (.uses / .envpatch / .dllpatch). Called by mods_tables.cpp
// after every index build.
void features::BuildModLevels() {
    std::vector<RefFile> uses, envs, dlls;
    const std::vector<std::string>& mods = LoadedMods();
    const std::string root = GameDir() + "\\Mods";
    for (size_t i = 0; i < mods.size(); ++i) Collect(root + "\\" + mods[i], static_cast<int>(i), mods[i], uses, envs, dlls);
    for (const RefFile& u : uses) BuildUses(u);
    for (const RefFile& r : envs) BuildPatch(r, false);
    for (const RefFile& r : dlls) BuildPatch(r, true);
}

void features::ApplyModLevels() {
    if (!patch::HookIndirectCall(kMissionDllLoadCall, reinterpret_cast<const void*>(&LoadMissionDll), kLoadLibraryIat))
        ModLog("[fail] Mods: mission DLL load at 0x%08X not recognised - mission DLLs from mods aren't used", kMissionDllLoadCall);
    bool ok = true;
    for (uint32_t site : kArchiveOpenCalls)
        ok &= patch::HookCall(site, reinterpret_cast<const void*>(&OpenLevelArchive), kArchiveOpen);
    ok &= patch::HookCall(kSchNameCall, reinterpret_cast<const void*>(&SchName), kSchName);
    ok &= patch::HookCall(kMusicCall, reinterpret_cast<const void*>(&MusicLoad), kMusicLoad);
    ok &= patch::HookCall(kFasNameCall, reinterpret_cast<const void*>(&FasName), kSprintf);
    if (!ok) ModLog("[fail] Mods: level data names not all hooked - .level files may not redirect everything");
}
