// The asset store's sound part (docs/internal/asset-store.md "Sound caches"): DS1's level sound caches (<level>.sch) are
// stored chunk by chunk in the same content-addressed pack as the archive files. A cache = its 8-byte header + chunks
// {u32 tag, u32 size, data}: BANK (u32 key = ELF hash of "<NAME>.sbk", entries -> sample ids), PFSM (sample: i32
// language, u32 id, u32 size, u16 rate, u8 bits, u8 pad, data), PFST (stream: i32 language, u32 id, ...), IMUS. The
// shipped caches repeat most banks / samples (26 files, 128.6 MB -> 3 843 unique chunks, 39.6 MB); a (language, id)
// never has two contents and a bank key never two banks. Each cache gets a manifest (sound\<cache>.sman: its pieces
// in file order), so it rebuilds byte-identical, and any bank can be handed to any level with its samples.
#include <windows.h>

#include <algorithm>
#include <cstring>
#include <unordered_set>

#include "pse_build.h"

namespace pse::build {

namespace {
constexpr char kSndMagic[8] = {'P', 'S', 'E', 'S', 'N', 'D', '0', '1'};

#pragma pack(push, 1)
struct SndHeader {
    char magic[8];
    uint32_t format, count;
    uint8_t schHeader[8];
    char cache[64];
};
struct SndEntry {
    uint32_t tag, blob, a, b;
};
#pragma pack(pop)
static_assert(sizeof(SndHeader) == 88 && sizeof(SndEntry) == 16);

std::string LowerBase(const std::string& s) {
    const size_t p = s.find_last_of("\\/");
    std::string b = p == std::string::npos ? s : s.substr(p + 1);
    for (char& c : b) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return b;
}

// Every u32 at every byte offset of a bank chunk after its key (bank entries aren't 4-byte aligned) - the ids it may
// refer to (DesertStormFix's BorrowBank rule).
void BankRefs(const std::string& bank, std::unordered_set<uint32_t>* refs) {
    for (size_t o = 12; o + 4 <= bank.size(); ++o) {
        uint32_t v;
        std::memcpy(&v, bank.data() + o, 4);
        refs->insert(v);
    }
}
}  // namespace

uint32_t Ds1BankKey(const char* s) {
    uint32_t h = 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); p && *p; ++p) {
        h = (h << 4) + *p;
        if (const uint32_t g = h & 0xF0000000u) h = (h & 0x0FFFFFFFu) ^ (g >> 24);
    }
    return h;
}

bool Ds1ParseSoundCache(const uint8_t* data, size_t size, uint8_t header[8], std::vector<SoundPiece>* out) {
    out->clear();
    if (!data || size < 8 || std::memcmp(data, "SCH", 4) != 0) return false;
    std::memcpy(header, data, 8);
    size_t p = 8;
    while (p + 8 <= size) {
        uint32_t tag, len;
        std::memcpy(&tag, data + p, 4);
        std::memcpy(&len, data + p + 4, 4);
        if (len > size - p - 8) break;   // runs past the end: the rest is kept as a raw tail
        SoundPiece s{tag, static_cast<uint32_t>(p), len + 8, 0, 0};
        if (tag == kSoundTagBank && len >= 4) std::memcpy(&s.a, data + p + 8, 4);
        if ((tag == kSoundTagSample || tag == kSoundTagStream) && len >= 8) {
            std::memcpy(&s.a, data + p + 8, 4);
            std::memcpy(&s.b, data + p + 12, 4);
        }
        out->push_back(s);
        p += 8 + static_cast<size_t>(len);
    }
    if (p < size) out->push_back({0, static_cast<uint32_t>(p), static_cast<uint32_t>(size - p), 0, 0});
    return true;
}

bool WriteSoundManifest(const std::wstring& path, const Store::SoundManifest& m) {
    SndHeader h{};
    std::memcpy(h.magic, kSndMagic, 8);
    h.format = kStoreFormat;
    h.count = static_cast<uint32_t>(m.pieces.size());
    std::memcpy(h.schHeader, m.header, 8);
    strncpy_s(h.cache, m.cache.c_str(), _TRUNCATE);
    std::string bytes(reinterpret_cast<const char*>(&h), sizeof h);
    for (const SoundPiece& p : m.pieces) {
        const SndEntry e{p.tag, p.offset, p.a, p.b};
        bytes.append(reinterpret_cast<const char*>(&e), sizeof e);
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    const bool ok = WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &w, nullptr) && w == bytes.size();
    CloseHandle(f);
    return ok;
}

bool ReadSoundManifest(const std::wstring& path, size_t blobCount, Store::SoundManifest* out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string raw(GetFileSize(f, nullptr), '\0');
    DWORD got = 0;
    const bool read = raw.empty() || (ReadFile(f, raw.data(), static_cast<DWORD>(raw.size()), &got, nullptr) && got == raw.size());
    CloseHandle(f);
    if (!read || raw.size() < sizeof(SndHeader)) return false;
    SndHeader h;
    std::memcpy(&h, raw.data(), sizeof h);
    if (std::memcmp(h.magic, kSndMagic, 8) || h.format != kStoreFormat ||
        raw.size() != sizeof h + static_cast<size_t>(h.count) * sizeof(SndEntry))
        return false;
    h.cache[63] = 0;
    out->cache = LowerBase(h.cache);
    std::memcpy(out->header, h.schHeader, 8);
    out->pieces.clear();
    out->pieces.reserve(h.count);
    for (uint32_t i = 0; i < h.count; ++i) {
        SndEntry e;
        std::memcpy(&e, raw.data() + sizeof h + i * sizeof(SndEntry), sizeof e);
        if (e.blob >= blobCount) return false;
        out->pieces.push_back({e.tag, e.blob, 0, e.a, e.b});
    }
    return true;
}

