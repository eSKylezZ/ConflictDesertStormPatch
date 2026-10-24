// ds2fix.widescreen - DesertStorm2Fix's widescreen / HUD part as an external PSE plugin (games/ds2/plugins/widescreen).
//
// The same feature sources as the standalone dinput8.dll build (games/ds2/src/features): widescreen.cpp (launcher
// modes), hud.cpp + hudfix.cpp + generated/layout_clamps.h (HUD scale k, layout clamps, the DS1 HUD fixes), frontend.cpp
// (the front end and the in-mission menus on the virtual 1280 x 1024 screen) and fontsharp.cpp (sharper text). What they
// need from outside comes from here:
//   patch::            -> pse.hooks (shim/core/patch.h, patch.cpp), owned by this plugin
//   dslog::            -> pse.log
//   settings::         -> this plugin's pse.settings (HudScaling, HudScalePercent; HKCU\Software\PSE\ds2\Plugins\
//                         ds2fix.widescreen), copied once from ds2fix's key (marker ImportedFromDs2fix)
//   graphics::         -> ds2fix.graphics v1 (the virtual screen mapping + the device stay in ds2fix's D3D9 hooks);
//                         g_onDeviceCreated = the layer's PSE_MSG_DEVICE_CREATED
//   features::SplitViewSize, menus::SetHudOption -> ds2fix.hudhost v1 (src/pse/ds2fix_hud.h)
// ds2fix runs without this plugin (no HUD scaling, no OPTIONS rows); this plugin needs ds2fix (manifest).
#include <windows.h>
#include <d3d9.h>

#include <cstdarg>
#include <cstdio>
#include <initializer_list>

#include "core/log.h"
#include "core/patch.h"
#include "core/settings.h"
#include "ds2fix_graphics.h"
#include "ds2fix_hud.h"
#include "features/features.h"
#include "features/graphics.h"
#include "features/menus.h"
#include "pse/pse_api.h"

namespace {
PSEPluginHandle g_self = 0;
const PSELogInterface* g_log = nullptr;
const PSESettingsInterface* g_settings = nullptr;
const Ds2FixGraphicsV1* g_gfx = nullptr;
const Ds2FixHudHostV1* g_host = nullptr;
settings::Values g_values;

constexpr char kKeyHudScaling[] = "HudScaling", kKeyHudScalePercent[] = "HudScalePercent";
constexpr char kOwnKey[] = "Software\\PSE\\ds2\\Plugins\\ds2fix.widescreen";
constexpr char kDs2FixKey[] = "Software\\PSE\\ds2\\Plugins\\ds2fix";
constexpr char kImported[] = "ImportedFromDs2fix";

// The two values moved out of ds2fix keep their names; ds2fix's (itself copied from the standalone build's key) are
// copied once. Plain registry reads: the loader's settings migration is host-only.
void ImportFromDs2Fix() {
    DWORD v = 0, size = sizeof v;
    if (RegGetValueA(HKEY_CURRENT_USER, kOwnKey, kImported, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return;
    for (const char* name : {kKeyHudScaling, kKeyHudScalePercent}) {
        size = sizeof v;
        if (RegGetValueA(HKEY_CURRENT_USER, kDs2FixKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
            g_settings->SetInt(g_self, name, static_cast<int32_t>(v));
    }
    const DWORD one = 1;
    RegSetKeyValueA(HKEY_CURRENT_USER, kOwnKey, kImported, REG_DWORD, &one, sizeof one);
}

bool DeclareSettings() {
    PSESettingDesc d = {};
    d.size = sizeof d;
    d.key = kKeyHudScaling;
    d.type = PSE_SETTING_BOOL;
    d.flags = PSE_SETTING_LIVE;
    d.defaultInt = 1;
    d.minInt = 0;
    d.maxInt = 1;
    d.label = "HUD SCALING";
    d.help = "Scale the HUD and menus with the screen resolution (OPTIONS > HUD SCALING)";
    d.group = "HUD";
    if (g_settings->Declare(g_self, &d) != PSE_OK) return false;
    d.key = kKeyHudScalePercent;
    d.type = PSE_SETTING_INT;
    d.defaultInt = 100;
    d.minInt = 25;
    d.maxInt = 400;
    d.step = 25;
    d.label = "HUD SIZE";
    d.help = "Size of the HUD on top of the resolution scaling, in percent (OPTIONS > HUD SIZE)";
    return g_settings->Declare(g_self, &d) == PSE_OK;
}

// ds2fix.hudhost: OPTIONS rows -> the feature's functions (hud.cpp)
int PSE_CALL OptScaling() { return features::HudScaling() ? 1 : 0; }
uint32_t PSE_CALL OptPercent() { return features::HudScalePercent(); }
void PSE_CALL OptSet(int on, uint32_t percent) { features::SetHudScale(on != 0, percent); }
const Ds2FixHudOptionV1 kOption = {&OptScaling, &OptPercent, &OptSet};

void PSE_CALL OnMessage(const PSEMessage* msg, void*) {
    if (msg->type != PSE_MSG_DEVICE_CREATED || !msg->data || !graphics::g_onDeviceCreated) return;
    auto dev = static_cast<IDirect3DDevice9*>(const_cast<void*>(msg->data));
    IDirect3DSurface9* back = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back)) && back) {
        D3DSURFACE_DESC desc;
        if (SUCCEEDED(back->GetDesc(&desc)))
            graphics::g_onDeviceCreated(static_cast<int>(desc.Width), static_cast<int>(desc.Height));
        back->Release();
    }
}
}  // namespace

// ---- what the feature sources call -----------------------------------------------------------------------------------
#ifndef DS_DIST
void dslog::Write(const char* fmt, ...) {
    if (!g_log) return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof line, fmt, args);
    va_end(args);
    g_log->Write(g_self, PSE_LOG_INFO, "%s", line);
}
#endif

