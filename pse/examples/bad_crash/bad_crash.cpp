// example.bad_crash - a test plugin that registers a bit of everything and then crashes in PSEPlugin_Load. The
// loader must catch the crash, take back the hook, the byte group, the frame subscription and the interface, disable
// the plugin and free the DLL - and the game carries on (pse/tests/test_plugins.cpp). Never install it in a game.
#include <pse/pse_api.h>

namespace {

PSEPluginHandle g_self = 0;

// Patched / hooked memory inside this DLL, so the test needs no game addresses.
uint8_t g_bytes[4] = {1, 2, 3, 4};
int(PSE_CALL* g_pointer)(int) = nullptr;
void* g_next = nullptr;

int PSE_CALL Original(int x) { return x; }
int PSE_CALL Hooked(int x) { return x + 100; }
int PSE_CALL OnFrame(PSEEventId, void*, void*) { return 0; }
const uint32_t kInterface[2] = {sizeof kInterface, 1};

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "example.bad_crash", "Crash test (example plugin)", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), 0, nullptr, PSE_PLUGIN_CLIENT_ONLY,
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    auto hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    auto messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    auto log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    if (!hooks || !messaging || !log) return 0;

    g_pointer = &Original;
    PSEHookId id = 0;
    hooks->HookPointer(g_self, reinterpret_cast<uint32_t>(&g_pointer), reinterpret_cast<void*>(&Hooked), 0, &g_next,
                       &id);
    static const uint8_t kOriginal[2] = {1, 2}, kPatched[2] = {0xAA, 0xBB};
    PSEBytePatch patch = {reinterpret_cast<uint32_t>(&g_bytes[0]), kOriginal, kPatched, 2, 0, "bytes"};
    hooks->PatchGroup(g_self, "bad_crash", &patch, 1);
    messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    messaging->RegisterInterface(g_self, "example.bad_crash", 1, kInterface);
    log->Write(g_self, PSE_LOG_INFO, "bad_crash: about to crash");

    volatile int* null = nullptr;
    *null = 1;   // access violation
    return 1;
}

}  // extern "C"
