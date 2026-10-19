// example.ui_demo - settings of every type (they get a page under OPTIONS > DESERTSTORMFIX > PLUGINS and, where
// flagged, rows in the launcher - no UI code here), plus what pse.ui adds: a banner drawn over the game every frame,
// a registered text and pop-up messages (docs/sdk/reference/ui.md, docs/sdk/guides/first-plugin.md).
//
// Built only against the SDK headers. pse.ui is served by the game's host (DesertStormFix): without it the plugin
// still loads, its settings still get their menu page, it just draws nothing.
#include <pse/pse_api.h>
#include <pse/pse_ui.h>

#include <cstdio>
#include <cstring>

namespace {

PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSESettingsInterface* g_settings = nullptr;
const PSEMessagingInterface* g_messaging = nullptr;
const PSEUiInterface* g_ui = nullptr;   // NULL without DesertStormFix
uint32_t g_helloHash = 0;

// ---- settings: one of each type ----
const char* const kFonts[] = {"SMALL", "LARGE", nullptr};
const char* const kAligns[] = {"LEFT", "CENTRE", "RIGHT", nullptr};

bool Declare(const char* key, PSESettingType type, uint32_t flags, int32_t def, const char* label, const char* help,
             const char* group, int32_t min = 0, int32_t max = 0, int32_t step = 0,
             const char* const* choices = nullptr, const char* defString = nullptr) {
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = key;
    d.type = type;
    d.flags = flags;
    d.defaultInt = def;
    d.minInt = min;
    d.maxInt = max;
    d.step = step;
    d.defaultString = defString;
    d.label = label;
    d.help = help;
    d.choices = choices;
    d.group = group;
    const PSEResult r = g_settings->Declare(g_self, &d);
    if (r != PSE_OK) g_log->Write(g_self, PSE_LOG_ERROR, "ui_demo: setting %s not declared (%d)", key, static_cast<int>(r));
    return r == PSE_OK;
}

bool DeclareAll() {
    const uint32_t live = PSE_SETTING_LIVE;
    return Declare("ShowBanner", PSE_SETTING_BOOL, live | PSE_SETTING_LAUNCHER, 1, "SHOW BANNER",
                   "Draws a line of text over the game (pse.ui DrawText)", "BANNER") &&
           Declare("BannerY", PSE_SETTING_INT, live | PSE_SETTING_LAUNCHER, 8, "BANNER HEIGHT (%)",
                   "Where the banner sits, in percent of the screen height", "BANNER", 2, 90, 2) &&
           Declare("BannerFont", PSE_SETTING_CHOICE, live | PSE_SETTING_LAUNCHER, 0, "BANNER FONT", nullptr, "BANNER", 0,
                   0, 0, kFonts) &&
           Declare("BannerAlign", PSE_SETTING_CHOICE, live, 1, "BANNER ALIGNMENT", nullptr, "BANNER", 0, 0, 0, kAligns) &&
           Declare("BannerColour", PSE_SETTING_COLOUR, live, 0xE0D878, "BANNER COLOUR",
                   "Enter opens the colour wheel", "BANNER") &&
           Declare("Greeting", PSE_SETTING_STRING, live, 0, "GREETING",
                   "Text settings are shown read-only in the menu (edit the registry value)", "BANNER", 0, 0, 0,
                   nullptr, "Hello from ui_demo – café über ☃") &&
           Declare("PopupOnLevel", PSE_SETTING_BOOL, PSE_SETTING_NEXT_LEVEL, 1, "POP-UP AT LEVEL START",
                   "Shows a pop-up message when a level has loaded", "MESSAGES") &&
           Declare("LogChanges", PSE_SETTING_BOOL, 0, 1, "LOG EVERY CHANGE",
                   "A restart setting: the menu marks it with * and asks for a restart", "MESSAGES") &&
           Declare("Secret", PSE_SETTING_INT, PSE_SETTING_HIDDEN, 7, "SECRET", "Hidden: never in a menu", nullptr, 0,
                   100);
}

// ---- drawing (PSE_EVENT_FRAME: every game frame) ----
int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    if (!g_ui || !g_settings->GetInt(g_self, "ShowBanner")) return 0;
    char text[256];
    g_settings->GetString(g_self, "Greeting", text, sizeof text);
    const uint32_t font = g_settings->GetInt(g_self, "BannerFont") ? PSE_UI_LARGE : 0;
    const int align = g_settings->GetInt(g_self, "BannerAlign");
    const float y = g_settings->GetInt(g_self, "BannerY") / 100.0f;
    const uint32_t rgb = static_cast<uint32_t>(g_settings->GetInt(g_self, "BannerColour")) & 0xFFFFFF;
    PSEUiScreen screen = {sizeof screen};
    if (g_ui->GetScreen(&screen) != PSE_OK) return 0;
    const float line = screen.lineHeight * (font ? 2.0f : 1.0f) / screen.height;   // fraction of the height
    const float w = g_ui->TextWidth(g_self, text, font);
    const float x = align == 0 ? 0.04f : align == 1 ? 0.5f : 0.96f;
    const float left = align == 0 ? x : align == 1 ? x - w / 2 : x - w;
    g_ui->DrawRect(g_self, left - 0.01f, y - line * 0.3f, w + 0.02f, line * 1.6f, 0xA0000000, 0);
    g_ui->DrawRect(g_self, left - 0.01f, y - line * 0.3f, 0.004f, line * 1.6f, 0xFF000000 | rgb, 0);
    g_ui->DrawText(g_self, x, y, text, 0xFF000000 | rgb, font | (align == 1 ? PSE_UI_CENTER : align == 2 ? PSE_UI_RIGHT : 0));
    return 0;
}

