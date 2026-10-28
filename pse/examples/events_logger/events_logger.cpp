// example.events_logger - logs the core game events and messages a game layer sends (docs/sdk/reference/messaging.md,
// game-ds1.md "Events and messages") and keeps a small co-save chunk (pse.serialization), built only against the SDK
// header. Works with the game layer alone: no DesertStormFix needed.
//
// Its own log file "example.events_logger.log" sits next to its DLL (PSE\Plugins) - the live tests read it, also in a
// run without DesertStormFix, where no DesertStormFix.log exists. Lines:
//   loaded / message <NAME> [data] / event <NAME> first [data] / event <NAME> [level] / frames <n> ...
//   cosave save frames=<n> / cosave load frames=<n> (or "cosave load no chunk") / cosave revert
//   ds1.soldier_damaged / _killed, ds1.vehicle_damaged / _destroyed <victim id> by <attacker id> ... (DS1: the first
//   10 damages, every kill / wreck), ds1.item_picked_up <soldier> took <item>, ds1.objective_completed <n>,
//   ds1.mission_end <result>, ds1.script_message 0x<id> <param 0> <param 1> (the first 15, then kills / pick-ups / doors
//   / enemies spotted)
#include <windows.h>

#include <pse/pse_api.h>
#include <pse/pse_game_ds1.h>   // the DS1 game events (named: no other game raises them)

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace {

PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
const PSESerializationInterface* g_ser = nullptr;
char g_file[MAX_PATH] = {};
uint32_t g_frames = 0, g_preRender = 0, g_postRender = 0, g_endScene = 0, g_present = 0;
constexpr uint32_t kTag = PSE_TAG('E', 'V', 'T', 'S');

void Line(const char* fmt, ...) {
    char text[512];
    va_list a;
    va_start(a, fmt);
    std::vsnprintf(text, sizeof text, fmt, a);
    va_end(a);
    if (g_log) g_log->Write(g_self, PSE_LOG_INFO, "events_logger: %s", text);
    if (!g_file[0]) return;
    FILE* f = nullptr;
    if (fopen_s(&f, g_file, "a") || !f) return;
    std::fprintf(f, "[%10lu] %s\n", GetTickCount(), text);
    std::fclose(f);
}

const char* MessageName(uint32_t type) {
    switch (type) {
        case PSE_MSG_POST_LOAD: return "POST_LOAD";
        case PSE_MSG_POST_POST_LOAD: return "POST_POST_LOAD";
        case PSE_MSG_GAME_ENTRY: return "GAME_ENTRY";
        case PSE_MSG_EXIT: return "EXIT";
        case PSE_MSG_NEW_GAME: return "NEW_GAME";
        case PSE_MSG_SAVE_BEGIN: return "SAVE_BEGIN";
        case PSE_MSG_SAVE_END: return "SAVE_END";
        case PSE_MSG_LOAD_BEGIN: return "LOAD_BEGIN";
        case PSE_MSG_LOAD_END: return "LOAD_END";
        case PSE_MSG_FRONT_END_READY: return "FRONT_END_READY";
        case PSE_MSG_DEVICE_CREATED: return "DEVICE_CREATED";
        case PSE_MSG_DEVICE_RESET: return "DEVICE_RESET";
        case PSE_MSG_DEVICE_LOST: return "DEVICE_LOST";
        default: return nullptr;
    }
}

void PSE_CALL OnMessage(const PSEMessage* m, void*) {
    const char* name = MessageName(m->type);
    if (!name) return;
    // String data (save path, level name) for the messages that carry one; the device ones carry a pointer.
    const bool text = m->type == PSE_MSG_NEW_GAME || (m->type >= PSE_MSG_SAVE_BEGIN && m->type <= PSE_MSG_LOAD_END) ||
                      m->type == PSE_MSG_FRONT_END_READY;
    if (text && m->data && m->dataLen) Line("message %s %s", name, static_cast<const char*>(m->data));
    else Line("message %s%s", name, m->data ? " (data)" : "");
}

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    if (++g_frames == 1) Line("event FRAME first");
    if (g_frames % 3000 == 0)
        Line("frames %u pre_render %u post_render %u end_scene %u present %u", g_frames, g_preRender, g_postRender,
             g_endScene, g_present);
    return 0;
}
int PSE_CALL OnCounted(PSEEventId e, void*, void* ctx) {
    uint32_t& n = *static_cast<uint32_t*>(ctx);
    if (++n == 1) {
        const char* name = e == PSE_EVENT_PRE_RENDER    ? "PRE_RENDER"
                           : e == PSE_EVENT_POST_RENDER ? "POST_RENDER"
                           : e == PSE_EVENT_END_SCENE   ? "END_SCENE"
                                                        : "PRESENT";
        Line("event %s first", name);
    }
    return 0;
}
int PSE_CALL OnLevel(PSEEventId e, void* data, void*) {
    const char* name = e == PSE_EVENT_LEVEL_LOAD_BEGIN ? "LEVEL_LOAD_BEGIN"
                       : e == PSE_EVENT_LEVEL_LOAD_END ? "LEVEL_LOAD_END"
                                                       : "LEVEL_UNLOAD";
    Line("event %s %s (frames %u)", name, data ? static_cast<const char*>(data) : "-", g_frames);
    return 0;
}

