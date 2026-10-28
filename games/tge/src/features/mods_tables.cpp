// Mods folder, part 2 (tgefix.modtables): data tables, texts and readable weapon / vehicle / unit files - port of
// DesertStorm2Fix's mods_tables.cpp (games/ds2/src/features/mods_tables.cpp; DS2 addresses in brackets) to
// GreatEscape.exe. Modder page: docs/sdk/reference/game-tge.md "Mods: tables, texts".
//
// How GreatEscape.exe reads its tables (mapped 2026-10-25):
//  - loader FUN_005ccfb0 [FUN_0040b360] reads 23 tables whole (FUN_004c5240(name, &buf, &size, Global Heap
//    [0x7CDC14], 6) [FUN_004c49e0] -> File_Resolve 0x4C44D0, hooked by mods.cpp) and hands each to its parser; a parser
//    returning 0 makes the load fail. Called at start-up (App_Init 0x5D3A9E) AND by Level_Init (0x5D21B9) for every
//    mission - so merged tables take effect at the next mission load. Order and parsers: Uniform 0x5979C0, Faces
//    0x595700, Weaps 0x59A030, Inven 0x598A90, Props 0x598750, HStat 0x5970D0, VStat 0x599C90, fNames 0x596A80, sNames
//    0x5969D0, intIDs 0x596920, Units 0x599820, Animal 0x599450, Vehicle 0x599040, featStat 0x5958E0, Structs 0x597C80,
//    Feats 0x5984A0, Doors 0x598120, Targets 0x597E50, Switches 0x485F70, UnitWpns 0x596E30(text, size, 1), HSkills
//    0x596CD0, VehWpns 0x596E30(text, size, 0), ExMods 0x596B30. Every table is in catalog.dat (and a copy in every
//    level*.dat); archive hash = LFSR over the LOWER-cased name. Error prints (0x4D0050) are stubbed: a bad row just
//    makes the parser return 0.
//  - Parsers read rows with sscanf (0x51F576) into 36-byte stack buffers and fail on: Units col 4 not an HStat row
//    ("Invalid base stat"), an unknown experience (0x595A10: Rookie ... Skill Four), an UnitWpns row whose unit isn't a
//    Units row, whose type isn't one of 0x595F90's (exact, case-sensitive) or whose item isn't a Weaps (WEAPON) / Inven
//    row (exact strcmp), more UnitWpns rows for a unit than its Units col 11 item slots, the same for VehWpns and
//    Vehicle.txt col 22 (gun slots), a VehWpns item that isn't a weapon, Vehicle col 4 not a VStat row, a second weapon
//    of the categories GRENADE LAUNCHER, GRENADE LAUNCH/RIFLE, FIST, a magazine over 999. Those rows are left out of
//    the merged tables here with a [fail] line instead.
//  - Weaps.txt has 74 columns (DS2 79, DS1 75): 0 id, 1 model, 3 picture (IMAGE_INV_*, lookup 0x594150 - unknown
//    names give picture 0x11), 4 category, 5 connector, 9 time between shots, 13 magazines, 14 magazine size, 15 reload
//    time, 21 damage, 22 range, 34 muzzle flash, 52 upright aim / 54 upright reload / 56 kneel aim / 58 kneel reload /
//    59 prone aim / 61 prone reload animations, 67 sound bank, 68 hearing range. Units.txt has 15: 0 name, 1 type
//    (SOLDIER, GOAT HERDER, CIVILIAN, EMIR), 2 body, 3 second body, 4 HStat row, 5 picture, 6 / 7 fNames / sNames, 9
//    experience, 10 speech bank, 11 item slots (= its UnitWpns rows), 12 Faces.txt row, 13 AI type. HStat.txt 22 (1 =
//    health). Vehicle.txt is comma-separated, 26 columns: 1 model, 2 wreck model, 4 VStat row, 22 gun slots.
//  - Texts: hash = the archive LFSR over the lower-cased key; lookup FUN_0047ea90 thiscall(catalog [0x7DC26C], hash)
//    -> UTF-16 [FUN_004fbe50] - hooked here (chained with modsmenu.cpp's hook on the same entry) to answer ModText.
//
// Files built here go to %TEMP%\GreatEscapeFix\merged\<content hash>\<name> and are served through mods.cpp's index
// (SetModFile).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "loader.h"

