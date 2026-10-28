// dsfix.mods: the Mods folder (mods.cpp: index, resolver, merged tables, sound caches, record edits, the .weapon /
// .skin / .vehicle / .gear / .unit files), the shared asset store + its pse.vfs provider (assetstore.cpp), mod weapon
// pictures (weaponicons.cpp), the MODS menu (modsmenu.cpp),
// custom campaigns (campaigns.cpp) and the kit weapon list (kitweapons.cpp) - one plugin DLL since 2026-10-25. What
// dsfix.dll calls here is the interface dsfix.mods (mods.spec); dsfix.dll keeps the mod report and whether the Mods
// folder is read at all (features/modhost.cpp), the original game files (originals.cpp) and the Global Heap (memory.cpp:
// dsfix.limits' enlarged pools come out of it - it asks LargestModFileSize here). Every other DesertStormFix
// plugin that asks for mod data loads after this one (`after = dsfix.mods` in its manifest). Without it: the game's own
// files only - no mods, no asset store, no MODS / CUSTOM CAMPAIGNS screens.
#include <windows.h>

#include <string>
#include <vector>

#include "features/features.h"
#include "kit/kit.h"
#include "mods/generated/mods_iface.h"

#include "mods/generated/mods_export.inc"

namespace {
bool Load() {
    // First: dsfix.dll's wrappers reach this plugin from now on - its own Apply calls below already go through some of
    // them (kit weapon ids for the sound banks, the mod files for the asset store's fallback ...).
    if (!RegisterModsInterface()) return false;
    // (dsfix.dll's kFeatures order before the move: mods menu, Mods folder, asset store, memory, weapon icons, ...,
    // campaigns)
    features::ApplyModsMenu();
    features::ApplyMods();
    features::ApplyAssetStore();
    features::ApplyWeaponIcons();
    features::ApplyCampaigns();
    return true;
}
}  // namespace

DSFIX_KIT_PLUGIN("dsfix.mods", "Mods folder, asset store, MODS menu, custom campaigns", PSE_PLUGIN_SIMULATION, Load)
