"""Photo mode, the free camera and the extended CHEATS menu live in their own plugin DLL dsfix.photo since 2026-10-24
(games/ds1/plugins/photo, on the dsfix kit's host client + interface dsfix.photo). In Mission 1: F5 opens photo mode,
a soldier is spawned and posed, a photo is saved, Esc closes it and the spawned soldier is removed again. The photo
written to <game>\\Screenshots is deleted afterwards."""
import glob, os, time

import pytest

from test_game_interface import ensure_mission1

VK_TAB, VK_RETURN, VK_ESCAPE, VK_RIGHT = 0x09, 0x0D, 0x1B, 0x27
# photo mode samples its keys in the game update (cheats.cpp's frame hook): an 80 ms posted press is often missed
HOLD = 300
TIME_MULT = 0x5F7E64   # the global time multiplier: photo mode freezes the world (0.0001)


def log_text(c, n=20000):
    lines = c.log(n)
    return "\n".join(lines) if isinstance(lines, list) else str(lines)


def wait_log(c, text, timeout, after=0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if log_text(c).count(text) > after:
            return True
        time.sleep(0.3)
    return False


@pytest.fixture
def photo(c):
    ps = {p["name"]: p for p in c.run("return dev.pse_plugins()")}
    if "dsfix.photo" not in ps:
        pytest.skip("dsfix.photo not installed")
    return ps["dsfix.photo"]


def test_photo_plugin_loaded_and_owns_its_hooks(c, photo):
    assert photo["external"] and not photo["disabled"] and not photo["host"], photo
    hooks = c.run("return dev.pse_hooks()")
    owners = {h["owner"] for h in hooks if h["va"] == 0x40F90D}   # the frame hook photo mode / the free camera run from
    assert owners == {"dsfix.photo"}, owners
    text = log_text(c)
    assert "[ok]   dsfix.host 1: registered" in text
    assert "dsfix.photo" in text and "is from another DesertStormFix build" not in text
    # dsfix.dll's text lookup reaches the plugin's texts (PHOTO MODE in the pause menu) through its wrapper
    assert c.run("return dev.game_text(%d)" % 0xC0140001) == "PHOTO MODE"


def test_photo_mode_round_trip(c, photo):
    game_dir = os.path.dirname(c.run('return dev.module_path("dinput8.dll")'))
    ensure_mission1(c)
    c.run("return dev.loadout_close()")
    time.sleep(1.0)
    before = set(glob.glob(os.path.join(game_dir, "Screenshots", "*.png")))
    opened = log_text(c).count("Photo mode: opened in")
    for _ in range(4):   # the first posted F5 after a level start is often not seen
        c.press("f5", HOLD)
        if wait_log(c, "Photo mode: opened in", 3, opened):
            break
    assert log_text(c).count("Photo mode: opened in") > opened, "F5 didn't open photo mode"
    try:
        assert c.read(TIME_MULT, "f32") < 0.01                    # world frozen
        spawned = log_text(c).count("Photo mode: spawned ")
        c.press("n", HOLD)                                              # spawn a soldier at the look point
        assert wait_log(c, "Photo mode: spawned ", 10, spawned)
        posed = log_text(c).count(" pose ")
        c.press(VK_RIGHT, HOLD)                                         # next pose for the selected soldier
        assert wait_log(c, " pose ", 10, posed)
        c.press(VK_RETURN, HOLD)                                        # take the photo
        assert wait_log(c, " saved to ", 15)
    finally:
        closed = log_text(c).count("Photo mode: closed")
        c.press(VK_ESCAPE, HOLD)
        assert wait_log(c, "Photo mode: closed", 10, closed)
        for f in set(glob.glob(os.path.join(game_dir, "Screenshots", "*.png"))) - before:
            os.remove(f)
    assert "spawned soldier(s) removed" in log_text(c)
    t0 = time.time()
    while time.time() - t0 < 5 and c.read(TIME_MULT, "f32") < 0.5:
        time.sleep(0.2)
    assert c.read(TIME_MULT, "f32") > 0.5                         # time runs again
    assert c.frames_advancing(1.0)
