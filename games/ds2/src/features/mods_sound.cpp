// Mods folder - sounds (sub-plugin ds2fix.modsound): sound bank packs, level sound replacement, music streams, Bink
// films and WAV-made weapon sound banks. Port of DesertStormFix's sound part (repo root src/features/mods.cpp:
// SoundCache, BorrowBank, MakeSoundBank, StreamLoad); modder page: repo root docs/sdk/ds2-mods.md "Sounds".
//
// How CDS II.exe plays sounds (2026-10-22; DS1 addresses in brackets) - the same sound library as DS1:
//  - level sound cache <level>.sch ("SCH\0", u32 size, chunks {tag, u32 size, data}: BANK {u32 key = ELF hash of
//    "<NAME>.sbk", events -> sample ids}, PFSM sample {i32 language, u32 id, u32 bytes, u16 rate, u8 bits, u8 pad,
//    data}, PFST stream {i32 language, u32 id, u32 bytes per channel, u16 rate, u8 4, u8 n, path}, IMUS interactive
//    music {u32 id, ..., path, segment table}). Loaded by CSoundManager::SetSoundCache FUN_004b5de0 thiscall(manager
//    [0x6800EC], name) [FUN_0055f540] from mission init (0x4129DA, "<level>.sch") and the front-end init (0x42C9A7,
//    "FrontEnd.sch"); it keeps samples of the manager's language (+0x3C) or -1, and EXITS THE GAME (__exit(0), "Cache
//    file ... seems to be corrupt") on a chunk it can't read - so every file we hand it is checked first. The file is
//    opened by FUN_004b5d60 (its only caller is SetSoundCache) through the open wrapper FUN_004c49c0(name, flags) at
//    0x4B5D6D -> resolver -> FUN_004c3140(path, "rb") (one game file object, [0x665E48]).
//    Here: that call is hooked. A level whose cache mods change (bank packs, level replacements, weapon banks) gets a
//    merged copy in %TEMP%\DesertStorm2Fix\sound\<name> opened from disk (archive bits of [0x665E4C] cleared for the
//    open, like mods.cpp's Open does for Mods paths); with nothing to change the game opens its own file.
//  - banks by name: CSoundManager::LoadSoundBank FUN_004b33c0 thiscall(name, flag) = ELF hash FUN_004c0ca0 of
//    "<name>.sbk" looked up in the loaded banks, else FUN_004b7250 reads the file "<name>.sbk" through the resolver
//    (the original tool's bank file - starts with 'V' + version; no game ships one). mods.cpp's resolver serves a mod's
//    .sbk by name, nothing to do here.
//  - samples: 4-bit ADPCM, the DS1 decoder with the same PlayStation VAG filter table (0x6196F0) -> mods_sound_adpcm.h.
//  - streams: CWaveDataSourcePsfStream (vtable 0x5EB560) ReadFromSoundCache 0x4B7790 thiscall(reader, cache) ret 4
//    [0x5610E0]: path +0x18 (game allocator FUN_004be320(size, 7), freed by FUN_004be3b0 in the destructor), bytes
//    per channel +0x14; opened by fopen in slot 4 (0x4B7900) - no resolver. DS2's only PFST is the front-end music
//    data\sound\PC_FrontEndMusic.psf ("PSF\x80" + u32, stereo 16-byte ADPCM blocks L / R). Speech is not streamed in
//    DS2 (PFSM samples per language: replace them with a level .sch).
//    CWaveDataSourceIMus (vtable 0x5EB4B4) ReadFromSoundCache 0x4B7370: path +0x14, segment table +0x18 (12 bytes per
//    segment, offsets into the .psf); opened by fopen in 0x4B7640. The 8 mission music files (PC_Lose, PC_Win,
//    PC_Mobeus, PC_Sandstorm, PC_FirstLight, PC_Firefight, PC_UmmQasr, PC_HouseToHouse) are these.
//    Here: both loads are wrapped (entry detours); a mod's <name>.psf (by file name) takes the path. Plain streams: the
//    length follows the mod's file. Interactive music: the cache's segment table points into the game's file, so only
//    a mod file of exactly the game file's size is used (same segment layout), else [fail].
//  - films: Bink, BinkOpen(name, flags) from one call 0x526401 (`call [0x5DA2A0]`, name = "data\FMV\<film><UK|US>PAL.bik"
//    relative to the game folder, no resolver) -> hooked: a mod's file of that name is opened instead.
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
#include "core/settings.h"
#include "features/features.h"
#include "features/mods_sound_adpcm.h"

