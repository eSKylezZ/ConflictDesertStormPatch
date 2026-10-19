"""Dev server live tests (run in file order: front end first, then Mission 1)."""
import statistics, threading, time

import pytest

from pse_client import PseError

HASH = 0x4B9A10   # FUN_004b9a10: cdecl(char* upper-case name) -> archive hash


def test_frontend_state(c):
    st = c.state()
    assert set(st) >= {"frame", "fe_state", "dispatch", "level", "focused", "players"}
    assert isinstance(st["fe_state"], int) and isinstance(st["level"], str)
    assert c.frames_advancing(2.0), "game thread doesn't run frames"
    assert c.run("return dev.fe_state()") == c.read(0x617C18)   # sync == async == raw read


def test_round_trip_timing(c):
    def measure(async_):
        times = []
        for _ in range(40):
            t0 = time.perf_counter()
            assert c.run("return 1 + 1", async_=async_) == 2
            times.append((time.perf_counter() - t0) * 1000)
        return statistics.median(times), max(times)
    sync_med, sync_max = measure(False)
    async_med, async_max = measure(True)
    print("\n[live] round trip: sync median %.2f ms (max %.2f), async median %.2f ms (max %.2f)"
          % (sync_med, sync_max, async_med, async_max))
    assert async_med < 50


def test_call_hash_function(c):
    assert c.call(HASH, "cdecl", "WEAPONS.RFX") == 0x4475E8D7
    assert c.call(HASH, "cdecl", "WEAPONS.RFX", async_=True) == 0x4475E8D7


def test_memory_round_trip(c):
    r = c.run("""
        local p = dev.alloc(64)
        dev.write_u32(p, 0xDEADBEEF); dev.write_f32(p + 4, 1.5); dev.write_cstring(p + 8, "hello")
        dev.write_bytes(p + 16, "01 02 ff")
        local out = {u32 = dev.u32(p), f = dev.f32(p + 4), s = dev.cstring(p + 8), b = dev.bytes(p + 16, 3),
                     i8 = dev.i8(p + 18)}
        dev.free(p)
        return out
    """)
    assert r == {"u32": 0xDEADBEEF, "f": 1.5, "s": "hello", "b": "0102ff", "i8": -1}


def test_errors_dont_kill_the_game(c):
    with pytest.raises(PseError, match="can't read"):
        c.run("return dev.u32(0)")
    with pytest.raises(PseError, match="can't read"):
        c.read(0x10, async_=True)
    with pytest.raises(PseError, match="exception 0xc0000005"):
        c.run("return dev.call(0x10, 'cdecl')")
    with pytest.raises(PseError, match="time limit"):
        c.run("while true do end", limit_ms=200)
    with pytest.raises(PseError):
        c.run("this is not lua")
    with pytest.raises(PseError, match="mine"):
        c.run("error('mine')")
    assert c.frames_advancing(2.0)


def test_print_and_values(c):
    r = c.request("print('a', 1, nil) return {1, 2, 3}, {x = 1}, nil, 'ü'")
    assert r["print"] == ["a\t1\tnil"]
    assert r["result"] == [[1, 2, 3], {"x": 1}, None, "ü"]
    assert r["thread"] == "game"


def test_log_tail(c):
    c.run("dev.log_write('live test marker 1234')")
    assert any("live test marker 1234" in l for l in c.log(20))


def test_console(c):
    out = c.console("help")
    assert out and out[0] == "> help"


def test_async_while_game_thread_busy(game):
    """A sync request keeps the game thread busy for 1.5 s; an async request on a second connection answers meanwhile."""
    done = {}
    def busy():
        with game.connect() as c2:
            done["sync"] = c2.run("dev.sleep(1500) return dev.frame()")
            done["sync_t"] = time.perf_counter()
    t = threading.Thread(target=busy)
    t.start()
    time.sleep(0.3)
    with game.connect() as c3:
        t0 = time.perf_counter()
        f = c3.run("return dev.frame()", async_=True)
        done["async_t"] = time.perf_counter()
        assert c3.last["thread"] == "pipe"
    t.join(10)
    print("\n[live] async answer during a busy game thread: %.2f ms" % ((done["async_t"] - t0) * 1000))
    assert done["async_t"] < done["sync_t"]
    assert done["sync"] >= f


def test_console_command_sync_only(c):
    with pytest.raises(PseError, match="game thread only"):
        c.run("return dev.console('help')", async_=True)


def test_mission1_play(c):
    """Starts Mission 1 through the console, answers PLEASE PRESS ANY KEY, reads player 1."""
    t0 = time.time()
    c.console("level Mission1.dll")
    answered_while_stalled = 0
    while time.time() - t0 < 60:   # the load is one long game frame: async requests must still be answered
        if not c.frames_advancing(0.3):
            st = c.state()
            if st["dispatch"] != 0 or st["level"].lower().startswith("mission1"):
                answered_while_stalled += 1
                if answered_while_stalled >= 3: break
    p = c.wait_for_play(timeout=180)
    load_s = time.time() - t0
    print("\n[live] Mission 1 playable after %.1f s; player 1 %r" % (load_s, p))
    assert answered_while_stalled >= 1
    assert c.state()["level"].lower().startswith("mission1")
    assert p["soldier"] and p["health"] > 0
    assert all(isinstance(p[k], float) for k in ("x", "y", "z", "health"))
    assert abs(p["x"]) < 1e6 and abs(p["z"]) < 1e6
    assert c.player(0, async_=False)["soldier"] == p["soldier"]