const settings::Values& settings::Get() { return g_values; }
void settings::Load() {
    g_values.hudScaling = g_settings->GetInt(g_self, kKeyHudScaling) != 0;
    g_values.hudScalePercent = static_cast<uint32_t>(g_settings->GetInt(g_self, kKeyHudScalePercent));
}
bool settings::Save(const Values& v) {
    const bool ok = g_settings->SetInt(g_self, kKeyHudScaling, v.hudScaling ? 1 : 0) == PSE_OK &&
                    g_settings->SetInt(g_self, kKeyHudScalePercent, static_cast<int32_t>(v.hudScalePercent)) == PSE_OK;
    g_values.hudScaling = v.hudScaling;
    g_values.hudScalePercent = v.hudScalePercent;
    return ok;
}

void (*graphics::g_onDeviceCreated)(int, int) = nullptr;
void graphics::SetVirtualScreen(bool on, int realW, int realH) { g_gfx->SetVirtualScreen(on ? 1 : 0, realW, realH); }
bool graphics::VirtualScreenOn() { return g_gfx->VirtualScreenOn() != 0; }
void graphics::RealToVirtual(long& x, long& y) { g_gfx->RealToVirtual(&x, &y); }
int graphics::RealWidth() { return g_gfx->RealWidth(); }
int graphics::RealHeight() { return g_gfx->RealHeight(); }
float graphics::VirtualScale() { return g_gfx->VirtualScale(); }
IDirect3DDevice9* graphics::Device() { return static_cast<IDirect3DDevice9*>(g_gfx->Device()); }

void features::SplitViewSize(int& w, int& h) {
    if (g_host && g_host->SplitViewSize) g_host->SplitViewSize(&w, &h);
}
void features::menus::SetHudOption(const HudOption& hud) {
    if (g_host && g_host->SetHudOption) g_host->SetHudOption(hud.set ? &kOption : nullptr);
}

// ---- plugin entry points ---------------------------------------------------------------------------------------------
extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"ds2", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "ds2fix.widescreen",
                                      "DesertStorm2Fix - widescreen and HUD", "DesertStorm2Fix",
                                      PSE_MAKE_VERSION(0, 1, 0), 0, kGames, PSE_PLUGIN_CLIENT_ONLY};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    g_self = pse->plugin;
    g_log = static_cast<const PSELogInterface*>(pse->Query(g_self, PSE_LOG_INTERFACE, 1));
    g_settings = static_cast<const PSESettingsInterface*>(pse->Query(g_self, PSE_SETTINGS_INTERFACE, 1));
    const auto* hooks = static_cast<const PSEHooksInterface*>(pse->Query(g_self, PSE_HOOKS_INTERFACE, 1));
    const auto* messaging = static_cast<const PSEMessagingInterface*>(pse->Query(g_self, PSE_MESSAGING_INTERFACE, 1));
    if (!g_log) return 0;
    if (!g_settings || !hooks || !messaging) {
        g_log->Write(g_self, PSE_LOG_ERROR, "ds2fix.widescreen: the loader lacks pse.settings / pse.hooks / pse.messaging v1");
        return 0;
    }
    g_gfx = static_cast<const Ds2FixGraphicsV1*>(messaging->QueryInterface(g_self, DS2FIX_GRAPHICS_INTERFACE, 1));
    if (!g_gfx || g_gfx->size < sizeof(Ds2FixGraphicsV1)) {
        g_log->Write(g_self, PSE_LOG_ERROR, "ds2fix.widescreen: needs ds2fix's %s v1 (DesertStorm2Fix)", DS2FIX_GRAPHICS_INTERFACE);
        return 0;
    }
    g_host = static_cast<const Ds2FixHudHostV1*>(messaging->QueryInterface(g_self, DS2FIX_HUDHOST_INTERFACE, 1));
    if (g_host && g_host->size < sizeof(Ds2FixHudHostV1)) g_host = nullptr;
    if (!g_host)   // older ds2fix: HUD k ignores split screen, no OPTIONS rows
        g_log->Write(g_self, PSE_LOG_WARN, "ds2fix.widescreen: ds2fix has no %s - no split-screen HUD sizes, no OPTIONS rows",
                     DS2FIX_HUDHOST_INTERFACE);
    if (!DeclareSettings()) return 0;
    ImportFromDs2Fix();
    settings::Load();
    patch::Attach(hooks, g_self);
    messaging->RegisterListener(g_self, nullptr, &OnMessage, nullptr);

    features::ApplyWidescreen();
    features::ApplyHud();
    features::ApplyFrontEnd();
    features::ApplyFontSharp();
    g_log->Write(g_self, PSE_LOG_INFO, "ds2fix.widescreen: loaded (HudScaling %d, HudScalePercent %u)",
                 g_values.hudScaling, g_values.hudScalePercent);
    return 1;
}

__declspec(dllexport) void PSE_CALL PSEPlugin_Unload(void) {
    features::menus::SetHudOption({});   // rows hidden; hooks / groups are dropped by the loader
}
}
