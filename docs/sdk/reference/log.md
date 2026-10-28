# pse.log (version 1)

Log lines and player-facing problem reports. Header: `pse/pse_api.h`. API 0 = unstable (phase 1).

```c
const PSELogInterface* log = pse->Query(pse->plugin, PSE_LOG_INTERFACE, 1);
```

## Line format

```text
[mm:ss.mmm] [plugin] LEVEL text
[00:03.417] [dsfix.discord] INFO connected to Discord
```

- Time = since the loader started (minutes keep counting past 59).
- `plugin` = the caller's plugin name; an unknown handle shows as `?`.
- `LEVEL` = `DEBUG`, `INFO`, `WARN` or `ERROR`.

Where lines go:

- The host's log (in DesertStormFix: `DesertStormFix.log`).
- `<log folder>\<plugin>.log` when the loader has a log folder. The file is rewritten at each game start and flushed
  after every line, so it survives a crash. Unknown handles get no file.

Lines from several threads never mix: each line is written whole.

## Functions

### Write

```c
void Write(PSEPluginHandle plugin, PSELogLevel level, const char* fmt, ...);
```

printf-style. Any thread, any time after the plugin's `Load` started. No length limit.

```c
log->Write(pse->plugin, PSE_LOG_INFO, "loaded %u weapons", count);
```

### Report

```c
void Report(PSEPluginHandle plugin, int error, const char* fmt, ...);
```

A message for players and modders: written to the log (level `ERROR` when `error` is non-zero, else `INFO`) and to
the mod report (`Mods\DesertStormFix-report.txt` in DS1) as `[plugin] text`. `error != 0` counts as a problem (the
main menu shows "N problems"). Any thread.

```c
log->Report(pse->plugin, 1, "%s: unknown weapon id '%s'", file, id);
```

Use `Report` for things a player or modder can fix (a bad mod file), `Write` for everything else.
