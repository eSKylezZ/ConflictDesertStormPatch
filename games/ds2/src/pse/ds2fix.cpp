// DesertStorm2Fix as the host plugin of the Pivotal Script Extender (PSE) - the DS2 product since 2026-10-22.
//
// The same features as the standalone dinput8.dll build (src/dllmain.cpp), loaded by PSE: <game>\dinput8.dll = the PSE
// shim, PSE\pse_loader.dll + PSE\Games\pse_ds2.dll (the DS2 game layer: detection, pse.ids, pse.game.ds2, the core game
// events incl. PSE_MSG_EXIT at WM_DESTROY, the co-save) and this DLL as PSE\Plugins\ds2fix.dll + ds2fix.pse
// ("host = true"; the DS2 layer allows the name "ds2fix"). Other DS2 plugins: games/ds2/plugins/<name>/.
// Docs: docs/sdk/reference/game-ds2.md (repo root) "DesertStorm2Fix as a host plugin".
//
//   - every pse:: call goes through pse/hostlink (pse.host), linked in from the repo root (pse/);
//   - patching = DesertStormFix's broker-backed src/core/patch.cpp at the repo root (a superset of games/ds2/src/core/patch.h),
//     so every byte group / call hook / jump / slot has an owner: the feature's sub-plugin "ds2fix.<feature>"
//     (OwnerScope); the D3D9 vtable slots through core/comhooks.h (graphics.cpp);
//   - settings: pse.settings of "ds2fix" (src/pse/settings_pse.cpp: HKCU\Software\PSE\ds2\Plugins\ds2fix, copied once
//     from the standalone build's Enhancements key);
//   - log lines (loader + plugins + features) -> DesertStorm2Fix.log in dev builds; release builds (no log of their own)
//     leave them to the layer's PSE\Logs\loader.log.
#include <windows.h>

#include <algorithm>
#include <deque>
#include <map>
#include <string>
#include <vector>

#include "core/log.h"
#include "core/patch.h"
#include "core/proxy.h"
#include "core/pse_host.h"
#include "core/settings.h"
#include "ds2fix_graphics.h"
#include "ds2fix_hud.h"
#include "ds2fix_options.h"
#include "ds2fix_pads.h"
#include "features/features.h"
#include "features/graphics.h"
#include "features/menus.h"
#include "hostlink.h"
#include "loader.h"

// DirectInput for the launcher pad code (core/gamepad.cpp): the SYSTEM dinput8.dll (the game folder's is the PSE shim).
FARPROC RealDInput8(const char* name) {
    static HMODULE real = nullptr;
    if (!real) {
        char path[MAX_PATH];
        const UINT n = GetSystemDirectoryA(path, MAX_PATH);
        if (n == 0 || n > MAX_PATH - 16) return nullptr;
        strcat_s(path, "\\dinput8.dll");
        real = LoadLibraryA(path);
        if (!real) return nullptr;
    }
    return GetProcAddress(real, name);
}

// ---- pse_host (the DS1 host's interface, core/pse_host.h, implemented for DS2) ----
namespace {
PSEPluginHandle g_core = 0;
#ifndef DS_DIST
void PSE_CALL LogSink(const char* line) { dslog::Write("%s", line); }
void PSE_CALL ReportSink(const char* line, int error) { dslog::Write("%s%s", error ? "[report] [fail] " : "[report] ", line); }

// Dev: the broker's records once the menus are up and after each level load (the DS1 console's "pse hooks"), plus a
// count of hooks / groups per owner - a feature patching outside the broker shows up as a "[warn] ... outside the
// broker" line of patch.cpp instead.
void DumpHooks(const char* when) {
    const PSEHooksInterface* hooks = pse::HooksInterface();
    if (!hooks) return;
    std::vector<PSEHookInfo> list(hooks->ListHooks(nullptr, 0));
    for (auto& h : list) h.size = sizeof h;
    list.resize(std::min<size_t>(hooks->ListHooks(list.data(), static_cast<uint32_t>(list.size())), list.size()));
    static const char* const kKinds[] = {"bytes", "call", "function", "indirect", "pointer"};
    std::map<std::string, int> owners;
    for (const PSEHookInfo& h : list) {
        ++owners[h.ownerName ? h.ownerName : "?"];
        dslog::Write("[dev] hook %-8s %-22s 0x%08X len %2u prio %5d pos %u %s",
                     static_cast<uint32_t>(h.kind) < 5 ? kKinds[h.kind] : "?", h.ownerName ? h.ownerName : "?", h.va,
                     h.len, h.priority, h.chainPosition, h.name ? h.name : "");
    }
    std::string sum;
    for (const auto& [name, n] : owners) sum += " " + name + "=" + std::to_string(n);
    dslog::Write("[dev] PSE hooks (%s): %zu records, %zu owners:%s", when, list.size(), owners.size(), sum.c_str());
}

void PSE_CALL OnMessage(const PSEMessage* msg, void*) {
    static bool menus = false;
    if (msg->type == PSE_MSG_FRONT_END_READY && !menus) menus = true, DumpHooks("menus");
}
int PSE_CALL OnLevelLoaded(PSEEventId, void* data, void*) {
    const char* level = static_cast<const char*>(data);
    if (level && _stricmp(level, "FrontEnd") != 0) DumpHooks(level);
    return 0;
}
#endif
}  // namespace

