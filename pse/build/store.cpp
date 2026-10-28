// The asset store: build (archives -> content-addressed pack + manifests + stamp) and read. docs/internal/asset-store.md.
#include <windows.h>

#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <unordered_set>

#include "pse_build.h"

namespace pse::build {

// Recovered names (names.cpp): hash -> name for the hashes in `wanted`.
// Collects name-like words from a blob; RecoverNames adds the game folder's exe / DLL words and resolves.
void CollectWords(const uint8_t* data, size_t size, std::unordered_set<std::string>* words);
void RecoverNames(const std::wstring& gameDir, std::unordered_set<std::string>* words,
                  const std::unordered_map<uint32_t, const char*>& wanted, uint32_t (*hash)(const char*),
                  std::unordered_map<uint32_t, std::string>* out,
                  const std::function<bool(uint32_t done, uint32_t total)>& tick = nullptr);
// Content kind of a blob from its first bytes ("DDS", "EVO" ...; "BIN" unknown) - names.cpp.
const char* ContentKind(const uint8_t* data, uint32_t size);

namespace {

constexpr char kPackMagic[8] = {'P', 'S', 'E', 'P', 'A', 'C', 'K', '1'};
constexpr char kIdxMagic[8] = {'P', 'S', 'E', 'I', 'D', 'X', '0', '1'};
constexpr char kManMagic[8] = {'P', 'S', 'E', 'M', 'A', 'N', '0', '1'};
constexpr uint32_t kNoName = 0xFFFFFFFFu;

#pragma pack(push, 1)
struct IdxHeader {
    char magic[8];
    uint32_t format, count;
    uint64_t packSize;
};
struct IdxEntry {
    uint8_t hash[32];
    uint64_t offset;
    uint32_t size, refs;
};
struct ManHeader {
    char magic[8];
    uint32_t format, count, namesBytes, reserved;
    char archive[64];
};
struct ManEntry {
    uint32_t nameHash, blob, nameOffset;
};
#pragma pack(pop)
static_assert(sizeof(IdxEntry) == 48 && sizeof(ManEntry) == 12 && sizeof(ManHeader) == 88);

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string BaseName(const std::string& s) {
    const size_t p = s.find_last_of("\\/");
    return p == std::string::npos ? s : s.substr(p + 1);
}

uint32_t Ms(std::chrono::steady_clock::time_point since) {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
}

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (1ll << 31);
    if (ok) {
        out->resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out->size()) {
            DWORD got = 0;
            const DWORD want = static_cast<DWORD>(std::min<size_t>(out->size() - done, 64u << 20));
            ok = ReadFile(f, out->data() + done, want, &got, nullptr) && got == want;
            done += got;
        }
    }
    CloseHandle(f);
    return ok;
}

class Writer {
public:
    bool Open(const std::wstring& path) {
        f_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        return f_ != INVALID_HANDLE_VALUE;
    }
    bool Write(const void* data, size_t len) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        while (len) {
            DWORD n = static_cast<DWORD>(std::min<size_t>(len, 64u << 20)), w = 0;
            if (!WriteFile(f_, p, n, &w, nullptr) || w != n) return ok_ = false;
            p += n, len -= n, pos_ += n;
        }
        return ok_;
    }
    bool Pad(uint32_t align) {
        static const uint8_t zero[64] = {};
        const uint64_t pad = (align - pos_ % align) % align;
        return pad ? Write(zero, static_cast<size_t>(pad)) : ok_;
    }
    uint64_t Pos() const { return pos_; }
    bool Close() {
        if (f_ != INVALID_HANDLE_VALUE) {
            ok_ = (FlushFileBuffers(f_) != 0) && ok_;
            CloseHandle(f_);
        }
        f_ = INVALID_HANDLE_VALUE;
        return ok_;
    }
    ~Writer() {
        if (f_ != INVALID_HANDLE_VALUE) CloseHandle(f_);
    }

private:
    HANDLE f_ = INVALID_HANDLE_VALUE;
    uint64_t pos_ = 0;
    bool ok_ = true;
};

struct InputFile {
    std::string name;       // as on disk
    std::wstring dir;       // the game folder, or OriginalsDir() for a moved file
    uint64_t size = 0;
    uint64_t mtime = 0;     // FILETIME
    bool sound = false;     // a sound cache (format.soundPattern), not an archive
};