int PSE_CALL OnLevelLoaded(PSEEventId, void* data, void*) {
    if (g_ui && g_settings->GetInt(g_self, "PopupOnLevel")) {
        char text[160];
        std::snprintf(text, sizeof text, "ui_demo: %s loaded", data ? static_cast<const char*>(data) : "level");
        g_ui->ShowPopup(g_self, text, 3000);
    }
    return 0;
}

void PSE_CALL Changed(const char* key, void*) {
    // The settings page (or the launcher) calls SetInt -> here, at once.
    g_log->Write(g_self, PSE_LOG_INFO, "ui_demo: changed %s = %d", key, g_settings->GetInt(g_self, key));
    if (g_ui) {
        char text[128];
        std::snprintf(text, sizeof text, "ui_demo: %s changed", key);
        g_ui->ShowPopup(g_self, text, 2500);
    }
}

const char* const kGames[] = {"ds1", nullptr};

}  // namespace

extern "C" {

__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info = {
    sizeof(PSEPluginInfo), 1, "example.ui_demo", "UI demo (example plugin)", "PSE", PSE_MAKE_VERSION(1, 0, 0), 0, kGames,
    PSE_PLUGIN_CLIENT_ONLY,
};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_settings = static_cast<const PSESettingsInterface*>(pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1));
    g_messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    if (!g_log || !g_settings || !g_messaging) return 0;
    g_ui = static_cast<const PSEUiInterface*>(pse->Query(g_self, PSE_UI_INTERFACE, 1));   // optional
    if (!DeclareAll()) return 0;
    g_settings->OnChange(g_self, &Changed, nullptr);
    g_messaging->Subscribe(g_self, PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    g_messaging->Subscribe(g_self, PSE_EVENT_LEVEL_LOAD_END, &OnLevelLoaded, nullptr, 0);
    if (g_ui) g_helloHash = g_ui->RegisterText(g_self, "hello", "Hello from ui_demo – café");
    g_log->Write(g_self, PSE_LOG_INFO, "ui_demo: loaded, pse.ui %s, text hash %08X", g_ui ? "present" : "missing",
                 g_helloHash);
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {}

}  // extern "C"
