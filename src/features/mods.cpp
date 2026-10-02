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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"

namespace {
constexpr uint32_t kResolve = 0x4BE810, kResolveCont = 0x4BE815;
constexpr uint8_t kResolveEntry[] = {0xA1, 0x60, 0x83, 0x5F, 0x00};  // mov eax, [0x5F8360]
constexpr uint32_t kPathBuffer = 0x63BF28;                            // the resolver's result buffer
constexpr size_t kPathBufferSize = 0x600;

using ResolveFn = char*(__cdecl*)(const char*, void*, int);
ResolveFn g_resolve = nullptr;

constexpr uint32_t kOpen = 0x4BD8A0, kOpenCont = 0x4BD8AA;
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
std::unordered_map<uint32_t, std::string> g_texts;               // catalog hash -> text (display names)
std::vector<ModFile> g_skinFiles;                                 // .skin: readable skin definitions
std::vector<features::SkinVariant> g_skinVariants;                // .skin files with "uniform name"
std::vector<ModFile> g_weaponFiles;                               // .weapon: readable weapon definitions
std::vector<std::string> g_bankPacks;                             // .sch files: sound banks added to every level
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
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".TXT") == 0) {
            g_tables[key].push_back({path, mod});
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
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".SCH") == 0) {
            g_bankPacks.push_back(path);
            ++count;
            continue;
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

// Every mod table becomes one file: the game's rows (if the game has that file) + each mod's rows in order.
void MergeTables() {
    if (g_tables.empty()) return;
    const std::string dir = g_mergedRoot = TempDir();
    for (auto& [key, parts] : g_tables) {
        std::string text;
        const std::string name = parts.front().path.substr(parts.front().path.find_last_of("\\/") + 1);
        const bool game = ReadGameFile(name, text);
        for (const ModFile& part : parts) {
            std::string rows;
            if (ReadAll(part.path, rows)) AppendRows(text, rows);
        }
        if (!game && parts.size() == 1) {  // a new table: used as it is
            g_files[key] = parts.front();
            continue;
        }
        const std::string out = dir + "\\" + name;
        HANDLE f = CreateFileA(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) {
            features::ModLog("[fail] Mods: can't write %s", out.c_str());
            continue;
        }
        DWORD written = 0;
        WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        CloseHandle(f);
        g_files[key] = {out, parts.back().mod};
        features::ModLog("Mods: %s = %s + %zu mod part(s)", name.c_str(), game ? "game rows" : "no game file", parts.size());
    }
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

std::string SoundCache(const char* name) {
    const std::string key = Upper(name);
    auto cached = g_soundCaches.find(key);
    if (cached != g_soundCaches.end()) return cached->second;
    std::string& result = g_soundCaches[key];
    std::string file;
    if (!ReadAll(GameDir() + "\\" + name, file)) return result;
    std::vector<Chunk> level = Chunks(file);
    if (level.empty()) return result;
    std::unordered_map<uint32_t, bool> banks;
    std::unordered_map<uint64_t, bool> samples;
    for (const Chunk& c : level) {
        if (c.tag == kTagBank) banks[BankKey(c)] = true;
        if (c.tag == kTagSample || c.tag == kTagStream) samples[SampleKey(c)] = true;
    }
    std::string extra;
    int added = 0;
    for (const std::string& path : g_bankPacks) {
        std::string pack;
        if (!ReadAll(path, pack)) continue;
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
    }
    if (extra.empty()) return result;
    file += extra;
    const uint32_t body = static_cast<uint32_t>(file.size() - 8);
    memcpy(file.data() + 4, &body, 4);
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    const std::string out = g_mergedRoot + "\\" + name;
    HANDLE f = CreateFileA(out.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return result;
    DWORD written = 0;
    const bool ok = WriteFile(f, file.data(), static_cast<DWORD>(file.size()), &written, nullptr) && written == file.size();
    CloseHandle(f);
    if (ok) {
        result = out;
        features::ModLog("Mods: %s + %d sound bank(s), %zu bytes of mod sound", name, added, extra.size());
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
std::vector<std::string> g_borrowed;
void BorrowBank(const std::string& name) {
    for (const std::string& b : g_borrowed)
        if (b == name) return;
    g_borrowed.push_back(name);
    const uint32_t key = ElfHash(name + ".sbk");
    for (const std::string& path : g_bankPacks) {  // a mod's own .sch already brings it
        std::string pack;
        if (!ReadAll(path, pack)) continue;
        for (const Chunk& c : Chunks(pack))
            if (c.tag == kTagBank && BankKey(c) == key) {
                features::ModLog("Mods: sound bank %s from %s", name.c_str(), path.c_str());
                return;
            }
    }
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((GameDir() + "\\*.sch").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
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
        }
        return;
    }
    features::ModLog("[fail] Mods: sound bank %s is in no level sound cache", name.c_str());
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
    std::string rows;
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
                    features::ModLog("[fail] Mods: %s - \"%s\" ignored: %s is a game weapon - to change it, make a new one "
                                 "(\"id = <new id>\", \"based on = %s\")", file.c_str(), k.c_str(), id.c_str(), id.c_str());
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
        {
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
        rows += line + "\r\n";
        g_weaponRows.push_back(row);
        g_modWeaponIds.push_back(id);
        if (!ownSound && row[kColSound] != "-1" && row[kColSound] != baseSound) BorrowBank(row[kColSound]);
        features::ModLog("Mods: weapon %s (%s) based on %s, sound %s", id.c_str(), file.c_str(), base.c_str(),
                     row[kColSound].c_str());
    }
    if (rows.empty()) return;
    if (g_mergedRoot.empty()) g_mergedRoot = TempDir();
    CreateDirectoryA((g_mergedRoot + "\\weapons").c_str(), nullptr);
    const std::string path = g_mergedRoot + "\\weapons\\Weaps.txt";  // one more part of the Weaps.txt table
    if (WriteAll(path, rows)) g_tables["WEAPS.TXT"].push_back({path, g_weaponFiles.back().mod});
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
        std::string uniform, soldier, texture, portrait, first, last, uniformName, body;
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
        if (!body.empty()) {  // bodies.cpp swaps the soldier's model whenever this texture's skin is put on
            if (g_files.find(Upper(body + ".evo")) == g_files.end())
                features::ModLog("[fail] Mods: %s - body %s: no %s.evo in a mod folder", file.c_str(), body.c_str(), body.c_str());
            else
                g_skinBodies[Upper(texture)] = body;
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
    if (!g_bankPacks.empty() && name && !strchr(name, ':') && !strpbrk(name, "\\/")) {
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

uint8_t* Trampoline(const uint8_t* entry, size_t len, uint32_t cont) {
    auto* tramp = static_cast<uint8_t*>(VirtualAlloc(nullptr, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!tramp) return nullptr;
    memcpy(tramp, entry, len);
    tramp[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>(cont - (reinterpret_cast<uint32_t>(tramp) + len + 5));
    memcpy(tramp + len + 1, &rel, 4);
    return tramp;
}
}  // namespace

const std::vector<features::SkinVariant>& features::SkinVariants() { return g_skinVariants; }

namespace {
int g_generation = 0;  // bumped by every reload (features::ModsGeneration: caches of mod data refresh)

void BuildAll() {
    BeginReport(GameDir() + "\\Mods");
    BuildIndex();
    BuildWeapons();
    BuildSkins();
    MergeTables();
    BorrowKitBanks();
}

void ResetState() {
    g_files.clear();
    g_mods.clear();
    g_tables.clear();
    g_texts.clear();
    g_skinFiles.clear();
    g_skinVariants.clear();
    g_weaponFiles.clear();
    g_bankPacks.clear();
    g_soundCaches.clear();
    g_modWeaponIds.clear();
    g_weaponFactions.clear();
    g_weaponBase.clear();
    g_weaponMagazines.clear();
    g_skinBodies.clear();
    g_weaponRows.clear();
    g_borrowed.clear();
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

void features::ApplyMods() {
    static bool done = false;
    if (done) return;
    done = true;
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
    g_resolve = reinterpret_cast<ResolveFn>(Trampoline(kResolveEntry, sizeof kResolveEntry, kResolveCont));
    g_open = reinterpret_cast<OpenFn>(Trampoline(kOpenEntry, sizeof kOpenEntry, kOpenCont));
    if (!g_resolve || !g_open) return;
    if (patch::WriteJump(kResolve, reinterpret_cast<const void*>(&Resolve)) &&
        patch::WriteJump(kOpen, reinterpret_cast<const void*>(&Open)))
        features::ModLog("Mods: %zu mods, %zu files", g_mods.size(), g_files.size());
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

// A game table as the game sees it: the merged / mod copy if there is one, else the archive's.
bool features::ReadGameTable(const char* name, std::string& out) {
    auto it = g_files.find(Upper(name));
    if (it != g_files.end()) return ReadAll(it->second.path, out);
    return ReadGameFile(name, out);
}

bool features::ModHasFile(const char* name) { return Find(name) != nullptr; }

const char* features::ModText(uint32_t hash) {
    if (g_texts.empty()) return nullptr;
    auto it = g_texts.find(hash);
    return it == g_texts.end() ? nullptr : it->second.c_str();
}

const std::vector<std::string>& features::ModWeapons() { return g_modWeaponIds; }
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
