// pse.game.ds1 (src/core/pse_game_ds1.cpp, docs/sdk/reference/game-ds1.md), compiled into this file: registration as
// the built-in plugin + interface, the pure parts (text hash, level names), and - on the mapped DesertStorm.exe - the
// memory reads with game globals written by the test. The text lookup itself needs a running game (live test
// tests/live/test_game_interface.py).
#include <windows.h>

#include <string>
#include <vector>

#include "games/ds1/pse_game_ds1.cpp"  // NOLINT: the unit under test (pse/games/ds1)
#include "exe_image.h"
#include "loader_fixture.h"
#include "pse_test.h"

using namespace pse_fixture;

namespace {
constexpr uint32_t kTextureHashFn = 0x4B9AB0;  // cdecl uint32(const char*) - the game's lower-case LFSR

const PSEGameDs1V1* G() { return pse_game_ds1::Table(); }

std::string Str(uint32_t (PSE_CALL* fn)(char*, uint32_t)) {
    char buf[256] = {};
    const uint32_t n = fn(buf, sizeof buf);
    if (n != strlen(buf)) return "!length " + std::to_string(n) + " vs " + std::to_string(strlen(buf));
    return buf;
}

void SetLevel(const char* name) {
    char* p = static_cast<char*>(exe::Ptr(0x606880));
    memset(p, 0, 64);
    strcpy_s(p, 64, name);
}

// Game globals written by a test, put back afterwards (exe::Restore resets every section).
struct ImageState {
    ~ImageState() {
        exe::Restore();
        pse_game_ds1::SetProviders({});
    }
};

int g_split = 0;
PSEDs1Online g_online{};
int FakeSplit() { return g_split; }
void FakeOnline(PSEDs1Online* out) {
    const uint32_t size = out->size;
    *out = g_online;
    out->size = size;
}
}  // namespace

// ---- tier 1 ----------------------------------------------------------------------------------------------------------

PSE_TEST("game ds1: table layout and version") {
    PSE_CHECK_EQ(G()->size, static_cast<uint32_t>(sizeof(PSEGameDs1V1)));
    PSE_CHECK_EQ(G()->version, 1u);
    // every slot filled (17 dwords: size, version, 15 functions)
    PSE_CHECK_EQ(sizeof(PSEGameDs1V1), 17 * sizeof(void*));
    const void* const* slots = reinterpret_cast<const void* const*>(G());
    for (size_t i = 2; i < sizeof(PSEGameDs1V1) / sizeof(void*); ++i) PSE_CHECK(slots[i] != nullptr);
}

PSE_TEST("game ds1: registered as built-in plugin pse.game.ds1 + interface") {
    Fresh();
    const PSEPluginHandle h = pse_game_ds1::Register();
    PSE_REQUIRE(h != 0);
    PSE_CHECK_EQ(pse::FindPlugin("pse.game.ds1"), h);
    const PSEPluginInfo* info = pse::PluginInfo(h);
    PSE_REQUIRE(info);
    PSE_CHECK_EQ(info->version, PSE_MAKE_VERSION(1, 0, 0));
    PSE_CHECK(info->flags & PSE_PLUGIN_INTERNAL);
    PSE_CHECK(info->flags & PSE_PLUGIN_CLIENT_ONLY);
    // A plugin finds it through Query (what an external plugin's PSEPluginLoad does) and through messaging.
    const PSEPluginHandle other = AddPlugin("test.user");
    const PSELoadInterface* load = pse::LoadInterfaceFor(other);
    PSE_REQUIRE(load);
    PSE_CHECK(load->Query(other, PSE_GAME_DS1_INTERFACE, 1) == G());
    PSE_CHECK(load->Query(other, PSE_GAME_DS1_INTERFACE, 2) == nullptr);
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(other, PSE_GAME_DS1_INTERFACE, 1) == G());
    // Again (a second Init path): same plugin, interface not duplicated.
    PSE_CHECK_EQ(pse_game_ds1::Register(), h);
    PSE_CHECK(load->Query(other, PSE_GAME_DS1_INTERFACE, 1) == G());
}

PSE_TEST("game ds1: manifests can require pse.game.ds1 >= 1.0 (and not >= 2.0)") {
    Fresh();
    PSE_REQUIRE(pse_game_ds1::Register());
    std::vector<pse::PluginCandidate> c(2);
    std::string error;
    PSE_REQUIRE(pse::ParseManifest("name = dsfix.discord\nrequires = pse.game.ds1 >= 1.0\n", &c[0].manifest, &error));
    PSE_REQUIRE(pse::ParseManifest("name = test.future\nrequires = pse.game.ds1 >= 2.0\n", &c[1].manifest, &error));
    c[0].version = c[1].version = PSE_MAKE_VERSION(1, 0, 0);
    const std::vector<size_t> order = pse::ResolvePluginOrder(c);
    PSE_CHECK_EQ(order.size(), 1u);
    PSE_CHECK(c[0].refusal.empty());
    PSE_CHECK(!c[1].refusal.empty());
}

