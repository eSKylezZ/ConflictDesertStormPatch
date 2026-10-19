// pse.ids (pse/loader/ids.*, docs/sdk/reference/ids.md): the generated DS1 database (tier 1 + tier 2 against the
// original exe) and the resolve API on a synthetic database (tier 1).
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#include "exe_image.h"
#include "generated/pse_ids_ds1.h"
#include "ids.h"
#include "loader_fixture.h"
#include "pse/pse_ids_ds1.h"
#include "pse_test.h"

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ReadOriginal(uint32_t va, void* out, uint32_t len, void*) { return exe::OriginalBytes(va, out, len); }

// The mapped original exe as an ImageView (PE header fields from the mapped headers, bytes from the file).
bool OriginalImage(pse::ids::ImageView* out) {
    const auto dos = exe::Read<IMAGE_DOS_HEADER>(exe::kImageBase);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto nt = exe::Read<IMAGE_NT_HEADERS32>(exe::kImageBase + static_cast<uint32_t>(dos.e_lfanew));
    if (nt.Signature != IMAGE_NT_SIGNATURE) return false;
    out->timestamp = nt.FileHeader.TimeDateStamp;
    out->sizeOfImage = nt.OptionalHeader.SizeOfImage;
    out->read = &ReadOriginal;
    out->ctx = nullptr;
    return true;
}

const pse::ids::GameDef& Ds1() { return pse_ids_ds1::kGame; }

}  // namespace

// ---- the DS1 database (tier 1: the table itself) -------------------------------------------------------------------

PSE_TEST("ids: ds1 table - ids sorted + unique, names unique, kinds valid") {
    const auto& g = Ds1();
    PSE_REQUIRE(g.entryCount > 100);
    PSE_CHECK_STR(g.gameId, "ds1");
    std::set<std::string> names;
    for (uint32_t i = 0; i < g.entryCount; ++i) {
        const auto& e = g.entries[i];
        PSE_CHECK(e.id != 0);
        if (i) PSE_CHECK(e.id > g.entries[i - 1].id);
        PSE_CHECK(e.kind >= PSE_ID_KIND_FUNCTION && e.kind <= PSE_ID_KIND_DATA);
        PSE_REQUIRE(e.name && *e.name);
        PSE_CHECK(std::isalpha(static_cast<unsigned char>(e.name[0])));
        for (const char* c = e.name; *c; ++c) PSE_CHECK(std::isalnum(static_cast<unsigned char>(*c)) || *c == '_');
        if (!names.insert(Lower(e.name)).second) PSE_CHECK_STR(e.name, "(a name used twice)");
    }
}

PSE_TEST("ids: ds1 every published id still exists with its name (ids are never removed or renumbered)") {
    const auto& g = Ds1();
    PSE_REQUIRE(g.publishedCount > 0);
    std::set<uint32_t> published;
    for (uint32_t i = 0; i < g.publishedCount; ++i) {
        const auto& p = g.published[i];
        published.insert(p.id);
        const pse::ids::EntryDef* e = nullptr;
        for (uint32_t k = 0; k < g.entryCount; ++k)
            if (g.entries[k].id == p.id) e = &g.entries[k];
        PSE_REQUIRE(e);
        PSE_CHECK_STR(e->name, p.name);
    }
    PSE_CHECK_EQ(published.size(), static_cast<size_t>(g.publishedCount));   // no id published twice
}

PSE_TEST("ids: ds1 every address has a known id, a valid signature and lies inside the image") {
    const auto& g = Ds1();
    PSE_REQUIRE(g.buildCount >= 1);
    for (uint32_t b = 0; b < g.buildCount; ++b) {
        const auto& bd = g.builds[b];
        std::set<uint32_t> seen;
        for (uint32_t i = 0; i < bd.addressCount; ++i) {
            const auto& a = bd.addresses[i];
            PSE_CHECK(seen.insert(a.id).second);
            bool known = false;
            for (uint32_t k = 0; k < g.entryCount; ++k) known |= g.entries[k].id == a.id;
            PSE_CHECK(known);
            PSE_CHECK(a.va >= 0x400000 && a.va < 0x400000 + bd.sizeOfImage);
            if (a.checkVa) PSE_CHECK(pse::ids::ValidSignature(a.signature));
        }
    }
}

