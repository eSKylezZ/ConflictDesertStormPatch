// The DS1 game layer's core events (pse/games/ds1/ds1_events.cpp) and co-save glue (ds1_saves.cpp), compiled into this
// file: tier 1 - NEW_GAME / save / load / legacy-image logic with the loader services; tier 2 - the layer's hooks on the
// mapped original DesertStorm.exe (sites, kinds, priorities, prologues) and how DesertStormFix's hooks on the same
// sites chain with them (framecap in front of the frame hook, scripthost behind the level-init hook).
#include <windows.h>

#include <string>
#include <vector>

#include "games/ds1/ds1_events.cpp"   // NOLINT: units under test (pse/games/ds1)
#include "games/ds1/ds1_saves.cpp"    // NOLINT
#include "exe_image.h"
#include "loader_fixture.h"
#include "pse_test.h"

using namespace pse_fixture;

namespace {
struct Seen {
    std::vector<std::pair<uint32_t, std::string>> messages;   // (type, string data)
    int loads = 0, reverts = 0, saves = 0;
    std::vector<uint8_t> loaded;   // the last chunk's bytes
};
Seen g_seen;
constexpr uint32_t kTag = PSE_TAG('T', 'E', 'S', 'T');

void PSE_CALL OnMessage(const PSEMessage* m, void*) {
    const bool text = m->data && m->dataLen && m->type >= PSE_MSG_NEW_GAME && m->type <= PSE_MSG_LOAD_END;
    g_seen.messages.emplace_back(m->type, text ? static_cast<const char*>(m->data) : "");
}
void PSE_CALL OnSave(PSESaveWriter* w, void*) {
    ++g_seen.saves;
    const char data[] = "saved";
    pse::SerializationInterface()->WriteChunk(w, kTag, 1, data, sizeof data);
}
void PSE_CALL OnLoad(PSESaveReader* r, void*) {
    ++g_seen.loads;
    uint32_t tag, version, len;
    while (pse::SerializationInterface()->NextChunk(r, &tag, &version, &len)) {
        g_seen.loaded.assign(len, 0);
        pse::SerializationInterface()->Read(r, g_seen.loaded.data(), len);
    }
}
void PSE_CALL OnRevert(void*) { ++g_seen.reverts; }

int Count(uint32_t type) {
    int n = 0;
    for (const auto& m : g_seen.messages) n += m.first == type ? 1 : 0;
    return n;
}

// A co-save image with one chunk of `plugin` (the file format, docs/sdk/reference/serialization.md).
std::vector<uint8_t> Image(const char* plugin, const std::string& data) {
    std::vector<uint8_t> b = {'P', 'S', 'E', 1, 1, 0, 0, 0};
    b.push_back(static_cast<uint8_t>(strlen(plugin)));
    b.insert(b.end(), plugin, plugin + strlen(plugin));
    const uint32_t fields[3] = {kTag, 1, static_cast<uint32_t>(data.size())};
    const auto* f = reinterpret_cast<const uint8_t*>(fields);
    b.insert(b.end(), f, f + sizeof fields);
    b.insert(b.end(), data.begin(), data.end());
    return b;
}
std::vector<uint8_t> g_legacyImage;
int g_legacyCalls = 0;
uint32_t PSE_CALL Legacy(const char*, void* buf, uint32_t bufLen) {
    ++g_legacyCalls;
    if (buf) memcpy(buf, g_legacyImage.data(), std::min<size_t>(bufLen, g_legacyImage.size()));
    return static_cast<uint32_t>(g_legacyImage.size());
}

void Touch(const std::string& path, const char* text) {
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
bool Exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

PSEPluginHandle Setup() {
    Fresh();
    ds1_saves::ResetForTests();
    g_seen = Seen{};
    const PSEPluginHandle layer = AddPlugin("pse.game.ds1");
    ds1_saves::SetOwner(layer);
    const PSEPluginHandle p = AddPlugin("test.cosave");
    pse::MessagingInterface()->RegisterListener(p, "pse", &OnMessage, nullptr);
    pse::SerializationInterface()->SetCallbacks(p, &OnSave, &OnLoad, &OnRevert, nullptr);
    return p;
}
}  // namespace

PSE_TEST("ds1 layer saves: NEW_GAME only when a game starts from the front end") {
    Setup();
    ds1_saves::LevelStart("FrontEnd.dll");
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 0);
    ds1_saves::LevelStart("Mission1.dll");
    PSE_REQUIRE(Count(PSE_MSG_NEW_GAME) == 1);
    PSE_CHECK_STR(g_seen.messages.back().second.c_str(), "Mission1.dll");
    PSE_CHECK_EQ(g_seen.reverts, 1);
    ds1_saves::LevelStart("Mission2.dll");   // the next mission inside the same game
    ds1_saves::LevelStart("Mission2.dll");   // RESTART LEVEL
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 1);
    ds1_saves::LevelStart("frontend.dll");   // any case
    ds1_saves::LevelStart("ambush.lua");     // Lua / mod levels count as games too
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 2);
    PSE_CHECK_EQ(g_seen.reverts, 2);
    DeleteTestKey();
}

