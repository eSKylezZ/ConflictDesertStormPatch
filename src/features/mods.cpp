// Loose-file mods: every file the game loads can be replaced (or added) by dropping it into a folder under Mods\.
//
// All game file access resolves names through FUN_004be810(name, search paths, flags) -> path in [0x63BF28] or 0:
// the archive index [0x63C534] (by name hash - catalog/chardata/missionN.dat ...) first, then "<game>\<name>" and
// "<game>\Data\<name>" on disk. Callers: FUN_004bebf0 (open: FUN_004bebd0 -> 22 callers, FUN_004bfb90 whole-file
// loads, meshes, tables, scripts) and FUN_004be7f0 (exists checks: the texture loader trying .dds/.tga/..., sounds).
// The entry is hooked: a name whose file name (path stripped, case-insensitive) exists in a mod folder resolves to that
// file, before the archives - so a mod adds new textures (.dds/.tga), meshes (.evo), animations (.prb), sounds ... that
// its table rows refer to (and can replace files the game resolves by name).
//
// Layout: <game>\Mods\<mod name>\...any sub folders...\<FILE.EXT>. Mods are applied in name order; when two mods ship
// the same file the later one wins (logged). Folders starting with '_' or '.' are skipped (switched off). The index is
// built once at start-up.
// Tables are additive: a mod's .txt whose name is a game file (CSkins_SAS_R.txt, Weaps.txt ...) holds only its NEW
// rows - they are appended to the game's own file (read from the .dat archives: 12-byte index {name hash, offset, size},
// hash = LFSR over the upper-cased name) and to the rows of other mods, and the merged file (in %TEMP%\DesertStormFix
// \merged) is what the game loads. So several mods can each add a skin to the same table.
// Archive mode: the game runs with [0x63B4C8] bit 0 set, and then its opener FUN_004bd8a0(file, path) (thiscall, ret 4)
// ignores the path's folder - it hashes the bare name and reads it from the archives (FUN_004bd5c0 on [0x63B4CC]), no
// disk at all. For a path inside Mods\ or the merged folder the bit is cleared for that call, so it takes its own disk
// branch (fopen "rb"; the file object keeps its flags at +0x130 for the reads).
// Note: a mission's own archive textures are bulk-loaded without the resolver - replacing an existing texture name does
// not reach those; new names (new skins / weapons) always do.
#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <initializer_list>
#include <set>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "features/features.h"

namespace {
constexpr uint32_t kResolve = 0x4BE810;
constexpr uint8_t kResolveEntry[] = {0xA1, 0x60, 0x83, 0x5F, 0x00};  // mov eax, [0x5F8360]
constexpr uint32_t kPathBuffer = 0x63BF28;                            // the resolver's result buffer
constexpr size_t kPathBufferSize = 0x600;

using ResolveFn = char*(__cdecl*)(const char*, void*, int);
ResolveFn g_resolve = nullptr;

constexpr uint32_t kOpen = 0x4BD8A0;
constexpr uint8_t kOpenEntry[] = {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x08, 0x02, 0x00, 0x00};
constexpr uint32_t kFileFlags = 0x63B4C8;  // bit 0 = archive mode
using OpenFn = int(__thiscall*)(void*, const char*);
OpenFn g_open = nullptr;
std::string g_modsRoot, g_mergedRoot;  // paths our resolver hands out start with one of these

struct ModFile {
    std::string path;
    int mod;
};
std::unordered_map<std::string, ModFile> g_files;  // upper-case file name -> file
std::vector<std::string> g_mods;
std::unordered_map<std::string, std::vector<ModFile>> g_tables;  // .txt parts per upper-case name, in mod order
// <TABLE>.replace.txt parts (key = "<TABLE>.TXT"): their rows replace every game row with the same first column
// (case-insensitive) at the first such row's place; "-KEY" deletes; new keys are appended. Later mods win per key.
std::unordered_map<std::string, std::vector<ModFile>> g_tableReplace;
// Streamed sounds (cutscene / objective speech, Data\Sound\<language>\*.psf): "AMERICAN\CS_M03_S01.PSF" for a file in
// a language folder (SAS = English, Delta = American), "CS_M03_S01.PSF" for one that serves every language.
std::unordered_map<std::string, std::string> g_streams;
std::unordered_map<uint32_t, std::string> g_texts;               // catalog hash -> text (display names)
// .strings "[M01]" sections: texts only for the mission whose text file (M01Strs.loc) the level loaded - missions
// reuse keys (MOV_TXT1 date, MOV_TXT2 place, SPEECH1..) with their own texts
std::unordered_map<std::string, std::unordered_map<uint32_t, std::string>> g_levelTexts;
std::string g_levelLoc;                                            // "M01" of the mission texts the level loaded
std::vector<ModFile> g_skinFiles;                                 // .skin: readable skin definitions
std::vector<features::SkinVariant> g_skinVariants;                // .skin files with "uniform name"
std::vector<ModFile> g_weaponFiles;                               // .weapon: readable weapon definitions
std::vector<ModFile> g_gearFiles, g_pouchFiles;                   // .gear / .pouches: loadout gear (gear.cpp)
std::vector<features::ModGearPiece> g_modGear;
std::vector<features::ModPouchSet> g_modPouchSets;
std::vector<ModFile> g_vehicleFiles;                              // .vehicle: vehicle skins / new vehicles (vehicles.cpp)
std::vector<ModFile> g_unitFiles;                                 // .unit: new soldier types (BuildUnits)
struct ModUnit {
    std::string id, model;
    std::vector<std::string> items;
};
std::vector<ModUnit> g_modUnits;                                  // built from .unit files (BorrowUnitData)
std::vector<features::ModVehicle> g_modVehicles;
std::vector<features::VehicleHandling> g_vehicleHandling;            // .vehicle "grip" / "spin damping"
std::unordered_map<std::string, bool> g_vehicleData;              // "UNIT|LEVEL.DAT" whose files were borrowed
std::vector<std::string> g_bankPacks;                             // .sch files: sound banks added to every level
// With the asset store (assetstore.cpp): banks of other levels that every level gets, by key (ElfHash "<NAME>.sbk") -
// served from the store as bank packs when a level cache is built (SoundCache) instead of copied into bank_*.sch files.
std::vector<uint32_t> g_storeBanks;
// Game data a mod uses without shipping it (built from the player's own install at start-up, BuildReferences):
// .uses lists, .envpatch / .rmdpatch = a scene / route network as changes against the game's own file.
std::vector<ModFile> g_usesFiles, g_envPatches, g_rmdPatches, g_dllPatches;
// .sch files named like a game level cache (MISSION1.SCH ...): their samples replace that level's samples with the
// same language + id (e.g. ModKit cli/ds_gc_audio.py: the GameCube's higher-rate sound effects). Upper-case name ->
// files in mod order (a later mod wins).
std::unordered_map<std::string, std::vector<std::string>> g_levelSounds;
std::unordered_map<std::string, std::string> g_soundCaches;       // upper-case level .sch -> merged file ("" = failed)

std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

// ---- mod report ----
// Release builds have no log, so what the mods did (and what went wrong: "[fail]" lines) also goes to
// Mods\DesertStormFix-report.txt - started afresh at every build of the mod state (start-up, MODS menu APPLY), later
// messages (weapon pictures at a level load) appended. A file at the top of Mods is not a mod (only folders are).
std::string g_reportPath;
int g_problems = 0;

void BeginReport(const std::string& modsRoot) {
    g_problems = 0;
    g_reportPath.clear();
    const DWORD attr = GetFileAttributesA(modsRoot.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) return;  // no Mods folder, no report
    g_reportPath = modsRoot + "\\DesertStormFix-report.txt";
    HANDLE f = CreateFileA(g_reportPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        g_reportPath.clear();
        return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char head[320];
    const int n = snprintf(head, sizeof head,
                           "DesertStormFix " DS_VERSION " - mod report, %04d-%02d-%02d %02d:%02d:%02d\r\n"
                           "Written when the game starts and when MODS > APPLY runs. Lines starting with [fail] are "
                           "problems, \"note\" lines are hints.\r\n\r\n",
                           t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    DWORD written = 0;
    WriteFile(f, head, static_cast<DWORD>(n), &written, nullptr);
    CloseHandle(f);
}

std::string GameDir();
bool ReadAll(const std::string& path, std::string& out);
uint32_t TextureHash(const std::string& name);
std::string Trim(const std::string& v);
std::string JoinRow(const std::vector<std::string>& row);

// A mod's .strings file: "KEY = text" lines (# comments; "\n" in a text = a new line). The text is found by the
// catalog's hash of the lower-cased key (TextureHash), so a game key (an objective, a briefing page, a message) is
// replaced and a new key is reachable by that hash - mission scripts and the editor compute the same value.
void LoadStrings(const std::string& path) {
    std::string text;
    if (!ReadAll(path, text)) return;
    int n = 0;
    std::string section;   // "" = every level, else "M01"
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        if (line[start] == '[') {   // "[M01]": the following texts only in that mission, "[all]": every level again
            const size_t end = line.find(']', start);
            section = Trim(line.substr(start + 1, end == std::string::npos ? std::string::npos : end - start - 1));
            for (char& c : section) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (section == "ALL") section.clear();
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        for (size_t k; (k = value.find("\\n")) != std::string::npos;) value.replace(k, 2, "\n");
        if (key.empty()) continue;
        (section.empty() ? g_texts : g_levelTexts[section])[TextureHash(key)] = value;
        ++n;
    }
    features::ModLog("Mods: %d text(s) from %s", n, path.substr(path.find_last_of("\\/") + 1).c_str());
}

uint32_t g_largestFile = 0;  // see ScanFolder
std::string g_largestName;

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
        if (key.size() > 12 && key.compare(key.size() - 12, 12, ".REPLACE.TXT") == 0) {
            g_tableReplace[key.substr(0, key.size() - 12) + ".TXT"].push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".TXT") == 0) {
            g_tables[key].push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 8 && key.compare(key.size() - 8, 8, ".STRINGS") == 0) {
            LoadStrings(path);
            ++count;
            continue;
        }
        if (key.size() > 5 && key.compare(key.size() - 5, 5, ".SKIN") == 0) {
            g_skinFiles.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 7 && key.compare(key.size() - 7, 7, ".WEAPON") == 0) {
            g_weaponFiles.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 5 && key.compare(key.size() - 5, 5, ".GEAR") == 0) {
            g_gearFiles.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 8 && key.compare(key.size() - 8, 8, ".VEHICLE") == 0) {
            g_vehicleFiles.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 5 && key.compare(key.size() - 5, 5, ".UNIT") == 0) {
            g_unitFiles.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 8 && key.compare(key.size() - 8, 8, ".POUCHES") == 0) {
            g_pouchFiles.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 5 && key.compare(key.size() - 5, 5, ".USES") == 0) {
            g_usesFiles.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 9 && key.compare(key.size() - 9, 9, ".ENVPATCH") == 0) {
            g_envPatches.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 9 && key.compare(key.size() - 9, 9, ".RMDPATCH") == 0) {
            g_rmdPatches.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 9 && key.compare(key.size() - 9, 9, ".DLLPATCH") == 0) {
            g_dllPatches.push_back({path, mod});
            ++count;
            continue;
        }
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".SCH") == 0) {
            if (GetFileAttributesA((GameDir() + "\\" + fd.cFileName).c_str()) != INVALID_FILE_ATTRIBUTES)
                g_levelSounds[key].push_back(path);   // named like a level cache: replaces its samples
            else
                g_bankPacks.push_back(path);
            ++count;
            continue;
        }
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".PSF") == 0) {
            std::string folder = Upper(dir.substr(dir.find_last_of("\\/") + 1));
            if (folder == "SAS" || folder == "UK" || folder == "BRITISH") folder = "ENGLISH";
            if (folder == "DELTA" || folder == "US") folder = "AMERICAN";
            static const char* const kLanguages[] = {"ENGLISH", "FRENCH", "GERMAN", "SPANISH", "ITALIAN", "JAPANESE",
                                                     "AMERICAN", "POLISH"};
            bool language = false;
            for (const char* l : kLanguages) language |= folder == l;
            g_streams[language ? folder + "\\" + key : key] = path;
            ++count;
            continue;
        }
        {   // the Global Heap is sized to the largest file a mod adds (memory.cpp); not ones the game never reads whole
            static const char* const kSkip[] = {".DLL", ".WAV", ".BLEND", ".BLEND1", ".EXE", ".ZIP", ".7Z", ".MP4", ".JSON", ".MD", ".PY"};
            const size_t dot = key.find_last_of('.');
            bool skip = false;
            for (const char* e : kSkip) skip |= dot != std::string::npos && key.compare(dot, std::string::npos, e) == 0;
            if (!skip && !fd.nFileSizeHigh && fd.nFileSizeLow > g_largestFile) {
                g_largestFile = fd.nFileSizeLow;
                g_largestName = fd.cFileName;
            }
        }
        auto it = g_files.find(key);
        if (it != g_files.end())
            features::ModLog("Mods: %s replaces %s's %s", g_mods[mod].c_str(), g_mods[it->second.mod].c_str(), fd.cFileName);
        g_files[key] = {path, mod};
        ++count;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

