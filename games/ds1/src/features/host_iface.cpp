// Interface dsfix.host (src/generated/dsfix_host.h from src/core/dsfix_host.spec): what DesertStormFix's own plugin
// DLLs call in dsfix.dll, through the kit's client (plugins/kit/host_client.cpp). DesertStormFix-internal - the
// layout changes with the spec; plugins and dsfix.dll ship together. Registered next to dsfix.services.
#include <windows.h>

#include <string>
#include <vector>

#include "core/log.h"
#include "core/net.h"
#include "core/pse_host.h"
#include "features/features.h"
#include "features/padio.h"
#include "generated/dsfix_host.h"
#include "loader.h"

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