// ---- Store: sound ----
void Store::IndexSound() {
    bySound_.clear();
    banks_.clear();
    samplesById_.clear();
    std::sort(sound_.begin(), sound_.end(), [](const SoundManifest& a, const SoundManifest& b) { return a.cache < b.cache; });
    std::unordered_set<uint32_t> seenBlob;
    for (size_t c = 0; c < sound_.size(); ++c) {
        SoundManifest& m = sound_[c];
        bySound_[m.cache] = c;
        for (size_t i = 0; i < m.pieces.size(); ++i) {
            SoundPiece& p = m.pieces[i];
            p.size = blobs_[p.offset].size;
            if (p.tag == kSoundTagBank) banks_.emplace(p.a, std::make_pair(static_cast<uint32_t>(c), static_cast<uint32_t>(i)));
            if ((p.tag == kSoundTagSample || p.tag == kSoundTagStream) && seenBlob.insert(p.offset).second)
                samplesById_[p.b].push_back(p.offset);
        }
    }
}

const Store::SoundManifest* Store::FindSound(const std::string& cache) const {
    auto it = bySound_.find(LowerBase(cache));
    return it == bySound_.end() ? nullptr : &sound_[it->second];
}

bool Store::ReadBlobs(const std::vector<uint32_t>& list, std::string* out) const {
    if (list.empty()) return true;
    HANDLE f = CreateFileW(PackPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    size_t total = 0;
    for (uint32_t b : list) {
        if (b >= blobs_.size()) {
            CloseHandle(f);
            return false;
        }
        total += blobs_[b].size;
    }
    out->reserve(out->size() + total);
    std::string span;
    bool ok = true;
    for (size_t i = 0; ok && i < list.size();) {
        // A run of blobs that follow each other in the pack (only alignment padding between them): one read.
        const uint64_t start = blobs_[list[i]].offset;
        uint64_t end = start + blobs_[list[i]].size;
        size_t j = i + 1;
        while (j < list.size() && blobs_[list[j]].offset >= end && blobs_[list[j]].offset - end < kPackAlign &&
               end - start < (8u << 20)) {
            end = blobs_[list[j]].offset + blobs_[list[j]].size;
            ++j;
        }
        span.resize(static_cast<size_t>(end - start));
        LARGE_INTEGER pos;
        pos.QuadPart = static_cast<LONGLONG>(start);
        DWORD got = 0;
        ok = SetFilePointerEx(f, pos, nullptr, FILE_BEGIN) &&
             (span.empty() || (ReadFile(f, span.data(), static_cast<DWORD>(span.size()), &got, nullptr) && got == span.size()));
        for (size_t k = i; ok && k < j; ++k)
            out->append(span.data() + (blobs_[list[k]].offset - start), blobs_[list[k]].size);
        i = j;
    }
    CloseHandle(f);
    return ok;
}

bool Store::RebuildSound(const std::string& cache, std::string* out) const {
    const SoundManifest* m = FindSound(cache);
    if (!m) return false;
    out->assign(reinterpret_cast<const char*>(m->header), 8);
    std::vector<uint32_t> list;
    list.reserve(m->pieces.size());
    for (const SoundPiece& p : m->pieces) list.push_back(p.offset);
    return ReadBlobs(list, out);
}

bool Store::HasSoundBank(uint32_t key, std::string* cache) const {
    auto it = banks_.find(key);
    if (it == banks_.end()) return false;
    if (cache) *cache = sound_[it->second.first].cache;
    return true;
}

bool Store::SoundBankPack(uint32_t key, std::string* out, std::string* cache) const {
    auto it = banks_.find(key);
    if (it == banks_.end()) return false;
    const SoundManifest& m = sound_[it->second.first];
    if (cache) *cache = m.cache;
    std::string bank;
    if (!ReadBlobs({m.pieces[it->second.second].offset}, &bank)) return false;
    std::unordered_set<uint32_t> refs;
    BankRefs(bank, &refs);
    std::vector<uint32_t> samples;
    for (uint32_t id : refs) {
        auto s = samplesById_.find(id);
        if (s != samplesById_.end()) samples.insert(samples.end(), s->second.begin(), s->second.end());
    }
    std::sort(samples.begin(), samples.end());   // pack order: neighbours read together, same output every time
    std::string body = bank;
    if (!ReadBlobs(samples, &body)) return false;
    out->assign("SCH\0", 4);
    const uint32_t size = static_cast<uint32_t>(body.size());
    out->append(reinterpret_cast<const char*>(&size), 4);
    out->append(body);
    return true;
}

}  // namespace pse::build
