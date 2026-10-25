"""The DS1 game layer drives PSE's core game events (pse/games/ds1/ds1_events.cpp, docs/sdk/reference/game-ds1.md
"Events and messages") and DesertStormFix chains on the same sites in its old order:

  - the layer's broker hooks (owner pse.game.ds1) sit on the frame / render / level / save / load sites and the
    Direct3DCreate8 slot, the D3D device slots of the running device;
  - dsfix.framecap runs in FRONT of the layer at the frame call (wait + lockstep, then PSE's frame, then the game's),
    dsfix.missions (the Lua script host) BEHIND it at Level_Init (NEW_GAME / LEVEL_LOAD_BEGIN before dsfix's level work);
  - example.events_logger (deployed by the live runner, PSE_EXAMPLE_PLUGINS=1) saw the events and messages: front end
    ready, device created, frames, render / EndScene / Present, a level change with NEW_GAME before its load begins.

The same without DesertStormFix: test_layer_without_dsfix.py."""
import os, sys, time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "tools")))

LAYER = "pse.game.ds1"
INNER, OUTER = -1000, 1000
DEVICE_GLOBAL = 0x755050
FRAME_CALL, RENDER_CALL, LEVEL_INIT_CALL = 0x4116EC, 0x40FD9E, 0x40E05D
SAVE_GAME, LOAD_GAME, IAT_D3D = 0x415650, 0x415B60, 0x5D8308


def _hooks(c):
    return c.run("return dev.pse_hooks()")


def _chain(hooks, va, kind):
    return sorted([h for h in hooks if h["va"] == va and h["kind"] == kind], key=lambda h: h["chain"])


def _owners(chain):
    return [(h["owner"], h["priority"]) for h in chain]


def test_layer_owns_the_event_sites(c):
    hooks = _hooks(c)
    for va, kind, prio in ((FRAME_CALL, "call_site", INNER), (RENDER_CALL, "call_site", INNER),
                           (LEVEL_INIT_CALL, "call_site", OUTER), (0x40DF18, "call_site", OUTER),
                           (0x411404, "call_site", OUTER), (SAVE_GAME, "function", OUTER),
                           (LOAD_GAME, "function", OUTER), (IAT_D3D, "pointer", OUTER)):
        chain = _chain(hooks, va, kind)
        mine = [h for h in chain if h["owner"] == LAYER]
        assert mine and mine[0]["priority"] == prio, ("0x%08X" % va, _owners(chain))
    # The old dsfix hooks on the save / load call sites are gone (the layer's entry hooks do it now).
    for site in (0x475404, 0x4772CA, 0x4A6D14, 0x449B73):
        assert not _chain(hooks, site, "call_site"), "0x%08X still hooked: %r" % (site, _chain(hooks, site, "call_site"))


def test_dsfix_chains_in_its_old_order(c):
    hooks = _hooks(c)
    frame = _owners(_chain(hooks, FRAME_CALL, "call_site"))
    print("\n[live] frame call chain:", frame)
    assert frame == [("dsfix.framecap", 0), (LAYER, INNER)], frame
    render = _owners(_chain(hooks, RENDER_CALL, "call_site"))
    assert render == [("dsfix.diagnostics", 0), (LAYER, INNER)] or [o for o, _ in render][-1] == LAYER, render
    level = _owners(_chain(hooks, LEVEL_INIT_CALL, "call_site"))
    print("[live] level init chain:", level)
    assert level[0] == (LAYER, OUTER) and ("dsfix.missions", 0) in level, level
    d3d = _owners(_chain(hooks, IAT_D3D, "pointer"))
    assert d3d[0] == (LAYER, OUTER) and ("dsfix.graphics", 0) in d3d, d3d


def test_device_slots_chain_layer_and_graphics(c):
    dev = c.read(DEVICE_GLOBAL)
    assert dev, "no D3D device"
    vt = c.read(dev)
    hooks = _hooks(c)
    for index, layer_prio, first in ((3, INNER, "dsfix.graphics"), (14, OUTER, LAYER), (15, INNER, "dsfix.graphics"),
                                     (35, INNER, "dsfix.graphics")):
        chain = _owners(_chain(hooks, vt + index * 4, "pointer"))
        assert (LAYER, layer_prio) in chain and ("dsfix.graphics", 0) in chain, (index, chain)
        assert chain[0][0] == first, (index, chain)


def _logger_lines(c=None):
    """example.events_logger's log - only when that plugin is loaded in the running game (a log left over from another
    run, without the plugin deployed now, would be read as if it were this game's)."""
    import bg
    path = os.path.join(bg.GAME, "PSE", "Plugins", "example.events_logger.log")
    if c is not None:
        loaded = {p["name"]: p for p in c.run("return dev.pse_plugins()")}
        if "example.events_logger" not in loaded or loaded["example.events_logger"]["disabled"]:
            pytest.skip("example.events_logger is not loaded in this game (tools/split/deploy_run.py deploys it)")
    if not os.path.exists(path):
        pytest.skip("example.events_logger is not deployed (tools/split/deploy_run.py deploys it)")
    return [l.rstrip("\n") for l in open(path, encoding="utf-8", errors="replace")]


def _index(lines, text, start=0):
    for i in range(start, len(lines)):
        if text in lines[i]:
            return i
    return -1


def test_events_reach_a_plugin_in_the_front_end(c):
    lines = _logger_lines(c)
    for text in ("loaded (game ds1", "message POST_LOAD", "message DEVICE_CREATED", "event FRAME first",
                 "event PRE_RENDER first", "event POST_RENDER first", "event END_SCENE first", "event PRESENT first",
                 "event LEVEL_LOAD_BEGIN FrontEnd", "event LEVEL_LOAD_END FrontEnd", "message FRONT_END_READY FrontEnd"):
        assert _index(lines, text) >= 0, (text, lines[-20:])
    assert _index(lines, "event LEVEL_LOAD_END FrontEnd") < _index(lines, "message FRONT_END_READY")


def test_level_change_events_and_new_game_order(c):
    _logger_lines(c)   # (skips without the plugin)
    if c.state()["fe_state"] != 1:   # NEW_GAME needs a game started FROM the front end: back to the main menu first
        c.start_level("FrontEnd.dll", wait=False)
        c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=90)
        lines = _logger_lines()
        assert _index(lines, "message FRONT_END_READY FrontEnd", max(0, len(lines) - 30)) >= 0, lines[-30:]
    before = len(_logger_lines())
    c.start_level("Mission1.dll")
    deadline = time.time() + 30
    lines = _logger_lines()
    while time.time() < deadline and _index(lines, "event LEVEL_LOAD_END Mission1", before) < 0:
        time.sleep(0.5)
        lines = _logger_lines()
    unload = _index(lines, "event LEVEL_UNLOAD", before)
    new_game = _index(lines, "message NEW_GAME Mission1", before)
    revert = _index(lines, "cosave revert", before)
    begin = _index(lines, "event LEVEL_LOAD_BEGIN Mission1", before)
    end = _index(lines, "event LEVEL_LOAD_END Mission1", before)
    print("\n[live] level change:", lines[before:end + 1][:12])
    assert 0 <= unload < begin < end, lines[before:]
    # NEW_GAME (+ the revert callbacks) before the level starts loading - as when dsfix sent it from its level hook.
    assert 0 <= revert < new_game < begin, lines[before:]
    # DesertStormFix's per-frame work still runs (its PSE_EVENT_FRAME subscriber): the game keeps playing.
    assert c.wait_for_play()
    assert c.frames_advancing(1.0)
