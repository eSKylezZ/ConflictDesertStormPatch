// Levels from mods (port of DesertStormFix's mission-DLL / game-reference / edited-scene support, repo root
// src/features/mods.cpp BuildReferences + src/features/scripthost.cpp; DS1 addresses in brackets):
//
//  1. Mission DLLs from mods: level init FUN_00412690 (cdecl(name)) loads a "*.dll" level through FUN_00422020
//     (stdcall(name) ret 4, from 0x412BAB [FUN_00449260]): `call [LoadLibraryA]` at 0x42202D with the bare name
//     (-> game folder / system paths, never Mods\), handle [0x62FE04], then GetProcAddress "LevelSetup" (0x422051) and
//     LevelSetup(&api). The call is hooked: a name the mod index has (Mods\<mod>\X.dll, or one built from a .dllpatch)
//     loads by its full path (LOAD_WITH_ALTERED_SEARCH_PATH). The game frees it as before.
//  2. "<stem>.level" next to a mod's mission DLL: "--@archive mission01" (or "--@archive none") = that level's data:
//     level init names everything after the level name minus its extension (lower-cased at 0x412723):
//       archive "<stem>.dat"   FUN_004c3400(slot 1, name, a, b) at 0x41276F (0, 0), 0x412879 (1, 0), 0x4130D0 (1, 0)
//                              [FUN_004bcf70 at 0x40C19C]
//       sound cache "<stem>.sch" copied by FUN_004c2300(src, dst) at 0x412963 (a strcpy) and loaded by FUN_004b5de0
//                              (thiscall sound mgr [0x6800EC]) at 0x4129DA [FUN_0055f540 at 0x40C2E6]
//       music "<stem>Music.sbk" FUN_004b33c0 thiscall(sound mgr, name, 0) at 0x412A49 [FUN_0055dd20 at 0x40C350]
//       physics pools "<STEM>.FAS" sprintf("%s.FAS", [world +0x1BBC]) at 0x5096BB inside FUN_00509660 (from 0x4127BB)
//     All four are redirected. (World +0x1BBC = the upper-cased stem, kept: mods.cpp's InMission reads it.)
//  3. Game data by reference (mods carry no game files): <name>.uses ("file X", "file A = B", "piece KIT = MODEL.EVO n"),
//     <name>.envpatch ("base MISSION01.ENV" + hunks), <name>.rmdpatch (DS2 route files: version 3, 29-byte markers),
//     <name>.dllpatch (byte changes against a shipped DLL, every original byte checked) -> built from the player's own
//     archives into %TEMP%\DesertStorm2Fix\merged\game at start-up (and by the MODS menu's Apply) and indexed like mod
//     files (mods.cpp AddBuiltModFile). Archive names are hashed lower-cased in DS2 (FUN_004c0b30).
//  4. Edited scenes: the scene's zone props are culled through "<scene>.OCT", which indexes them by their place in the
//     scene - a scene a mod changed would draw / collide with the wrong props. Level init 0x412E62..0x412EB8 [0x40C633]:
//     start-up flag 'c' (FUN_004c0ce0 at 0x412E64) -> FUN_004ea870(1) builds every zone's octree and writes it
//     (fopen "wb" at 0x4EA8B8), FUN_004e2800 frees them (0x412E99), FUN_004ea9b0 loads the file back (0x412EB1); flag
//     'd' (the shipped start-up options "-fl -d") loads the shipped one. When the level's scene comes from a mod (its
//     .ENV is in the mod index, not with its own .OCT next to it) the flag check answers yes, the file goes to %TEMP%
//     and the free + load-back are skipped (the trees stay as built - like DS1's vehicle-swap path). Also for a shipped
//     scene while a mod gives a unit more kit items than the game (features::ModKitsGrown, mods_tables.cpp): the extra
//     item objects are made during the scene load and shift every later object against the shipped .OCT.
//     AI navigation: Routing_Load FUN_004bcd10(scene, [0x61BB88]) at 0x412FE7 reads <scene>.QTD + .RMD; a 0 makes level
//     init build both from the scene (FUN_004bce70 at 0x413003 [FUN_00507470]). For a mod scene without its own .QTD /
//     .RMD (or .rmdpatch) the load answers 0, and the hand-placed markers (the first ones, flags bit 0 clear - mission
//     scripts address them by index) are put back in front of the generated ones from the base level's route file
//     (see BuildNav).
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
// ---- small helpers ----
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