namespace {
constexpr uint32_t kCacheOpenCall = 0x4B5D6D, kOpenWrapper = 0x4C49C0;   // in FUN_004b5d60 (SetSoundCache's open)
constexpr uint32_t kFileOpen = 0x4C3140, kReadMode = 0x5DEE04;            // FUN_004c3140(path, "rb")
constexpr uint32_t kFileFlags = 0x665E4C;                                 // bit 0 archive mode, bit 1 archive build
constexpr uint32_t kStreamLoad = 0x4B7790;
constexpr uint8_t kStreamLoadEntry[] = {0x83, 0xEC, 0x0C, 0x8D, 0x44, 0x24, 0x00};   // sub esp,0xC; lea eax,[esp]
constexpr uint32_t kMusicLoad = 0x4B7370;
constexpr uint8_t kMusicLoadEntry[] = {0x83, 0xEC, 0x08, 0x8D, 0x44, 0x24, 0x00};    // sub esp,8; lea eax,[esp]
constexpr uint32_t kAlloc = 0x4BE320, kFree = 0x4BE3B0;                   // game allocator (size, tag) / free
constexpr uint32_t kBinkOpenCall = 0x526401, kBinkOpenSlot = 0x5DA2A0;
constexpr uint32_t kSoundManager = 0x6800EC;                              // +0x3C language kept by SetSoundCache

constexpr uint32_t kTagBank = 0x4B4E4142, kTagSample = 0x4D534650, kTagStream = 0x54534650, kTagMusic = 0x53554D49;

using OpenWrapperFn = void*(__cdecl*)(const char*, int);
using FileOpenFn = void*(__cdecl*)(const char*, const char*);
using LoadFn = char(__thiscall*)(void*, void*);
using BinkOpenFn = void*(__stdcall*)(const char*, uint32_t);
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
    dir = std::string(tmp) + "DesertStorm2Fix";
    CreateDirectoryA(dir.c_str(), nullptr);
    dir += "\\sound";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

uint32_t ElfHash(const std::string& s) {   // FUN_004c0be0 / FUN_004c0ca0: the bank key of "<NAME>.sbk" (case-sensitive)
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
std::vector<std::string> g_packs;                                      // mod .sch bank packs (index order)
std::unordered_map<std::string, std::vector<std::string>> g_levelSounds;   // upper level .sch -> mod files (mod order)
struct MadePack {
    std::string name, pack;   // bank name, pack bytes ("SCH" + chunks)
    bool uses;                // from a .uses "bank" line (BorrowGameBank) - else a .weapon's (Make / BorrowSoundBank)
};
std::vector<MadePack> g_madePacks;   // banks the plugin made / borrowed for mods
std::unordered_map<std::string, std::string> g_streams;                // upper .psf name -> mod path
std::unordered_map<std::string, std::string> g_films;                  // upper .bik name -> mod path
uint32_t g_generation = 1;                                             // bumps when any of the above changes
struct Merged {
    uint32_t generation = 0;
    std::string path;   // "" = the game's own file is fine
};
std::unordered_map<std::string, Merged> g_merged;                      // upper level .sch -> merged copy
bool g_hooked = false;

void ScanIndex() {
    g_packs.clear();
    g_levelSounds.clear();
    g_streams.clear();
    g_films.clear();
    const std::string game = GameDir();
    for (const features::ModFileRef& f : features::ModFilesWithExtension(".SCH")) {
        std::string file, error;
        if (!ReadAll(f.path, file) || Chunks(file, &error).empty()) {
            features::ModLog("[fail] Sounds: %s (%s) - %s, not used", BaseName(f.path).c_str(), f.mod.c_str(),
                             error.empty() ? "can't be read" : error.c_str());
            continue;
        }
        const std::string name = Upper(BaseName(f.path));
        if (GetFileAttributesA((game + "\\" + name).c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_levelSounds[name].push_back(f.path);   // named like a level cache: replaces that level's sounds
            features::ModLog("Sounds: %s (%s) - sounds of %s replaced", BaseName(f.path).c_str(), f.mod.c_str(), name.c_str());
        } else {
            g_packs.push_back(f.path);
            features::ModLog("Sounds: %s (%s) - bank pack, added to every level", BaseName(f.path).c_str(), f.mod.c_str());
        }
    }
    // ModFilesWithExtension: mod name order; a later mod's file of the same name wins (the index already did that)
    for (const features::ModFileRef& f : features::ModFilesWithExtension(".PSF")) g_streams[Upper(BaseName(f.path))] = f.path;
    for (const features::ModFileRef& f : features::ModFilesWithExtension(".BIK")) g_films[Upper(BaseName(f.path))] = f.path;
    ++g_generation;
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
void AddMadePack(const std::string& name, const std::string& chunks, bool uses = false) {
    std::string out("SCH\0", 4);
    const uint32_t body = static_cast<uint32_t>(chunks.size());
    out.append(reinterpret_cast<const char*>(&body), 4);
    out += chunks;
    g_madePacks.push_back({name, out, uses});
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
    const std::string gamePath = GameDir() + "\\" + BaseName(name);
    if (!ReadAll(gamePath, file)) return m.path;   // (an archive-only cache: the game's open, unchanged)
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
        // overlay chunks the level doesn't have: added like a pack
        for (const auto& [k, bytes] : bankSwap)
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

// SetSoundCache's open (call 0x4B5D6D in FUN_004b5d60): the merged copy from disk, else the game's own.
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
    char* copy = reinterpret_cast<char*(__cdecl*)(uint32_t, int)>(kAlloc)(static_cast<uint32_t>(it->second.size() + 1), 7);
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
            features::ModLog("[fail] Sounds: %s is %llu bytes, the game's %s %llu - mission music (interactive, in segments) "
                             "must keep the game file's size and layout; the game's own is played",
                             it->second.c_str(), mod, BaseName(path).c_str(), own);
        return r;
    }
    SwapPath(path, "music");
    return r;
}

// ---- films ----
void* __stdcall BinkOpenHook(const char* name, uint32_t flags) {
    const auto real = *reinterpret_cast<BinkOpenFn*>(kBinkOpenSlot);
    if (name && !g_films.empty()) {
        auto it = g_films.find(Upper(BaseName(name)));
        if (it != g_films.end()) {
            if (void* bink = real(it->second.c_str(), flags)) {
                static std::unordered_set<std::string> logged;
                if (logged.insert(it->second).second)
                    features::ModLog("Sounds: film %s -> %s", BaseName(name).c_str(), it->second.c_str());
                return bink;
            }
            features::ModLog("[fail] Sounds: Bink couldn't open %s - the game's film is played", it->second.c_str());
        }
    }
    return real(name, flags);
}

template <class Fn>
bool HookEntry(uint32_t entry, const void* fn, Fn& next, const uint8_t* prologue, size_t len) {
#ifdef DS2_PSE_HOST
    return patch::HookFunction(entry, fn, reinterpret_cast<void**>(&next), prologue, len);
#else
    if (!patch::Matches(entry, prologue, len)) return false;
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

void features::ApplyModSounds() {
    static bool done = false;
    if (done) return;
    done = true;
    if (!ModsFolderEnabled()) return;
    ScanIndex();
    // (the hooks go in with no sound mods too: the MODS menu can switch one on while the game runs)
    bool ok = patch::HookCall(kCacheOpenCall, reinterpret_cast<const void*>(&OpenCache), kOpenWrapper);
    if (!ok) ModLog("[fail] Sounds: sound cache open at 0x%08X not recognised - sound packs / level sounds not used", kCacheOpenCall);
    if (!HookEntry(kStreamLoad, reinterpret_cast<const void*>(&StreamLoad), g_streamLoad, kStreamLoadEntry, sizeof kStreamLoadEntry))
        ModLog("[fail] Sounds: stream load at 0x%08X not recognised - music streams can't be replaced", kStreamLoad);
    if (!HookEntry(kMusicLoad, reinterpret_cast<const void*>(&MusicLoad), g_musicLoad, kMusicLoadEntry, sizeof kMusicLoadEntry))
        ModLog("[fail] Sounds: music load at 0x%08X not recognised - mission music can't be replaced", kMusicLoad);
    if (!patch::HookIndirectCall(kBinkOpenCall, reinterpret_cast<const void*>(&BinkOpenHook), kBinkOpenSlot))
        ModLog("[fail] Sounds: film open at 0x%08X not recognised - films can't be replaced", kBinkOpenCall);
    g_hooked = ok;
#ifndef DS_DIST
    {   // Dev\SoundTestWeapon = "donor;new bank;folder;shot.wav;tail.wav" - what a .weapon's shot / tail sound will call
        char v[1024] = {};
        DWORD size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, "Software\\DesertStorm2Fix\\Dev", "SoundTestWeapon", RRF_RT_REG_SZ, nullptr, v,
                         &size) == ERROR_SUCCESS) {
            std::vector<std::string> f;
            for (const char* p = v;; ++p) {
                const char* e = strchr(p, ';');
                f.emplace_back(p, e ? e - p : strlen(p));
                if (!e) break;
                p = e;
            }
            f.resize(5);
            MakeWeaponSoundBank(f[0], f[1], f[2], f[3].empty() ? std::vector<std::string>{} : std::vector<std::string>{f[3]},
                                f[4], "Dev\\SoundTestWeapon");
        }
    }
#endif
    ModLog("[ok]   Sounds: %zu bank pack(s), %zu level(s) with replaced sounds, %zu stream(s), %zu film(s)", g_packs.size(),
           g_levelSounds.size(), g_streams.size(), g_films.size());
}

void features::ReloadModSounds() {
    if (!ModsFolderEnabled()) return;
    ScanIndex();
    ModLog("[ok]   Sounds: %zu bank pack(s), %zu level(s) with replaced sounds, %zu stream(s), %zu film(s) - level sounds "
           "change at the next level load", g_packs.size(), g_levelSounds.size(), g_streams.size(), g_films.size());
}

namespace {
bool Borrow(const std::string& name, bool uses, const std::string& file) {
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
        features::ModLog("[fail] Sounds: %s%ssound bank %s is in no level sound cache", file.c_str(), file.empty() ? "" : " - ",
                         name.c_str());
        return false;
    }
    std::string chunks = bank;
    for (const std::string& s : samples) chunks += s;
    AddMadePack(name, chunks, uses);
    features::ModLog("Sounds: bank %s borrowed from %s (%zu samples)%s%s - every level gets it", name.c_str(), from.c_str(),
                     samples.size(), file.empty() ? "" : " for ", file.c_str());
    return true;
}
}  // namespace

bool features::BorrowSoundBank(const std::string& name) { return Borrow(name, false, ""); }
bool features::BorrowGameBank(const std::string& name, const std::string& file) { return Borrow(name, true, file); }

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
        ModLog("[fail] Sounds: %s - sound %s not found in any level sound cache", file.c_str(), donor.c_str());
        return false;
    }
    const std::vector<uint32_t> order = BankSampleOrder(bank, samples);
    if (order.size() < 3) {
        ModLog("[fail] Sounds: %s - sound %s isn't a gun's (%zu samples)", file.c_str(), donor.c_str(), order.size());
        return false;
    }
    // gun banks: click, distant tail, shot 1, shot 2 (every DS2 hand-held gun) - shots replace 3.., tail replaces 2
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

namespace {
void ClearMade(bool uses) {
    const size_t before = g_madePacks.size();
    g_madePacks.erase(std::remove_if(g_madePacks.begin(), g_madePacks.end(), [uses](const MadePack& m) { return m.uses == uses; }),
                      g_madePacks.end());
    if (g_madePacks.size() != before) ++g_generation;
}
}  // namespace
void features::ClearWeaponSoundBanks() { ClearMade(false); }
void features::ClearGameBankReferences() { ClearMade(true); }