bool pse_host::Init(const PSELoadInterface* pse) {
    if (!pse::hostlink::Attach(pse)) return false;
    g_core = pse->plugin;
#ifndef DS_DIST
    const PSEHostInterface* host = pse::hostlink::Host();
    host->SetLogSink(&LogSink);
    host->SetReportSink(&ReportSink);
#endif
    return true;
}
PSEPluginHandle pse_host::Core() { return g_core; }
PSEPluginHandle pse_host::Register(const PSEPluginInfo* info) { return g_core ? pse::RegisterInternalPlugin(info) : 0; }
PSEPluginHandle pse_host::Find(const char* name) { return g_core && name ? pse::FindPlugin(name) : 0; }
void pse_host::RegisterFeatures(const Feature* table, size_t count) {
    static std::deque<PSEPluginInfo> infos;   // must outlive the process
    for (size_t i = 0; i < count; ++i) {
        const Feature& f = table[i];
        if (Find(f.name)) continue;
        infos.push_back({sizeof(PSEPluginInfo), 1, f.name, f.display, "DesertStorm2Fix", kDs2FixVersion,
                         PSE_API_VERSION, nullptr, f.flags});
        if (!Register(&infos.back())) dslog::Write("[fail] PSE: internal plugin %s not registered", f.name);
    }
}
void pse_host::RunAs(const char* plugin, void (*fn)()) {
    if (!fn) return;
    patch::OwnerScope scope(Find(plugin));
    fn();
}
void pse_host::Shutdown() {
    if (g_core) pse::hostlink::Shutdown();
}