PSE_TEST("ids: ds1 public constants match the table") {
    const auto* ids = pse::ids::Interface();
    pse::ids::ResetForTests();
    pse::ids::ImageView none;   // no build: names / ids still answer
    PSE_REQUIRE(pse::ids::RegisterGame(Ds1(), none, 0) == PSE_OK);
    PSE_CHECK_EQ(ids->IdOf("ds1", "Soldier_TakeDamage"), static_cast<PSEId>(PSE_ID(ds1, Soldier_TakeDamage)));
    PSE_CHECK_EQ(ids->IdOf("ds1", "FrontEnd_SetState"), static_cast<PSEId>(PSE_ID(ds1, FrontEnd_SetState)));
    PSE_CHECK_EQ(ids->IdOf("ds1", "Renderer"), static_cast<PSEId>(PSE_ID(ds1, Renderer)));
    PSE_CHECK_STR(ids->Name("ds1", PSE_ID_ds1_Text_Lookup), "Text_Lookup");
    PSE_CHECK(PSE_IDS_DS1_DATABASE_VERSION == Ds1().databaseVersion);
    PSE_CHECK_EQ(ids->Resolve("ds1", PSE_ID_ds1_Text_Lookup), 0u);   // unknown build: nothing resolves
    pse::ids::ResetForTests();
}

// ---- tier 2: against the original exe ------------------------------------------------------------------------------

PSE_TEST("ids: ds1 every signature matches the original exe (Steam = GOG build)") {
    PSE_REQUIRE_EXE();
    pse::ids::ImageView image;
    PSE_REQUIRE(OriginalImage(&image));
    const auto& g = Ds1();
    const pse::ids::BuildDef* build = nullptr;
    for (uint32_t b = 0; b < g.buildCount; ++b)
        if (g.builds[b].timestamp == image.timestamp && g.builds[b].sizeOfImage == image.sizeOfImage) build = &g.builds[b];
    PSE_REQUIRE(build);
    PSE_CHECK_STR(build->name, "steam-gog-1.0");
    for (uint32_t i = 0; i < build->addressCount; ++i) {
        const auto& a = build->addresses[i];
        if (!a.checkVa) continue;
        if (!pse::ids::SignatureMatches(image, a.checkVa, a.signature)) {
            char what[96];
            std::snprintf(what, sizeof what, "id %u at 0x%08X: %s", a.id, a.checkVa, a.signature);
            PSE_CHECK_STR(what, "(matching signature)");
        }
        // Code ids lie in .text, the rest outside it.
        std::string section;
        PSE_CHECK(exe::SectionOf(a.va, &section));
        uint32_t kind = 0;
        for (uint32_t k = 0; k < g.entryCount; ++k)
            if (g.entries[k].id == a.id) kind = g.entries[k].kind;
        if (kind == PSE_ID_KIND_FUNCTION || kind == PSE_ID_KIND_SITE) PSE_CHECK_STR(section.c_str(), ".text");
        else PSE_CHECK(section != ".text");
    }

    pse::ids::ResetForTests();
    pse::ids::RegisterSummary sum;
    PSE_REQUIRE(pse::ids::RegisterGame(g, image, 0, &sum) == PSE_OK);
    PSE_CHECK_STR(sum.build.c_str(), "steam-gog-1.0");
    PSE_CHECK_EQ(sum.resolved, g.entryCount);
    PSE_CHECK_EQ(sum.mismatched, 0u);
    PSE_CHECK_EQ(sum.missing, 0u);
    pse::ids::ResetForTests();
}

