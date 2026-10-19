// The Desert Storm II game layer (pse/games/ds2, docs/sdk/reference/game-ds2.md), compiled into this file where it is
// pure loader / memory code: tier 1 - detection (the built pse_ds2.dll), the ds2 ID database table, the co-save path
// rules + NEW_GAME, pse.game.ds2 registration; tier 2 - on the mapped original CDS II.exe (exe_image MapGame("ds2")):
// every database signature, the layer's event hooks (sites, kinds, priorities, prologues), the interface's memory
// reads, and the loader's start-up picking pse_ds2.dll over pse_ds1.dll (the loader is game-neutral).
#include <windows.h>

#include <cctype>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "exe_image.h"
#include "games/ds2/ds2_events.cpp"     // NOLINT: units under test (pse/games/ds2)
#include "games/ds2/ds2_saves.cpp"      // NOLINT
#include "games/ds2/pse_game_ds2.cpp"   // NOLINT
#include "generated/pse_ids_ds2.h"
#include "ids.h"
#include "loader_fixture.h"
#include "pse/pse_ids_ds2.h"
#include "pse_test.h"

using namespace pse_fixture;

namespace {
constexpr const char* kDs2Sha = "E9B46420DC00B13C3C48A9E9B7B3FDA29458D5EA83AC0D203F5B68FDD111542A";
constexpr const char* kDs1Sha = "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164";
constexpr const char* kSecuromSha = "44EF1E16170BAF3F981267C170F0671D8015CD9F8BD347B48C63B819978295EF";

const pse::ids::GameDef& Ds2() { return pse_ids_ds2::kGame; }

bool ReadOriginal(uint32_t va, void* out, uint32_t len, void*) { return exe::OriginalBytes(va, out, len); }
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

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
void Touch(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
std::wstring W(const std::string& s) { return std::wstring(s.begin(), s.end()); }
}  // namespace

// ---- detection (tier 1: the built DLL) -------------------------------------------------------------------------------

PSE_TEST("ds2 layer: pse_ds2.dll detects exactly the disc-free CDS II.exe, pse_ds1.dll doesn't") {
    HMODULE m = LoadLibraryA(PSE_GAME_LAYER_DS2_DLL);
    PSE_REQUIRE(m);
    auto info = reinterpret_cast<const PSEGameLayerInfo*>(GetProcAddress(m, "PSEGame_Info"));
    auto detect = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m, "PSEGame_Detect"));
    PSE_REQUIRE(info && detect && GetProcAddress(m, "PSEGame_Init"));
    PSE_CHECK_STR(info->gameId, "ds2");
    PSE_CHECK_STR(info->plugin->name, "pse.game.ds2");
    PSE_REQUIRE(info->hostPlugins && info->hostPlugins[0]);
    PSE_CHECK_STR(info->hostPlugins[0], "ds2fix");
    PSE_CHECK(info->hostPlugins[1] == nullptr);
    PSEGameDetectInfo d{sizeof d, kDs2Sha, 0x400000, 0x3F4B1A8A, L"CDS II.exe"};
    PSE_CHECK_EQ(detect(&d), 1);
    const std::string lower = Lower(kDs2Sha);
    d.exeSha256 = lower.c_str();
    PSE_CHECK_EQ(detect(&d), 1);
    d.exeSha256 = kSecuromSha;   // the retail SecuROM exe: same header, other code - refused
    PSE_CHECK_EQ(detect(&d), 0);
    d.exeSha256 = kDs1Sha;       // Desert Storm 1
    PSE_CHECK_EQ(detect(&d), 0);
    d.exeSha256 = kDs2Sha;
    d.imageBase = 0x10000000;
    PSE_CHECK_EQ(detect(&d), 0);
    d.exeSha256 = "";
    PSE_CHECK_EQ(detect(&d), 0);
    FreeLibrary(m);

    HMODULE m1 = LoadLibraryA(PSE_GAME_LAYER_DLL);   // and the DS1 layer says no to DS2
    PSE_REQUIRE(m1);
    auto detect1 = reinterpret_cast<PSEGameDetectFn>(GetProcAddress(m1, "PSEGame_Detect"));
    PSEGameDetectInfo d2{sizeof d2, kDs2Sha, 0x400000, 0x3F4B1A8A, L"CDS II.exe"};
    PSE_CHECK_EQ(detect1(&d2), 0);
    FreeLibrary(m1);
}