// The game folder's archives and sound caches (by the format's patterns; uninstaller data never), then the moved ones
// in OriginalsDir (a name in both: the game folder's), sorted by lower-case name.
std::vector<InputFile> ListInputs(const std::wstring& gameDir, const ArchiveFormat& format) {
    std::vector<InputFile> out;
    std::unordered_set<std::string> seen;
    for (const std::wstring& dir : {gameDir, OriginalsDir(gameDir)})
    for (int kind = 0; kind < 2; ++kind) {
        const char* pattern = kind ? format.soundPattern : format.pattern;
        if (!pattern || (kind && !format.parseSound)) continue;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\" + Widen(pattern)).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::string name = Narrow(fd.cFileName);
            if (_strnicmp(name.c_str(), "unins", 5) == 0 || !seen.insert(Lower(name)).second) continue;
            InputFile in;
            in.name = name;
            in.dir = dir;
            in.size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            in.mtime = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            in.sound = kind == 1;
            out.push_back(in);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const InputFile& a, const InputFile& b) { return Lower(a.name) < Lower(b.name); });
    return out;
}

void DeleteMatching(const std::wstring& dir, const wchar_t* pattern) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::string Hex64(uint64_t v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(v));
    return b;
}

struct StampInput {
    std::string name;
    uint64_t size, mtime;
};
struct StampData {
    uint32_t format = 0;
    std::string builder, game;
    std::vector<StampInput> inputs;
};

bool ReadStamp(const std::wstring& dir, StampData* s) {
    std::vector<uint8_t> raw;
    if (!ReadFileAll(dir + L"\\stamp.txt", &raw)) return false;
    std::string text(raw.begin(), raw.end());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        char a[300] = {}, b[300] = {};
        unsigned long long size = 0, mtime = 0;
        unsigned format = 0;
        if (std::sscanf(line.c_str(), "pse-asset-store %u", &format) == 1) s->format = format;
        else if (std::sscanf(line.c_str(), "builder %299s", a) == 1) s->builder = a;
        else if (std::sscanf(line.c_str(), "game %299s", a) == 1) s->game = a;
        else if (std::sscanf(line.c_str(), "input %299s %llu %llx %299s", a, &size, &mtime, b) >= 3)
            s->inputs.push_back({a, size, mtime});
    }
    return s->format != 0;
}

int Major(const std::string& v) { return std::atoi(v.c_str()); }

bool WriteText(const std::wstring& path, const std::string& text) {
    Writer w;
    return w.Open(path) && w.Write(text.data(), text.size()) && w.Close();
}

void Log(const StoreOptions& o, const std::string& s) {
    if (o.log) o.log(s);
}

std::string Fmt(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    return buf;
}

BCRYPT_ALG_HANDLE Sha256Alg() {
    static BCRYPT_ALG_HANDLE alg = [] {
        BCRYPT_ALG_HANDLE a = nullptr;
        if (BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) a = nullptr;
        return a;
    }();
    return alg;
}

}  // namespace

// ---- content hash ----
bool ContentHash::operator==(const ContentHash& o) const { return std::memcmp(b, o.b, 32) == 0; }
bool ContentHash::operator<(const ContentHash& o) const { return std::memcmp(b, o.b, 32) < 0; }
std::string ContentHash::Hex() const {
    static const char* d = "0123456789abcdef";
    std::string s(64, '0');
    for (int i = 0; i < 32; ++i) s[2 * i] = d[b[i] >> 4], s[2 * i + 1] = d[b[i] & 15];
    return s;
}
size_t ContentHashHasher::operator()(const ContentHash& h) const {
    size_t v;
    std::memcpy(&v, h.b, sizeof v);
    return v;
}

bool HashContent(const void* data, size_t len, ContentHash* out) {
    BCRYPT_ALG_HANDLE alg = Sha256Alg();
    if (!alg) return false;
    // BCryptHash (one-shot) is Windows 10+; the hash object route works everywhere CNG does.
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) != 0) return false;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    bool ok = true;
    while (ok && len) {
        const ULONG n = static_cast<ULONG>(std::min<size_t>(len, 1u << 30));
        ok = BCryptHashData(h, const_cast<PUCHAR>(p), n, 0) == 0;
        p += n, len -= n;
    }
    ok = ok && BCryptFinishHash(h, out->b, 32, 0) == 0;
    BCryptDestroyHash(h);
    return ok;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// ---- where ----
std::wstring OriginalsDir(const std::wstring& gameDir) { return gameDir + L"\\PSE\\Originals"; }