PSE_TEST("ids: ds1 spot checks - ids resolve to the addresses the code uses") {
    PSE_REQUIRE_EXE();
    pse::ids::ImageView image;
    PSE_REQUIRE(OriginalImage(&image));
    pse::ids::ResetForTests();
    PSE_REQUIRE(pse::ids::RegisterGame(Ds1(), image, 0) == PSE_OK);
    const auto* ids = pse::ids::Interface();
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, Text_Lookup), 0x4F6730u);           // pse_game_ds1.cpp kLookup
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, FrontEnd_State), 0x617C18u);        // kFrontEndState
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, Main_DispatchMode), 0x60EC8Cu);     // kDispatchMode
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, Level_SceneName), 0x606880u);       // kLevelName
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, World), 0x63C948u);                 // kWorld
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, Text_Strings), 0x60EDCCu);          // kStrings
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, Input_Blocks), 0x60F5B8u);          // kBlocks
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, Window_Handle), 0x606A60u);         // kGameWindow
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds1, Soldier_TakeDamage), 0x424200u);
    PSE_CHECK_EQ(ids->ResolveByName(nullptr, "Site_GlyphBindCall"), 0x53CC9Bu);   // features.h kGlyphBindSite
    // Vtable slots hold the functions of the same name.
    PSE_CHECK_EQ(exe::Read<uint32_t>(PSE_RESOLVE(ids, ds1, Soldier_TakeDamageSlot)), PSE_RESOLVE(ids, ds1, Soldier_TakeDamage));
    PSE_CHECK_EQ(exe::Read<uint32_t>(PSE_RESOLVE(ids, ds1, Soldier_HealSlot)), PSE_RESOLVE(ids, ds1, Soldier_Heal));
    PSE_CHECK_EQ(exe::Read<uint32_t>(PSE_RESOLVE(ids, ds1, Vehicle_Vtable)), PSE_RESOLVE(ids, ds1, Vehicle_Update));
    // IAT slots name their imports.
    PSE_CHECK_STR(exe::ImportName(PSE_RESOLVE(ids, ds1, Iat_Direct3DCreate8)).c_str(), "D3D8.DLL!Direct3DCreate8");
    PSE_CHECK_STR(exe::ImportName(PSE_RESOLVE(ids, ds1, Iat_TimeGetTime)).c_str(), "WINMM.DLL!timeGetTime");
    // A call site calls the function of the same name.
    const uint32_t site = PSE_RESOLVE(ids, ds1, Site_FrameDispatchCall);
    PSE_CHECK_EQ(exe::Read<uint8_t>(site), 0xE8u);
    PSE_CHECK_EQ(site + 5 + exe::Read<uint32_t>(site + 1), PSE_RESOLVE(ids, ds1, Main_FrameDispatch));
    // A pure game function through its id.
    auto hash = exe::Cdecl<uint32_t>(PSE_RESOLVE(ids, ds1, Hash_Name), "WEAPONS.RFX");
    PSE_REQUIRE(hash.ok);
    PSE_CHECK_EQ(hash.value, 0x4475E8D7u);
    pse::ids::ResetForTests();
}

// ---- tier 1: the API on a synthetic database ------------------------------------------------------------------------