std::string TempDir() {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "DesertStorm2Fix";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string BuiltDir() {
    std::string dir = TempDir() + "\\merged";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\game";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

// ---- the player's own game files ----
// Archive name hash (FUN_004c0b30): LFSR over the LOWER-cased bare name (DS1 hashed it upper-cased).
uint32_t ArchiveHash(const std::string& name) {
    uint32_t x = 1;
    for (char ch : BaseName(name)) {
        const unsigned char c = static_cast<unsigned char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
            x = x << 1 | in;
        }
    }
    return x;
}

std::vector<std::string> g_dats;   // catalog.dat first, then the rest of <game>\*.dat

// The game's own copy of `name` out of its archives (directory: 12-byte {hash, offset, size} entries up to the first
// entry's data, 0x8000 in every shipped archive).
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
std::unordered_set<std::string> g_built;   // upper-case names this module built (not real mod files)
std::unordered_map<std::string, std::string> g_envBase;   // upper-case scene stem built from an .envpatch -> base stem

bool AddBuilt(const std::string& name, const std::string& data, const std::string& mod, const char* what) {
    const std::string key = Upper(name);
    if (!features::ModFilePath(key.c_str()).empty()) return false;   // the mod ships its own (or another built it)
    const std::string path = BuiltDir() + "\\" + key;
    if (!WriteAll(path, data)) {
        features::ModLog("[fail] Mods: %s: can't write %s (%s)", mod.c_str(), path.c_str(), what);
        return false;
    }
    if (!features::AddBuiltModFile(key, path, mod)) return false;
    g_built.insert(key);
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

// Route files: i32 -1, i32 version, i32 count, then markers - DS2 version 3 = 29 bytes {f32 x, y, z, weight; u16
// link[5]; u16 flags; u8}, DS1 version 1 = 32 bytes {f32 x, y, z; u16 link[5]; u16; u16 flags; u16; f32 weight}.
// Patch lines: "<i> x y z l0 l1 l2 l3 l4 flags weight" (ModKit gamerefs.make_rmd_patch).
bool ApplyRmdPatch(const std::string& patch, std::string& out, std::string& base, std::string& error) {
    const std::vector<std::string> p = SplitLines(patch);
    if (p.size() < 2 || p[0].compare(0, 5, "base ") != 0) {
        error = "no \"base <file>\" / \"count\" lines";
        return false;
    }
    base = p[0].substr(5);
    while (!base.empty() && (base.back() == ' ' || base.back() == '\t')) base.pop_back();
    std::string file;
    unsigned count = 0;
    if (!ReadGameFile(base, file) || file.size() < 12) {
        error = base + " isn't in this game's archives";
        return false;
    }
    if (sscanf_s(p[1].c_str(), "count %u", &count) != 1 || count > 65535) {
        error = "bad \"count\" line";
        return false;
    }
    int32_t head[3];
    memcpy(head, file.data(), 12);
    const size_t size = head[0] == -1 && head[1] >= 3 ? 29 : head[0] == -1 && head[1] > 0 ? 32 : 0;
    if (!size || 12 + size * static_cast<size_t>(head[2]) != file.size()) {
        error = base + " is a route file version this game doesn't use";
        return false;
    }
    const size_t baseCount = static_cast<size_t>(head[2]);
    out.assign(file.data(), 12);
    const int32_t n = static_cast<int32_t>(count);
    memcpy(out.data() + 8, &n, 4);
    out.resize(12 + size * count, '\0');
    memcpy(out.data() + 12, file.data() + 12, size * std::min<size_t>(baseCount, count));
    for (size_t i = 2; i < p.size(); ++i) {
        unsigned idx, l[5], flags;
        float x, y, z, w;
        if (sscanf_s(p[i].c_str(), "%u %f %f %f %u %u %u %u %u %u %f", &idx, &x, &y, &z, &l[0], &l[1], &l[2], &l[3],
                     &l[4], &flags, &w) != 11 || idx >= count)
            continue;
        char* m = out.data() + 12 + size * idx;
        uint16_t links[5];
        for (int k = 0; k < 5; ++k) links[k] = static_cast<uint16_t>(l[k]);
        const uint16_t f = static_cast<uint16_t>(flags);
        memcpy(m, &x, 4); memcpy(m + 4, &y, 4); memcpy(m + 8, &z, 4);
        if (size == 29) {
            memcpy(m + 12, &w, 4);
            memcpy(m + 16, links, 10);
            memcpy(m + 26, &f, 2);
        } else {
            memcpy(m + 12, links, 10);
            memcpy(m + 24, &f, 2);
            if (idx >= baseCount) { const uint16_t filler = 0xCDCD; memcpy(m + 26, &filler, 2); }
            memcpy(m + 28, &w, 4);
        }
    }
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

// .dllpatch: "base Mission01.dll" (a file in the game folder), "base-size <bytes>", optional "size <bytes>", then
// "@ <file offset hex> <original hex | -> <new hex>" (as DS1, ModKit gamerefs.make_dll_patch).
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

// Kit piece: object `index` of a world-piece model (no transform keys - every object in world space) as a one-object
// static prop: the bytes from its EOBJ to its NEXT / DONE, header patched (model name +8, node type +0x8C = 0, parent
// = "ROOT", last tag DONE). DS2's EOBJ (version 0x35, reader FUN_005087b0) has its parent name at +0xAC (DS1 0x2F:
// +0xB0); objects are found by their "EOBJ" + the first object's version after the previous one's NEXT. Same rules as
// ModKit conflict_ds_tools/kits.py.
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
        error = model + " has a model version kit pieces don't know";
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

std::string FileName(const std::string& path) { return BaseName(path); }

void BuildUses(const features::ModFileRef& u) {
    std::string text;
    if (!ReadAll(u.path, text)) return;
    int files = 0, failed = 0;
    for (std::string line : SplitLines(text)) {
        const size_t hash = line.find('#');
        if (hash != std::string::npos) line.resize(hash);
        char kind[8] = {}, a[128] = {}, eq[4] = {}, b[128] = {};
        const int n = sscanf_s(line.c_str(), "%7s %127s %3s %127s", kind, 8, a, 128, eq, 4, b, 128);
        if (n < 2) continue;
        if (!_stricmp(kind, "bank")) {   // a game sound bank by name: into every level's cache (mods_sound.cpp)
            if (features::BorrowGameBank(a, u.mod + ": " + FileName(u.path))) ++files;
            else ++failed;
        } else if (!_stricmp(kind, "piece")) {             // piece KIT_X = MODEL.EVO <index>
            char model[128] = {};
            int index = -1;
            if (sscanf_s(line.c_str(), "%*s %*s = %127s %d", model, 128, &index) != 2) {
                features::ModLog("[fail] Mods: %s: %s - bad line \"%s\"", u.mod.c_str(), FileName(u.path).c_str(), line.c_str());
                ++failed;
                continue;
            }
            const std::string target = std::string(a) + ".EVO";
            if (!features::ModFilePath(target.c_str()).empty()) { ++files; continue; }
            std::string data, error;
            if (!CutPiece(model, index, a, data, error)) {
                features::ModLog("[fail] Mods: %s uses piece %d of %s: %s", u.mod.c_str(), index, model, error.c_str());
                ++failed;
                continue;
            }
            if (AddBuilt(target, data, u.mod, "kit piece")) ++files;
        } else if (!_stricmp(kind, "file")) {
            const std::string source = n == 4 && !strcmp(eq, "=") ? b : a;
            if (!features::ModFilePath(a).empty()) { ++files; continue; }
            std::string data;
            if (!ReadGameFile(source, data)) {
                features::ModLog("[fail] Mods: %s uses %s, which isn't in this game's data", u.mod.c_str(), source.c_str());
                ++failed;
                continue;
            }
            if (AddBuilt(a, data, u.mod, "file")) ++files;
        } else {
            features::ModLog("[fail] Mods: %s: %s - unknown line \"%s\"", u.mod.c_str(), FileName(u.path).c_str(), line.c_str());
            ++failed;
        }
    }
    features::ModLog("Mods: %s uses %d game file(s) from your install (%s)%s", u.mod.c_str(), files,
                     FileName(u.path).c_str(), failed ? " - see the [fail] lines" : "");
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

bool g_dllRedirect = false;
std::string g_archive;   // "mission01", "none", or empty
std::string g_levelStem;

bool UsesOwnArchive() { return g_dllRedirect && !g_archive.empty() && _stricmp(g_archive.c_str(), "none") != 0; }

// The level's stem from the archive name level init built ("<stem>.dat"): a mod's "<stem>.level" decides.
void StartLevel(const char* archivePath) {
    g_dllRedirect = false;
    g_archive.clear();
    g_levelStem = archivePath ? Stem(archivePath) : std::string();
    if (g_levelStem.empty()) return;
    // the level being loaded = the campaign flow's current name (FUN_00430900 on 0x631608); a .lua level (scripthost.cpp)
    // names its data in its own header ("--@archive mission01"), without one = no level archive unless <stem>.dat exists
    bool lua = false;
    const char* level = reinterpret_cast<const char*(__thiscall*)(uint32_t)>(0x430900)(0x631608);
    std::string path, luaArchive = features::ScriptArchiveFor(level, lua);
    if (lua) {
        path = level;
        g_archive = luaArchive;
        if (g_archive.empty()) {
            if (GetFileAttributesA((GameDir() + "\\" + g_levelStem + ".dat").c_str()) != INVALID_FILE_ATTRIBUTES ||
                !features::ModFilePath((g_levelStem + ".dat").c_str()).empty())
                return;
            g_archive = "none";
        }
    } else {
        path = features::ModFilePath((g_levelStem + ".level").c_str());
        if (path.empty()) return;
        std::string head;
        if (!ReadAll(path, head)) return;
        head.resize(std::min<size_t>(head.size(), 2048));
        g_archive = ArchiveDirectiveIn(head);
    }
    if (g_archive.empty()) {
        features::ModLog("[fail] Mods: %s has no \"--@archive <level>\" line - the level's own data names are used",
                         path.c_str());
        return;
    }
    if (_stricmp(g_archive.c_str(), "none") != 0 &&
        GetFileAttributesA((GameDir() + "\\" + g_archive + ".dat").c_str()) == INVALID_FILE_ATTRIBUTES &&
        features::ModFilePath((g_archive + ".dat").c_str()).empty()) {
        features::ModLog("[fail] Mods: %s: --@archive %s - there is no %s.dat; the level's own data names are used",
                         path.c_str(), g_archive.c_str(), g_archive.c_str());
        g_archive.clear();
        return;
    }
    g_dllRedirect = true;
    dslog::Write("Mods: level %s - data from %s (%s)", g_levelStem.c_str(), g_archive.c_str(), path.c_str());
}

// ---- hooks ----
constexpr uint32_t kMissionDllLoadCall = 0x42202D, kLoadLibraryIat = 0x5DA054;
constexpr uint32_t kArchiveOpen = 0x4C3400;
constexpr uint32_t kArchiveOpenCalls[] = {0x41276F, 0x412879, 0x4130D0};
constexpr uint32_t kSchNameCall = 0x412963, kStrCopy = 0x4C2300;
constexpr uint32_t kMusicCall = 0x412A49, kMusicLoad = 0x4B33C0;
constexpr uint32_t kFasNameCall = 0x5096BB, kSprintf = 0x58C396;
constexpr uint32_t kOctreeFlagCall = 0x412E64, kStartFlag = 0x4C0CE0;
constexpr uint32_t kOctreeOpenCall = 0x4EA8B8, kGameFopen = 0x58C6C3;
constexpr uint32_t kOctreeFreeCall = 0x412E99, kOctreeFree = 0x4E2800;
constexpr uint32_t kOctreeLoadCall = 0x412EB1, kOctreeLoad = 0x4EA9B0;
constexpr uint32_t kRoutingLoadCall = 0x412FE7, kRoutingLoad = 0x4BCD10;
constexpr uint32_t kRoutingBuildCall = 0x413003, kRoutingBuild = 0x4BCE70;
// Script info "<scene>.si" (per-level flags the mission DLL toggles through API +0x1F0 -> FUN_00440430 -> entry
// [0x631848 +0x54][i] (0x54 B each, count +0x58)): LoadEnvironment FUN_00411970 (scene load, from the DLL's set-up via
// 0x421DAC) reads it with FUN_00432cc0(0x631848, name) at 0x411CD7 only when the campaign flow (0x631608) has a
// current entry ([0x631618] != -1) - i.e. the level was found in the campaign table by name. A mod's DLL level with a
// new name ("MyOp.dll") has none: the array stayed null and Mission01's script crashed at 0x43629E on its first +0x1F0
// call. The no-op debug print right after (0x411CE1, "DONE :LoadEnvironment") loads it for such levels.
constexpr uint32_t kSiPrintCall = 0x411CE1, kDebugPrint = 0x4031A0, kSiLoad = 0x432CC0, kSiObject = 0x631848;
constexpr uint32_t kWorld = 0x68008C;   // world +0x1BB4 scene name (set by the scene load), +0x1BBC level stem

bool g_modDll = false;   // this level's script DLL came from a mod

HMODULE WINAPI LoadMissionDll(const char* name) {
    const std::string own = name ? features::ModFilePath(name) : std::string();
    g_modDll = !own.empty();
    if (own.empty()) return LoadLibraryA(name);
    HMODULE m = LoadLibraryExA(own.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    const DWORD error = m ? 0 : GetLastError();
    if (m) {
        static std::unordered_set<std::string> logged;
        if (logged.insert(Upper(own)).second) features::ModLog("Mods: mission DLL %s from %s", name, own.c_str());
    } else {
        features::ModLog("[fail] Mods: mission DLL %s from %s didn't load (error %lu)", name, own.c_str(), error);
    }
    return m;
}

using ArchiveOpenFn = int(__cdecl*)(int, const char*, int, int);
int __cdecl OpenLevelArchive(int slot, const char* path, int a, int b) {
    const auto open = reinterpret_cast<ArchiveOpenFn>(kArchiveOpen);
    // first open of a level (0x41276F) = a new level: (0, 0); the later two reopen the same name with (1, 0)
    if (a == 0 && b == 0) StartLevel(path);
    if (!g_dllRedirect) return open(slot, path, a, b);
    if (_stricmp(g_archive.c_str(), "none") == 0) return 0;
    const std::string own = g_archive + ".dat";
    return open(slot, own.c_str(), a, b);
}

void __cdecl SchName(const char* src, char* dst) {
    if (UsesOwnArchive()) {
        const std::string own = g_archive + ".sch";
        reinterpret_cast<void(__cdecl*)(const char*, char*)>(kStrCopy)(own.c_str(), dst);
        return;
    }
    reinterpret_cast<void(__cdecl*)(const char*, char*)>(kStrCopy)(src, dst);
}

void* __fastcall LoadMusicBank(void* manager, void*, const char* path, int flag) {
    using Load = void*(__thiscall*)(void*, const char*, int);
    const std::string own = g_archive + "Music.sbk";
    return reinterpret_cast<Load>(kMusicLoad)(manager, UsesOwnArchive() ? own.c_str() : path, flag);
}

int __cdecl FasName(char* out, const char* format, const char* stem) {
    using Sprintf = int(__cdecl*)(char*, const char*, ...);
    if (UsesOwnArchive()) {
        const std::string own = Upper(g_archive);
        return reinterpret_cast<Sprintf>(kSprintf)(out, format, own.c_str());
    }
    return reinterpret_cast<Sprintf>(kSprintf)(out, format, stem);
}

// The level's scene name (world +0x1BB4, set by the scene load: "Mission01.env" style) -> its stem.
std::string SceneStem() {
    const auto* world = *reinterpret_cast<const uint8_t* const*>(kWorld);
    const char* scene = world ? *reinterpret_cast<const char* const*>(world + 0x1BB4) : nullptr;
    return scene && *scene ? Stem(scene) : std::string();
}

// A real file the mod folder ships (not one this module built from a reference)
bool ModShips(const std::string& name) {
    return !features::ModFilePath(name.c_str()).empty() && !g_built.count(Upper(BaseName(name)));
}

bool SceneFromMod() {
    const std::string stem = SceneStem();
    return !stem.empty() && !features::ModFilePath((stem + ".ENV").c_str()).empty();
}

bool g_octreeRebuild = false;
DWORD g_octreeStart = 0;

int __cdecl OctreeFlag(int letter) {
    g_octreeRebuild = false;
    // also when a mod's unit kits create more objects than the game's (mods_tables.cpp): the shipped .OCT then points
    // past the scene's own objects (vehicles fell through / sank into the ground)
    if (letter == 'c' && ((SceneFromMod() && !ModShips(SceneStem() + ".OCT")) || features::ModKitsGrown())) {
        g_octreeRebuild = true;
        g_octreeStart = GetTickCount();
        return 1;
    }
    return reinterpret_cast<int(__cdecl*)(int)>(kStartFlag)(letter);
}

void* __cdecl OctreeOpen(const char* name, const char* mode) {
    using Fopen = void*(__cdecl*)(const char*, const char*);
    if (!g_octreeRebuild) return reinterpret_cast<Fopen>(kGameFopen)(name, mode);
    const std::string path = TempDir() + "\\mod-scene-" + BaseName(name ? name : "scene.OCT");
    return reinterpret_cast<Fopen>(kGameFopen)(path.c_str(), mode);
}

void __fastcall OctreeFree(void* world, void*) {
    if (!g_octreeRebuild) reinterpret_cast<void(__thiscall*)(void*)>(kOctreeFree)(world);
}

int __fastcall OctreeLoad(void* world, void*) {
    if (g_octreeRebuild) {
        g_octreeRebuild = false;
        features::ModLog("Mods: octree for %s %s rebuilt from the scene in %lu ms",
                         SceneFromMod() ? "the mod scene" : "the scene (bigger unit kits)", SceneStem().c_str(),
                         GetTickCount() - g_octreeStart);
        return 1;
    }
    return reinterpret_cast<int(__thiscall*)(void*)>(kOctreeLoad)(world);
}

// The level's texts "<STEM>.BLF" (compiled locale; level init 0x4127E1: FUN_0040f820(world FUN_004e2b70 "<stem>.BLF",
// 1) -> FUN_004fc3d0; also CLocale load FUN_004fc600 thiscall([0x62F460], name) ret 4 at 0x40F9D4 / 0x42CE97 and
// FUN_004fc3d0 at 0x41F36A with the campaign's level name):
// a level whose data comes from another level's archive (.lua "--@archive", <stem>.level) has no BLF of its own, and
// the failed load left the game without its texts ("*no string*" all over the end screen). The archive level's BLF is
// loaded instead.
constexpr uint32_t kBlfLoad = 0x4FC600, kBlfBinary = 0x4FC3D0;
constexpr uint32_t kBlfLoadCalls[] = {0x40F9D4, 0x42CE97};
constexpr uint32_t kBlfBinaryCall = 0x41F36A;   // FUN_004fc3d0 thiscall(locale, name, check sources) with campaign +0x18
constexpr uint32_t kLevelTextsCall = 0x4127E1, kLevelTexts = 0x40F820;   // level init: FUN_0040f820(world "<stem>.BLF", 1)

// The archive level's name for `name` when it is the level being loaded (the campaign flow's current level, or the
// campaign's level name) and that level takes its data from another level's archive; else `name`. Decided here, not
// from StartLevel: the flow path (0x42CE97) loads the next level's texts before its init.
const char* BlfFor(const char* name) {
    if (!name) return name;
    const char* flow = reinterpret_cast<const char*(__thiscall*)(uint32_t)>(0x430900)(0x631608);
    const char* campaign = *reinterpret_cast<char* const*>(0x62F454) ? *reinterpret_cast<char* const*>(0x62F454) + 0x18 : nullptr;
    for (const char* level : {flow, campaign}) {
        if (!level || !*level || _stricmp(Stem(level).c_str(), Stem(name).c_str()) != 0) continue;
        bool lua = false;
        std::string archive = features::ScriptArchiveFor(level, lua);
        if (!lua) {
            const std::string path = features::ModFilePath((Stem(level) + ".level").c_str());
            std::string head;
            if (!path.empty() && ReadAll(path, head)) archive = ArchiveDirectiveIn(head.substr(0, 2048));
        }
        if (archive.empty() || _stricmp(archive.c_str(), "none") == 0) return name;
        static std::string own;
        own = Upper(archive);
        return own.c_str();
    }
    return name;
}

int __fastcall BlfLoad(void* locale, void*, const char* name) {
    return reinterpret_cast<int(__thiscall*)(void*, const char*)>(kBlfLoad)(locale, BlfFor(name));
}
int __cdecl LevelTexts(const char* name, int flag) {
    return reinterpret_cast<int(__cdecl*)(const char*, int)>(kLevelTexts)(BlfFor(name), flag);
}
int __fastcall BlfBinary(void* locale, void*, const char* name, int check) {
    return reinterpret_cast<int(__thiscall*)(void*, const char*, int)>(kBlfBinary)(locale, BlfFor(name), check);
}

void __cdecl AfterLoadEnvironment(const char* text) {
    reinterpret_cast<void(__cdecl*)(const char*)>(kDebugPrint)(text);
    if (!g_modDll || *reinterpret_cast<const int32_t*>(0x631618) != -1) return;   // (.lua levels: scripthost.cpp)
    const std::string stem = SceneStem();
    if (stem.empty()) return;
    const std::string si = stem + ".si";
    if (features::ModFilePath(si.c_str()).empty()) {
        std::string probe;
        if (!ReadGameFile(si, probe)) return;   // the scene has no script info
    }
    reinterpret_cast<void(__thiscall*)(uint32_t, const char*)>(kSiLoad)(kSiObject, si.c_str());
    features::ModLog("Mods: %s - script info %s loaded (the level isn't in the campaign)", stem.c_str(), si.c_str());
}

// ---- AI navigation for edited scenes ----
// Marker set [0x680100]: +0 array (allocated with a count in front), +4 count; marker 32 B in memory: f32 x, y, z,
// weight, u16 link[5] (+0x10, 0xFFFF none), u16 flags (+0x1A, bit 0 = generated for a quadtree cell), u8 (+0x1C).
// Quadtree root [0x6800FC], router [0x6800E8] (FUN_004b9340 thiscall(count)), walkable cell count [0x65B71C].
bool g_navRebuild = false;
std::string g_navScene;

int __cdecl RoutingLoad(const char* scene, int arg) {
    g_navRebuild = false;
    const std::string stem = scene ? Stem(scene) : std::string();
    if (!stem.empty() && SceneFromMod() && !ModShips(stem + ".QTD") && features::ModFilePath((stem + ".RMD").c_str()).empty()) {
        g_navRebuild = true;
        g_navScene = stem;
        return 0;   // level init builds the walk map + generated markers from the scene
    }
    return reinterpret_cast<int(__cdecl*)(const char*, int)>(kRoutingLoad)(scene, arg);
}

struct NavMarker {
    float x, y, z, weight;
    uint16_t links[5];
    uint16_t flags;
    uint8_t extra, pad[3];
};
static_assert(sizeof(NavMarker) == 32, "DS2 marker layout");

void ShiftCells(uint8_t* node, uint16_t by, int& cells) {
    for (int depth = 0; node && depth < 64; ++depth) {
        uint8_t* children = *reinterpret_cast<uint8_t**>(node + 4);
        if (!children) {
            if (node[0] & 2) { *reinterpret_cast<uint16_t*>(node + 2) += by; ++cells; }
            return;
        }
        for (int k = 0; k < 3; ++k) ShiftCells(children + k * 0x14, by, cells);
        node = children + 3 * 0x14;
    }
}

// The level's hand-placed markers: the first markers of the base level's route file (flags bit 0 clear; mission
// scripts address them by index) - put back in front of the generated ones like DS1's scripthost.cpp BuildNav: the
// generated markers (and every walkable cell's marker index, quadtree node +2) move up behind them, links into the
// generated part are matched by cell position, shipped generated<->generated links the builder doesn't make are kept,
// islands (FUN_004bac30, marker +0x1C) and the router (FUN_004b9340) are redone.
void MergeHandPlaced(const std::string& rmdName) {
    const DWORD t0 = GetTickCount();
    std::string file;
    auto* set = *reinterpret_cast<uint32_t**>(0x680100);
    int32_t head[3] = {};
    if (!set || !ReadGameFile(rmdName, file) || file.size() < 12) {
        features::ModLog("Mods: AI navigation for %s built from the scene (no %s: no hand-placed markers)", g_navScene.c_str(),
                         rmdName.c_str());
        return;
    }
    memcpy(head, file.data(), 12);
    if (head[0] != -1 || head[1] < 3 || head[2] < 0 || 12 + 29ull * static_cast<uint32_t>(head[2]) != file.size()) {
        features::ModLog("[fail] Mods: %s isn't a Desert Storm II route file - no hand-placed markers", rmdName.c_str());
        return;
    }
    const uint32_t oldCount = static_cast<uint32_t>(head[2]);
    std::vector<NavMarker> old(oldCount);
    for (uint32_t i = 0; i < oldCount; ++i) {
        const char* m = file.data() + 12 + 29 * i;
        NavMarker& o = old[i];
        memset(&o, 0, sizeof o);
        memcpy(&o.x, m, 16);          // x, y, z, weight
        memcpy(o.links, m + 16, 10);
        memcpy(&o.flags, m + 26, 2);
        o.extra = 0;
    }
    uint32_t hand = 0;
    while (hand < oldCount && !(old[hand].flags & 1)) ++hand;
    const auto* built = reinterpret_cast<const NavMarker*>(set[0]);
    const uint32_t n = set[1];
    if (!hand || !built || hand + n > 0xFFF0) {
        features::ModLog("Mods: AI navigation for %s built from the scene: %u markers (%s has no hand-placed ones)",
                         g_navScene.c_str(), n, rmdName.c_str());
        return;
    }
    auto key = [](float x, float z) { return (static_cast<uint64_t>(static_cast<uint32_t>(static_cast<int32_t>(lroundf(x)))) << 32) ^
                                             static_cast<uint32_t>(static_cast<int32_t>(lroundf(z))); };
    std::unordered_map<uint64_t, uint16_t> byPos;
    for (uint32_t i = 0; i < n; ++i) byPos[key(built[i].x, built[i].z)] = static_cast<uint16_t>(i);
    auto mapOld = [&](uint16_t l) -> uint16_t {          // a link of the old file -> index in the new set
        if (l == 0xFFFF || l >= oldCount) return 0xFFFF;
        if (l < hand) return l;
        auto it = byPos.find(key(old[l].x, old[l].z));
        if (it != byPos.end()) return static_cast<uint16_t>(it->second + hand);
        float best = 600.0f * 600.0f;
        uint16_t at = 0xFFFF;                             // the cell changed: nearest generated marker within 6 m
        for (uint32_t i = 0; i < n; ++i) {
            const float dx = built[i].x - old[l].x, dz = built[i].z - old[l].z, d = dx * dx + dz * dz;
            if (d < best) { best = d; at = static_cast<uint16_t>(i + hand); }
        }
        return at;
    };
    uint32_t tmp[8] = {};
    if (!reinterpret_cast<int(__thiscall*)(uint32_t*, uint32_t)>(0x4BA020)(tmp, hand + n) || !tmp[0]) return;
    auto* merged = reinterpret_cast<NavMarker*>(tmp[0]);
    for (uint32_t i = 0; i < n; ++i) {
        merged[hand + i] = built[i];
        for (uint16_t& l : merged[hand + i].links)
            if (l != 0xFFFF) l = static_cast<uint16_t>(l + hand);
    }
    int kept = 0;
    for (uint32_t i = hand; i < oldCount; ++i) {
        const uint16_t a = mapOld(static_cast<uint16_t>(i));
        if (a == 0xFFFF || byPos.find(key(old[i].x, old[i].z)) == byPos.end()) continue;
        for (uint16_t l : old[i].links) {
            if (l == 0xFFFF || l < hand || l >= oldCount || byPos.find(key(old[l].x, old[l].z)) == byPos.end()) continue;
            const uint16_t b = mapOld(l);
            bool has = false;
            for (uint16_t x : merged[a].links) has |= x == b;
            for (uint16_t& x : merged[a].links)
                if (!has && x == 0xFFFF) { x = b; has = true; ++kept; }
        }
    }
    int lost = 0;
    for (uint32_t m = 0; m < hand; ++m) {
        merged[m] = old[m];
        for (uint16_t& l : merged[m].links) {
            const uint16_t was = l;
            l = mapOld(l);
            if (was != 0xFFFF && l == 0xFFFF) ++lost;
            if (l == 0xFFFF || l < hand || was >= oldCount) continue;
            bool back = false;                                // the old generated marker linked back: so does the new
            for (uint16_t b : old[was].links) back |= b == m;
            if (!back) continue;
            NavMarker& q = merged[l];
            bool has = false;
            for (uint16_t b : q.links) has |= b == m;
            for (uint16_t& b : q.links)
                if (!has && b == 0xFFFF) { b = static_cast<uint16_t>(m); has = true; }
            for (uint16_t& b : q.links) {                    // all 5 used: drop one the original marker didn't have
                if (has) break;
                bool original = false;
                for (uint16_t o : old[was].links) original |= o != 0xFFFF && mapOld(o) == b;
                if (!original) { b = static_cast<uint16_t>(m); has = true; }
            }
        }
    }
    reinterpret_cast<void(__thiscall*)(uint32_t*)>(0x4BA0F0)(set);   // the built array
    set[0] = tmp[0];
    set[1] = hand + n;
    int cells = 0;
    ShiftCells(*reinterpret_cast<uint8_t**>(0x6800FC), static_cast<uint16_t>(hand), cells);
    for (uint32_t i = 0; i < hand + n; ++i) merged[i].extra = 0;   // the flood fill only visits unmarked markers
    reinterpret_cast<void(__thiscall*)(uint32_t*)>(0x4BAC30)(set);  // islands -> set +8
    if (void* router = *reinterpret_cast<void**>(0x6800E8))
        reinterpret_cast<void*(__thiscall*)(void*, uint32_t)>(0x4B9340)(router, hand + n);
    features::ModLog("Mods: AI navigation for the mod scene %s built from the scene: %u markers + %u hand-placed from %s "
                     "(%d links lost), %d cells, %d shipped links kept, %u islands, %lu ms", g_navScene.c_str(), n, hand,
                     rmdName.c_str(), lost, cells, kept, set[2], GetTickCount() - t0);
}

void __cdecl RoutingBuild(void* world) {
    reinterpret_cast<void(__cdecl*)(void*)>(kRoutingBuild)(world);
    if (!g_navRebuild) return;
    g_navRebuild = false;
    auto base = g_envBase.find(Upper(g_navScene));
    MergeHandPlaced((base != g_envBase.end() ? base->second : g_navScene) + ".RMD");
}
}  // namespace

void features::BuildModReferences() {
    g_built.clear();
    g_envBase.clear();
    ClearGameBankReferences();   // "bank" lines borrowed again below
    for (const ModFileRef& u : ModFilesWithExtension(".USES")) BuildUses(u);
    for (int kind = 0; kind < 2; ++kind)
        for (const ModFileRef& m : ModFilesWithExtension(kind ? ".RMDPATCH" : ".ENVPATCH")) {
            std::string patch, out, base, error;
            const std::string file = FileName(m.path);
            const std::string name = Upper(Stem(file)) + (kind ? ".RMD" : ".ENV");
            if (!ReadAll(m.path, patch) ||
                !(kind ? ApplyRmdPatch(patch, out, base, error) : ApplyEnvPatch(patch, out, base, error))) {
                ModLog("[fail] Mods: %s: %s doesn't fit this game: %s", m.mod.c_str(), file.c_str(),
                       error.empty() ? "can't read it" : error.c_str());
                continue;
            }
            if (!kind) g_envBase[Upper(Stem(file))] = Upper(Stem(base));
            if (AddBuilt(name, out, m.mod, kind ? "routes" : "scene"))
                ModLog("Mods: %s = %s + %s (%s)", name.c_str(), base.c_str(), file.c_str(), m.mod.c_str());
            else
                ModLog("Mods: %s: %s not used - %s ships %s itself", m.mod.c_str(), file.c_str(), m.mod.c_str(), name.c_str());
        }
    for (const ModFileRef& m : ModFilesWithExtension(".DLLPATCH")) {
        std::string patch, out, base, error;
        const std::string file = FileName(m.path);
        const std::string name = Upper(Stem(file)) + ".DLL";
        if (!ReadAll(m.path, patch) || !ApplyDllPatch(patch, out, base, error)) {
            ModLog("[fail] Mods: %s: %s doesn't fit this game: %s - the game's own DLL is used", m.mod.c_str(), file.c_str(),
                   error.empty() ? "can't read it" : error.c_str());
            continue;
        }
        if (AddBuilt(name, out, m.mod, "mission DLL"))
            ModLog("Mods: %s = %s + %s (%s)", name.c_str(), base.c_str(), file.c_str(), m.mod.c_str());
    }
    for (const ModFileRef& m : ModFilesWithExtension(".LEVEL")) {
        std::string head;
        ReadAll(m.path, head);
        const std::string archive = ArchiveDirectiveIn(head.substr(0, 2048));
        if (archive.empty())
            ModLog("[fail] Mods: %s: %s has no \"--@archive <level>\" line", m.mod.c_str(), FileName(m.path).c_str());
        else
            ModLog("Mods: %s: level %s uses the data of %s", m.mod.c_str(), Stem(FileName(m.path)).c_str(), archive.c_str());
    }
}

void features::ApplyModLevels() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!settings::Get().featMods) return;
    int ok = 0, total = 0;
    auto hook = [&](uint32_t site, const void* fn, uint32_t target) {
        ++total;
        if (patch::HookCall(site, fn, target)) ++ok;
        else ModLog("[fail] Mods: level code at 0x%08X not recognised", site);
    };
    ++total;
    if (patch::HookIndirectCall(kMissionDllLoadCall, reinterpret_cast<const void*>(&LoadMissionDll), kLoadLibraryIat)) ++ok;
    else ModLog("[fail] Mods: mission DLL load at 0x%08X not recognised - mission DLLs from mods won't load", kMissionDllLoadCall);
    for (uint32_t site : kArchiveOpenCalls) hook(site, reinterpret_cast<const void*>(&OpenLevelArchive), kArchiveOpen);
    hook(kSchNameCall, reinterpret_cast<const void*>(&SchName), kStrCopy);
    hook(kMusicCall, reinterpret_cast<const void*>(&LoadMusicBank), kMusicLoad);
    hook(kFasNameCall, reinterpret_cast<const void*>(&FasName), kSprintf);
    hook(kOctreeFlagCall, reinterpret_cast<const void*>(&OctreeFlag), kStartFlag);
    hook(kOctreeOpenCall, reinterpret_cast<const void*>(&OctreeOpen), kGameFopen);
    hook(kOctreeFreeCall, reinterpret_cast<const void*>(&OctreeFree), kOctreeFree);
    hook(kOctreeLoadCall, reinterpret_cast<const void*>(&OctreeLoad), kOctreeLoad);
    hook(kSiPrintCall, reinterpret_cast<const void*>(&AfterLoadEnvironment), kDebugPrint);
    hook(kRoutingLoadCall, reinterpret_cast<const void*>(&RoutingLoad), kRoutingLoad);
    hook(kRoutingBuildCall, reinterpret_cast<const void*>(&RoutingBuild), kRoutingBuild);
    for (uint32_t site : kBlfLoadCalls) hook(site, reinterpret_cast<const void*>(&BlfLoad), kBlfLoad);
    hook(kBlfBinaryCall, reinterpret_cast<const void*>(&BlfBinary), kBlfBinary);
    hook(kLevelTextsCall, reinterpret_cast<const void*>(&LevelTexts), kLevelTexts);
    dslog::Write("%s Mods: levels from mods - %d of %d hooks (mission DLLs, .level, edited scenes)",
                 ok == total ? "[ok]  " : "[fail]", ok, total);
}