// ---- the ds2 ID database (tier 1) ------------------------------------------------------------------------------------

PSE_TEST("ids: ds2 table - ids sorted + unique, names unique, one build, every id has an address") {
    const auto& g = Ds2();
    PSE_CHECK_STR(g.gameId, "ds2");
    PSE_REQUIRE(g.entryCount >= 100);
    std::set<std::string> names;
    for (uint32_t i = 0; i < g.entryCount; ++i) {
        const auto& e = g.entries[i];
        if (i) PSE_CHECK(e.id > g.entries[i - 1].id);
        PSE_CHECK(e.kind >= PSE_ID_KIND_FUNCTION && e.kind <= PSE_ID_KIND_DATA);
        if (!names.insert(Lower(e.name)).second) PSE_CHECK_STR(e.name, "(a name used twice)");
    }
    PSE_REQUIRE(g.buildCount == 1u);
    PSE_CHECK_STR(g.builds[0].name, "nodisc-1.0");
    PSE_CHECK_EQ(g.builds[0].timestamp, 0x3F4B1A8Au);
    PSE_CHECK_EQ(g.builds[0].sizeOfImage, 0x5A6000u);
    PSE_CHECK_EQ(g.builds[0].addressCount, g.entryCount);
    for (uint32_t i = 0; i < g.publishedCount; ++i) {   // published ids still exist with their names
        bool found = false;
        for (uint32_t k = 0; k < g.entryCount; ++k)
            found = found || (g.entries[k].id == g.published[i].id && strcmp(g.entries[k].name, g.published[i].name) == 0);
        PSE_CHECK(found);
    }
    // Names shared with ds1 for the same job: one plugin source resolves the same name on both games.
    PSE_CHECK_EQ(PSE_ID_ds2_Main_FrameDispatch, 2u);
    PSE_CHECK_EQ(PSE_ID_ds2_Site_FrameDispatchCall, 47u);
    PSE_CHECK_EQ(PSE_ID_ds2_Iat_Direct3DCreate9, 105u);
}

// ---- co-save + NEW_GAME (tier 1) -------------------------------------------------------------------------------------

namespace {
struct Seen {
    std::vector<std::pair<uint32_t, std::string>> messages;
    int loads = 0, reverts = 0, saves = 0;
    std::string loaded;
};
Seen g_seen;
constexpr uint32_t kTag = PSE_TAG('D', 'S', '2', 'T');
void PSE_CALL OnMessage(const PSEMessage* m, void*) {
    const bool text = m->data && m->dataLen && m->type >= PSE_MSG_NEW_GAME && m->type <= PSE_MSG_LOAD_END;
    g_seen.messages.emplace_back(m->type, text ? static_cast<const char*>(m->data) : "");
}
void PSE_CALL OnSave(PSESaveWriter* w, void*) {
    ++g_seen.saves;
    pse::SerializationInterface()->WriteChunk(w, kTag, 1, "ds2", 4);
}
void PSE_CALL OnLoad(PSESaveReader* r, void*) {
    ++g_seen.loads;
    uint32_t tag, version, len;
    while (pse::SerializationInterface()->NextChunk(r, &tag, &version, &len)) {
        std::string b(len, '\0');
        pse::SerializationInterface()->Read(r, b.data(), len);
        g_seen.loaded = b.c_str();
    }
}
void PSE_CALL OnRevert(void*) { ++g_seen.reverts; }
int Count(uint32_t type) {
    int n = 0;
    for (const auto& m : g_seen.messages) n += m.first == type ? 1 : 0;
    return n;
}
void SetupSaves() {
    Fresh();
    ds2_saves::ResetForTests();
    g_seen = Seen{};
    ds2_saves::SetOwner(AddPlugin("pse.game.ds2"));
    const PSEPluginHandle p = AddPlugin("test.cosave");
    pse::MessagingInterface()->RegisterListener(p, "pse", &OnMessage, nullptr);
    pse::SerializationInterface()->SetCallbacks(p, &OnSave, &OnLoad, &OnRevert, nullptr);
}
}  // namespace