namespace {
// ds2fix.graphics v1 (src/pse/ds2fix_graphics.h) for the DS2 plugins: graphics.h behind a C table.
void PSE_CALL GfxSetVirtualScreen(int on, int w, int h) { graphics::SetVirtualScreen(on != 0, w, h); }
int PSE_CALL GfxVirtualScreenOn() { return graphics::VirtualScreenOn() ? 1 : 0; }
void PSE_CALL GfxRealToVirtual(long* x, long* y) {
    if (x && y) graphics::RealToVirtual(*x, *y);
}
int PSE_CALL GfxRealWidth() { return graphics::RealWidth(); }
int PSE_CALL GfxRealHeight() { return graphics::RealHeight(); }
float PSE_CALL GfxVirtualScale() { return graphics::VirtualScale(); }
void* PSE_CALL GfxDevice() { return graphics::Device(); }
static_assert(graphics::kVirtualW == DS2FIX_VIRTUAL_W && graphics::kVirtualH == DS2FIX_VIRTUAL_H);
const Ds2FixGraphicsV1 kGraphicsInterface = {sizeof(Ds2FixGraphicsV1), 1, &GfxSetVirtualScreen, &GfxVirtualScreenOn,
                                             &GfxRealToVirtual, &GfxRealWidth, &GfxRealHeight, &GfxVirtualScale,
                                             &GfxDevice};

// ds2fix.hudhost v1 (src/pse/ds2fix_hud.h) for the widescreen / HUD plugin ds2fix.widescreen: the split-screen view
// size its HUD scale follows, and the OPTIONS rows (menus.cpp) for its settings.
const Ds2FixHudOptionV1* g_hudOption = nullptr;
bool HudOptScaling() { return g_hudOption->Scaling() != 0; }
uint32_t HudOptPercent() { return g_hudOption->Percent(); }
void HudOptSet(bool on, uint32_t percent) { g_hudOption->Set(on ? 1 : 0, percent); }
void PSE_CALL HostSplitViewSize(int* w, int* h) {
    if (w && h) features::SplitViewSize(*w, *h);
}
void PSE_CALL HostSetHudOption(const Ds2FixHudOptionV1* option) {
    g_hudOption = option;
    features::menus::HudOption rows;
    if (option) {
        rows.scaling = &HudOptScaling;
        rows.percent = &HudOptPercent;
        rows.set = &HudOptSet;
    }
    features::menus::SetHudOption(rows);
}
const Ds2FixHudHostV1 kHudHostInterface = {sizeof(Ds2FixHudHostV1), 1, &HostSplitViewSize, &HostSetHudOption};

// ds2fix.options v1 (src/pse/ds2fix_options.h): plugins' ON / OFF rows in the OPTIONS screen (menus.cpp toggles). One
// trampoline per slot (menus' rows take plain functions).
struct OptionSlot {
    const Ds2FixToggleV1* toggle = nullptr;
    PSEPluginHandle owner = 0;
    int row = -1;
};
OptionSlot g_options[features::menus::kMaxToggles];
template <int I>
bool OptGet() { return g_options[I].toggle && g_options[I].toggle->Get() != 0; }
template <int I>
void OptSet(bool on) {
    if (g_options[I].toggle) g_options[I].toggle->Set(on ? 1 : 0);
}
bool (*const kOptGet[])() = {&OptGet<0>, &OptGet<1>, &OptGet<2>, &OptGet<3>};
void (*const kOptSet[])(bool) = {&OptSet<0>, &OptSet<1>, &OptSet<2>, &OptSet<3>};
static_assert(sizeof kOptGet / sizeof kOptGet[0] == features::menus::kMaxToggles);
int PSE_CALL OptionsAddToggle(PSEPluginHandle plugin, const Ds2FixToggleV1* toggle) {
    if (!toggle || !toggle->Get || !toggle->Set) return 0;
    for (int i = 0; i < features::menus::kMaxToggles; ++i)
        if (!g_options[i].toggle) {
            features::menus::Toggle t;
            t.label = toggle->label;
            t.description = toggle->description;
            t.get = kOptGet[i];
            t.set = kOptSet[i];
            g_options[i] = {toggle, plugin, -1};
            g_options[i].row = features::menus::AddToggle(t);
            if (g_options[i].row < 0) {
                g_options[i] = OptionSlot{};
                return 0;
            }
            dslog::Write("Menus: OPTIONS row %s added by a plugin", toggle->label ? toggle->label : "?");
            return 1;
        }
    return 0;
}
void PSE_CALL OptionsRemoveToggle(PSEPluginHandle plugin, const Ds2FixToggleV1* toggle) {
    for (auto& o : g_options)
        if (o.toggle == toggle && o.owner == plugin) {
            features::menus::RemoveToggle(o.row);
            o = OptionSlot{};
        }
}
const Ds2FixOptionsV1 kOptionsInterface = {sizeof(Ds2FixOptionsV1), 1, &OptionsAddToggle, &OptionsRemoveToggle};

// ds2fix.pads v1 (src/pse/ds2fix_pads.h): the game's joysticks read for menus (coop.cpp's raw reader).
int PSE_CALL PadsCount() { return features::PadCount(); }
uint32_t PSE_CALL PadsButtons(int j) { return features::PadMenuButtons(j); }
int PSE_CALL PadsKind(int j) { return features::PadKind(j); }
const Ds2FixPadsV1 kPadsInterface = {sizeof(Ds2FixPadsV1), 1, &PadsCount, &PadsButtons, &PadsKind};
// ds2fix.coopinfo: the co-op session for DS2 plugins - see ds2fix_pads.h (CoopPlayers / CoopJoystick)
int PSE_CALL CoopInfoPlayers() { return features::CoopPlayers(); }
int PSE_CALL CoopInfoJoystick(int player) { return features::CoopJoystick(player); }
const Ds2FixCoopV1 kCoopInterface = {sizeof(Ds2FixCoopV1), 1, &CoopInfoPlayers, &CoopInfoJoystick};
int PSE_CALL SkinsDress(void* soldier, const char* body, const char* texture) {
    return features::DressSoldier(soldier, body, texture) ? 1 : 0;
}
const Ds2FixSkinsV1 kSkinsInterface = {sizeof(Ds2FixSkinsV1), 1, &SkinsDress};

// One row per feature = one internal plugin; Apply runs inside its owner scope (dllmain.cpp's Init order).
// Widescreen modes, HUD scaling, the front end's virtual screen and sharper text are the external plugin
// ds2fix.widescreen (games/ds2/plugins/widescreen) since 2026-10-22.
const pse_host::Feature kFeatures[] = {
    {"ds2fix.cinematics", "Skip intro films", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyCinematics},
    {"ds2fix.menus", "Menus (co-op entry)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyMenus},
    {"ds2fix.splitscreen", "Split screen", PSE_PLUGIN_SIMULATION, &features::ApplySplitScreen},
    {"ds2fix.coop", "Co-op join screen", PSE_PLUGIN_SIMULATION, &features::ApplyCoop},
    {"ds2fix.weaponicons", "Weapon pictures from mods (.weapon hud icon)", PSE_PLUGIN_CLIENT_ONLY,
     &features::ApplyWeaponIcons},
    {"ds2fix.mods", "Mods folder", PSE_PLUGIN_SIMULATION, &features::ApplyMods},
    {"ds2fix.modlevels", "Levels from mods (mission DLLs, .level, game-data references, edited scenes)",
     PSE_PLUGIN_SIMULATION, &features::ApplyModLevels},
    {"ds2fix.skins", ".skin files (soldier textures, bodies, names)", PSE_PLUGIN_SIMULATION, &features::ApplySkins},
    {"ds2fix.modsound", "Mods folder sounds (bank packs, level sounds, music, films)", PSE_PLUGIN_CLIENT_ONLY,
     &features::ApplyModSounds},
    {"ds2fix.modsmenu", "Mods menu (MODS screen, mod count)", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyModsMenu},
    {"ds2fix.scripthost", "Lua mission scripts (.lua levels, custom campaigns, +level)", PSE_PLUGIN_SIMULATION,
     &features::ApplyScriptHost},
    {"ds2fix.graphics", "Direct3D hooks", PSE_PLUGIN_CLIENT_ONLY, &graphics::Install},
    {"ds2fix.devbg", "Dev background test mode", PSE_PLUGIN_CLIENT_ONLY, &features::ApplyDevBackground},
};
}  // namespace

extern "C" {
__declspec(dllexport) extern const PSEPluginInfo PSEPlugin_Info;
const char* const kGames[] = {"ds2", nullptr};
const PSEPluginInfo PSEPlugin_Info = {sizeof(PSEPluginInfo), 1, "ds2fix", "DesertStorm2Fix", "DesertStorm2Fix",
                                      pse_host::kDs2FixVersion, 0, kGames, PSE_PLUGIN_SIMULATION};

__declspec(dllexport) int PSE_CALL PSEPlugin_Load(const PSELoadInterface* pse) {
    dslog::Open();
    if (!pse_host::Init(pse)) {
        dslog::Write("[fail] ds2fix.dll needs PSE's pse.host (manifest host = true, game layer pse_ds2.dll)");
        return 0;
    }
    dslog::Write("DesertStorm2Fix " DS_VERSION " loaded as a PSE host plugin (game %s)", pse->gameId);
    dslog::Write("Game: CDS II.exe, %s", features::ExeName(features::IdentifyExe()));
    settings::Load();
    pse_host::RegisterFeatures(kFeatures, sizeof kFeatures / sizeof kFeatures[0]);
    for (const auto& f : kFeatures) pse_host::RunAs(f.name, f.apply);
    const auto* messaging = static_cast<const PSEMessagingInterface*>(pse->Query(pse->plugin, PSE_MESSAGING_INTERFACE, 1));
    if (!messaging || messaging->RegisterInterface(pse_host::Find("ds2fix.graphics"), DS2FIX_GRAPHICS_INTERFACE, 1,
                                                   &kGraphicsInterface) != PSE_OK)
        dslog::Write("[fail] PSE: interface %s not registered", DS2FIX_GRAPHICS_INTERFACE);
    if (!messaging || messaging->RegisterInterface(pse_host::Find("ds2fix.menus"), DS2FIX_HUDHOST_INTERFACE, 1,
                                                   &kHudHostInterface) != PSE_OK)
        dslog::Write("[fail] PSE: interface %s not registered", DS2FIX_HUDHOST_INTERFACE);
    if (!messaging || messaging->RegisterInterface(pse_host::Find("ds2fix.menus"), DS2FIX_OPTIONS_INTERFACE, 1,
                                                   &kOptionsInterface) != PSE_OK)
        dslog::Write("[fail] PSE: interface %s not registered", DS2FIX_OPTIONS_INTERFACE);
    if (!messaging || messaging->RegisterInterface(pse_host::Find("ds2fix.coop"), DS2FIX_PADS_INTERFACE, 1,
                                                   &kPadsInterface) != PSE_OK ||
        messaging->RegisterInterface(pse_host::Find("ds2fix.coop"), DS2FIX_COOP_INTERFACE, 1, &kCoopInterface) != PSE_OK)
        dslog::Write("[fail] PSE: interfaces %s / %s not registered", DS2FIX_PADS_INTERFACE, DS2FIX_COOP_INTERFACE);
    if (!messaging || messaging->RegisterInterface(pse_host::Find("ds2fix.skins"), DS2FIX_SKINS_INTERFACE, 1,
                                                   &kSkinsInterface) != PSE_OK)
        dslog::Write("[fail] PSE: interface %s not registered", DS2FIX_SKINS_INTERFACE);
#ifndef DS_DIST
    if (messaging) {
        messaging->RegisterListener(pse->plugin, nullptr, &OnMessage, nullptr);
        messaging->Subscribe(pse->plugin, PSE_EVENT_LEVEL_LOAD_END, &OnLevelLoaded, nullptr, 0);
    }
#endif
    return 1;
}
}