PSE_TEST("game ds1: TextHash = catalog hashes of the old Discord title table") {
    struct {
        const char* key;
        uint32_t hash;
    } cases[] = {
        {"MISS11B_TITLE", 150235186u},  {"MISS11_TITLE", 3976419625u}, {"MISS12B_TITLE", 481082815u},
        {"MISS12_TITLE", 2484535804u},  {"MISS10_TITLE", 3169110448u}, {"MISS1_TITLE", 2836128679u},
        {"MISS2_TITLE", 3491740530u},   {"MISS3_TITLE", 2180525035u},  {"MISS4_TITLE", 3969226520u},
        {"MISS5B_TITLE", 2041233653u},  {"MISS5_TITLE", 3178366849u},  {"MISS6_TITLE", 3295043412u},
        {"MISS7_TITLE", 2508312525u},   {"MISS8_TITLE", 1926267181u},  {"MISS9_TITLE", 590811572u},
        {"TRAIN1_TITLE", 1029915587u},  {"TRAIN2_TITLE", 1148656406u}, {"TRAIN3_TITLE", 361794447u},
    };
    for (const auto& c : cases) PSE_CHECK_EQ(G()->TextHash(c.key), c.hash);
    PSE_CHECK_EQ(G()->TextHash("MISS1_TITLE"), 0xA90BE3A7u);
    PSE_CHECK_EQ(G()->TextHash("miss1_title"), G()->TextHash("MISS1_TITLE"));   // case-insensitive
    PSE_CHECK_EQ(G()->TextHash(""), 1u);
    PSE_CHECK_EQ(G()->TextHash(nullptr), 1u);
}

PSE_TEST("game ds1: level display names") {
    PSE_CHECK_STR(pse_game_ds1::LevelDisplayName("Mission4.env").c_str(), "Mission 4");
    PSE_CHECK_STR(pse_game_ds1::LevelDisplayName("Mission5a.env").c_str(), "Mission 5A");
    PSE_CHECK_STR(pse_game_ds1::LevelDisplayName("mission11b.env").c_str(), "Mission 11B");
    PSE_CHECK_STR(pse_game_ds1::LevelDisplayName("MP_MISSION2.env").c_str(), "Multiplayer mission 2");
    PSE_CHECK_STR(pse_game_ds1::LevelDisplayName("Training1.env").c_str(), "Training 1");
    PSE_CHECK_STR(pse_game_ds1::LevelDisplayName("ambush.env").c_str(), "ambush");
    PSE_CHECK_STR(pse_game_ds1::LevelDisplayName("").c_str(), "");
    // Without a text table (none outside the game) the title is the number part; menus have none.
    PSE_CHECK_STR(pse_game_ds1::LevelTitleUtf8("Mission5a.env").c_str(), "Mission 5A");
    PSE_CHECK_STR(pse_game_ds1::LevelTitleUtf8("FrontEnd.env").c_str(), "");
    PSE_CHECK_STR(pse_game_ds1::LevelTitleUtf8("intro.env").c_str(), "");
    PSE_CHECK_STR(pse_game_ds1::LevelTitleUtf8("Outro.env").c_str(), "");
}

PSE_TEST("game ds1: buffers, size fields and no game memory") {
    // Small buffers: always NUL-terminated, full length returned; bufLen 0 = size query.
    PSE_CHECK_EQ(CopyOut("Mission 4", nullptr, 0), 9u);
    char b[4] = {'x', 'x', 'x', 'x'};
    PSE_CHECK_EQ(CopyOut("Mission 4", b, sizeof b), 9u);
    PSE_CHECK_STR(b, "Mis");
    // Structs with a short size field are refused.
    PSEDs1Player p{};
    p.size = sizeof p - 4;
    PSE_CHECK_EQ(G()->GetLocalPlayer(0, &p), 0);
    PSEDs1Online o{};
    o.size = 4;
    PSE_CHECK_EQ(G()->GetOnline(&o), 0);
    PSE_CHECK_EQ(G()->GetLocalPlayer(0, nullptr), 0);
    if (exe::Mapped()) return;   // the rest is about reads without the game image (they must not crash)
    PSE_CHECK_EQ(G()->FrontEndState(), 0u);
    PSE_CHECK_EQ(G()->LevelKind(), static_cast<uint32_t>(PSE_DS1_LEVEL_NONE));
    PSE_CHECK_STR(Str(G()->LevelName).c_str(), "");
    PSE_CHECK_EQ(G()->InCutscene(), 0);
    PSE_CHECK(G()->GameWindow() == nullptr);
    char t[16] = {'x'};
    PSE_CHECK_EQ(G()->LookupText(0xA90BE3A7u, t, sizeof t), 0u);
    PSE_CHECK_STR(t, "");
}

