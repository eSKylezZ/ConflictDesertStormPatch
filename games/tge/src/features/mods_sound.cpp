// Mods folder - sounds (tgefix.modsound): sound bank packs, level sound replacement, music streams and WAV-made weapon
// sound banks. Port of DesertStorm2Fix's mods_sound.cpp (games/ds2/src/features/mods_sound.cpp; DS2 addresses in
// brackets); films are films.cpp.
//
// How GreatEscape.exe plays sounds (mapped 2026-10-25) - the same sound library as DS1 / DS2:
//  - sound caches (loose in the game folder): default.sch (front end, screens, films) and <level>.sch ("SCH\0", u32
//    size, chunks BANK {u32 key = ELF hash of "<NAME>.sbk"}, PFSM sample {i32 language, u32 id, u32 bytes, u16 rate,
//    u8 bits, u8 pad, data}, PFST stream, IMUS interactive music). Loaded by CSoundManager::SetSoundCache
//    FUN_0041f950 thiscall(manager [0x79EDC8], name) [FUN_004b5de0] from Level_Init (0x5D2072) and Screen_Load /
//    start-up ("default.sch"); it keeps samples of the manager's language (+0x3C) or -1 and EXITS THE GAME (" Cache file
//    ... seems to be corrupt") on a chunk it can't read - so every file we hand it is checked first. The file is opened
//    by FUN_0041f8e0 (thiscall(reader = manager +0x12A4, name)) through FUN_004c5220(name, 0) at 0x41F8ED -> resolver ->
//    FUN_004c6430(path, "rb") [0x4B5D6D -> FUN_004c3140]. That call is hooked: a cache mods change gets a merged copy in
//    %TEMP%\GreatEscapeFix\sound\<name> opened from disk (archive bits of [0x7ACDDC] cleared for the open). .SCH names
//    never go through mods.cpp's resolver (a mod's LEVEL01.SCH is a partial overlay, not a whole cache).
//  - banks by name: CSoundManager::LoadSoundBank FUN_0041db60 thiscall(name, flag) = ELF hash (case-sensitive) of
//    "<name>.sbk" among the loaded banks, else the file through the resolver.
//  - samples: 4-bit ADPCM with the PlayStation VAG filter table (0x623E60) -> mods_sound_adpcm.h. Gun banks (LUGER,
//    MG42, STEN, MP38, WEBLEY, MODEL10, MAS): 4 samples, click / tail / shot 1 / shot 2 as on DS1 / DS2 (assumed).
//  - streams: CWaveDataSourcePsfStream (vtable 0x5F60C0) ReadFromSoundCache FUN_004214d0 thiscall(reader, cache)
//    [0x4B7790]: path +0x18 (game allocator FUN_004cf8c0(size, 0x15), freed by FUN_004cf950), bytes per channel +0x14;
//    opened by fopen (slot 4, 0x421640) - no resolver. TGE's only PFST: data\sound\GreatEscapeTitle.psf.
//    CWaveDataSourceIMus (vtable 0x5F6014) ReadFromSoundCache FUN_00421040 [0x4B7370]: path +0x14, segment table +0x18
//    (12 bytes per segment); the level music (GEStealth01..03, GECombat01, GEChase01, GEDriving01, MissionFailure).
//    Both loads are wrapped (entry detours): a mod's <name>.psf (by file name) takes the path. Plain streams: the length
//    follows the mod's file. Interactive music: only a mod file of exactly the game file's size (same segment layout).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/mods_sound_adpcm.h"

