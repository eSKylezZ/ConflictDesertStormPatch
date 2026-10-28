#pragma once
// Interface "dsfix.timeofday" (published by the plugin dsfix.timeofday, PSE\Plugins\dsfix.timeofday.dll): what
// dsfix.dll asks of the time of day - the loadout screen's TIME OF DAY / TIME FLOW rows (customise.cpp) and the host's
// choices for an online mission (hostsync.cpp). Without the plugin dsfix.dll shows no rows and sends "mission default".
// Version 1 (2026-10-24); later versions only add members at the end (check `size`).
#include <cstdint>

#include "pse/pse_api.h"

#define DSFIX_TIMEOFDAY_INTERFACE "dsfix.timeofday"
// TIME OF DAY: 0 mission default, 1 dawn, 2 day, 3 dusk, 4 night. TIME FLOW: 0 mission default, 1-4 one game hour per
// 10 / 5 / 2 / 1 real minutes.
#define DSFIX_TIMEOFDAY_COUNT 5
#define DSFIX_TIMEFLOW_COUNT 5

typedef struct DsfixTimeOfDayV1 {
    uint32_t size;      // sizeof(DsfixTimeOfDayV1) of the publishing plugin
    uint32_t version;   // 1
    const char*(PSE_CALL* TimeLabel)(int i);
    const char*(PSE_CALL* FlowLabel)(int i);
    int(PSE_CALL* Time)(void);                 // the remembered choices
    int(PSE_CALL* Flow)(void);
    void(PSE_CALL* Set)(int time, int flow);   // applied at once in a mission, remembered (not on a linked level)
    int(PSE_CALL* Continues)(void);            // this linked level continues the previous one's hour
    void(PSE_CALL* Rule)(void);                // apply the choices now (the briefing screen opened)
} DsfixTimeOfDayV1;
