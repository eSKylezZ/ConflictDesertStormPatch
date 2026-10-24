#pragma once
// Interface "dsfix.services" (published by DesertStormFix's host plugin dsfix.dll, messaging RegisterInterface): what
// DesertStormFix features in their own plugin DLLs (games/ds1/plugins, the dsfix kit) need from dsfix.dll itself.
// Query it in PSEPlugin_Load (dsfix loads first: it is the host plugin); the manifest says `requires = dsfix >= 3.3`.
//
//   const DsfixServicesV1* s = static_cast<const DsfixServicesV1*>(
//       messaging->QueryInterface(self, DSFIX_SERVICES_INTERFACE, 1));
//   if (s && s->size >= sizeof(DsfixServicesV1)) ... s->GameText(hash) ...
//
// Version 1 (2026-10-24). Newer versions only add members at the end (check `size`). The providers are called from
// the game thread without the crash guard (like every cross-plugin call); dsfix drops them all at PSE_MSG_EXIT.
#include <cstdint>

#include "pse/pse_api.h"

#define DSFIX_SERVICES_INTERFACE "dsfix.services"

// Trigger feel of a pad player's weapon (DualSense adaptive triggers, Xbox trigger motors, full pull to fire).
enum DsfixTriggerFeel {
    DSFIX_FEEL_DEFAULT = 0,   // the one feel for every weapon (the feature is off)
    DSFIX_FEEL_NONE = 1,      // nothing to fire (empty hands, grenades, knife ...)
    DSFIX_FEEL_LIGHT = 2,
    DSFIX_FEEL_RIFLE = 3,
    DSFIX_FEEL_HEAVY = 4,
    DSFIX_FEEL_MACHINE_GUN = 5,
    DSFIX_FEEL_LAUNCHER = 6
};

// Puddle reflections (dsfix.dll's D3D9 renderers 1-3, render/water.cpp): one piece of a puddle decal - the game's own
// decal geometry (world space, 0x24-byte vertices = FVF 0x152) - and how the water looks.
typedef struct DsfixPuddlePart {
    const void* vertices;
    int vertexCount;
    const uint16_t* indices;
    int indexCount;
    float opacity;            // 0..1 (drying)
} DsfixPuddlePart;
typedef struct DsfixWaterLook {
    float sky[3];             // reflected where the mirrored point is off the view
    float fogColour[3];
    float fogStart, fogEnd;   // cm
    float timeSeconds, rain;  // ripples
    void* mask;               // the puddle picture (the game's D3D8 texture; alpha = water)
} DsfixWaterLook;

// Text for a catalog hash before the game's own catalog (dsfix's text lookup, the hook on FUN_004f6730), or NULL.
// The returned text must stay valid until the next call.
typedef const char*(PSE_CALL* DsfixTextProvider)(uint32_t hash);
// The trigger feel for player 0-3 (a DsfixTriggerFeel).
typedef int(PSE_CALL* DsfixTriggerFeelProvider)(int player);
// The screen object (the game's front-end screen class, vtable included) for a front-end state DesertStormFix itself
// doesn't serve (the state -> screen hook on FUN_00480930), or NULL. Asked every time the game looks the state up.
typedef void*(PSE_CALL* DsfixScreenProvider)(uint32_t state);