// DS1 game events (pse_game_ds1.h): logged only - an example of reading the damage event (a plugin may also change
// `amount` in "ds1.soldier_damage", the one before the game applies it).
uint32_t g_damaged = 0;
int PSE_CALL OnSoldier(PSEEventId, void* data, void* ctx) {
    const auto* e = static_cast<const PSEDs1DamageEvent*>(data);
    const char* name = static_cast<const char*>(ctx);
    if ((!std::strcmp(name, PSE_DS1_EVENT_SOLDIER_DAMAGED) || !std::strcmp(name, PSE_DS1_EVENT_VEHICLE_DAMAGED)) &&
        ++g_damaged > 10)
        return 0;
    Line("%s %d by %d amount %.1f health %.1f -> %.1f", name, e->victimId, e->attackerId, e->amount, e->healthBefore,
         e->healthAfter);
    return 0;
}
int PSE_CALL OnPickUp(PSEEventId, void* data, void*) {
    const auto* e = static_cast<const PSEDs1PickUpEvent*>(data);
    Line("%s %d took %d", PSE_DS1_EVENT_ITEM_PICKED_UP, e->soldierId, e->itemId);
    return 0;
}
int PSE_CALL OnObjective(PSEEventId, void* data, void*) {
    const auto* e = static_cast<const PSEDs1ObjectiveEvent*>(data);
    Line("%s %u title 0x%08X", PSE_DS1_EVENT_OBJECTIVE_COMPLETED, e->index, e->titleHash);
    return 0;
}
uint32_t g_messages = 0;
int PSE_CALL OnScriptMessage(PSEEventId, void* data, void*) {   // the first 15, then kills / pick-ups / doors / spotted
    const auto* e = static_cast<const PSEDs1ScriptMessageEvent*>(data);
    const bool notable = e->id == 0x00 || e->id == 0x03 || e->id == 0x15 || e->id == 0x34;
    if (++g_messages > 15 && !notable) return 0;
    if (e->params) Line("%s 0x%02X %d %d", PSE_DS1_EVENT_SCRIPT_MESSAGE, e->id, static_cast<int>(e->params[0]),
                        static_cast<int>(e->params[1]));
    else Line("%s 0x%02X", PSE_DS1_EVENT_SCRIPT_MESSAGE, e->id);
    return 0;
}
int PSE_CALL OnMissionEnd(PSEEventId, void* data, void*) {
    Line("%s %u", PSE_DS1_EVENT_MISSION_END, static_cast<const PSEDs1MissionEndEvent*>(data)->result);
    return 0;
}

