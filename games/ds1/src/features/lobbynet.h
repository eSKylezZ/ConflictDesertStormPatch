#pragma once
// The co-op lobby over the network (lobbynet.cpp) for the lobby screen (lobbymenu.cpp) and the console. The table rules
// and wire format: lobby.h. Other features use the features:: Lobby* functions (features.h).
#include <string>

#include "features/lobby.h"

namespace lobbynet {
const lobby::Table& Current();   // the host's table (empty without a session / before the host's arrived)
bool IsHost();
int MyPc();                      // this PC's number in the table (host 1), 0 = no session
// A change to this PC's players ("LB J/L/N/C/V/R/D ...", lobby.h): the host applies it at once, a joiner sends it to
// the host (the table that comes back shows the result). false = refused here / no session / frozen.
bool Request(const std::string& text);
// This PC's k-th player's remembered name (settings "PlayerName<k+1>", else the Windows user name + number)
std::string DefaultName(int k);
void RememberName(int k, const std::string& name);
int LocalIndex(int player);      // k of a player among this PC's players (table order), -1 not ours
}  // namespace lobbynet