namespace {
constexpr uint32_t kCacheOpenCall = 0x41F8ED, kOpenWrapper = 0x4C5220;   // in FUN_0041f8e0 (SetSoundCache's open)
constexpr uint32_t kFileOpen = 0x4C6430, kReadMode = 0x618AF0;           // FUN_004c6430(path, "rb")
constexpr uint32_t kFileFlags = 0x7ACDDC;                                // bit 0 archive mode, bit 1 archive build
constexpr uint32_t kStreamLoad = 0x4214D0;
constexpr uint8_t kStreamLoadEntry[] = {0x83, 0xEC, 0x0C, 0x56, 0x57};   // sub esp,0xC; push esi; push edi
constexpr uint32_t kMusicLoad = 0x421040;
constexpr uint8_t kMusicLoadEntry[] = {0x83, 0xEC, 0x08, 0x56, 0x57};    // sub esp,8; push esi; push edi
constexpr uint32_t kAlloc = 0x4CF8C0, kFree = 0x4CF950;                  // game allocator (size, tag) / free

constexpr uint32_t kTagBank = 0x4B4E4142, kTagSample = 0x4D534650, kTagStream = 0x54534650, kTagMusic = 0x53554D49;

using OpenWrapperFn = void*(__cdecl*)(const char*, int);
using FileOpenFn = void*(__cdecl*)(const char*, const char*);
using LoadFn = char(__thiscall*)(void*, void*);
LoadFn g_streamLoad = nullptr, g_musicLoad = nullptr;

std::string Upper(std::string s) {
    CharUpperBuffA(s.data(), static_cast<DWORD>(s.size()));
    return s;
}
std::string BaseName(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? path : path.substr(slash + 1);
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
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (512ll << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &got, nullptr) && got == out.size());
    }
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
uint64_t FileSize(const std::string& path) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &info)) return 0;
    return (uint64_t{info.nFileSizeHigh} << 32) | info.nFileSizeLow;
}
std::string TempDir() {
    static std::string dir;
    if (!dir.empty()) return dir;
    char tmp[MAX_PATH] = {};
    GetTempPathA(MAX_PATH, tmp);
    dir = std::string(tmp) + "GreatEscapeFix";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\sound";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

uint32_t ElfHash(const std::string& s) {   // FUN_004d01b0: the bank key of "<NAME>.sbk" (case-sensitive)
    uint32_t h = 0;
    for (unsigned char c : s) {
        h = (h << 4) + c;
        if (const uint32_t g = h & 0xF0000000) h = (h & 0x0FFFFFFF) ^ (g >> 24);
    }
    return h;
}

// ---- .sch chunks ----
struct Chunk {
    uint32_t tag;
    const char* data;  // header included
    uint32_t size;     // header included
};

// Every chunk of a sound cache; `error` gets why the file can't be used (the game exits on a chunk it can't read).
std::vector<Chunk> Chunks(const std::string& file, std::string* error = nullptr) {
    std::vector<Chunk> out;
    auto fail = [&](const char* why) {
        if (error) *error = why;
        out.clear();
        return out;
    };
    if (file.size() < 8 || memcmp(file.data(), "SCH\0", 4) != 0) return fail("not a sound cache (no SCH header)");
    for (size_t p = 8; p < file.size();) {
        if (p + 8 > file.size()) return fail("cut off (half a chunk header at the end)");
        uint32_t tag, size;
        memcpy(&tag, file.data() + p, 4);
        memcpy(&size, file.data() + p + 4, 4);
        if (size > file.size() - p - 8) return fail("cut off (a chunk runs past the end)");
        if (tag != kTagBank && tag != kTagSample && tag != kTagStream && tag != kTagMusic)
            return fail("unknown chunk (only BANK, PFSM, PFST, IMUS)");
        if ((tag == kTagSample || tag == kTagStream) && size < 16) return fail("a sample chunk is too short");
        if (tag == kTagSample) {
            uint32_t bytes;
            memcpy(&bytes, file.data() + p + 16, 4);
            if (bytes + 16 != size) return fail("a sample's size doesn't match its chunk");
        }
        if ((tag == kTagBank || tag == kTagMusic) && size < 4) return fail("a bank / music chunk is too short");
        out.push_back({tag, file.data() + p, size + 8});
        p += 8 + size;
    }
    return out;
}

int32_t Language(const Chunk& c) {
    int32_t lang = 0;
    memcpy(&lang, c.data + 8, 4);
    return lang;
}
uint32_t SampleId(const Chunk& c) {
    uint32_t id = 0;
    memcpy(&id, c.data + 12, 4);
    return id;
}
uint64_t SampleKey(const Chunk& c) { return uint64_t(static_cast<uint32_t>(Language(c))) << 32 | SampleId(c); }
uint32_t BankKey(const Chunk& c) {   // BANK: its key; IMUS: its id
    uint32_t key = 0;
    memcpy(&key, c.data + 8, 4);
    return key;
}
bool IsSample(const Chunk& c) { return c.tag == kTagSample || c.tag == kTagStream; }

// ---- what the mods bring ----
std::vector<std::string> g_packs;                                          // mod .sch bank packs (mod order)
std::unordered_map<std::string, std::vector<std::string>> g_levelSounds;   // upper level .sch -> mod files (mod order)
struct MadePack {
    std::string name, pack;   // bank name, pack bytes ("SCH" + chunks)
};
std::vector<MadePack> g_madePacks;                                         // banks made / borrowed for .weapon files
std::unordered_map<std::string, std::string> g_streams;                    // upper .psf name -> mod path
uint32_t g_generation = 1;                                                 // bumps when any of the above changes
struct Merged {
    uint32_t generation = 0;
    std::string path;   // "" = the game's own file is fine
};
std::unordered_map<std::string, Merged> g_merged;                          // upper level .sch -> merged copy

void ScanFolder(const std::string& dir, const std::string& mod, std::vector<std::pair<std::string, std::string>>& sch) {
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
        const std::string key = Upper(name), path = dir + "\\" + name;
        if (key.size() > 4 && key.compare(key.size() - 4, 4, ".SCH") == 0) sch.push_back({path, mod});
        else if (key.size() > 4 && key.compare(key.size() - 4, 4, ".PSF") == 0) g_streams[key] = path;   // later mod wins
    }
    for (const std::string& d : dirs) ScanFolder(dir + "\\" + d, mod, sch);
}