PSE_TEST("ds2 layer saves: DS2's path rules (save appends .sav, load replaces the extension)") {
    PSE_CHECK_STR(ds2_saves::SaveFileFor("C:\\Saves\\Bridge 1").c_str(), "C:\\Saves\\Bridge 1.sav");
    PSE_CHECK_STR(ds2_saves::SaveFileFor("C:\\Saves\\Bridge.sav").c_str(), "C:\\Saves\\Bridge.sav.sav");   // the game's
    PSE_CHECK_STR(ds2_saves::LoadFileFor("C:\\Saves\\Bridge 1").c_str(), "C:\\Saves\\Bridge 1.sav");
    PSE_CHECK_STR(ds2_saves::LoadFileFor("C:\\Saves\\Bridge.sav").c_str(), "C:\\Saves\\Bridge.sav");
    PSE_CHECK_STR(ds2_saves::LoadFileFor("C:\\Saves\\Act 1.5").c_str(), "C:\\Saves\\Act 1.sav");      // _splitpath
    PSE_CHECK_STR(ds2_saves::LoadFileFor("C:\\Sa.ves\\Bridge").c_str(), "C:\\Sa.ves\\Bridge.sav");    // dot in a folder
    PSE_CHECK_STR(ds2_saves::CoSavePathFor("C:\\Saves\\Bridge 1.sav").c_str(), "C:\\Saves\\Bridge 1.pse");
    PSE_CHECK_STR(ds2_events::Utf8(L"C:\\Saves\\Br\u00FCcke").c_str(), "C:\\Saves\\Br\xC3\xBC" "cke");
    PSE_CHECK_STR(ds2_events::Utf8(nullptr).c_str(), "");
}

PSE_TEST("ds2 layer saves: NEW_GAME from the front end, co-save written / read, no NEW_GAME for a loaded save") {
    SetupSaves();
    ds2_saves::LevelStart("FrontEnd");
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 0);
    ds2_saves::LevelStart("mission01.dll");
    PSE_REQUIRE(Count(PSE_MSG_NEW_GAME) == 1);
    PSE_CHECK_STR(g_seen.messages.back().second.c_str(), "mission01.dll");
    PSE_CHECK_EQ(g_seen.reverts, 1);
    ds2_saves::LevelStart("mission02.dll");   // the next mission of the same game
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 1);

    const std::string sav = TempPath("ds2 save.sav"), co = TempPath("ds2 save.pse");
    DeleteFileA(co.c_str());
    Touch(co, "stale");   // the .sav isn't there yet: a left-over co-save goes first
    ds2_saves::SaveBegin(sav);
    Touch(sav, "game save");
    ds2_saves::SaveEnd(sav, true, false);
    PSE_CHECK_EQ(g_seen.saves, 1);
    PSE_REQUIRE(Exists(co));
    PSE_CHECK_EQ(Count(PSE_MSG_SAVE_BEGIN), 1);
    PSE_CHECK_EQ(Count(PSE_MSG_SAVE_END), 1);
    PSE_CHECK_STR(g_seen.messages.back().second.c_str(), sav.c_str());

    ds2_saves::LevelStart("FrontEnd");
    ds2_saves::LoadBegin(sav);
    ds2_saves::LoadEnd(sav, true);
    PSE_CHECK_EQ(g_seen.loads, 1);
    PSE_CHECK_STR(g_seen.loaded.c_str(), "ds2");
    ds2_saves::LevelStart("mission02.dll");   // the save's level
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 1);

    // A refused load: LOAD_END only; the next start from the front end is a new game again.
    ds2_saves::LevelStart("FrontEnd");
    ds2_saves::LoadEnd(sav, false);
    PSE_CHECK_EQ(Count(PSE_MSG_LOAD_END), 2);
    ds2_saves::LevelStart("mission01.dll");
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 2);
    DeleteFileA(sav.c_str());
    DeleteFileA(co.c_str());
    DeleteTestKey();
}

// ---- pse.game.ds2 (tier 1) -------------------------------------------------------------------------------------------

