#pragma once
#include <cstdint>
#include <string>

// Stored through pse.settings, plugin "dsfix": HKCU\Software\PSE\ds1\Plugins\dsfix (docs/sdk/ds1-settings.md).
// Until 2026-10-19 they lived in HKLM\SOFTWARE\Pivotal Games\Conflict Desert Storm\Enhancements (virtualized per
// user like the game's keys); Load() copies that key once (marker MigratedFromEnhancements), the old one stays.
// Changed from the launcher dialog / in-game Options menu, never from files.
namespace settings {
struct Values {
    bool hudScaling = true;
    uint32_t hudScalePercent = 100;  // on top of the automatic min(W/800, H/600)
    bool controller = true;
    uint32_t padDeadzone = 40;       // percent
    uint32_t fpsCap = 240;           // 30..240 (240 = the most the game runs at; older 0 = "unlimited" reads as 240)
    bool fpsCapToRefresh = true;     // also never exceed the monitor refresh rate
    uint32_t splitScreenLayout = 0;  // splitscreen::Layout: 0 horizontal (top / bottom), 1 vertical (side by side)
    uint32_t displayMode = 0;        // display::Mode: 0 fullscreen, 1 windowed, 2 borderless window
    bool skipIntro = false;          // no publisher / developer logo slideshow at start-up
    bool skipCutscenes = false;      // mission cutscenes end as soon as they start (Esc / pad skip them anyway)
    uint32_t antialiasing = 0;       // MSAA samples (0 = off; the highest supported count <= this is used)
    uint32_t anisotropy = 16;        // anisotropic filtering level for linear-filtered textures (0 = off)
    // MouseAcceleration: a setting of the plugin dsfix.mouse (own DLL since 2026-10-23)
    bool vsync = true;               // fullscreen: present on the monitor's refresh (no tearing / uneven frames)
    bool perfOverlay = false;        // performance overlay (FPS, frame times, level / zone / position); F11 toggles it
    // Release builds stop at 2: Renderer 3 (own DXGI presenter, HDR, bloom) can hang the GPU on explosions - dev only
    // until fixed (TODO.md "Modern renderer notes").
#ifdef DS_DIST
    static constexpr uint32_t kMaxRenderer = 2;
#else
    static constexpr uint32_t kMaxRenderer = 3;
#endif
    uint32_t renderer = 0;           // 0 the game's Direct3D 8, 1 Direct3D 9 (src/render), 2 Direct3D 9 on 12, 3 + own DXGI presenter
    bool hdr = false;                // Renderer 3 + an HDR display: scRGB output, highlights expanded (src/render/hdr.hlsl)
    uint32_t hdrPaperWhite = 200;    // nits for SDR white (HUD, menus, mid-tones)
    uint32_t hdrPeak = 0;            // nits for the brightest highlights (0 = the display's reported peak)
    uint32_t hdrHighlights = 100;    // percent of the way from paper white to the peak
    uint32_t bloom = 0;              // Renderer 3: glow around bright light - 0 off, 1 subtle, 2 strong
    bool skipLauncher = false;       // start without the launcher dialog (its settings are in OPTIONS > VIDEO); Shift
                                     // held at start-up, or a start that never got going, shows it again
    // Vertical field of view per player in degrees (Fov1..Fov4; the game's own is 70). Online every view on this PC
    // uses player 1's - each player sees their own preference.
    uint32_t fov[4] = {70, 70, 70, 70};
    // Menu bar colour (OPTIONS > DESERTSTORMFIX > MENU BAR COLOUR, barcolour.cpp): the game's dark green menu buttons
    // recoloured to menuBarColour (0xRRGGBB) while menuBarCustom is on. The default is the game's own green.
    bool menuBarCustom = false;
    uint32_t menuBarColour = 0x1D3909;
    // The same for the HUD's green panels (soldier panels, name bars, weapon panel / inventory rows, health bar frame,
    // mission timer); default = their own green.
    bool hudColourCustom = false;
    uint32_t hudColour = 0x34571B;
    // Footprints (footprints.cpp, OPTIONS > DESERTSTORMFIX > FOOTPRINTS): 0 off, 1 the game's (8 s + 4 s fade),
    // 2 long (90 s + 30 s), 3 permanent (until the decal store needs the room).
    uint32_t footprints = 1;