// ---- the game's own caches: which one holds a bank ----
std::unordered_map<uint32_t, std::string> g_bankCache;   // bank key -> game .sch file (first one by name)
bool g_bankScan = false;

void ScanGameBanks() {
    if (g_bankScan) return;
    g_bankScan = true;
    const std::string game = GameDir();
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((game + "\\*.sch").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> names;
    do names.push_back(fd.cFileName);
    while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    for (const std::string& n : names) {
        std::string file;
        if (!ReadAll(game + "\\" + n, file)) continue;
        for (const Chunk& c : Chunks(file))
            if (c.tag == kTagBank) g_bankCache.emplace(BankKey(c), game + "\\" + n);
    }
}

// Bank <name> and the samples it refers to (every language), from the first game cache that has it.
bool FindBank(const std::string& name, std::string& bank, std::vector<std::string>& samples, std::string* from = nullptr) {
    ScanGameBanks();
    const uint32_t key = ElfHash(name + ".sbk");
    auto it = g_bankCache.find(key);
    std::string file;
    if (it == g_bankCache.end() || !ReadAll(it->second, file)) return false;
    const std::vector<Chunk> chunks = Chunks(file);
    const Chunk* b = nullptr;
    for (const Chunk& c : chunks)
        if (c.tag == kTagBank && BankKey(c) == key) b = &c;
    if (!b) return false;
    bank.assign(b->data, b->size);
    std::unordered_set<uint32_t> refs;   // bank entries aren't 4-byte aligned: every offset is a candidate
    for (uint32_t o = 12; o + 4 <= b->size; ++o) {
        uint32_t v;
        memcpy(&v, b->data + o, 4);
        refs.insert(v);
    }
    samples.clear();
    for (const Chunk& c : chunks)
        if (IsSample(c) && refs.count(SampleId(c))) samples.emplace_back(c.data, c.size);
    if (from) *from = BaseName(it->second);
    return true;
}

// Sample ids in the order the bank refers to them.
std::vector<uint32_t> BankSampleOrder(const std::string& bank, const std::vector<std::string>& samples) {
    std::unordered_set<uint32_t> have;
    for (const std::string& s : samples) {
        uint32_t id;
        memcpy(&id, s.data() + 12, 4);
        have.insert(id);
    }
    std::vector<uint32_t> order;
    for (uint32_t o = 12; o + 4 <= bank.size(); ++o) {
        uint32_t v;
        memcpy(&v, bank.data() + o, 4);
        if (have.count(v) && std::find(order.begin(), order.end(), v) == order.end()) order.push_back(v);
    }
    return order;
}

bool HaveMadePack(const std::string& name) {
    for (const MadePack& m : g_madePacks)
        if (_stricmp(m.name.c_str(), name.c_str()) == 0) return true;
    return false;
}
void AddMadePack(const std::string& name, const std::string& chunks) {
    std::string out("SCH\0", 4);
    const uint32_t body = static_cast<uint32_t>(chunks.size());
    out.append(reinterpret_cast<const char*>(&body), 4);
    out += chunks;
    g_madePacks.push_back({name, out});
    ++g_generation;
}

// ---- the merged level cache ----
// The level's cache with the mods' level replacements swapped in (BANK by key, IMUS by id, PFSM / PFST by language +
// id; a mod sample of language -1 also replaces every language's sample of that id), then every bank / sample of the
// packs the cache doesn't have yet appended. "" = nothing to change.
std::string BuildCache(const char* name) {
    const std::string key = Upper(BaseName(name));
    Merged& m = g_merged[key];
    if (m.generation == g_generation) return m.path;
    m = {g_generation, ""};
    auto overlays = g_levelSounds.find(key);
    if (overlays == g_levelSounds.end() && g_packs.empty() && g_madePacks.empty()) return m.path;
    std::string file;
    if (!ReadAll(GameDir() + "\\" + BaseName(name), file)) return m.path;   // (an archive-only cache: the game's open)
    std::vector<Chunk> level = Chunks(file);
    if (level.empty()) return m.path;
    int replaced = 0, added = 0, banks = 0;
    if (overlays != g_levelSounds.end()) {
        std::unordered_map<uint32_t, std::string> bankSwap, musicSwap;
        std::unordered_map<uint64_t, std::string> sampleSwap;
        std::unordered_map<uint32_t, std::string> anyLanguage;   // language -1 overlays by id
        for (const std::string& path : overlays->second) {
            std::string pack;
            if (!ReadAll(path, pack)) continue;
            for (const Chunk& c : Chunks(pack)) {
                const std::string bytes(c.data, c.size);
                if (c.tag == kTagBank) bankSwap[BankKey(c)] = bytes;
                else if (c.tag == kTagMusic) musicSwap[BankKey(c)] = bytes;
                else {
                    sampleSwap[SampleKey(c)] = bytes;
                    if (Language(c) == -1) anyLanguage[SampleId(c)] = bytes;
                }
            }
        }
        std::unordered_set<uint32_t> usedBanks, usedMusic;
        std::unordered_set<uint64_t> usedSamples;
        std::string rebuilt(file.data(), 8);
        for (const Chunk& c : level) {
            const std::string* swap = nullptr;
            std::string relabelled;
            if (c.tag == kTagBank) {
                auto it = bankSwap.find(BankKey(c));
                if (it != bankSwap.end()) swap = &it->second, usedBanks.insert(BankKey(c));
            } else if (c.tag == kTagMusic) {
                auto it = musicSwap.find(BankKey(c));
                if (it != musicSwap.end()) swap = &it->second, usedMusic.insert(BankKey(c));
            } else {
                auto it = sampleSwap.find(SampleKey(c));
                if (it != sampleSwap.end()) {
                    swap = &it->second;
                    usedSamples.insert(SampleKey(c));
                } else if (auto any = anyLanguage.find(SampleId(c));
                           any != anyLanguage.end() && memcmp(any->second.data(), c.data, 4) == 0) {   // same tag
                    relabelled = any->second;   // the mod's sample for every language: keep this one's language
                    memcpy(relabelled.data() + 8, c.data + 8, 4);
                    swap = &relabelled;
                    usedSamples.insert(uint64_t(0xFFFFFFFFu) << 32 | SampleId(c));
                }
            }
            if (swap) {
                rebuilt += *swap;
                ++replaced;
            } else {
                rebuilt.append(c.data, c.size);
            }
        }
        for (const auto& [k, bytes] : bankSwap)   // overlay chunks the level doesn't have: added like a pack
            if (!usedBanks.count(k)) rebuilt += bytes, ++added;
        for (const auto& [k, bytes] : musicSwap)
            if (!usedMusic.count(k)) rebuilt += bytes, ++added;
        for (const auto& [k, bytes] : sampleSwap)
            if (!usedSamples.count(k)) rebuilt += bytes, ++added;
        file.swap(rebuilt);
        level = Chunks(file);
        features::ModLog("Sounds: %s - %d sound(s) replaced, %d added from %zu mod file(s)", key.c_str(), replaced, added,
                         overlays->second.size());
    }
    std::unordered_set<uint32_t> haveBanks;
    std::unordered_set<uint64_t> haveSamples;
    for (const Chunk& c : level) {
        if (c.tag == kTagBank) haveBanks.insert(BankKey(c));
        if (IsSample(c)) haveSamples.insert(SampleKey(c));
    }
    std::string extra;
    auto addPack = [&](const std::string& pack) {   // its banks / samples the cache doesn't have yet
        for (const Chunk& c : Chunks(pack)) {
            if (c.tag == kTagBank && haveBanks.insert(BankKey(c)).second) {
                extra.append(c.data, c.size);
                ++banks;
            } else if (IsSample(c) && haveSamples.insert(SampleKey(c)).second) {
                extra.append(c.data, c.size);
            }
        }
    };
    for (const std::string& path : g_packs) {
        std::string pack;
        if (ReadAll(path, pack)) addPack(pack);
    }
    for (const MadePack& p : g_madePacks) addPack(p.pack);
    if (extra.empty() && overlays == g_levelSounds.end()) return m.path;   // nothing new for this level
    file += extra;
    const uint32_t body = static_cast<uint32_t>(file.size() - 8);
    memcpy(file.data() + 4, &body, 4);
    std::string error;
    if (Chunks(file, &error).empty()) {   // never hand the game a cache it would exit on
        features::ModLog("[fail] Sounds: %s - merged cache %s; the game's own is used", key.c_str(), error.c_str());
        return m.path;
    }
    const std::string out = TempDir() + "\\" + BaseName(name);
    if (!WriteAll(out, file)) {
        features::ModLog("[fail] Sounds: %s - can't write %s; the game's own cache is used", key.c_str(), out.c_str());
        return m.path;
    }
    m.path = out;
    features::ModLog("Sounds: %s + %d bank(s) from packs, %zu bytes (%s)", key.c_str(), banks, extra.size(), out.c_str());
    return m.path;
}

// SetSoundCache's open (call 0x41F8ED in FUN_0041f8e0): the merged copy from disk, else the game's own.
void* __cdecl OpenCache(const char* name, int flags) {
    const std::string merged = name ? BuildCache(name) : std::string();
    if (merged.empty()) return reinterpret_cast<OpenWrapperFn>(kOpenWrapper)(name, flags);
    auto* fileFlags = reinterpret_cast<uint32_t*>(kFileFlags);
    const uint32_t saved = *fileFlags & 3u;
    *fileFlags &= ~3u;   // this one from disk (the opener copies the flags into the file object)
    void* file = reinterpret_cast<FileOpenFn>(kFileOpen)(merged.c_str(), reinterpret_cast<const char*>(kReadMode));
    *fileFlags |= saved;
    if (!file) {
        features::ModLog("[fail] Sounds: can't open %s - the game's own %s is used", merged.c_str(), name);
        return reinterpret_cast<OpenWrapperFn>(kOpenWrapper)(name, flags);
    }
#ifndef DS_DIST
    dslog::Write("Sounds: %s opened from %s", name, merged.c_str());
#endif
    return file;
}

// ---- streams ----
// path (game allocator) -> the mod's file; returns the mod file's size (0 = not replaced)
uint64_t SwapPath(char*& path, const char* what) {
    if (!path || g_streams.empty()) return 0;
    const std::string name = Upper(BaseName(path));
    auto it = g_streams.find(name);
    if (it == g_streams.end()) return 0;
    const uint64_t size = FileSize(it->second);
    if (size < 8 + 32) {
        features::ModLog("[fail] Sounds: %s is too small for a %s - the game's own is played", it->second.c_str(), what);
        return 0;
    }
    char* copy = reinterpret_cast<char*(__cdecl*)(uint32_t, int)>(kAlloc)(static_cast<uint32_t>(it->second.size() + 1), 0x15);
    if (!copy) return 0;
    memcpy(copy, it->second.c_str(), it->second.size() + 1);
    reinterpret_cast<void(__cdecl*)(void*)>(kFree)(path);
    path = copy;
    static std::unordered_set<std::string> logged;
    if (logged.insert(it->second).second) features::ModLog("Sounds: %s %s -> %s", what, name.c_str(), it->second.c_str());
    return size;
}

char __fastcall StreamLoad(uint8_t* reader, void*, void* cache) {
    const char r = g_streamLoad(reader, cache);
    if (!r) return r;
    char*& path = *reinterpret_cast<char**>(reader + 0x18);
    if (const uint64_t size = SwapPath(path, "stream"))
        *reinterpret_cast<uint32_t*>(reader + 0x14) = static_cast<uint32_t>((size - 8) / 32 * 16);   // whole L / R block pairs
    return r;
}

char __fastcall MusicLoad(uint8_t* reader, void*, void* cache) {
    const char r = g_musicLoad(reader, cache);
    char*& path = *reinterpret_cast<char**>(reader + 0x14);
    if (!r || !path || g_streams.empty()) return r;
    auto it = g_streams.find(Upper(BaseName(path)));
    if (it == g_streams.end()) return r;
    const uint64_t own = FileSize(GameDir() + "\\" + path), mod = FileSize(it->second);
    if (own != mod) {
        static std::unordered_set<std::string> logged;
        if (logged.insert(it->second).second)
            features::ModLog("[fail] Sounds: %s is %llu bytes, the game's %s %llu - level music (interactive, in segments) "
                             "must keep the game file's size and layout; the game's own is played",
                             it->second.c_str(), mod, BaseName(path).c_str(), own);
        return r;
    }
    SwapPath(path, "music");
    return r;
}
}  // namespace

