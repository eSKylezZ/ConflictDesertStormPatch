// example.hello_client - calls another plugin. Its manifest says `requires = example.hello >= 1.0`, so the loader
// loads example.hello first (or refuses this plugin when hello is missing / too old). Other plugins' interfaces are
// looked up in PSE_MSG_POST_LOAD: by then every plugin's Load has run.
//
// For the tests it publishes "example.hello_client" v1 { Answer() } = what hello's Add(2, 3) returned (-1 = not
// asked yet / hello missing).
#include <pse/pse_api.h>

#include "example_hello.h"

namespace {

PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
int g_answer = -1;

typedef struct ExampleHelloClientV1 {
    uint32_t size;
    int(PSE_CALL* Answer)(void);
} ExampleHelloClientV1;

int PSE_CALL Answer(void) { return g_answer; }
const ExampleHelloClientV1 kInterface = {sizeof(ExampleHelloClientV1), &Answer};

void PSE_CALL OnLoaderMessage(const PSEMessage* msg, void*) {
    if (msg->type != PSE_MSG_POST_LOAD) return;
    auto hello = static_cast<const ExampleHelloV1*>(g_messaging->QueryInterface(g_self, EXAMPLE_HELLO_INTERFACE, 1));
    if (!hello || hello->size < sizeof(ExampleHelloV1)) {
        g_log->Write(g_self, PSE_LOG_WARN, "hello_client: example.hello v1 is not available");
        return;
    }
    g_answer = hello->Add(2, 3);
    g_log->Write(g_self, PSE_LOG_INFO, "hello_client: example.hello says 2 + 3 = %d", g_answer);
}

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "example.hello_client", "Hello client (example plugin)", "PSE",
    PSE_MAKE_VERSION(1, 0, 0), 0, nullptr /* any game */, PSE_PLUGIN_CLIENT_ONLY,
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    if (!g_log || !g_messaging) return 0;
    // Messages from the loader only ("pse"): PSE_MSG_POST_LOAD, PSE_MSG_POST_POST_LOAD, ...
    if (g_messaging->RegisterListener(g_self, "pse", &OnLoaderMessage, nullptr) != PSE_OK) return 0;
    if (g_messaging->RegisterInterface(g_self, "example.hello_client", 1, &kInterface) != PSE_OK) return 0;
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    if (g_log) g_log->Write(g_self, PSE_LOG_INFO, "hello_client: unloading");
}

}  // extern "C"