PSE_TEST("game ds2: table, built-in plugin pse.game.ds2, manifests can require it") {
    const PSEGameDs2V1* g = pse_game_ds2::Table();
    PSE_CHECK_EQ(g->size, static_cast<uint32_t>(sizeof(PSEGameDs2V1)));
    PSE_CHECK_EQ(g->version, 1u);
    const void* const* slots = reinterpret_cast<const void* const*>(g);
    for (size_t i = 2; i < sizeof(PSEGameDs2V1) / sizeof(void*); ++i) PSE_CHECK(slots[i] != nullptr);
    Fresh();
    const PSEPluginHandle h = pse_game_ds2::Register();
    PSE_REQUIRE(h != 0);
    PSE_CHECK(pse::PluginInfo(h)->flags & PSE_PLUGIN_CLIENT_ONLY);
    const PSEPluginHandle other = AddPlugin("test.user");
    PSE_CHECK(pse::LoadInterfaceFor(other)->Query(other, PSE_GAME_DS2_INTERFACE, 1) == g);
    PSE_CHECK(pse::LoadInterfaceFor(other)->Query(other, PSE_GAME_DS2_INTERFACE, 2) == nullptr);
    PSE_CHECK_EQ(pse_game_ds2::KindOf(""), static_cast<uint32_t>(PSE_DS2_LEVEL_NONE));
    PSE_CHECK_EQ(pse_game_ds2::KindOf("FrontEnd"), static_cast<uint32_t>(PSE_DS2_LEVEL_FRONTEND));
    PSE_CHECK_EQ(pse_game_ds2::KindOf("frontend"), static_cast<uint32_t>(PSE_DS2_LEVEL_FRONTEND));
    PSE_CHECK_EQ(pse_game_ds2::KindOf("Mission01"), static_cast<uint32_t>(PSE_DS2_LEVEL_MISSION));
    std::vector<pse::PluginCandidate> c(1);
    std::string error;
    PSE_REQUIRE(pse::ParseManifest("name = test.ds2only\nrequires = pse.game.ds2 >= 1.0\n", &c[0].manifest, &error));
    c[0].version = PSE_MAKE_VERSION(1, 0, 0);
    PSE_CHECK_EQ(pse::ResolvePluginOrder(c).size(), 1u);
    DeleteTestKey();
}

// ---- tier 2: the mapped CDS II.exe -----------------------------------------------------------------------------------

