// Tier 2: pure game functions called inside the mapped DesertStorm.exe (never run as a program). Expected values
// come from the ModKit's Python versions (ConflictDesertStormModKit/conflict_ds_tools: evo.archive_hash,
// evo_write.texture_hash, sound.elf_hash) and from addresses noted in CLAUDE.md.
#include <cctype>
#include <cstring>
#include <string>

#include "exe_image.h"
#include "pse_test.h"

namespace {
constexpr uint32_t kArchiveHash = 0x4B9A10;  // cdecl uint32(const char* name) - LFSR over the bytes as given
constexpr uint32_t kTextureHash = 0x4B9AB0;  // cdecl uint32(const char* name) - same LFSR over tolower(name)
constexpr uint32_t kElfHash = 0x4B9BF0;      // cdecl uint32(const char* data, int length)
constexpr uint32_t kGameClock = 0x4BA370;    // cdecl int() - calls QueryPerformanceCounter first

// Reference LFSR (CLAUDE.md "Game data archives"): start 1, per bit LSB first, in = bit^h0^h1^h21^h31.
uint32_t Lfsr(const std::string& s) {
    uint32_t h = 1;
    for (unsigned char c : s)
        for (int b = 0; b < 8; ++b) {
            uint32_t in = ((c >> b) ^ h ^ (h >> 1) ^ (h >> 21) ^ (h >> 31)) & 1;
            h = (h << 1) | in;
        }
    return h;
}
uint32_t Elf(const std::string& s) {
    uint32_t h = 0;
    for (unsigned char c : s) {
        h = (h << 4) + c;
        if (uint32_t g = h & 0xF0000000) h = (h & 0x0FFFFFFF) ^ (g >> 24);
    }
    return h;
}

uint32_t GameArchiveHash(const char* s) {
    auto r = exe::Cdecl<uint32_t>(kArchiveHash, s);
    if (!r.ok) ::pse_test::Fail(__FILE__, __LINE__, std::string("archive hash faulted: ") + r.fault.Describe());
    return r.value;
}
}  // namespace

PSE_TEST("game: archive name hash FUN_004b9a10 (ModKit evo.archive_hash)") {
    PSE_REQUIRE_EXE();
    struct {
        const char* name;
        uint32_t hash;
    } cases[] = {
        {"WEAPONS.RFX", 0x4475E8D7},        // CLAUDE.md
        {"MISSION1.RMD", 0x8AE3BFA2},       // archive entry _8AE3BFA2
        {"MISSION1.QTD", 0x8A55A97A},       // archive entry _8A55A97A
        {"HERO01_ARMSTRONG.EVO", 0x9F4A9D59},
        {"DEFAULT.SPL", 0x21294B76},
        {"A", 0x16E},
        {"", 0x1},
        {"switch_0", 0xE7AF852A},           // scene name hashes are taken over the name as written
    };
    for (const auto& c : cases) PSE_CHECK_EQ(GameArchiveHash(c.name), c.hash);
}

PSE_TEST("game: archive hash agrees with the reference LFSR on many names") {
    PSE_REQUIRE_EXE();
    uint32_t seed = 12345;
    for (int i = 0; i < 2000; ++i) {
        std::string s;
        seed = seed * 1103515245 + 12345;
        int len = static_cast<int>(seed >> 16) % 40;
        for (int k = 0; k < len; ++k) {
            seed = seed * 1103515245 + 12345;
            s += static_cast<char>(1 + (seed >> 16) % 255);
        }
        uint32_t game = GameArchiveHash(s.c_str());
        if (game != Lfsr(s)) {
            PSE_CHECK_EQ(game, Lfsr(s));
            break;
        }
    }
}