// Co-save: one chunk with the frame count at the time of the save.
void PSE_CALL Save(PSESaveWriter* w, void*) {
    g_ser->WriteChunk(w, kTag, 1, &g_frames, sizeof g_frames);
    Line("cosave save frames=%u", g_frames);
}
void PSE_CALL Load(PSESaveReader* r, void*) {
    uint32_t tag, version, len, frames = 0;
    bool found = false;
    while (g_ser->NextChunk(r, &tag, &version, &len))
        if (tag == kTag && version == 1 && len == sizeof frames) found = g_ser->Read(r, &frames, sizeof frames) == 4;
    if (found) Line("cosave load frames=%u", frames);
    else Line("cosave load no chunk");
}
void PSE_CALL Revert(void*) { Line("cosave revert"); }

// The core events / messages are the same on every game layer: the same DLL logs DS1 and DS2.
const char* const kGames[] = {"ds1", "ds2", nullptr};

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "example.events_logger", "Events logger (example plugin)", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), 0, kGames, PSE_PLUGIN_CLIENT_ONLY,
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    g_ser = static_cast<const PSESerializationInterface*>(pse->Query(g_self, PSE_SERIALIZATION_INTERFACE, 1));
    if (!g_log || !g_messaging || !g_ser) return 0;

    HMODULE me = nullptr;   // the log file next to this DLL, started fresh every run
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&Line), &me);
    if (GetModuleFileNameA(me, g_file, MAX_PATH) && std::strlen(g_file) > 4) {
        std::strcpy(g_file + std::strlen(g_file) - 4, ".log");
        DeleteFileA(g_file);
    } else {
        g_file[0] = 0;
    }

    g_messaging->RegisterListener(g_self, "pse", &OnMessage, nullptr);
    bool ok = g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0) != 0;
    ok = g_messaging->Subscribe(g_self, PSE_EVENT_PRE_RENDER, &OnCounted, &g_preRender, 0) && ok;
    ok = g_messaging->Subscribe(g_self, PSE_EVENT_POST_RENDER, &OnCounted, &g_postRender, 0) && ok;
    ok = g_messaging->Subscribe(g_self, PSE_EVENT_END_SCENE, &OnCounted, &g_endScene, 0) && ok;
    ok = g_messaging->Subscribe(g_self, PSE_EVENT_PRESENT, &OnCounted, &g_present, 0) && ok;
    const PSEEventId levelEvents[] = {PSE_EVENT_LEVEL_LOAD_BEGIN, PSE_EVENT_LEVEL_LOAD_END, PSE_EVENT_LEVEL_UNLOAD};
    for (PSEEventId e : levelEvents) ok = g_messaging->Subscribe(g_self, e, &OnLevel, nullptr, 0) && ok;
    static const char* const soldierEvents[] = {PSE_DS1_EVENT_SOLDIER_DAMAGED, PSE_DS1_EVENT_SOLDIER_KILLED,
                                                PSE_DS1_EVENT_VEHICLE_DAMAGED, PSE_DS1_EVENT_VEHICLE_DESTROYED};
    for (const char* name : soldierEvents)
        ok = g_messaging->Subscribe(g_self, g_messaging->EventId(name), &OnSoldier, const_cast<char*>(name), 0) && ok;
    ok = g_messaging->Subscribe(g_self, g_messaging->EventId(PSE_DS1_EVENT_ITEM_PICKED_UP), &OnPickUp, nullptr, 0) && ok;
    ok = g_messaging->Subscribe(g_self, g_messaging->EventId(PSE_DS1_EVENT_OBJECTIVE_COMPLETED), &OnObjective, nullptr,
                                0) && ok;
    ok = g_messaging->Subscribe(g_self, g_messaging->EventId(PSE_DS1_EVENT_MISSION_END), &OnMissionEnd, nullptr, 0) && ok;
    ok = g_messaging->Subscribe(g_self, g_messaging->EventId(PSE_DS1_EVENT_SCRIPT_MESSAGE), &OnScriptMessage, nullptr,
                                0) && ok;
    ok = g_ser->SetCallbacks(g_self, &Save, &Load, &Revert, nullptr) == PSE_OK && ok;
    Line("loaded (game %s, API %u)%s", pse->gameId, pse->apiVersion, ok ? "" : " - some subscriptions failed");
    return ok ? 1 : 0;
}

}  // extern "C"
