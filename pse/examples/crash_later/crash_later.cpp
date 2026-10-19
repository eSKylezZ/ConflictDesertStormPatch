// Test plugins that load fine and crash LATER, in a callback the loader calls (pse/tests/test_plugins.cpp). The loader
// must catch the fault, disable the plugin (hooks, byte groups, listeners, subscriptions, tasks, settings, co-save
// callbacks taken back), report it and carry on. Never install them in a game.
//
// One source, built four times (pse/CMakeLists.txt) with CRASH_NAME / CRASH_MODE:
//   example.crash_postload   faults in its PSE_MSG_POST_LOAD listener (its POST_POST_LOAD listener must never run)
//   example.crash_frame      faults in a PSE_EVENT_FRAME subscriber (priority 100); a second, later subscriber of its
//                            own must be skipped in the same frame, other plugins' subscribers must still run
//   example.crash_task       queues two tasks in Load; the first faults, the second must never run
//   example.crash_callbacks  faults where the environment variable PSE_CRASH_AT says: "setting" (settings OnChange),
//                            "save" / "load" / "revert" (co-save), "message" (a listener for type 0x4242), "nested"
//                            (a frame subscriber raises an event of its own whose subscriber - same plugin - faults;
//                            the outer callback then carries on and returns), "unload" (PSEPlugin_Unload)
// Every variant also hooks a pointer and patches a byte group inside its own DLL, so the tests can check both are
// taken back without game addresses.
#include <windows.h>

#include <pse/pse_api.h>

#include <cstring>

#ifndef CRASH_NAME
#define CRASH_NAME "example.crash_frame"
#endif
#ifndef CRASH_MODE
#define CRASH_MODE 2
#endif
// CRASH_MODE: 1 postload, 2 frame, 3 task, 4 callbacks (PSE_CRASH_AT)
#pragma warning(disable : 4127)   // "conditional expression is constant": CRASH_MODE picks the variant