std::wstring DefaultStoreDir(const std::wstring& gameDir, const char* gameId) {
    auto writable = [](const std::wstring& dir) {
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        const std::wstring probe = dir + L"\\.pse-write-test";
        HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        CloseHandle(f);
        return true;
    };
    std::wstring dir = gameDir;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    const std::wstring cache = dir + L"\\PSE\\Cache";
    if (writable(cache)) {
        SHCreateDirectoryExW(nullptr, (cache + L"\\assets").c_str(), nullptr);
        return cache + L"\\assets";
    }
    // Program Files and other read-only installs: per user, keyed by the install path.
    std::wstring lower = dir;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    ContentHash h;
    HashContent(lower.data(), lower.size() * sizeof(wchar_t), &h);
    wchar_t* local = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) base = local;
    CoTaskMemFree(local);
    const std::wstring out = base + L"\\PSE\\" + Widen(gameId ? gameId : "game") + L"-" + Widen(h.Hex().substr(0, 8)) +
                             L"\\assets";
    SHCreateDirectoryExW(nullptr, out.c_str(), nullptr);
    return out;
}

// ---- stamp ----
bool StampValid(const std::wstring& storeDir, const std::wstring& gameDir, const ArchiveFormat& format, std::string* why) {
    auto fail = [&](const std::string& w) {
        if (why) *why = w;
        return false;
    };
    StampData s;
    if (!ReadStamp(storeDir, &s)) return fail("no stamp");
    if (s.format != kStoreFormat) return fail(Fmt("store format %u, this builder writes %u", s.format, kStoreFormat));
    if (Major(s.builder) != Major(kBuilderVersion))
        return fail("builder " + s.builder + ", this one is " + kBuilderVersion);
    if (s.game != format.game) return fail("game " + s.game);
    const std::vector<InputFile> now = ListInputs(gameDir, format);
    if (now.size() != s.inputs.size()) return fail(Fmt("%zu archives now, %zu in the stamp", now.size(), s.inputs.size()));
    for (size_t i = 0; i < now.size(); ++i) {
        const StampInput& a = s.inputs[i];
        if (Lower(a.name) != Lower(now[i].name)) return fail("archive " + now[i].name + " is new or renamed");
        if (a.size != now[i].size || a.mtime != now[i].mtime) return fail(now[i].name + " changed");
    }
    for (const wchar_t* f : {L"store.pack", L"store.idx"})
        if (GetFileAttributesW((storeDir + L"\\" + f).c_str()) == INVALID_FILE_ATTRIBUTES)
            return fail(Narrow(f) + " is missing");
    if (format.soundPattern && format.parseSound &&
        std::any_of(now.begin(), now.end(), [](const InputFile& i) { return i.sound; }) &&
        GetFileAttributesW((storeDir + L"\\sound").c_str()) == INVALID_FILE_ATTRIBUTES)
        return fail("the sound manifests are missing");
    if (why) why->clear();
    return true;
}

