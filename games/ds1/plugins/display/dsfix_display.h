#pragma once
// Interface "dsfix.display" (published by the plugin dsfix.display, PSE\Plugins\dsfix.display.dll): what dsfix.dll asks
// of the display mode - OPTIONS > VIDEO's live mode change (graphics.cpp ChangeDisplayMode) places the window for the
// new mode before the device reset. Without the plugin the game stays fullscreen and the window isn't moved.
// Version 1 (2026-10-24); later versions only add members at the end (check `size`).
#include <cstdint>

#include "pse/pse_api.h"

#define DSFIX_DISPLAY_INTERFACE "dsfix.display"

typedef struct DsfixDisplayV1 {
    uint32_t size;      // sizeof(DsfixDisplayV1) of the publishing plugin
    uint32_t version;   // 1
    // The window for a w x h mode in display mode `mode` (0 fullscreen, 1 windowed, 2 borderless).
    void(PSE_CALL* PlaceGameWindow)(uint32_t w, uint32_t h, uint32_t mode);
} DsfixDisplayV1;
