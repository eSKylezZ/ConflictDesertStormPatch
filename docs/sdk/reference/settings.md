# pse.settings (version 1)

Typed plugin settings, stored in the registry and shown in the game's menus by the game layer. No ini files.
Header: `pse/pse_api.h`. API 0 = unstable (phase 1).

```c
const PSESettingsInterface* set = pse->Query(pse->plugin, PSE_SETTINGS_INTERFACE, 1);
```

All functions: game thread.

## Storage

`HKCU\Software\PSE\<gameId>\Plugins\<plugin name>`, one value per setting, named by its key:

| Type | Registry type | Value |
| --- | --- | --- |
| `PSE_SETTING_BOOL` | REG_DWORD | 0 / 1 |
| `PSE_SETTING_INT` | REG_DWORD | `minInt`..`maxInt` |
| `PSE_SETTING_CHOICE` | REG_DWORD | index into `choices` |
| `PSE_SETTING_COLOUR` | REG_DWORD | 0xRRGGBB |
| `PSE_SETTING_STRING` | REG_SZ | UTF-8 text (stored as UTF-16) |

Nothing is written until a value is changed. A stored value is never used unchecked: out of range is clamped (bool
→ 0/1, int → [min, max], choice → [0, count − 1], colour → low 24 bits); a value of the wrong registry type is
ignored (the default is used).

Names that were never declared are left alone, so a plugin (or the host) may keep other values of its own in the
same key – e.g. binary blobs or names built at run time, which `Declare` can't describe. DesertStormFix does this for
pad layouts, key profiles and remembered loadouts ([ds1-settings.md](../ds1-settings.md)). The interface itself has
no binary accessor (none is planned for API 0).

Host side (loader internals, `pse/loader/loader.h`, not part of the SDK): `pse::PluginSettingsKey(plugin)` returns
that key path, and `pse::MigrateRegistryValues(sources, dstRoot, dstPath, marker)` copies a host's old settings key
into it once (every value and type, existing names kept, then a REG_DWORD marker; the sources are never changed). DS1
uses it to move DesertStormFix's old `Enhancements` values ([ds1-settings.md](../ds1-settings.md)).

## Declaring

```c
typedef struct PSESettingDesc {
    uint32_t size;               /* sizeof(PSESettingDesc) */
    const char* key;             /* registry value name: [A-Za-z0-9_] */
    PSESettingType type;
    uint32_t flags;              /* PSE_SETTING_LIVE / LAUNCHER / HIDDEN / NEXT_LEVEL */
    int32_t defaultInt;          /* bool / int / choice index / colour */
    int32_t minInt, maxInt;      /* int */
    int32_t step;                /* int, menu step (0 = 1) */
    const char* defaultString;   /* string */
    const char* label;           /* menu text, upper case by convention */
    const char* help;            /* one line, may be NULL */
    const char* const* choices;  /* choice: NULL-terminated labels */
    const char* group;           /* menu sub-heading, may be NULL */
} PSESettingDesc;
```

Flags (for the menus): `PSE_SETTING_LIVE` – takes effect at once (use `OnChange`), otherwise the menu says "restart
the game"; `PSE_SETTING_NEXT_LEVEL` – at the next level start; `PSE_SETTING_LAUNCHER` – also in the launcher;
`PSE_SETTING_HIDDEN` – not shown (internal / dev).

### Declare

```c
PSEResult Declare(PSEPluginHandle plugin, const PSESettingDesc* desc);
```

Once per setting, in `Load`. Reads the stored value (or the default; both clamped). The loader copies the
description and its strings, so `desc` can be a local. Errors: `PSE_E_INVALID_ARG` (bad handle, NULL / short
`desc`, key not `[A-Za-z0-9_]+`, unknown type, `minInt > maxInt`, a choice without choices), `PSE_E_EXISTS` (this
plugin already declared that key). Keys are per plugin: two plugins may use the same key.

```c
static const char* const kModes[] = {"OFF", "AIMING", "ALWAYS", NULL};
PSESettingDesc d = {sizeof d};
d.key = "Gyro"; d.type = PSE_SETTING_CHOICE; d.flags = PSE_SETTING_LIVE;
d.defaultInt = 1; d.label = "GYRO AIM"; d.choices = kModes;
set->Declare(me, &d);
```

