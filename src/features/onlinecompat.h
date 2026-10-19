#pragma once
// Online plugin-set check, DesertStormFix side (docs/sdk/online-compat.md, pse/loader/compat.cpp): when an online peer
// connects, both PCs send their simulation plugins (name + version + DLL SHA-256 / built-in fingerprint), the loader
// API + game id, the DesertStormFix build and the switched-on mods (name + content hash) as "PSEC ..." game commands.
// Any difference refuses the join before a mission starts: the host doesn't START with (or mirror levels to) that
// player and drops it, the joiner ignores START and leaves; both online screens list the differences and the online
// log has them.
#include <cstdint>
#include <string>
#include <vector>

#include "core/net.h"

namespace onlinecompat {
enum class State { None, Waiting, Ok, Refused };

// Every frame from online.cpp (OnFrameOnline), before the commands: tracks connections, sends this PC's list to a new
// peer, times out a peer that sends none (an older build), drops / leaves after a refusal.
void Frame(const net::Snapshot& snap);
// A game command: true = it was a "PSEC" line of the check (used up here).
bool Command(const net::Command& c);
// The check's result for a player (1 = the host, as seen by a joiner; 2.. = joiners, as seen by the host).
State PeerState(int player);
// Host: every connected player passed. false + why ("player 2: checking plugins and mods ...") otherwise.
bool AllPassed(const net::Snapshot& snap, std::string* why);
// Lines for the HOST / JOIN screens: each player's (host) or the host's (joiner) result, the differences of a refusal
// (at most maxDiffs, then "... n more"). A refusal stays listed after the connection ended, until ClearNotice().
std::vector<std::string> StatusLines(bool host, size_t maxDiffs);
// The menus start a new session / leave the online screens.
void ClearNotice();
// Text of this PC's simulation fingerprint (DS_VERSION + the patch tables + kSimRevision), for logs.
std::string Fingerprint();
}  // namespace onlinecompat