PSE_TEST("ids: ds2 every signature matches CDS II.exe, all ids resolve; spot checks") {
    PSE_REQUIRE_EXE_FOR("ds2");
    pse::ids::ImageView image;
    PSE_REQUIRE(OriginalImage(&image));
    const auto& g = Ds2();
    const auto& b = g.builds[0];
    for (uint32_t i = 0; i < b.addressCount; ++i) {
        const auto& a = b.addresses[i];
        if (a.checkVa && !pse::ids::SignatureMatches(image, a.checkVa, a.signature)) {
            char what[96];
            std::snprintf(what, sizeof what, "id %u at 0x%08X: %s", a.id, a.checkVa, a.signature);
            PSE_CHECK_STR(what, "(matching signature)");
        }
        uint32_t kind = 0;
        for (uint32_t k = 0; k < g.entryCount; ++k)
            if (g.entries[k].id == a.id) kind = g.entries[k].kind;
        std::string section;
        PSE_CHECK(exe::SectionOf(a.va, &section));
        if (kind == PSE_ID_KIND_FUNCTION || kind == PSE_ID_KIND_SITE) PSE_CHECK_STR(section.c_str(), ".text");
        else PSE_CHECK(section != ".text");
    }
    pse::ids::ResetForTests();
    pse::ids::RegisterSummary sum;
    PSE_REQUIRE(pse::ids::RegisterGame(g, image, 0, &sum) == PSE_OK);
    PSE_CHECK_STR(sum.build.c_str(), "nodisc-1.0");
    PSE_CHECK_EQ(sum.resolved, g.entryCount);
    PSE_CHECK_EQ(sum.mismatched, 0u);
    const auto* ids = pse::ids::Interface();
    // The layer's constants are the database's addresses.
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Site_FrameDispatchCall), ds2_events::kFrameCall);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Main_FrameDispatch), ds2_events::kFrameDispatch);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Site_RenderCall), ds2_events::kRenderCall);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Render_Frame), ds2_events::kRenderFrame);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Site_FrontEndInitCall), ds2_events::kFrontEndInitCall);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, FrontEnd_Init), ds2_events::kFrontEndInit);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Site_LevelInitCall), ds2_events::kLevelInitCall);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Level_Init), ds2_events::kLevelInit);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Site_LevelUnloadCall), ds2_events::kUnloadCall);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Level_Unload), ds2_events::kLevelUnload);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Site_WmDestroyCall), ds2_events::kDestroyCall);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Save_Game), ds2_events::kSaveGame);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Save_Load), ds2_events::kLoadGame);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Iat_Direct3DCreate9), ds2_events::kIatDirect3DCreate9);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, FrontEnd_State), 0x63433Cu);   // pse_game_ds2.cpp
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Main_DispatchMode), 0x627158u);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, World), 0x68008Cu);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Split_Object), 0x626C18u);
    PSE_CHECK_EQ(PSE_RESOLVE(ids, ds2, Window_Handle), 0x62F41Cu);
    // IAT slots name their imports; call sites call their functions.
    const struct {
        uint32_t id;
        const char* import;
    } iats[] = {{PSE_ID_ds2_Iat_Direct3DCreate9, "d3d9.dll!Direct3DCreate9"},
                {PSE_ID_ds2_Iat_CreateWindowExA, "USER32.dll!CreateWindowExA"},
                {PSE_ID_ds2_Iat_DialogBoxParamA, "USER32.dll!DialogBoxParamA"},
                {PSE_ID_ds2_Iat_MessageBoxA, "USER32.dll!MessageBoxA"},
                {PSE_ID_ds2_Iat_ScreenToClient, "USER32.dll!ScreenToClient"},
                {PSE_ID_ds2_Iat_PostQuitMessage, "USER32.dll!PostQuitMessage"}};
    for (const auto& i : iats) PSE_CHECK_STR(exe::ImportName(ids->Resolve("ds2", i.id)).c_str(), i.import);
    const struct {
        uint32_t site, fn;
    } calls[] = {{PSE_ID_ds2_Site_FrameDispatchCall, PSE_ID_ds2_Main_FrameDispatch},
                 {PSE_ID_ds2_Site_RenderCall, PSE_ID_ds2_Render_Frame},
                 {PSE_ID_ds2_Site_LevelInitCall, PSE_ID_ds2_Level_Init},
                 {PSE_ID_ds2_Site_MissionLoadCall, PSE_ID_ds2_Mission_Load},
                 {PSE_ID_ds2_Site_FrontEndInitCall, PSE_ID_ds2_FrontEnd_Init},
                 {PSE_ID_ds2_Site_LevelUnloadCall, PSE_ID_ds2_Level_Unload},
                 {PSE_ID_ds2_Site_KeyWaitCall, PSE_ID_ds2_Input_WaitAnyKey},
                 {PSE_ID_ds2_Site_PickSoldierCall, PSE_ID_ds2_Input_PickSoldier},
                 {PSE_ID_ds2_Site_EngineInitCall, PSE_ID_ds2_Engine_Init}};
    for (const auto& c : calls) {
        const uint32_t site = ids->Resolve("ds2", c.site);
        uint8_t b5[5] = {};
        PSE_REQUIRE(exe::OriginalBytes(site, b5, 5));
        PSE_CHECK_EQ(b5[0], uint8_t{0xE8});
        int32_t rel = 0;
        memcpy(&rel, b5 + 1, 4);
        PSE_CHECK_EQ(site + 5 + static_cast<uint32_t>(rel), ids->Resolve("ds2", c.fn));
    }
    pse::ids::ResetForTests();
}

namespace {
std::vector<PSEHookInfo> Hooks() {
    const PSEHooksInterface* h = pse::HooksInterface();
    std::vector<PSEHookInfo> list(h->ListHooks(nullptr, 0));
    for (auto& i : list) i.size = sizeof i;
    list.resize(h->ListHooks(list.data(), static_cast<uint32_t>(list.size())));
    return list;
}
const PSEHookInfo* At(const std::vector<PSEHookInfo>& list, uint32_t va, PSEPluginHandle owner) {
    for (const auto& i : list)
        if (i.va == va && i.owner == owner) return &i;
    return nullptr;
}
}  // namespace