    // Optional features (OPTIONS > DESERTSTORMFIX). The core - widescreen, HUD / text scaling, controllers, split
    // screen + co-op join, timing / frame cap / display / graphics fixes - is always on; these can be turned off.
    bool featLoadout = true;         // mission briefing + squad loadout screen, overview replay, kit unlocks / carry-over
    bool featUniformSelect = false;  // the game's SELECT UNIFORM (SAS / US Delta) screen after DIFFICULTY (off = skipped)
    bool featTimeOfDay = true;       // TIME OF DAY / TIME FLOW choices
    bool featLoadoutSpin = true;     // loadout screen camera turns round the soldier by itself (off = it stays put)
    bool featLoadoutLookAhead = true;  // loadout screen: the players' soldiers look straight ahead (not where the view was)
    bool featFreeGear = false;       // loadout: one row per pouch spot (any pouch anywhere) instead of whole pouch sets
    bool featVoices = true;          // squad voice accents (SAS / Delta / a skin's own)
    bool featPlayerVoice = false;    // the soldier you control says the lines the game gives only AI soldiers
    bool featStreamerMode = true;    // online: join / reply codes and IP addresses hidden on screen (online.cpp)
    bool featNameplates = true;      // co-op: the other players' names over their soldiers (nameplates.cpp)
    bool featMultiKbm = true;        // split screen: one keyboard + mouse per player (Raw Input, multikbm.cpp)
    bool featCoopScripts = true;     // the missions' own co-op handling (script API player count, coopscripts.cpp)
    // FeatAimAudio / FeatShadowFix: settings of the plugins dsfix.audio / dsfix.shadows (own DLLs since 2026-10-23)
    bool featMods = true;            // Mods folder + MODS menu
    bool featCustomMissions = true;  // Lua missions + CUSTOM CAMPAIGNS
    bool featVersus = true;          // VERSUS menu (off = the game's MULTIPLAYER entry)
    bool featCheats = true;          // extended CHEATS menu, free camera, DEVELOPER page
    bool featWeather = true;         // WEATHER row on the loadout screen (weather.cpp); off = every mission as made
    bool featFootprintsAll = false;  // every soldier near a camera leaves footprints (game: only your squad)
    bool featFootprintsWeather = true;  // wind / rain / snow wipe footprints (faster the stronger the weather)
    bool featRunInBackground = false;   // keep running (and reading pads) without focus - two copies on one PC
    bool featDevConsole = false;     // developer console (` key): force-start levels (always on in dev builds)
};

// One optional feature: its registry value, field and whether a change needs a restart of the game.
struct Toggle {
    const char* name;            // registry value (DWORD 0 / 1)
    bool Values::*field;
    bool restart;                // true = the game reads it only at start-up
    const char* label;           // OPTIONS > DESERTSTORMFIX row text (pse.settings label)
};
const Toggle* Toggles(int& count);

const Values& Get();
void Load();
bool Save(const Values& v);
bool SetToggle(bool Values::*field, bool on);  // save one optional feature
// One Values field by its registry name ("FpsCap", "Fov2", "FeatMods" ...) - dev server / tests.
bool GetByName(const char* name, uint32_t& value);
bool SetByName(const char* name, uint32_t value);   // saved like Save (only this field changes)

// Single DWORDs outside Values: game = the game's own "Device Settings" key (the launcher's Detail Settings:
// ResolutionIndex, Shadows, DrawDistance ...), else our key (state such as StartPending, CoopProgress, PadPreset<n>).
bool ReadDword(bool game, const char* name, uint32_t& value);
bool WriteDword(bool game, const char* name, uint32_t value);

// Other values in our key that aren't settings::Values (remembered loadouts, unlocks, pad layouts, key profiles ...).
// Raw registry under HKCU\<Key()>: names are built at run time (per role / level / player) and some are binary, which
// pse.settings can't declare. Undeclared names are ignored by pse.settings, so both share the key.
const char* Key();                                          // "Software\PSE\ds1\Plugins\dsfix"
bool ReadString(const char* name, std::string& value);      // REG_SZ
bool WriteString(const char* name, const std::string& value);
bool ReadBinary(const char* name, void* data, uint32_t size);  // REG_BINARY of exactly `size` bytes
bool WriteBinary(const char* name, const void* data, uint32_t size);
bool DeleteValue(const char* name);
}  // namespace settings
