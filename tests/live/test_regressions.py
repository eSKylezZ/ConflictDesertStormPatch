"""Regression tests for crashes found while testing (one test per bug, named after it)."""
import time


def _log_text(c, n=3000):
    lines = c.log(n)
    return "\n".join(lines) if isinstance(lines, list) else str(lines)


def test_level_change_while_loadout_screen_is_open(c):
    """2026-10-19: a level load while the briefing / loadout screen was up crashed at 0x569FD7 (the screen's late draw
    asked the game's text lookup for the level title while the load had freed the text tables). Now the draw pauses
    during a load and the lookup is fault-safe."""
    # the screen this test needs is the optional briefing / loadout screen (FeatLoadout, applies at the next level
    # start): on for the test, the player's value put back
    was = c.run('return dev.setting("FeatLoadout")')
    c.run('return dev.set_setting("FeatLoadout", 1)')
    try:
        _level_change_while_loadout_screen_is_open(c)
    finally:
        if was is not None: c.run('return dev.set_setting("FeatLoadout", %d)' % was)


def _level_change_while_loadout_screen_is_open(c):
    before = _log_text(c, 20000).count("Customise: opened in")
    c.console("level Mission1.dll")
    c.wait_for_play(timeout=180)
    t0 = time.time()
    opened = False
    while time.time() - t0 < 20:          # the screen opens after 0.5 s of plain play
        opened = _log_text(c, 20000).count("Customise: opened in") > before
        if opened: break
        time.sleep(0.5)
    assert opened, "the loadout screen didn't open - the test would prove nothing"
    # Leave it open and change level from the console (the scripted path that crashed).
    c.console("level FrontEnd.dll")
    t0 = time.time()
    while time.time() - t0 < 120:
        st = c.state()
        if st["fe_state"] == 1 and st["dispatch"] == 0: break
        if not c.frames_advancing(1.0): c.press("space")
    st = c.state()
    assert st["fe_state"] == 1 and st["dispatch"] == 0, st
    assert c.frames_advancing(1.0), "game stopped after the level change"


def test_dev_call_without_convention(c):
    """2026-10-19: dev.call(addr) with only an address computed -1 arguments (a huge stack copy) and ended the game.
    Now it calls with no arguments (default cdecl); a fault inside the callee comes back as an error."""
    from pse_client import PseError
    try:
        c.run("return dev.call(0x4BA370)")   # FUN_004ba370: the game clock, no arguments - returns ms
    except PseError:
        pass                                 # an error reply is fine too - the game must survive
    assert c.frames_advancing(1.0), "game stopped after dev.call(addr)"
