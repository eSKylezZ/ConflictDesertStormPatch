// Interface dsfix.host (src/generated/dsfix_host.h from src/core/dsfix_host.spec): what DesertStormFix's own plugin
// DLLs call in dsfix.dll, through the kit's client (plugins/kit/host_client.cpp). DesertStormFix-internal - the
// layout changes with the spec; plugins and dsfix.dll ship together. Registered next to dsfix.services.
#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "core/pse_host.h"
#include "core/settings.h"
#include "features/features.h"
#include "features/overlay.h"
#include "features/framerec.h"
#include "features/padio.h"
#include "render/d3d8to9.h"
#include "generated/dsfix_host.h"
#include "loader.h"
#include "lua_iface.h"

// Not plain C as they are, so through these (dsfix_host.spec): DesertStormFix's settings struct (plain data) and the
// online check's list of loaded plugins (lines "name<TAB>version<TAB>flags<TAB>hash"; dsfix.online parses them back).
const settings::Values* features::DsfixSettingsValues() { return &settings::Get(); }

std::string features::CompatLocalPluginsText() {
    std::string out;
    for (const pse::compat::PluginEntry& e : pse::compat::LocalPlugins()) {
        char num[32];
        snprintf(num, sizeof num, "\t%u\t%u\t", e.version, e.flags);
        if (!out.empty()) out += '\n';
        out += e.name + num + e.hash;
    }
    return out;
}

// The loader gives its private pse.lua.host only to host plugins; dsfix.dll passes it on to its own mission plugin
// (dsfix.missions owns the Lua state of .lua levels and translated missions).
const void* features::LuaHostTable() {
    const PSELoadInterface* li = pse::LoadInterfaceFor(pse_host::Core());
    return li && li->Query ? li->Query(li->plugin, PSE_LUA_HOST_INTERFACE, 1) : nullptr;
}

// For dsfix.loadout (plugins/loadout/glue.cpp turns the rows back into the structs).
void features::BorrowVehicleDataList(const char* level, const std::string& units) {
    std::vector<std::string> list;
    for (size_t pos = 0; pos <= units.size();) {
        size_t end = units.find('\n', pos);
        if (end == std::string::npos) end = units.size();
        if (end > pos) list.push_back(units.substr(pos, end - pos));
        pos = end + 1;
    }
    BorrowVehicleData(level, list);
}
void features::SetLoadoutSpin(bool on) { settings::SetToggle(&settings::Values::featLoadoutSpin, on); }

namespace {
void Row(std::string& out, std::initializer_list<std::string> fields) {
    bool first = true;
    for (const std::string& f : fields) {
        if (!first) out += '\t';
        out += f;
        first = false;
    }
    out += '\n';
}
}  // namespace
std::string features::ModGearRows() {
    std::string out;
    for (const ModGearPiece& g : ModGear()) Row(out, {g.model, std::to_string(g.kind), g.name});
    return out;
}
std::string features::ModPouchSetRows() {
    std::string out;
    for (const ModPouchSet& p : ModPouchSets())
        Row(out, {p.name, p.model[0], p.model[1], p.model[2], p.model[3], p.model[4], p.model[5], p.model[6]});
    return out;
}
std::string features::ModVehicleRows() {
    std::string out;
    for (const ModVehicle& v : ModVehicles()) Row(out, {v.id, v.name, v.base, v.faction});
    return out;
}
std::string features::SkinVariantRows() {
    std::string out;
    for (const SkinVariant& v : SkinVariants()) Row(out, {std::to_string(v.uniform), std::to_string(v.role), v.texture, v.name});
    return out;
}

#include "generated/dsfix_host_impl.inc"

void features::RegisterHostInterface() {
    static bool done = false;
    if (done) return;
    done = true;
    const PSEMessagingInterface* m = pse::MessagingInterface();
    const bool ok = m && m->RegisterInterface(pse_host::Core(), DSFIX_HOST_INTERFACE, 1, &kTable) == PSE_OK;
    dslog::Write(ok ? "[ok]   " DSFIX_HOST_INTERFACE " 1: registered (layout %08X)" : "[fail] " DSFIX_HOST_INTERFACE ": not registered",
                 DSFIX_HOST_LAYOUT);
}