PSE_TEST("ds2 layer image: event hooks on CDS II.exe - sites, kinds, priorities, prologues") {
    PSE_REQUIRE_EXE_FOR("ds2");
    exe::Restore();
    Fresh();
    ds2_events::ResetForTests();
    const PSEPluginHandle layer = AddPlugin("pse.game.ds2");
    PSE_CHECK_EQ(ds2_events::Install(layer), 0);
    PSE_CHECK_EQ(ds2_events::Install(layer), 0);   // once
    const auto list = Hooks();
    struct Want {
        uint32_t va;
        PSEHookKind kind;
        int32_t priority;
        uint32_t len;
    };
    const Want wants[] = {
        {ds2_events::kFrameCall, PSE_HOOK_CALL_SITE, PSE_DS2_LAYER_PRIORITY_INNER, 5},
        {ds2_events::kRenderCall, PSE_HOOK_CALL_SITE, PSE_DS2_LAYER_PRIORITY_INNER, 5},
        {ds2_events::kFrontEndInitCall, PSE_HOOK_CALL_SITE, PSE_DS2_LAYER_PRIORITY_OUTER, 5},
        {ds2_events::kLevelInitCall, PSE_HOOK_CALL_SITE, PSE_DS2_LAYER_PRIORITY_OUTER, 5},
        {ds2_events::kUnloadCall, PSE_HOOK_CALL_SITE, PSE_DS2_LAYER_PRIORITY_OUTER, 5},
        {ds2_events::kDestroyCall, PSE_HOOK_CALL_SITE, PSE_DS2_LAYER_PRIORITY_OUTER, 5},
        {ds2_events::kSaveGame, PSE_HOOK_FUNCTION, PSE_DS2_LAYER_PRIORITY_OUTER, 10},
        {ds2_events::kLoadGame, PSE_HOOK_FUNCTION, PSE_DS2_LAYER_PRIORITY_OUTER, 10},
        {ds2_events::kIatDirect3DCreate9, PSE_HOOK_POINTER, PSE_DS2_LAYER_PRIORITY_OUTER, 4},
    };
    for (const Want& w : wants) {
        const PSEHookInfo* i = At(list, w.va, layer);
        if (!i) {
            char what[64];
            std::snprintf(what, sizeof what, "hook at 0x%08X", w.va);
            PSE_CHECK_STR(what, "(installed)");
            continue;
        }
        PSE_CHECK_EQ(static_cast<int>(i->kind), static_cast<int>(w.kind));
        PSE_CHECK_EQ(i->priority, w.priority);
        PSE_CHECK_EQ(i->len, w.len);
    }
    uint8_t orig[10] = {};
    PSE_REQUIRE(exe::OriginalBytes(ds2_events::kSaveGame, orig, 10));
    PSE_CHECK(memcmp(orig, ds2_events::kSavePrologue, 10) == 0);
    PSE_REQUIRE(exe::OriginalBytes(ds2_events::kLoadGame, orig, 10));
    PSE_CHECK(memcmp(orig, ds2_events::kLoadPrologue, 10) == 0);
    PSE_CHECK_EQ(exe::Read<uint8_t>(ds2_events::kSaveGame), uint8_t{0xE9});
    PSE_CHECK(exe::Read<uint32_t>(ds2_events::kIatDirect3DCreate9) ==
              static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&ds2_events::Direct3DCreate9Hook)));

    // A DesertStorm2Fix-style hook on the same frame call at priority 0 runs first and reaches the layer.
    const PSEPluginHandle ds2fix = AddPlugin("ds2fix.framecap");
    static void* next = nullptr;
    PSEHookId id = 0;
    PSE_CHECK_EQ(pse::HooksInterface()->HookCallSite(ds2fix, ds2_events::kFrameCall, ds2_events::kFrameDispatch,
                                                     reinterpret_cast<void*>(+[]() {}), 0, &next, &id),
                 PSE_OK);
    PSE_CHECK(next == reinterpret_cast<void*>(&ds2_events::FrameHook));
    pse::hooks::ResetForTests();
    ds2_events::ResetForTests();
    exe::Restore();
    DeleteTestKey();
}