// ---- tier 2: the mapped exe --------------------------------------------------------------------------------------

PSE_TEST("game ds1 image: TextHash = the game's FUN_004b9ab0") {
    PSE_REQUIRE_EXE();
    for (const char* key : {"MISS1_TITLE", "TRAIN3_TITLE", "SAVE_NAME", "PC_ESC_RETURN", "a", ""}) {
        auto r = exe::Cdecl<uint32_t>(kTextureHashFn, key);
        PSE_REQUIRE(r.ok);
        PSE_CHECK_EQ(G()->TextHash(key), r.value);
    }
}

PSE_TEST("game ds1 image: level name, kind, title, front-end state, cutscene bit, window") {
    PSE_REQUIRE_EXE();
    ImageState restore;
    PSE_CHECK_EQ(exe::Read<uint32_t>(0x60EDCC), 0u);   // no text table in the image: titles = number part
    struct {
        const char* level;
        PSEDs1LevelKind kind;
        const char* title;
    } cases[] = {
        {"Mission1.env", PSE_DS1_LEVEL_MISSION, "Mission 1"},
        {"Mission11b.env", PSE_DS1_LEVEL_MISSION, "Mission 11B"},
        {"Training2.env", PSE_DS1_LEVEL_MISSION, "Training 2"},
        {"MP_MISSION3.env", PSE_DS1_LEVEL_MISSION, "Multiplayer mission 3"},
        {"FrontEnd.env", PSE_DS1_LEVEL_FRONTEND, ""},
        {"intro.env", PSE_DS1_LEVEL_FRONTEND, ""},
        {"Outro.env", PSE_DS1_LEVEL_OUTRO, ""},
        {"", PSE_DS1_LEVEL_NONE, ""},
    };
    for (const auto& c : cases) {
        SetLevel(c.level);
        PSE_CHECK_STR(Str(G()->LevelName).c_str(), c.level);
        PSE_CHECK_EQ(G()->LevelKind(), static_cast<uint32_t>(c.kind));
        PSE_CHECK_STR(Str(G()->LevelTitle).c_str(), c.title);
    }
    *static_cast<uint32_t*>(exe::Ptr(0x617C18)) = 0xE;
    *static_cast<uint32_t*>(exe::Ptr(0x60EC8C)) = 1;
    PSE_CHECK_EQ(G()->FrontEndState(), 0xEu);
    PSE_CHECK_EQ(G()->DispatchMode(), 1u);

    std::vector<uint8_t> world(0x2000, 0);
    PSE_CHECK_EQ(G()->InCutscene(), 0);   // no world
    *static_cast<uint32_t*>(exe::Ptr(0x63C948)) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(world.data()));
    PSE_CHECK_EQ(G()->InCutscene(), 0);
    world[0x1B8C] = 0x03;
    PSE_CHECK_EQ(G()->InCutscene(), 1);
    world[0x1B8C] = 0x02;
    PSE_CHECK_EQ(G()->InCutscene(), 0);
    *static_cast<uint32_t*>(exe::Ptr(0x63C948)) = 0x10;   // bad pointer: guarded
    PSE_CHECK_EQ(G()->InCutscene(), 0);

    *static_cast<uint32_t*>(exe::Ptr(0x606A60)) = 0x12345;
    PSE_CHECK(G()->GameWindow() == reinterpret_cast<void*>(0x12345));
}

