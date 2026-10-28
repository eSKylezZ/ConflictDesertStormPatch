// features::KitWeaponIds in dsfix.dll: mods.cpp's start-up pass (BorrowKitBanks) needs it before the plugins load;
// dsfix.loadout asks it through dsfix.host.
#include <string>
#include <vector>

#include "features/features.h"
#include "features/kit_weapons.h"

std::vector<std::string> features::KitWeaponIds() {
    std::vector<std::string> out;
    for (const KitWeapon& w : kKitWeapons) out.push_back(w.item);
    for (const std::string& w : ModWeapons()) out.push_back(w);
    return out;
}
