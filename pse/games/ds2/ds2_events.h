#pragma once
// DS2 game layer: the hooks that drive PSE's core game events and messages - the same events / messages as DS1
// (docs/sdk/reference/game-ds2.md "Events and messages", messaging.md). Installed by PSEGame_Init through the hooks
// broker, owner pse.game.ds2.
//
//   site                                         priority  sends
//   0x414BFC call Main_FrameDispatch               INNER     PSE_MSG_FRONT_END_READY, pse::Frame (tasks + PSE_EVENT_FRAME)
//   0x410990 call Render_Frame(1)                  INNER     PSE_EVENT_PRE_RENDER / PSE_EVENT_POST_RENDER
//   0x42CF9B call FrontEnd_Init (thiscall)         OUTER     PSE_EVENT_LEVEL_LOAD_BEGIN / _END ("FrontEnd")
//   0x42CB64 call Level_Init(name)                 OUTER     PSE_MSG_NEW_GAME, PSE_EVENT_LEVEL_LOAD_BEGIN / _END (name)
//   0x42CF1E call Level_Unload                     OUTER     PSE_EVENT_LEVEL_UNLOAD (the loaded level's name)
//   0x40E8E0 WM_DESTROY: call 0x40D730             OUTER     PSE_EVENT_LEVEL_UNLOAD, PSE_MSG_EXIT + plugins unloaded
//   0x41C600 Save_Game (entry, 10-byte prologue)   OUTER     PSE_MSG_SAVE_BEGIN / _END + the co-save (ds2_saves)
//   0x41C7C0 Save_Load (entry, 10-byte prologue)   OUTER     PSE_MSG_LOAD_BEGIN / _END + the co-save
//   0x5DA2C4 IAT Direct3DCreate9                   OUTER     -> IDirect3D9::CreateDevice (16, OUTER): PSE_MSG_DEVICE_CREATED
//   device Reset (16, OUTER) / TestCooperativeLevel (3), Present (17), EndScene (42) (INNER): PSE_MSG_DEVICE_RESET /
//   _LOST, PSE_EVENT_PRESENT / PSE_EVENT_END_SCENE
// INNER = PSE_DS2_LAYER_PRIORITY_INNER (-1000), OUTER = PSE_DS2_LAYER_PRIORITY_OUTER (1000) - pse/pse_game_ds2.h.
#include <cstdint>
#include <string>

#include "pse/pse_api.h"

namespace ds2_events {
// Installs every hook (once; later calls return the first result). Returns the number of hook groups that failed (0 =
// all in; each failure has a "[fail]" log line - the game runs on without that event).
int Install(PSEPluginHandle self);

// For the tests: the sites / entries above (ids in pse/ids/ds2.ids.txt).
constexpr uint32_t kFrameCall = 0x414BFC, kFrameDispatch = 0x412040;
constexpr uint32_t kRenderCall = 0x410990, kRenderFrame = 0x40FBA0;
constexpr uint32_t kFrontEndInitCall = 0x42CF9B, kFrontEndInit = 0x42C930;
constexpr uint32_t kLevelInitCall = 0x42CB64, kLevelInit = 0x412690;
constexpr uint32_t kUnloadCall = 0x42CF1E, kLevelUnload = 0x42C8B0;
constexpr uint32_t kDestroyCall = 0x40E8E0, kDestroyTarget = 0x40D730;
constexpr uint32_t kSaveGame = 0x41C600, kLoadGame = 0x41C7C0;
constexpr uint32_t kIatDirect3DCreate9 = 0x5DA2C4;
// The first two instructions of Save_Game / Save_Load (the entry detour moves 10 bytes).
constexpr uint8_t kSavePrologue[] = {0x8B, 0x44, 0x24, 0x08, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00};  // mov eax,[esp+8]; sub esp,0x104
constexpr uint8_t kLoadPrologue[] = {0x8B, 0x44, 0x24, 0x04, 0x81, 0xEC, 0x04, 0x01, 0x00, 0x00};  // mov eax,[esp+4]; sub esp,0x104
// The scene name the front end loads under (FrontEnd_Init stores this literal as the world's scene name).
constexpr const char* kFrontEndName = "FrontEnd";

// UTF-16 path from the game -> UTF-8.
std::string Utf8(const wchar_t* path);

// Tests only: forget that Install ran and the COM class records (broker records are reset by the broker's own call).
void ResetForTests();
}  // namespace ds2_events