PSE_TEST("game ds1 image: local players (split screen, online, front end)") {
    PSE_REQUIRE_EXE();
    ImageState restore;
    pse_game_ds1::Providers prov;
    prov.splitScreenPlayers = &FakeSplit;
    prov.online = &FakeOnline;
    pse_game_ds1::SetProviders(prov);
    g_online = {};
    g_split = 0;

    // Two fake soldiers in blocks 0 and 1 (block 0x60F5B8 + i * 0x478, soldier +0x310; pos +0x30, health +0x54).
    std::vector<uint8_t> s0(0x100, 0), s1(0x100, 0);
    auto setSoldier = [](std::vector<uint8_t>& s, float x, float y, float z, float health) {
        const float pos[3] = {x, y, z};
        memcpy(&s[0x30], pos, sizeof pos);
        memcpy(&s[0x54], &health, 4);
    };
    setSoldier(s0, -12011.6f, 1612.0f, 23392.3f, 150.0f);
    setSoldier(s1, 100.0f, 200.0f, 300.0f, 75.5f);
    auto block = [](int i) { return static_cast<uint32_t*>(exe::Ptr(0x60F5B8 + i * 0x478 + 0x310)); };
    *block(0) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s0.data()));
    *block(1) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s1.data()));

    PSEDs1Player p{};
    p.size = sizeof p;
    SetLevel("FrontEnd.env");
    PSE_CHECK_EQ(G()->LocalPlayerCount(), 0u);   // menus: no players
    PSE_CHECK_EQ(G()->GetLocalPlayer(0, &p), 0);

    SetLevel("Mission1.env");
    PSE_CHECK_EQ(G()->LocalPlayerCount(), 1u);
    PSE_REQUIRE(G()->GetLocalPlayer(0, &p) == 1);
    PSE_CHECK_EQ(p.block, 0u);
    PSE_CHECK_EQ(p.soldier, *block(0));
    PSE_CHECK_EQ(p.position.x, -12011.6f);
    PSE_CHECK_EQ(p.position.z, 23392.3f);
    PSE_CHECK_EQ(p.health, 150.0f);
    PSE_CHECK_EQ(G()->GetLocalPlayer(1, &p), 0);   // only one player

    g_split = 2;
    PSE_CHECK_EQ(G()->SplitScreenPlayers(), 2u);
    PSE_CHECK_EQ(G()->LocalPlayerCount(), 2u);
    PSE_REQUIRE(G()->GetLocalPlayer(1, &p) == 1);
    PSE_CHECK_EQ(p.block, 1u);
    PSE_CHECK_EQ(p.health, 75.5f);
    *block(1) = 0;   // no soldier (cutscene)
    PSE_CHECK_EQ(G()->GetLocalPlayer(1, &p), 0);
    *block(1) = 0x20;   // stale pointer: guarded
    PSE_CHECK_EQ(G()->GetLocalPlayer(1, &p), 0);
    *block(1) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s1.data()));

    // Online joiner (player 2): one local player, block 1.
    g_split = 0;
    g_online.active = 1;
    g_online.role = PSE_DS1_ONLINE_JOINER;
    g_online.localPlayer = 2;
    g_online.players = 2;
    g_online.connectedPeers = 1;
    PSEDs1Online o{};
    o.size = sizeof o;
    PSE_CHECK_EQ(G()->GetOnline(&o), 1);
    PSE_CHECK_EQ(o.size, static_cast<uint32_t>(sizeof o));
    PSE_CHECK_EQ(o.role, static_cast<uint32_t>(PSE_DS1_ONLINE_JOINER));
    PSE_CHECK_EQ(o.players, 2u);
    PSE_CHECK_EQ(G()->LocalPlayerCount(), 1u);
    PSE_REQUIRE(G()->GetLocalPlayer(0, &p) == 1);
    PSE_CHECK_EQ(p.block, 1u);
    PSE_CHECK_EQ(G()->GetLocalPlayer(1, &p), 0);
}

PSE_TEST("game ds1 image: exe SHA-256 ignores the 4 GB flag") {
    PSE_REQUIRE_EXE();
    const std::wstring path(exe::Path().begin(), exe::Path().end());
    PSE_CHECK_STR(pse_game_ds1::ExeFileSha256(path.c_str()).c_str(), kKnownSha256);
    // A copy with Large Address Aware set hashes the same.
    const std::string copy = TempPath("laa.exe");
    PSE_REQUIRE(CopyFileA(exe::Path().c_str(), copy.c_str(), FALSE));
    {
        HANDLE f = CreateFileA(copy.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        PSE_REQUIRE(f != INVALID_HANDLE_VALUE);
        uint32_t lfanew = 0;
        DWORD n = 0;
        SetFilePointer(f, 0x3C, nullptr, FILE_BEGIN);
        ReadFile(f, &lfanew, 4, &n, nullptr);
        uint8_t ch = 0;
        SetFilePointer(f, static_cast<LONG>(lfanew + 4 + 18), nullptr, FILE_BEGIN);
        ReadFile(f, &ch, 1, &n, nullptr);
        ch |= 0x20;
        SetFilePointer(f, static_cast<LONG>(lfanew + 4 + 18), nullptr, FILE_BEGIN);
        WriteFile(f, &ch, 1, &n, nullptr);
        CloseHandle(f);
    }
    const std::wstring wcopy(copy.begin(), copy.end());
    PSE_CHECK_STR(pse_game_ds1::ExeFileSha256(wcopy.c_str()).c_str(), kKnownSha256);
    DeleteFileA(copy.c_str());
    PSE_CHECK_STR(pse_game_ds1::ExeFileSha256(L"C:\\no\\such\\file.exe").c_str(), "");
    // In pse_tests the running exe is pse_tests.exe: a hash, but not the known build.
    PSE_CHECK_EQ(strlen(G()->ExeSha256()), 64u);
    PSE_CHECK_EQ(G()->ExeKnown(), 0);
}
