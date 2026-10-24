// pse.game.deniedops v1 (public SDK header, docs/sdk/reference/game-deniedops.md). Read-only interface served by the
// Conflict: Denied Ops game layer (PSE\Games\pse_deniedops.dll), registered as the built-in plugin "pse.game.deniedops".
// Unstable (API version 0): may change until SDK 1.0. Plain C, 32-bit x86, __cdecl - see pse_api.h "ABI rules".
#ifndef PSE_GAME_DENIEDOPS_H
#define PSE_GAME_DENIEDOPS_H

#include "pse_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PSE_GAME_DENIEDOPS_PLUGIN "pse.game.deniedops"
#define PSE_GAME_DENIEDOPS_INTERFACE "pse.game.deniedops"
#define PSE_GAME_DENIEDOPS_INTERFACE_VERSION 1u

typedef struct PSEGameDeniedOpsV1 {
    uint32_t size;   /* sizeof(PSEGameDeniedOpsV1) */
    uint32_t version; /* 1 */
    /* Upper-case hex SHA-256 of the running exe file (LAA bit cleared). Never empty once the layer is attached. */
    const char* (PSE_CALL* ExeSha256)(void);
    /* 1 if the running exe matches the one build this layer was mapped on (the Steam no-CD ConflictDeniedOps.exe). */
    int(PSE_CALL* ExeKnown)(void);
} PSEGameDeniedOpsV1;

#ifdef __cplusplus
}
#endif
#endif