namespace {

// ---- small helpers ----
std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::string Trim(const std::string& v) {
    const size_t a = v.find_first_not_of(" \t\r\n"), b = v.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
}
// "Magazine Size" / "magazine_size" -> "magazinesize"
std::string KeyOf(const std::string& k) {
    std::string out;
    for (char c : k)
        if (c != ' ' && c != '_' && c != '-' && c != '\t') out += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return out;
}
bool Yes(const std::string& v) {
    const std::string k = KeyOf(v);
    return k == "yes" || k == "on" || k == "true" || k == "1";
}
bool EndsWith(const std::string& s, const char* tail) {
    const size_t n = strlen(tail);
    return s.size() >= n && _stricmp(s.c_str() + s.size() - n, tail) == 0;
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

// Archive / catalog hash: LFSR over the lower-cased name. Texture keys in EVO files and catalog text keys use the same.
uint32_t LowerHash(const std::string& name) {
    uint32_t x = 1;
    for (char ch : name) {
        const unsigned char c = static_cast<unsigned char>(tolower(static_cast<unsigned char>(ch)));
        for (int b = 0; b < 8; ++b) x = x << 1 | (((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1);
    }
    return x;
}
uint32_t Fnv(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

std::string GameDir() {
    char exe[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    const size_t slash = dir.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : dir.substr(0, slash);
}

// ---- the game's own files (.dat archives in the game folder; catalog.dat first: it holds the tables) ----
struct Archive {
    std::string path;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> index;   // hash -> offset, size
};
std::vector<Archive> g_archives;

void LoadArchives() {
    if (!g_archives.empty()) return;
    const std::string dir = GameDir();
    std::vector<std::string> names = {"catalog.dat"};
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.dat").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (_stricmp(fd.cFileName, "catalog.dat") && _strnicmp(fd.cFileName, "unins", 5)) names.push_back(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    for (const std::string& n : names) {
        Archive a;
        a.path = dir + "\\" + n;
        HANDLE f = CreateFileA(a.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        uint32_t first[3] = {};
        DWORD got = 0;
        ReadFile(f, first, 12, &got, nullptr);
        const DWORD fileSize = GetFileSize(f, nullptr);
        const uint32_t dirSize = std::min<uint32_t>(first[1], 1u << 22);   // the directory ends where the data starts
        std::vector<uint32_t> index(dirSize / 4 + 3);
        SetFilePointer(f, 0, nullptr, FILE_BEGIN);
        ReadFile(f, index.data(), dirSize, &got, nullptr);
        CloseHandle(f);
        for (size_t i = 0; i + 2 < got / 4; i += 3) {
            const uint32_t hash = index[i], off = index[i + 1], size = index[i + 2];
            if (!hash && !off && !size) break;
            if (off + uint64_t{size} <= fileSize) a.index.emplace(hash, std::make_pair(off, size));
        }
        g_archives.push_back(std::move(a));
    }
}

bool GameHas(const std::string& name) {
    LoadArchives();
    const uint32_t hash = LowerHash(name);
    for (const Archive& a : g_archives)
        if (a.index.count(hash)) return true;
    return false;
}

bool ReadGameFile(const std::string& name, std::string& out) {
    LoadArchives();
    const uint32_t hash = LowerHash(name);
    for (const Archive& a : g_archives) {
        auto it = a.index.find(hash);
        if (it == a.index.end()) continue;
        HANDLE f = CreateFileA(a.path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) continue;
        out.resize(it->second.second);
        DWORD got = 0;
        SetFilePointer(f, static_cast<LONG>(it->second.first), nullptr, FILE_BEGIN);
        const bool ok = ReadFile(f, out.data(), it->second.second, &got, nullptr) && got == it->second.second;
        CloseHandle(f);
        if (ok) return true;
    }
    return false;
}

// ---- the mods' files ----
struct Part {
    std::string path;   // "" = generated (text below)
    std::string text;
    int mod = 0;
    std::string label;  // for the report: "<mod>\file"
};
struct Scan {
    std::map<std::string, std::vector<Part>> add, replace;   // upper table name ("WEAPS.TXT") -> parts in mod order
    std::vector<Part> strings, weapons, vehicles, units;
} g_scan;
std::vector<std::string> g_modNames;
std::unordered_map<std::string, std::string> g_modFiles;   // upper file name -> path (later mod wins), for models

void ScanFolder(const std::string& dir, int mod) {
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> files, dirs;
    do {
        if (fd.cFileName[0] == '.') continue;
        (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? dirs : files).push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    auto byName = [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; };
    std::sort(files.begin(), files.end(), byName);
    std::sort(dirs.begin(), dirs.end(), byName);
    for (const std::string& name : files) {
        const std::string path = dir + "\\" + name;
        const std::string key = Upper(name);
        g_modFiles[key] = path;
        Part p{path, {}, mod, g_modNames[mod] + "\\" + name};
        if (EndsWith(key, ".REPLACE.TXT")) g_scan.replace[key.substr(0, key.size() - 12) + ".TXT"].push_back(p);
        else if (EndsWith(key, ".TXT")) g_scan.add[key].push_back(p);
        else if (EndsWith(key, ".STRINGS")) g_scan.strings.push_back(p);
        else if (EndsWith(key, ".WEAPON")) g_scan.weapons.push_back(p);
        else if (EndsWith(key, ".VEHICLE")) g_scan.vehicles.push_back(p);
        else if (EndsWith(key, ".UNIT")) g_scan.units.push_back(p);
    }
    for (const std::string& d : dirs) ScanFolder(dir + "\\" + d, mod);
}

std::vector<std::pair<std::string, std::string>> KeyValues(const std::string& text) {
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
    return kv;
}

// ---- table rows (every TGE table is comma-separated) ----
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
std::vector<std::string> Fields(const std::string& line) {
    std::vector<std::string> col;
    for (size_t a = 0;;) {
        const size_t e = line.find(',', a);
        col.push_back(Trim(line.substr(a, e == std::string::npos ? std::string::npos : e - a)));
        if (e == std::string::npos) break;
        a = e + 1;
    }
    return col;
}
std::string Join(const std::vector<std::string>& row) {
    std::string out;
    for (size_t i = 0; i < row.size(); ++i) out += (i ? "," : "") + row[i];
    return out;
}
// First column (up to ','), trimmed, upper-case: the key replace parts match on.
std::string RowKey(const std::string& line) {
    std::string t = Trim(line);
    return Upper(Trim(t.substr(0, t.find(','))));
}
bool Blank(const std::string& line) {
    const std::string t = Trim(line);
    return t.empty() || t[0] == '#' || (t.size() > 1 && t[0] == '/' && t[1] == '/');
}

std::string PartText(const Part& p) {
    if (p.path.empty()) return p.text;
    std::string t;
    ReadAll(p.path, t);
    return t;
}

// <TABLE>.replace.txt: every game row with the same first column gives way to the mod's rows for that key (at the first
// old row's place), "-KEY" deletes the key, keys the table doesn't have are appended; a later part replaces a key whole.
int ApplyReplacements(std::string& text, const std::vector<Part>& parts) {
    std::unordered_map<std::string, std::vector<std::string>> rows;
    std::vector<std::string> order;
    for (const Part& part : parts) {
        std::unordered_set<std::string> seenHere;
        for (const std::string& line : Lines(PartText(part))) {
            if (Blank(line)) continue;
            const bool remove = Trim(line)[0] == '-';
            const std::string key = RowKey(remove ? Trim(line).substr(1) : line);
            if (key.empty()) continue;
            if (!rows.count(key)) order.push_back(key);
            if (seenHere.insert(key).second) rows[key].clear();
            if (!remove) rows[key].push_back(line);
        }
    }
    if (rows.empty()) return 0;
    std::string out;
    std::unordered_set<std::string> done;
    for (const std::string& line : Lines(text)) {
        auto it = rows.find(RowKey(line));
        if (it == rows.end() || Trim(line).empty()) {
            out += line + "\r\n";
            continue;
        }
        if (!done.insert(it->first).second) continue;
        for (const std::string& r : it->second) out += r + "\r\n";
    }
    for (const std::string& key : order)
        if (!done.count(key))
            for (const std::string& r : rows[key]) out += r + "\r\n";
    text.swap(out);
    return static_cast<int>(rows.size());
}
void AppendRows(std::string& text, const std::string& rows) {
    if (rows.empty()) return;
    if (!text.empty() && text.back() != '\n') text += "\r\n";
    text += rows;
    if (text.back() != '\n') text += "\r\n";
}

// ---- texts (.strings, display names) ----
std::deque<std::wstring> g_textPool;   // never freed: the game may keep a pointer it was handed
std::unordered_map<uint32_t, const wchar_t*> g_texts;
std::unordered_map<std::string, std::unordered_map<uint32_t, const wchar_t*>> g_levelTexts;   // "LEVELA" / "L01"

const wchar_t* Keep(const std::string& utf8) {
    std::wstring w;
    for (UINT cp : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_ACP)}) {
        const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
        const int n = MultiByteToWideChar(cp, flags, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
        if (n <= 0 && !utf8.empty()) continue;
        w.resize(n);
        MultiByteToWideChar(cp, flags, utf8.data(), static_cast<int>(utf8.size()), w.data(), n);
        break;
    }
    g_textPool.push_back(std::move(w));
    return g_textPool.back().c_str();
}

uint32_t TextKey(const std::string& key) {   // "0xA08F3D1B" = that hash, else the hash of the key
    if (key.size() == 10 && key[0] == '0' && (key[1] == 'x' || key[1] == 'X')) {
        char* end = nullptr;
        const unsigned long v = strtoul(key.c_str() + 2, &end, 16);
        if (end && !*end) return static_cast<uint32_t>(v);
    }
    return LowerHash(key);
}

void LoadStrings(const Part& p) {
    std::string text = PartText(p);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    int n = 0;
    std::string section;
    for (const std::string& raw : Lines(text)) {
        const std::string line = Trim(raw);
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '[') {
            const size_t end = line.find(']');
            section = Upper(Trim(line.substr(1, end == std::string::npos ? std::string::npos : end - 1)));
            if (section == "ALL") section.clear();
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = Trim(line.substr(0, eq));
        std::string value = Trim(line.substr(eq + 1));
        for (size_t k; (k = value.find("\\n")) != std::string::npos;) value.replace(k, 2, "\n");
        if (key.empty()) continue;
        (section.empty() ? g_texts : g_levelTexts[section])[TextKey(key)] = Keep(value);
        ++n;
    }
    features::ModLog("Mods: %d text(s) from %s", n, p.label.c_str());
}

// The running level's section names: "LEVELA" / "LEVEL01" (the mission DLL's name) and the short form "L01" / "LA".
std::vector<std::string> g_levelKeys;
int PSE_CALL OnLevelLoad(PSEEventId, void* data, void*) {
    g_levelKeys.clear();
    std::string u = Upper(data ? static_cast<const char*>(data) : "");
    if (!EndsWith(u, ".DLL")) return 0;
    u.resize(u.size() - 4);
    g_levelKeys.push_back(u);
    if (u.compare(0, 5, "LEVEL") == 0 && u.size() > 5) g_levelKeys.push_back("L" + u.substr(5));
    return 0;
}

// ---- models (EVO: the EOBJ / TEXL layout DS1 and DS2 share) ----
bool ModelBytes(const std::string& model, std::string& evo) {
    auto it = g_modFiles.find(Upper(model + ".evo"));
    if (it != g_modFiles.end()) return ReadAll(it->second, evo);
    return ReadGameFile(model + ".evo", evo);
}
bool ModelExists(const std::string& model) {
    return g_modFiles.count(Upper(model + ".evo")) || GameHas(model + ".evo") || features::ModFilePath((model + ".evo").c_str());
}
bool TextureExists(const std::string& tex) {
    for (const char* ext : {".dds", ".tga", ".png", ".bmp"})
        if (g_modFiles.count(Upper(tex + ext)) || GameHas(tex + ext)) return true;
    return false;
}

std::string g_mergedRoot;
std::string MergedDir(const std::string& sub) {
    const std::string dir = g_mergedRoot + "\\" + sub;
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

// A file built here becomes part of the mods index (mods.cpp), in a folder named by its content.
void Serve(const std::string& name, const std::string& bytes, int mod) {
    char sub[16];
    snprintf(sub, sizeof sub, "%08X", Fnv(bytes));
    const std::string path = MergedDir(sub) + "\\" + name;
    if (!WriteAll(path, bytes)) {
        features::ModLog("[fail] Mods: can't write %s", path.c_str());
        return;
    }
    features::SetModFile(name, path, mod);
}

// A retextured copy of a model: <newName>.evo = <model>.evo with the model name in every object header (EOBJ +8,
// char[64]) and the chosen TEXL slots (u32 n + n x {u32 hash, char[64] name}) replaced; the texture hash is also in
// every geometry part that uses it, so every copy of the old hash is swapped. "texture = X" = the slot named like the
// model (else the first), "texture <slot> = X" that slot.
bool CopyModel(const std::string& model, const std::string& newName,
               const std::vector<std::pair<std::string, std::string>>& retexture, const std::string& label, int mod) {
    std::string evo;
    if (newName.size() > 63 || !ModelBytes(model, evo)) {
        features::ModLog("[fail] Mods: %s - model %s not found", label.c_str(), model.c_str());
        return false;
    }
    int objects = 0, slots = 0;
    std::vector<std::pair<uint32_t, uint32_t>> swaps;
    for (size_t p = evo.find("EOBJ"); p != std::string::npos; p = evo.find("EOBJ", p + 4)) {
        if (p + 8 + 64 > evo.size() || _strnicmp(evo.c_str() + p + 8, model.c_str(), 64) != 0) continue;
        memset(&evo[p + 8], 0, 64);
        memcpy(&evo[p + 8], newName.c_str(), newName.size());
        ++objects;
    }
    auto texl = [&](auto&& each) {
        for (size_t p = evo.find("TEXL"); p != std::string::npos; p = evo.find("TEXL", p + 4)) {
            uint32_t n = 0;
            if (p + 8 > evo.size()) break;
            memcpy(&n, evo.data() + p + 4, 4);
            if (n == 0 || n > 64 || p + 8 + n * 68 > evo.size()) continue;
            for (uint32_t i = 0; i < n; ++i) each(p + 8 + i * 68);
        }
    };
    std::string mainTex;
    texl([&](size_t e) {
        const std::string name(evo.c_str() + e + 4, strnlen(evo.c_str() + e + 4, 64));
        if (_stricmp(name.c_str(), model.c_str()) == 0 || mainTex.empty()) mainTex = name;
    });
    for (const auto& [slot, tex] : retexture) {
        if (tex.size() > 63) continue;
        const std::string want = slot.empty() ? KeyOf(mainTex) : slot;
        texl([&](size_t e) {
            char* name = &evo[e + 4];
            if (KeyOf(std::string(name, strnlen(name, 64))) != want) return;
            memset(name, 0, 64);
            memcpy(name, tex.c_str(), tex.size());
            uint32_t oldHash;
            memcpy(&oldHash, &evo[e], 4);
            swaps.push_back({oldHash, LowerHash(tex)});
            ++slots;
        });
    }
    for (const auto& [from, to] : swaps)
        for (size_t q = 0; q + 4 <= evo.size(); ++q)
            if (!memcmp(&evo[q], &from, 4)) memcpy(&evo[q], &to, 4);
    if (!objects) {
        features::ModLog("[fail] Mods: %s - %s.evo has no object named %s", label.c_str(), model.c_str(), model.c_str());
        return false;
    }
    if (!slots) features::ModLog("[fail] Mods: %s - no texture slot of %s matched", label.c_str(), model.c_str());
    for (const auto& [slot, tex] : retexture)
        if (!TextureExists(tex))
            features::ModLog("[fail] Mods: %s - texture %s isn't in a mod or the game (%s.dds / .tga)", label.c_str(),
                             tex.c_str(), tex.c_str());
    Serve(newName + ".evo", evo, mod);
    features::ModLog("Mods: model %s = %s with %d texture slot(s) changed", newName.c_str(), model.c_str(), slots);
    return true;
}

// ---- generated table parts ----
std::map<std::string, std::vector<Part>> g_genAdd, g_genReplace;   // from .weapon / .vehicle / .unit
void AddGenerated(std::map<std::string, std::vector<Part>>& to, const char* table, const std::string& rows, int mod,
                  const char* what) {
    if (rows.empty()) return;
    to[table].push_back(Part{{}, rows, mod, what});
}

// The merged text of a table: game rows, replace parts (mod files, then generated), then appended parts.
std::string TableText(const std::string& key, bool* game = nullptr, int* replaced = nullptr) {
    std::string text;
    const bool has = ReadGameFile(key, text);
    if (game) *game = has;
    std::vector<Part> rep;
    if (auto it = g_scan.replace.find(key); it != g_scan.replace.end()) rep = it->second;
    if (auto it = g_genReplace.find(key); it != g_genReplace.end()) rep.insert(rep.end(), it->second.begin(), it->second.end());
    const int n = rep.empty() ? 0 : ApplyReplacements(text, rep);
    if (replaced) *replaced = n;
    for (auto* m : {&g_scan.add, &g_genAdd})
        if (auto it = m->find(key); it != m->end())
            for (const Part& p : it->second) AppendRows(text, PartText(p));
    return text;
}
std::vector<std::vector<std::string>> TableRows(const std::string& key) {
    std::vector<std::vector<std::string>> rows;
    for (const std::string& line : Lines(TableText(key)))
        if (!Blank(line)) rows.push_back(Fields(line));
    return rows;
}
const std::vector<std::string>* FindRow(const std::vector<std::vector<std::string>>& rows, const std::string& key) {
    for (const auto& r : rows)
        if (!r.empty() && _stricmp(r[0].c_str(), key.c_str()) == 0) return &r;
    return nullptr;
}
int LastMod(const std::string& key) {
    int mod = 0;
    for (auto* m : {&g_scan.add, &g_scan.replace, &g_genAdd, &g_genReplace})
        if (auto it = m->find(key); it != m->end())
            for (const Part& p : it->second) mod = std::max(mod, p.mod);
    return mod;
}

// ---- .weapon ----
// Weaps.txt columns (0-based, TGE) with a name; anything else as "column N" (1-based).
struct Field {
    const char* key;
    int col;
};
const Field kWeaponFields[] = {
    {"model", 1}, {"type", 4}, {"category", 4}, {"connector", 5},
    {"timebetweenshots", 9}, {"fireinterval", 9}, {"magazines", 13}, {"magazinesize", 14}, {"magazine", 14},
    {"reloadtime", 15}, {"damage", 21}, {"range", 22},
    {"aimanimation", 52}, {"reloadanimation", 54}, {"kneelaimanimation", 56}, {"kneelreloadanimation", 58},
    {"proneaimanimation", 59}, {"pronereloadanimation", 61}, {"soundbank", 67}, {"hearingrange", 68},
};
constexpr int kColCategory = 4, kColMuzzleFlash = 34, kColSound = 67, kColHearing = 68, kWeapsColumns = 74;
constexpr const char* kSilencedDonor = "ALLIED_WELROD_MK1";   // the game's silenced pistol (hearing 5)

void BuildWeapons() {
    if (g_scan.weapons.empty()) return;
    std::vector<std::vector<std::string>> weapons = TableRows("WEAPS.TXT");
    std::string rows, replaceRows;
    int lastMod = 0;
    for (const Part& wf : g_scan.weapons) {
        const auto kv = KeyValues(PartText(wf));
        std::string id, base;
        for (auto& [k, v] : kv) {
            if (k == "id") id = v;
            if (k == "basedon") base = v;
        }
        // a game weapon by its own id without "based on": only its display name can change
        if (base.empty() && !id.empty() && FindRow(weapons, id)) {
            for (auto& [k, v] : kv)
                if (k == "displayname" || k == "name") g_texts[LowerHash(id)] = Keep(v);
                else if (k != "id")
                    features::ModLog("[fail] Mods: %s - \"%s\" ignored: %s is a game weapon - to change it in place add "
                                     "\"based on = %s\"", wf.label.c_str(), k.c_str(), id.c_str(), id.c_str());
            continue;
        }
        const std::vector<std::string>* baseRow = base.empty() ? nullptr : FindRow(weapons, base);
        if (id.empty() || id.size() > 35 || id.find_first_of(", \t") != std::string::npos || !baseRow) {
            features::ModLog("[fail] Mods: %s - needs \"id = ...\" (max 35 characters, no commas / spaces) and \"based on "
                             "= <weapon id>\" (\"%s\" not found)", wf.label.c_str(), base.c_str());
            continue;
        }
        const bool inPlace = _stricmp(base.c_str(), id.c_str()) == 0;
        if (!inPlace && FindRow(weapons, id)) {
            features::ModLog("[fail] Mods: %s - weapon %s exists already (based on = %s changes it in place)",
                             wf.label.c_str(), id.c_str(), id.c_str());
            continue;
        }
        std::vector<std::string> row = *baseRow;
        row.resize(std::max<size_t>(row.size(), kWeapsColumns), "-1");
        row[0] = inPlace ? (*baseRow)[0] : id;
        auto soundOf = [&](const std::string& v) -> std::string {
            const std::vector<std::string>* r = FindRow(weapons, v);
            return r && r->size() > kColSound ? (*r)[kColSound] : v;
        };
        for (auto& [k, v] : kv)
            if (k == "silenced" && Yes(v)) {
                row[kColMuzzleFlash] = "-1";
                row[kColHearing] = "5.00";
                row[kColSound] = soundOf(kSilencedDonor);
            }
        std::vector<std::pair<std::string, std::string>> retexture;
        std::vector<std::string> shots;   // WAV files for the shot variations / the distant tail (mods_sound.cpp)
        std::string tail;
        const std::string baseSound = row[kColSound];
        for (auto& [k, v] : kv) {
            if (k == "id" || k == "basedon" || k == "silenced") continue;
            if (k == "displayname" || k == "name") {
                g_texts[LowerHash(id)] = Keep(v);
            } else if (k.compare(0, 7, "texture") == 0) {
                retexture.push_back({k.substr(7), v});
            } else if (k == "hudicon" || k == "icon" || k == "picture") {
                // a picture file in a mod (weaponicons.cpp: inventory, held-item HUD), else a game picture name
                std::string path;
                for (const char* ext : {"", ".png", ".dds", ".tga"})
                    if (path.empty())
                        if (const char* p = features::ModFilePath((v + ext).c_str())) path = p;
                const int n = path.empty() ? -1 : features::AddWeaponIcon(path, (*baseRow)[3], wf.label);
                if (n >= 0) row[3] = "TGEFIX_ICON_" + std::to_string(n);
                else if (path.empty() && features::WeaponPictureKnown(v)) row[3] = v;
                else
                    features::ModLog("[fail] Mods: %s - hud icon \"%s\": %s - %s's picture is used", wf.label.c_str(),
                                     v.c_str(), path.empty() ? "no such picture file (.png / .dds / .tga) in a mod or game "
                                     "picture name (IMAGE_INV_...)" : "weapon pictures aren't hooked", base.c_str());
            } else if (k == "muzzleflash") {
                row[kColMuzzleFlash] = Yes(v) ? "MuzzleFlash" : (KeyOf(v) == "no" || KeyOf(v) == "off" ? "-1" : v);
            } else if (k == "sound") {
                row[kColSound] = soundOf(v);
            } else if (k == "shotsound" || k == "shotsound1" || k == "soundfile") {
                shots.insert(shots.begin(), v);
            } else if (k == "shotsound2") {
                shots.push_back(v);
            } else if (k == "tailsound") {
                tail = v;
            } else if (k.compare(0, 6, "column") == 0 && atoi(k.c_str() + 6) >= 2 && atoi(k.c_str() + 6) <= kWeapsColumns) {
                row[atoi(k.c_str() + 6) - 1] = v;
            } else {
                bool known = false;
                for (const Field& f : kWeaponFields)
                    if (k == f.key) row[f.col] = v, known = true;
                if (!known) features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", wf.label.c_str(), k.c_str());
            }
        }
        if (!inPlace) {   // the game allows one weapon of these categories ("More than one ... has been loaded!")
            const std::string cat = Upper(Trim(row[kColCategory]));
            if (cat == "GRENADE LAUNCHER" || cat == "GRENADE LAUNCH/RIFLE" || cat == "FIST") {
                features::ModLog("[fail] Mods: %s - the game allows only one weapon of type \"%s\" (%s): %s left out",
                                 wf.label.c_str(), row[kColCategory].c_str(), base.c_str(), id.c_str());
                continue;
            }
        }
        if (!retexture.empty()) {
            const std::string copy = inPlace ? Upper(id) + "_MOD" : id;
            if (CopyModel(row[1], copy, retexture, wf.label, wf.mod)) row[1] = copy;
        } else if (!ModelExists(row[1])) {
            features::ModLog("[fail] Mods: %s - model %s isn't in the game or a mod (%s.evo)", wf.label.c_str(),
                             row[1].c_str(), row[1].c_str());
        }
        if (!shots.empty() || !tail.empty()) {   // own shot sounds: a new bank named like the weapon, from its current one
            const std::string dir = wf.path.substr(0, wf.path.find_last_of("\\/"));
            const std::string bank = inPlace ? Upper(id) + "_MOD" : id;
            if (features::MakeWeaponSoundBank(row[kColSound], bank, dir, shots, tail, wf.label)) row[kColSound] = bank;
        } else if (row[kColSound] != "-1" && _stricmp(row[kColSound].c_str(), baseSound.c_str()) != 0) {
            features::BorrowSoundBank(row[kColSound], wf.label);   // another weapon's sound: its bank into every level
        }
        lastMod = std::max(lastMod, wf.mod);
        if (inPlace) {
            replaceRows += Join(row) + "\r\n";
            for (auto& r : weapons)
                if (!r.empty() && _stricmp(r[0].c_str(), id.c_str()) == 0) r = row;
            features::ModLog("Mods: game weapon %s changed (%s)", row[0].c_str(), wf.label.c_str());
        } else {
            rows += Join(row) + "\r\n";
            weapons.push_back(row);
            features::ModLog("Mods: weapon %s (%s) based on %s, model %s, sound bank %s", id.c_str(), wf.label.c_str(),
                             base.c_str(), row[1].c_str(), row[kColSound].c_str());
        }
    }
    AddGenerated(g_genAdd, "WEAPS.TXT", rows, lastMod, ".weapon files");
    AddGenerated(g_genReplace, "WEAPS.TXT", replaceRows, lastMod, ".weapon files");
}

// ---- .vehicle ----
// id, based on (a Vehicle.txt row), texture[ <slot>], damaged texture[ <slot>], model, damaged model, name. A new id =
// a copy of the base row (+ its VehWpns rows); based on = its own id = the game vehicle changed in place.
void BuildVehicles() {
    if (g_scan.vehicles.empty()) return;
    std::vector<std::vector<std::string>> vehicles = TableRows("VEHICLE.TXT"), guns = TableRows("VEHWPNS.TXT");
    std::string rows, replaceRows, gunRows;
    int lastMod = 0;
    for (const Part& vf : g_scan.vehicles) {
        std::string id, base;
        const auto kv = KeyValues(PartText(vf));
        for (auto& [k, v] : kv) {
            if (k == "id") id = v;
            if (k == "basedon") base = v;
        }
        const std::vector<std::string>* baseRow = base.empty() ? nullptr : FindRow(vehicles, base);
        if (id.empty() || id.size() > 35 || id.find_first_of(", \t") != std::string::npos || !baseRow || baseRow->size() < 3) {
            features::ModLog("[fail] Mods: %s - needs \"id = ...\" (max 35 characters, no commas / spaces) and \"based on "
                             "= <vehicle>\" (a Vehicle.txt row; \"%s\" not found)", vf.label.c_str(), base.c_str());
            continue;
        }
        const bool inPlace = _stricmp(base.c_str(), id.c_str()) == 0;
        if (!inPlace && FindRow(vehicles, id)) {
            features::ModLog("[fail] Mods: %s - vehicle %s exists already", vf.label.c_str(), id.c_str());
            continue;
        }
        std::vector<std::string> row = *baseRow;
        row[0] = inPlace ? (*baseRow)[0] : id;
        std::vector<std::pair<std::string, std::string>> retexture, damaged;
        for (auto& [k, v] : kv) {
            if (k == "id" || k == "basedon") continue;
            if (k == "name" || k == "displayname") g_texts[LowerHash(id)] = Keep(v);
            else if (k.compare(0, 14, "damagedtexture") == 0) damaged.push_back({k.substr(14), v});
            else if (k.compare(0, 7, "texture") == 0) retexture.push_back({k.substr(7), v});
            else if (k == "model") row[1] = v;
            else if (k == "damagedmodel") row[2] = v;
            else features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", vf.label.c_str(), k.c_str());
        }
        // In place: the vehicle's model file itself is replaced by the retextured copy (same name) - a level's scene
        // names the model in its own record, so a new model name in Vehicle.txt wouldn't reach placed vehicles. New ids
        // get <ID>.evo (+ <ID>.DYN).
        const std::string stem = Upper(id);
        if (!retexture.empty() && row[1] != "-1") {
            const std::string model = row[1];
            if (CopyModel(model, inPlace ? model : stem, retexture, vf.label, vf.mod) && !inPlace) {
                row[1] = stem;
                std::string dyn;   // tracks / wheels: <model>.DYN, copied under the new name
                const auto modDyn = g_modFiles.find(Upper(model + ".DYN"));
                if (ReadGameFile(model + ".dyn", dyn) || (modDyn != g_modFiles.end() && ReadAll(modDyn->second, dyn)))
                    Serve(stem + ".DYN", dyn, vf.mod);
            }
        }
        if (!damaged.empty() && row[2] != "-1" &&
            CopyModel(row[2], inPlace ? row[2] : stem + "_WRECK", damaged, vf.label, vf.mod) && !inPlace)
            row[2] = stem + "_WRECK";
        for (int c : {1, 2})
            if (row[c] != "-1" && !ModelExists(row[c]))
                features::ModLog("[fail] Mods: %s - model %s isn't in the game or a mod", vf.label.c_str(), row[c].c_str());
        lastMod = std::max(lastMod, vf.mod);
        if (inPlace) {
            replaceRows += Join(row) + "\r\n";
            features::ModLog("Mods: game vehicle %s changed (%s)", row[0].c_str(), vf.label.c_str());
            continue;
        }
        int n = 0;
        for (const auto& g : guns)
            if (g.size() >= 4 && _stricmp(g[0].c_str(), base.c_str()) == 0) {
                std::vector<std::string> copy = g;
                copy[0] = id;
                gunRows += Join(copy) + "\r\n";
                ++n;
            }
        rows += Join(row) + "\r\n";
        vehicles.push_back(row);
        features::ModLog("Mods: vehicle %s (%s) based on %s, model %s, %d gun(s)", id.c_str(), vf.label.c_str(),
                         base.c_str(), row[1].c_str(), n);
    }
    AddGenerated(g_genAdd, "VEHICLE.TXT", rows, lastMod, ".vehicle files");
    AddGenerated(g_genReplace, "VEHICLE.TXT", replaceRows, lastMod, ".vehicle files");
    AddGenerated(g_genAdd, "VEHWPNS.TXT", gunRows, lastMod, ".vehicle files");
}

// ---- .unit ----
// id, based on (a Units.txt row), type, model (body), second model, health (own HStat row), experience, voice (speech
// bank), face (a Faces.txt row), item = <weapon / inventory item>[, amount] (any number; "none" = no items), column N /
// stat column N. based on = its own id = the game unit changed in place (its HStat row is copied when health / stat
// columns change - other units can share it).
const char* const kUnitTypes[] = {"SOLDIER", "GOAT HERDER", "CIVILIAN", "EMIR"};
const char* const kExperience[] = {"Rookie", "Green", "Trained", "Regular", "Veteran", "Expert", "Professional", "Guard",
                                   "Crack", "Elite", "Specialist", "Skill One", "Skill Two", "Skill Three", "Skill Four"};
const char* const kItemTypes[] = {
    "HUMAN", "MACHINE", "BUILDING", "FEATURE", "WEAPON", "UNIFORM", "PHRASE BOOK", "MEDI KIT", "C4 DETONATOR", "RADIO",
    "BODY ARMOUR", "HEAD ARMOUR", "AMMO", "DOOR", "BINOCULARS", "NIGHT VISION GOGGLES", "LASER DESIGNATOR", "KEY",
    "TARGET", "MISCELLANEOUS", "ANIMAL", "BRIBE", "LIGHT", "ID PAPERS", "LOCKPICK", "BIGOBJECT", "FOOD", "BIGPARCEL"};
template <size_t N>
const char* OneOf(const char* const (&list)[N], const std::string& v, bool caseless = true) {
    for (const char* s : list)
        if (caseless ? _stricmp(s, v.c_str()) == 0 : strcmp(s, v.c_str()) == 0) return s;
    return nullptr;
}
constexpr int kUnitColumns = 15, kColStats = 4, kColExperience = 9, kColVoice = 10, kColSlots = 11, kColFace = 12;

void BuildUnits() {
    if (g_scan.units.empty()) return;
    std::vector<std::vector<std::string>> units = TableRows("UNITS.TXT"), stats = TableRows("HSTAT.TXT"),
                                          kits = TableRows("UNITWPNS.TXT"), weapons = TableRows("WEAPS.TXT"),
                                          inventory = TableRows("INVEN.TXT"), faces = TableRows("FACES.TXT");
    std::string unitRows, unitReplace, statRows, kitRows, kitReplace;
    int lastMod = 0;
    for (const Part& uf : g_scan.units) {
        std::string id, base, type, model, model2, experience, voice, face;
        int health = -1;
        bool ownKit = false, ok = true;
        std::vector<std::pair<std::string, std::string>> items;
        std::vector<std::pair<int, std::string>> columns, statColumns;
        for (auto& [k, v] : KeyValues(PartText(uf))) {
            if (k == "id") id = v;
            else if (k == "basedon") base = v;
            else if (k == "type" || k == "kind") type = v;
            else if (k == "model" || k == "body") model = v;
            else if (k == "secondmodel" || k == "model2") model2 = v;
            else if (k == "health") health = atoi(v.c_str());
            else if (k == "experience") experience = v;
            else if (k == "voice" || k == "speechbank") voice = v;
            else if (k == "face") face = v;
            else if (k == "item") {
                ownKit = true;
                if (_stricmp(v.c_str(), "none") == 0) continue;
                const size_t comma = v.find(',');
                items.push_back({Trim(v.substr(0, comma)), comma == std::string::npos ? "0" : Trim(v.substr(comma + 1))});
            } else if (k.compare(0, 10, "statcolumn") == 0 && atoi(k.c_str() + 10) > 1) {
                statColumns.push_back({atoi(k.c_str() + 10), v});
            } else if (k.compare(0, 6, "column") == 0 && atoi(k.c_str() + 6) > 1) {
                columns.push_back({atoi(k.c_str() + 6), v});
            } else {
                features::ModLog("[fail] Mods: %s - unknown setting \"%s\"", uf.label.c_str(), k.c_str());
            }
        }
        const std::vector<std::string>* found = base.empty() ? nullptr : FindRow(units, base);
        if (id.empty() || id.size() > 35 || id.find_first_of(", \t") != std::string::npos || !found ||
            found->size() < kUnitColumns) {
            features::ModLog("[fail] Mods: %s - needs \"id = ...\" (max 35 characters, no commas / spaces) and \"based on "
                             "= <unit>\" (a Units.txt row; \"%s\" not found)", uf.label.c_str(), base.c_str());
            continue;
        }
        const bool inPlace = _stricmp(base.c_str(), id.c_str()) == 0;
        if (!inPlace && FindRow(units, id)) {
            features::ModLog("[fail] Mods: %s - unit %s exists already (based on = %s changes it in place)", uf.label.c_str(),
                             id.c_str(), id.c_str());
            continue;
        }
        const std::vector<std::string> baseRow = *found;
        std::vector<std::string> row = baseRow;
        row[0] = inPlace ? baseRow[0] : id;
        if (!type.empty()) {
            const char* t = OneOf(kUnitTypes, type);
            if (!t) {
                features::ModLog("[fail] Mods: %s - type \"%s\" isn't SOLDIER, GOAT HERDER, CIVILIAN or EMIR",
                                 uf.label.c_str(), type.c_str());
                continue;
            }
            row[1] = t;
        }
        if (!model.empty()) row[2] = model;
        if (!model2.empty()) row[3] = model2;
        for (int c : {2, 3})
            if (row[c] != "-1" && !ModelExists(row[c])) {
                features::ModLog("[fail] Mods: %s - model %s.evo isn't in the game or a mod", uf.label.c_str(), row[c].c_str());
                ok = false;
            }
        if (!ok) continue;
        if (!experience.empty()) {
            const char* e = OneOf(kExperience, experience);
            if (!e) {
                features::ModLog("[fail] Mods: %s - experience \"%s\": Rookie, Green, Trained, Regular, Veteran, Expert, "
                                 "Professional, Guard, Crack, Elite or Specialist", uf.label.c_str(), experience.c_str());
                continue;
            }
            row[kColExperience] = e;
        }
        if (!voice.empty()) row[kColVoice] = voice;
        if (!face.empty()) {
            const std::vector<std::string>* f = FindRow(faces, face);
            if (!f) {
                features::ModLog("[fail] Mods: %s - face \"%s\" isn't a Faces.txt row", uf.label.c_str(), face.c_str());
                continue;
            }
            row[kColFace] = (*f)[0];
        }
        if (health >= 0 || !statColumns.empty()) {   // own base stats, named like the unit
            const std::vector<std::string>* baseStats = FindRow(stats, baseRow[kColStats]);
            if (!baseStats) {
                features::ModLog("[fail] Mods: %s - base stats %s not found", uf.label.c_str(), baseRow[kColStats].c_str());
                continue;
            }
            std::vector<std::string> s = *baseStats;
            const std::string statName = inPlace ? Upper(id) + "_MOD" : id;
            if (statName.size() > 35) {
                features::ModLog("[fail] Mods: %s - stat row name %s longer than 35 characters", uf.label.c_str(),
                                 statName.c_str());
                continue;
            }
            s[0] = statName;
            if (health >= 0) s[1] = std::to_string(health);
            for (auto& [n, v] : statColumns)
                if (n <= static_cast<int>(s.size())) s[n - 1] = v;
                else ok = false;
            if (!ok) {
                features::ModLog("[fail] Mods: %s - HStat.txt has %zu columns", uf.label.c_str(), s.size());
                continue;
            }
            if (!FindRow(stats, statName)) {
                statRows += Join(s) + "\r\n";
                stats.push_back(s);
            }
            row[kColStats] = statName;
        }
        for (auto& [n, v] : columns)
            if (n <= static_cast<int>(row.size()) && n != kColSlots + 1) row[n - 1] = v;
            else ok = false;
        if (!ok) {
            features::ModLog("[fail] Mods: %s - Units.txt has 15 columns; column 12 (item slots) comes from the items",
                             uf.label.c_str());
            continue;
        }
        std::vector<std::vector<std::string>> kit;
        if (ownKit) {
            for (auto& [item, amount] : items) {
                const std::vector<std::string>* w = FindRow(weapons, item);
                const std::vector<std::string>* inv = w ? nullptr : FindRow(inventory, item);
                if (!w && (!inv || inv->size() < 2)) {
                    features::ModLog("[fail] Mods: %s - item %s isn't a weapon (Weaps.txt) or an inventory item (Inven.txt)",
                                     uf.label.c_str(), item.c_str());
                    ok = false;
                    break;
                }
                kit.push_back({row[0], w ? (*w)[0] : (*inv)[0], w ? "WEAPON" : (*inv)[1], amount});
            }
            if (!ok) continue;
        } else if (!inPlace) {
            for (const auto& k : kits)
                if (k.size() >= 4 && _stricmp(k[0].c_str(), baseRow[0].c_str()) == 0) kit.push_back({id, k[1], k[2], k[3]});
        }
        lastMod = std::max(lastMod, uf.mod);
        if (inPlace) {
            if (ownKit) {   // the unit's kit replaced: every UnitWpns row of it goes, the new ones come in
                kitReplace += "-" + row[0] + "\r\n";
                for (const auto& k : kit) kitRows += Join(k) + "\r\n";
            }
            unitReplace += Join(row) + "\r\n";   // item slots are set by the table check
            features::ModLog("Mods: game unit %s changed (%s)%s", row[0].c_str(), uf.label.c_str(), ownKit ? ", new kit" : "");
            continue;
        }
        for (const auto& k : kit) kitRows += Join(k) + "\r\n";
        unitRows += Join(row) + "\r\n";
        units.push_back(row);
        features::ModLog("Mods: unit %s based on %s, type %s, model %s, %zu item(s)", id.c_str(), baseRow[0].c_str(),
                         row[1].c_str(), row[2].c_str(), kit.size());
    }
    AddGenerated(g_genAdd, "UNITS.TXT", unitRows, lastMod, ".unit files");
    AddGenerated(g_genReplace, "UNITS.TXT", unitReplace, lastMod, ".unit files");
    AddGenerated(g_genAdd, "HSTAT.TXT", statRows, lastMod, ".unit files");
    AddGenerated(g_genReplace, "UNITWPNS.TXT", kitReplace, lastMod, ".unit files");
    AddGenerated(g_genAdd, "UNITWPNS.TXT", kitRows, lastMod, ".unit files");
}

// ---- table checks (rows the parsers would fail the load on are left out) ----
struct Table {
    std::string key;
    bool touched = false, changed = false;
    std::vector<std::string> lines;          // as merged (blank / comment lines dropped when written)
    std::unordered_set<std::string> game;    // the game's own rows (trimmed): only reference checks for those
    bool Mod(const std::string& line) const { return !game.count(Trim(line)); }
    std::string Text() const {
        std::string t;
        const bool rules = EndsWith(key, "STATERULES.TXT");   // read by a tokenizer with comments of its own
        for (const std::string& l : lines)
            if (rules || !Blank(l)) t += l + "\r\n";
        return t;
    }
};

void CheckTables(std::map<std::string, Table>& tables) {
    // The game compares names exactly (strcmp): the checks do too.
    auto names = [&](const Table& t) {
        std::unordered_set<std::string> out;
        for (const std::string& l : t.lines)
            if (!Blank(l)) out.insert(Trim(Fields(l)[0]));
        return out;
    };
    auto drop = [&](Table& t, size_t i, const std::string& why) {
        features::ModLog("[fail] Mods: %s row \"%s\" left out - %s", t.key.c_str(), Trim(t.lines[i]).substr(0, 80).c_str(),
                         why.c_str());
        t.lines[i] = "";
        t.changed = true;
    };
    Table& weaps = tables["WEAPS.TXT"];
    Table& inven = tables["INVEN.TXT"];
    Table& hstat = tables["HSTAT.TXT"];
    Table& units = tables["UNITS.TXT"];
    Table& kits = tables["UNITWPNS.TXT"];
    Table& vehicles = tables["VEHICLE.TXT"];
    Table& vstat = tables["VSTAT.TXT"];
    Table& guns = tables["VEHWPNS.TXT"];
    // Weaps.txt: id length, column count, one weapon per singleton category, magazine size
    if (weaps.touched) {
        std::unordered_set<std::string> singles;
        for (size_t i = 0; i < weaps.lines.size(); ++i) {
            if (Blank(weaps.lines[i])) continue;
            const auto c = Fields(weaps.lines[i]);
            if (!weaps.Mod(weaps.lines[i])) {
                if (c.size() > kColCategory) singles.insert(Upper(c[kColCategory]));
                continue;
            }
            if (c[0].size() > 35) drop(weaps, i, "the weapon id is longer than 35 characters");
            else if (c.size() < kWeapsColumns) drop(weaps, i, "Weaps.txt rows have 74 columns, this one " + std::to_string(c.size()));
            else if (c[1].size() > 35) drop(weaps, i, "the model name is longer than 35 characters");
            else if (atoi(c[14].c_str()) > 999) drop(weaps, i, "a magazine holds at most 999 rounds");
            else {
                const std::string cat = Upper(c[kColCategory]);
                if ((cat == "GRENADE LAUNCHER" || cat == "GRENADE LAUNCH/RIFLE" || cat == "FIST") && !singles.insert(cat).second)
                    drop(weaps, i, "the game allows only one weapon of type \"" + c[kColCategory] + "\"");
            }
        }
    }
    for (size_t i = 0; hstat.touched && i < hstat.lines.size(); ++i)
        if (!Blank(hstat.lines[i]) && hstat.Mod(hstat.lines[i]) && Fields(hstat.lines[i])[0].size() > 35)
            drop(hstat, i, "the stat name is longer than 35 characters");
    // Units.txt: lengths, base stats, experience
    const bool unitCheck = units.touched || hstat.touched || kits.touched || weaps.touched || inven.touched;
    if (unitCheck) {
        const auto statNames = names(hstat);
        for (size_t i = 0; i < units.lines.size(); ++i) {
            if (Blank(units.lines[i])) continue;
            const auto c = Fields(units.lines[i]);
            std::string why;
            const bool mod = units.Mod(units.lines[i]);
            if (!mod) {
                if (c.size() > kColStats && !statNames.count(c[kColStats]))
                    why = "base stats \"" + c[kColStats] + "\" aren't an HStat.txt row";
            } else if (c.size() < kUnitColumns) why = "Units.txt rows have 15 columns";
            else if (c[0].size() > 35 || c[2].size() > 35 || c[3].size() > 35 || c[kColVoice].size() > 35)
                why = "a name is longer than 35 characters";
            else if (!statNames.count(c[kColStats])) why = "base stats \"" + c[kColStats] + "\" aren't an HStat.txt row";
            else if (!OneOf(kExperience, c[kColExperience], false)) why = "unknown experience \"" + c[kColExperience] + "\"";
            if (!why.empty()) drop(units, i, why);
        }
        // UnitWpns.txt: unit, type, item
        const auto unitNames = names(units), weaponNames = names(weaps), invenNames = names(inven);
        std::unordered_map<std::string, int> count;
        for (size_t i = 0; i < kits.lines.size(); ++i) {
            if (Blank(kits.lines[i])) continue;
            const auto c = Fields(kits.lines[i]);
            std::string why;
            if (c.size() < 4) why = "UnitWpns.txt rows have 4 columns";
            else if (kits.Mod(kits.lines[i]) && (c[0].size() > 35 || c[1].size() > 35 || c[2].size() > 35))
                why = "a name is longer than 35 characters";
            else if (!unitNames.count(c[0])) why = "unit \"" + c[0] + "\" isn't a Units.txt row";
            else if (!OneOf(kItemTypes, c[2], false)) why = "unknown item type \"" + c[2] + "\"";
            else if (c[2] == "WEAPON" ? !weaponNames.count(c[1]) : !invenNames.count(c[1]))
                why = "item \"" + c[1] + "\" isn't a " + (c[2] == "WEAPON" ? "Weaps.txt" : "Inven.txt") + " row";
            if (!why.empty()) drop(kits, i, why);
            else ++count[c[0]];
        }
        // item slots (Units col 12) = the unit's UnitWpns rows: more rows = a failed load, fewer = empty slots
        for (std::string& line : units.lines) {
            if (Blank(line)) continue;
            auto c = Fields(line);
            if (c.size() < kUnitColumns) continue;
            const int want = count.count(c[0]) ? count[c[0]] : 0;
            if (atoi(c[kColSlots].c_str()) == want) continue;
            features::ModLog("Mods: Units.txt %s - item slots %s -> %d (its UnitWpns.txt rows)", c[0].c_str(),
                             c[kColSlots].c_str(), want);
            c[kColSlots] = std::to_string(want);
            line = Join(c);
            units.changed = true;
        }
    }
    // Vehicle.txt / VehWpns.txt: VStat row, vehicle + weapon known, gun slots (Vehicle col 23) = its VehWpns rows
    if (vehicles.touched || guns.touched || weaps.touched || vstat.touched) {
        const auto vstatNames = names(vstat);
        for (size_t i = 0; i < vehicles.lines.size(); ++i) {
            if (Blank(vehicles.lines[i]) || !vehicles.Mod(vehicles.lines[i])) continue;
            const auto c = Fields(vehicles.lines[i]);
            if (c.size() < 26) drop(vehicles, i, "Vehicle.txt rows have 26 columns");
            else if (!vstatNames.count(c[4])) drop(vehicles, i, "base stats \"" + c[4] + "\" aren't a VStat.txt row");
        }
        const auto vehicleNames = names(vehicles), weaponNames = names(weaps);
        std::unordered_map<std::string, int> count;
        for (size_t i = 0; i < guns.lines.size(); ++i) {
            if (Blank(guns.lines[i])) continue;
            const auto c = Fields(guns.lines[i]);
            std::string why;
            if (c.size() < 4) why = "VehWpns.txt rows have 4 columns";
            else if (!vehicleNames.count(c[0])) why = "vehicle \"" + c[0] + "\" isn't a Vehicle.txt row";
            else if (c[2] != "WEAPON" || !weaponNames.count(c[1])) why = "\"" + c[1] + "\" isn't a weapon";
            if (!why.empty()) drop(guns, i, why);
            else ++count[c[0]];
        }
        for (std::string& line : vehicles.lines) {
            if (Blank(line)) continue;
            auto c = Fields(line);
            if (c.size() < 26) continue;
            const int want = count.count(c[0]) ? count[c[0]] : 0;
            if (atoi(c[22].c_str()) == want) continue;
            features::ModLog("Mods: Vehicle.txt %s - gun slots %s -> %d (its VehWpns.txt rows)", c[0].c_str(), c[22].c_str(), want);
            c[22] = std::to_string(want);
            line = Join(c);
            vehicles.changed = true;
        }
    }
}

int g_tablesMerged = 0;

void MergeAll() {
    std::set<std::string> keys;
    for (auto* m : {&g_scan.add, &g_scan.replace, &g_genAdd, &g_genReplace})
        for (const auto& [k, parts] : *m) keys.insert(k);
    static const char* const kChecked[] = {"WEAPS.TXT", "INVEN.TXT", "HSTAT.TXT", "UNITS.TXT", "UNITWPNS.TXT",
                                           "VEHICLE.TXT", "VSTAT.TXT", "VEHWPNS.TXT"};
    std::map<std::string, Table> tables;
    auto gameRows = [](Table& t) {
        std::string text;
        ReadGameFile(t.key, text);
        for (const std::string& l : Lines(text)) t.game.insert(Trim(l));
        return text;
    };
    for (const std::string& key : keys) {
        bool game = false;
        int replaced = 0;
        const std::string text = TableText(key, &game, &replaced);
        size_t parts = 0;
        for (auto* m : {&g_scan.add, &g_genAdd})
            if (auto it = m->find(key); it != m->end()) parts += it->second.size();
        if (!game && parts <= 1 && !replaced && !g_genAdd.count(key)) continue;   // a mod's own new file: served as it is
        Table& t = tables[key];
        t.key = key;
        t.touched = t.changed = true;
        t.lines = Lines(text);
        gameRows(t);
        if (replaced) features::ModLog("Mods: %s - %d key(s) replaced", key.c_str(), replaced);
        features::ModLog("Mods: %s = %s + %zu mod part(s)", key.c_str(), game ? "game rows" : "no game file", parts);
    }
    bool anyChecked = false;
    for (const char* k : kChecked) anyChecked = anyChecked || tables.count(k);
    if (anyChecked)
        for (const char* k : kChecked)
            if (!tables.count(k)) {
                Table& t = tables[k];
                t.key = k;
                t.lines = Lines(gameRows(t));
            }
    CheckTables(tables);
    g_tablesMerged = 0;
    for (auto& [key, t] : tables) {
        if (!t.changed) continue;
        Serve(key, t.Text(), LastMod(key));
        ++g_tablesMerged;
    }
}

// ---- text lookup hook (chained with modsmenu.cpp's on the same entry) ----
constexpr uint32_t kTextLookup = 0x47EA90;
constexpr uint8_t kTextLookupPrologue[] = {0x8B, 0x41, 0x38, 0x85, 0xC0};   // mov eax, [ecx+0x38]; test eax, eax
using TextLookupFn = const wchar_t*(__thiscall*)(void*, uint32_t);
TextLookupFn g_textLookup = nullptr;
#ifndef DS_DIST
// Dev\TextLog = 1: every hash the game asks for, once, with its text (finding the keys a .strings file can change).
void NoteText(uint32_t hash, const wchar_t* text) {
    static const bool on = [] {
        DWORD v = 0, size = sizeof v;
        return RegGetValueA(HKEY_CURRENT_USER, "Software\\GreatEscapeFix\\Dev", "TextLog", RRF_RT_REG_DWORD, nullptr, &v,
                            &size) == ERROR_SUCCESS && v;
    }();
    static std::unordered_set<uint32_t> seen;
    if (!on || !seen.insert(hash).second) return;
    char utf8[256] = {};
    if (text) WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof utf8 - 1, nullptr, nullptr);
    dslog::Write("[dev] Text 0x%08X = \"%s\"", hash, utf8);
}
#else
void NoteText(uint32_t, const wchar_t*) {}
#endif

const wchar_t* __fastcall TextLookup(void* catalog, void*, uint32_t hash) {
    const wchar_t* t = features::ModText(hash);
    if (!t) t = g_textLookup(catalog, hash);
    NoteText(hash, t);
    return t;
}

bool g_built = false;
}  // namespace

// Mods folder state -> merged tables, texts, model copies. Called by mods.cpp after every index build (start-up and the
// MODS menu's Apply) - the files it writes go into that index.
void features::BuildModTables() {
    g_scan = Scan{};
    g_genAdd.clear();
    g_genReplace.clear();
    g_modFiles.clear();
    g_texts.clear();
    g_levelTexts.clear();
    g_modNames = LoadedMods();
    ScanModSounds();   // before the weapon files ask for banks
    const std::string root = GameDir() + "\\Mods";
    for (size_t i = 0; i < g_modNames.size(); ++i) ScanFolder(root + "\\" + g_modNames[i], static_cast<int>(i));
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    g_mergedRoot = std::string(tmp) + "GreatEscapeFix";
    CreateDirectoryA(g_mergedRoot.c_str(), nullptr);
    g_mergedRoot += "\\merged";
    CreateDirectoryA(g_mergedRoot.c_str(), nullptr);
    for (const Part& p : g_scan.strings) LoadStrings(p);
    BuildWeapons();
    BuildVehicles();
    BuildUnits();
    MergeAll();
    BuildModLevels();
    size_t level = 0;
    for (const auto& [k, m] : g_levelTexts) level += m.size();
    if (g_built || g_tablesMerged || !g_texts.empty() || level)
        features::ModLog("[ok]   Mods: %d table(s) merged, %zu text(s) (+ %zu for single levels), %zu .weapon, %zu .vehicle, "
                         "%zu .unit file(s)", g_tablesMerged, g_texts.size(), level, g_scan.weapons.size(),
                         g_scan.vehicles.size(), g_scan.units.size());
    g_built = true;
}

// A mod's text for this catalog hash, or null. Level sections win over [all] while their level runs.
const wchar_t* features::ModText(uint32_t hash) {
    if (!g_levelTexts.empty())
        for (const std::string& key : g_levelKeys) {
            auto lv = g_levelTexts.find(key);
            if (lv == g_levelTexts.end()) continue;
            auto it = lv->second.find(hash);
            if (it != lv->second.end()) return it->second;
        }
    if (g_texts.empty()) return nullptr;
    auto it = g_texts.find(hash);
    return it == g_texts.end() ? nullptr : it->second;
}

void features::ApplyModTables() {
    if (!Get().mods) return;
    ApplyModLevels();
    if (!patch::HookFunction(kTextLookup, reinterpret_cast<const void*>(&TextLookup), reinterpret_cast<void**>(&g_textLookup),
                             kTextLookupPrologue, sizeof kTextLookupPrologue))
        ModLog("[fail] Mods: text lookup at 0x%08X not recognised - .strings files aren't used", kTextLookup);
    if (const PSEMessagingInterface* m = pse::MessagingInterface())
        m->Subscribe(pse_host::Find("tgefix.modtables"), PSE_EVENT_LEVEL_LOAD_BEGIN, &OnLevelLoad, nullptr, 0);
}