typedef struct DsfixServicesV1 {
    uint32_t size;      // sizeof(DsfixServicesV1) of the publishing dsfix.dll
    uint32_t version;   // 1
    // The game's own text for a catalog hash in the current language (NULL = none / no catalog yet).
    const char*(PSE_CALL* GameText)(uint32_t hash);
    // The icon character of player 0-3's button for a control action (default.key action number), NULL = no icon.
    const char*(PSE_CALL* ControlIconText)(int player, uint32_t action);
    // 1 while an online mission runs (each PC's views differ; local-only shortcuts must stay off).
    int(PSE_CALL* OnlineMission)(void);
    // The host's value of a synced simulation setting while an online mission runs, else -1 (also: not sent).
    // Names: "jpfixes", "followers", "weather" (the weather kind, RANDOM resolved by the host), "timeofday_on",
    // "timeofday", "timeflow" (the host's time-of-day choices; sent together), "voices" (accents on),
    // "voice0" .. "voice3" (the host's VOICE choice per squad role).
    int(PSE_CALL* HostValue)(const char* name);
    // Changes whenever the Mods folder is (re)read (MODS menu hot reload): cached mod data is stale then.
    int(PSE_CALL* ModsGeneration)(void);
    // Mod vehicles' handling (.vehicle grip / spin damping): count, then entry i (model upper case; < 0 = the game's).
    uint32_t(PSE_CALL* VehicleHandlingCount)(void);
    int(PSE_CALL* VehicleHandlingAt)(uint32_t i, const char** model, float* grip, float* spin);
    // One text provider per plugin (NULL removes it); providers are asked in the order they were set.
    PSEResult(PSE_CALL* SetTextProvider)(PSEPluginHandle plugin, DsfixTextProvider fn);
    // The trigger feel provider (one; the last one set wins, NULL removes it when it is the caller's).
    PSEResult(PSE_CALL* SetTriggerFeelProvider)(PSEPluginHandle plugin, DsfixTriggerFeelProvider fn);
    // DesertStormFix's own bool / int setting by registry name (plugin "dsfix"'s key, e.g. "FeatLoadout"), 0 if unknown.
    int(PSE_CALL* DsfixSetting)(const char* key);
    // Mods (mods.cpp): is the Mods folder read; the body / normal map a .skin gives the soldiers wearing `texture`
    // ("" = none; valid until the next call on that thread); a line for the log and Mods\DesertStormFix-report.txt
    // ("[fail] ..." counts as a mod problem on the main menu).
    int(PSE_CALL* ModsFolderEnabled)(void);
    const char*(PSE_CALL* SkinBody)(const char* texture);
    const char*(PSE_CALL* SkinNormalMap)(const char* texture);
    void(PSE_CALL* ModReport)(const char* line);
    // The briefing / loadout screen is up (customise.cpp).
    int(PSE_CALL* LoadoutScreenOpen)(void);
    // A weapon made from the level's own scene record, in the soldier's hands with `mags` magazines (no unlock); a
    // MediKit the same way. NULL / 0 when the level has no record to copy (loadout.cpp).
    void*(PSE_CALL* GiveSpawnedWeapon)(void* soldier, const char* weapon, int mags);
    int(PSE_CALL* GiveSpawnedMedikit)(void* soldier);
    // fn() runs after DesertStormFix's own settings changed in the launcher (its OK, before the game starts; also
    // Set to Defaults) - re-read what DsfixSetting gives. One per plugin, NULL removes it.
    PSEResult(PSE_CALL* SetSettingsListener)(PSEPluginHandle plugin, void(PSE_CALL* fn)(void));
    // The game window is in front (or DesertStormFix's run-in-background setting / test mode treats it so).
    int(PSE_CALL* GameFocused)(void);
    // dev8 = the game's D3D8 device: the puddles' reflections into the current view (after the transparents). 0 when
    // the device isn't dsfix's D3D9 one (Renderer 0: plain decals) or the shaders failed.
    int(PSE_CALL* DrawPuddleReflections)(void* dev8, const DsfixPuddlePart* parts, int count, const DsfixWaterLook* look);
    // The level this one continues from (linked missions: Mission4 -> Mission5A ...; customise.cpp), "" if none.
    // Valid until the next call.
    const char*(PSE_CALL* LinkedFrom)(void);
    // The voice bank a .skin gives the soldiers wearing `texture` ("" = none; valid until the next call on that thread),
    // and the game's speech language: 0 UK / 6 US English, -1 another language (one voice per soldier, no accents).
    const char*(PSE_CALL* SkinVoice)(const char* texture);
    int(PSE_CALL* SpeechLanguage)(void);
    // One screen provider per plugin (NULL removes it), asked in the order they were set after DesertStormFix's own
    // states (dsfix.developer: CHEATS > DEVELOPER, state 0x46).
    PSEResult(PSE_CALL* SetScreenProvider)(PSEPluginHandle plugin, DsfixScreenProvider fn);
} DsfixServicesV1;
