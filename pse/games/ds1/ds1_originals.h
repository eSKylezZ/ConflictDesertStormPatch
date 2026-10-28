#pragma once
// DS1 game layer: the game finds its files in <game>\PSE\Originals. DesertStormFix moves the original archives, sound
// caches and level script DLLs there once it has processed them (src/features/originals.cpp); this redirect lives in
// the game layer so the game still finds them when DesertStormFix isn't loaded (removed, switched off, failed).
//
//   0x5D8138 IAT CreateFileA, 0x5D80B0 CreateFileW, 0x5D810C GetFileAttributesA, 0x5D8104 FindFirstFileA,
//   0x5D8070 LoadLibraryA - pointer hooks, owner pse.game.ds1, priority OUTER: a path in the game folder whose file
//   isn't there but is in PSE\Originals opens that copy (a bare DLL name counts as the game folder, as Windows looks
//   there first). Wildcards and every other path are passed on unchanged; a file back in the game folder wins.
#include <cstdint>
#include <string>

#include "pse/pse_api.h"

namespace ds1_originals {
// Installs the five hooks (once). Returns the number that failed (each with a "[fail]" log line).
int Install(PSEPluginHandle self);
// The PSE\Originals path for a game-folder path whose file is there instead; "" = use the path as it is.
std::string Redirect(const char* path);

constexpr uint32_t kIatLoadLibraryA = 0x5D8070, kIatCreateFileW = 0x5D80B0, kIatFindFirstFileA = 0x5D8104,
                   kIatGetFileAttributesA = 0x5D810C, kIatCreateFileA = 0x5D8138;

// Tests only: the folders (the game's = the exe's) and "not installed".
void SetFoldersForTests(const std::string& game);
void ResetForTests();
}  // namespace ds1_originals