// ---- build ----
bool BuildStore(const StoreOptions& options, StoreStats* statsOut, std::string* error) {
    const auto t0 = std::chrono::steady_clock::now();
    StoreStats st;
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        Log(options, "[fail] " + e);
        if (statsOut) *statsOut = st;
        return false;
    };
    const ArchiveFormat& format = options.format ? *options.format : Ds1Archives();
    const std::wstring dir = options.outDir.empty() ? DefaultStoreDir(options.gameDir, format.game) : options.outDir;
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    SHCreateDirectoryExW(nullptr, (dir + L"\\manifests").c_str(), nullptr);

    std::string why;
    if (!options.force && StampValid(dir, options.gameDir, format, &why)) {
        st.upToDate = true;
        st.msTotal = Ms(t0);
        Log(options, "asset store up to date (" + Narrow(dir) + ")");
        if (statsOut) *statsOut = st;
        return true;
    }
    Log(options, "building the asset store in " + Narrow(dir) + (why.empty() ? "" : " (" + why + ")"));
    const std::vector<InputFile> inputs = ListInputs(options.gameDir, format);
    if (std::none_of(inputs.begin(), inputs.end(), [](const InputFile& i) { return !i.sound; }))
        return fail("no archives (" + std::string(format.pattern) + ") in " + Narrow(options.gameDir));
    uint64_t totalInput = 0, doneInput = 0;
    for (const InputFile& in : inputs) totalInput += in.size;
    // Progress: reading / hashing / writing = 0..550, the name dictionary 550..1000 (it runs at the end and takes about
    // as long: measured 1.8 s / 3.0 s of a 4.8 s build).
    const uint32_t namesShare = options.names ? 450 : 0;
    auto progress = [&](uint64_t done, const std::string& what) {
        if (options.progress)
            options.progress(static_cast<uint32_t>(totalInput ? done * (1000 - namesShare) / totalInput : 0), what);
    };
    auto cancelled = [&] { return options.cancelled && options.cancelled(); };
    Writer pack;
    // A cancelled / failed build removes what it wrote so far (no valid store behind it: no stamp, no pack).
    auto abandon = [&](const std::string& e, bool byUser) {
        pack.Close();
        DeleteFileW((dir + L"\\store.pack.tmp").c_str());
        DeleteFileW((dir + L"\\store.idx.tmp").c_str());
        st.cancelled = byUser;
        return fail(e);
    };

    // The old stamp goes first: a build that stops half-way leaves no valid store behind.
    DeleteFileW((dir + L"\\stamp.txt").c_str());

    if (!pack.Open(dir + L"\\store.pack.tmp")) return fail("can't write " + Narrow(dir) + "\\store.pack.tmp");
    {
        uint8_t header[16] = {};
        std::memcpy(header, kPackMagic, 8);
        std::memcpy(header + 8, &kStoreFormat, 4);
        pack.Write(header, sizeof header);
    }

    struct BlobRec {
        ContentHash hash;
        uint64_t offset;
        uint32_t size, refs;
    };
    std::vector<BlobRec> blobs;
    std::unordered_map<ContentHash, uint32_t, ContentHashHasher> byContent;
    struct ManRec {
        std::string archive;
        std::vector<ManEntry> entries;
    };
    std::vector<ManRec> manifests;
    std::vector<std::string> stampInputs;
    // Name hash -> (blob -> archives) for the variants summary.
    std::unordered_map<uint32_t, std::map<uint32_t, std::vector<std::string>>> variants;
    std::unordered_set<std::string> words;   // name dictionary (optional): words found in meshes / text files
    std::vector<const char*> blobKinds;   // ContentKind per blob (the names must agree with it)

    std::vector<Store::SoundManifest> soundMans;   // sound caches: pieces with offset = blob index

    uint32_t msRead = 0, msHash = 0, msWrite = 0, msNames = 0;
    // One blob per distinct content: the index of `bytes`' blob (written to the pack when new); `dictionary` = its
    // words feed the name dictionary (archive files, not sound chunks).
    auto addBlob = [&](const uint8_t* bytes, uint32_t size, bool dictionary, uint32_t* index) {
        ContentHash h;
        if (!HashContent(bytes, size, &h)) return false;
        auto it = byContent.find(h);
        if (it != byContent.end()) {
            *index = it->second;
        } else {
            *index = static_cast<uint32_t>(blobs.size());
            byContent.emplace(h, *index);
            auto tw = std::chrono::steady_clock::now();
            pack.Pad(kPackAlign);
            blobs.push_back({h, pack.Pos(), size, 0});
            pack.Write(bytes, size);
            msWrite += Ms(tw);
            const char* kind = dictionary ? ContentKind(bytes, size) : "SND";
            blobKinds.push_back(kind);
            if (dictionary && options.names &&
                (!std::strcmp(kind, "EVO") || ((!std::strcmp(kind, "RFX") || !std::strcmp(kind, "BIN")) && size < (4u << 20)))) {
                auto tn = std::chrono::steady_clock::now();
                CollectWords(bytes, size, &words);
                msNames += Ms(tn);
            }
        }
        ++blobs[*index].refs;
        return true;
    };
    for (const InputFile& in : inputs) {
        if (cancelled()) return abandon("cancelled", true);
        progress(doneInput, in.name);
        const std::wstring path = in.dir + L"\\" + Widen(in.name);
        auto tr = std::chrono::steady_clock::now();
        std::vector<uint8_t> data;
        if (!ReadFileAll(path, &data)) return abandon("can't read " + Narrow(path), false);
        msRead += Ms(tr);
        doneInput += in.size;
        ContentHash whole;
        if (in.sound) {   // a level sound cache: one blob per chunk (docs/internal/asset-store.md "Sound caches")
            Store::SoundManifest sm;
            std::vector<SoundPiece> pieces;
            if (!format.parseSound(data.data(), data.size(), sm.header, &pieces)) {
                ++st.skippedFiles;
                Log(options, "  " + in.name + ": not a sound cache - skipped");
                stampInputs.push_back(Fmt("input %s %llu %s skipped", in.name.c_str(),   // (still an input: the stamp
                                          static_cast<unsigned long long>(in.size), Hex64(in.mtime).c_str()));   // lists it)
                continue;
            }
            auto th = std::chrono::steady_clock::now();
            HashContent(data.data(), data.size(), &whole);
            sm.cache = Lower(in.name);
            for (SoundPiece p : pieces) {
                uint32_t index;
                if (!addBlob(data.data() + p.offset, p.size, false, &index))
                    return abandon("SHA-256 (Windows CNG) unavailable", false);
                if (index + 1 == blobs.size() && blobs[index].refs == 1) {   // new content
                    ++st.soundBlobs;
                    st.soundStoreBytes += p.size;
                }
                p.offset = index;
                sm.pieces.push_back(p);
                ++st.soundPieces;
            }
            msHash += Ms(th);
            ++st.soundCaches;
            st.soundInputBytes += data.size();
            stampInputs.push_back(Fmt("input %s %llu %s %s", in.name.c_str(), static_cast<unsigned long long>(in.size),
                                      Hex64(in.mtime).c_str(), whole.Hex().c_str()));
            soundMans.push_back(std::move(sm));
            continue;
        }
        std::vector<ArchiveEntry> entries;
        uint32_t bad = 0;
        if (!format.parse(data.data(), data.size(), &entries, &bad)) {
            ++st.skippedFiles;
            Log(options, "  " + in.name + ": not an archive - skipped");
            stampInputs.push_back(Fmt("input %s %llu %s skipped", in.name.c_str(), static_cast<unsigned long long>(in.size),
                                      Hex64(in.mtime).c_str()));
            continue;
        }
        if (bad) Log(options, Fmt("  [warn] %s: %u entries point outside the file - left out", in.name.c_str(), bad));
        st.badEntries += bad;
        ++st.archives;
        auto th = std::chrono::steady_clock::now();
        HashContent(data.data(), data.size(), &whole);
        ManRec man;
        man.archive = Lower(in.name);
        uint32_t n = 0;
        for (const ArchiveEntry& e : entries) {
            if ((++n & 511) == 0 && cancelled()) return abandon("cancelled", true);
            uint32_t index;
            if (!addBlob(data.data() + e.offset, e.size, true, &index))
                return abandon("SHA-256 (Windows CNG) unavailable", false);
            man.entries.push_back({e.nameHash, index, kNoName});
            variants[e.nameHash][index].push_back(man.archive);
            st.inputBytes += e.size;
            ++st.files;
        }
        msHash += Ms(th);
        stampInputs.push_back(Fmt("input %s %llu %s %s", in.name.c_str(), static_cast<unsigned long long>(in.size),
                                  Hex64(in.mtime).c_str(), whole.Hex().c_str()));
        manifests.push_back(std::move(man));
    }
    if (manifests.empty()) return abandon("no archives (" + std::string(format.pattern) + ") in " + Narrow(options.gameDir), false);
    if (cancelled()) return abandon("cancelled", true);
    msHash -= std::min(msHash, msWrite + msNames);
    const uint64_t packSize = pack.Pos();
    if (!pack.Close()) return fail("writing store.pack failed (disk full?)");
    st.uniqueBlobs = static_cast<uint32_t>(blobs.size());
    st.storeBytes = packSize;
    st.msRead = msRead, st.msHash = msHash, st.msWrite = msWrite;

    // Names (optional): the dictionary only answers hashes that are in some manifest.
    std::unordered_map<uint32_t, std::string> names;
    if (options.names) {
        if (options.progress) options.progress(1000 - namesShare, "file names");
        auto tn = std::chrono::steady_clock::now();
        std::unordered_map<uint32_t, const char*> wanted;   // name hash -> content kind of (one of) its blobs
        for (const ManRec& m : manifests)
            for (const ManEntry& e : m.entries) wanted.emplace(e.nameHash, blobKinds[e.blob]);
        RecoverNames(options.gameDir, &words, wanted, format.nameHash, &names, [&](uint32_t done, uint32_t total) {
            if (options.progress && total)
                options.progress(1000 - namesShare + static_cast<uint32_t>(uint64_t(done) * namesShare / total), "file names");
            return !cancelled();
        });
        st.msNames = msNames + Ms(tn);
    }
    words.clear();
    if (cancelled()) return abandon("cancelled", true);

    // store.idx
    {
        Writer w;
        if (!w.Open(dir + L"\\store.idx.tmp")) return fail("can't write store.idx");
        IdxHeader h{};
        std::memcpy(h.magic, kIdxMagic, 8);
        h.format = kStoreFormat;
        h.count = static_cast<uint32_t>(blobs.size());
        h.packSize = packSize;
        w.Write(&h, sizeof h);
        std::vector<IdxEntry> out(blobs.size());
        for (size_t i = 0; i < blobs.size(); ++i) {
            std::memcpy(out[i].hash, blobs[i].hash.b, 32);
            out[i].offset = blobs[i].offset;
            out[i].size = blobs[i].size;
            out[i].refs = blobs[i].refs;
        }
        w.Write(out.data(), out.size() * sizeof(IdxEntry));
        if (!w.Close()) return fail("writing store.idx failed");
    }
    // manifests\<archive>.man (old ones removed first: an archive that is gone must not keep a manifest)
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"\\manifests\\*.man").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do DeleteFileW((dir + L"\\manifests\\" + fd.cFileName).c_str());
            while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    for (ManRec& m : manifests) {
        std::string nameBytes;
        for (ManEntry& e : m.entries) {
            auto it = names.find(e.nameHash);
            if (it == names.end()) continue;
            e.nameOffset = static_cast<uint32_t>(nameBytes.size());
            nameBytes += it->second;
            nameBytes += '\0';
            ++st.namesKnown;
        }
        ManHeader h{};
        std::memcpy(h.magic, kManMagic, 8);
        h.format = kStoreFormat;
        h.count = static_cast<uint32_t>(m.entries.size());
        h.namesBytes = static_cast<uint32_t>(nameBytes.size());
        strncpy_s(h.archive, m.archive.c_str(), _TRUNCATE);
        Writer w;
        if (!w.Open(dir + L"\\manifests\\" + Widen(m.archive) + L".man")) return fail("can't write a manifest");
        w.Write(&h, sizeof h);
        w.Write(m.entries.data(), m.entries.size() * sizeof(ManEntry));
        w.Write(nameBytes.data(), nameBytes.size());
        if (!w.Close()) return fail("writing a manifest failed");
    }
    // sound\<cache>.sman (old ones removed first)
    SHCreateDirectoryExW(nullptr, (dir + L"\\sound").c_str(), nullptr);
    DeleteMatching(dir + L"\\sound", L"*.sman");
    for (const Store::SoundManifest& m : soundMans)
        if (!WriteSoundManifest(dir + L"\\sound\\" + Widen(m.cache) + L".sman", m)) return fail("can't write a sound manifest");
    // Variants: one name, different bytes in different archives.
    std::vector<std::pair<uint32_t, const std::map<uint32_t, std::vector<std::string>>*>> multi;
    for (const auto& [hash, perBlob] : variants)
        if (perBlob.size() > 1) multi.push_back({hash, &perBlob});
    st.namesWithVariants = static_cast<uint32_t>(multi.size());
    std::sort(multi.begin(), multi.end(), [](const auto& a, const auto& b) {
        return a.second->size() != b.second->size() ? a.second->size() > b.second->size() : a.first < b.first;
    });
    std::string variantsText;
    // How different are they? Same-size variants are compared byte by byte with the first one (the shipped DS1 ones
    // differ only in a stale pointer the export tool left in the file).
    HANDLE packRead = CreateFileW((dir + L"\\store.pack.tmp").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                  OPEN_EXISTING, 0, nullptr);
    auto readBlob = [&](uint32_t blob, std::vector<uint8_t>* out) {
        out->resize(blobs[blob].size);
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(blobs[blob].offset);
        DWORD got = 0;
        return packRead != INVALID_HANDLE_VALUE && SetFilePointerEx(packRead, pos, nullptr, FILE_BEGIN) &&
               (out->empty() || (ReadFile(packRead, out->data(), blobs[blob].size, &got, nullptr) && got == out->size()));
    };
    for (const auto& [hash, perBlob] : multi) {
        auto it = names.find(hash);
        std::string line = Fmt("%08X %s %zu variants:", hash, it != names.end() ? it->second.c_str() : "?", perBlob->size());
        std::vector<uint8_t> first, other;
        const bool haveFirst = readBlob(perBlob->begin()->first, &first);
        for (const auto& [blob, archives] : *perBlob) {
            line += Fmt(" [%u bytes", blobs[blob].size);
            if (blob != perBlob->begin()->first && haveFirst && readBlob(blob, &other)) {
                if (other.size() == first.size()) {
                    size_t n = 0, at = 0;
                    for (size_t i = 0; i < other.size(); ++i)
                        if (other[i] != first[i]) at = n++ ? at : i;
                    line += Fmt(", %zu byte(s) differ from the first at +0x%zX", n, at);
                } else {
                    line += ", other size";
                }
            }
            line += ":";
            for (const std::string& a : archives) line += " " + a;
            line += "]";
        }
        if (st.variantExamples.size() < 12) st.variantExamples.push_back(line);
        variantsText += line + "\n";
    }
    if (packRead != INVALID_HANDLE_VALUE) CloseHandle(packRead);
    WriteText(dir + L"\\variants.txt", variantsText);

    if (!MoveFileExW((dir + L"\\store.pack.tmp").c_str(), (dir + L"\\store.pack").c_str(), MOVEFILE_REPLACE_EXISTING) ||
        !MoveFileExW((dir + L"\\store.idx.tmp").c_str(), (dir + L"\\store.idx").c_str(), MOVEFILE_REPLACE_EXISTING))
        return fail("can't move the new store into place (in use by a running game?)");

    st.msTotal = Ms(t0);
    std::string stamp = Fmt("pse-asset-store %u\nbuilder %s\ngame %s\n", kStoreFormat, kBuilderVersion, format.game);
    for (const std::string& s : stampInputs) stamp += s + "\n";
    stamp += Fmt("summary archives=%u files=%u blobs=%u input=%llu pack=%llu variants=%u names=%u sound=%u chunks=%u "
                 "soundinput=%llu soundblobs=%u soundbytes=%llu ms=%u\n",
                 st.archives, st.files, st.uniqueBlobs, static_cast<unsigned long long>(st.inputBytes),
                 static_cast<unsigned long long>(st.storeBytes), st.namesWithVariants, st.namesKnown, st.soundCaches,
                 st.soundPieces, static_cast<unsigned long long>(st.soundInputBytes), st.soundBlobs,
                 static_cast<unsigned long long>(st.soundStoreBytes), st.msTotal);
    if (!WriteText(dir + L"\\stamp.txt", stamp)) return fail("can't write stamp.txt");

    const double archiveStored = static_cast<double>(st.storeBytes) - static_cast<double>(st.soundStoreBytes);
    Log(options, Fmt("asset store: %u archives, %u files -> %u unique blobs; %.1f MB in, %.1f MB stored (%.1f MB saved by "
                     "deduplication); %u names with per-archive variants; %u of %u entries named; sound: %u caches, %u "
                     "chunks, %.1f MB -> %u unique chunks, %.1f MB; pack %.1f MB; %u ms (read %u, hash %u, write %u, names %u)",
                     st.archives, st.files, st.uniqueBlobs - st.soundBlobs, st.inputBytes / 1048576.0,
                     archiveStored / 1048576.0, (static_cast<double>(st.inputBytes) - archiveStored) / 1048576.0,
                     st.namesWithVariants, st.namesKnown, st.files, st.soundCaches, st.soundPieces,
                     st.soundInputBytes / 1048576.0, st.soundBlobs, st.soundStoreBytes / 1048576.0,
                     st.storeBytes / 1048576.0, st.msTotal, st.msRead, st.msHash, st.msWrite, st.msNames));
    if (options.progress) options.progress(1000, "done");
    if (statsOut) *statsOut = st;
    return true;
}