namespace {

// A 64 KB fake "image" at 0x10000 with known bytes.
struct FakeImage {
    std::vector<uint8_t> mem = std::vector<uint8_t>(0x10000, 0);
    static bool Read(uint32_t va, void* out, uint32_t len, void* ctx) {
        auto* self = static_cast<FakeImage*>(ctx);
        if (va < 0x10000 || va + len > 0x20000) return false;
        std::memcpy(out, self->mem.data() + (va - 0x10000), len);
        return true;
    }
    pse::ids::ImageView View(uint32_t timestamp, uint32_t size) {
        pse::ids::ImageView v;
        v.timestamp = timestamp;
        v.sizeOfImage = size;
        v.read = &Read;
        v.ctx = this;
        return v;
    }
};

const pse::ids::EntryDef kEntries[] = {
    {1, "Alpha_Fn", PSE_ID_KIND_FUNCTION},
    {2, "Beta_Global", PSE_ID_KIND_GLOBAL},
    {5, "Gamma_Site", PSE_ID_KIND_SITE},
    {9, "Delta_OnlyInB", PSE_ID_KIND_DATA},
    {10, "Eps_Unchecked", PSE_ID_KIND_DATA},
};
// Build A: everything matches except Gamma; Build B (same header, other bytes) only Alpha.
const pse::ids::AddressDef kAddrA[] = {
    {1, 0x11000, 0x11000, "5589E583EC??"},
    {2, 0x12000, 0x11100, "A1??????00C3"},   // reference window elsewhere
    {5, 0x11200, 0x11200, "E8DEADBEEF"},     // won't match
    {10, 0x13000, 0, ""},                    // not verified
};
const pse::ids::AddressDef kAddrB[] = {
    {1, 0x11000, 0x11000, "5589E5"},
    {9, 0x14000, 0x14000, "FFFFFFFF"},
};
const pse::ids::BuildDef kBuilds[] = {
    {"test-a", "first", 0x1234, 0x10000, "-", kAddrA, 4},
    {"test-b", "second", 0x1234, 0x10000, "-", kAddrB, 2},
    {"test-c", "other header", 0x9999, 0x10000, "-", kAddrB, 2},
};
const pse::ids::PublishedDef kPublished[] = {{1, "Alpha_Fn"}};
const pse::ids::GameDef kTestGame = {sizeof(pse::ids::GameDef), "tst", "Test Game", 7, kEntries, 5, kBuilds, 3,
                                     kPublished, 1};

void FillFake(FakeImage& img) {
    const uint8_t fn[] = {0x55, 0x89, 0xE5, 0x83, 0xEC, 0x10};
    std::memcpy(img.mem.data() + 0x1000, fn, sizeof fn);
    const uint8_t ref[] = {0xA1, 0x00, 0x20, 0x01, 0x00, 0xC3};
    std::memcpy(img.mem.data() + 0x1100, ref, sizeof ref);
    const uint8_t call[] = {0xE8, 0x00, 0x00, 0x00, 0x00};
    std::memcpy(img.mem.data() + 0x1200, call, sizeof call);
}

}  // namespace

PSE_TEST("ids: signature parser + matcher (wildcards, bad input, unreadable memory)") {
    FakeImage img;
    FillFake(img);
    auto v = img.View(0x1234, 0x10000);
    PSE_CHECK(pse::ids::SignatureMatches(v, 0x11000, "5589E583EC10"));
    PSE_CHECK(pse::ids::SignatureMatches(v, 0x11000, "55????83"));
    PSE_CHECK(pse::ids::SignatureMatches(v, 0x11000, "5589e5"));   // lower case
    PSE_CHECK(!pse::ids::SignatureMatches(v, 0x11000, "5589E584"));
    PSE_CHECK(!pse::ids::SignatureMatches(v, 0x1FFFE, "00000000"));   // runs past the readable range
    PSE_CHECK(!pse::ids::ValidSignature(""));
    PSE_CHECK(!pse::ids::ValidSignature("5"));
    PSE_CHECK(!pse::ids::ValidSignature("ZZ"));
    PSE_CHECK(!pse::ids::ValidSignature("????"));                  // needs one fixed byte
    PSE_CHECK(!pse::ids::ValidSignature("000102030405060708090A0B0C0D0E0F10"));   // 17 bytes
    PSE_CHECK(pse::ids::ValidSignature("000102030405060708090A0B0C0D0E0F"));
}