void BuildIndex() {
    const std::string root = g_modsRoot = GameDir() + "\\Mods";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((root + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> names;
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.' && fd.cFileName[0] != '_')
            names.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const std::string& name : names) {
        const int mod = static_cast<int>(g_mods.size());
        g_mods.push_back(name);
        int count = 0;
        ScanFolder(root + "\\" + name, mod, count);
        features::ModLog("Mods: %s - %d files", name.c_str(), count);
    }
}

// ---- additive tables ----
uint32_t ArchiveHash(const std::string& name) {
    uint32_t x = 1;
    for (unsigned char c : Upper(name))
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
            x = x << 1 | in;
        }
    return x;
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

// The game's own copy of `name` from the archives in the game folder (catalog.dat first: it holds the tables).
bool ReadGameFile(const std::string& name, std::string& out) {
    const uint32_t hash = ArchiveHash(name);
    std::vector<std::string> dats = {"catalog.dat"};
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "\\*.dat").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (_stricmp(fd.cFileName, "catalog.dat")) dats.push_back(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (const std::string& dat : dats) {
        HANDLE f = CreateFileA((GameDir() + "\\" + dat).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        std::vector<uint32_t> index(0x8000 / 4);
        DWORD got = 0;
        ReadFile(f, index.data(), 0x8000, &got, nullptr);
        const DWORD fileSize = GetFileSize(f, nullptr);
        bool found = false;
        for (size_t i = 0; i + 2 < got / 4; i += 3) {
            const uint32_t eh = index[i], offset = index[i + 1], size = index[i + 2];
            if (!eh && !offset && !size) break;
            if (eh != hash || offset + size > fileSize) continue;
            out.resize(size);
            SetFilePointer(f, static_cast<LONG>(offset), nullptr, FILE_BEGIN);
            found = ReadFile(f, out.data(), size, &got, nullptr) && got == size;
            break;
        }
        CloseHandle(f);
        if (found) return true;
    }
    return false;
}

std::string TempDir() {
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    std::string dir = std::string(tmp) + "DesertStormFix";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\merged";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

void AppendRows(std::string& text, const std::string& rows) {
    if (rows.empty()) return;
    if (!text.empty() && text.back() != '\n') text += "\r\n";
    text += rows;
}

// First column of a table row (trimmed, upper-case) - the key replace parts match on.
std::string RowKey(const std::string& line) {
    std::string k = Trim(line.substr(0, line.find(',')));
    CharUpperBuffA(k.data(), static_cast<DWORD>(k.size()));
    return k;
}

// Apply <TABLE>.replace.txt parts to a table's text: all rows of a replaced key give way to the mod's rows for it
// (at the first old row's place), "-KEY" removes the key, keys the table doesn't have are appended.
int ApplyReplacements(std::string& text, const std::vector<ModFile>& parts) {
    std::unordered_map<std::string, std::vector<std::string>> rows;  // key -> new rows (empty = delete)
    std::vector<std::string> order;                                   // keys in first-seen order (for appending)
    for (const ModFile& part : parts) {
        std::string body;
        if (!ReadAll(part.path, body)) continue;
        std::unordered_map<std::string, bool> seenHere;               // a later part replaces a key's rows as a whole
        for (size_t pos = 0; pos < body.size();) {
            size_t nl = body.find('\n', pos);
            if (nl == std::string::npos) nl = body.size();
            std::string line = body.substr(pos, nl - pos);
            pos = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (Trim(line).empty() || Trim(line)[0] == '#') continue;
            const bool remove = Trim(line)[0] == '-';
            const std::string key = RowKey(remove ? Trim(line).substr(1) : line);
            if (key.empty()) continue;
            if (!rows.count(key)) order.push_back(key);
            if (!seenHere[key]) rows[key].clear(), seenHere[key] = true;
            if (!remove) rows[key].push_back(line);
        }
    }
    if (rows.empty()) return 0;
    std::string out;
    std::unordered_map<std::string, bool> done;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto it = rows.find(RowKey(line));
        if (it == rows.end() || Trim(line).empty()) {
            out += line + "\r\n";
            continue;
        }
        if (done[it->first]) continue;  // further game rows of a replaced key
        done[it->first] = true;
        for (const std::string& r : it->second) out += r + "\r\n";
    }
    for (const std::string& key : order)
        if (!done[key])
            for (const std::string& r : rows[key]) out += r + "\r\n";
    text.swap(out);
    return static_cast<int>(rows.size());
}

// Every mod table becomes one file: the game's rows (if the game has that file) with the replace parts applied, then
// each mod's appended rows in order.
void MergeTables() {
    for (auto& [key, parts] : g_tableReplace)   // a table that is only replaced still needs its merged copy
        if (!g_tables.count(key)) g_tables[key];
    if (g_tables.empty()) return;
    const std::string dir = g_mergedRoot = TempDir();
    for (auto& [key, parts] : g_tables) {
        std::string text;
        auto replace = g_tableReplace.find(key);
        if (parts.empty() && replace == g_tableReplace.end()) continue;   // an empty entry (g_tables[key] read somewhere)
        const ModFile& first = !parts.empty() ? parts.front() : replace->second.front();
        std::string name = first.path.substr(first.path.find_last_of("\\/") + 1);
        if (parts.empty()) name = name.substr(0, name.size() - 12) + ".txt";   // X.replace.txt -> X.txt
        const bool game = ReadGameFile(name, text);
        int replaced = 0;
        if (replace != g_tableReplace.end()) replaced = ApplyReplacements(text, replace->second);
        for (const ModFile& part : parts) {
            std::string rows;
            if (ReadAll(part.path, rows)) AppendRows(text, rows);
        }
        if (!game && parts.size() == 1 && !replaced) {  // a new table: used as it is
            g_files[key] = parts.front();
            continue;
        }
        if (replaced)
            features::ModLog("Mods: %s - %d key(s) replaced by %zu file(s)", name.c_str(), replaced, replace->second.size());
        const std::string out = dir + "\\" + name;
        HANDLE f = CreateFileA(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            features::ModLog("[fail] Mods: can't write %s", out.c_str());
            continue;
        }
        DWORD written = 0;
        WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(f);
        g_files[key] = {out, parts.empty() ? replace->second.back().mod : parts.back().mod};
        features::ModLog("Mods: %s = %s + %zu mod part(s)", name.c_str(), game ? "game rows" : "no game file", parts.size());
    }
}

// ---- unit tables ----
// Units.txt / HStat.txt / UnitWpns.txt rows the game can't load safely (research/custom_units.md): a text field longer
// than the loader's buffer (35 / HStat name 39 chars), an unknown unit type / base stat / experience (the type falls
// to an unhandled 6, the other two make the game drop the whole table), a UnitWpns row for a unit that doesn't exist
// (whole table dropped). Such rows are left out of the merged table with a [fail] line. Column 14 (item slots) is
// raised to the unit's UnitWpns row count - more rows than slots = "Trying to allocate" box + table dropped; more slots
// than rows is harmless now (units.cpp skips empty slots) and only noted for mod rows. Runs after MergeTables, only
// when a mod touches one of the three tables.
std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> Fields(const std::string& line) {   // as the game's %[^','] sees them: not trimmed
    std::vector<std::string> out;
    for (size_t a = 0;;) {
        const size_t e = line.find(',', a);
        out.push_back(line.substr(a, e == std::string::npos ? std::string::npos : e - a));
        if (e == std::string::npos) return out;
        a = e + 1;
    }
}

bool TableText(const char* key, const char* name, std::string& text) {
    auto it = g_files.find(key);
    return it != g_files.end() ? ReadAll(it->second.path, text) : ReadGameFile(name, text);
}

void StoreTable(const char* key, const char* name, const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& l : lines) text += l + "\r\n";
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    const std::string path = g_mergedRoot + "\\" + name;
    if (!WriteAll(path, text)) {
        features::ModLog("[fail] Mods: can't write %s", path.c_str());
        return;
    }
    auto it = g_files.find(key);
    g_files[key] = {path, it != g_files.end() ? it->second.mod : -1};
}

void CheckUnitTables() {
    if (!g_tables.count("UNITS.TXT") && !g_tables.count("HSTAT.TXT") && !g_tables.count("UNITWPNS.TXT")) return;
    std::string unitsText, statsText, itemsText, gameUnits;
    if (!TableText("UNITS.TXT", "Units.txt", unitsText) || !TableText("HSTAT.TXT", "HStat.txt", statsText) ||
        !TableText("UNITWPNS.TXT", "UnitWpns.txt", itemsText)) {
        features::ModLog("[fail] Units: unit tables not readable - not checked");
        return;
    }
    ReadGameFile("Units.txt", gameUnits);
    std::set<std::string> gameRows;
    for (const std::string& l : Lines(gameUnits)) gameRows.insert(l);
    static const char* const kTypes[] = {"SOLDIER", "CIVILIAN", "GOAT HERDER", "EMIR", "MISSILE EXPERT", "POW"};
    static const char* const kExperience[] = {"Rookie", "Green", "Trained", "Regular", "Veteran", "Expert", "Professional",
                                              "Guard", "Crack", "Elite", "Specialist", "Skill One", "Skill Two",
                                              "Skill Three", "Skill Four"};
    auto known = [](const std::string& v, const char* const* list, size_t n) {
        for (size_t i = 0; i < n; ++i)
            if (v == list[i]) return true;
        return false;
    };
    constexpr int kMaxSlots = 25;

    bool statsChanged = false;
    std::vector<std::string> stats;
    std::set<std::string> statNames;
    for (const std::string& l : Lines(statsText)) {
        const std::string name = Fields(l)[0];
        if (!l.empty() && name.size() > 39) {
            features::ModLog("[fail] HStat.txt: row \"%s\" left out - its name is longer than 39 characters", name.c_str());
            statsChanged = true;
            continue;
        }
        if (!l.empty()) statNames.insert(name);
        stats.push_back(l);
    }

    bool unitsChanged = false;
    std::vector<std::string> units;
    std::set<std::string> unitNames;
    for (const std::string& l : Lines(unitsText)) {
        if (l.empty()) continue;
        const std::vector<std::string> f = Fields(l);
        std::string why;
        if (f.size() < 14) {
            why = "it has " + std::to_string(f.size()) + " of the 14 columns";
        } else {
            for (size_t i = 0; i < 13 && why.empty(); ++i)
                if (i != 8 && f[i].size() > 35)
                    why = "column " + std::to_string(i + 1) + " is longer than 35 characters";
            if (why.empty() && !known(f[1], kTypes, 6))
                why = "unit type \"" + f[1] + "\" isn't SOLDIER, CIVILIAN, GOAT HERDER, EMIR, MISSILE EXPERT or POW";
            if (why.empty() && !statNames.count(f[4])) why = "base stat \"" + f[4] + "\" isn't in HStat.txt";
            if (why.empty() && !known(f[9], kExperience, sizeof kExperience / sizeof *kExperience))
                why = "experience \"" + f[9] + "\" isn't a known level (Rookie, Green, Trained, Regular, Veteran ...)";
        }
        if (!why.empty()) {
            features::ModLog("[fail] Units.txt: row \"%s\" left out - %s", f[0].c_str(), why.c_str());
            unitsChanged = true;
            continue;
        }
        unitNames.insert(f[0]);
        units.push_back(l);
    }

    bool itemsChanged = false;
    std::vector<std::string> items;
    std::unordered_map<std::string, int> itemRows;
    for (const std::string& l : Lines(itemsText)) {
        if (l.empty()) continue;
        const std::vector<std::string> f = Fields(l);
        std::string why;
        if (f.size() < 4) why = "it has " + std::to_string(f.size()) + " of the 4 columns";
        for (size_t i = 0; i < 3 && i < f.size() && why.empty(); ++i)
            if (f[i].size() > 35) why = "column " + std::to_string(i + 1) + " is longer than 35 characters";
        if (why.empty() && !unitNames.count(f[0])) why = "unit \"" + f[0] + "\" isn't in Units.txt";
        if (why.empty() && itemRows[f[0]] >= kMaxSlots) why = "the unit already has 25 items";
        if (!why.empty()) {
            features::ModLog("[fail] UnitWpns.txt: row \"%s\" left out - %s", l.c_str(), why.c_str());
            itemsChanged = true;
            continue;
        }
        ++itemRows[f[0]];
        items.push_back(l);
    }

    for (std::string& l : units) {
        std::vector<std::string> f = Fields(l);
        const int slots = atoi(f[13].c_str()), rows = itemRows[f[0]];
        if (rows > slots || slots > kMaxSlots) {
            const int fixed = rows > slots ? rows : kMaxSlots;
            features::ModLog("Units.txt: %s - item slots (column 14) %d -> %d (%d UnitWpns.txt row(s))", f[0].c_str(), slots,
                             fixed, rows);
            l = l.substr(0, l.find_last_of(',') + 1) + std::to_string(fixed);
            unitsChanged = true;
        } else if (slots > rows && !gameRows.count(l)) {
            features::ModLog("note Units.txt: %s has %d item slot(s) (column 14) but %d UnitWpns.txt row(s) - the rest "
                             "stay empty", f[0].c_str(), slots, rows);
        }
    }
    if (statsChanged) StoreTable("HSTAT.TXT", "HStat.txt", stats);
    if (unitsChanged) StoreTable("UNITS.TXT", "Units.txt", units);
    if (itemsChanged) StoreTable("UNITWPNS.TXT", "UnitWpns.txt", items);
    features::ModLog("[ok]   Units: %zu units, %zu base stats, %zu unit items checked", units.size(), statNames.size(),
                     items.size());
}

// Vehicle.txt / VStat.txt / VehWpns.txt / Props.txt, same idea: rows left out when a field is longer than its buffer
// (Vehicle.txt seat / exit connectors 34 characters, other names 35; VStat.txt name 39, propulsion 35), the base stats
// (VStat.txt) or propulsion (Props.txt) is unknown, a VehWpns row names an unknown vehicle or a non-weapon (the game
// drops the whole table for these), a Props.txt row's type (column 2) isn't one of the 12 the game knows (FUN_00493e60:
// a wrong one stops the game at start-up). Column 1 is the name VStat.txt uses - a new name with a built-in type is a new
// propulsion with its own engine sound; column 4 is never read. Vehicle.txt column 26 (gun slots) is set to the vehicle's VehWpns row count - the
// game's own data always matches; an empty slot crashes the vehicle set-up (units.cpp). More than 8 crew is only noted
// (the game cuts it to 8 with a box).
void CheckVehicleTables() {
    if (!g_tables.count("VEHICLE.TXT") && !g_tables.count("VSTAT.TXT") && !g_tables.count("VEHWPNS.TXT") &&
        !g_tables.count("PROPS.TXT"))
        return;
    std::string vehiclesText, statsText, gunsText, propsText, weaponsText;
    if (!TableText("VEHICLE.TXT", "Vehicle.txt", vehiclesText) || !TableText("VSTAT.TXT", "VStat.txt", statsText) ||
        !TableText("VEHWPNS.TXT", "VehWpns.txt", gunsText) || !TableText("PROPS.TXT", "Props.txt", propsText) ||
        !TableText("WEAPS.TXT", "Weaps.txt", weaponsText)) {
        features::ModLog("[fail] Vehicles: vehicle tables not readable - not checked");
        return;
    }
    constexpr int kMaxSlots = 25;
    static const char* const kPropTypes[] = {"Wheels", "Tracks", "Infantry", "Ski", "Halftracks", "Airplane", "Scud",
                                             "Amphibious", "Goat", "Chinook", "Apache", "None"};
    bool propsChanged = false;
    std::vector<std::string> propRows;
    std::set<std::string> props, weapons;
    for (const std::string& l : Lines(propsText)) {
        if (l.empty()) continue;
        const std::vector<std::string> f = Fields(l);
        std::string why;
        if (f.size() < 4) why = "it has " + std::to_string(f.size()) + " of the 4 columns";
        for (size_t i = 0; i < 3 && i < f.size() && why.empty(); ++i)
            if (f[i].size() > 35) why = "column " + std::to_string(i + 1) + " is longer than 35 characters";
        if (why.empty() && std::find_if(std::begin(kPropTypes), std::end(kPropTypes),
                                        [&](const char* t) { return f[1] == t; }) == std::end(kPropTypes))
            why = "type \"" + f[1] + "\" (column 2) isn't Wheels, Tracks, Infantry, Ski, Halftracks, Airplane, Scud, "
                  "Amphibious, Goat, Chinook, Apache or None";
        if (!why.empty()) {
            features::ModLog("[fail] Props.txt: row \"%s\" left out - %s", f[0].c_str(), why.c_str());
            propsChanged = true;
            continue;
        }
        props.insert(f[0]);
        propRows.push_back(l);
    }
    for (const std::string& l : Lines(weaponsText))
        if (!l.empty()) weapons.insert(Fields(l)[0]);

    bool statsChanged = false;
    std::vector<std::string> stats;
    std::set<std::string> statNames;
    for (const std::string& l : Lines(statsText)) {
        if (l.empty()) continue;
        const std::vector<std::string> f = Fields(l);
        std::string why;
        if (f.size() < 27) why = "it has " + std::to_string(f.size()) + " of the 27 columns";
        else if (f[0].size() > 39) why = "its name is longer than 39 characters";
        else if (f[12].size() > 35 || !props.count(f[12])) why = "propulsion \"" + f[12] + "\" isn't in Props.txt";
        if (!why.empty()) {
            features::ModLog("[fail] VStat.txt: row \"%s\" left out - %s", f[0].c_str(), why.c_str());
            statsChanged = true;
            continue;
        }
        if (atoi(f[11].c_str()) > 8)
            features::ModLog("note VStat.txt: %s has %d crew - the game allows 8", f[0].c_str(), atoi(f[11].c_str()));
        statNames.insert(f[0]);
        stats.push_back(l);
    }

    bool vehiclesChanged = false;
    std::vector<std::string> vehicles;
    std::set<std::string> vehicleNames;
    for (const std::string& l : Lines(vehiclesText)) {
        if (l.empty()) continue;
        const std::vector<std::string> f = Fields(l);
        std::string why;
        if (f.size() < 27) why = "it has " + std::to_string(f.size()) + " of the 27 columns";
        for (size_t i = 0; i < 25 && i < f.size() && why.empty(); ++i) {
            if (i == 5) continue;   // a number
            const size_t max = i >= 6 && i <= 21 ? 34 : 35;
            if (f[i].size() > max) why = "column " + std::to_string(i + 1) + " is longer than " + std::to_string(max) + " characters";
        }
        if (why.empty() && !statNames.count(f[4])) why = "base stats \"" + f[4] + "\" aren't in VStat.txt";
        if (!why.empty()) {
            features::ModLog("[fail] Vehicle.txt: row \"%s\" left out - %s", f[0].c_str(), why.c_str());
            vehiclesChanged = true;
            continue;
        }
        vehicleNames.insert(f[0]);
        vehicles.push_back(l);
    }

    bool gunsChanged = false;
    std::vector<std::string> guns;
    std::unordered_map<std::string, int> gunRows;
    for (const std::string& l : Lines(gunsText)) {
        if (l.empty()) continue;
        const std::vector<std::string> f = Fields(l);
        std::string why;
        if (f.size() < 4) why = "it has " + std::to_string(f.size()) + " of the 4 columns";
        for (size_t i = 0; i < 3 && i < f.size() && why.empty(); ++i)
            if (f[i].size() > 35) why = "column " + std::to_string(i + 1) + " is longer than 35 characters";
        if (why.empty() && !vehicleNames.count(f[0])) why = "vehicle \"" + f[0] + "\" isn't in Vehicle.txt";
        if (why.empty() && (f[2] != "WEAPON" || !weapons.count(f[1]))) why = "\"" + f[1] + "\" isn't a weapon (Weaps.txt, kind WEAPON)";
        if (why.empty() && gunRows[f[0]] >= kMaxSlots) why = "the vehicle already has 25 guns";
        if (!why.empty()) {
            features::ModLog("[fail] VehWpns.txt: row \"%s\" left out - %s", l.c_str(), why.c_str());
            gunsChanged = true;
            continue;
        }
        ++gunRows[f[0]];
        guns.push_back(l);
    }

    for (std::string& l : vehicles) {
        const std::vector<std::string> f = Fields(l);
        const int slots = atoi(f[25].c_str()), rows = gunRows[f[0]];
        if (slots == rows) continue;
        features::ModLog("Vehicle.txt: %s - gun slots (column 26) %d -> %d (VehWpns.txt rows)", f[0].c_str(), slots, rows);
        std::vector<std::string> g = f;
        g[25] = std::to_string(rows);
        l = JoinRow(g);
        vehiclesChanged = true;
    }
    if (propsChanged) StoreTable("PROPS.TXT", "Props.txt", propRows);
    if (statsChanged) StoreTable("VSTAT.TXT", "VStat.txt", stats);
    if (vehiclesChanged) StoreTable("VEHICLE.TXT", "Vehicle.txt", vehicles);
    if (gunsChanged) StoreTable("VEHWPNS.TXT", "VehWpns.txt", guns);
    features::ModLog("[ok]   Vehicles: %zu vehicles, %zu base stats, %zu guns checked", vehicles.size(), statNames.size(),
                     guns.size());
}

// ---- sound banks ----
// A level's sound cache <level>.sch = "SCH\0", u32 size, chunks {tag, u32 size, data}: BANK (u32 key = ELF hash of
// "<NAME>.sbk", entries -> sample ids), PFSM / PFST (i32 language, u32 sample id, ...), IMUS. Read in order by
// FUN_0055f540. A weapon's shot bank (Weaps.txt col 64 + ".sbk") that is in no cache is looked for as a file - and not
// found (MP5SILENCEDSUBMG.sbk in Mission 1). Mods ship banks as small .sch packs (ModKit cli/ds_sound.py pack); every level
// cache the game opens gets the packs' banks and samples it doesn't have yet appended (merged copy in the temp dir).
struct Chunk {
    uint32_t tag;
    const char* data;  // header included
    uint32_t size;     // header included
};

std::vector<Chunk> Chunks(const std::string& file) {
    std::vector<Chunk> out;
    if (file.size() < 8 || memcmp(file.data(), "SCH", 4) != 0) return out;
    for (size_t p = 8; p + 8 <= file.size();) {
        uint32_t tag, size;
        memcpy(&tag, file.data() + p, 4);
        memcpy(&size, file.data() + p + 4, 4);
        if (p + 8 + size > file.size()) break;
        out.push_back({tag, file.data() + p, size + 8});
        p += 8 + size;
    }
    return out;
}

constexpr uint32_t kTagBank = 0x4B4E4142, kTagSample = 0x4D534650, kTagStream = 0x54534650;  // BANK, PFSM, PFST

uint64_t SampleKey(const Chunk& c) {  // language + id
    uint32_t lang = 0, id = 0;
    if (c.size >= 16) memcpy(&lang, c.data + 8, 4), memcpy(&id, c.data + 12, 4);
    return uint64_t(lang) << 32 | id;
}

uint32_t BankKey(const Chunk& c) {
    uint32_t key = 0;
    if (c.size >= 12) memcpy(&key, c.data + 8, 4);
    return key;
}

uint32_t ElfHash(const std::string& s);

// ---- squad voices in the other accent ----
// The squad's speech banks (Bradley / Ramirez / Jones / Foley.sbk, UNITS.TXT col 11 -> soldier +0x40) refer to sample
// ids shared by every language; each id has a PFSM per language (0 English = the SAS actors, 6 American = Delta's,
// 1-4 French ...) and the cache loader FUN_0055f540 keeps only the current speech language ([[0x63C9AC]+0x24], set by
// the uniform setter FUN_0040a320 in the English game) or -1. So the level cache gets, per hero bank, a copy for the
// other accent: <Name>_US.sbk (or _UK) whose ids have the top bit set, with that accent's samples as language -1.
const char* const kHeroBanks[4] = {"Bradley", "Ramirez", "Jones", "Foley"};
constexpr uint32_t kVoiceIdBit = 0x80000000;

int SpeechLanguageNow() {   // 0 UK or 6 US English; -1 = another language (one voice per soldier, no accents)
    auto* catalog = *reinterpret_cast<uint8_t**>(0x60EDCC);
    auto* sound = *reinterpret_cast<uint8_t**>(0x63C9AC);
    if (!catalog || !sound || *reinterpret_cast<int*>(catalog + 0x44) != 0) return -1;
    const int lang = *reinterpret_cast<int*>(sound + 0x24);
    return lang == 0 || lang == 6 ? lang : -1;
}

// A copy of bank `bank` (chunk) for language `lang`: ids remapped, those samples as language -1.
std::string AccentBank(const Chunk& bank, uint32_t newKey, int lang, const std::vector<Chunk>& chunks) {
    std::unordered_map<uint32_t, const Chunk*> byId;   // the accent's samples
    for (const Chunk& c : chunks)
        if (c.tag == kTagSample && static_cast<int>(SampleKey(c) >> 32) == lang) byId[static_cast<uint32_t>(SampleKey(c))] = &c;
    std::string copy(bank.data, bank.size), samples;
    memcpy(copy.data() + 8, &newKey, 4);
    std::unordered_map<uint32_t, bool> done;
    for (uint32_t o = 12; o + 4 <= copy.size(); ++o) {   // bank entries aren't 4-byte aligned: every offset is a candidate
        uint32_t v;
        memcpy(&v, copy.data() + o, 4);
        auto it = byId.find(v);
        if (it == byId.end()) continue;
        const uint32_t nv = v | kVoiceIdBit, all = 0xFFFFFFFF;
        memcpy(copy.data() + o, &nv, 4);
        if (done[v]) continue;
        done[v] = true;
        std::string c(it->second->data, it->second->size);
        memcpy(c.data() + 8, &all, 4);
        memcpy(c.data() + 12, &nv, 4);
        samples += c;
    }
    return done.empty() ? std::string() : copy + samples;
}

std::string SoundCache(const char* name) {
    const int speech = SpeechLanguageNow();
    const std::string key = Upper(name) + "|" + std::to_string(speech);
    auto cached = g_soundCaches.find(key);
    if (cached != g_soundCaches.end()) return cached->second;
    std::string& result = g_soundCaches[key];
    std::string file;
    // The level's own cache: from the asset store when it is on (rebuilt byte-identical from its chunks), else the file.
    const bool fromStore = features::AssetStoreSoundCache(name, &file);
    if (!fromStore && !ReadAll(GameDir() + "\\" + name, file)) return result;
    std::vector<Chunk> level = Chunks(file);
    if (level.empty()) return result;
    int replaced = 0;
    auto overlays = g_levelSounds.find(Upper(name));
    if (overlays != g_levelSounds.end()) {   // mod samples replace the level's (same language + id), later mods win
        std::vector<std::string> packs;
        std::unordered_map<uint64_t, std::string> swap;
        for (const std::string& path : overlays->second) {
            std::string pack;
            if (!ReadAll(path, pack)) continue;
            for (const Chunk& c : Chunks(pack))
                if (c.tag == kTagSample) swap[SampleKey(c)].assign(c.data, c.size);
        }
        if (!swap.empty()) {
            std::string rebuilt(file.data(), 8);
            for (const Chunk& c : level) {
                auto it = c.tag == kTagSample ? swap.find(SampleKey(c)) : swap.end();
                if (it == swap.end()) {
                    rebuilt.append(c.data, c.size);
                } else {
                    rebuilt += it->second;
                    ++replaced;
                }
            }
            file.swap(rebuilt);
            level = Chunks(file);
            features::ModLog("Mods: %s - %d sample(s) replaced from %zu file(s)", name, replaced,
                             overlays->second.size());
        }
    }
    std::unordered_map<uint32_t, bool> banks;
    std::unordered_map<uint64_t, bool> samples;
    for (const Chunk& c : level) {
        if (c.tag == kTagBank) banks[BankKey(c)] = true;
        if (c.tag == kTagSample || c.tag == kTagStream) samples[SampleKey(c)] = true;
    }
    std::string extra;
    int added = 0;
    if (speech >= 0) {
        for (const char* hero : kHeroBanks) {
            const uint32_t heroKey = ElfHash(std::string(hero) + ".sbk");
            const uint32_t altKey = ElfHash(std::string(hero) + (speech == 0 ? "_US" : "_UK") + ".sbk");
            for (const Chunk& c : level)
                if (c.tag == kTagBank && BankKey(c) == heroKey && !banks[altKey]) {
                    const std::string alt = AccentBank(c, altKey, speech == 0 ? 6 : 0, level);
                    if (!alt.empty()) {
                        banks[altKey] = true;
                        extra += alt;
                        ++added;
                    }
                }
        }
    }
    auto addPack = [&](const std::string& pack) {   // its banks / samples the cache doesn't have yet
        for (const Chunk& c : Chunks(pack)) {
            if (c.tag == kTagBank && !banks[BankKey(c)]) {
                banks[BankKey(c)] = true;
                extra.append(c.data, c.size);
                ++added;
            } else if ((c.tag == kTagSample || c.tag == kTagStream) && !samples[SampleKey(c)]) {
                samples[SampleKey(c)] = true;
                extra.append(c.data, c.size);
            }
        }
    };
    for (const std::string& path : g_bankPacks) {   // mods' packs (+ Borrow* copies when the store is off)
        std::string pack;
        if (ReadAll(path, pack)) addPack(pack);
    }
    int fromStoreBanks = 0;
    for (uint32_t bank : g_storeBanks) {   // other levels' banks, straight from the asset store
        std::string pack;
        if (banks[bank] || !features::AssetStoreBankPack(bank, &pack, nullptr)) continue;
        addPack(pack);
        ++fromStoreBanks;
    }
    if (fromStoreBanks) dslog::Write("Mods: %s - %d bank(s) of other levels from the asset store", name, fromStoreBanks);
    if (extra.empty() && !replaced) return result;   // (the store's copy = the file: the game reads the file)
    file += extra;
    const uint32_t body = static_cast<uint32_t>(file.size() - 8);
    memcpy(file.data() + 4, &body, 4);
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    const std::string out = g_mergedRoot + "\\" + (speech == 6 ? "us_" : "") + name;
    HANDLE f = CreateFileA(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return result;
    DWORD written = 0;
    const bool ok = WriteFile(f, file.data(), static_cast<DWORD>(file.size()), &written, nullptr) && written == file.size();
    CloseHandle(f);
    if (ok) {
        result = out;
        dslog::Write("Mods: %s + %d sound bank(s) (voices and mods), %zu bytes%s", name, added, extra.size(),
                     fromStore ? " - level cache from the asset store" : "");
    }
    return result;
}

// ---- readable weapon files (.weapon) ----
// "key = value" lines, # comments; key case, spaces and underscores don't matter. A weapon starts as a copy of
// `based on` (a Weaps.txt row - the game's or a mod's) and changes only what is listed; the plugin writes the row.
// `sound` / `silenced` borrow another weapon's shot sound: its bank is copied out of whichever level sound cache has it
// (with its samples) and added to every level, so it plays everywhere.
std::string Trim(const std::string& v) {
    const size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
}

std::string KeyOf(const std::string& k) {
    std::string out;
    for (char c : k)
        if (c != ' ' && c != '_' && c != '-' && c != '\t') out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> SplitRow(const std::string& line) {
    std::vector<std::string> col;
    for (size_t a = 0;;) {
        const size_t e = line.find(',', a);
        col.push_back(Trim(line.substr(a, e == std::string::npos ? std::string::npos : e - a)));
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return col;
}

bool Yes(const std::string& v) {
    const std::string k = KeyOf(v);
    return k == "yes" || k == "on" || k == "true" || k == "1";
}

// Weaps.txt columns (0-based) with a name; anything else can be set as "column N" (1-based, like a spreadsheet).
struct Field {
    const char* key;
    int col;
};
const Field kFields[] = {
    {"model", 1},        {"hudicon", 4},      {"icon", 4},           {"type", 5},         {"category", 5},
    {"timebetweenshots", 10}, {"fireinterval", 10}, {"magazines", 14}, {"magazinesize", 15}, {"magazine", 15},
    {"reloadtime", 16},  {"damage", 22},      {"range", 23},         {"hearingrange", 64}, {"soundbank", 63},
    // animations (.prb names without extension): standing / prone aim and reload, throw
    {"aimanimation", 53}, {"reloadanimation", 54}, {"throwanimation", 55}, {"proneaimanimation", 56},
    {"pronereloadanimation", 57}, {"pronethrowanimation", 58},
};
constexpr int kColMuzzleFlash = 35, kColSound = 63, kColHearing = 64, kWeapsColumns = 75;
constexpr const char* kSilencedDonor = "US_WPN_MP5SilencedSubMG";

std::vector<std::string> g_modWeaponIds;  // ids made by .weapon files (and game weapons a .weapon makes choosable)
std::vector<std::vector<std::string>> g_weaponRows;  // Weaps.txt rows known so far (game + mods)
std::unordered_map<std::string, int> g_weaponFactions;     // upper-case id -> features::kFaction* mask (.weapon "faction")
std::unordered_map<std::string, std::string> g_weaponBase;  // upper-case id -> "based on" (factions are inherited)
std::unordered_map<std::string, int> g_weaponMagazines;
std::unordered_map<std::string, std::string> g_skinVoices;   // upper-case skin texture -> voice bank name
std::unordered_map<std::string, std::string> g_skinNormals;  // upper-case skin texture -> normal map (.skin "normal map")
std::unordered_map<std::string, std::string> g_skinBodies;  // upper-case skin texture -> body model (.skin "body")    // upper-case id -> "magazines" set in its .weapon file

// "SAS, Delta" / "Russian Iraqi" / "allied" / "eastern" / "all" -> mask (0 = nothing recognised).
int ParseFactions(const std::string& v) {
    int mask = 0;
    std::string word;
    for (size_t i = 0; i <= v.size(); ++i) {
        const char c = i < v.size() ? static_cast<char>(tolower(static_cast<unsigned char>(v[i]))) : ',';
        if (isalpha(static_cast<unsigned char>(c))) {
            word += c;
            continue;
        }
        if (word == "sas" || word == "uk" || word == "british") mask |= features::kFactionSas;
        else if (word == "delta" || word == "us" || word == "american") mask |= features::kFactionDelta;
        else if (word == "russian" || word == "russia" || word == "ru" || word == "spetsnaz") mask |= features::kFactionRussian;
        else if (word == "iraqi" || word == "iraq" || word == "ir") mask |= features::kFactionIraqi;
        else if (word == "allied" || word == "allies" || word == "coalition") mask |= features::kFactionSas | features::kFactionDelta;
        else if (word == "eastern" || word == "enemy") mask |= features::kFactionRussian | features::kFactionIraqi;
        else if (word == "all" || word == "any") mask |= 15;
        word.clear();
    }
    return mask;
}

const std::vector<std::string>* WeaponRow(const std::string& id) {
    for (const auto& r : g_weaponRows)
        if (!r.empty() && _stricmp(r[0].c_str(), id.c_str()) == 0) return &r;
    return nullptr;
}

void LoadWeaponRows() {
    std::string text;
    ReadGameFile("Weaps.txt", text);
    if (auto rep = g_tableReplace.find("WEAPS.TXT"); rep != g_tableReplace.end()) ApplyReplacements(text, rep->second);
    auto it = g_tables.find("WEAPS.TXT");
    if (it != g_tables.end())
        for (const ModFile& part : it->second) {
            std::string rows;
            if (ReadAll(part.path, rows)) AppendRows(text, rows);
        }
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::string line = Trim(text.substr(pos, nl - pos));
        if (!line.empty()) g_weaponRows.push_back(SplitRow(line));
        pos = nl + 1;
    }
}

uint32_t ElfHash(const std::string& s) {  // FUN_004b9bf0, the sound bank key (case-sensitive)
    uint32_t h = 0;
    for (unsigned char c : s) {
        h = (h << 4) + c;
        if (const uint32_t g = h & 0xF0000000) h = (h & 0x0FFFFFFF) ^ (g >> 24);
    }
    return h;
}

// Copies bank <name>.sbk and the samples it refers to out of the first game sound cache that has it into a bank pack.
// With the asset store on, nothing is copied: the bank's key goes into g_storeBanks and SoundCache adds it (with its
// samples) to every level cache from the store. Returns true when the level caches get something new.
std::vector<std::string> g_borrowed;
bool BorrowBank(const std::string& name) {
    for (const std::string& b : g_borrowed)
        if (b == name) return false;
    g_borrowed.push_back(name);
    const uint32_t key = ElfHash(name + ".sbk");
    for (const std::string& path : g_bankPacks) {  // a mod's own .sch already brings it
        std::string pack;
        if (!ReadAll(path, pack)) continue;
        for (const Chunk& c : Chunks(pack))
            if (c.tag == kTagBank && BankKey(c) == key) {
                features::ModLog("Mods: sound bank %s from %s", name.c_str(), path.c_str());
                return false;
            }
    }
    if (features::AssetStoreSound()) {
        std::string cache;
        if (!features::AssetStoreHasBank(key, &cache)) {
            features::ModLog("[fail] Mods: sound bank %s is in no level sound cache", name.c_str());
            return false;
        }
        if (std::find(g_storeBanks.begin(), g_storeBanks.end(), key) != g_storeBanks.end()) return false;
        g_storeBanks.push_back(key);
        features::ModLog("Mods: sound %s served by the asset store (from %s)", name.c_str(), cache.c_str());
        return true;
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "\\*.sch").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<std::string> caches;
    do caches.push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    for (const std::string& cache : caches) {
        std::string file;
        if (!ReadAll(GameDir() + "\\" + cache, file)) continue;
        const std::vector<Chunk> chunks = Chunks(file);
        const Chunk* bank = nullptr;
        for (const Chunk& c : chunks)
            if (c.tag == kTagBank && BankKey(c) == key) bank = &c;
        if (!bank) continue;
        std::string pack(bank->data, bank->size);
        std::unordered_map<uint32_t, bool> refs;  // bank entries aren't 4-byte aligned: every offset is a candidate
        for (uint32_t o = 12; o + 4 <= bank->size; ++o) {
            uint32_t v;
            memcpy(&v, bank->data + o, 4);
            refs[v] = true;
        }
        int samples = 0;
        for (const Chunk& c : chunks)
            if ((c.tag == kTagSample || c.tag == kTagStream) && refs.count(static_cast<uint32_t>(SampleKey(c)))) {
                pack.append(c.data, c.size);
                ++samples;
            }
        std::string out("SCH\0", 4);
        const uint32_t body = static_cast<uint32_t>(pack.size());
        out.append(reinterpret_cast<const char*>(&body), 4);
        out += pack;
        if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
        const std::string path = g_mergedRoot + "\\bank_" + name + ".sch";
        if (WriteAll(path, out)) {
            g_bankPacks.push_back(path);
            features::ModLog("Mods: sound %s borrowed from %s (%d samples)", name.c_str(), cache.c_str(), samples);
            return true;
        }
        return false;
    }
    features::ModLog("[fail] Mods: sound bank %s is in no level sound cache", name.c_str());
    return false;
}

// A retextured copy of a mesh: <newName>.evo = <model>.evo (from the archives) with the model name in every object
// header (EOBJ +8, char[64]) and the named texture slots of its TEXL lists (u32 n + n x {u32 hash, char[64] name};
// FUN_0054af90 registers each by name, the materials match by the hash = TextureHash) replaced. "texture = X" replaces the slot named like the
// model (else the first), "texture <slot name> = X" that slot.
// ---- custom sounds (WAV -> the game's ADPCM samples) ----
// Sample chunk PFSM: i32 language (-1), u32 id, u32 data size, u16 rate, u8 bits (4 = ADPCM), u8 pad, data. ADPCM
// (decoder FUN_00561ab0 / FUN_00561d50): 16-byte blocks = header (high nibble filter, low nibble shift) + 15 bytes =
// 30 samples, low nibble first; nibble n -> (n << 12) * 2^-shift - c0*s1 - c1*s2 (table 0x5FE64C, the PlayStation
// VAG filters). Gun banks (every weapon's shot bank has this layout) refer to 4 samples in order: click, distant tail,
// shot variation 1, shot variation 2.
const double kFilters[5][2] = {{0.0, 0.0}, {-0.9375, 0.0}, {-1.796875, 0.8125}, {-1.53125, 0.859375}, {-1.90625, 0.9375}};

struct Pcm {
    std::vector<int16_t> samples;  // mono
    uint32_t rate = 0;
};

bool ReadWav(const std::string& path, Pcm& out) {
    std::string f;
    if (!ReadAll(path, f) || f.size() < 12 || memcmp(f.data(), "RIFF", 4) || memcmp(f.data() + 8, "WAVE", 4)) return false;
    uint16_t format = 0, channels = 0, bits = 0;
    const char* data = nullptr;
    uint32_t dataSize = 0;
    for (size_t p = 12; p + 8 <= f.size();) {
        uint32_t size;
        memcpy(&size, f.data() + p + 4, 4);
        if (!memcmp(f.data() + p, "fmt ", 4) && size >= 16) {
            memcpy(&format, f.data() + p + 8, 2);
            memcpy(&channels, f.data() + p + 10, 2);
            memcpy(&out.rate, f.data() + p + 12, 4);
            memcpy(&bits, f.data() + p + 22, 2);
            if (format == 0xFFFE && size >= 26) memcpy(&format, f.data() + p + 32, 2);  // WAVE_FORMAT_EXTENSIBLE
        } else if (!memcmp(f.data() + p, "data", 4)) {
            data = f.data() + p + 8;
            dataSize = static_cast<uint32_t>(std::min<size_t>(size, f.size() - p - 8));
        }
        p += 8 + size + (size & 1);
    }
    if (!data || !channels || !out.rate || out.rate > 65535 || !(format == 1 || format == 3)) return false;
    const uint32_t step = bits / 8 * channels, frames = step ? dataSize / step : 0;
    out.samples.resize(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        double sum = 0;
        for (uint16_t c = 0; c < channels; ++c) {
            const char* s = data + i * step + c * (bits / 8);
            double v = 0;
            if (format == 3 && bits == 32) { float x; memcpy(&x, s, 4); v = x * 32767.0; }
            else if (bits == 8) v = (static_cast<uint8_t>(*s) - 128) * 256.0;
            else if (bits == 16) { int16_t x; memcpy(&x, s, 2); v = x; }
            else if (bits == 24) v = static_cast<int32_t>((static_cast<uint8_t>(s[0]) << 8) | (static_cast<uint8_t>(s[1]) << 16) | (static_cast<uint32_t>(static_cast<uint8_t>(s[2])) << 24)) / 65536.0;
            else if (bits == 32) { int32_t x; memcpy(&x, s, 4); v = x / 65536.0; }
            sum += v;
        }
        out.samples[i] = static_cast<int16_t>(std::max(-32768.0, std::min(32767.0, sum / channels)));
    }
    return frames > 0;
}

// One block: best filter + shift by trial encoding against the decoder's own history.
void EncodeBlock(const int16_t* in, double& s1, double& s2, uint8_t out[16]) {
    double bestErr = 1e300, bestS1 = 0, bestS2 = 0;
    for (int f = 0; f < 5; ++f) {
        double peak = 0, p1 = s1, p2 = s2;  // ideal residual peak for this filter
        for (int i = 0; i < 30; ++i) {
            const double r = in[i] + kFilters[f][0] * p1 + kFilters[f][1] * p2;
            peak = std::max(peak, std::fabs(r));
            p2 = p1, p1 = in[i];
        }
        int shift = 12;
        while (shift > 0 && peak * (1 << shift) / 4096.0 > 7.0) --shift;
        uint8_t block[16] = {static_cast<uint8_t>(f << 4 | shift)};
        double d1 = s1, d2 = s2, err = 0;
        for (int i = 0; i < 30; ++i) {
            const double pred = -kFilters[f][0] * d1 - kFilters[f][1] * d2;
            int n = static_cast<int>(std::lround((in[i] - pred) * (1 << shift) / 4096.0));
            n = std::max(-8, std::min(7, n));
            double v = n * 4096.0 / (1 << shift) + pred;
            v = std::max(-32768.0, std::min(32767.0, v));
            err += (v - in[i]) * (v - in[i]);
            d2 = d1, d1 = v;
            block[1 + i / 2] |= static_cast<uint8_t>((n & 15) << (i & 1 ? 4 : 0));
        }
        if (err < bestErr) bestErr = err, bestS1 = d1, bestS2 = d2, memcpy(out, block, 16);
    }
    s1 = bestS1, s2 = bestS2;
}

// A PFSM chunk (header included) for `pcm` under sample id `id`.
std::string EncodeSample(const Pcm& pcm, uint32_t id) {
    std::string data;
    double s1 = 0, s2 = 0;
    for (size_t i = 0; i < pcm.samples.size(); i += 30) {
        int16_t block[30] = {};
        memcpy(block, pcm.samples.data() + i, std::min<size_t>(30, pcm.samples.size() - i) * 2);
        uint8_t out[16];
        EncodeBlock(block, s1, s2, out);
        data.append(reinterpret_cast<const char*>(out), 16);
    }
    std::string c = "PFSM";
    auto put = [&c](uint32_t v) { c.append(reinterpret_cast<const char*>(&v), 4); };
    put(static_cast<uint32_t>(16 + data.size()));
    put(0xFFFFFFFF);
    put(id);
    put(static_cast<uint32_t>(data.size()));
    put(pcm.rate | 4u << 16 | 0xCCu << 24);
    return c + data;
}

// The chunks of bank <name>.sbk and its samples, from the first game sound cache that has it.
bool FindBank(const std::string& name, std::string& bank, std::vector<std::pair<uint32_t, std::string>>& samples) {
    const uint32_t key = ElfHash(name + ".sbk");
    if (features::AssetStoreSound()) {   // the store's bank pack: the bank + its samples (no .sch scanning)
        std::string pack;
        if (!features::AssetStoreBankPack(key, &pack, nullptr)) return false;
        bank.clear();
        samples.clear();
        for (const Chunk& c : Chunks(pack)) {
            if (c.tag == kTagBank && bank.empty()) bank.assign(c.data, c.size);
            else if (c.tag == kTagSample || c.tag == kTagStream)
                samples.push_back({static_cast<uint32_t>(SampleKey(c)), std::string(c.data, c.size)});
        }
        return !bank.empty();
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "\\*.sch").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<std::string> caches;
    do caches.push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    for (const std::string& cache : caches) {
        std::string file;
        if (!ReadAll(GameDir() + "\\" + cache, file)) continue;
        const std::vector<Chunk> chunks = Chunks(file);
        const Chunk* b = nullptr;
        for (const Chunk& c : chunks)
            if (c.tag == kTagBank && BankKey(c) == key) b = &c;
        if (!b) continue;
        bank.assign(b->data, b->size);
        std::unordered_map<uint32_t, bool> refs;  // bank entries aren't 4-byte aligned: every offset is a candidate
        for (uint32_t o = 12; o + 4 <= b->size; ++o) {
            uint32_t v;
            memcpy(&v, b->data + o, 4);
            refs[v] = true;
        }
        samples.clear();
        for (const Chunk& c : chunks)
            if ((c.tag == kTagSample || c.tag == kTagStream) && refs.count(static_cast<uint32_t>(SampleKey(c))))
                samples.push_back({static_cast<uint32_t>(SampleKey(c)), std::string(c.data, c.size)});
        return true;
    }
    return false;
}

// Sample ids in the order the bank refers to them.
std::vector<uint32_t> BankSampleOrder(const std::string& bank, const std::vector<std::pair<uint32_t, std::string>>& samples) {
    std::vector<uint32_t> order;
    for (uint32_t o = 12; o + 4 <= bank.size(); ++o) {
        uint32_t v;
        memcpy(&v, bank.data() + o, 4);
        for (const auto& s : samples)
            if (s.first == v && std::find(order.begin(), order.end(), v) == order.end()) order.push_back(v);
    }
    return order;
}

void AddBankPack(const std::string& name, const std::string& chunks) {
    std::string out("SCH\0", 4);
    const uint32_t body = static_cast<uint32_t>(chunks.size());
    out.append(reinterpret_cast<const char*>(&body), 4);
    out += chunks;
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    const std::string path = g_mergedRoot + "\\bank_" + name + ".sch";
    if (WriteAll(path, out)) g_bankPacks.push_back(path);
}

// A new bank <newName> = bank <donor> with its shot variations (and optionally the tail) replaced by WAV files from
// the mod folder. Returns false (and logs) if anything is missing.
bool MakeSoundBank(const std::string& donor, const std::string& newName, const std::string& dir,
                   const std::vector<std::string>& shots, const std::string& tail, const std::string& file) {
    std::string bank;
    std::vector<std::pair<uint32_t, std::string>> samples;
    if (!FindBank(donor, bank, samples)) {
        features::ModLog("[fail] Mods: %s - sound %s not found", file.c_str(), donor.c_str());
        return false;
    }
    const std::vector<uint32_t> order = BankSampleOrder(bank, samples);
    if (order.size() < 3) {
        features::ModLog("[fail] Mods: %s - sound %s isn't a gun's (%zu samples)", file.c_str(), donor.c_str(), order.size());
        return false;
    }
    std::unordered_map<uint32_t, std::string> replace;  // old sample id -> WAV file
    if (!shots.empty())
        for (size_t i = 2; i < order.size(); ++i) replace[order[i]] = shots[(i - 2) % shots.size()];
    if (!tail.empty()) replace[order[1]] = tail;
    std::string chunks;
    const uint32_t key = ElfHash(newName + ".sbk");
    memcpy(&bank[8], &key, 4);
    int n = 0;
    for (const auto& [id, data] : samples) {
        auto it = replace.find(id);
        if (it == replace.end()) {
            chunks += data;  // kept (the click; the tail unless replaced)
            continue;
        }
        Pcm pcm;
        if (!ReadWav(dir + "\\" + it->second, pcm)) {
            features::ModLog("[fail] Mods: %s - can't read %s (PCM or float WAV, up to 65535 Hz)", file.c_str(),
                         it->second.c_str());
            return false;
        }
        const uint32_t newId = ElfHash(newName + "#" + std::to_string(++n)) | 0x10000000;
        for (size_t o = 12; o + 4 <= bank.size(); ++o)
            if (!memcmp(&bank[o], &id, 4)) memcpy(&bank[o], &newId, 4);
        chunks += EncodeSample(pcm, newId);
        features::ModLog("Mods: sound %s: %s (%u Hz, %.2f s)", newName.c_str(), it->second.c_str(), pcm.rate,
                     pcm.samples.size() / static_cast<double>(pcm.rate));
    }
    AddBankPack(newName, bank + chunks);
    g_borrowed.push_back(newName);  // provided: the kit bank pass needn't look for it
    return true;
}

// A .skin's own voice: bank <newName> = squad bank <hero> ("Bradley", "Foley", "Ramirez", "Jones") in one accent
// (language 0 = the SAS actors, 6 = Delta's), every sample under a new id as language -1 so it loads in any level and
// game language, lines replaced by WAV files ("voice line N", N = the bank's samples in order from 1, as the ModKit's
// ds_sound.py wav numbers them). The pack is added to every level's sound cache that lacks it.
bool MakeVoiceBank(const std::string& hero, int lang, const std::string& newName, const std::string& dir,
                   const std::unordered_map<int, std::string>& lines, const std::string& file) {
    std::string bank;
    std::vector<std::pair<uint32_t, std::string>> samples;
    if (!FindBank(hero, bank, samples)) {
        features::ModLog("[fail] Mods: %s - voice of %s not found in any level sound cache", file.c_str(), hero.c_str());
        return false;
    }
    const std::vector<uint32_t> order = BankSampleOrder(bank, samples);
    const uint32_t key = ElfHash(newName + ".sbk");
    memcpy(&bank[8], &key, 4);
    std::string chunks;
    for (const auto& [line, wav] : lines)
        if (line < 1 || line > static_cast<int>(order.size()))
            features::ModLog("[fail] Mods: %s - voice line %d: %s has lines 1-%zu", file.c_str(), line, hero.c_str(), order.size());
    for (size_t n = 0; n < order.size(); ++n) {
        const uint32_t id = order[n], newId = ElfHash(newName + "#" + std::to_string(n + 1)) | 0x10000000;
        std::string chunk;
        auto wav = lines.find(static_cast<int>(n + 1));
        if (wav != lines.end()) {
            Pcm pcm;
            if (!ReadWav(dir + "\\" + wav->second, pcm)) {
                features::ModLog("[fail] Mods: %s - can't read %s (PCM or float WAV, up to 65535 Hz)", file.c_str(),
                                 wav->second.c_str());
                return false;
            }
            chunk = EncodeSample(pcm, newId);
        } else {   // the accent's sample, else the one all languages share
            const std::string* pick = nullptr;
            for (const auto& [sid, data] : samples) {
                if (sid != id || data.size() < 16) continue;
                int32_t l;
                memcpy(&l, data.data() + 8, 4);
                if (l == lang || (!pick && l == -1)) pick = &data;
            }
            if (!pick) continue;
            chunk = *pick;
            const uint32_t all = 0xFFFFFFFF;
            memcpy(&chunk[8], &all, 4);
            memcpy(&chunk[12], &newId, 4);
        }
        for (size_t o = 12; o + 4 <= bank.size(); ++o)
            if (!memcmp(&bank[o], &id, 4)) memcpy(&bank[o], &newId, 4);
        chunks += chunk;
    }
    AddBankPack(newName, bank + chunks);
    features::ModLog("Mods: voice %s = %s (%s) with %zu own line(s)", newName.c_str(), hero.c_str(), lang == 6 ? "Delta" : "SAS",
                     lines.size());
    return true;
}

// Texture key (FUN_004b9ab0): the archive LFSR over the lower-cased name, as stored in TEXL.
uint32_t TextureHash(const std::string& name) {
    std::string lower = name;
    for (char& c : lower) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    uint32_t x = 1;
    for (unsigned char c : lower)
        for (int b = 0; b < 8; ++b) x = x << 1 | (((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1);
    return x;
}

bool CopyModel(const std::string& model, const std::string& newName,
               const std::vector<std::pair<std::string, std::string>>& retexture, const std::string& file) {
    std::string evo;
    if (newName.size() > 63 || !ReadGameFile(model + ".evo", evo)) {
        features::ModLog("[fail] Mods: %s - model %s not found", file.c_str(), model.c_str());
        return false;
    }
    int objects = 0, slots = 0;
    std::vector<std::pair<uint32_t, uint32_t>> swaps;  // texture hash old -> new
    for (size_t p = evo.find("EOBJ"); p != std::string::npos; p = evo.find("EOBJ", p + 4)) {
        if (p + 8 + 64 > evo.size() || _stricmp(evo.c_str() + p + 8, model.c_str()) != 0) continue;
        memset(&evo[p + 8], 0, 64);
        memcpy(&evo[p + 8], newName.c_str(), newName.size());
        ++objects;
    }
    for (size_t p = evo.find("TEXL"); p != std::string::npos; p = evo.find("TEXL", p + 4)) {
        uint32_t n = 0;
        if (p + 8 > evo.size()) break;
        memcpy(&n, evo.data() + p + 4, 4);  // "TEXL", u32 n, n x {u32 hash, char[64] name}
        if (n > 64 || p + 8 + n * 68 > evo.size()) continue;
        int main = -1;
        for (uint32_t i = 0; i < n; ++i)
            if (_stricmp(evo.c_str() + p + 8 + i * 68 + 4, model.c_str()) == 0) main = static_cast<int>(i);
        if (main < 0) main = 0;
        for (const auto& [slot, tex] : retexture) {
            if (tex.size() > 63) continue;
            for (uint32_t i = 0; i < n; ++i) {
                char* name = &evo[p + 8 + i * 68 + 4];
                if (slot.empty() ? static_cast<int>(i) == main : KeyOf(name) == slot) {
                    memset(name, 0, 64);
                    memcpy(name, tex.c_str(), tex.size());
                    uint32_t old;
                    memcpy(&old, &evo[p + 8 + i * 68], 4);
                    swaps.push_back({old, TextureHash(tex)});
                    ++slots;
                }
            }
        }
    }
    // The texture hash is also in every geometry part that uses the texture (how parts find it), not only in TEXL.
    for (const auto& [from, to] : swaps)
        for (size_t q = 0; q + 4 <= evo.size(); ++q)
            if (!memcmp(&evo[q], &from, 4)) memcpy(&evo[q], &to, 4);
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    CreateDirectoryA((g_mergedRoot + "\\models").c_str(), nullptr);
    const std::string path = g_mergedRoot + "\\models\\" + newName + ".evo";
    if (!objects || !WriteAll(path, evo)) return false;
    g_files[Upper(newName + ".evo")] = {path, 0};
    features::ModLog("Mods: model %s = %s with %d texture slot(s) changed", newName.c_str(), model.c_str(), slots);
    return true;
}

void BuildWeapons() {
    if (g_weaponFiles.empty()) return;
    LoadWeaponRows();
    std::string rows, replaceRows;   // new weapons / game weapons changed in place
    for (const ModFile& wf : g_weaponFiles) {
        std::string text;
        if (!ReadAll(wf.path, text)) continue;
        const std::string file = wf.path.substr(wf.path.find_last_of("\\/") + 1);
        std::vector<std::pair<std::string, std::string>> kv;
        for (size_t pos = 0; pos < text.size();) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            kv.push_back({KeyOf(line.substr(0, eq)), Trim(line.substr(eq + 1))});
        }
        std::string id, base;
        int factions = 0;
        for (auto& [k, v] : kv) {
            if (k == "id") id = v;
            if (k == "basedon") base = v;
            if (k == "faction" || k == "factions") {
                factions = ParseFactions(v);
                if (!factions) features::ModLog("[fail] Mods: %s - faction \"%s\": SAS, Delta, Russian and / or Iraqi", file.c_str(), v.c_str());
            }
        }
        if (factions && !id.empty()) g_weaponFactions[Upper(id)] = factions;
        if (base.empty() && !id.empty() && WeaponRow(id)) {
            // a game weapon by its own id, no "based on": made choosable on the Squad Loadout screen as it is (its
            // model has to be loadable in the level - a mod can ship it); a display name and factions may come along
            for (auto& [k, v] : kv)
                if (k == "displayname") g_texts[TextureHash(id)] = v;
                else if (k != "id" && k != "faction" && k != "factions")
                    features::ModLog("[fail] Mods: %s - \"%s\" ignored: %s is a game weapon - to change it in place add "
                                 "\"based on = %s\", or make a new one (\"id = <new id>\")", file.c_str(), k.c_str(),
                                 id.c_str(), id.c_str());
            if (std::find_if(g_modWeaponIds.begin(), g_modWeaponIds.end(),
                             [&](const std::string& w) { return _stricmp(w.c_str(), id.c_str()) == 0; }) == g_modWeaponIds.end())
                g_modWeaponIds.push_back((*WeaponRow(id))[0]);
            features::ModLog("Mods: game weapon %s choosable (%s)", id.c_str(), file.c_str());
            continue;
        }
        const std::vector<std::string>* baseRow = base.empty() ? nullptr : WeaponRow(base);
        if (id.empty() || !baseRow) {
            features::ModLog("[fail] Mods: %s - needs \"id = ...\" and \"based on = <weapon id>\" (\"%s\" not found)",
                         file.c_str(), base.c_str());
            continue;
        }
        // "based on" = its own id: the game weapon itself is changed (its row replaced in place, no new weapon)
        const bool inPlace = _stricmp(base.c_str(), id.c_str()) == 0;
        std::vector<std::string> row = *baseRow;
        const std::string baseSound = row.size() > kColSound ? row[kColSound] : "";
        row.resize(std::max<size_t>(row.size(), kWeapsColumns), "-1");
        row[0] = id;
        auto soundOf = [&](const std::string& v) -> std::string {  // a weapon id -> its bank, else a bank name
            const std::vector<std::string>* r = WeaponRow(v);
            return r && r->size() > kColSound ? (*r)[kColSound] : v;
        };
        for (auto& [k, v] : kv)  // silenced first, so settings listed in the file win
            if (k == "silenced" && Yes(v)) {
                row[kColMuzzleFlash] = "-1";
                row[kColHearing] = "8.00";
                row[kColSound] = soundOf(kSilencedDonor);
            }
        std::vector<std::pair<std::string, std::string>> retexture;  // old texture key ("" = the main one) -> new
        std::vector<std::string> shots;  // WAV files for the shot variations
        std::string tail;                // WAV file for the distant tail
        g_weaponBase[Upper(id)] = base;
        for (auto& [k, v] : kv) {
            if (k == "id" || k == "basedon" || k == "silenced" || k == "faction" || k == "factions") continue;
            if (k == "shotsound" || k == "shotsound1" || k == "soundfile") {
                shots.insert(shots.begin(), v);
            } else if (k == "shotsound2") {
                shots.push_back(v);
            } else if (k == "tailsound") {
                tail = v;
            } else if (k == "displayname") {
                g_texts[TextureHash(id)] = v;  // a weapon's name comes from the catalog key = its id (same hash)
            } else if (k.compare(0, 7, "texture") == 0) {
                retexture.push_back({k.substr(7), v});
            } else if (k == "hudicon" || k == "icon") {
                // a picture file in a mod folder (HUD weapon panel, inventory, pick-up icon), else a game picture name
                std::string path;
                for (const char* ext : {"", ".png", ".dds", ".tga"}) {
                    auto it = g_files.find(Upper(v + ext));
                    if (it != g_files.end()) {
                        path = it->second.path;
                        break;
                    }
                }
                row[4] = path.empty() ? v : "DSFIX_ICON_" + std::to_string(features::AddWeaponIcon(path, (*baseRow)[4]));
            } else if (k == "muzzleflash") {
                row[kColMuzzleFlash] = Yes(v) ? "MuzzleFlash" : "-1";
            } else if (k == "sound") {
                row[kColSound] = soundOf(v);
            } else if (k.compare(0, 6, "column") == 0 && atoi(k.c_str() + 6) >= 2 && atoi(k.c_str() + 6) <= kWeapsColumns) {
                row[atoi(k.c_str() + 6) - 1] = v;
            } else {
                bool known = false;
                for (const Field& f : kFields)
                    if (k == f.key) row[f.col] = v, known = true;
                // The Squad Loadout screen hands a weapon out with a fixed count per slot unless its file says how
                // many (total, the one in the gun included - the HUD shows the spare ones, like the game's kits).
                if (k == "magazines" && atoi(v.c_str()) > 0) g_weaponMagazines[Upper(id)] = std::min(atoi(v.c_str()), 99);
                if (!known) features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", file.c_str(), k.c_str());
            }
        }
        bool silenced = false, ownModel = false, ownIcon = false;
        for (auto& [k, v] : kv) {
            silenced |= k == "silenced" && Yes(v);
            ownModel |= k == "model";
            ownIcon |= k == "hudicon" || k == "icon";
        }
        if (silenced && !ownModel)
            features::ModLog("Mods: note - %s is silenced but keeps %s's model: give it one with a suppressor (\"model = ...\")",
                         file.c_str(), base.c_str());
        if ((ownModel || !retexture.empty()) && !ownIcon)
            features::ModLog("Mods: note - %s looks different but keeps %s's HUD / inventory picture (\"hud icon = ...\")",
                         file.c_str(), base.c_str());
        // The weapons parser (FUN_00491xxx) keeps one weapon per category for GRENADE LAUNCH/RIFLE (the M16A2 + M203),
        // GRENADE LAUNCHER, GRENADE (frag) and FIST, and a second one stops the game with "More than one ... has been
        // loaded!". A copy of the M16A2 becomes a plain assault rifle; copies of the other three can't be made.
        if (!inPlace) {
            const std::string cat = Upper(Trim(row[5]));
            if (cat == "GRENADE LAUNCH/RIFLE") {
                row[5] = "ASSAULT RIFLE";
                features::ModLog("Mods: note - %s: the game allows only one rifle with a grenade launcher (the M16A2) - %s "
                             "is an assault rifle without one", file.c_str(), id.c_str());
            } else if (cat == "GRENADE LAUNCHER" || cat == "GRENADE" || cat == "FIST") {
                features::ModLog("[fail] Mods: %s - the game allows only one weapon of type \"%s\" (%s): %s left out",
                             file.c_str(), row[5].c_str(), base.c_str(), id.c_str());
                continue;
            }
        }
        if (!retexture.empty() && CopyModel(row[1], id, retexture, file)) row[1] = id;
        bool ownSound = false;
        if (!shots.empty() || !tail.empty()) {
            if (shots.empty()) shots.push_back("");  // tail only: the shots stay the donor's
            std::vector<std::string> wavs;
            for (const std::string& w : shots)
                if (!w.empty()) wavs.push_back(w);
            const std::string dir = wf.path.substr(0, wf.path.find_last_of("\\/"));
            if (MakeSoundBank(row[kColSound], id, dir, wavs.empty() ? std::vector<std::string>{} : wavs, tail, file)) {
                row[kColSound] = id;
                ownSound = true;
            }
        }
        std::string line;
        for (size_t i = 0; i < row.size(); ++i) line += (i ? "," : "") + row[i];
        if (inPlace) {
            row[0] = (*baseRow)[0];   // keep the game's spelling of the id
            line = row[0] + line.substr(line.find(','));
            replaceRows += line + "\r\n";
            for (auto& r : g_weaponRows)
                if (!r.empty() && _stricmp(r[0].c_str(), id.c_str()) == 0) r = row;
            if (!ownSound && row[kColSound] != "-1" && row[kColSound] != baseSound) BorrowBank(row[kColSound]);
            features::ModLog("Mods: game weapon %s changed (%s)", row[0].c_str(), file.c_str());
            continue;
        }
        rows += line + "\r\n";
        g_weaponRows.push_back(row);
        g_modWeaponIds.push_back(id);
        if (!ownSound && row[kColSound] != "-1" && row[kColSound] != baseSound) BorrowBank(row[kColSound]);
        features::ModLog("Mods: weapon %s (%s) based on %s, sound %s", id.c_str(), file.c_str(), base.c_str(),
                     row[kColSound].c_str());
    }
    if (rows.empty() && replaceRows.empty()) return;
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    CreateDirectoryA((g_mergedRoot + "\\weapons").c_str(), nullptr);
    const std::string path = g_mergedRoot + "\\weapons\\Weaps.txt";  // one more part of the Weaps.txt table
    if (!rows.empty() && WriteAll(path, rows)) g_tables["WEAPS.TXT"].push_back({path, g_weaponFiles.back().mod});
    const std::string repPath = g_mergedRoot + "\\weapons\\Weaps.replace.txt";  // game weapons changed in place
    if (!replaceRows.empty() && WriteAll(repPath, replaceRows))
        g_tableReplace["WEAPS.TXT"].push_back({repPath, g_weaponFiles.back().mod});
}

// ---- readable skin files (.skin) ----
// uniform (SAS / Delta / Russian / Iraqi), soldier (Bradley / Foley / Connors / Jones or rifleman / sniper / heavy /
// engineer), texture, optional portrait, first name, last name. Written as a CSkins_<SAS|US|RU|IR>_<R|S|H|C>.txt row
// (RU / IR = the multiplayer factions' tables, which the game loads too); names the game doesn't know are added to
// fNames.txt / sNames.txt (the skin rows name them by hash - an unknown name shows empty).
std::string NextNameRow(const char* table, const std::string& name, std::vector<std::string>& added) {
    for (const std::string& a : added)
        if (_stricmp(a.c_str(), name.c_str()) == 0) return "";
    std::string text;
    ReadGameFile(table, text);
    int maxId = 0;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        const std::vector<std::string> col = SplitRow(text.substr(pos, nl - pos));
        pos = nl + 1;
        if (col.size() >= 2) {
            if (_stricmp(col[0].c_str(), name.c_str()) == 0) return "";
            maxId = std::max(maxId, atoi(col[1].c_str()));
        }
    }
    added.push_back(name);
    return name + "," + std::to_string(maxId + 100 + static_cast<int>(added.size())) + "\r\n";
}

// A renamed copy of a body model (a mod's, else the game's): every object header's model name (EOBJ +8, char[64])
// becomes newName, so the game loads it as a model of its own. Written to the merged folder and indexed like a mod file.
bool CopyBody(const std::string& source, const std::string& newName, const std::string& file) {
    std::string evo;
    auto it = g_files.find(Upper(source + ".evo"));
    if (!(it != g_files.end() ? ReadAll(it->second.path, evo) : ReadGameFile(source + ".evo", evo))) {
        features::ModLog("[fail] Mods: %s - body %s not found", file.c_str(), source.c_str());
        return false;
    }
    int objects = 0;
    for (size_t p = evo.find("EOBJ"); p != std::string::npos; p = evo.find("EOBJ", p + 4)) {
        if (p + 8 + 64 > evo.size() || _stricmp(evo.c_str() + p + 8, source.c_str()) != 0) continue;
        memset(&evo[p + 8], 0, 64);
        memcpy(&evo[p + 8], newName.c_str(), newName.size());
        ++objects;
    }
    if (!objects) {
        features::ModLog("[fail] Mods: %s - %s.evo has no objects named %s", file.c_str(), source.c_str(), source.c_str());
        return false;
    }
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    CreateDirectoryA((g_mergedRoot + "\\bodies").c_str(), nullptr);
    const std::string path = g_mergedRoot + "\\bodies\\" + newName + ".evo";
    if (!WriteAll(path, evo)) return false;
    g_files[Upper(newName + ".evo")] = {path, g_skinFiles.back().mod};
    return true;
}

void BuildSkins() {
    if (g_skinFiles.empty()) return;
    static const char* const kRoles[][3] = {{"bradley", "rifleman", "R"}, {"foley", "sniper", "S"},
                                            {"connors", "heavy", "H"},    {"jones", "engineer", "C"}};
    std::unordered_map<std::string, std::string> tables;  // table name -> new rows
    std::vector<std::string> firstAdded, lastAdded;
    int n = 0;
    for (const ModFile& sf : g_skinFiles) {
        std::string text;
        if (!ReadAll(sf.path, text)) continue;
        const std::string file = sf.path.substr(sf.path.find_last_of("\\/") + 1);
        std::string uniform, soldier, texture, portrait, first, last, uniformName, body, normalMap, voice, voiceFrom, voiceFolder;
        std::unordered_map<int, std::string> voiceLines;
        for (size_t pos = 0; pos < text.size();) {
            size_t nl = text.find('\n', pos);
            if (nl == std::string::npos) nl = text.size();
            std::string line = text.substr(pos, nl - pos);
            pos = nl + 1;
            if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = KeyOf(line.substr(0, eq)), v = Trim(line.substr(eq + 1));
            if (k == "uniform") uniform = KeyOf(v);
            else if (k == "soldier" || k == "role") soldier = KeyOf(v);
            else if (k == "texture") texture = v;
            else if (k == "portrait") portrait = v;
            else if (k == "firstname") first = v;
            else if (k == "lastname") last = v;
            else if (k == "uniformname") uniformName = v;
            else if (k == "body" || k == "model") body = v;
            else if (k == "normalmap" || k == "bumpmap") normalMap = v;
            else if (k == "voice" || k == "accent") voice = KeyOf(v);
            else if (k == "voicefrom" || k == "voiceof") voiceFrom = KeyOf(v);
            else if (k == "voicefolder" || k == "voicelines") voiceFolder = v;
            else if (k.compare(0, 9, "voiceline") == 0 && k.size() > 9 && isdigit(static_cast<unsigned char>(k[9])))
                voiceLines[atoi(k.c_str() + 9)] = v;
            else features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", file.c_str(), k.c_str());
        }
        static const char* const kSideTables[4] = {"SAS", "US", "RU", "IR"};
        const int sideIndex = uniform == "sas" ? 0
                              : (uniform == "delta" || uniform == "usdelta" || uniform == "us") ? 1
                              : (uniform == "russian" || uniform == "russia" || uniform == "ru" || uniform == "spetsnaz") ? 2
                              : (uniform == "iraqi" || uniform == "iraq" || uniform == "ir") ? 3
                                                                                             : -1;
        const char* side = sideIndex >= 0 ? kSideTables[sideIndex] : nullptr;
        int role = -1;
        for (int r = 0; r < 4; ++r)
            if (soldier == kRoles[r][0] || soldier == kRoles[r][1] || (r == 2 && soldier == "heavyweapons") ||
                (r == 3 && soldier == "combatengineer"))
                role = r;
        const std::string table = side && role >= 0 ? std::string("CSkins_") + side + "_" + kRoles[role][2] + ".txt" : "";
        if (!table.empty() && !uniformName.empty() && (first.empty() || last.empty())) {
            // a uniform for the soldier himself: his names from the table's first row (the hero)
            std::string rows;
            ReadGameFile(table, rows);
            const std::vector<std::string> hero = SplitRow(rows.substr(0, rows.find_first_of("\r\n")));
            if (hero.size() >= 5) {
                if (first.empty()) first = hero[3];
                if (last.empty()) last = hero[4];
            }
        }
        if (!side || role < 0 || texture.empty() || first.empty() || last.empty()) {
            features::ModLog("[fail] Mods: %s - needs uniform (SAS / Delta / Russian / Iraqi), soldier, texture and first / last name (or a "
                         "\"uniform name\")", file.c_str());
            continue;
        }
        for (char& c : first) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        for (char& c : last) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if (portrait.empty()) {  // the soldier's own hero portrait (Russian / Iraqi: the side's first multiplayer face)
            static const char* const kPortraits[4][4] = {
                {"BradleySASPortrait", "FoleySASPortrait", "RamirezSASPortrait", "JonesSASPortrait"},
                {"BradleyPortrait", "FoleyPortrait", "RamirezPortrait", "JonesPortrait"},
                {"Hero01_RUPortrait_01", "Hero04_RUPortrait_01", "Hero02_RUPortrait_01", "Hero03_RUPortrait_01"},
                {"Hero01_IRPortrait_01", "Hero04_IRPortrait_01", "Hero02_IRPortrait_01", "Hero03_IRPortrait_01"}};
            portrait = kPortraits[sideIndex][role];
        }
        static const char* const kKeys[4] = {"Bradley", "Foley", "Ramirez", "Jones"};
        ++n;
        tables[table] += std::string(kKeys[role]) + (uniformName.empty() ? "SkinMod" : "SkinUni") + std::to_string(n) +
                         "," + texture + "," + portrait + "," + first + "," + last + "," + std::to_string(900 + n) + "\r\n";
        if (!body.empty() && g_files.find(Upper(body + ".evo")) == g_files.end()) {
            features::ModLog("[fail] Mods: %s - body %s: no %s.evo in a mod folder", file.c_str(), body.c_str(), body.c_str());
            body.clear();
        }
        if (!normalMap.empty()) {
            // The game binds a normal map to a body model (Characters_*.rfx: REFX_DOT3_BUMP "model" NORMAL_MAP "x"), and
            // the model is shared by everyone wearing it - so this skin gets a body of its own: a renamed copy of its
            // body (or the soldier's game body) that bodies.cpp swaps in and binds the normal map to.
            bool found = false;
            for (const char* ext : {".tga", ".dds"}) found |= g_files.count(Upper(normalMap + ext)) > 0;
            if (!found) {
                features::ModLog("[fail] Mods: %s - normal map %s: no %s.tga / .dds in a mod folder", file.c_str(),
                                 normalMap.c_str(), normalMap.c_str());
            } else {
                static const char* const kBodies[4] = {"HERO01_ARMSTRONG", "HERO04_FOLEY", "HERO02_RAMIREZ", "HERO03_JONES"};
                const std::string source = body.empty() ? kBodies[role] : body;
                std::string copy = "DSFIX_NM_";
                for (char c : Upper(texture)) copy += isalnum(static_cast<unsigned char>(c)) ? c : '_';
                if (copy.size() > 60) copy.resize(60);
                if (CopyBody(source, copy, file)) {
                    body = copy;
                    g_skinNormals[Upper(texture)] = normalMap;
                }
            }
        }
        if (!body.empty()) g_skinBodies[Upper(texture)] = body;  // bodies.cpp swaps it in with this texture's skin
        const std::string skinDir = sf.path.substr(0, sf.path.find_last_of("\\/"));
        if (!voiceFolder.empty()) {   // every "...lineNN.wav" in a folder (ds_sound.py wav names them so)
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((skinDir + "\\" + voiceFolder + "\\*.wav").c_str(), &fd);
            int found = 0;
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    const std::string name = Upper(fd.cFileName);
                    const size_t at = name.rfind("LINE");
                    if (at == std::string::npos || !isdigit(static_cast<unsigned char>(name[at + 4]))) continue;
                    const int line = atoi(name.c_str() + at + 4);
                    if (!voiceLines.count(line)) voiceLines[line] = voiceFolder + "\\" + fd.cFileName, ++found;
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
            if (!found)
                features::ModLog("[fail] Mods: %s - voice folder %s: no ...line01.wav ... files", file.c_str(), voiceFolder.c_str());
        }
        if (!voice.empty() || !voiceFrom.empty() || !voiceLines.empty()) {   // a voice of its own (voices.cpp)
            static const char* const kVoiceBanks[4] = {"Bradley", "Foley", "Ramirez", "Jones"};
            int from = role;
            for (int r = 0; r < 4; ++r)
                if (voiceFrom == kRoles[r][0] || voiceFrom == kRoles[r][1] || (r == 2 && voiceFrom == "ramirez")) from = r;
            if (!voiceFrom.empty() && from == role && voiceFrom != kRoles[role][0] && voiceFrom != kRoles[role][1] &&
                !(role == 2 && voiceFrom == "ramirez"))
                features::ModLog("[fail] Mods: %s - voice from \"%s\": Bradley, Foley, Connors or Jones", file.c_str(), voiceFrom.c_str());
            int lang = sideIndex == 1 ? 6 : 0;
            if (voice == "delta" || voice == "us" || voice == "american") lang = 6;
            else if (voice == "sas" || voice == "uk" || voice == "british") lang = 0;
            else if (!voice.empty())
                features::ModLog("[fail] Mods: %s - voice \"%s\": SAS or Delta", file.c_str(), voice.c_str());
            std::string name = "DSFIX_V_";
            for (char c : Upper(texture)) name += isalnum(static_cast<unsigned char>(c)) ? c : '_';
            if (MakeVoiceBank(kVoiceBanks[from], lang, name, skinDir, voiceLines, file)) g_skinVoices[Upper(texture)] = name;
        }
        tables["fNames.txt"] += NextNameRow("fNames.txt", first, firstAdded);
        tables["sNames.txt"] += NextNameRow("sNames.txt", last, lastAdded);
        if (uniformName.empty()) {
            features::ModLog("Mods: skin %s %s (%s %s, %s)", first.c_str(), last.c_str(), side, kRoles[role][1], texture.c_str());
        } else {
            for (char& c : uniformName) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            g_skinVariants.push_back({sideIndex, role, texture, uniformName});
            features::ModLog("Mods: uniform %s for %s %s (%s %s, %s)", uniformName.c_str(), first.c_str(), last.c_str(), side,
                         kRoles[role][1], texture.c_str());
        }
    }
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    CreateDirectoryA((g_mergedRoot + "\\skins").c_str(), nullptr);
    for (auto& [name, rows] : tables) {
        if (rows.empty()) continue;
        const std::string path = g_mergedRoot + "\\skins\\" + name;
        if (WriteAll(path, rows)) g_tables[Upper(name)].push_back({path, g_skinFiles.back().mod});
    }
}

// Every weapon the customise screen can hand out needs its shot bank in the level's sound cache: a weapon made without
// it crashes (the sound emitter constructor FUN_004c9a90 reads the missing bank). The banks of all kit weapons are
// copied once at start-up - one pass over the game's caches, multiplayer ones first (they carry every weapon) - into
// one pack that every level gets its missing banks / samples from.
void BorrowKitBanks() {
    if (g_weaponRows.empty()) LoadWeaponRows();
    std::vector<std::string> names;
    for (const std::string& id : features::KitWeaponIds())
        if (const std::vector<std::string>* r = WeaponRow(id))
            if (r->size() > kColSound && (*r)[kColSound] != "-1" && !(*r)[kColSound].empty() &&
                std::find(names.begin(), names.end(), (*r)[kColSound]) == names.end() &&
                std::find(g_borrowed.begin(), g_borrowed.end(), (*r)[kColSound]) == g_borrowed.end())
                names.push_back((*r)[kColSound]);
    std::unordered_map<uint32_t, std::string> want;
    for (const std::string& n : names) want[ElfHash(n + ".sbk")] = n;
    if (want.empty()) return;
    if (features::AssetStoreSound()) {   // the store serves them: keys only, no .sch reading / pack file
        int found = 0;
        for (const auto& [key, name] : want) {
            if (!features::AssetStoreHasBank(key, nullptr)) continue;
            g_borrowed.push_back(name);
            if (std::find(g_storeBanks.begin(), g_storeBanks.end(), key) == g_storeBanks.end()) g_storeBanks.push_back(key);
            ++found;
        }
        features::ModLog("Mods: %d kit weapon sound banks available in every level (asset store)%s", found,
                         found == static_cast<int>(want.size()) ? "" : " (some not found)");
        return;
    }
    std::vector<std::string> caches;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "\\*.sch").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do caches.push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    std::stable_partition(caches.begin(), caches.end(), [](const std::string& c) { return _strnicmp(c.c_str(), "mp_", 3) == 0; });
    std::string pack;
    std::unordered_map<uint64_t, bool> packed;
    int found = 0;
    for (const std::string& cache : caches) {
        if (want.empty()) break;
        std::string file;
        if (!ReadAll(GameDir() + "\\" + cache, file)) continue;
        const std::vector<Chunk> chunks = Chunks(file);
        std::unordered_map<uint32_t, bool> refs;
        for (const Chunk& c : chunks) {
            if (c.tag != kTagBank || !want.count(BankKey(c))) continue;
            pack.append(c.data, c.size);
            g_borrowed.push_back(want[BankKey(c)]);
            want.erase(BankKey(c));
            ++found;
            for (uint32_t o = 12; o + 4 <= c.size; ++o) {
                uint32_t v;
                memcpy(&v, c.data + o, 4);
                refs[v] = true;
            }
        }
        for (const Chunk& c : chunks)
            if ((c.tag == kTagSample || c.tag == kTagStream) && refs.count(static_cast<uint32_t>(SampleKey(c))) &&
                !packed[SampleKey(c)]) {
                packed[SampleKey(c)] = true;
                pack.append(c.data, c.size);
            }
    }
    if (!pack.empty()) AddBankPack("kit_weapons", pack);
    features::ModLog("Mods: %d kit weapon sound banks available in every level%s", found,
                 want.empty() ? "" : " (some not found)");
}

const ModFile* Find(const char* name) {
    if (!name || !*name || strchr(name, ':')) return nullptr;  // absolute paths (saves, settings) stay as they are
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    auto it = g_files.find(Upper(base));
    return it == g_files.end() ? nullptr : &it->second;
}

char* __cdecl Resolve(const char* name, void* paths, int flags) {
    if (name && !strchr(name, ':') && !strpbrk(name, "\\/")) {   // level sound caches: voices + mod banks
        const size_t len = strlen(name);
        if (len > 4 && _stricmp(name + len - 4, ".sch") == 0) {
            const std::string merged = SoundCache(name);
            if (!merged.empty() && merged.size() < kPathBufferSize) {
                memcpy(reinterpret_cast<char*>(kPathBuffer), merged.c_str(), merged.size() + 1);
                return reinterpret_cast<char*>(kPathBuffer);
            }
        }
    }
    if (const ModFile* f = Find(name)) {
        if (f->path.size() < kPathBufferSize) {
            char* out = reinterpret_cast<char*>(kPathBuffer);
            memcpy(out, f->path.c_str(), f->path.size() + 1);
#ifndef DS_DIST
            static std::unordered_map<std::string, bool> logged;
            if (!logged[f->path]) {
                logged[f->path] = true;
                features::ModLog("Mods: %s -> %s", name, f->path.c_str());
            }
#endif
            return out;
        }
    }
    return g_resolve(name, paths, flags);
}
bool StartsWith(const char* path, const std::string& root) {
    return !root.empty() && _strnicmp(path, root.c_str(), root.size()) == 0;
}

int __fastcall Open(void* file, void* /*edx*/, const char* path) {
    if (!path || !(StartsWith(path, g_modsRoot) || StartsWith(path, g_mergedRoot))) return g_open(file, path);
    auto* flags = reinterpret_cast<uint32_t*>(kFileFlags);
    const uint32_t saved = *flags;
    *flags &= ~1u;  // this one from disk
    const int result = g_open(file, path);
    *flags = saved | (*flags & ~saved & ~1u);  // keep anything the call set, archive mode back
    if (!result) features::ModLog("[fail] Mods: can't open %s", path);
    return result;
}

// Entry hook through the broker; next = its trampoline (the moved prologue) or the next subscriber.
template <class Fn>
bool HookEntry(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
}

// The resolver and the archive opener (both entry hooks).
bool HookResolveAndOpen() {
    return HookEntry(kResolve, reinterpret_cast<const void*>(&Resolve), g_resolve, kResolveEntry, sizeof kResolveEntry) &&
           HookEntry(kOpen, reinterpret_cast<const void*>(&Open), g_open, kOpenEntry, sizeof kOpenEntry);
}
}  // namespace

const std::vector<features::SkinVariant>& features::SkinVariants() { return g_skinVariants; }
const std::vector<features::ModGearPiece>& features::ModGear() { return g_modGear; }
const std::vector<features::ModPouchSet>& features::ModPouchSets() { return g_modPouchSets; }

namespace {
int g_generation = 0;  // bumped by every reload (features::ModsGeneration: caches of mod data refresh)

// ---- game data by reference (no game files in the mod) ----
// A mission made with the editor reuses the game's own files - models and textures from other levels, a level's AI
// walk map, its scene - and must not ship them. Instead:
//   <name>.uses       lines  "file T72M1.EVO"                  the player's copy (any game .dat) under that name
//                            "file BASETEST.QTD = MISSION1.QTD" the player's copy of the right-hand file, renamed
//                            "bank VehicleTankWeaponT72"       a sound bank from the game's level sound caches
//   <name>.envpatch  "base MISSION1.ENV", then hunks "@ <start> <delete> <insert>" + <insert> lines: the scene as
//                    line changes against the game's scene (start = 0-based line of the base) -> <name>.ENV
//   <name>.rmdpatch  "base MISSION1.RMD", "count <n>", then "<i> x y z l0 l1 l2 l3 l4 flags weight" for every route
//                    marker that differs from the base marker <i> -> <name>.RMD
//   <name>.dllpatch  a changed copy of a shipped mission DLL (Pivotal's code isn't redistributed): "base Mission6.dll"
//                    (a file in the game folder), "base-size <bytes>", optional "size <bytes>" (output, default the
//                    base's), then hunks "@ <file offset hex> <original bytes hex | -> <new bytes hex>" -> <name>.DLL,
//                    loaded through scripthost.cpp's mission DLL hook. Every original byte must match (another
//                    version of the DLL is refused); "-" = bytes past the base's end. # starts a comment.
// Built into %TEMP%\DesertStormFix\merged\game at start-up and indexed like mod files; a real file of the same name in
// a mod wins. ModKit editor/commands.py writes them (save_mission), conflict_ds_tools/env.py applies them too.
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

std::string GameDataDir() {
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    const std::string dir = g_mergedRoot + "\\game";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

void AddBuilt(const std::string& name, const std::string& data, int mod, const char* what) {
    const std::string key = Upper(name);
    if (g_files.count(key)) return;   // the mod ships its own
    const std::string path = GameDataDir() + "\\" + name;
    if (!WriteAll(path, data)) {
        features::ModLog("[fail] Mods: %s: can't write %s", g_mods[mod].c_str(), path.c_str());
        return;
    }
    g_files[key] = {path, mod};
    (void)what;
}

bool ApplyEnvPatch(const std::string& patch, std::string& out, std::string& base) {
    const std::vector<std::string> p = SplitLines(patch);
    if (p.empty() || p[0].compare(0, 5, "base ") != 0) return false;
    base = p[0].substr(5);
    std::string text;
    if (!ReadGameFile(base, text)) return false;
    const bool crlf = text.find("\r\n") != std::string::npos;
    const std::vector<std::string> lines = SplitLines(text);
    std::vector<std::string> result;
    size_t at = 0;   // next base line to copy
    for (size_t i = 1; i < p.size(); ++i) {
        if (p[i].empty() || p[i][0] != '@') continue;
        unsigned start = 0, del = 0, ins = 0;
        if (sscanf_s(p[i].c_str(), "@ %u %u %u", &start, &del, &ins) != 3 || start < at || start + del > lines.size() ||
            i + ins >= p.size() + 1)
            return false;
        while (at < start) result.push_back(lines[at++]);
        at += del;
        for (unsigned k = 0; k < ins; ++k) result.push_back(p[++i]);
    }
    while (at < lines.size()) result.push_back(lines[at++]);
    out.clear();
    for (const std::string& l : result) out += l + (crlf ? "\r\n" : "\n");
    return true;
}

bool ApplyRmdPatch(const std::string& patch, std::string& out, std::string& base) {
    const std::vector<std::string> p = SplitLines(patch);
    if (p.size() < 2 || p[0].compare(0, 5, "base ") != 0) return false;
    base = p[0].substr(5);
    std::string file;
    unsigned count = 0;
    if (!ReadGameFile(base, file) || file.size() < 12 || sscanf_s(p[1].c_str(), "count %u", &count) != 1 || count > 65535)
        return false;
    out.assign(file.data(), 12);
    uint32_t n = count;
    memcpy(out.data() + 8, &n, 4);
    out.resize(12 + 32 * count, '\0');
    const size_t baseCount = (file.size() - 12) / 32;
    memcpy(out.data() + 12, file.data() + 12, 32 * std::min<size_t>(baseCount, count));
    for (size_t i = 2; i < p.size(); ++i) {
        unsigned idx, l[5], flags;
        float x, y, z, w;
        if (sscanf_s(p[i].c_str(), "%u %f %f %f %u %u %u %u %u %u %f", &idx, &x, &y, &z, &l[0], &l[1], &l[2], &l[3], &l[4],
                     &flags, &w) != 11 || idx >= count)
            continue;
        char* m = out.data() + 12 + 32 * idx;
        if (idx >= baseCount) { const uint16_t filler = 0xCDCD; memcpy(m + 26, &filler, 2); }   // like the editor's
        memcpy(m, &x, 4); memcpy(m + 4, &y, 4); memcpy(m + 8, &z, 4);
        for (int k = 0; k < 5; ++k) { const uint16_t v = static_cast<uint16_t>(l[k]); memcpy(m + 12 + 2 * k, &v, 2); }
        const uint16_t f = static_cast<uint16_t>(flags);
        memcpy(m + 24, &f, 2);
        memcpy(m + 28, &w, 4);
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

// error = why it doesn't fit (for the report)
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
    // a bare file name in the game folder (no paths out of it)
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
        char offsetText[16] = {}, origText[4096] = {}, newText[4096] = {};
        size_t offset = 0;
        std::string orig, now;
        if (sscanf_s(h.c_str(), "@ %15s %4095s %4095s", offsetText, 16, origText, 4096, newText, 4096) != 3 ||
            sscanf_s(offsetText, "%zx", &offset) != 1 || !HexBytes(newText, now) ||
            (strcmp(origText, "-") && !HexBytes(origText, orig)) || (!orig.empty() && orig.size() != now.size()) ||
            offset + now.size() > out.size()) {
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

// Kit piece: object `index` of a world-piece model (no transform keys - every object is in world space) as a one-object
// static prop: the bytes from its EOBJ to its NEXT / DONE, header patched (model name +8, node type +0x8C = 0, parent
// +0xB0 = "ROOT"), last tag DONE. Objects are found by their "EOBJ" + version 0x2F tag after the previous one's NEXT.
// Same rules as ModKit conflict_ds_tools/kits.py.
bool CutPiece(const std::string& model, int index, const std::string& name, std::string& out) {
    std::string data;
    if (!ReadGameFile(model, data) || data.size() < 8 || data.compare(0, 4, "EOBJ") != 0) return false;
    static const char kNext[] = "NEXTEOBJ/   ";
    std::vector<size_t> starts = {0};
    for (size_t p = data.find(std::string(kNext, 12)); p != std::string::npos; p = data.find(std::string(kNext, 12), p + 4))
        starts.push_back(p + 4);
    if (index < 0 || index >= static_cast<int>(starts.size())) return false;
    const size_t a = starts[index], b = index + 1 < static_cast<int>(starts.size()) ? starts[index + 1] : data.size();
    out = data.substr(a, b - a);
    if (out.size() < 0xF0 || (out.compare(out.size() - 4, 4, "NEXT") && out.compare(out.size() - 4, 4, "DONE"))) return false;
    std::string n = name.substr(0, 63);
    memset(&out[8], 0, 64);
    memcpy(&out[8], n.data(), n.size());
    const uint32_t staticType = 0;
    memcpy(&out[0x8C], &staticType, 4);
    memset(&out[0xB0], 0, 64);
    memcpy(&out[0xB0], "ROOT", 4);
    memcpy(&out[out.size() - 4], "DONE", 4);
    return true;
}

void BuildReferences() {
    for (const ModFile& u : g_usesFiles) {
        std::string text;
        if (!ReadAll(u.path, text)) continue;
        int files = 0, banks = 0;
        for (std::string line : SplitLines(text)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            char kind[8] = {}, a[128] = {}, eq[4] = {}, b[128] = {};
            const int n = sscanf_s(line.c_str(), "%7s %127s %3s %127s", kind, 8, a, 128, eq, 4, b, 128);
            if (n < 2) continue;
            if (!_stricmp(kind, "bank")) {
                BorrowBank(a);
                ++banks;
            } else if (!_stricmp(kind, "piece")) {            // piece KIT_X = MODEL.EVO <index>
                char model[128] = {};
                int index = -1;
                if (sscanf_s(line.c_str(), "%*s %*s = %127s %d", model, 128, &index) != 2) continue;
                std::string data;
                if (g_files.count(Upper(std::string(a) + ".EVO"))) { ++files; continue; }
                if (!CutPiece(model, index, a, data)) {
                    features::ModLog("[fail] Mods: %s uses piece %d of %s, which this game's data doesn't have",
                                     g_mods[u.mod].c_str(), index, model);
                    continue;
                }
                AddBuilt(std::string(a) + ".EVO", data, u.mod, "piece");
                ++files;
            } else if (!_stricmp(kind, "file")) {
                const std::string source = n == 4 && !strcmp(eq, "=") ? b : a;
                std::string data;
                if (g_files.count(Upper(a))) { ++files; continue; }
                if (!ReadGameFile(source, data)) {
                    features::ModLog("[fail] Mods: %s uses %s, which isn't in this game's data", g_mods[u.mod].c_str(),
                                     source.c_str());
                    continue;
                }
                AddBuilt(a, data, u.mod, "file");
                ++files;
            }
        }
        features::ModLog("Mods: %s uses %d game file(s) and %d sound bank(s) from your install", g_mods[u.mod].c_str(),
                         files, banks);
    }
    for (int kind = 0; kind < 2; ++kind)
        for (const ModFile& m : kind ? g_rmdPatches : g_envPatches) {
            std::string patch, out, base;
            const std::string file = m.path.substr(m.path.find_last_of("\\/") + 1);
            const std::string name = file.substr(0, file.find_last_of('.')) + (kind ? ".RMD" : ".ENV");
            if (!ReadAll(m.path, patch) || !(kind ? ApplyRmdPatch(patch, out, base) : ApplyEnvPatch(patch, out, base))) {
                features::ModLog("[fail] Mods: %s: %s doesn't fit this game's %s", g_mods[m.mod].c_str(), file.c_str(),
                                 base.empty() ? "files" : base.c_str());
                continue;
            }
            AddBuilt(name, out, m.mod, kind ? "routes" : "scene");
            features::ModLog("Mods: %s = %s + %s", name.c_str(), base.c_str(), file.c_str());
        }
    for (const ModFile& m : g_dllPatches) {
        std::string patch, out, base, error;
        const std::string file = m.path.substr(m.path.find_last_of("\\/") + 1);
        const std::string name = file.substr(0, file.find_last_of('.')) + ".DLL";
        if (!ReadAll(m.path, patch) || !ApplyDllPatch(patch, out, base, error)) {
            features::ModLog("[fail] Mods: %s: %s doesn't fit this game: %s", g_mods[m.mod].c_str(), file.c_str(),
                             error.empty() ? "can't read it" : error.c_str());
            continue;
        }
        AddBuilt(name, out, m.mod, "mission DLL");
        features::ModLog("Mods: %s = %s + %s", name.c_str(), base.c_str(), file.c_str());
    }
}

// ---- gear (.gear / .pouches, the loadout screen's HEADGEAR / BACKPACK / FACE / pouch rows, gear.cpp) ----
// A .gear file adds one piece:
//   slot = head | back | face | pouch    which rows offer it
//   name = Pink Beret                     shown on the row (default: from the id)
//   id = PINK_BERET                       its model name (letters, digits, _; max 35)
// and its model, one of:
//   based on = IR_BERET_RED + texture = PinkBeret     a game model with your texture (.dds / .tga in the mod) - no
//                                                      modelling; "texture <part> = X" for a model with more parts
//   model = MYHAT                          a model the mod ships (MYHAT.evo), e.g. exported from Blender based on a
//                                          game piece (the connector point is the model's origin)
//   (neither)                              the mod ships <id>.evo
// A .pouches file adds one set to the POUCHES row: name = ..., then belt / belt left / belt right / hip left /
// hip right / chest left / chest right = a pouch (game model or a .gear pouch id), none or - for nothing; spots left
// out are empty.
std::vector<std::pair<std::string, std::string>> ReadKeyValues(const std::string& path) {
    std::vector<std::pair<std::string, std::string>> kv;
    std::string text;
    if (!ReadAll(path, text)) return kv;
    for (size_t pos = 0; pos < text.size();) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        if (const size_t hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        kv.push_back({KeyOf(line.substr(0, eq)), Trim(line.substr(eq + 1))});
    }
    return kv;
}

void BuildGear() {
    static const char* const kSlots[] = {"head", "back", "face", "pouch"};
    for (const ModFile& gf : g_gearFiles) {
        const std::string file = gf.path.substr(gf.path.find_last_of("\\/") + 1);
        std::string id, name, base, model;
        int slot = -1;
        std::vector<std::pair<std::string, std::string>> retexture;
        for (auto& [k, v] : ReadKeyValues(gf.path)) {
            if (k == "id") id = Upper(v);
            else if (k == "name" || k == "displayname") name = v;
            else if (k == "basedon") base = Upper(v);
            else if (k == "model") model = Upper(v);
            else if (k.compare(0, 7, "texture") == 0) retexture.push_back({k.substr(7), v});
            else if (k == "slot") {
                const std::string s = KeyOf(v);
                for (int i = 0; i < 4; ++i)
                    if (s == kSlots[i] || (i == 0 && (s == "headgear" || s == "hat")) || (i == 1 && s == "backpack") ||
                        (i == 3 && (s == "pouches" || s == "belt")))
                        slot = i;
            } else {
                features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", file.c_str(), k.c_str());
            }
        }
        if (id.empty() || id.size() > 35 || slot < 0) {
            features::ModLog("[fail] Mods: %s - needs \"id = ...\" (max 35 characters) and \"slot = head / back / face / "
                             "pouch\"", file.c_str());
            continue;
        }
        if (!base.empty()) {   // a game model with the mod's texture
            if (retexture.empty()) retexture.push_back({"", id});   // (no texture: its own copy under the new name)
            if (!CopyModel(base, id, retexture, file)) continue;
        } else if (!model.empty() && model != id) {
            if (!Find((model + ".evo").c_str())) {
                features::ModLog("[fail] Mods: %s - model %s.evo isn't in a mod", file.c_str(), model.c_str());
                continue;
            }
            id = model;
        } else if (!Find((id + ".evo").c_str())) {
            features::ModLog("[fail] Mods: %s - %s.evo isn't in a mod (or give \"based on = <game model>\" + \"texture = "
                             "...\")", file.c_str(), id.c_str());
            continue;
        }
        if (name.empty()) {
            name = id;
            for (char& c : name) c = c == '_' ? ' ' : c;
        }
        for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        g_modGear.push_back({id, slot, name});
        features::ModLog("Mods: gear %s (%s) on the %s row%s", name.c_str(), id.c_str(),
                         slot == 0 ? "HEADGEAR" : slot == 1 ? "BACKPACK" : slot == 2 ? "FACE" : "pouch",
                         base.empty() ? "" : (" - " + base + " with a new texture").c_str());
    }
    static const char* const kSpots[7][3] = {{"belt", "beltback", "back"}, {"beltleft", "left", "spine2l"},
                                             {"beltright", "right", "spine2r"}, {"hipleft", "hipl", nullptr},
                                             {"hipright", "hipr", nullptr}, {"chestleft", "chestl", nullptr},
                                             {"chestright", "chestr", nullptr}};
    for (const ModFile& pf : g_pouchFiles) {
        const std::string file = pf.path.substr(pf.path.find_last_of("\\/") + 1);
        features::ModPouchSet set;
        int pieces = 0;
        for (auto& [k, v] : ReadKeyValues(pf.path)) {
            if (k == "name") {
                set.name = v;
                continue;
            }
            int spot = -1;
            for (int s = 0; s < 7; ++s)
                for (const char* alias : kSpots[s])
                    if (alias && k == alias) spot = s;
            if (spot < 0) {
                features::ModLog("[fail] Mods: %s - unknown spot \"%s\" (belt, belt left, belt right, hip left, hip right, "
                                 "chest left, chest right)", file.c_str(), k.c_str());
                continue;
            }
            const std::string m = Upper(v);
            set.model[spot] = m == "NONE" || m == "-" ? "" : m;
            pieces += set.model[spot].empty() ? 0 : 1;
        }
        if (set.name.empty()) set.name = file.substr(0, file.find_last_of('.'));
        for (char& c : set.name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        g_modPouchSets.push_back(set);
        features::ModLog("Mods: pouch set %s - %d piece(s)", set.name.c_str(), pieces);
    }
}

// ---- vehicles (.vehicle: the loadout screen's VEHICLE rows, vehicles.cpp) ----
// A .vehicle file adds one vehicle to the choices:
//   id = DESERT_ABRAMS                    its unit name (letters, digits, _; max 31)
//   based on = US_VEH_M1Abrams            the game vehicle (VEHICLE.TXT) whose seats, handling, guns and sounds it has
//   name = M1 Abrams (Desert)             shown on the row (default: from the id)
//   texture = AbramsDesert                a skin: the base model with your texture ("texture <part> = X" for a model
//                                         with more texture slots); "damaged texture" likewise for the wreck
//   model = MYTANK                        or a model the mod ships (MYTANK.evo, + MYTANK.DYN for tracks / wheels -
//                                         the base model's is used when it has none); "damaged model" likewise
//   faction = US / UK / Iraqi             shown with the name (default: the base vehicle's side)
//   engine sound = Hovercraft             its own engine bank (a .sch bank pack in the mod, events as Wheels.sbk /
//                                         Tracks.sbk): a Props.txt row <ID>_ENGINE (the base's propulsion type, this
//                                         bank) and a VStat.txt copy <ID>_STATS naming it
//   grip = 0.3                            tyre friction (handling.cpp; the game's vehicles have unlimited friction,
//                                         so they never slide): low = drifts and slides like a hovercraft
//   spin damping = 2                      how fast the chassis stops turning (the game's 0.2): stops a sliding
//                                         vehicle from spinning on after the steering is let go
// Rows written: VEHICLE.TXT (the base row with the id and models), VEHWPNS.TXT (the base vehicle's guns), VStat.txt /
// Props.txt with "engine sound". Handling = the model's .DYN (research/custom_units.md has the field list).
std::vector<std::vector<std::string>> TableRows(const char* table) {
    std::vector<std::vector<std::string>> rows;
    std::string text;
    if (!features::ReadGameTable(table, text)) return rows;
    for (const std::string& line : SplitLines(text))
        if (!Trim(line).empty() && Trim(line)[0] != '#') rows.push_back(SplitRow(line));
    return rows;
}

const std::vector<std::string>* FindRow(const std::vector<std::vector<std::string>>& rows, const std::string& key) {
    for (const auto& r : rows)
        if (!r.empty() && _stricmp(r[0].c_str(), key.c_str()) == 0) return &r;
    return nullptr;
}

std::string JoinRow(const std::vector<std::string>& row) {
    std::string line;
    for (size_t i = 0; i < row.size(); ++i) line += (i ? "," : "") + row[i];
    return line;
}

// <model>.DYN (tracks, wheels) for a renamed model: the mod's own, else a copy of the base model's.
void CopyDyn(const std::string& base, const std::string& model) {
    if (Find((model + ".DYN").c_str())) return;
    std::string dyn;
    if (!ReadGameFile(base + ".DYN", dyn)) {
        const ModFile* own = Find((base + ".DYN").c_str());
        if (!own || !ReadAll(own->path, dyn)) return;
    }
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    CreateDirectoryA((g_mergedRoot + "\\models").c_str(), nullptr);
    const std::string path = g_mergedRoot + "\\models\\" + model + ".DYN";
    if (WriteAll(path, dyn)) g_files[Upper(model + ".DYN")] = {path, -1};
}

std::vector<std::vector<std::string>> RowsOf(const std::string& text) {
    std::vector<std::vector<std::string>> rows;
    for (const std::string& line : SplitLines(text))
        if (!Trim(line).empty() && Trim(line)[0] != '#') rows.push_back(SplitRow(line));
    return rows;
}

void BuildVehicles() {
    if (g_vehicleFiles.empty()) return;
    // the game's rows + mods' own VEHICLE.TXT parts (not merged yet: a .vehicle may be based on one of them too)
    std::vector<std::vector<std::string>> vehicles, weapons;
    std::string text;
    if (ReadGameFile("VEHICLE.TXT", text)) vehicles = RowsOf(text);
    for (const ModFile& part : g_tables["VEHICLE.TXT"])
        if (ReadAll(part.path, text))
            for (auto& r : RowsOf(text)) vehicles.push_back(r);
    if (ReadGameFile("VEHWPNS.TXT", text)) weapons = RowsOf(text);
    for (const ModFile& part : g_tables["VEHWPNS.TXT"])
        if (ReadAll(part.path, text))
            for (auto& r : RowsOf(text)) weapons.push_back(r);
    std::vector<std::vector<std::string>> stats, props;   // for "engine sound"
    if (ReadGameFile("VSTAT.TXT", text)) stats = RowsOf(text);
    if (auto it = g_tables.find("VSTAT.TXT"); it != g_tables.end())
        for (const ModFile& part : it->second)
            if (ReadAll(part.path, text))
                for (auto& r : RowsOf(text)) stats.push_back(r);
    if (ReadGameFile("PROPS.TXT", text)) props = RowsOf(text);
    if (auto it = g_tables.find("PROPS.TXT"); it != g_tables.end())
        for (const ModFile& part : it->second)
            if (ReadAll(part.path, text))
                for (auto& r : RowsOf(text)) props.push_back(r);
    std::string rows, gunRows, statRows, propRows;
    int lastMod = 0;
    for (const ModFile& vf : g_vehicleFiles) {
        const std::string file = vf.path.substr(vf.path.find_last_of("\\/") + 1);
        std::string id, name, base, model, damagedModel, faction, engine;
        float grip = -1.0f, spin = -1.0f;
        std::vector<std::pair<std::string, std::string>> retexture, damagedRetexture;
        for (auto& [k, v] : ReadKeyValues(vf.path)) {
            if (k == "id") id = v;
            else if (k == "name" || k == "displayname") name = v;
            else if (k == "basedon") base = v;
            else if (k == "model") model = Upper(v);
            else if (k == "damagedmodel" || k == "wreckmodel") damagedModel = Upper(v);
            else if (k == "faction" || k == "side") faction = v;
            else if (k == "enginesound" || k == "engine") engine = v;
            else if (k == "grip") grip = static_cast<float>(atof(v.c_str()));
            else if (k == "spindamping") spin = static_cast<float>(atof(v.c_str()));
            else if (k.compare(0, 14, "damagedtexture") == 0) damagedRetexture.push_back({k.substr(14), v});
            else if (k.compare(0, 7, "texture") == 0) retexture.push_back({k.substr(7), v});
            else features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", file.c_str(), k.c_str());
        }
        const std::vector<std::string>* found = base.empty() ? nullptr : FindRow(vehicles, base);
        if (id.empty() || id.size() > 31 || id.find_first_of(" ,") != std::string::npos || !found || found->size() < 5) {
            features::ModLog("[fail] Mods: %s - needs \"id = ...\" (max 31 characters, no spaces) and \"based on = <game "
                             "vehicle>\" (a VEHICLE.TXT row, e.g. US_VEH_Hum_Vee)", file.c_str());
            continue;
        }
        if (FindRow(vehicles, id)) {
            features::ModLog("[fail] Mods: %s - vehicle %s exists already", file.c_str(), id.c_str());
            continue;
        }
        const std::vector<std::string> baseRow = *found;   // (vehicles grows below)
        std::vector<std::string> row = baseRow;
        row[0] = id;
        const std::string baseModel = Upper(row[1]);
        if (!model.empty()) {   // the mod's own model
            if (!Find((model + ".evo").c_str())) {
                features::ModLog("[fail] Mods: %s - model %s.evo isn't in a mod", file.c_str(), model.c_str());
                continue;
            }
            CopyDyn(baseModel, model);
            row[1] = model;
        } else if (!retexture.empty()) {   // a skin: the base model with new textures, under the vehicle's id
            if (!CopyModel(baseModel, Upper(id), retexture, file)) continue;
            CopyDyn(baseModel, Upper(id));
            row[1] = Upper(id);
        }
        if (!damagedModel.empty()) {
            if (Find((damagedModel + ".evo").c_str())) row[2] = damagedModel;
            else features::ModLog("[fail] Mods: %s - damaged model %s.evo isn't in a mod", file.c_str(), damagedModel.c_str());
        } else if (!damagedRetexture.empty() && row[2] != "-1" && _stricmp(row[2].c_str(), "dummy") != 0) {
            if (CopyModel(Upper(row[2]), Upper(id) + "_WRECK", damagedRetexture, file)) row[2] = Upper(id) + "_WRECK";
        }
        if (!engine.empty()) {   // own engine bank: a Props row (base's propulsion type) + a VStat copy naming it
            if (engine.size() > 4 && _stricmp(engine.c_str() + engine.size() - 4, ".sbk") == 0) engine.resize(engine.size() - 4);
            const std::vector<std::string>* st = FindRow(stats, row[4]);
            const std::vector<std::string>* pr = st && st->size() > 12 ? FindRow(props, (*st)[12]) : nullptr;
            if (!pr || pr->size() < 4 || engine.size() > 31) {
                features::ModLog("[fail] Mods: %s - engine sound: the base vehicle's stats / propulsion weren't found",
                                 file.c_str());
            } else {
                std::vector<std::string> p = *pr, v = *st;
                p[0] = id + "_ENGINE";
                p[2] = engine + ".sbk";
                v[0] = id + "_STATS";
                v[12] = p[0];
                propRows += JoinRow(p) + "\r\n";
                statRows += JoinRow(v) + "\r\n";
                props.push_back(p);
                stats.push_back(v);
                row[4] = v[0];
            }
        }
        if (grip >= 0.0f || spin >= 0.0f) g_vehicleHandling.push_back({Upper(row[1]), grip, spin});
        rows += JoinRow(row) + "\r\n";
        vehicles.push_back(row);
        int guns = 0;
        for (const auto& w : weapons)
            if (!w.empty() && _stricmp(w[0].c_str(), baseRow[0].c_str()) == 0) {
                std::vector<std::string> g = w;
                g[0] = id;
                gunRows += JoinRow(g) + "\r\n";
                ++guns;
            }
        if (name.empty()) {
            name = id;
            for (char& c : name) c = c == '_' ? ' ' : c;
        }
        for (char& c : name) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        for (char& c : faction) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        g_modVehicles.push_back({id, name, baseRow[0], faction});
        lastMod = vf.mod;
        features::ModLog("Mods: vehicle %s (%s) based on %s, model %s, %d gun(s)", name.c_str(), id.c_str(),
                         baseRow[0].c_str(), row[1].c_str(), guns);
    }
    if (rows.empty()) return;
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    CreateDirectoryA((g_mergedRoot + "\\vehicles").c_str(), nullptr);
    const std::string path = g_mergedRoot + "\\vehicles\\Vehicle.txt";
    if (WriteAll(path, rows)) g_tables["VEHICLE.TXT"].push_back({path, lastMod});
    const std::string gunPath = g_mergedRoot + "\\vehicles\\VehWpns.txt";
    if (!gunRows.empty() && WriteAll(gunPath, gunRows)) g_tables["VEHWPNS.TXT"].push_back({gunPath, lastMod});
    const std::string statPath = g_mergedRoot + "\\vehicles\\VStat.txt", propPath = g_mergedRoot + "\\vehicles\\Props.txt";
    if (!statRows.empty() && WriteAll(statPath, statRows)) g_tables["VSTAT.TXT"].push_back({statPath, lastMod});
    if (!propRows.empty() && WriteAll(propPath, propRows)) g_tables["PROPS.TXT"].push_back({propPath, lastMod});
}

// ---- .unit files: new soldier types (research/custom_units.md) ----
// "key = value" lines, # comments. A unit starts as a copy of `based on` (a Units.txt row - the game's or a mod's), its
// base stats (HStat.txt) and its kit (UnitWpns.txt), and changes only what is listed:
//   id = Iraqi_Militia                 required, max 35 characters, no commas
//   based on = Iraqi_Conscript         required
//   type = SOLDIER                     SOLDIER, CIVILIAN, GOAT HERDER, EMIR, MISSILE EXPERT or POW (game behaviour)
//   model = IraqInfantry01_Sandy       a soldier model (game or mod .evo) with the standard 51-object skeleton
//   health = 60
//   experience = Veteran               Rookie, Green, Trained, Regular, Veteran, Expert, Professional, Guard, Crack,
//                                      Elite, Specialist, Skill One .. Skill Four
//   voice = Iraqi_soldier              speech bank (.sbk); one no level has is borrowed from the game's sound caches
//   item = IR_WPN_AK47                 kit, one line per item (a weapon or an inventory item, optionally ", amount");
//                                      any item line replaces the base's kit; "item = none" = no kit
//   column N = value                   Units.txt column N (1-based) for the rest
//   stat column N = value              HStat.txt column N (1-based)
// Writes Units.txt / HStat.txt (own row "<id>" when a stat changes) / UnitWpns.txt parts; column 14 (item slots) is the
// item count. CheckUnitTables validates the result like any other mod row.
std::vector<std::string> NodeNames(const std::string& evo) {   // DS1 EOBJ (version 0x2F): node name at +0x48
    std::vector<std::string> out;
    for (size_t p = evo.find("EOBJ"); p != std::string::npos; p = evo.find("EOBJ", p + 4))
        if (p + 0x48 + 64 <= evo.size() && static_cast<uint8_t>(evo[p + 4]) == 0x2F && evo[p + 5] == 0)
            out.push_back(Upper(std::string(evo.c_str() + p + 0x48, strnlen(evo.c_str() + p + 0x48, 64))));
    return out;
}

bool ModelBytes(const std::string& model, std::string& out) {
    auto it = g_files.find(Upper(model + ".evo"));
    return it != g_files.end() ? ReadAll(it->second.path, out) : ReadGameFile(model + ".evo", out);
}

void BuildUnits() {
    if (g_unitFiles.empty()) return;
    auto tableRows = [](const char* name, const char* key) {   // the game's rows + mods' own parts (not merged yet)
        std::vector<std::vector<std::string>> rows;
        std::string text;
        if (ReadGameFile(name, text)) rows = RowsOf(text);
        auto it = g_tables.find(key);   // (not g_tables[key]: an empty entry would make MergeTables look for a replace part)
        if (it != g_tables.end())
            for (const ModFile& part : it->second)
                if (ReadAll(part.path, text))
                    for (auto& r : RowsOf(text)) rows.push_back(r);
        return rows;
    };
    std::vector<std::vector<std::string>> units = tableRows("Units.txt", "UNITS.TXT"),
                                          stats = tableRows("HStat.txt", "HSTAT.TXT"),
                                          kits = tableRows("UnitWpns.txt", "UNITWPNS.TXT"),
                                          weapons = tableRows("Weaps.txt", "WEAPS.TXT"),
                                          inventory = tableRows("Inven.txt", "INVEN.TXT");
    static const char* const kTypes[] = {"SOLDIER", "CIVILIAN", "GOAT HERDER", "EMIR", "MISSILE EXPERT", "POW"};
    std::string unitRows, statRows, kitRows;
    int lastMod = 0;
    for (const ModFile& uf : g_unitFiles) {
        const std::string file = uf.path.substr(uf.path.find_last_of("\\/") + 1);
        std::string id, base, type, model, experience, voice;
        int health = -1;
        bool ownKit = false, ok = true;
        std::vector<std::pair<std::string, std::string>> items;   // item, amount
        std::vector<std::pair<int, std::string>> columns, statColumns;
        for (auto& [k, v] : ReadKeyValues(uf.path)) {
            if (k == "id") id = v;
            else if (k == "basedon") base = v;
            else if (k == "type" || k == "kind") type = v;
            else if (k == "model") model = v;
            else if (k == "health") health = atoi(v.c_str());
            else if (k == "experience") experience = v;
            else if (k == "voice" || k == "speechbank") voice = v;
            else if (k == "item") {
                ownKit = true;
                if (_stricmp(v.c_str(), "none") == 0) continue;
                const size_t comma = v.find(',');
                items.push_back({Trim(v.substr(0, comma)), comma == std::string::npos ? "0" : Trim(v.substr(comma + 1))});
            } else if (k.compare(0, 10, "statcolumn") == 0 && atoi(k.c_str() + 10) > 1) statColumns.push_back({atoi(k.c_str() + 10), v});
            else if (k.compare(0, 6, "column") == 0 && atoi(k.c_str() + 6) > 1) columns.push_back({atoi(k.c_str() + 6), v});
            else if (k == "scale") features::ModLog("[fail] Mods: %s - \"scale\" isn't supported yet (ignored)", file.c_str());
            else features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", file.c_str(), k.c_str());
        }
        const std::vector<std::string>* found = base.empty() ? nullptr : FindRow(units, base);
        if (id.empty() || id.size() > 35 || id.find(',') != std::string::npos || !found || found->size() < 14) {
            features::ModLog("[fail] Mods: %s - needs \"id = ...\" (max 35 characters, no commas) and \"based on = <unit>\" "
                             "(a Units.txt row, e.g. Iraqi_Conscript)", file.c_str());
            continue;
        }
        if (FindRow(units, id)) {
            features::ModLog("[fail] Mods: %s - unit %s exists already", file.c_str(), id.c_str());
            continue;
        }
        const std::vector<std::string> baseRow = *found;
        std::vector<std::string> row = baseRow;
        row[0] = id;
        if (!type.empty()) {
            std::string t = type;
            for (char& c : t) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if (std::find_if(std::begin(kTypes), std::end(kTypes), [&](const char* s) { return t == s; }) == std::end(kTypes)) {
                features::ModLog("[fail] Mods: %s - type \"%s\" isn't SOLDIER, CIVILIAN, GOAT HERDER, EMIR, MISSILE EXPERT "
                                 "or POW", file.c_str(), type.c_str());
                continue;
            }
            row[1] = t;
        }
        if (!model.empty()) row[2] = model;
        std::string evo, baseEvo;
        if (!ModelBytes(row[2], evo)) {
            features::ModLog("[fail] Mods: %s - model %s.evo isn't in the game or a mod", file.c_str(), row[2].c_str());
            continue;
        }
        if (_stricmp(row[2].c_str(), baseRow[2].c_str()) != 0 && ModelBytes(baseRow[2], baseEvo)) {
            const std::vector<std::string> mine = NodeNames(evo), theirs = NodeNames(baseEvo);
            if (mine != theirs) {
                features::ModLog("[fail] Mods: %s - model %s has %zu objects, %s has %zu: a soldier model needs the same "
                                 "skeleton (objects and order) as the game's soldiers - the animations have one track per "
                                 "object", file.c_str(), row[2].c_str(), mine.size(), baseRow[2].c_str(), theirs.size());
                continue;
            }
        }
        if (!experience.empty()) row[9] = experience;
        if (!voice.empty()) {
            if (voice.size() < 4 || _stricmp(voice.c_str() + voice.size() - 4, ".sbk") != 0) voice += ".sbk";
            if (_stricmp(voice.c_str(), baseRow[10].c_str()) != 0) BorrowBank(voice.substr(0, voice.size() - 4));
            row[10] = voice;
        }
        if (health >= 0 || !statColumns.empty()) {   // own base stats, named like the unit
            const std::vector<std::string>* baseStats = FindRow(stats, baseRow[4]);
            if (!baseStats || id.size() > 39) {
                features::ModLog("[fail] Mods: %s - base stats %s not found", file.c_str(), baseRow[4].c_str());
                continue;
            }
            std::vector<std::string> s = *baseStats;
            s[0] = id;
            if (health >= 0) s[1] = std::to_string(health);
            for (auto& [n, v] : statColumns)
                if (n <= static_cast<int>(s.size())) s[n - 1] = v;
                else ok = false;
            if (!ok) {
                features::ModLog("[fail] Mods: %s - HStat.txt has %zu columns", file.c_str(), s.size());
                continue;
            }
            statRows += JoinRow(s) + "\r\n";
            stats.push_back(s);
            row[4] = id;
        }
        for (auto& [n, v] : columns)
            if (n <= static_cast<int>(row.size()) && n != 14) row[n - 1] = v;
            else ok = false;
        if (!ok) {
            features::ModLog("[fail] Mods: %s - Units.txt has 14 columns; column 14 (item slots) comes from the items",
                             file.c_str());
            continue;
        }
        std::vector<std::vector<std::string>> kit;
        if (ownKit) {
            for (auto& [item, amount] : items) {
                const std::vector<std::string>* w = FindRow(weapons, item);
                const std::vector<std::string>* inv = w ? nullptr : FindRow(inventory, item);
                if (!w && (!inv || inv->size() < 2)) {
                    features::ModLog("[fail] Mods: %s - item %s isn't a weapon (Weaps.txt) or an inventory item (Inven.txt)",
                                     file.c_str(), item.c_str());
                    ok = false;
                    break;
                }
                kit.push_back({id, w ? (*w)[0] : (*inv)[0], w ? "WEAPON" : (*inv)[1], amount});
            }
            if (!ok) continue;
        } else {
            for (const auto& k : kits)
                if (k.size() >= 4 && k[0] == baseRow[0]) kit.push_back({id, k[1], k[2], k[3]});
        }
        if (kit.size() > 25) {
            features::ModLog("[fail] Mods: %s - %zu items, at most 25", file.c_str(), kit.size());
            continue;
        }
        row[13] = std::to_string(kit.size());
        ModUnit mu{id, row[2], {}};
        for (const auto& k : kit) kitRows += JoinRow(k) + "\r\n", mu.items.push_back(k[1]);
        g_modUnits.push_back(mu);
        unitRows += JoinRow(row) + "\r\n";
        units.push_back(row);
        lastMod = uf.mod;
        features::ModLog("Mods: unit %s based on %s, type %s, model %s, %zu item(s)", id.c_str(), baseRow[0].c_str(),
                         row[1].c_str(), row[2].c_str(), kit.size());
    }
    if (unitRows.empty()) return;
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    const std::string dir = g_mergedRoot + "\\units";
    CreateDirectoryA(dir.c_str(), nullptr);
    if (WriteAll(dir + "\\Units.txt", unitRows)) g_tables["UNITS.TXT"].push_back({dir + "\\Units.txt", lastMod});
    if (!statRows.empty() && WriteAll(dir + "\\HStat.txt", statRows)) g_tables["HSTAT.TXT"].push_back({dir + "\\HStat.txt", lastMod});
    if (!kitRows.empty() && WriteAll(dir + "\\UnitWpns.txt", kitRows))
        g_tables["UNITWPNS.TXT"].push_back({dir + "\\UnitWpns.txt", lastMod});
}

// ---- vehicle data from other levels (vehicles.cpp: a swapped-in vehicle the level's archive doesn't carry) ----
// A vehicle needs its model, wreck model + shard, crew animations (VEHICLE.TXT), its .DYN and the models named there
// (tracks, wheels), its guns (VEHWPNS.TXT -> WEAPS.TXT: models, muzzle flashes, animations), every texture those
// models name (TEXL), and sound banks: the engine (VSTAT.TXT column 12) and each gun's (WEAPS.TXT column 64). Files the
// level's archive (or catalog / chardata) has are left to the game; the rest are copied from whichever game archive
// has them into the merged folder the resolver serves, banks go into the level's sound cache as packs.
struct ArchiveLoc {
    std::string dat;
    uint32_t offset, size;
};

std::vector<uint32_t> ArchiveEntries(const std::string& dat) {   // {hash, offset, size} x n of a game archive
    std::vector<uint32_t> entries(0x8000 / 4);
    HANDLE f = CreateFileA((GameDir() + "\\" + dat).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    DWORD got = 0;
    ReadFile(f, entries.data(), 0x8000, &got, nullptr);
    CloseHandle(f);
    entries.resize(got / 4 / 3 * 3);
    for (size_t i = 0; i + 2 < entries.size(); i += 3)
        if (!entries[i] && !entries[i + 1] && !entries[i + 2]) {
            entries.resize(i);
            break;
        }
    return entries;
}

const std::unordered_map<uint32_t, ArchiveLoc>& GameArchiveIndex() {   // first archive per name hash (catalog first)
    static std::unordered_map<uint32_t, ArchiveLoc> index;
    if (!index.empty()) return index;
    std::vector<std::string> dats = {"catalog.dat", "chardata.dat"};
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "\\*.dat").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do
            if (_stricmp(fd.cFileName, "catalog.dat") && _stricmp(fd.cFileName, "chardata.dat")) dats.push_back(fd.cFileName);
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (const std::string& dat : dats) {
        const std::vector<uint32_t> e = ArchiveEntries(dat);
        for (size_t i = 0; i + 2 < e.size(); i += 3) index.emplace(e[i], ArchiveLoc{dat, e[i + 1], e[i + 2]});
    }
    return index;
}

bool ArchiveHas(const std::string& dat, uint32_t hash) {   // does that game archive carry the file?
    static std::unordered_map<std::string, std::unordered_map<uint32_t, bool>> sets;
    const std::string key = Upper(dat);
    auto it = sets.find(key);
    if (it == sets.end()) {
        auto& set = sets[key];
        const std::vector<uint32_t> e = ArchiveEntries(dat);
        for (size_t i = 0; i + 2 < e.size(); i += 3) set[e[i]] = true;
        it = sets.find(key);
    }
    return it->second.count(hash) != 0;
}

// The file as the level will see it: true if the game finds it anyway (level archive, catalog, chardata, a mod) or it
// was copied now. data = its bytes when wanted (models: their textures and .DYN are read from them).
bool BorrowLevelFile(const std::string& name, const std::string& levelDat, std::string* data, int& copied) {
    const std::string key = Upper(name);
    if (auto f = g_files.find(key); f != g_files.end()) return data ? ReadAll(f->second.path, *data) : true;
    const uint32_t hash = ArchiveHash(name);
    const auto& index = GameArchiveIndex();
    auto loc = index.find(hash);
    if (loc == index.end()) return false;
    const bool present = ArchiveHas(levelDat, hash) || ArchiveHas("catalog.dat", hash) || ArchiveHas("chardata.dat", hash);
    if (present && !data) return true;
    std::string bytes;
    HANDLE f = CreateFileA((GameDir() + "\\" + loc->second.dat).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return present;
    bytes.resize(loc->second.size);
    DWORD got = 0;
    SetFilePointer(f, static_cast<LONG>(loc->second.offset), nullptr, FILE_BEGIN);
    const bool ok = ReadFile(f, bytes.data(), loc->second.size, &got, nullptr) && got == loc->second.size;
    CloseHandle(f);
    if (!ok) return present;
    // [PSE asset store] dev test of the store's global fallback (assetstore.cpp, Dev\PseNoBorrow): no copy - the store
    // serves the other level's file. The bytes are still handed back (their textures / .DYN names are followed).
    if (!present && features::AssetStoreServesBorrows()) {
        if (data) data->swap(bytes);
        return true;
    }
    if (!present) {
        const std::string path = GameDataDir() + "\\" + name;
        if (!WriteAll(path, bytes)) return false;
        g_files[key] = {path, -1};
        ++copied;
    }
    if (data) data->swap(bytes);
    return true;
}

bool LooksLikeName(const std::string& t) {
    if (t.empty() || !isalpha(static_cast<unsigned char>(t[0])) || _stricmp(t.c_str(), "dummy") == 0 || t.size() > 60)
        return false;
    for (char c : t)
        if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-') return false;
    return true;
}

std::vector<std::string> TexlNames(const std::string& evo) {   // texture names of every TEXL list in a model
    std::vector<std::string> out;
    for (size_t p = evo.find("TEXL"); p != std::string::npos; p = evo.find("TEXL", p + 4)) {
        uint32_t n = 0;
        if (p + 8 > evo.size()) break;
        memcpy(&n, evo.data() + p + 4, 4);
        if (n > 64 || p + 8 + n * 68 > evo.size()) continue;
        for (uint32_t i = 0; i < n; ++i) {
            const char* name = evo.c_str() + p + 8 + i * 68 + 4;
            const std::string t(name, strnlen(name, 64));
            if (!t.empty() && _stricmp(t.c_str(), "Default") && std::find(out.begin(), out.end(), t) == out.end())
                out.push_back(t);
        }
    }
    return out;
}

// FeatMods off: the Mods folder isn't read (no index, report, tables, .weapon / .skin files) - only what other features
// need from this file is built: the kit weapons' sound banks (loadout swaps) and, through the resolver, the level sound
// caches with the squad's other-accent voices (voices.cpp).
void BuildAll() {
    if (!features::ModsFolderEnabled()) {
        BorrowKitBanks();
        return;
    }
    BeginReport(GameDir() + "\\Mods");
    BuildIndex();
    BuildReferences();
    BuildWeapons();
    BuildSkins();
    BuildGear();
    BuildVehicles();
    BuildUnits();
    MergeTables();
    CheckUnitTables();
    CheckVehicleTables();
    BorrowKitBanks();
}

void ResetState() {
    g_files.clear();
    g_largestFile = 0;
    g_largestName.clear();
    g_mods.clear();
    g_tables.clear();
    g_tableReplace.clear();
    g_texts.clear();
    g_levelTexts.clear();
    g_skinFiles.clear();
    g_skinVariants.clear();
    g_weaponFiles.clear();
    g_gearFiles.clear();
    g_pouchFiles.clear();
    g_modGear.clear();
    g_modPouchSets.clear();
    g_vehicleFiles.clear();
    g_unitFiles.clear();
    g_modUnits.clear();
    g_modVehicles.clear();
    g_vehicleHandling.clear();
    g_vehicleData.clear();
    g_bankPacks.clear();
    g_storeBanks.clear();
    g_levelSounds.clear();
    g_streams.clear();
    g_soundCaches.clear();
    g_modWeaponIds.clear();
    g_weaponFactions.clear();
    g_weaponBase.clear();
    g_weaponMagazines.clear();
    g_skinBodies.clear();
    g_skinNormals.clear();
    g_skinVoices.clear();
    g_weaponRows.clear();
    g_borrowed.clear();
    g_usesFiles.clear();
    g_envPatches.clear();
    g_rmdPatches.clear();
    g_dllPatches.clear();
}

// The game's start-up tables (FUN_00411740: file -> parser(text, size[, a, b])), in its order - names before the skins
// that look them up, weapons first. Every parser builds a fresh table for its file.
struct TableParser {
    const char* name;
    uint32_t fn;
    int args;  // extra arguments: -1 none, else a (and b for the skins)
    int a, b;
};
const TableParser kTableParsers[] = {
    {"Weaps.txt", 0x490110, -1, 0, 0},   {"Inven.txt", 0x492610, -1, 0, 0},    {"Props.txt", 0x494260, -1, 0, 0},
    {"HStat.txt", 0x4918B0, -1, 0, 0},   {"VStat.txt", 0x491AA0, -1, 0, 0},    {"fNames.txt", 0x493150, -1, 0, 0},
    {"sNames.txt", 0x493200, -1, 0, 0},  {"intIDs.txt", 0x4932B0, -1, 0, 0},   {"ConnObjs.txt", 0x496EB0, -1, 0, 0},
    {"Units.txt", 0x491E00, -1, 0, 0},   {"Vehicle.txt", 0x4921F0, -1, 0, 0},  {"featStat.txt", 0x4943C0, -1, 0, 0},
    {"Structs.txt", 0x494C70, -1, 0, 0}, {"Feats.txt", 0x4944F0, -1, 0, 0},    {"Doors.txt", 0x494770, -1, 0, 0},
    {"Targets.txt", 0x494A40, -1, 0, 0}, {"Switches.txt", 0x4F7540, 1, 0x13, 0}, {"UnitWpns.txt", 0x492AC0, 1, 1, 0},
    {"HSkills.txt", 0x492DA0, -1, 0, 0}, {"VehWpns.txt", 0x492AC0, 1, 0, 0},   {"ExMods.txt", 0x492F90, -1, 0, 0},
    {"MPSkins.txt", 0x497020, -1, 0, 0}, {"MPSkinItems.txt", 0x497180, -1, 0, 0},
};
constexpr uint32_t kSkinParser = 0x496D20;  // (text, size, side 0 SAS / 1 US / 2 RU / 3 IR, role 0 R / 1 S / 2 H / 3 C)

// Runs one table's parser on the text the game would load now (a merged / mod copy, else the archive's).
bool ParseTable(const std::string& name) {
    std::string text;
    auto it = g_files.find(Upper(name));
    if (!(it != g_files.end() ? ReadAll(it->second.path, text) : ReadGameFile(name, text))) {
        features::ModLog("[fail] Mods: reload - %s not found", name.c_str());
        return false;
    }
    text.push_back('\0');
    char* data = text.data();
    const int size = static_cast<int>(text.size() - 1);
    static const char* const kSides[] = {"SAS", "US", "RU", "IR"};
    static const char kRoles[] = {'R', 'S', 'H', 'C'};
    for (int side = 0; side < 4; ++side)
        for (int role = 0; role < 4; ++role) {
            char skin[32];
            snprintf(skin, sizeof skin, "CSkins_%s_%c.txt", kSides[side], kRoles[role]);
            if (_stricmp(skin, name.c_str()) == 0)
                return reinterpret_cast<int(__cdecl*)(char*, int, int, int)>(kSkinParser)(data, size, side, role) != 0;
        }
    for (const TableParser& t : kTableParsers)
        if (_stricmp(t.name, name.c_str()) == 0) {
            if (_stricmp(name.c_str(), "Weaps.txt") == 0)
                // single weapons the parser refuses to see twice ("More than one ... has been loaded!"): grenade
                // launcher, grenade launcher / rifle, fist, grenade stat - pointers into the old weapon table
                for (uint32_t single : {0x619838u, 0x619894u, 0x61939Cu, 0x6193A0u}) *reinterpret_cast<void**>(single) = nullptr;
            return t.args < 0 ? reinterpret_cast<int(__cdecl*)(char*, int)>(t.fn)(data, size) != 0
                              : reinterpret_cast<int(__cdecl*)(char*, int, int)>(t.fn)(data, size, t.a) != 0;
        }
    features::ModLog("Mods: reload - %s isn't a start-up table (read when a level loads)", name.c_str());
    return true;
}

int TableOrder(const std::string& upper) {  // position in the game's loading order (skins after the names)
    for (size_t i = 0; i < std::size(kTableParsers); ++i)
        if (Upper(kTableParsers[i].name) == upper) return static_cast<int>(i) * 10;
    return upper.compare(0, 7, "CSKINS_") == 0 ? 75 : 1000;  // skins: after sNames (6) / intIDs (7)
}
}  // namespace

int features::ModsGeneration() { return g_generation; }

// Hot reload (MODS menu): the mod state is built again from the folders as they are now, then the game's parsers run
// again for every start-up table a mod touched before or touches now (Weaps, CSkins_*, fNames ...), fed the text
// the game would read at start-up. (Rerunning the game's whole loader FUN_00411740 fails: by the menu its archive
// list no longer holds catalog.dat.) Each parser allocates a fresh table; the old ones stay allocated, so anything
// still pointing at them - the front-end's own soldiers - stays valid; mod weapons are appended after the game's
// rows, so the game's weapons keep their indices. Files resolved at a level load (models, textures, animations,
// sound caches) simply follow the new index. Front-end only.
bool features::ReloadMods() {
    if (!features::ModsFolderEnabled()) return false;   // (no MODS menu then)
    std::vector<std::string> tables;
    for (auto& [key, parts] : g_tables) tables.push_back(key);
    ResetState();
    features::ResetWeaponIcons();
    BuildAll();
    ++g_generation;
    for (auto& [key, parts] : g_tables)
        if (std::find(tables.begin(), tables.end(), key) == tables.end()) tables.push_back(key);
    std::sort(tables.begin(), tables.end(), [](const std::string& x, const std::string& y) {
        return TableOrder(x) != TableOrder(y) ? TableOrder(x) < TableOrder(y) : x < y;
    });
    bool ok = true;
    for (const std::string& t : tables) {
        std::string name = t;  // the parser table and our lookups are case-insensitive; the archive hash upper-cases
        ok &= ParseTable(name);
    }
    dslog::Write(ok ? "Mods: reloaded - %zu mods, %zu files, %zu tables" : "[fail] Mods: reload - %zu mods, %zu files, %zu tables",
                 g_mods.size(), g_files.size(), tables.size());
    return ok;
}

// Mission textures: a level's own archive hands the texture manager each texture as a memory buffer (texture +0x4C
// data, +0x50 size), and the per-texture load FUN_00549a10 (fastcall texture; called from 0x54A3CB / 0x54A42D /
// 0x54A4A4) decodes that buffer without asking for a file - so a mod's file of the same name was never looked at.
// When a mod has the texture's name (+8, with or without .dds / .tga), the buffer is dropped for the call and the
// load takes the file path (FUN_00548ec0 -> the resolver -> the mod's file).
constexpr uint32_t kTextureLoad = 0x549A10;
constexpr uint32_t kTextureLoadCalls[] = {0x54A3CB, 0x54A42D, 0x54A4A4};
int __fastcall TextureLoad(uint8_t* tex) {
    const char* name = tex ? *reinterpret_cast<const char**>(tex + 8) : nullptr;
#ifndef DS_DIST
    {
        static DWORD on = 2;
        if (on == 2) {
            DWORD size = sizeof on;
            on = 0;
            RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "TextureLog", RRF_RT_REG_DWORD, nullptr, &on, &size);
        }
        if (on && name)
            dslog::Write("[dev]  texture load %s (buffer %p, %u bytes, flags %08X)", name, *reinterpret_cast<void**>(tex + 0x4C),
                         *reinterpret_cast<uint32_t*>(tex + 0x50), *reinterpret_cast<uint32_t*>(tex + 0x48));
    }
#endif
    if (name && !g_files.empty() && *reinterpret_cast<void**>(tex + 0x4C)) {
        bool mod = Find(name) != nullptr;
        for (const char* ext : {".dds", ".tga"})
            if (!mod) mod = Find((std::string(name) + ext).c_str()) != nullptr;
        if (mod) {
            *reinterpret_cast<void**>(tex + 0x4C) = nullptr;
            *reinterpret_cast<uint32_t*>(tex + 0x50) = 0;
            features::ModLog("Mods: texture %s replaced in this level", name);
        }
    }
    return reinterpret_cast<int(__fastcall*)(uint8_t*)>(kTextureLoad)(tex);
}

// ---- streamed sounds ----
// Cutscene and objective speech are streams: PFST entries of the level sound cache {id, u32 ADPCM bytes per channel,
// u16 rate 44097, u8 format 4, u8 n, path "data\\sound\\English\\CS_M03_S01.psf"}, read by WaveDataSourcePsfStream
// (vtable 0x5DAF70) - its load-from-cache 0x5610E0 (thiscall, ret 4) keeps the path at +0x18 and the length at +0x14;
// open 0x561250 fopens the path itself (no resolver), skips the 8-byte header and the reader stops at +0x14. Each
// file is a whole mix (music, effects, speech), stereo 44.1 kHz, 16-byte ADPCM blocks interleaved L / R. The load is
// wrapped: a mod's file for that language (or for all) takes the path, and its length the size.
constexpr uint32_t kStreamLoad = 0x5610E0;
constexpr uint8_t kStreamLoadEntry[] = {0x83, 0xEC, 0x0C, 0x8D, 0x44, 0x24, 0x00};  // sub esp,0xC; lea eax,[esp]
using StreamLoadFn = char(__thiscall*)(void*, void*);
StreamLoadFn g_streamLoad = nullptr;

char __fastcall StreamLoad(uint8_t* reader, void*, void* cache) {
    const char r = g_streamLoad(reader, cache);
    char*& path = *reinterpret_cast<char**>(reader + 0x18);
    if (!r || !path || g_streams.empty()) return r;
    const std::string game = Upper(path);
    const size_t slash = game.find_last_of("\\/");
    const std::string name = game.substr(slash + 1);
    const size_t folderStart = game.find_last_of("\\/", slash - 1);
    const std::string language = slash == std::string::npos || folderStart == std::string::npos
                                     ? std::string() : game.substr(folderStart + 1, slash - folderStart - 1);
    auto it = g_streams.find(language + "\\" + name);
    if (it == g_streams.end()) it = g_streams.find(name);
    if (it == g_streams.end()) return r;
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(it->second.c_str(), GetFileExInfoStandard, &info) || info.nFileSizeLow < 8 + 32) return r;
    using Alloc = char*(__cdecl*)(int, int);
    char* copy = reinterpret_cast<Alloc>(0x4B8450)(static_cast<int>(it->second.size()) + 1, 0x14);
    if (!copy) return r;
    memcpy(copy, it->second.c_str(), it->second.size() + 1);
    reinterpret_cast<void(__cdecl*)(void*)>(0x4B8590)(path);
    path = copy;
    *reinterpret_cast<uint32_t*>(reader + 0x14) = (info.nFileSizeLow - 8) / 32 * 16;   // whole L / R block pairs
    static std::unordered_map<std::string, bool> logged;
    if (!logged[it->second]) {
        logged[it->second] = true;
        features::ModLog("Mods: %s %s -> %s", language.c_str(), name.c_str(), it->second.c_str());
    }
    return r;
}

#ifndef DS_DIST
// Dev: the stream open's fopen (call 0x56732D at 0x561269) logs each stream file opened.
void* __cdecl StreamOpen(const char* path, const char* mode) {
    dslog::Write("[dev] stream opened: %s", path ? path : "(null)");
    return reinterpret_cast<void*(__cdecl*)(const char*, const char*)>(0x56732D)(path, mode);
}
#endif

// "Does this file exist?" FUN_004bfc80(name): in archive mode it only asks the archive index (tail jump at 0x4BFC89 to
// FUN_004be360, name still on the stack) - a file only a mod has was "missing". Its callers then skip the file without
// opening it, e.g. a vehicle's <model>.dyn (FUN_00512a30: tracks, wheels, physics) for a vehicle a mod brings into a
// level that has none (user: tanks placed in Mission 1 had no treads).
namespace {
constexpr uint32_t kArchiveExistsJump = 0x4BFC89, kArchiveExists = 0x4BE360;
int __cdecl ArchiveExists(const char* name) {
    if (Find(name)) return 1;
    return reinterpret_cast<int(__cdecl*)(const char*)>(kArchiveExists)(name);
}
}  // namespace

// Script API +0x58 (0x481360) loads a mission's texts: thiscall FUN_004f56d0([0x60EDCC], "M01Strs.loc") at 0x48136F.
// The name picks the "[M01]" section of the .strings files.
constexpr uint32_t kLoadLocCall = 0x48136F, kLoadLoc = 0x4F56D0;
int __fastcall LoadLevelTexts(void* self, void*, const char* name) {
    std::string base = name ? name : "";
    base = base.substr(base.find_last_of("\\/") + 1);
    for (char& c : base) c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    const size_t cut = base.find("STRS");
    g_levelLoc = cut != std::string::npos ? base.substr(0, cut) : base.substr(0, base.find('.'));
    using Fn = int(__thiscall*)(void*, const char*);
    return reinterpret_cast<Fn>(kLoadLoc)(self, name);
}

// "+level <name>" (the world editor's Play): the mission lives in a Mods folder - read it even with FeatMods off.
bool features::ModsFolderEnabled() {
    static const bool on = [] {
        if (settings::Get().featMods) return true;
        const char* cmd = GetCommandLineA();
        return cmd && strstr(cmd, "+level ") != nullptr;
    }();
    return on;
}

void features::ApplyMods() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!features::ModsFolderEnabled()) {
        // Only the resolver + disk open stay: level sound caches with voices / kit weapon banks (merged into %TEMP%).
        BuildAll();
        if (!patch::Matches(kResolve, kResolveEntry, sizeof kResolveEntry) || !patch::Matches(kOpen, kOpenEntry, sizeof kOpenEntry)) {
            dslog::Write("[fail] Mods: unexpected game code - level sound caches (voices, kit weapon banks) not merged");
            return;
        }
        if (HookResolveAndOpen())
            dslog::Write("[off]  Mods: Mods folder ignored (FeatMods) - level sound caches only");
        return;
    }
    if (!settings::Get().featMods) dslog::Write("Mods: FeatMods off, but +level on the command line - Mods folder read");
    if (!patch::HookCall(kLoadLocCall, reinterpret_cast<const void*>(&LoadLevelTexts), kLoadLoc))
        features::ModLog("[fail] Mods: mission text load at 0x%08X not recognised - [M01] text sections apply nowhere",
                         kLoadLocCall);
    for (uint32_t site : kTextureLoadCalls)
        if (!patch::HookCall(site, reinterpret_cast<const void*>(&TextureLoad), kTextureLoad))
            features::ModLog("[fail] Mods: texture load call at 0x%08X not recognised - mission textures can't be replaced", site);
    BuildAll();
    // The resolver hooks go in even with no mods, so mods switched on in the MODS menu work without a restart
    // (an empty index passes every name through).
    if (!patch::Matches(kResolve, kResolveEntry, sizeof kResolveEntry) || !patch::Matches(kOpen, kOpenEntry, sizeof kOpenEntry)) {
        features::ModLog("[fail] Mods: unexpected game code - mods not loaded");
        return;
    }
    if (!HookEntry(kStreamLoad, reinterpret_cast<const void*>(&StreamLoad), g_streamLoad, kStreamLoadEntry,
                   sizeof kStreamLoadEntry))
        features::ModLog("[fail] Mods: stream load at 0x%08X not recognised - cutscene sounds can't be replaced", kStreamLoad);
#ifndef DS_DIST
    patch::HookCall(0x561269, reinterpret_cast<const void*>(&StreamOpen), 0x56732D);
#endif
    const uint8_t existsJump[] = {0xE9, 0xD2, 0xE6, 0xFF, 0xFF};   // jmp 0x4BE360 (a tail jump, redirected)
    if (!patch::WriteJump(kArchiveExistsJump, reinterpret_cast<const void*>(&ArchiveExists), sizeof existsJump, existsJump))
        features::ModLog("[fail] Mods: exists check at 0x%08X not recognised - files only a mod has (vehicle .dyn) are skipped",
                         kArchiveExistsJump);
    if (HookResolveAndOpen())
        features::ModLog("[ok]   Mods: %zu mods, %zu files", g_mods.size(), g_files.size());
}

int features::ModCount() { return static_cast<int>(g_mods.size()); }
int features::ModProblems() { return g_problems; }

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

const std::vector<std::string>& features::LoadedMods() { return g_mods; }

std::vector<features::ModLevel> features::ModLevels() {
    std::vector<ModLevel> out;
    for (const auto& [key, f] : g_files) {
        const bool dll = key.size() > 4 && key.compare(key.size() - 4, 4, ".DLL") == 0;
        const bool lua = key.size() > 4 && key.compare(key.size() - 4, 4, ".LUA") == 0;
        if (!dll && !lua) continue;
        const bool built = !g_mergedRoot.empty() && _strnicmp(f.path.c_str(), g_mergedRoot.c_str(), g_mergedRoot.size()) == 0;
        const std::string name = f.path.substr(f.path.find_last_of("\\/") + 1);
        out.push_back({name, f.mod >= 0 && f.mod < static_cast<int>(g_mods.size()) ? g_mods[f.mod] : std::string(), built});
    }
    return out;
}

// A game table as the game sees it: the merged / mod copy if there is one, else the archive's.
bool features::ReadGameTable(const char* name, std::string& out) {
    auto it = g_files.find(Upper(name));
    if (it != g_files.end()) return ReadAll(it->second.path, out);
    return ReadGameFile(name, out);
}

const std::vector<features::ModVehicle>& features::ModVehicles() { return g_modVehicles; }

// A level's render pools come from "<level>.FAS" (loader FUN_004fb880 at 0x40C1B1: "Loading Bar" n, then the pool
// sizes "EnvInstances" / "EnvObjects" / "VertexBuffer" / "VBufferObject", default 2000 each), measured for the level's
// own scene. Objects made past a full pool lose parts silently - a vehicle swapped in with more parts than the scene's
// own (Mission 9: an Abrams or a Humvee for the Bradley) had no ground under it and fell through the map, and the
// loadout screen's preview vehicles weren't made at all. The level gets a copy with `extra` more of each.
void features::PadLevelPools(const char* level, int extra) {
    std::string base = level ? level : "";
    if (base.size() > 4) base.resize(base.size() - 4);
    const std::string name = base + ".FAS", key = Upper(name);
    std::string text;
    if (const ModFile* own = Find(name.c_str()); own && !StartsWith(own->path.c_str(), g_mergedRoot)) {
        if (!ReadAll(own->path, text)) return;   // a mod's own copy
    } else if (!ReadGameFile(name, text)) {
        return;
    }
    std::string out;
    int pools = 0;
    for (const std::string& line : SplitLines(text)) {
        const size_t q = line.find('"', line.find('"') + 1);
        if (q == std::string::npos || line.find("Loading Bar") != std::string::npos) {
            out += line + "\r\n";
            continue;
        }
        const int n = atoi(line.c_str() + q + 1);
        out += line.substr(0, q + 1) + " " + std::to_string(std::max(n, 2000) + extra) + "\r\n";
        ++pools;
    }
    if (!pools) return;
    const std::string path = GameDataDir() + "\\" + name;
    if (WriteAll(path, out)) {
        g_files[key] = {path, -1};
        dslog::Write("Vehicles: %s - %d render pools +%d", name.c_str(), pools, extra);
    }
}

// A vehicle the game can build: its model and its guns' models (VEHWPNS.TXT -> WEAPS.TXT column 2) are in a mod or in
// one of the game's archives. VEHICLE.TXT has rows whose models were never shipped (M1A1abrahms needs
// VHAbrams_M1A1_MG, UK_VEH_SAS_LandRover_MG needs SAS_LandRover_With_50cal) - picked, they fail to load.
bool features::VehicleDataComplete(const std::string& unit) {
    static std::unordered_map<std::string, bool> cache;
    static int generation = -1;
    if (generation != g_generation) cache.clear(), generation = g_generation;
    const std::string key = Upper(unit);
    if (auto it = cache.find(key); it != cache.end()) return it->second;
    auto exists = [](const std::string& model) {
        if (model.empty() || model == "-1") return true;
        return Find((model + ".EVO").c_str()) != nullptr || GameArchiveIndex().count(ArchiveHash(model + ".EVO")) != 0;
    };
    const auto vehicles = TableRows("VEHICLE.TXT"), guns = TableRows("VEHWPNS.TXT"), weapons = TableRows("WEAPS.TXT");
    const std::vector<std::string>* row = FindRow(vehicles, unit);
    bool ok = row && row->size() > 1 && exists((*row)[1]);
    for (const auto& g : guns)
        if (ok && g.size() > 1 && _stricmp(g[0].c_str(), unit.c_str()) == 0)
            if (const auto* w = FindRow(weapons, g[1]); w && w->size() > 1) ok = exists((*w)[1]);
    if (!ok) features::ModLog("Vehicles: %s left out - its model or a gun's model isn't in the game's data", unit.c_str());
    return cache[key] = ok;
}

// Every model (+ its TEXL textures, its .DYN and the models named there) or animation named in `names` that the level's
// archive lacks: copied from another game archive (BorrowLevelFile). Names that aren't files are skipped.
void BorrowNamedFiles(std::vector<std::string> names, const std::string& levelDat, int& copied, int& missing) {
    std::vector<std::string> done;
    for (size_t i = 0; i < names.size(); ++i) {   // (grows: names found in .DYN files)
        const std::string n = names[i];
        if (!LooksLikeName(n) || std::find(done.begin(), done.end(), Upper(n)) != done.end()) continue;
        done.push_back(Upper(n));
        std::string evo;
        if (BorrowLevelFile(n + ".EVO", levelDat, &evo, copied)) {
            for (const std::string& t : TexlNames(evo)) {
                if (t.find('.') != std::string::npos) {
                    if (!BorrowLevelFile(t, levelDat, nullptr, copied)) ++missing;
                } else if (!BorrowLevelFile(t + ".DDS", levelDat, nullptr, copied) &&
                           !BorrowLevelFile(t + ".TGA", levelDat, nullptr, copied)) {
                    ++missing;
                }
            }
            std::string dyn;
            if (BorrowLevelFile(n + ".DYN", levelDat, &dyn, copied)) {
                std::string word;
                for (char c : dyn + " ") {
                    if (isspace(static_cast<unsigned char>(c)) || c == '"' || c == ',') {
                        if (LooksLikeName(word)) names.push_back(word);
                        word.clear();
                    } else {
                        word += c;
                    }
                }
            }
            continue;
        }
        BorrowLevelFile(n + ".PRB", levelDat, nullptr, copied);   // crew / gunner / weapon animations
    }
}

// .unit soldier types in any level: their model and their kit's files (Weaps.txt / Inven.txt row names: models,
// muzzle flashes, animations) and the weapons' sound banks, copied in like a swapped-in vehicle's. Called at level init
// (scripthost.cpp, next to VehiclesLevelStart) - before the level's archive and sound cache are read.
void features::BorrowUnitData(const char* level) {
    if (g_modUnits.empty() || !level) return;
    std::string base = level;
    if (base.size() > 4) base.resize(base.size() - 4);
    const std::string levelDat = base + ".dat";
    const auto weapons = TableRows("WEAPS.TXT"), inventory = TableRows("INVEN.TXT");
    bool newBanks = false;
    for (const ModUnit& u : g_modUnits) {
        const std::string key = "UNIT " + Upper(u.id) + "|" + Upper(levelDat);
        if (g_vehicleData.count(key)) continue;
        g_vehicleData[key] = true;
        std::vector<std::string> names = {u.model}, banks;
        for (const std::string& item : u.items) {
            if (const auto* w = FindRow(weapons, item)) {
                names.insert(names.end(), w->begin() + 1, w->end());
                if (w->size() > 63) banks.push_back((*w)[63]);
            } else if (const auto* inv = FindRow(inventory, item)) {
                names.insert(names.end(), inv->begin() + 1, inv->end());
                if (inv->size() > 12) banks.push_back((*inv)[12]);
            }
        }
        int copied = 0, missing = 0;
        const bool store = features::AssetStoreServesBorrows();   // the asset store serves other levels' files
        if (!store) BorrowNamedFiles(names, levelDat, copied, missing);
        for (const std::string& b : banks)
            if (LooksLikeName(b) && _stricmp(b.c_str(), "None")) newBanks |= BorrowBank(b);
        if (store)
            features::ModLog("Units: %s for %s - files of other levels served by the asset store", u.id.c_str(), base.c_str());
        else if (copied || missing)
            features::ModLog("Units: %s for %s - %d file(s) copied from other levels%s", u.id.c_str(), base.c_str(), copied,
                             missing ? ", some textures not found" : "");
    }
    if (newBanks) {   // the level's sound cache is built again with the new packs
        const std::string prefix = Upper(base) + ".SCH|";
        for (auto it = g_soundCaches.begin(); it != g_soundCaches.end();)
            it = it->first.compare(0, prefix.size(), prefix) == 0 ? g_soundCaches.erase(it) : std::next(it);
    }
}

void features::BorrowVehicleData(const char* level, const std::vector<std::string>& units) {
    std::string base = level ? level : "";
    if (base.size() > 4) base.resize(base.size() - 4);   // the game's own rule: "Mission9.dll" -> mission9.dat / .sch
    const std::string levelDat = base + ".dat";
    const auto vehicles = TableRows("VEHICLE.TXT"), stats = TableRows("VSTAT.TXT"), guns = TableRows("VEHWPNS.TXT"),
               weapons = TableRows("WEAPS.TXT");
    bool newBanks = false;
    for (const std::string& unit : units) {
        const std::string key = Upper(unit) + "|" + Upper(levelDat);
        if (g_vehicleData.count(key)) continue;
        g_vehicleData[key] = true;
        const std::vector<std::string>* row = FindRow(vehicles, unit);
        if (!row || row->size() < 5) {
            features::ModLog("[fail] Vehicles: %s isn't in VEHICLE.TXT", unit.c_str());
            continue;
        }
        std::vector<std::string> names(row->begin() + 1, row->end()), banks;
        if (const auto* st = FindRow(stats, (*row)[4]); st && st->size() > 12) banks.push_back((*st)[12]);
        for (const auto& g : guns)
            if (g.size() > 1 && _stricmp(g[0].c_str(), unit.c_str()) == 0)
                if (const auto* w = FindRow(weapons, g[1])) {
                    names.insert(names.end(), w->begin() + 1, w->end());
                    if (w->size() > 63) banks.push_back((*w)[63]);
                }
        int copied = 0, missing = 0;
        const bool store = features::AssetStoreServesBorrows();   // the asset store serves other levels' files
        if (!store) BorrowNamedFiles(names, levelDat, copied, missing);
        bool noBanks = false;   // dev A/B: Dev\VehicleNoBanks = 1 borrows no sound banks
#ifndef DS_DIST
        {
            DWORD v = 0, size = sizeof v;
            noBanks = RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStormFix\\Dev", "VehicleNoBanks", RRF_RT_REG_DWORD,
                                   nullptr, &v, &size) == ERROR_SUCCESS && v;
        }
#endif
        for (const std::string& b : banks)
            if (!noBanks && LooksLikeName(b) && _stricmp(b.c_str(), "None")) newBanks |= BorrowBank(b);
        features::ModLog("Vehicles: %s for %s - %d file(s) copied from other levels%s%s", unit.c_str(), base.c_str(), copied,
                         missing ? ", some textures not found" : "", store ? " (served by the asset store)" : "");
    }
    if (newBanks) {   // the level's sound cache is built again with the new packs
        const std::string prefix = Upper(base) + ".SCH|";
        for (auto it = g_soundCaches.begin(); it != g_soundCaches.end();)
            it = it->first.compare(0, prefix.size(), prefix) == 0 ? g_soundCaches.erase(it) : std::next(it);
    }
}

const std::vector<features::VehicleHandling>& features::ModVehicleHandling() { return g_vehicleHandling; }
bool features::ModHasFile(const char* name) { return Find(name) != nullptr; }
uint32_t features::LargestModFile(std::string* name) {
    if (name) *name = g_largestName;
    return g_largestFile;
}

std::string features::ModFilePath(const char* name) {
    const ModFile* f = Find(name);
    return f ? f->path : std::string();
}

const char* features::ModText(uint32_t hash) {
    if (!g_levelLoc.empty() && !g_levelTexts.empty()) {
        auto lv = g_levelTexts.find(g_levelLoc);
        if (lv != g_levelTexts.end()) {
            auto it = lv->second.find(hash);
            if (it != lv->second.end()) return it->second.c_str();
        }
    }
    if (g_texts.empty()) return nullptr;
    auto it = g_texts.find(hash);
    return it == g_texts.end() ? nullptr : it->second.c_str();
}

const std::vector<std::string>& features::ModWeapons() { return g_modWeaponIds; }
int features::SpeechLanguage() { return SpeechLanguageNow(); }

std::string features::SkinVoice(const char* texture) {
    auto it = g_skinVoices.find(Upper(texture ? texture : ""));
    return it == g_skinVoices.end() ? std::string() : it->second;
}

std::string features::SkinNormalMap(const char* texture) {
    if (!texture || g_skinNormals.empty()) return {};
    auto it = g_skinNormals.find(Upper(texture));
    return it == g_skinNormals.end() ? std::string() : it->second;
}

std::string features::SkinBody(const char* texture) {
    if (!texture || g_skinBodies.empty()) return {};
    auto it = g_skinBodies.find(Upper(texture));
    return it == g_skinBodies.end() ? std::string() : it->second;
}

int features::ModWeaponMagazines(const std::string& id) {
    auto it = g_weaponMagazines.find(Upper(id));
    return it == g_weaponMagazines.end() ? 0 : it->second;
}

// A .weapon's "faction", else its "based on" weapon's, else by the game's naming: IR_ weapons are the Russian / Iraqi
// side's (the multiplayer kits, MPSkinItems.txt, give them to RUSSIAN and IRAQI), the rest the SAS / Delta side's.
int features::WeaponFactions(const char* id) {
    std::string cur = id ? id : "";
    for (int depth = 0; depth < 8 && !cur.empty(); ++depth) {
        const std::string up = Upper(cur);
        if (auto it = g_weaponFactions.find(up); it != g_weaponFactions.end()) return it->second;
        auto base = g_weaponBase.find(up);
        if (base == g_weaponBase.end() || base->second.empty()) break;
        cur = base->second;
    }
    return _strnicmp(cur.c_str(), "IR_", 3) == 0 ? kFactionRussian | kFactionIraqi : kFactionSas | kFactionDelta;
}