PSE_TEST("game: lower-case texture / catalog hash FUN_004b9ab0 (ModKit evo_write.texture_hash)") {
    PSE_REQUIRE_EXE();
    struct {
        const char* name;
        uint32_t hash;
    } cases[] = {
        {"M16A2", 0x668CC3AF},
        {"HERO01_UK_01", 0x94C9076A},
        {"switch_0", 0xE7AF852A},
        {"Pivotal Games", 0x5900C644},
        {"mixedCase.DDS", 0x69876244},
    };
    for (const auto& c : cases) {
        auto r = exe::Cdecl<uint32_t>(kTextureHash, c.name);
        PSE_REQUIRE(r.ok);
        PSE_CHECK_EQ(r.value, c.hash);
        std::string lower = c.name;
        for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        PSE_CHECK_EQ(r.value, Lfsr(lower));
    }
}

PSE_TEST("game: ELF hash FUN_004b9bf0 (ModKit sound.elf_hash)") {
    PSE_REQUIRE_EXE();
    struct {
        const char* name;
        uint32_t hash;
    } cases[] = {
        {"MP5SILENCEDSUBMG.sbk", 0x08C824DB},
        {"Bradley.sbk", 0x0283DE3B},
        {"SAW-SILENCED.sbk", 0x05F0D90B},
        {"x", 0x78},
    };
    for (const auto& c : cases) {
        auto r = exe::Cdecl<uint32_t>(kElfHash, c.name, static_cast<int>(std::strlen(c.name)));
        PSE_REQUIRE(r.ok);
        PSE_CHECK_EQ(r.value, c.hash);
        PSE_CHECK_EQ(r.value, Elf(c.name));
    }
    auto empty = exe::Cdecl<uint32_t>(kElfHash, "abc", 0);
    PSE_REQUIRE(empty.ok);
    PSE_CHECK_EQ(empty.value, 0u);
}

PSE_TEST("image: a game function that calls an import is reported, not a crash") {
    PSE_REQUIRE_EXE();
    auto r = exe::Cdecl<int>(kGameClock);
    PSE_CHECK(!r.ok);
    PSE_CHECK_EQ(r.fault.code, exe::kImportTrapCode);
    PSE_CHECK_STR(r.fault.import.c_str(), "KERNEL32.DLL!QueryPerformanceCounter");
    PSE_CHECK_EQ(r.fault.caller, 0x4BA37Eu);  // return address after `call [0x5D80F4]`
    PSE_CHECK_STR(exe::ImportName(0x5D80F4).c_str(), "KERNEL32.DLL!QueryPerformanceCounter");
    std::printf("    %s\n", r.fault.Describe().c_str());
    PSE_CHECK_EQ(GameArchiveHash("WEAPONS.RFX"), 0x4475E8D7u);  // still usable afterwards
}

PSE_TEST("image: an access violation inside a game function is reported") {
    PSE_REQUIRE_EXE();
    auto r = exe::Cdecl<uint32_t>(kArchiveHash, static_cast<const char*>(nullptr));
    PSE_CHECK(!r.ok);
    PSE_CHECK_EQ(r.fault.code, 0xC0000005u);
    PSE_CHECK_EQ(r.fault.dataAddress, 0u);
    PSE_CHECK_EQ(GameArchiveHash("WEAPONS.RFX"), 0x4475E8D7u);
}

PSE_TEST("image: mapped like the loader (headers, caves zero, Restore)") {
    PSE_REQUIRE_EXE();
    PSE_CHECK_EQ(exe::Read<uint16_t>(0x400000), 0x5A4Du);  // "MZ"
    PSE_CHECK(exe::ImportCount() > 50);
    PSE_CHECK_EQ(exe::Read<uint32_t>(0x5D7B00), 0u);  // HUD cave: beyond .text's raw data
    PSE_CHECK_EQ(exe::Read<uint32_t>(0x5E5CB0), 0u);  // .rdata cave
    PSE_CHECK_EQ(exe::Read<uint8_t>(0x4B9A10), 0x83u);
    *static_cast<uint32_t*>(exe::Ptr(0x5D7B00)) = 0x12345678;  // tests may write globals / caves ...
    exe::Restore();                                             // ... and put the file's bytes back
    PSE_CHECK_EQ(exe::Read<uint32_t>(0x5D7B00), 0u);
    std::printf("    mapped %s\n", exe::Path().c_str());
}
