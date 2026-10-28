#pragma once
// dsfix kit: what a DesertStormFix feature needs when it lives in its own PSE plugin DLL (PSE\Plugins\dsfix.<name>.dll)
// instead of inside dsfix.dll. Built on the public SDK only (pse_api.h) - no pse.host, no loader internals.
//
// The kit compiles DesertStormFix's own core/patch.cpp (every patch through the hooks broker, owned by this plugin)
// and supplies what that file and core/log.h expect from the host: pse::HooksInterface(), pse::PluginName(),
// pse_host::Core() and dslog::Write (-> pse.log, so the lines land in DesertStormFix.log with the plugin's name).
// A moved feature's sources keep using patch:: and dslog:: as before; only settings::Get() reads become kit settings.
//
// Settings: each split plugin owns its pse.settings key (HKCU\Software\PSE\ds1\Plugins\dsfix.<name>). DeclareBool
// copies the value once from plugin "dsfix"'s key, where DesertStormFix kept it before the split (marker
// "ImportedFromDsfix", like dsfix.discord).
#include <windows.h>

#include <cstdint>

#include "core/dsfix_services.h"
#include "pse/pse_api.h"

namespace kit {

// First thing in PSEPlugin_Load. False (and an error line when pse.log exists) without log / hooks / settings /
// messaging - the plugin should return 0 then.
bool Load(const PSELoadInterface* pse);

PSEPluginHandle Self();
const PSELoadInterface* LoadInterface();   // the loader's PSEPlugin_Load argument for this plugin
const char* Name();   // PSEPlugin_Info.name
const char* GameId();
const PSELogInterface* Log();
const PSEHooksInterface* Hooks();
const PSESettingsInterface* Settings();
const PSEMessagingInterface* Messaging();
const void* Query(const char* id, uint32_t minVersion);

// Before declaring it: copy `value` once from another plugin's key (a setting that lived in an internal dsfix.<x>
// sub-plugin's key before its feature moved into this DLL); only while ours has none.
void ImportFrom(const char* plugin, const char* value);
// An ON / OFF setting under its old DesertStormFix name (imported once from "dsfix"'s key). `changed` runs on the
// thread that changed it (the menu: the game thread). flags: PSE_SETTING_LIVE = takes effect at once (the default);
// 0 = "restart the game" (the DESERTSTORMFIX / PLUGINS menus mark the row). False when Declare failed.
bool DeclareBool(const char* key, bool defaultOn, const char* label, const char* help, const char* group,
                 void (*changed)(bool on) = nullptr, uint32_t flags = PSE_SETTING_LIVE);
bool GetBool(const char* key);
bool SetBool(const char* key, bool on);   // stored + the change callbacks run
// An int value (imported once the same way): e.g. a remembered choice with no menu row (flags PSE_SETTING_HIDDEN).
bool DeclareInt(const char* key, int defaultValue, int min, int max, uint32_t flags = PSE_SETTING_HIDDEN);
int GetInt(const char* key);
bool SetInt(const char* key, int value);

// dsfix.dll's interface dsfix.services v1 (core/dsfix_services.h), or null (an error line once) - queried at the
// first call. A plugin that needs it says `requires = dsfix >= 3.3` in its manifest.
const DsfixServicesV1* Services();

// fn once per game frame (PSE_EVENT_FRAME, before dsfix.dll's own per-frame work). False when the subscribe failed.
bool OnFrame(void (*fn)());

// HKCU\Software\DesertStormFix\Dev\<name> (dev test values; release builds always answer `fallback`).
DWORD DevDword(const char* name, DWORD fallback = 0);

}  // namespace kit

// The plugin's exports for a split-out feature: PSEPlugin_Info (version 1.0.0, game ds1, author DesertStormFix) and
// PSEPlugin_Load = kit::Load, then `load()` (bool(): its settings + Apply; false = not loaded). Once per plugin, in a
// .cpp at file scope. PSEPlugin_SimFingerprint comes from the kit itself (kit.cpp).
#define DSFIX_KIT_PLUGIN(NAME, DISPLAY, FLAGS, LOAD)                                                                 \
    namespace {                                                                                                      \
    const char* const kKitGames[] = {"ds1", nullptr};                                                                \
    }                                                                                                                \
    extern "C" {                                                                                                     \
    __declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;                                                 \
    const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, NAME, DISPLAY, "DesertStormFix",                 \
                                          PSE_MAKE_VERSION(1, 0, 0), 0, kKitGames, FLAGS};                           \
    __declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {                                 \
        return kit::Load(pse) && (LOAD)() ? 1 : 0;                                                                   \
    }                                                                                                                \
    }
