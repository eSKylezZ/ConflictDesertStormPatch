"""pse.lua in the real game (docs/sdk/reference/lua.md): the external plugin example.lua_native adds `example.*` to the
.lua level tests/live/data/pselua_test.lua (Mission 1's map, --@archive mission1).

Needs (the runner does both, before the game starts, and undoes them afterwards):
  - <game>\\PSE\\Plugins\\example.lua_native.dll + .pse (from the build's pse_plugins folder);
  - <game>\\Mods\\PseLuaTest\\pselua_test.lua (a copy of tests/live/data/pselua_test.lua - the mod index is built at
    start-up, so the file must be there when the game starts).
Set PSE_LUA_TEST=1 to run (otherwise skipped). The plugin crashes on purpose (example.crash) late in the level - run
this file in its own game session or last."""
import os, re, time

import pytest

pytestmark = pytest.mark.skipif(os.environ.get("PSE_LUA_TEST") != "1",
                                reason="pse.lua live test not set up (PSE_LUA_TEST=1, plugin + Mods\\PseLuaTest deployed)")

LEVEL = "pselua_test.lua"


def log_text(c, n=5000):
    lines = c.log(n)
    return "\n".join(lines) if isinstance(lines, list) else str(lines)


def notes(c):
    """{key: value} of the script's 'pselua key = value' lines (latest wins)."""
    out = {}
    for m in re.finditer(r"\[script\] pselua_test\.lua: pselua (\w+) = (.*)", log_text(c)):
        out[m.group(1)] = m.group(2).strip()
    return out


def wait_note(c, key, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        n = notes(c)
        if key in n: return n
        time.sleep(0.5)
    raise AssertionError("no 'pselua %s' line after %.0f s; notes: %r" % (key, timeout, notes(c)))


@pytest.fixture(scope="module")
def level(game):
    c = game.client
    plugins = {p["name"] for p in c.run("return dev.pse_plugins()")}
    if "example.lua_native" not in plugins:
        pytest.skip("example.lua_native not loaded (deploy it into <game>\\PSE\\Plugins)")
    out = c.console("level " + LEVEL)
    assert any(l.startswith("starting ") for l in out), out
    n = wait_note(c, "init", 180)
    c.wait_for_play(180)   # presses Space through PLEASE PRESS ANY KEY: update() only runs in play
    return n


def test_plugin_registered(c, level):
    assert "lua_native: example.add / greet / sum / info / call / fail / crash registered" in log_text(c)
    assert "plugin Lua function(s) installed" in log_text(c)


def test_natives_from_the_script(c, level):
    n = level
    assert n["namespace"] == "table"
    assert n["add"] == "5"
    assert n["add_float"] == "2.5"
    assert n["greet"] == "Hello, Bradley!"
    assert n["sum"] == "6.5"
    assert n["info"] == "example.lua_native 1.0.0"
    assert n["call"] == "42"   # example.call -> CallGlobal("triple", 14) back into the script


def test_errors_become_lua_errors(c, level):
    n = level
    assert n["fail_ok"] == "false"
    assert "example.fail: live error (plugin example.lua_native)" in n["fail_msg"]
    assert "pselua_test.lua:" in n["fail_msg"]   # the script line that called it
    assert "example.greet: bad argument #1 (string expected, got no value)" in n["badarg"]


def test_level_start_event_calls_the_script(c, level):
    t0 = time.time()
    while time.time() - t0 < 30:
        if 'the script says "level start seen in pselua_test.lua"' in log_text(c): return
        time.sleep(0.5)
    raise AssertionError("no level-start CallGlobal answer in the log")


def test_crash_is_caught_and_the_game_keeps_running(c, level):
    n = wait_note(c, "uncaught", 240)   # the script crashes the plugin after 60 updates
    assert n["crash_ok"] == "false"
    assert "example.crash: plugin example.lua_native crashed (exception 0xC0000005" in n["crash_msg"]
    assert "and was disabled" in n["crash_msg"]
    assert "example.add: plugin example.lua_native is not loaded" in n["after_crash"]
    text = log_text(c)
    # plugins.cpp PluginCrashed: "plugins: example.lua_native crashed in the Lua function example.crash: ... - disabled"
    assert re.search(r"example\.lua_native crashed in the Lua function example\.crash: .* - disabled", text), text[-3000:]
    assert "unreachable" not in n                        # the uncaught error ended the script's update
    assert "[fail] script pselua_test.lua: update:" in text or "script pselua_test.lua: update" in text
    time.sleep(1.0)
    assert c.frames_advancing(2.0), "the game stopped after the plugin crash"
    st = c.state()
    assert st["dispatch"] == 0