// ---- read ----
bool Store::Open(const std::wstring& storeDir, std::string* error) {
    Close();
    auto fail = [&](const std::string& e) {
        if (error) *error = e;
        Close();
        return false;
    };
    dir_ = storeDir;
    std::vector<uint8_t> idx;
    if (!ReadFileAll(storeDir + L"\\store.idx", &idx) || idx.size() < sizeof(IdxHeader)) return fail("no store.idx");
    IdxHeader h;
    std::memcpy(&h, idx.data(), sizeof h);
    if (std::memcmp(h.magic, kIdxMagic, 8) || h.format != kStoreFormat ||
        idx.size() != sizeof h + static_cast<size_t>(h.count) * sizeof(IdxEntry))
        return fail("store.idx is damaged or of another format");
    blobs_.resize(h.count);
    for (uint32_t i = 0; i < h.count; ++i) {
        IdxEntry e;
        std::memcpy(&e, idx.data() + sizeof h + i * sizeof(IdxEntry), sizeof e);
        if (e.offset + e.size > h.packSize) return fail("store.idx points outside store.pack");
        std::memcpy(blobs_[i].hash.b, e.hash, 32);
        blobs_[i].offset = e.offset;
        blobs_[i].size = e.size;
    }
    WIN32_FILE_ATTRIBUTE_DATA pa;
    if (!GetFileAttributesExW(PackPath().c_str(), GetFileExInfoStandard, &pa) ||
        ((static_cast<uint64_t>(pa.nFileSizeHigh) << 32) | pa.nFileSizeLow) != h.packSize)
        return fail("store.pack is missing or has the wrong size");

    WIN32_FIND_DATAW fd;
    HANDLE fh = FindFirstFileW((storeDir + L"\\manifests\\*.man").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            std::vector<uint8_t> raw;
            if (!ReadFileAll(storeDir + L"\\manifests\\" + fd.cFileName, &raw) || raw.size() < sizeof(ManHeader)) continue;
            ManHeader mh;
            std::memcpy(&mh, raw.data(), sizeof mh);
            const size_t need = sizeof mh + static_cast<size_t>(mh.count) * sizeof(ManEntry) + mh.namesBytes;
            if (std::memcmp(mh.magic, kManMagic, 8) || mh.format != kStoreFormat || raw.size() != need) continue;
            Manifest m;
            mh.archive[63] = 0;
            m.archive = Lower(mh.archive);
            const char* names = reinterpret_cast<const char*>(raw.data() + sizeof mh + mh.count * sizeof(ManEntry));
            bool ok = true;
            for (uint32_t i = 0; i < mh.count && ok; ++i) {
                ManEntry e;
                std::memcpy(&e, raw.data() + sizeof mh + i * sizeof(ManEntry), sizeof e);
                if (e.blob >= blobs_.size()) {
                    ok = false;
                    break;
                }
                m.entries.push_back({e.nameHash, e.blob, blobs_[e.blob].size});
                m.byHash.emplace(e.nameHash, e.blob);
                if (e.nameOffset != kNoName && e.nameOffset < mh.namesBytes)
                    m.names.emplace(e.nameHash, std::string(names + e.nameOffset,
                                                            strnlen(names + e.nameOffset, mh.namesBytes - e.nameOffset)));
            }
            if (!ok) return fail("manifest " + Narrow(fd.cFileName) + " points outside store.idx");
            byArchive_[m.archive] = manifests_.size();
            manifests_.push_back(std::move(m));
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    if (manifests_.empty()) return fail("no manifests");
    // Sound caches (sound\*.sman; a store without them simply has no sound part).
    fh = FindFirstFileW((storeDir + L"\\sound\\*.sman").c_str(), &fd);
    if (fh != INVALID_HANDLE_VALUE) {
        do {
            SoundManifest m;
            if (!ReadSoundManifest(storeDir + L"\\sound\\" + fd.cFileName, blobs_.size(), &m)) {
                FindClose(fh);
                return fail("sound manifest " + Narrow(fd.cFileName) + " is damaged");
            }
            sound_.push_back(std::move(m));
        } while (FindNextFileW(fh, &fd));
        FindClose(fh);
    }
    IndexSound();
    return true;
}

void Store::Close() {
    blobs_.clear();
    manifests_.clear();
    byArchive_.clear();
    sound_.clear();
    bySound_.clear();
    banks_.clear();
    samplesById_.clear();
}

const Store::Manifest* Store::Find(const std::string& archive) const {
    auto it = byArchive_.find(Lower(BaseName(archive)));
    return it == byArchive_.end() ? nullptr : &manifests_[it->second];
}

int32_t Store::Lookup(const std::string& archive, uint32_t nameHash) const {
    const Manifest* m = Find(archive);
    if (!m) return -1;
    auto it = m->byHash.find(nameHash);
    return it == m->byHash.end() ? -1 : static_cast<int32_t>(it->second);
}

std::string Store::NameOf(uint32_t nameHash) const {
    for (const Manifest& m : manifests_) {
        auto it = m.names.find(nameHash);
        if (it != m.names.end()) return it->second;
    }
    return {};
}

bool Store::Read(uint32_t blob, std::vector<uint8_t>* out) const {
    if (blob >= blobs_.size()) return false;
    HANDLE f = CreateFileW(PackPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER pos;
    pos.QuadPart = static_cast<LONGLONG>(blobs_[blob].offset);
    out->resize(blobs_[blob].size);
    DWORD got = 0;
    const bool ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) &&
                    (out->empty() || (ReadFile(f, out->data(), static_cast<DWORD>(out->size()), &got, nullptr) &&
                                      got == out->size()));
    CloseHandle(f);
    return ok;
}

}  // namespace pse::build
