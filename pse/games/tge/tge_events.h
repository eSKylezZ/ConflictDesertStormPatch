#pragma once
// TGE game layer: the hooks that drive PSE's core game events and messages - the same events / messages as DS1 / DS2
// (docs/sdk/reference/game-tge.md "Events and messages", messaging.md). Installed by PSEGame_Init through the hooks
// broker, owner pse.game.tge.
//
//   site                                          priority  sends
//   0x5D5988 call Main_FrameDispatch                INNER     PSE_MSG_FRONT_END_READY, pse::Frame (tasks + PSE_EVENT_FRAME)
//   0x5D5692 call Render_Frame(int)                 INNER     PSE_EVENT_PRE_RENDER / PSE_EVENT_POST_RENDER
//   0x5D0DE0 Screen_Load (entry, 6-byte prologue)   OUTER     LEVEL_UNLOAD (previous), LEVEL_LOAD_BEGIN / _END (entry name: "FE", "FMV02" ...)
//   0x5D1DE0 Level_Init(name) (entry, 9 bytes)      OUTER     LEVEL_LOAD_BEGIN / _END (the mission's "Level01.dll" ...)
//   0x5CEE10 Level_Unload (entry, 6 bytes)          OUTER     PSE_EVENT_LEVEL_UNLOAD (the loaded level's name)
//   0x5D0089 WM_DESTROY: call App_Shutdown          OUTER     PSE_EVENT_LEVEL_UNLOAD, PSE_MSG_EXIT + plugins unloaded
//   0x5F52A4 IAT Direct3DCreate8                    OUTER     -> IDirect3D8::CreateDevice (15, OUTER): PSE_MSG_DEVICE_CREATED
//   device Reset (14, OUTER) / TestCooperativeLevel (3), Present (15), EndScene (35) (INNER): PSE_MSG_DEVICE_RESET /
//   _LOST, PSE_EVENT_PRESENT / PSE_EVENT_END_SCENE
// Not yet: saves (no SAVE / LOAD messages, no co-save) and PSE_MSG_NEW_GAME.
// INNER = PSE_TGE_LAYER_PRIORITY_INNER (-1000), OUTER = PSE_TGE_LAYER_PRIORITY_OUTER (1000) - pse/pse_game_tge.h.
#include <cstdint>

#include "pse/pse_api.h"

namespace tge_events {
// Installs every hook (once; later calls return the first result). Returns the number of hooks that failed (0 = all in;
// each failure has a "[fail]" log line - the game runs on without that event).
int Install(PSEPluginHandle self);

// The sites / entries above (ids in pse/ids/tge.ids.txt).
constexpr uint32_t kFrameCall = 0x5D5988, kFrameDispatch = 0x5D5810;
constexpr uint32_t kRenderCall = 0x5D5692, kRenderFrame = 0x5D18D0;
constexpr uint32_t kScreenLoad = 0x5D0DE0, kLevelInit = 0x5D1DE0, kLevelUnload = 0x5CEE10;
constexpr uint32_t kShutdownCall = 0x5D0089, kShutdown = 0x5CCA80;
constexpr uint32_t kIatDirect3DCreate8 = 0x5F52A4;
constexpr uint8_t kScreenLoadPrologue[] = {0x81, 0xEC, 0x00, 0x02, 0x00, 0x00};                    // sub esp, 0x200
constexpr uint8_t kLevelInitPrologue[] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0xB0, 0x02, 0x00, 0x00};    // push ebp; mov ebp, esp; sub esp, 0x2B0
constexpr uint8_t kUnloadPrologue[] = {0x8B, 0x0D, 0x20, 0xED, 0x79, 0x00};                        // mov ecx, [Renderer]
// The front end's level-table entry name.
constexpr const char* kFrontEndName = "FE";

// The name Screen_Load / Mission_Load are about to load (Level_TableIndex -> table entry name, or the name buffers).
const char* PendingLevelName();
}  // namespace tge_events