PSE_TEST("game ds2 image: front-end state, dispatch mode, level name / kind, cutscene bit, split screen, window") {
    PSE_REQUIRE_EXE_FOR("ds2");
    exe::Restore();
    const PSEGameDs2V1* g = pse_game_ds2::Table();
    char buf[64] = {};
    // The file's values: nothing loaded.
    PSE_CHECK_EQ(g->FrontEndState(), 0u);
    PSE_CHECK_EQ(g->DispatchMode(), 0u);
    PSE_CHECK_EQ(g->LevelName(buf, sizeof buf), 0u);
    PSE_CHECK_EQ(g->LevelKind(), static_cast<uint32_t>(PSE_DS2_LEVEL_NONE));
    PSE_CHECK_EQ(g->InCutscene(), 0);
    PSE_CHECK_EQ(g->SplitScreenPlayers(), 1u);
    PSE_CHECK(g->GameWindow() == nullptr);
    // Written the way the game does.
    static uint8_t world[0x1C00] = {};
    static char scene[] = "Mission01";
    *reinterpret_cast<uint32_t*>(&world[0x1BBC]) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(scene));
    *static_cast<uint32_t*>(exe::Ptr(0x68008C)) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(world));
    *static_cast<uint32_t*>(exe::Ptr(0x63433C)) = 0x13;
    *static_cast<uint8_t*>(exe::Ptr(0x627158)) = 1;
    PSE_CHECK_EQ(g->FrontEndState(), 0x13u);
    PSE_CHECK_EQ(g->DispatchMode(), 1u);
    PSE_CHECK_EQ(g->LevelName(buf, sizeof buf), 9u);
    PSE_CHECK_STR(buf, "Mission01");
    PSE_CHECK_EQ(g->LevelName(buf, 4), 9u);   // cut, NUL-terminated
    PSE_CHECK_STR(buf, "Mis");
    PSE_CHECK_EQ(g->LevelKind(), static_cast<uint32_t>(PSE_DS2_LEVEL_MISSION));
    world[0x1B8C] = 1;
    PSE_CHECK_EQ(g->InCutscene(), 1);
    static char frontEnd[] = "FrontEnd";
    *reinterpret_cast<uint32_t*>(&world[0x1BBC]) = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(frontEnd));
    PSE_CHECK_EQ(g->LevelKind(), static_cast<uint32_t>(PSE_DS2_LEVEL_FRONTEND));
    *reinterpret_cast<uint32_t*>(&world[0x1BBC]) = 0x10;   // a stale pointer: "", no crash
    PSE_CHECK_EQ(g->LevelName(buf, sizeof buf), 0u);
    uint32_t* split = static_cast<uint32_t*>(exe::Ptr(0x626C18));
    split[0] = 1;
    split[1] = 3;
    PSE_CHECK_EQ(g->SplitScreenPlayers(), 3u);
    split[1] = 9;   // nonsense: one view
    PSE_CHECK_EQ(g->SplitScreenPlayers(), 1u);
    *static_cast<uint32_t*>(exe::Ptr(0x62F41C)) = 0x12340;
    PSE_CHECK(g->GameWindow() == reinterpret_cast<void*>(0x12340));
    // The mapped file hashes to the known build (4 GB flag ignored).
    PSE_CHECK_STR(pse_game_ds2::ExeFileSha256(W(exe::Path()).c_str()).c_str(), kDs2Sha);
    exe::Restore();
}

// ---- start-up: the loader picks the DS2 layer (game-neutral loader) --------------------------------------------------

namespace {
std::vector<std::string> g_fallback;
void Fallback(const char* line) { g_fallback.emplace_back(line); }
bool FallbackHas(const char* text) {
    for (const std::string& l : g_fallback)
        if (l.find(text) != std::string::npos) return true;
    return false;
}
std::string ReadText(const std::string& path) {
    std::string out;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) out.append(b, n);
        fclose(f);
    }
    return out;
}
}  // namespace

