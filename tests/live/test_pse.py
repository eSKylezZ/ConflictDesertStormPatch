"""PSE (Pivotal Script Extender) inside the running game: plugin registry + hooks broker ownership.

Every DesertStormFix feature is an internal plugin "dsfix.<feature>" (docs/sdk/ds1-plugins.md); its byte groups, call
hooks and jumps belong to it in the broker. Patches made outside any feature scope belong to "dsfix".
"""
import collections, os, re, sys

TOOLS = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools"))

SIM, CLIENT, INTERNAL = 0x1, 0x2, 0x80000000

# A sample of the feature table (dllmain.cpp kFeatures) with the flag each must carry. Features split into their own
# DLLs (dsfix.widescreen, dsfix.limits, ...) are external now - test_split_plugins.py covers them.
FEATURES = {
    "dsfix.frontend": CLIENT, "dsfix.timing": SIM,
    "dsfix.barcolour": CLIENT, "dsfix.weaponicons": CLIENT,
    "dsfix.ingame_input": CLIENT, "dsfix.overlay": CLIENT,
    "dsfix.diagnostics": CLIENT, "dsfix.mods": SIM, "dsfix.graphics": CLIENT,
}
GLYPH_BIND = 0x53CC9B   # texture bind of the plain image draw: weaponicons 200 -> barcolour 100 -> fontsharp 0
# (dsfix.fontsharp is its own DLL since 2026-10-24; the chain spans dsfix.dll and that plugin)


def _plugins(c):
    return {p["name"]: p for p in c.run("return dev.pse_plugins()")}


def test_core_plugins_registered(c):
    plugins = _plugins(c)
    assert "pse" in plugins and "dsfix" in plugins
    for name, flag in FEATURES.items():
        assert name in plugins, name
        flags = plugins[name]["flags"]
        assert flags & flag, (name, hex(flags))
        assert flags & INTERNAL, (name, hex(flags))
    dsfix = sorted(n for n in plugins if n.startswith("dsfix."))
    print("\n[live] %d internal feature plugins" % len(dsfix))
    assert len(dsfix) >= 40


def test_every_hook_owned_by_a_dsfix_plugin(c):
    plugins = _plugins(c)
    hooks = c.run("return dev.pse_hooks()")
    assert hooks, "the broker lists nothing - patches didn't go through it"
    internal = {n for n, p in plugins.items() if p["flags"] & INTERNAL}
    by_owner = collections.Counter(h["owner"] for h in hooks)
    print("\n[live] %d broker records, %d owners: %s" % (len(hooks), len(by_owner),
          ", ".join("%s %d" % kv for kv in sorted(by_owner.items()))))
    for h in hooks:
        if h["owner"] in internal:   # external plugins (PSE\Plugins) may own their own hooks
            # (pse.game.ds1 = the game layer's event hooks: frame, render, level, save / load, D3D - test_layer_events.py)
            assert h["owner"] in ("dsfix", "pse.game.ds1") or h["owner"].startswith("dsfix."), h
    # Patches belong to their features, not to the catch-all "dsfix".
    assert len([o for o in by_owner if o.startswith("dsfix.")]) >= 20
    unscoped = [h for h in hooks if h["owner"] == "dsfix"]
    print("[live] owned by plain dsfix (made outside a feature scope): %d" % len(unscoped))
    for h in unscoped:
        print("    %s 0x%08X %s" % (h["kind"], h["va"], h["name"]))
    # Core groups are always on; their sites must be owned in game memory.
    for va, owner in ((0x004488B3, "dsfix.widescreen"),):   # Widescreen: launcher 4:3 filter
        owners = [h["owner"] for h in hooks if h["va"] <= va < h["va"] + h["len"]]
        assert owner in owners, (hex(va), owners)


def test_glyph_bind_is_a_shared_chain(c):
    hooks = sorted((h for h in c.run("return dev.pse_hooks()") if h["va"] == GLYPH_BIND), key=lambda h: h["chain"])
    assert all(h["kind"] == "call_site" for h in hooks), hooks
    owners = [h["owner"] for h in hooks]
    plugins = _plugins(c)
    # weaponicons subscribes only with the Mods folder on (FeatMods); the other two always.
    want = ["dsfix.barcolour", "dsfix.fontsharp"]
    if "dsfix.weaponicons" in owners:
        want.insert(0, "dsfix.weaponicons")
    print("\n[live] 0x%08X chain: %s" % (GLYPH_BIND, " -> ".join("%s(%d)" % (h["owner"], h["priority"]) for h in hooks)))
    assert owners == want, owners
    prio = {h["owner"]: h["priority"] for h in hooks}
    assert prio["dsfix.barcolour"] == 100 and prio["dsfix.fontsharp"] == 0
    if "dsfix.weaponicons" in prio:
        assert prio["dsfix.weaponicons"] == 200
    assert c.read(GLYPH_BIND, "u8") == 0xE8   # still a call (to the first subscriber)
    assert "dsfix.fontsharp" in plugins


def test_no_failed_hooks_in_log(c):
    lines = c.run("return dev.log(20000)", async_=True)
    text = "\n".join(lines) if isinstance(lines, list) else str(lines)
    rows = text.splitlines()
    hookish = re.compile(r"call hook|jump at|call at|function hook|pointer hook|not applied|broker|\[conflict\]|"
                         r"not reverted|hooked outside|refused")
    bad = [l for l in rows if ("[fail]" in l or "[conflict]" in l) and hookish.search(l)]
    other = [l for l in rows if ("[fail]" in l or "[warn]" in l) and l not in bad]
    print("\n[live] other [fail] / [warn] lines (not about hooks): %d" % len(other))
    for l in other[:40]:
        print("    " + l)
    assert not bad, bad
    assert any("Sharp text: glyph texture hook installed" in l for l in rows)
    assert any("Menu / HUD colour: image texture binds hooked" in l for l in rows)


def test_game_still_runs(c):
    assert isinstance(c.run("return dev.fe_state()"), int)
    assert c.frames_advancing(2.0), "game thread doesn't run frames"
