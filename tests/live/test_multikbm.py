"""Several keyboards + mice in split screen (src/features/multikbm.cpp): two pretend keyboards and two pretend mice
(dev.kbm_inject) join the co-op screen, Mission 3 starts in 2-player split screen, and each player's keys / mouse move
only that player's soldier. Real devices are never read in background runs."""
import time

BLOCKS, BLOCK_SIZE = 0x60F5B8, 0x478
W, SPACE, ENTER = 0x11, 0x39, 0x1C
KB1, KB2, MOUSE1, MOUSE2 = 0, 1, 2, 3   # pretend devices


def _log_text(c, n=4000):
    lines = c.log(n)
    return "\n".join(lines) if isinstance(lines, list) else str(lines)


def _tap(c, kb, code):
    c.run("dev.kbm_inject(%d, 'key', %d, false)" % (kb, code))
    time.sleep(0.15)
    c.run("dev.kbm_inject(%d, 'key', %d, true)" % (kb, code))
    time.sleep(0.15)


def _click(c, mouse):
    c.run("dev.kbm_inject(%d, 'button', 1)" % mouse)
    time.sleep(0.15)
    c.run("dev.kbm_inject(%d, 'button', 2)" % mouse)
    time.sleep(0.15)


def _yaw(c, player):
    return c.run("local s = dev.u32(%d + 0x310); if s == 0 then return nil end; "
                 "return dev.f32(dev.u32(s + 0x2C) + 0x104)" % (BLOCKS + player * BLOCK_SIZE))


def _dist(a, b):
    return ((a["x"] - b["x"]) ** 2 + (a["z"] - b["z"]) ** 2) ** 0.5


def test_two_keyboards_two_mice(c):
    assert c.run("return dev.kbm_start()"), "Raw Input didn't start: " + _log_text(c, 200)[-800:]
    log = _log_text(c, 20000)
    assert "Raw Input running" in log
    print("[kbm]", [l for l in log.splitlines() if "Several keyboards" in l][-3:])

    # The co-op join screen; each pretend keyboard joins with Space, then each player clicks their mouse.
    c.call(0x47EF20, "cdecl", 0x3E)
    c.wait_until("dev.fe_state() == 0x3E", timeout=10)
    time.sleep(0.5)
    _tap(c, KB1, SPACE)
    _click(c, MOUSE1)
    _tap(c, KB2, SPACE)
    _click(c, MOUSE2)
    log = _log_text(c, 400)
    assert "player 1's mouse is device" in log and "player 2's mouse is device" in log, log[-1500:]
    _tap(c, KB1, ENTER)   # Begin
    c.wait_until("dev.fe_state() == 2", timeout=10)
    log = _log_text(c, 400)
    assert "each keyboard player reads their own devices" in log, log[-1500:]

    c.start_level("Mission3.dll")
    t0 = time.time()
    while time.time() - t0 < 30 and not c.run("return dev.loadout_open()"):
        time.sleep(0.3)
    if c.run("return dev.loadout_open()"):
        c.run("return dev.loadout_close()")
    time.sleep(3)   # the soldiers settle after the screen

    # Keyboard 2 holds W: player 2 walks, player 1 doesn't.
    p1, p2 = c.player(0), c.player(1)
    assert p1 and p2, "no soldiers for both players"
    c.run("dev.kbm_inject(%d, 'key', %d, false)" % (KB2, W))
    time.sleep(2.5)
    c.run("dev.kbm_inject(%d, 'key', %d, true)" % (KB2, W))
    time.sleep(0.5)
    q1, q2 = c.player(0), c.player(1)
    print("[kbm] W on keyboard 2: player 1 moved %.0f cm, player 2 %.0f cm" % (_dist(p1, q1), _dist(p2, q2)))
    assert _dist(p2, q2) > 100, "player 2 didn't walk"
    assert _dist(p1, q1) < 30, "player 1 walked with player 2's keyboard"

    # Mouse 1 moves right: player 1 turns, player 2 doesn't.
    y1, y2 = _yaw(c, 0), _yaw(c, 1)
    for _ in range(40):
        c.run("dev.kbm_inject(%d, 'move', 40, 0)" % MOUSE1)
        time.sleep(0.03)
    time.sleep(0.5)
    z1, z2 = _yaw(c, 0), _yaw(c, 1)
    turn = lambda a, b: abs((b - a + 180) % 360 - 180)
    print("[kbm] mouse 1 moved: player 1 turned %.1f deg, player 2 %.1f deg" % (turn(y1, z1), turn(y2, z2)))
    assert turn(y1, z1) > 5, "player 1 didn't turn with mouse 1"
    assert turn(y2, z2) < 1, "player 2 turned with player 1's mouse"

    c.console("level FrontEnd.dll")
