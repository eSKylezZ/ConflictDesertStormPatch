#pragma once
#include <cstdint>

// Frame recorder: one CSV row per game frame - where the time went (frame-cap wait, V-Sync wait, present, render,
// logic, overlay) and what the game did (files opened, textures created, level loading, cutscene, menus).
// Ctrl+F11 starts / stops a recording in any build; Dev\FrameRecord = 1 records from the start (dev builds).
// File: DesertStormFix-frames-<date>-<time>.csv next to the exe.
namespace framerec {
bool Recording();
void Toggle();
double Now();  // ms, QPC

// Frame boundary (frame-cap hook, after its wait): closes the previous frame's row.
void BeginFrame(double capWaitMs);
// Events inside the frame.
void AddRender(double ms);                      // the render loop call (includes presents made inside it)
void AddPresent(double vblankWaitMs, double presentMs);
void AddFile(const char* name, double ms, bool found);
void AddTexture(uint32_t w, uint32_t h, uint32_t format, double ms);
void AddOverlay(double ms);                     // performance overlay redraw (GDI) - measured so it can be told apart
void Note(const char* text);                    // free-form note for this frame's row

void ApplyHooks();  // render loop timing hook
}  // namespace framerec
