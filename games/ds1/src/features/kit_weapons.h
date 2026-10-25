// The squad's kit weapons with their models (catalog.dat Weaps.txt column 2) - shared by dsfix.dll (mods.cpp prepares
// their shot banks at start-up, before any plugin loads: features::KitWeaponIds, kitweapons.cpp) and dsfix.loadout
// (the loadout screen's choices). A weapon's model has to be in the level's own archive (<level>.dat) - one that isn't
// (the SPAS-12 in Mission 1) crashes the level load (0x4D081A).
#pragma once

enum class KitKind { Main, Pistol, Launcher, Grenade, Other };

struct KitWeapon {
    const char* item;
    const char* model;
    KitKind kind;
};

inline constexpr KitWeapon kKitWeapons[] = {
    {"US_WPN_M16A2", "Rifle02_M16wM203", KitKind::Main},
    {"US_WPN_M16", "Rifle03_ColtCommando", KitKind::Main},
    {"US_WPN_HKPSG1", "Sniper02_PSG1", KitKind::Main},
    {"US_WPN_BarrettSniper", "SNIPER01_BARRETL50", KitKind::Main},
    {"US_WPN_AccuracyInter", "SNIPER03_L96A1", KitKind::Main},
    {"US_WPN_M60E3", "LMG04_M60E3", KitKind::Main},
    {"US_WPN_SAW_lightmg", "LMG01_M249SAW", KitKind::Main},
    {"US_WPN_MP5SilencedSubMG", "SMG01_MP5SD3", KitKind::Main},
    {"US_WPN_Remmington870", "Shotgun01_REM870", KitKind::Main},
    {"US_WPN_FranchiSPAS", "Shotgun02_FranchiSPAS12", KitKind::Main},
    {"US_WPN_SIGP228", "Pistol03_SIGP228", KitKind::Pistol},
    {"US_WPN_Beretta92F", "Pistol02_Beretta92F", KitKind::Pistol},
    {"US_WPN_DesertEagle", "Pistol01_DesertEagle", KitKind::Pistol},
    {"US_WPN_LAW66", "LAW66_AntiTankLauncher", KitKind::Launcher},
    {"US_WPN_LAW80", "LAW80_AntiTankLauncher", KitKind::Launcher},
    {"US_WPN_FIM92", "AA01_FIM92Stinger", KitKind::Launcher},
    {"US_WPN_GrenadeFrag", "Grenade_Frag", KitKind::Grenade},
    {"US_WPN_GrenadeSmoke", "Grenade_Smoke", KitKind::Grenade},
};
