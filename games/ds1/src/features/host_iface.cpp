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
#include "features/padio.h"
#include "generated/dsfix_host.h"
#include "loader.h"

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