// The Mods folder's sound files (after every index build, before the weapon files ask for banks).
void features::ScanModSounds() {
    g_packs.clear();
    g_levelSounds.clear();
    g_streams.clear();
    g_madePacks.clear();
    const std::string root = GameDir() + "\\Mods", game = GameDir();
    std::vector<std::pair<std::string, std::string>> sch;
    for (const std::string& mod : LoadedMods()) ScanFolder(root + "\\" + mod, mod, sch);
    for (const auto& [path, mod] : sch) {
        std::string file, error;
        if (!ReadAll(path, file) || Chunks(file, &error).empty()) {
            ModLog("[fail] Sounds: %s (%s) - %s, not used", BaseName(path).c_str(), mod.c_str(),
                   error.empty() ? "can't be read" : error.c_str());
            continue;
        }
        const std::string name = Upper(BaseName(path));
        if (GetFileAttributesA((game + "\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_levelSounds[name].push_back(path);   // named like a game cache: replaces that cache's sounds
            ModLog("Sounds: %s (%s) - sounds of %s replaced", BaseName(path).c_str(), mod.c_str(), name.c_str());
        } else {
            g_packs.push_back(path);
            ModLog("Sounds: %s (%s) - bank pack, added to every level", BaseName(path).c_str(), mod.c_str());
        }
    }
    ++g_generation;
}

void features::ApplyModSounds() {
    if (!Get().mods) return;
    // (the hooks go in with no sound mods too: the MODS menu can switch one on while the game runs)
    if (!patch::HookCall(kCacheOpenCall, reinterpret_cast<const void*>(&OpenCache), kOpenWrapper))
        ModLog("[fail] Sounds: sound cache open at 0x%08X not recognised - sound packs / level sounds not used", kCacheOpenCall);
    if (!patch::HookFunction(kStreamLoad, reinterpret_cast<const void*>(&StreamLoad), reinterpret_cast<void**>(&g_streamLoad),
                             kStreamLoadEntry, sizeof kStreamLoadEntry))
        ModLog("[fail] Sounds: stream load at 0x%08X not recognised - music streams can't be replaced", kStreamLoad);
    if (!patch::HookFunction(kMusicLoad, reinterpret_cast<const void*>(&MusicLoad), reinterpret_cast<void**>(&g_musicLoad),
                             kMusicLoadEntry, sizeof kMusicLoadEntry))
        ModLog("[fail] Sounds: music load at 0x%08X not recognised - level music can't be replaced", kMusicLoad);
    ModLog("[ok]   Sounds: %zu bank pack(s), %zu cache(s) with replaced sounds, %zu stream(s), %zu made bank(s)",
           g_packs.size(), g_levelSounds.size(), g_streams.size(), g_madePacks.size());
}

bool features::BorrowSoundBank(const std::string& name, const std::string& file) {
    if (name.empty() || HaveMadePack(name)) return true;
    for (const std::string& path : g_packs) {   // a mod's own pack already has it
        std::string pack;
        if (!ReadAll(path, pack)) continue;
        for (const Chunk& c : Chunks(pack))
            if (c.tag == kTagBank && BankKey(c) == ElfHash(name + ".sbk")) return true;
    }
    std::string bank, from;
    std::vector<std::string> samples;
    if (!FindBank(name, bank, samples, &from)) {
        ModLog("[fail] Sounds: %s - sound bank %s is in no sound cache", file.c_str(), name.c_str());
        return false;
    }
    std::string chunks = bank;
    for (const std::string& s : samples) chunks += s;
    AddMadePack(name, chunks);
    ModLog("Sounds: bank %s borrowed from %s (%zu samples) for %s - every level gets it", name.c_str(), from.c_str(),
           samples.size(), file.c_str());
    return true;
}

bool features::MakeWeaponSoundBank(const std::string& donor, const std::string& newName, const std::string& dir,
                                   const std::vector<std::string>& shots, const std::string& tail, const std::string& file) {
    if (newName.empty() || newName.size() > 60) {
        ModLog("[fail] Sounds: %s - bank name \"%s\" must be 1-60 characters", file.c_str(), newName.c_str());
        return false;
    }
    if (HaveMadePack(newName)) {
        ModLog("[fail] Sounds: %s - a sound bank %s was already made", file.c_str(), newName.c_str());
        return false;
    }
    std::string bank;
    std::vector<std::string> samples;
    if (!FindBank(donor, bank, samples)) {
        ModLog("[fail] Sounds: %s - sound %s not found in any sound cache", file.c_str(), donor.c_str());
        return false;
    }
    const std::vector<uint32_t> order = BankSampleOrder(bank, samples);
    if (order.size() < 3) {
        ModLog("[fail] Sounds: %s - sound %s isn't a gun's (%zu samples)", file.c_str(), donor.c_str(), order.size());
        return false;
    }
    // gun banks: click, distant tail, shot 1, shot 2 - shots replace 3.., tail replaces 2
    std::unordered_map<uint32_t, std::string> replace;   // old sample id -> WAV file
    if (!shots.empty())
        for (size_t i = 2; i < order.size(); ++i) replace[order[i]] = shots[(i - 2) % shots.size()];
    if (!tail.empty()) replace[order[1]] = tail;
    const uint32_t key = ElfHash(newName + ".sbk");
    memcpy(&bank[8], &key, 4);
    std::unordered_map<uint32_t, std::string> encoded;   // old id -> new chunk (once per id, whatever the language)
    std::string chunks;
    int n = 0;
    for (const std::string& s : samples) {
        uint32_t id;
        memcpy(&id, s.data() + 12, 4);
        auto it = replace.find(id);
        if (it == replace.end()) {
            chunks += s;   // kept (the click; the tail unless replaced)
            continue;
        }
        if (encoded.count(id)) continue;
        std::string wav;
        adpcm::Pcm pcm;
        const std::string path = dir + "\\" + it->second;
        if (!ReadAll(path, wav) || !adpcm::ParseWav(wav, pcm)) {
            ModLog("[fail] Sounds: %s - can't read %s (PCM or float WAV, up to 65535 Hz)", file.c_str(), it->second.c_str());
            return false;
        }
        const uint32_t newId = ElfHash(newName + "#" + std::to_string(++n)) | 0x10000000;
        for (size_t o = 12; o + 4 <= bank.size(); ++o)
            if (!memcmp(&bank[o], &id, 4)) memcpy(&bank[o], &newId, 4);
        encoded[id] = adpcm::EncodeSample(pcm, newId);
        chunks += encoded[id];
        ModLog("Sounds: %s: %s (%u Hz, %.2f s)", newName.c_str(), it->second.c_str(), pcm.rate,
               pcm.samples.size() / static_cast<double>(pcm.rate));
    }
    AddMadePack(newName, bank + chunks);
    ModLog("Sounds: bank %s made from %s for %s - every level gets it", newName.c_str(), donor.c_str(), file.c_str());
    return true;
}
