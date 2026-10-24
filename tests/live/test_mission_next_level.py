"""Live: the next mission started from the MISSION SUCCESSFUL screen on the Lua path (Dev\\MissionSource = lua,
src/features/missionsource.cpp; docs/internal/missions.md "Mission 1: deeper checks").

Mission 1 from its translated Lua, the game's debug "complete mission" (0x444DC0 = script message 0x11 -> +0x48C(2),
+0x490("Mission2.dll")), then the end-screen chain the game's own way: 0x12 MISSION SUCCESSFUL (Enter) -> 0x3A ->
0x10 (Enter does nothing: its update FUN_00459ad0 moves on with the menu BACK action FUN_00478380 = Esc / pad
Triangle, then copies campaign +0x37 into the next-level slot and returns 3 when campaign +0 = 0xE, else state 0x13)
-> 0x13 / 0x0A -> Mission 2 loads. Checks: Mission 2 plays and runs from the Lua cache too (the first image is freed
and the next one loaded), no [fail] / game error lines on the way.
Needs tools/bg.py (local tooling).
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

STATE = ("local s = dev.state() local c = dev.u32(0x60EDC0) "
         "if c ~= 0 then s.result = dev.u32(c + 4) s.campaign_next = dev.cstring(c + 0x37) end return s")
STATE_END_STATS = 0x10


@pytest.fixture(scope="module")
def run():
    dev = {"StartLevel": "", "SplitPlayers": 0, "MissionSource": "lua", "PseRebuild": 1, "NoErrorBoxes": 1}
    # FeatMods 0: a mod's mission replacement (RestoredMissions) would win over the translated cache
    enh = {"FeatLoadout": 0, "SkipCutscenes": 1, "FpsCap": 60, "FeatMods": 0}
    res = {"states": []}
    with bg.Game(dev=dev, enh=enh) as g:
        c = pse_client.Client(pse_client.wait_for_pipe(g.p.pid, timeout=120))
        c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
        c.start_level("Mission1.dll", wait=True, timeout=240)
        time.sleep(3)
        c.run("dev.call(0x444DC0, 'cdecl') return 1")
        res["after_complete"] = c.run(STATE, async_=True)
        t0, last_key = time.time(), 0.0
        while time.time() - t0 < 180 and g.alive():
            st = c.run(STATE, async_=True, timeout=10)
            if not res["states"] or res["states"][-1] != (st["fe_state"], st["dispatch"]):
                res["states"].append((st["fe_state"], st["dispatch"]))
            if "mission2" in (st.get("level") or "").lower() and st["fe_state"] == 0xE and st["dispatch"] == 0:
                res["played"] = c.frames_advancing(1.0)
                break
            if time.time() - last_key > 1.5:
                if st["fe_state"] == 0x14: c.close_briefing()
                elif not c.frames_advancing(1.0): c.press("space")
                else: c.press("esc" if st["fe_state"] == STATE_END_STATS else "return")
                last_key = time.time()
            time.sleep(0.3)
        res["log"] = c.log(8000) or []
        c.close()
    print("\n[next] states:", " ".join("%X/%d" % s for s in res["states"]))
    return res


def test_mission1_asked_for_mission2(run):
    st = run["after_complete"]
    assert st.get("result") == 2 and st.get("campaign_next") == "Mission2.dll", st


def test_mission2_plays_after_the_end_screen(run):
    assert run.get("played"), "Mission 2 didn't start: states %s" % run["states"]
    assert 0x12 in [s for s, _ in run["states"]], "MISSION SUCCESSFUL not seen"


def test_both_missions_ran_from_lua(run):
    lines = [l for l in run["log"] if "Mission source" in l]
    for n in (1, 2):
        assert any("Mission%d.dll runs from Lua" % n in l for l in lines), "Mission %d not on the Lua path" % n
    assert not any("[fail]" in l for l in lines)


def test_no_game_errors(run):
    bad = [l for l in run["log"] if "[game Error]" in l or "[fail] Mission" in l]
    assert not bad, bad[:5]
