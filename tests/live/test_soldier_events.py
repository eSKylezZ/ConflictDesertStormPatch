"""Live: the DS1 game layer's soldier events ("ds1.soldier_damaged" / "ds1.soldier_killed", pse_game_ds1.h) reach an
external plugin. Mission 1 with the dev value GoreKill = 3 (dsfix dev builds: 3 s into play the six nearest enemies
are killed, plain kills through the soldiers' damage function and explosions alternately); example.events_logger
(deployed by the runner into PSE\\Plugins) logs the events into example.events_logger.log next to its DLL.
Needs tools/bg.py and a dev build with the example plugin deployed.
"""
import os, sys, time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py (local tooling)", allow_module_level=True)

LOG = os.path.join(bg.GAME, "PSE", "Plugins", "example.events_logger.log")


@pytest.fixture(scope="module")
def run():
    if not os.path.exists(os.path.join(bg.GAME, "PSE", "Plugins", "example.events_logger.dll")):
        pytest.skip("example.events_logger is not deployed")
    dev = {"StartLevel": "", "SplitPlayers": 0, "NoErrorBoxes": 1, "GoreKill": 3}
    enh = {"FeatLoadout": 0, "SkipCutscenes": 1, "FpsCap": 60}
    with bg.Game(dev=dev, enh=enh) as g:
        c = pse_client.Client(pse_client.wait_for_pipe(g.p.pid, timeout=120))
        c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
        c.start_level("Mission1.dll", wait=True, timeout=240)
        t0 = time.time()
        while time.time() - t0 < 30 and g.alive():
            if sum("ds1.soldier_killed" in l for l in open(LOG, encoding="utf-8", errors="replace")) >= 3:
                break
            time.sleep(0.5)
        # the other game events, through the game's own functions: Mission 1's fuel truck (309) wrecked, Bradley (0)
        # picks up Foley's medikit (52), objective 0 completed, the debug "complete mission"
        obj = "local t = dev.u32(dev.u32(0x63C9A0) + 0x30) local function o(id) return dev.u32(t + id * 4) end "
        f32 = "local function f(x) return string.unpack('<I4', string.pack('<f', x)) end "
        c.run(obj + f32 + "dev.call(0x43E1F0, 'thiscall', o(309), f(100000.0), 0, 1, f(1.0), 0) return 1")
        c.run(obj + "dev.call(0x432070, 'thiscall', o(0), o(52)) return 1")
        c.wait_until("dev.u32(0x60EDC0) ~= 0 and dev.u8(dev.u32(0x60EDC0) + 0x12E8) > 0", timeout=60)   # objectives
        c.run("dev.call(0x4819E0, 'cdecl', 0) return 1")
        c.run("dev.call(0x444DC0, 'cdecl') return 1")
        time.sleep(1.5)
        log = c.log(8000) or []
        c.close()
    lines = [l.rstrip("\n") for l in open(LOG, encoding="utf-8", errors="replace")]
    for l in lines:
        if "ds1." in l:
            print("[soldier]", l)
    return {"lines": lines, "log": log}


def test_hook_installed(run):
    assert any("event hooks installed (frame, render, level, save / load, game events, D3D)" in l for l in run["log"])


def test_killed_events(run):
    killed = [l for l in run["lines"] if "ds1.soldier_killed" in l]
    assert len(killed) >= 3, run["lines"][-10:]
    for l in killed:   # "ds1.soldier_killed <id> by <id> amount a health b -> c": went from above 0 to 0 or below
        before, after = (float(x) for x in l.split("health ")[1].split(" -> "))
        assert before > 0 >= after, l


def test_damaged_before_killed(run):
    lines = run["lines"]
    first_kill = next(i for i, l in enumerate(lines) if "ds1.soldier_killed" in l)
    victim = lines[first_kill].split("ds1.soldier_killed ")[1].split(" ")[0]
    assert "ds1.soldier_damaged " + victim + " " in lines[first_kill - 1], lines[first_kill - 2:first_kill + 1]


def test_vehicle_destroyed(run):
    lines = run["lines"]
    assert any(l.split("] ")[1].startswith("ds1.vehicle_destroyed 309 ") for l in lines), lines[-12:]


def test_pick_up_objective_mission_end(run):
    text = [l.split("] ", 1)[1] for l in run["lines"]]
    assert "ds1.item_picked_up 0 took 52" in text, text[-12:]
    assert any(t.startswith("ds1.objective_completed 0 title 0x") for t in text), text[-12:]
    assert "ds1.mission_end 2" in text, text[-12:]


def test_script_messages(run):
    """The same game moments as the mission script saw them: a kill (0x00) for a dev kill's victim, the pick-up (0x03:
    Bradley 0, medikit 52)."""
    text = [l.split("] ", 1)[1] for l in run["lines"]]
    first_kill = next(t for t in text if t.startswith("ds1.soldier_killed "))
    victim = first_kill.split(" ")[1]
    assert any(t.startswith("ds1.script_message 0x00 " + victim + " ") for t in text), text[-20:]
    assert "ds1.script_message 0x03 0 52" in text, text[-20:]
