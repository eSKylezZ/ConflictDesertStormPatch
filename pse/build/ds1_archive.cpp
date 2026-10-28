// DS1's archive format (the part of §12's builder the DS1 game layer supplies): *.dat = an index of 12-byte entries
// {u32 name hash, u32 offset, u32 size} from offset 0 up to the first entry's data (0x8000 in every shipped archive),
// all-zero entry = end; the data follows. Only name hashes are stored. The game reads them with FUN_004bd210 (index)
// and FUN_004bd5c0 (lookup by hash); the hash is FUN_004b9a10 over the upper-cased bare name.
#include <cstring>

#include "pse_build.h"

namespace pse::build {

uint32_t Ds1NameHash(const char* name) {
    if (!name) return 0;
    const char* base = name;
    for (const char* p = name; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    uint32_t x = 1;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(base); *p; ++p) {
        unsigned char c = *p;
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char>(c - 32);
        for (int b = 0; b < 8; ++b) {
            const uint32_t in = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1;
            x = x << 1 | in;
        }
    }
    return x;
}

bool Ds1ParseArchive(const uint8_t* data, size_t size, std::vector<ArchiveEntry>* out, uint32_t* bad) {
    out->clear();
    if (bad) *bad = 0;
    if (!data || size < 12) return false;
    uint32_t first;
    std::memcpy(&first, data + 4, 4);
    if (first < 12 || first > size) return false;
    uint32_t badCount = 0;
    for (size_t p = 0; p + 12 <= first; p += 12) {
        ArchiveEntry e;
        std::memcpy(&e, data + p, 12);
        if (!e.nameHash && !e.offset && !e.size) break;
        if (e.offset < first || static_cast<uint64_t>(e.offset) + e.size > size) {
            ++badCount;
            continue;
        }
        out->push_back(e);
    }
    if (bad) *bad = badCount;
    // Something else that happens to end in .dat: no usable entry, or more broken entries than good ones.
    return !out->empty() && badCount <= out->size();
}

const ArchiveFormat& Ds1Archives() {
    static const ArchiveFormat f = {"ds1", &Ds1NameHash, &Ds1ParseArchive, "*.dat", "*.sch", &Ds1ParseSoundCache};
    return f;
}

}  // namespace pse::build
