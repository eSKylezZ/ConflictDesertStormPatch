#pragma once
// Interface "dsfix.voices" (published by the plugin dsfix.voices, PSE\Plugins\dsfix.voices.dll): what dsfix.dll asks
// of the squad voices - the loadout screen's VOICE row (customise.cpp) and the host's co-op choices for an online
// mission (hostsync.cpp). Without the plugin dsfix.dll shows no VOICE row and sends AUTO for every role.
// Version 1 (2026-10-24); later versions only add members at the end (check `size`).
#include <cstdint>

#include "pse/pse_api.h"

#define DSFIX_VOICES_INTERFACE "dsfix.voices"
// VOICE: 0 AUTO (the uniform's accent, or the skin's own voice), 1 SAS, 2 DELTA, 3 OWN (MOD).
#define DSFIX_VOICE_COUNT 4

typedef struct DsfixVoicesV1 {
    uint32_t size;      // sizeof(DsfixVoicesV1) of the publishing plugin
    uint32_t version;   // 1
    const char*(PSE_CALL* Label)(int v);
    int(PSE_CALL* Choice)(int role);            // squad role 0-3 (online: the host's)
    void(PSE_CALL* SetChoice)(int role, int v);  // remembered for the current mode (single player / co-op)
    int(PSE_CALL* CoopChoice)(int role);        // what this PC would send as host (its co-op choices)
    // What the VOICE row shows for a soldier ("AUTO - DELTA" ...), into buf (always NUL-terminated); its length.
    uint32_t(PSE_CALL* Text)(void* soldier, int role, char* buf, uint32_t bufLen);
    int(PSE_CALL* HasOwnVoice)(void* soldier);  // his skin brings a voice (.skin "voice")
} DsfixVoicesV1;