PSE_TEST("ds1 layer saves: save writes <name>.pse, load reads it, the loaded level gets no NEW_GAME") {
    Setup();
    const std::string sav = TempPath("layer save.sav"), co = TempPath("layer save.pse");
    DeleteFileA(co.c_str());
    Touch(co, "stale co-save of a deleted save");   // the .sav doesn't exist yet: this one must go first
    ds1_saves::SaveBegin(sav.c_str());
    Touch(sav, "game save");                          // (what the game's saver does between the two)
    ds1_saves::SaveEnd(sav.c_str(), true, false);
    PSE_CHECK_EQ(g_seen.saves, 1);
    PSE_REQUIRE(Exists(co));
    PSE_CHECK_EQ(Count(PSE_MSG_SAVE_BEGIN), 1);
    PSE_CHECK_EQ(Count(PSE_MSG_SAVE_END), 1);
    PSE_CHECK_STR(g_seen.messages.back().second.c_str(), sav.c_str());
    // The name without ".sav" (the game adds it): same co-save.
    PSE_CHECK_STR(ds1_saves::CoSavePathFor(ds1_saves::WithSav(sav.substr(0, sav.size() - 4).c_str())).c_str(), co.c_str());

    // A failed write: SAVE_END, no callbacks.
    ds1_saves::SaveEnd(sav.c_str(), false, true);
    PSE_CHECK_EQ(g_seen.saves, 1);
    PSE_CHECK_EQ(Count(PSE_MSG_SAVE_END), 2);

    ds1_saves::LevelStart("FrontEnd.dll");
    ds1_saves::LoadBegin(sav.c_str());
    ds1_saves::LoadEnd(sav.c_str(), true);
    PSE_CHECK_EQ(g_seen.loads, 1);
    PSE_CHECK_STR(reinterpret_cast<const char*>(g_seen.loaded.data()), "saved");
    PSE_CHECK_EQ(Count(PSE_MSG_LOAD_BEGIN), 1);
    PSE_CHECK_EQ(Count(PSE_MSG_LOAD_END), 1);
    ds1_saves::LevelStart("Mission1.dll");   // the save's level: no NEW_GAME, no revert
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 0);
    PSE_CHECK_EQ(g_seen.reverts, 0);

    // A load the game refused: LOAD_END only, and the next game start from the front end is a new game again.
    ds1_saves::LevelStart("FrontEnd.dll");
    ds1_saves::LoadBegin(sav.c_str());
    ds1_saves::LoadEnd(sav.c_str(), false);
    PSE_CHECK_EQ(g_seen.loads, 1);
    PSE_CHECK_EQ(Count(PSE_MSG_LOAD_END), 2);
    ds1_saves::LevelStart("Mission1.dll");
    PSE_CHECK_EQ(Count(PSE_MSG_NEW_GAME), 1);
    DeleteFileA(sav.c_str());
    DeleteFileA(co.c_str());
    DeleteTestKey();
}

PSE_TEST("ds1 layer saves: no .pse -> the host's legacy image, else every plugin reverts") {
    Setup();
    const std::string sav = TempPath("layer legacy.sav"), co = TempPath("layer legacy.pse");
    DeleteFileA(co.c_str());
    Touch(sav, "game save");
    ds1_saves::LoadEnd(sav.c_str(), true);   // no co-save, no legacy reader: revert
    PSE_CHECK_EQ(g_seen.reverts, 1);
    PSE_CHECK_EQ(g_seen.loads, 0);

    g_legacyImage = Image("test.cosave", "from the sidecar");
    g_legacyCalls = 0;
    ds1_saves::SetLegacy(&Legacy);
    ds1_saves::LoadEnd(sav.c_str(), true);
    PSE_CHECK_EQ(g_seen.loads, 1);
    PSE_CHECK_EQ(g_legacyCalls, 2);   // size, then the bytes
    PSE_CHECK(std::string(g_seen.loaded.begin(), g_seen.loaded.end()) == "from the sidecar");
    PSE_CHECK(!Exists(co));             // reading never writes

    g_legacyImage.clear();              // the reader has nothing for this save: revert
    ds1_saves::LoadEnd(sav.c_str(), true);
    PSE_CHECK_EQ(g_seen.reverts, 2);

    // A real .pse wins over the legacy reader.
    g_legacyImage = Image("test.cosave", "from the sidecar");
    ds1_saves::SaveEnd(sav.c_str(), true, true);
    g_legacyCalls = 0;
    ds1_saves::LoadEnd(sav.c_str(), true);
    PSE_CHECK_EQ(g_legacyCalls, 0);
    PSE_CHECK_STR(reinterpret_cast<const char*>(g_seen.loaded.data()), "saved");
    ds1_saves::SetLegacy(nullptr);
    DeleteFileA(sav.c_str());
    DeleteFileA(co.c_str());
    DeleteTestKey();
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
void* g_dsfixNext = nullptr;
void __cdecl DsfixFrame() {}
int __cdecl DsfixLevelInit(const char*) { return 0; }
}  // namespace

