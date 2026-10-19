#pragma once
// DS1 game layer: the hooks that drive PSE's core game events and messages (docs/sdk/reference/game-ds1.md "Events and
// messages", messaging.md). Installed by PSEGame_Init through the hooks broker, owner pse.game.ds1, so every plugin gets
// frames, render / D3D events, level events and the co-save whether DesertStormFix is installed or not.
//
//   site                                    priority  sends
//   0x4116EC call Main_FrameDispatch         INNER     pse::Frame (tasks + PSE_EVENT_FRAME), PSE_MSG_FRONT_END_READY
//   0x40FD9E call Render_Frame(1)            INNER     PSE_EVENT_PRE_RENDER / PSE_EVENT_POST_RENDER
//   0x40E05D call Level_Init(name)           OUTER     PSE_MSG_NEW_GAME, PSE_EVENT_LEVEL_LOAD_BEGIN / _END
//   0x40DF18, 0x411404 call Level_Unload     OUTER     PSE_EVENT_LEVEL_UNLOAD
//   0x415650 Save_Game (entry)               OUTER     PSE_MSG_SAVE_BEGIN / _END + the co-save (ds1_saves)
//   0x415B60 Save_Load (entry)               OUTER     PSE_MSG_LOAD_BEGIN / _END + the co-save
//   0x5D8308 IAT Direct3DCreate8             OUTER     -> IDirect3D8::CreateDevice (OUTER): PSE_MSG_DEVICE_CREATED
//   device Reset (OUTER) / TestCooperativeLevel, EndScene, Present (INNER): PSE_MSG_DEVICE_RESET / _LOST,
//                                                       PSE_EVENT_END_SCENE / PSE_EVENT_PRESENT
// INNER = PSE_DS1_LAYER_PRIORITY_INNER (-1000), OUTER = PSE_DS1_LAYER_PRIORITY_OUTER (1000) - pse/pse_game_ds1.h.
#include <cstdint>

#include "pse/pse_api.h"

namespace ds1_events {
// Installs every hook (once; later calls return the first result). Returns the number of hook groups that failed (0 =
// all in; each failure has a "[fail]" log line - the game runs on without that event).
int Install(PSEPluginHandle self);

// For the tests: the sites / entries above.
constexpr uint32_t kFrameCall = 0x4116EC, kFrameDispatch = 0x40DE00;
constexpr uint32_t kRenderCall = 0x40FD9E, kRenderFrame = 0x40EF70;
constexpr uint32_t kLevelInitCall = 0x40E05D, kLevelInit = 0x40C090;
constexpr uint32_t kUnloadCalls[] = {0x40DF18, 0x411404};
constexpr uint32_t kLevelUnload = 0x40D990;
constexpr uint32_t kSaveGame = 0x415650, kLoadGame = 0x415B60;
constexpr uint32_t kIatDirect3DCreate8 = 0x5D8308;
// The first bytes of Save_Game / Save_Load (checked before the entry detour).
constexpr uint8_t kSavePrologue[] = {0xA1, 0xC0, 0xED, 0x60, 0x00};   // mov eax, [0x60EDC0]
constexpr uint8_t kLoadPrologue[] = {0x8B, 0x44, 0x24, 0x04, 0x53};   // mov eax, [esp+4]; push ebx

// Tests only: forget that Install ran and the COM class records (broker records are reset by the broker's own call).
void ResetForTests();
}  // namespace ds1_events
