"""Phase 2 layout in the running game (docs/sdk/layout.md): the shim dinput8.dll, PSE\\pse_loader.dll, the DS1 game layer
PSE\\Games\\pse_ds1.dll and DesertStormFix as the HOST plugin PSE\\Plugins\\dsfix.dll with its internal sub-plugins.
Needs a game started from the split build (runner sets PSE_LAYOUT=1); otherwise skipped."""
import os

import pytest

pytestmark = pytest.mark.skipif(os.environ.get("PSE_LAYOUT") != "1", reason="not the phase 2 layout (PSE_LAYOUT=1)")

INTERNAL = 0x80000000


def _plugins(c):
    return {p["name"]: p for p in c.run("return dev.pse_plugins()")}


def test_modules_in_their_places(c):
    paths = {m: c.run("return dev.module_path(%r)" % m) for m in
             ("dinput8.dll", "pse_loader.dll", "pse_ds1.dll", "dsfix.dll")}
    print("\n[live] " + ", ".join("%s = %s" % kv for kv in paths.items()))
    assert all(paths.values()), paths
    game = os.path.dirname(paths["dinput8.dll"]).lower()
    assert paths["pse_loader.dll"].lower() == os.path.join(game, "pse", "pse_loader.dll")
    assert paths["pse_ds1.dll"].lower() == os.path.join(game, "pse", "games", "pse_ds1.dll")
    assert paths["dsfix.dll"].lower() == os.path.join(game, "pse", "plugins", "dsfix.dll")


def test_loader_game_layer_and_host_plugin(c):
    plugins = _plugins(c)
    loader, layer, dsfix = plugins["pse"], plugins["pse.game.ds1"], plugins["dsfix"]
    assert not loader["external"]
    # The game layer: built in (registered by the loader), with host rights.
    assert layer["flags"] & INTERNAL and not layer["external"] and layer["host"]
    # DesertStormFix: an external plugin DLL that is a host plugin.
    assert dsfix["external"] and dsfix["host"] and not dsfix["flags"] & INTERNAL and not dsfix["disabled"]
    assert dsfix["version"].startswith("3.")
    # Its features: internal sub-plugins (no crash guard, PSE_PLUGIN_INTERNAL), not hosts, all enabled.
    # DesertStormFix's own plugin DLLs (dsfix.discord, the split-out features on the dsfix kit) are ordinary external
    # plugins; everything else named dsfix.* is a sub-plugin inside dsfix.dll.
    own_dlls = {n for n, p in plugins.items() if n.startswith("dsfix.") and p["external"]}
    subs = {n: p for n, p in plugins.items() if n.startswith("dsfix.") and n not in own_dlls}
    # The plugin split is finished (2026-10-25): what stays inside dsfix.dll is the host layer (docs/sdk/ds1-host-layer.md)
    # - any new internal plugin should be a plugin DLL instead; every feature is one of the own DLLs
    host_layer = {"dsfix.console", "dsfix.dev", "dsfix.diagnostics", "dsfix.fixmenu", "dsfix.framecap", "dsfix.frontend",
                  "dsfix.ingame_input", "dsfix.launcher", "dsfix.memory", "dsfix.originals", "dsfix.overlay", "dsfix.restart"}
    assert set(subs) == host_layer, sorted(set(subs) ^ host_layer)
    assert len(own_dlls) >= 33, len(own_dlls)
    for n, p in subs.items():
        assert p["flags"] & INTERNAL and not p["external"] and not p["host"] and not p["disabled"], (n, p)
    for n in own_dlls:
        d = plugins[n]
        assert not d["host"] and not d["flags"] & INTERNAL and not d["disabled"], (n, d)
    print("\n[live] loader + pse.game.ds1 (host) + dsfix (external host) + %d sub-plugins + DLLs %s"
          % (len(subs), ", ".join(sorted(own_dlls))))


def test_host_loaded_first_and_log_kept(c):
    # Handles follow registration order: loader, game layer, dsfix, its sub-plugins, then other external plugins.
    plugins = sorted(c.run("return dev.pse_plugins()"), key=lambda p: p["handle"])
    names = [p["name"] for p in plugins]
    assert names[:3] == ["pse", "pse.game.ds1", "dsfix"], names[:5]
    first_other = min(i for i, p in enumerate(plugins) if p["external"] and p["name"] != "dsfix") \
        if any(p["external"] and p["name"] != "dsfix" for p in plugins) else len(plugins)
    assert all(p["name"].startswith("dsfix") for p in plugins[2:first_other])
    lines = c.log(20000)
    text = "\n".join(lines) if isinstance(lines, list) else str(lines)
    # Lines the loader wrote before dsfix set its sink reached DesertStormFix.log.
    assert "start-up: game layer pse_ds1.dll recognises the exe: game ds1" in text
    assert "plugins: loaded dsfix " in text
    assert "start-up: done in" in text


def test_ids_registered_by_the_game_layer(c):
    # pse.ids is published by the game layer in PSEGame_Init, before dsfix patched anything: every signature matches.
    lines = c.log(20000)
    text = "\n".join(lines) if isinstance(lines, list) else str(lines)
    line = next((l for l in text.splitlines() if "pse.ids: ds1 database" in l), None)
    assert line, "no pse.ids registration line"
    print("\n[live] " + line.strip())
    assert "[pse.game.ds1]" in line and " 0 mismatched" in line
