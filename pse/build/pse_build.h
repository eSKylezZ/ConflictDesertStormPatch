#pragma once
// pse_build - the first-run builder of docs/internal/DESIGN.md §12 (stage 1: the asset store). Plain C++, no game code: it
// reads files, never runs or patches the game. Used by:
//   - pse_build.exe (pse_build_main.cpp): the command line front end (dev, ModKit, tests);
//   - DesertStormFix's asset-store glue (src/features/assetstore.cpp): builds / opens the store inside the game.
// Format, build stamp, search order and measurements: docs/internal/asset-store.md.
//
// The store is CONTENT-ADDRESSED: every file of every archive is hashed (SHA-256) and stored once in one pack file;
// each archive gets a manifest {name hash -> content}. A file that is the same in 12 missions is stored once; a file
// whose name is the same but whose bytes differ between two levels ("per-level variant") is two blobs, and each
// level's manifest points at its own.
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pse::build {

constexpr uint32_t kStoreFormat = 1;               // store.pack / store.idx / *.man layout (bump = rebuild)
constexpr const char* kBuilderVersion = "2.0.0";   // builder code; a different MAJOR forces a rebuild (2: + sound caches)
constexpr uint32_t kPackAlign = 16;                // blobs start on 16-byte boundaries in store.pack

struct ContentHash {
    uint8_t b[32] = {};
    bool operator==(const ContentHash& o) const;
    bool operator<(const ContentHash& o) const;
    std::string Hex() const;   // lower-case, 64 characters
};
struct ContentHashHasher {
    size_t operator()(const ContentHash& h) const;
};
// SHA-256 (Windows CNG). False if CNG is unavailable.
bool HashContent(const void* data, size_t len, ContentHash* out);

// ---- game archive formats (each game layer supplies one; DS1 is the only one so far) ------------------------------
struct ArchiveEntry {
    uint32_t nameHash, offset, size;
};
// One piece of a sound cache file (DS1 <level>.sch: chunks {tag, u32 size, data}). offset / size cover the whole
// chunk, header included (in a Store's SoundManifest: offset = blob index). `a` = BANK key (ELF hash of "<NAME>.sbk") or
// PFSM / PFST language (i32), `b` = PFSM / PFST sample id; 0 for other tags. tag 0 = bytes after the last whole chunk
// (kept so a cache rebuilds byte-identical).
struct SoundPiece {
    uint32_t tag, offset, size, a, b;
};
constexpr uint32_t kSoundTagBank = 0x4B4E4142;     // "BANK"
constexpr uint32_t kSoundTagSample = 0x4D534650;   // "PFSM"
constexpr uint32_t kSoundTagStream = 0x54534650;   // "PFST"
struct ArchiveFormat {
    const char* game;                                       // "ds1"
    uint32_t (*nameHash)(const char* name);                 // the game's file-name hash (path folders ignored)
    // Parses the index of a whole archive file. False = not an archive of this game (other .dat files, e.g. the
    // uninstaller's unins000.dat). Entries that point outside the file are left out and counted in *bad.
    bool (*parse)(const uint8_t* data, size_t size, std::vector<ArchiveEntry>* out, uint32_t* bad);
    // File-name patterns of archives in the game folder ("*.dat"); names starting with "unins" are never archives.
    const char* pattern;
    // Sound caches (optional; nullptr = the game has none): file pattern ("*.sch") and parser. The parser fills the
    // 8-byte file header and pieces covering every byte after it, in file order. False = not a sound cache.
    const char* soundPattern = nullptr;
    bool (*parseSound)(const uint8_t* data, size_t size, uint8_t header[8], std::vector<SoundPiece>* out) = nullptr;
};
const ArchiveFormat& Ds1Archives();

// DS1: FUN_004b9a10 - LFSR over the upper-cased bare file name (folders stripped). "WEAPONS.RFX" = 0x4475E8D7.
uint32_t Ds1NameHash(const char* name);
bool Ds1ParseArchive(const uint8_t* data, size_t size, std::vector<ArchiveEntry>* out, uint32_t* bad);
// DS1 level sound cache: "SCH\0", u32 size of the rest, chunks {u32 tag, u32 size, data} (BANK / PFSM / PFST / IMUS) -
// read in order by the game's FUN_0055f540. sound.cpp.
bool Ds1ParseSoundCache(const uint8_t* data, size_t size, uint8_t header[8], std::vector<SoundPiece>* out);
// The sound bank key: FUN_004b9bf0, ELF hash over "<NAME>.sbk" (case-sensitive, as the game spells it).
uint32_t Ds1BankKey(const char* bankFileName);

// ---- building -------------------------------------------------------------------------------------------------------
struct StoreOptions {
    std::wstring gameDir;                 // the install (folder of the game's exe and archives)
    std::wstring outDir;                  // the store folder ("" = DefaultStoreDir(gameDir))
    const ArchiveFormat* format = nullptr;   // nullptr = Ds1Archives()
    bool force = false;                   // rebuild even when the stamp matches
    bool names = true;                    // recover file names for the manifests (dictionary, optional)
    std::function<void(const std::string&)> log;   // progress / summary lines (may be empty)
    // First-boot progress window (may be empty): called from the building thread with 0..1000 and what is being done
    // ("mission3.dat", "file names"). `cancelled` is polled between files: true = stop, leave no store (BuildStore
    // returns false, StoreStats::cancelled set).
    std::function<void(uint32_t permille, const std::string& current)> progress;
    std::function<bool()> cancelled;
};

struct StoreStats {
    bool upToDate = false;        // the stamp matched: nothing was done
    uint32_t archives = 0;        // archives read
    uint32_t skippedFiles = 0;    // matching files that are not archives (unins000.dat)
    uint32_t files = 0;           // archive entries stored (all manifests together)
    uint32_t badEntries = 0;      // entries left out (outside their file)
    uint32_t uniqueBlobs = 0;
    uint64_t inputBytes = 0;      // sum of all entries' sizes
    uint64_t storeBytes = 0;      // store.pack size
    uint32_t namesWithVariants = 0;   // name hashes with >= 2 different contents across archives
    uint32_t namesKnown = 0;      // entries whose name the dictionary recovered
    bool cancelled = false;       // the build was cancelled (options.cancelled)
    uint32_t soundCaches = 0;     // sound cache files read (*.sch)
    uint32_t soundPieces = 0;     // their chunks (all manifests together)
    uint64_t soundInputBytes = 0; // their total size
    uint32_t soundBlobs = 0;      // unique chunk contents among them (part of uniqueBlobs)
    uint64_t soundStoreBytes = 0; // bytes of those unique chunks in store.pack
    uint32_t msTotal = 0, msRead = 0, msHash = 0, msWrite = 0, msNames = 0;
    // Name hash -> archives per variant (for the summary; filled when a build ran).
    std::vector<std::string> variantExamples;
};

// <game>\PSE\Cache\assets when <game>\PSE\Cache can be written, else %LOCALAPPDATA%\PSE\<game>-<install hash>\assets
// (install hash = 8 hex digits of the SHA-256 of the lower-case game folder path). Creates the folders.
std::wstring DefaultStoreDir(const std::wstring& gameDir, const char* gameId = "ds1");
// Where a player's original game files go once processed (<game>\PSE\Originals, DesertStormFix's "move originals");
// every reader of the game's archives / sound caches / mission DLLs looks in the game folder first, then here.
std::wstring OriginalsDir(const std::wstring& gameDir);
// Whether the stamp in storeDir matches the archives in gameDir (format, builder major, every input's name, size and
// modification time). *why = the first difference (empty when valid).
bool StampValid(const std::wstring& storeDir, const std::wstring& gameDir, const ArchiveFormat& format,
                std::string* why = nullptr);
// Builds (or confirms) the store. False + *error on failure (an existing valid store is left untouched).
bool BuildStore(const StoreOptions& options, StoreStats* stats, std::string* error);

// ---- reading --------------------------------------------------------------------------------------------------------
struct Blob {
    uint64_t offset = 0;   // in store.pack
    uint32_t size = 0;
    ContentHash hash;
};

class Store {
public:
    struct SoundManifest {
        std::string cache;                                // lower-case file name: "mission1.sch"
        uint8_t header[8] = {};                           // the file's own first 8 bytes
        std::vector<SoundPiece> pieces;                   // in file order; offset = blob index, size = chunk bytes
    };
    struct Manifest {
        std::string archive;                              // lower-case file name: "mission1.dat"
        std::vector<ArchiveEntry> entries;                // as in the archive (offset = blob index here)
        std::unordered_map<uint32_t, uint32_t> byHash;    // name hash -> blob index (first entry of that hash)
        std::unordered_map<uint32_t, std::string> names;  // name hash -> recovered name (may be empty)
    };

    bool Open(const std::wstring& storeDir, std::string* error = nullptr);
    void Close();
    bool IsOpen() const { return !blobs_.empty() || !manifests_.empty(); }

    const std::wstring& Dir() const { return dir_; }
    std::wstring PackPath() const { return dir_ + L"\\store.pack"; }
    const std::vector<Blob>& Blobs() const { return blobs_; }
    const std::vector<Manifest>& Manifests() const { return manifests_; }
    // Manifest of an archive by its file name (any case, folders ignored), or nullptr.
    const Manifest* Find(const std::string& archive) const;
    // Blob index of `nameHash` in that archive, or -1.
    int32_t Lookup(const std::string& archive, uint32_t nameHash) const;
    // A recovered name for a hash from any manifest ("" = unknown).
    std::string NameOf(uint32_t nameHash) const;
    // The blob's bytes (opens store.pack each call: use for tests / tools, the game reads the pack itself).
    bool Read(uint32_t blob, std::vector<uint8_t>* out) const;
    // Several blobs appended to *out in the given order (one pack handle, neighbouring blobs read in one go).
    bool ReadBlobs(const std::vector<uint32_t>& blobs, std::string* out) const;

    // ---- sound caches (sound.cpp) ----
    const std::vector<SoundManifest>& SoundCaches() const { return sound_; }
    const SoundManifest* FindSound(const std::string& cache) const;   // any case, folders ignored
    // The cache file as the game ships it, rebuilt from the store's pieces (byte-identical).
    bool RebuildSound(const std::string& cache, std::string* out) const;
    // Whether some cache has bank `key`; *cache = the first one (name order).
    bool HasSoundBank(uint32_t key, std::string* cache = nullptr) const;
    // A bank pack for bank `key`: "SCH\0" + u32 size + the BANK chunk + every sample chunk (PFSM / PFST, any cache, every
    // language) whose id the bank mentions - the same rule as DesertStormFix's BorrowBank. False = no cache has it.
    bool SoundBankPack(uint32_t key, std::string* out, std::string* cache = nullptr) const;

private:
    void IndexSound();
    std::wstring dir_;
    std::vector<Blob> blobs_;
    std::vector<Manifest> manifests_;
    std::unordered_map<std::string, size_t> byArchive_;
    std::vector<SoundManifest> sound_;
    std::unordered_map<std::string, size_t> bySound_;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> banks_;          // key -> (cache, piece)
    std::unordered_map<uint32_t, std::vector<uint32_t>> samplesById_;            // sample id -> blobs (one per language)
};

// Sound manifests on disk: <store>\sound\<cache>.sman (sound.cpp; used by BuildStore / Store::Open).
bool WriteSoundManifest(const std::wstring& path, const Store::SoundManifest& m);
bool ReadSoundManifest(const std::wstring& path, size_t blobCount, Store::SoundManifest* out);

// UTF-8 <-> UTF-16 helpers used by the CLI and the glue.
std::wstring Widen(const std::string& s);
std::string Narrow(const std::wstring& s);

}  // namespace pse::build