namespace {

PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
const PSETasksInterface* g_tasks = nullptr;
const PSESettingsInterface* g_settings = nullptr;
const PSESerializationInterface* g_cosave = nullptr;
char g_at[32] = {};
PSEEventId g_nested = 0;

uint8_t g_bytes[4] = {1, 2, 3, 4};
const uint32_t kInterface[2] = {sizeof kInterface, 1};
int(PSE_CALL* g_pointer)(int) = nullptr;
void* g_next = nullptr;
int PSE_CALL Original(int x) { return x; }
int PSE_CALL Hooked(int x) { return x + 100; }

void Crash() {
    volatile int* null = nullptr;
    *null = 1;   // access violation
}

bool At(const char* what) { return std::strcmp(g_at, what) == 0; }

void PSE_CALL OnLoaderMessage(const PSEMessage* msg, void*) {
    if (CRASH_MODE == 1 && msg->type == PSE_MSG_POST_LOAD) {
        g_log->Write(g_self, PSE_LOG_INFO, "crash_later: crashing in PostLoad");
        Crash();
    }
    if (msg->type == PSE_MSG_POST_POST_LOAD) g_log->Write(g_self, PSE_LOG_INFO, "crash_later: PostPostLoad reached");
}

void PSE_CALL OnAnyMessage(const PSEMessage* msg, void*) {
    if (msg->type == 0x4242 && At("message")) Crash();
}

int PSE_CALL OnFrameFirst(PSEEventId, void*, void*) {
    if (CRASH_MODE == 2) {
        g_log->Write(g_self, PSE_LOG_INFO, "crash_later: crashing in a frame event");
        Crash();
    }
    if (CRASH_MODE == 4 && At("nested")) {
        g_messaging->Raise(g_self, g_nested, nullptr);   // its own subscriber faults in there
        g_log->Write(g_self, PSE_LOG_INFO, "crash_later: outer frame callback carried on");
    }
    return 0;
}

int PSE_CALL OnFrameLater(PSEEventId, void*, void*) {
    g_log->Write(g_self, PSE_LOG_INFO, "crash_later: second frame subscriber ran");
    return 0;
}

int PSE_CALL OnNested(PSEEventId, void*, void*) {
    Crash();
    return 0;
}

void PSE_CALL FirstTask(void*) {
    g_log->Write(g_self, PSE_LOG_INFO, "crash_later: crashing in a task");
    Crash();
}

void PSE_CALL SecondTask(void*) { g_log->Write(g_self, PSE_LOG_INFO, "crash_later: second task ran"); }

void PSE_CALL OnSetting(const char*, void*) {
    if (At("setting")) Crash();
}
void PSE_CALL OnSave(PSESaveWriter* w, void*) {
    if (At("save")) Crash();
    g_cosave->WriteChunk(w, 1, 1, "x", 1);
}
void PSE_CALL OnLoad(PSESaveReader*, void*) {
    if (At("load")) Crash();
}
void PSE_CALL OnRevert(void*) {
    if (At("revert")) Crash();
}

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, CRASH_NAME, "Late crash test (example plugin)", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), 0, nullptr, PSE_PLUGIN_CLIENT_ONLY,
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    g_tasks = static_cast<const PSETasksInterface*>(pse->Query(g_self, PSE_TASKS_INTERFACE, 1));
    g_settings = static_cast<const PSESettingsInterface*>(pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1));
    g_cosave = static_cast<const PSESerializationInterface*>(pse->Query(g_self, PSE_SERIALIZATION_INTERFACE, 1));
    auto hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    if (!g_log || !g_messaging || !g_tasks || !g_settings || !g_cosave || !hooks) return 0;
    if (CRASH_MODE == 4) GetEnvironmentVariableA("PSE_CRASH_AT", g_at, sizeof g_at);

    g_pointer = &Original;
    PSEHookId id = 0;
    hooks->HookPointer(g_self, reinterpret_cast<uint32_t>(&g_pointer), reinterpret_cast<void*>(&Hooked), 0, &g_next,
                       &id);
    static const uint8_t kOriginal[2] = {1, 2}, kPatched[2] = {0xAA, 0xBB};
    PSEBytePatch patch = {reinterpret_cast<uint32_t>(&g_bytes[0]), kOriginal, kPatched, 2, 0, "bytes"};
    hooks->PatchGroup(g_self, "crash_later", &patch, 1);

    g_messaging->RegisterListener(g_self, "pse", &OnLoaderMessage, nullptr);
    g_messaging->RegisterListener(g_self, nullptr, &OnAnyMessage, nullptr);
    g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrameFirst, nullptr, 100);
    g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrameLater, nullptr, -100);
    g_nested = g_messaging->EventId("example.crash.nested");
    g_messaging->Subscribe(g_self, g_nested, &OnNested, nullptr, 0);
    if (CRASH_MODE == 3) {
        g_tasks->RunOnGameThread(g_self, &FirstTask, nullptr);
        g_tasks->RunOnGameThread(g_self, &SecondTask, nullptr);
    }
    PSESettingDesc s = {};
    s.size = sizeof s;
    s.key = "Value";
    s.type = PSE_SETTING_INT;
    s.flags = PSE_SETTING_LIVE;
    s.minInt = 0;
    s.maxInt = 100;
    s.label = "CRASH TEST VALUE";
    g_settings->Declare(g_self, &s);
    g_settings->OnChange(g_self, &OnSetting, nullptr);
    g_cosave->SetCallbacks(g_self, &OnSave, &OnLoad, &OnRevert, nullptr);
    g_messaging->RegisterInterface(g_self, CRASH_NAME, 1, kInterface);
    g_log->Write(g_self, PSE_LOG_INFO, "crash_later: loaded (%s)", g_at[0] ? g_at : "fixed mode");
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    if (At("unload")) Crash();
    g_log->Write(g_self, PSE_LOG_INFO, "crash_later: unloading");
}

}  // extern "C"