PSE_TEST("ds2 start-up: both game layers installed, the loader picks pse_ds2.dll for CDS II.exe; plugins for both games load") {
    PSE_REQUIRE_EXE_FOR("ds2");
    pse::ResetForTests();
    g_fallback.clear();
    const std::string dir = TempPath("ds2tree");
    CreateDirectoryA(dir.c_str(), nullptr);
    CreateDirectoryA((dir + "\\Games").c_str(), nullptr);
    CreateDirectoryA((dir + "\\Plugins").c_str(), nullptr);
    PSE_REQUIRE(CopyFileA(PSE_GAME_LAYER_DLL, (dir + "\\Games\\pse_ds1.dll").c_str(), FALSE));
    PSE_REQUIRE(CopyFileA(PSE_GAME_LAYER_DS2_DLL, (dir + "\\Games\\pse_ds2.dll").c_str(), FALSE));
    const bool hello = Exists(std::string(PSE_PLUGIN_DIR) + "\\example.hello.dll");
    if (hello)
        for (const char* ext : {".dll", ".pse"})
            PSE_REQUIRE(CopyFileA((std::string(PSE_PLUGIN_DIR) + "\\example.hello" + ext).c_str(),
                                  (dir + "\\Plugins\\example.hello" + ext).c_str(), FALSE));

    pse::StartOptions o;
    o.pseDir = W(dir);
    o.exePath = W(exe::Path());
    o.imageBase = exe::kImageBase;
    o.registryRoot = TestRegistryRoot();
    o.fallbackLog = &Fallback;
    const pse::StartResult r = pse::Start(o);
    PSE_REQUIRE(r.running);
    PSE_CHECK_STR(r.gameId.c_str(), "ds2");
    PSE_CHECK_STR(r.exeSha256.c_str(), kDs2Sha);
    PSE_CHECK(r.gameLayer.find(L"pse_ds2.dll") != std::wstring::npos);
    PSE_CHECK(FallbackHas("pse_ds1.dll (ds1) doesn't recognise this exe"));
    PSE_CHECK(FallbackHas("game layer pse_ds2.dll recognises the exe: game ds2"));
    PSE_CHECK(GetModuleHandleA("pse_ds1.dll") == nullptr);   // the layer that said no is freed
    const PSEPluginHandle layer = pse::FindPlugin("pse.game.ds2");
    PSE_REQUIRE(layer);
    PSE_CHECK(pse::PluginHost(layer));
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(layer, PSE_GAME_DS2_INTERFACE, 1) != nullptr);
    PSE_CHECK(pse::MessagingInterface()->QueryInterface(layer, "pse.game.ds1", 1) == nullptr);
    // The layer's hooks on the mapped exe, owned by it.
    const auto list = Hooks();
    for (uint32_t va : {ds2_events::kFrameCall, ds2_events::kLevelInitCall, ds2_events::kSaveGame,
                        ds2_events::kIatDirect3DCreate9})
        PSE_CHECK(At(list, va, layer) != nullptr);
    // The DS1 example plugin, built once, loaded under ds2 (its games list says ds1 + ds2).
    if (hello) {
        PSE_CHECK_EQ(r.pluginsLoaded, 1u);
        PSE_CHECK(pse::FindPlugin("example.hello") != 0);
    }
    // No host plugin: the layer's own log file has every line, the start-up ones included.
    const std::string log = ReadText(dir + "\\Logs\\loader.log");
    PSE_CHECK(log.find("recognises the exe: game ds2") != std::string::npos);
    PSE_CHECK(log.find("Game layer: event hooks installed") != std::string::npos);
    PSE_CHECK(log.find("start-up: done in") != std::string::npos);
    if (hello) PSE_CHECK(log.find("hello: loaded (game ds2") != std::string::npos);
    pse::ResetForTests();   // frees the layer (its log file closes)
    exe::Restore();
    for (const char* f : {"\\Games\\pse_ds1.dll", "\\Games\\pse_ds2.dll", "\\Plugins\\example.hello.dll",
                          "\\Plugins\\example.hello.pse", "\\Logs\\loader.log"})
        DeleteFileA((dir + f).c_str());
    for (const char* d : {"\\Games", "\\Plugins", "\\Logs", ""}) RemoveDirectoryA((dir + d).c_str());
    DeleteTestKey();
}