PSE_TEST("ids: resolve API - build pick, mismatch -> 0 + one log line, unchecked, names, info") {
    pse_fixture::Fresh();
    pse::ids::ResetForTests();
    FakeImage img;
    FillFake(img);
    pse::ids::RegisterSummary sum;
    PSE_REQUIRE(pse::ids::RegisterGame(kTestGame, img.View(0x1234, 0x10000), pse::FindPlugin("pse"), &sum) == PSE_OK);
    PSE_CHECK_STR(sum.build.c_str(), "test-a");   // A matches 2 signatures, B only 1: same header, A wins
    PSE_CHECK_EQ(sum.ids, 5u);
    PSE_CHECK_EQ(sum.resolved, 3u);       // Alpha, Beta, Eps (unchecked)
    PSE_CHECK_EQ(sum.mismatched, 1u);     // Gamma
    PSE_CHECK_EQ(sum.missing, 1u);        // Delta has no address in A
    PSE_CHECK(pse_fixture::AnyLineContains("tst id 5 Gamma_Site"));

    // Served through the loader like any interface.
    const PSELoadInterface* li = pse::LoadInterfaceFor(pse_fixture::AddPlugin("test.ids"));
    PSE_REQUIRE(li);
    const auto* ids = static_cast<const PSEIdsInterface*>(li->Query(li->plugin, PSE_IDS_INTERFACE, 1));
    PSE_REQUIRE(ids);
    PSE_CHECK(ids == pse::ids::Interface());
    PSE_CHECK(li->Query(li->plugin, PSE_IDS_INTERFACE, 2) == nullptr);

    PSE_CHECK_EQ(ids->Resolve("tst", 1), 0x11000u);
    PSE_CHECK_EQ(ids->Resolve("tst", 2), 0x12000u);
    PSE_CHECK_EQ(ids->Resolve("tst", 5), 0u);
    PSE_CHECK_EQ(ids->Resolve("tst", 9), 0u);
    PSE_CHECK_EQ(ids->Resolve("tst", 10), 0x13000u);
    PSE_CHECK_EQ(ids->Resolve("tst", 3), 0u);        // unknown id
    PSE_CHECK_EQ(ids->Resolve("nope", 1), 0u);       // unknown game
    PSE_CHECK_EQ(ids->ResolveByName("tst", "Beta_Global"), 0x12000u);
    PSE_CHECK_EQ(ids->ResolveByName("tst", "beta_global"), 0u);   // names are case-sensitive
    PSE_CHECK_STR(ids->Name("tst", 9), "Delta_OnlyInB");          // names answer even without an address
    PSE_CHECK(ids->Name("tst", 4) == nullptr);
    PSE_CHECK_EQ(ids->IdOf("tst", "Gamma_Site"), 5u);
    PSE_CHECK_EQ(ids->IdOf("tst", "Missing"), 0u);
    // NULL / "" = the running game = the first one registered.
    PSE_CHECK_EQ(ids->Resolve(nullptr, 1), 0x11000u);
    PSE_CHECK_EQ(ids->Resolve("", 1), 0x11000u);

    PSEIdInfo info{};
    info.size = sizeof info;
    PSE_CHECK(ids->Info("tst", 5, &info) == PSE_OK);
    PSE_CHECK_EQ(info.status, PSE_ID_MISMATCH);
    PSE_CHECK_EQ(info.kind, PSE_ID_KIND_SITE);
    PSE_CHECK_EQ(info.va, 0u);
    PSE_CHECK(ids->Info("tst", 9, &info) == PSE_OK);
    PSE_CHECK_EQ(info.status, PSE_ID_NOT_IN_BUILD);
    PSE_CHECK(ids->Info("tst", 1, &info) == PSE_OK);
    PSE_CHECK_EQ(info.status, PSE_ID_OK);
    PSE_CHECK_STR(info.name, "Alpha_Fn");
    PSE_CHECK(ids->Info("tst", 3, &info) == PSE_E_NOT_FOUND);
    PSE_CHECK_EQ(info.status, PSE_ID_UNKNOWN);
    PSE_CHECK(info.name == nullptr);
    PSEIdInfo small{};
    small.size = 4;
    PSE_CHECK(ids->Info("tst", 1, &small) == PSE_E_INVALID_ARG);

    std::vector<uint32_t> listed;
    for (uint32_t i = 0;; ++i) {
        PSEIdInfo e{};
        e.size = sizeof e;
        if (ids->InfoAt("tst", i, &e) != PSE_OK) break;
        listed.push_back(e.id);
    }
    PSE_CHECK(listed == (std::vector<uint32_t>{1, 2, 5, 9, 10}));

    PSEIdBuildInfo b{};
    b.size = sizeof b;
    PSE_CHECK(ids->Build("tst", &b) == PSE_OK);
    PSE_CHECK_STR(b.build, "test-a");
    PSE_CHECK_STR(b.gameName, "Test Game");
    PSE_CHECK_EQ(b.databaseVersion, 7u);
    PSE_CHECK_EQ(b.idCount, 5u);
    PSE_CHECK_EQ(b.resolvedCount, 3u);
    PSE_CHECK_EQ(b.mismatchCount, 1u);
    PSE_CHECK_EQ(b.buildCount, 3u);
    PSE_CHECK_EQ(b.timestamp, 0x1234u);
    PSE_CHECK(ids->Build("nope", &b) == PSE_E_NOT_FOUND);
    pse::ids::ResetForTests();
}

