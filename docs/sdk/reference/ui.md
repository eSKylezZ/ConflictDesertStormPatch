# pse.ui (version 1)

Drawing over the game, texts the game can show, pop-up messages and keyboard focus – the few things a plugin needs that
its **settings pages** don't already give it. Header: `pse/pse_ui.h` (includes `pse/pse_api.h`). API 0 = unstable
(phase 1).

```c
#include <pse/pse_ui.h>
const PSEUiInterface* ui = pse->Query(pse->plugin, PSE_UI_INTERFACE, 1);   /* may be NULL */
```

**Served by the game's host**, not the loader: in Conflict: Desert Storm DesertStormFix (`dsfix`) registers it while it
loads, before any other plugin, so `Query` in your `Load` finds it. Without DesertStormFix it is NULL – treat it as
optional (draw nothing) unless your manifest `requires = dsfix`. All functions: **game thread** only (use
`pse.tasks` `RunOnGameThread` from other threads). Strings are UTF-8.

## Settings need no UI code

Every setting a plugin declares with [pse.settings](settings.md) gets a row in the game's menus automatically:

- **OPTIONS > DESERTSTORMFIX > PLUGINS** lists every plugin in `PSE\Plugins` – name, version, `LOADED` / `SETTINGS` /
  `REFUSED` / `FAILED`; the footer shows the plugin id, how many settings it has, or the loader's reason for a refused /
  failed one. Selecting a loaded plugin opens **its settings page**:

  | Type | Row | Change |
  | --- | --- | --- |
  | bool | `ON` / `OFF` | Left / Right / Enter flip it |
  | int | the number | Left / Right / Enter move by `step` (0 = 1), stopping at `minInt` / `maxInt` |
  | choice | the choice's label | Left / Right / Enter, wrapping |
  | colour | `#RRGGBB` | Enter opens the colour wheel (arrows / stick = hue + saturation, Q / E = brightness, Delete = the default, Enter OK, Esc cancel) |
  | string | the text | shown read-only in v1 |

  `group` starts a `- GROUP -` heading row; `PSE_SETTING_HIDDEN` settings have no row; `help` is shown under the list
  while the row is selected. A change calls `SetInt` right away: the value is stored and your `OnChange` callbacks run.
  Restart settings (neither `LIVE` nor `NEXT_LEVEL`) show `*`, next-level ones `+`; once a restart setting differs from
  what the game started with, the footer says **RESTART THE GAME TO APPLY**. Keyboard (arrows, W / S, Enter, Esc),
  pads and the mouse (wheel scrolls) work as on every other menu page.
- **The launcher**: settings flagged `PSE_SETTING_LAUNCHER` (bool → checkbox, int / choice → dropdown) appear in the
  launcher's *Detail Settings › Enhancements* column under the plugin's display name, applied with OK (`SetInt` →
  `OnChange`), reset by *Set to Defaults*. Int dropdowns list every step when there are at most 101 values, else 21
  values spread over the range (plus the default and the stored value). Colour / string settings aren't shown there.
- Labels / choices / help are UTF-8: the game's fonts draw Windows-1252, so other characters are replaced (accented
  letters missing from the font by their base letter, typographic quotes / dashes / ellipsis by ASCII, anything else by
  `?`). Menu labels are upper case by convention.

DesertStormFix's own settings (plugin `dsfix`) keep their own menus (OPTIONS > DESERTSTORMFIX, OPTIONS > VIDEO, the
launcher column); its row in PLUGINS says so.

## Drawing

```c
PSEResult DrawRect (PSEPluginHandle p, float x, float y, float w, float h, uint32_t argb, uint32_t flags);
PSEResult DrawText (PSEPluginHandle p, float x, float y, const char* text, uint32_t argb, uint32_t flags);
PSEResult DrawImage(PSEPluginHandle p, PSEUiImage image, float x, float y, float w, float h, uint32_t argb, uint32_t flags);
PSEUiImage LoadImage(PSEPluginHandle p, const char* path, uint32_t* width, uint32_t* height);
float     TextWidth(PSEPluginHandle p, const char* text, uint32_t flags);
PSEResult GetScreen(PSEUiScreen* out);
```

**Immediate-mode, per game frame**: call the Draw functions every frame (subscribe to `PSE_EVENT_FRAME`). What you
queue during a game frame is drawn over that frame's picture – after the game's 3D, HUD and menus, in call order – and
then forgotten; stop calling and it disappears. At most 4096 items per frame (all plugins together), then
`PSE_E_NO_MEMORY`.

- Coordinates: fractions of the screen (0..1, top-left origin) by default; `PSE_UI_PIXELS` = back-buffer pixels.
  `GetScreen` gives the size in pixels, the game's 2D scale (`hudScale`: 800×600 art is drawn this much larger) and
  the small font's line height in pixels (`lineHeight`; the large font ≈ twice), and whether the front end is up.