## Reading and writing

### GetInt / SetInt

```c
int32_t GetInt(PSEPluginHandle plugin, const char* key);
PSEResult SetInt(PSEPluginHandle plugin, const char* key, int32_t value);
```

For bool / int / choice / colour. `GetInt` returns 0 for an undeclared key or a string setting. `SetInt` clamps the
value like Declare, writes it to the registry and runs the plugin's change callbacks – only when the (clamped) value
differs from the current one. Errors: `PSE_E_NOT_FOUND` (undeclared), `PSE_E_INVALID_ARG` (string setting),
`PSE_E_ACCESS` (registry write failed; the value is unchanged).

### GetString / SetString

```c
uint32_t GetString(PSEPluginHandle plugin, const char* key, char* buf, uint32_t bufLen);
PSEResult SetString(PSEPluginHandle plugin, const char* key, const char* value);
```

`GetString` copies into `buf` (cut to fit, always NUL-terminated when `bufLen` > 0) and returns the full length in
bytes without the NUL – call with `bufLen` 0 to size the buffer. Undeclared key or non-string setting: returns 0 and
an empty `buf`. `SetString` works like `SetInt`; errors `PSE_E_NOT_FOUND`, `PSE_E_INVALID_ARG` (NULL value or not a
string setting), `PSE_E_ACCESS`.

### OnChange

```c
typedef void (*PSESettingChanged)(const char* key, void* ctx);
PSEResult OnChange(PSEPluginHandle plugin, PSESettingChanged cb, void* ctx);
```

`cb` runs after any of this plugin's settings changed (from a menu or `SetInt` / `SetString`), with the key. Several
callbacks per plugin are allowed. Errors: `PSE_E_INVALID_ARG`.

```c
static void Changed(const char* key, void* ctx) {
    if (strcmp(key, "Gyro") == 0) g_gyro = set->GetInt(me, "Gyro");
}
set->OnChange(me, Changed, NULL);
```

## Enumerating (game layer menus)

### Describe

```c
PSEResult Describe(PSEPluginHandle owner, uint32_t index, const PSESettingDesc** out, PSEPluginHandle* outOwner);
```

The `index`-th declared setting of `owner` (0 = of all plugins), in declaration order. `*out` stays valid until exit
(the loader's copy); `outOwner` may be NULL. Errors: `PSE_E_INVALID_ARG` (NULL `out`), `PSE_E_NOT_FOUND` (index past
the end – stop there).

```c
const PSESettingDesc* d; PSEPluginHandle who;
for (uint32_t i = 0; set->Describe(0, i, &d, &who) == PSE_OK; ++i) AddMenuRow(who, d);
```

## In the menus and the launcher (DS1)

Declaring is all a plugin does – the game layer's host builds the UI from `Describe` ([ui.md](ui.md#settings-need-no-ui-code)):

- **OPTIONS > DESERTSTORMFIX > PLUGINS** lists every plugin (version, load state, the reason a plugin was refused /
  failed) and opens a **settings page per plugin**: bool ON / OFF, int by `step` between `minInt` and `maxInt`, choice
  labels (wrapping), colour `#RRGGBB` + the colour wheel, string read-only; `group` = heading rows, `help` = the footer
  line, `HIDDEN` = no row. A change is `SetInt` → stored + `OnChange` at once. Flags only change the marker: `LIVE` none,
  `NEXT_LEVEL` `+`, restart `*` (and RESTART THE GAME TO APPLY once such a value differs from the one the game started
  with – taken at `PSE_MSG_POST_LOAD`).
- **Launcher**: `PSE_SETTING_LAUNCHER` bool / int / choice settings of external plugins get a checkbox / dropdown in
  *Detail Settings › Enhancements*, one block per plugin, saved with OK, reset by *Set to Defaults*.
- `label`, `help`, `choices`, `group` are UTF-8; characters the game's fonts lack are replaced in the game's menus (the
  launcher shows them as they are).