PSE_TEST("ids: unknown build -> every id 0, one log line; re-registering replaces; bad tables refused") {
    pse_fixture::Fresh();
    pse::ids::ResetForTests();
    FakeImage img;
    FillFake(img);
    pse::ids::RegisterSummary sum;
    PSE_REQUIRE(pse::ids::RegisterGame(kTestGame, img.View(0x5555, 0x10000), pse::FindPlugin("pse"), &sum) == PSE_OK);
    PSE_CHECK_STR(sum.build.c_str(), "");
    PSE_CHECK_EQ(sum.resolved, 0u);
    PSE_CHECK(pse_fixture::AnyLineContains("tst: unknown exe"));
    const auto* ids = pse::ids::Interface();
    PSE_CHECK_EQ(ids->Resolve("tst", 1), 0u);
    PSE_CHECK_STR(ids->Name("tst", 1), "Alpha_Fn");
    PSEIdInfo info{};
    info.size = sizeof info;
    PSE_CHECK(ids->Info("tst", 1, &info) == PSE_OK);
    PSE_CHECK_EQ(info.status, PSE_ID_UNKNOWN_BUILD);

    // The other header -> build C (B's addresses).
    PSE_REQUIRE(pse::ids::RegisterGame(kTestGame, img.View(0x9999, 0x10000), pse::FindPlugin("pse"), &sum) == PSE_OK);
    PSE_CHECK_STR(sum.build.c_str(), "test-c");
    PSE_CHECK_EQ(ids->Resolve("tst", 1), 0x11000u);
    PSE_CHECK_EQ(ids->Resolve("tst", 9), 0u);   // FFFFFFFF doesn't match the zero bytes

    // A reader that can't read anything: signatures fail, never a fault.
    pse::ids::ImageView blind;
    blind.timestamp = 0x9999;
    blind.sizeOfImage = 0x10000;
    blind.read = [](uint32_t, void*, uint32_t, void*) { return false; };
    PSE_REQUIRE(pse::ids::RegisterGame(kTestGame, blind, pse::FindPlugin("pse"), &sum) == PSE_OK);
    PSE_CHECK_EQ(sum.resolved, 0u);
    PSE_CHECK_EQ(sum.mismatched, 2u);

    pse::ids::GameDef bad = kTestGame;
    bad.size = 4;
    PSE_CHECK(pse::ids::RegisterGame(bad, blind, 0) == PSE_E_INVALID_ARG);
    const pse::ids::EntryDef unsorted[] = {{2, "B", 1}, {1, "A", 1}};
    bad = kTestGame;
    bad.entries = unsorted;
    bad.entryCount = 2;
    PSE_CHECK(pse::ids::RegisterGame(bad, blind, 0) == PSE_E_INVALID_ARG);
    const pse::ids::EntryDef zero[] = {{0, "Z", 1}};
    bad.entries = zero;
    bad.entryCount = 1;
    PSE_CHECK(pse::ids::RegisterGame(bad, blind, 0) == PSE_E_INVALID_ARG);
    pse::ids::ResetForTests();
}

PSE_TEST("ids: the test process's own image registers as an unknown build without faulting") {
    pse_fixture::Fresh();
    pse::ids::ResetForTests();
    pse::ids::ImageView self;
    PSE_REQUIRE(pse::ids::ProcessImage(&self));
    PSE_CHECK(self.timestamp != 0);
    pse::ids::RegisterSummary sum;
    PSE_REQUIRE(pse::ids::RegisterGameForProcess(Ds1(), pse::FindPlugin("pse"), &sum) == PSE_OK);
    PSE_CHECK_STR(sum.build.c_str(), "");   // pse_tests.exe is no DesertStorm.exe
    PSE_CHECK_EQ(pse::ids::Interface()->Resolve("ds1", PSE_ID_ds1_World), 0u);
    pse::ids::ResetForTests();
}