PSE_TEST("ds1 layer image: event hooks on the original exe - sites, kinds, priorities, prologues") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Fresh();
    ds1_events::ResetForTests();
    const PSEPluginHandle layer = AddPlugin("pse.game.ds1");
    PSE_CHECK_EQ(ds1_events::Install(layer), 0);
    PSE_CHECK_EQ(ds1_events::Install(layer), 0);   // once
    const auto list = Hooks();
    struct Want {
        uint32_t va;
        PSEHookKind kind;
        int32_t priority;
        uint32_t len;
    };
    const Want wants[] = {
        {ds1_events::kFrameCall, PSE_HOOK_CALL_SITE, PSE_DS1_LAYER_PRIORITY_INNER, 5},
        {ds1_events::kRenderCall, PSE_HOOK_CALL_SITE, PSE_DS1_LAYER_PRIORITY_INNER, 5},
        {ds1_events::kLevelInitCall, PSE_HOOK_CALL_SITE, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kUnloadCalls[0], PSE_HOOK_CALL_SITE, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kUnloadCalls[1], PSE_HOOK_CALL_SITE, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kSaveGame, PSE_HOOK_FUNCTION, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kLoadGame, PSE_HOOK_FUNCTION, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kIatDirect3DCreate8, PSE_HOOK_POINTER, PSE_DS1_LAYER_PRIORITY_OUTER, 4},
    };
    for (const Want& w : wants) {
        const PSEHookInfo* i = At(list, w.va, layer);
        PSE_REQUIRE(i);
        PSE_CHECK_EQ(static_cast<int>(i->kind), static_cast<int>(w.kind));
        PSE_CHECK_EQ(i->priority, w.priority);
        PSE_CHECK_EQ(i->len, w.len);
        PSE_CHECK_EQ(i->chainPosition, 0u);
    }
    // The prologues the entry detours moved are whole instructions of exactly 5 bytes.
    std::vector<uint8_t> orig(5);
    PSE_REQUIRE(exe::OriginalBytes(ds1_events::kSaveGame, orig.data(), 5));
    PSE_CHECK(memcmp(orig.data(), ds1_events::kSavePrologue, 5) == 0);
    PSE_REQUIRE(exe::OriginalBytes(ds1_events::kLoadGame, orig.data(), 5));
    PSE_CHECK(memcmp(orig.data(), ds1_events::kLoadPrologue, 5) == 0);
    PSE_CHECK_EQ(exe::Read<uint8_t>(ds1_events::kSaveGame), uint8_t{0xE9});

    // DesertStormFix on the same sites (its own priorities, 0): framecap runs FIRST at the frame call and reaches the
    // layer through next; scripthost runs AFTER the layer at level init (the layer's messages bracket it).
    const PSEPluginHandle dsfix = AddPlugin("dsfix.framecap");
    PSEHookId id = 0;
    PSE_CHECK_EQ(pse::HooksInterface()->HookCallSite(dsfix, ds1_events::kFrameCall, ds1_events::kFrameDispatch,
                                                     reinterpret_cast<void*>(&DsfixFrame), 0, &g_dsfixNext, &id),
                 PSE_OK);
    PSE_CHECK(g_dsfixNext == reinterpret_cast<void*>(&ds1_events::FrameHook));
    PSE_CHECK(ds1_events::g_frameNext == reinterpret_cast<void*>(static_cast<uintptr_t>(ds1_events::kFrameDispatch)));
    void* levelNext = nullptr;
    PSE_CHECK_EQ(pse::HooksInterface()->HookCallSite(dsfix, ds1_events::kLevelInitCall, ds1_events::kLevelInit,
                                                     reinterpret_cast<void*>(&DsfixLevelInit), 0, &levelNext, &id),
                 PSE_OK);
    PSE_CHECK(ds1_events::g_levelInitNext == reinterpret_cast<void*>(&DsfixLevelInit));
    PSE_CHECK(levelNext == reinterpret_cast<void*>(static_cast<uintptr_t>(ds1_events::kLevelInit)));
    const auto after = Hooks();
    PSE_CHECK_EQ(At(after, ds1_events::kFrameCall, dsfix)->chainPosition, 0u);
    PSE_CHECK_EQ(At(after, ds1_events::kFrameCall, layer)->chainPosition, 1u);
    PSE_CHECK_EQ(At(after, ds1_events::kLevelInitCall, layer)->chainPosition, 0u);
    PSE_CHECK_EQ(At(after, ds1_events::kLevelInitCall, dsfix)->chainPosition, 1u);

    pse::hooks::ResetForTests();
    ds1_events::ResetForTests();
    exe::Restore();
    DeleteTestKey();
}
