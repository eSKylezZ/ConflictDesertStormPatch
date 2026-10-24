#pragma once
// Interface "dsfix.limits" v1, published by the plugin dsfix.limits (PSE\Plugins\dsfix.limits.dll) with
// pse.messaging RegisterInterface; find it with QueryInterface(self, DSFIX_LIMITS_INTERFACE, 1) after PSE_MSG_POST_LOAD
// (or later, at run time). NULL = the plugin isn't installed / its patches failed: the engine pools are the game's.
#include <cstdint>

#include "pse/pse_api.h"

#define DSFIX_LIMITS_INTERFACE "dsfix.limits"

typedef struct DsfixLimitsV1 {
    uint32_t size;
    uint32_t version;   // 1
    // Extra bytes the Global Heap needs for the enlarged engine pools (0 when the limits are not raised). Read by
    // dsfix.memory when the game creates the heap.
    uint32_t(PSE_CALL* HeapExtra)(void);
} DsfixLimitsV1;
