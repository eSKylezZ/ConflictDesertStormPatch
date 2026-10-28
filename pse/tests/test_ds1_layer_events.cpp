// The DS1 game layer's core events (pse/games/ds1/ds1_events.cpp) and co-save glue (ds1_saves.cpp), compiled into this
// file: tier 1 - NEW_GAME / save / load / legacy-image logic with the loader services; tier 2 - the layer's hooks on the
// mapped original DesertStorm.exe (sites, kinds, priorities, prologues) and how DesertStormFix's hooks on the same
// sites chain with them (framecap in front of the frame hook, scripthost behind the level-init hook).
#include <windows.h>

#include <string>
#include <vector>

#include "games/ds1/ds1_events.cpp"   // NOLINT: units under test (pse/games/ds1)
#include "games/ds1/ds1_saves.cpp"    // NOLINT
#include "games/ds1/ds1_originals.cpp"   // NOLINT
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
        {ds1_events::kTakeDamage, PSE_HOOK_FUNCTION, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kVehicleDamage, PSE_HOOK_FUNCTION, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kPickUp, PSE_HOOK_FUNCTION, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
        {ds1_events::kObjectiveComplete, PSE_HOOK_FUNCTION, PSE_DS1_LAYER_PRIORITY_OUTER, 10},
        {ds1_events::kMissionEnd, PSE_HOOK_FUNCTION, PSE_DS1_LAYER_PRIORITY_OUTER, 7},
        {ds1_events::kPickUpAddItemCall, PSE_HOOK_CALL_SITE, PSE_DS1_LAYER_PRIORITY_OUTER, 5},
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
    PSE_REQUIRE(exe::OriginalBytes(ds1_events::kTakeDamage, orig.data(), 5));
    PSE_CHECK(memcmp(orig.data(), ds1_events::kTakeDamagePrologue, 5) == 0);
    const struct {
        uint32_t va;
        const uint8_t* bytes;
        size_t n;
    } prologues[] = {{ds1_events::kVehicleDamage, ds1_events::kVehicleDamagePrologue, 5},
                     {ds1_events::kPickUp, ds1_events::kPickUpPrologue, 5},
                     {ds1_events::kObjectiveComplete, ds1_events::kObjectivePrologue, 10},
                     {ds1_events::kMissionEnd, ds1_events::kMissionEndPrologue, 7}};
    for (const auto& p : prologues) {
        std::vector<uint8_t> b(p.n);
        PSE_REQUIRE(exe::OriginalBytes(p.va, b.data(), static_cast<uint32_t>(p.n)));
        PSE_CHECK(memcmp(b.data(), p.bytes, p.n) == 0);
    }
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

PSE_TEST("ds1 layer originals: a game-folder file that isn't there opens its PSE\\Originals copy") {
    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    const std::string game = std::string(tmp) + "pse_originals_test", orig = game + "\\PSE\\Originals";
    CreateDirectoryA(game.c_str(), nullptr);
    CreateDirectoryA((game + "\\PSE").c_str(), nullptr);
    CreateDirectoryA(orig.c_str(), nullptr);
    auto touch = [](const std::string& p) {
        CloseHandle(CreateFileA(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr));
    };
    touch(orig + "\\mission1.dat");   // moved
    touch(orig + "\\catalog.dat");    // moved, but back in the game folder too
    touch(game + "\\catalog.dat");
    ds1_originals::SetFoldersForTests(game);
    PSE_CHECK_EQ(ds1_originals::Redirect((game + "\\mission1.dat").c_str()), orig + "\\mission1.dat");
    PSE_CHECK_EQ(ds1_originals::Redirect((game + "\\MISSION1.DAT").c_str()), orig + "\\MISSION1.DAT");
    PSE_CHECK(ds1_originals::Redirect((game + "\\catalog.dat").c_str()).empty());       // the game folder's wins
    PSE_CHECK(ds1_originals::Redirect((game + "\\nothing.dat").c_str()).empty());       // in neither
    PSE_CHECK(ds1_originals::Redirect((game + "\\*.dat").c_str()).empty());             // wildcards untouched
    PSE_CHECK(ds1_originals::Redirect((game + "\\Data\\mission1.dat").c_str()).empty());   // other folders too
    PSE_CHECK(ds1_originals::Redirect("").empty());
    for (const std::string& f : {orig + "\\mission1.dat", orig + "\\catalog.dat", game + "\\catalog.dat"})
        DeleteFileA(f.c_str());
    RemoveDirectoryA(orig.c_str());
    RemoveDirectoryA((game + "\\PSE").c_str());
    RemoveDirectoryA(game.c_str());
    ds1_originals::ResetForTests();
}

PSE_TEST("ds1 layer image: the originals redirect hooks the exe's five file imports, owned by the layer") {
    PSE_REQUIRE_EXE();
    exe::Restore();
    Fresh();
    ds1_originals::ResetForTests();
    const PSEPluginHandle layer = AddPlugin("pse.game.ds1");
    PSE_CHECK_EQ(ds1_originals::Install(layer), 0);
    const auto list = Hooks();
    for (uint32_t slot : {ds1_originals::kIatCreateFileA, ds1_originals::kIatCreateFileW,
                          ds1_originals::kIatGetFileAttributesA, ds1_originals::kIatFindFirstFileA,
                          ds1_originals::kIatLoadLibraryA}) {
        const PSEHookInfo* i = At(list, slot, layer);
        PSE_REQUIRE(i);
        PSE_CHECK_EQ(static_cast<int>(i->kind), static_cast<int>(PSE_HOOK_POINTER));
        PSE_CHECK_EQ(i->priority, PSE_DS1_LAYER_PRIORITY_OUTER);
    }
    pse::hooks::ResetForTests();
    ds1_originals::ResetForTests();
    exe::Restore();
    DeleteTestKey();
}

namespace {
// A made-up soldier: health at +0x54, script id at +8. The game's damage function stand-in takes the amount off.
struct FakeSoldier {
    uint8_t bytes[0x80] = {};   // (items: owner at +0x6C)
    void Set(int32_t id, float health) { memcpy(bytes + 8, &id, 4); memcpy(bytes + 0x54, &health, 4); }
    float Health() const { float h; memcpy(&h, bytes + 0x54, 4); return h; }
};
float g_gameGot = -1;
void __fastcall FakeTakeDamage(void* soldier, void*, float amount, void*) {
    g_gameGot = amount;
    float h;
    memcpy(&h, static_cast<uint8_t*>(soldier) + 0x54, 4);
    h -= amount;
    memcpy(static_cast<uint8_t*>(soldier) + 0x54, &h, 4);
}
std::vector<std::string> g_damageSeen;
float g_scale = 1.0f;
int PSE_CALL OnDamage(PSEEventId, void* data, void* ctx) {
    auto* e = static_cast<PSEDs1DamageEvent*>(data);
    char b[160];
    snprintf(b, sizeof b, "%s id %d by %d amount %.1f health %.1f -> %.1f", static_cast<const char*>(ctx), e->victimId,
             e->attackerId, e->amount, e->healthBefore, e->healthAfter);
    g_damageSeen.push_back(b);
    if (!strcmp(static_cast<const char*>(ctx), "damage")) e->amount *= g_scale;
    return 0;
}
}  // namespace

PSE_TEST("ds1 layer events: soldier damage - a subscriber changes or cancels it, killed on the way to 0") {
    Fresh();
    const PSEPluginHandle plugin = AddPlugin("test.damage");
    const PSEMessagingInterface* m = pse::MessagingInterface();
    ds1_events::g_evDamage = m->EventId(PSE_DS1_EVENT_SOLDIER_DAMAGE);
    ds1_events::g_evDamaged = m->EventId(PSE_DS1_EVENT_SOLDIER_DAMAGED);
    ds1_events::g_evKilled = m->EventId(PSE_DS1_EVENT_SOLDIER_KILLED);
    ds1_events::g_damageNext = reinterpret_cast<void*>(&FakeTakeDamage);
    static char kDamage[] = "damage", kDamaged[] = "damaged", kKilled[] = "killed";
    m->Subscribe(plugin, ds1_events::g_evDamage, &OnDamage, kDamage, 0);
    m->Subscribe(plugin, ds1_events::g_evDamaged, &OnDamage, kDamaged, 0);
    m->Subscribe(plugin, ds1_events::g_evKilled, &OnDamage, kKilled, 0);
    FakeSoldier victim, shooter;
    victim.Set(31, 100.0f);
    shooter.Set(7, 150.0f);
    g_damageSeen.clear();
    g_scale = 0.5f;   // half damage
    ds1_events::DamageHook(victim.bytes, nullptr, 40.0f, shooter.bytes);
    PSE_CHECK(g_gameGot == 20.0f);
    PSE_CHECK(victim.Health() == 80.0f);
    g_scale = 0.0f;   // cancelled: the game isn't called
    g_gameGot = -1;
    ds1_events::DamageHook(victim.bytes, nullptr, 40.0f, nullptr);
    PSE_CHECK(g_gameGot == -1.0f);
    g_scale = 1.0f;   // the killing blow
    ds1_events::DamageHook(victim.bytes, nullptr, 90.0f, shooter.bytes);
    g_gameGot = -1;
    ds1_events::DamageHook(victim.bytes, nullptr, 10.0f, shooter.bytes);   // already dead: straight to the game
    PSE_CHECK(g_gameGot == 10.0f);
    const std::vector<std::string> want = {
        "damage id 31 by 7 amount 40.0 health 100.0 -> 100.0",
        "damaged id 31 by 7 amount 20.0 health 100.0 -> 80.0",
        "damage id 31 by -1 amount 40.0 health 80.0 -> 80.0",
        "damage id 31 by 7 amount 90.0 health 80.0 -> 80.0",
        "damaged id 31 by 7 amount 90.0 health 80.0 -> -10.0",
        "killed id 31 by 7 amount 90.0 health 80.0 -> -10.0",
    };
    PSE_CHECK_EQ(g_damageSeen.size(), want.size());
    for (size_t i = 0; i < want.size() && i < g_damageSeen.size(); ++i) PSE_CHECK_EQ(g_damageSeen[i], want[i]);
    ds1_events::g_damageNext = nullptr;
    DeleteTestKey();
}

namespace {
std::vector<std::string> g_more;
int PSE_CALL OnMore(PSEEventId, void* data, void* ctx) {
    const char* name = static_cast<const char*>(ctx);
    char b[160];
    if (!strcmp(name, "picked")) {
        auto* e = static_cast<PSEDs1PickUpEvent*>(data);
        snprintf(b, sizeof b, "picked %d took %d", e->soldierId, e->itemId);
    } else if (!strcmp(name, "end")) {
        snprintf(b, sizeof b, "end %u", static_cast<PSEDs1MissionEndEvent*>(data)->result);
    } else {
        auto* e = static_cast<PSEDs1DamageEvent*>(data);
        snprintf(b, sizeof b, "%s %d amount %.1f health %.1f -> %.1f", name, e->victimId, e->amount, e->healthBefore,
                 e->healthAfter);
        if (!strcmp(name, "vdamage")) e->amount *= g_scale;
    }
    g_more.push_back(b);
    return 0;
}
int g_finishSeen = -1;
void __fastcall FakeVehicleDamage(void* v, void*, float amount, void*, int, float, int finish) {
    g_finishSeen = finish;
    float h;
    memcpy(&h, static_cast<uint8_t*>(v) + 0x54, 4);
    h -= amount;
    memcpy(static_cast<uint8_t*>(v) + 0x54, &h, 4);
}
int __fastcall FakeAddItem(void*, void*, void* item, int, int) {   // takes it unless the item's id is 99 (refused)
    int32_t id;
    memcpy(&id, static_cast<uint8_t*>(item) + 8, 4);
    return id != 99;
}
void __fastcall FakePickUp(void* soldier, void*, void* item) {   // the game's pick-up: AddItem through its call site
    ds1_events::AddItemHook(soldier, nullptr, item, 0, 0);
}
uint32_t g_resultSet = 0;
void __fastcall FakeSetResult(void*, void*, uint32_t result) { g_resultSet = result; }
}  // namespace

PSE_TEST("ds1 layer events: vehicles (finish calls can't be cancelled), pick-ups, mission end") {
    Fresh();
    const PSEPluginHandle plugin = AddPlugin("test.more");
    const PSEMessagingInterface* m = pse::MessagingInterface();
    ds1_events::g_evVehDamage = m->EventId(PSE_DS1_EVENT_VEHICLE_DAMAGE);
    ds1_events::g_evVehDamaged = m->EventId(PSE_DS1_EVENT_VEHICLE_DAMAGED);
    ds1_events::g_evVehDestroyed = m->EventId(PSE_DS1_EVENT_VEHICLE_DESTROYED);
    ds1_events::g_evPickUp = m->EventId(PSE_DS1_EVENT_ITEM_PICKED_UP);
    ds1_events::g_evMissionEnd = m->EventId(PSE_DS1_EVENT_MISSION_END);
    ds1_events::g_vehicleNext = reinterpret_cast<void*>(&FakeVehicleDamage);
    ds1_events::g_pickUpNext = reinterpret_cast<void*>(&FakePickUp);
    ds1_events::g_addItemNext = reinterpret_cast<void*>(&FakeAddItem);
    ds1_events::g_missionEndNext = reinterpret_cast<void*>(&FakeSetResult);
    static char kV1[] = "vdamage", kV2[] = "vdamaged", kV3[] = "destroyed", kP[] = "picked", kE[] = "end";
    m->Subscribe(plugin, ds1_events::g_evVehDamage, &OnMore, kV1, 0);
    m->Subscribe(plugin, ds1_events::g_evVehDamaged, &OnMore, kV2, 0);
    m->Subscribe(plugin, ds1_events::g_evVehDestroyed, &OnMore, kV3, 0);
    m->Subscribe(plugin, ds1_events::g_evPickUp, &OnMore, kP, 0);
    m->Subscribe(plugin, ds1_events::g_evMissionEnd, &OnMore, kE, 0);
    g_more.clear();
    FakeSoldier tank, soldier, item, refused;
    tank.Set(309, 500.0f);
    soldier.Set(0, 150.0f);
    item.Set(245, 0.0f);
    refused.Set(99, 0.0f);
    g_scale = 0.0f;   // cancelled
    ds1_events::VehicleDamageHook(tank.bytes, nullptr, 100.0f, nullptr, 1, 1.0f, 0);
    PSE_CHECK(tank.Health() == 500.0f);
    g_scale = 1.0f;
    ds1_events::VehicleDamageHook(tank.bytes, nullptr, 600.0f, nullptr, 1, 1.0f, 0);   // destroyed
    PSE_CHECK(g_finishSeen == 0);
    tank.Set(309, 50.0f);
    g_scale = 0.0f;   // a finish call: no before-event, so no cancel
    ds1_events::VehicleDamageHook(tank.bytes, nullptr, 100.0f, nullptr, 1, 1.0f, 1);
    PSE_CHECK(g_finishSeen == 1 && tank.Health() == -50.0f);
    ds1_events::PickUpHook(soldier.bytes, nullptr, item.bytes);
    ds1_events::PickUpHook(soldier.bytes, nullptr, refused.bytes);   // the game refused: no event
    ds1_events::MissionEndHook(nullptr, nullptr, 2);
    PSE_CHECK(g_resultSet == 2);
    const std::vector<std::string> want = {
        "vdamage 309 amount 100.0 health 500.0 -> 500.0",
        "vdamage 309 amount 600.0 health 500.0 -> 500.0",
        "vdamaged 309 amount 600.0 health 500.0 -> -100.0",
        "destroyed 309 amount 600.0 health 500.0 -> -100.0",
        "vdamaged 309 amount 100.0 health 50.0 -> -50.0",
        "destroyed 309 amount 100.0 health 50.0 -> -50.0",
        "picked 0 took 245",
        "end 2",
    };
    PSE_CHECK_EQ(g_more.size(), want.size());
    for (size_t i = 0; i < want.size() && i < g_more.size(); ++i) PSE_CHECK_EQ(g_more[i], want[i]);
    ds1_events::g_vehicleNext = ds1_events::g_pickUpNext = ds1_events::g_missionEndNext = nullptr;
    ds1_events::g_addItemNext = nullptr;
    DeleteTestKey();
}

namespace {
// A made-up script object: message records what reached the script; an "online" wrapper like authority.cpp's.
std::vector<std::string> g_scriptSeen;
int __cdecl FakeScriptMessage(int id, uint32_t* p) {
    g_scriptSeen.push_back("script " + std::to_string(id) + " " + std::to_string(p ? p[0] : 0));
    return 7;
}
int PSE_CALL OnScriptMessage(PSEEventId, void* data, void*) {
    auto* e = static_cast<PSEDs1ScriptMessageEvent*>(data);
    g_scriptSeen.push_back("event " + std::to_string(e->id) + " " + std::to_string(e->params ? e->params[0] : 0));
    return 0;
}
ds1_events::ScriptObject g_onlineCopy{};
ds1_events::ScriptObject* g_onlineOriginal = nullptr;
int __cdecl OnlineMessage(int id, uint32_t* p) {
    g_scriptSeen.push_back("online " + std::to_string(id));
    return reinterpret_cast<int(__cdecl*)(int, uint32_t*)>(g_onlineOriginal->fn[2])(id, p);
}
}  // namespace

PSE_TEST("ds1 layer image: script messages - the layer's copy of the script object raises them, chains stay one-way") {
    PSE_REQUIRE_EXE();
    Fresh();
    const PSEPluginHandle plugin = AddPlugin("test.messages");
    const PSEMessagingInterface* m = pse::MessagingInterface();
    ds1_events::g_evScriptMessage = m->EventId(PSE_DS1_EVENT_SCRIPT_MESSAGE);
    m->Subscribe(plugin, ds1_events::g_evScriptMessage, &OnScriptMessage, nullptr, 0);
    auto& slot = *reinterpret_cast<ds1_events::ScriptObject**>(exe::Ptr(ds1_events::kScriptObject));
    ds1_events::ScriptObject* const saved = slot;
    ds1_events::ScriptObject script{};
    script.fn[2] = reinterpret_cast<void*>(&FakeScriptMessage);
    slot = &script;
    ds1_events::WrapScriptObject();
    ds1_events::WrapScriptObject();   // once: a second level-init call doesn't wrap the copy again
    PSE_CHECK(slot == &ds1_events::g_scriptProxy && ds1_events::g_scriptOriginal == &script);
    g_onlineOriginal = slot;           // online mode wraps whatever is there (the layer's copy)
    g_onlineCopy = *slot;
    g_onlineCopy.fn[2] = reinterpret_cast<void*>(&OnlineMessage);
    slot = &g_onlineCopy;
    g_scriptSeen.clear();
    uint32_t params[8] = {37, 2};
    const int r = reinterpret_cast<int(__cdecl*)(int, uint32_t*)>(slot->fn[2])(0x15, params);   // door 37 opening
    PSE_CHECK_EQ(r, 7);
    const std::vector<std::string> want = {"online 21", "event 21 37", "script 21 37"};
    PSE_CHECK_EQ(g_scriptSeen.size(), want.size());
    for (size_t i = 0; i < want.size() && i < g_scriptSeen.size(); ++i) PSE_CHECK_EQ(g_scriptSeen[i], want[i]);
    slot = saved;
    ds1_events::g_evScriptMessage = 0;
    DeleteTestKey();
}