- `DrawText`: one line (`\n` ends it); `y` is the top of the line. Small menu font by default, `PSE_UI_LARGE` = the
  large title font; `PSE_UI_CENTER` / `PSE_UI_RIGHT` make `x` the centre / right end. Text is drawn with the game's own
  font code, so it looks like the menus and scales with the HUD. `TextWidth` measures in the units `flags` selects.
- `DrawImage`: an image from `LoadImage` (PNG, DDS, BMP, JPEG via WIC; path absolute or relative to the game folder,
  UTF-8; loaded once, the same path returns the same id, kept until exit). `argb` tints it (`0xFFFFFFFF` = as is).
- `PSE_UI_FOLLOW_MENU`: the item fades with the front-end menus (their transitions). Without it, it is drawn as given.
- Colours `0xAARRGGBB`; alpha 0 draws nothing.

```c
static int PSE_CALL OnFrame(PSEEventId e, void* data, void* ctx) {
    ui->DrawRect(me, 0.02f, 0.02f, 0.30f, 0.05f, 0xA0000000, 0);
    ui->DrawText(me, 0.03f, 0.03f, "Hello from my plugin", 0xFFE0D878, 0);
    return 0;
}
```

## Texts

```c
uint32_t RegisterText(PSEPluginHandle p, const char* key, const char* text);
uint32_t TextHash(PSEPluginHandle p, const char* key);
```

A text the **game** shows by its text hash (DS1: the catalog lookup behind every menu item, title, prompt and message –
e.g. a menu item or pop-up built through raw game calls or `pse.game.ds1`'s `LookupText`). The host hands out the
hashes: **never pick hash numbers yourself** (DesertStormFix's own screens used hand-picked `0xC0xx....` ranges; those
stay DesertStormFix's). Same plugin + key = same hash for the whole run; registering the key again replaces the text.
DS1: hashes are `0xC4000000 | (plugin handle & 0xFFF) << 12 | n`, up to 4095 keys per plugin; the text is converted to
the font's characters as above. Returns 0 on bad arguments or when the plugin has used all its keys. `TextHash` = 0 for
a key not registered.

## Pop-up messages

```c
PSEResult ShowPopup(PSEPluginHandle p, const char* text, uint32_t ms);   /* ms 0 = 4000 */
```

DS1: in a mission the game's own message bar across the top of the screen (the one for picked-up items and warnings);
a newer message replaces the shown one. In the front end, where the game has none, DesertStormFix draws a bar like it
itself. The message is shown from the next frame in which the game plays (never in the middle of a level load – so
asking from `PSE_EVENT_LEVEL_LOAD_END` is fine).

## Keyboard focus

```c
PSEResult CaptureKeyboard(PSEPluginHandle p, int on);
int       ReadKey(PSEPluginHandle p, PSEUiKey* out);   /* out->size = sizeof(PSEUiKey) */
```

While your plugin's own UI is up: `CaptureKeyboard(p, 1)` sends key presses and typed characters to you instead of the
game (and of DesertStormFix's own screens) until `CaptureKeyboard(p, 0)` – or until your plugin fails / is unloaded.
`ReadKey` takes the oldest event (`PSE_UI_KEY_DOWN`: `code` = Windows virtual-key code, `repeat` 1 for auto-repeat;
`PSE_UI_KEY_CHAR`: `code` = the Unicode character); up to 64 are queued, older ones dropped. Key releases still reach
the game, so nothing stays held; Alt+F4 still closes the game. The mouse and pads are **not** captured. One plugin at
a time: `PSE_E_CONFLICT` while another plugin has it; `PSE_E_WRONG_PHASE` before the game window exists.

## Not in v1 (deferred)

- **Own front-end screens** (a menu page of a plugin's own with its own rows / actions) and **pause-menu items**: the
  game layer will hand out screen state ids and wrap the game's list objects; until then a plugin's menu presence is its
  settings page.
- Editable string settings (an on-screen text field), sliders, per-setting "available" conditions.
- Icon characters in plugin text (DesertStormFix's button-icon glyphs), the menu zoom transition for drawn items
  (`PSE_UI_FOLLOW_MENU` fades only), capturing pads / the mouse.
- Retained (persistent) overlay items, render-target / 3D drawing.

## Example

`pse/examples/ui_demo` (`example.ui_demo`): one setting of every type (+ a hidden one) in two groups – its page and its
launcher rows come for free –, a banner drawn every frame from those settings, a registered text, and a pop-up on every
setting change and at each level start. Built by `pse_add_plugin` like the other examples; tested by
`tests/live/test_ui_plugins.py`.

## DS1 implementation

`src/features/pseui.cpp` (the interface, registered from `fixmenu.cpp`'s `ApplyFixMenu`, owner `dsfix.overlay`; drawn
in an overlay late draw), `src/features/pluginsmenu.cpp` (PLUGINS = front-end state `0x4D`, a plugin's page = `0x4E`),
`src/features/launcher.cpp` (launcher rows, control ids 1300+), pure parts in `src/core/pse_ui_model.cpp` (page model
from `Describe`, value stepping, UTF-8 → font text, the text-hash table; unit tests `pse/tests/test_ui_model.cpp`).
