#pragma once
// PSE host link (static library pse_hostlink, PRIVATE - docs/sdk/reference/plugins.md "Host plugins").
//
// A trusted host plugin (DesertStormFix's dsfix.dll) and the game layers (PSE\Games\pse_<game>.dll) are compiled
// against the loader's internal C++ header loader.h - the same declarations the phase-1 single DLL used. This library
// implements those declarations for a DLL that is NOT the loader: every call goes through the private "pse.host"
// table (host_api.h) to the one loader instance in pse_loader.dll. Pure helpers (x86 length decoder, SHA-256 / online
// compat encoding, the registry migration copy, string helpers) are compiled in as they are.
//
// Never link this together with pse_core (one or the other).
#include "loader.h"

namespace pse::hostlink {

// In PSEPlugin_Load / PSEGame_Init, before any other pse:: call: fetches "pse.host" through the plugin's own Query.
// false = the loader didn't hand it out (not a host plugin on the game layer's list, or an older / newer loader).
bool Attach(const PSELoadInterface* pse);
bool Attached();
// The caller's own handle (the PSELoadInterface's plugin).
PSEPluginHandle Self();
const PSEHostInterface* Host();
// Window destroyed: PSE_MSG_EXIT + external plugins unloaded (pse.host Shutdown).
void Shutdown();

}  // namespace pse::hostlink
