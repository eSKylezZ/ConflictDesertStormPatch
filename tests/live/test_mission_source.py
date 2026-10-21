"""Live: a mission DLL run from its C++-generated Lua (Dev\\MissionSource = lua, src/features/missionsource.cpp,
pse/build/mission2lua; docs/internal/mission-translation.md).

Two games, the same script each (own games - dev values are read at start-up): the main menu, then the console's
`level Mission1.dll`, Space through PLEASE PRESS ANY KEY, 60 s of play (the intro film plays, SkipCutscenes 0), under
the determinism harness (Dev\\DetTest: virtual clock, seeded random tables, blocked input) with the script API trace
(Dev\\ApiTrace):
  run 1  Dev\\MissionSource = dll   (the DLL, as shipped)
  run 2  Dev\\MissionSource = lua   (PseRebuild 1: the translator writes <game>\\PSE\\Cache\\missions\\Mission1.lua now)
Checks: run 2 translated Mission1.dll and ran it from Lua (log), no fallback; its API trace equals run 1's (every mission
call, the animation registrations too - the level stays a .dll level, so no script-host calls) - compared with
tools/mission_parity/compare.py on the mission's part of the traces (from its LevelSetup on), frames included; the world hashes (detcmp.py); the script object's save block (size / save) is
byte-identical. Afterwards the cache folder the run made is removed and the dev / settings values are restored.
Needs the local tooling (tools/bg.py, tools/mission_parity). Runner: run_live5.py (BUILD=<build dir>, VANILLA=0).
"""
import os, shutil, subprocess, sys, time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
TOOLS = os.path.join(REPO, "tools")
PARITY = os.path.join(TOOLS, "mission_parity")
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py (local tooling)", allow_module_level=True)
if not os.path.exists(os.path.join(PARITY, "compare.py")):
    pytest.skip("needs tools/mission_parity (local tooling)", allow_module_level=True)

PLAY_SECONDS = float(os.environ.get("M2L_PLAY", "60"))
SAVE_LUA = ("local obj = dev.u32(0x60F578); local size = dev.call(dev.u32(obj + 0x14), 'cdecl'); "
            "local buf = dev.alloc(size + 64); local e = dev.call(dev.u32(obj + 0xC), 'cdecl', buf); "
            "local hex = dev.bytes(buf, size); dev.free(buf); return {size, e - buf, hex}")


def run_mission(mode, n, out):
    trace = os.path.join(bg.GAME, "DesertStormFix-apitrace-%d.txt" % n)
    det = os.path.join(bg.GAME, "DesertStormFix-det-%d.txt" % n)
    for f in (trace, det):
        if os.path.exists(f):
            os.remove(f)
    dev = {"StartLevel": "", "SplitPlayers": 0, "MissionSource": mode, "PseRebuild": 1, "ApiTrace": n, "DetTest": 1,
           "DetRun": n, "DetFrames": 3600, "DetKeysAfterBriefing": 0}
    enh = {"FeatLoadout": 0, "SkipCutscenes": 0, "FpsCap": 60}
    res = {}
    with bg.Game(dev=dev, enh=enh) as g:
        pid = pse_client.wait_for_pipe(g.p.pid, timeout=120)
        c = pse_client.Client(pid)
        c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
        t0 = time.time()
        c.start_level("Mission1.dll", wait=False)
        played = None   # Space every 0.25 s through PLEASE PRESS ANY KEY (the determinism notes)
        while time.time() - t0 < 240 and g.alive():
            st = c.run("return dev.state()", async_=True)
            if st.get("dispatch") == 0 and st.get("fe_state") not in (None, 1) and \
                    any("levelInitialise" in l for l in (c.log(400) or [])):
                played = played or time.time()
                if time.time() - played > 1.0:
                    break
            c.run('dev.key("space", 40)', async_=True)
            time.sleep(0.25)
        print("\n[m2l] %s: playing after %.1f s" % (mode, time.time() - t0))
        t1 = time.time()
        while time.time() - t1 < PLAY_SECONDS and g.alive():   # (bounded: PLAY_SECONDS)
            time.sleep(1)
            try:   # FeatLoadout 0 keeps the loadout screen shut; if it ever opens, close it (posted keys don't reach it)
                c.run("return dev.loadout_close()", timeout=10)
            except Exception:
                pass
        res["alive"] = g.alive()
        res["advancing"] = c.frames_advancing(1.0)
        res["save"] = c.run(SAVE_LUA)
        res["log"] = c.log(5000) or []
        c.close()
    for f, name in ((trace, "trace"), (det, "det")):
        dst = os.path.join(out, "%s_%s.txt" % (mode, name))
        if os.path.exists(f):
            shutil.move(f, dst)
        res[name] = dst
    return res


@pytest.fixture(scope="module")
def runs(tmp_path_factory):
    out = str(tmp_path_factory.mktemp("m2l"))
    cache = os.path.join(bg.GAME, "PSE", "Cache")
    missions = os.path.join(cache, "missions")
    had_cache, had_missions = os.path.isdir(cache), os.path.isdir(missions)
    try:
        r = {"dll": run_mission("dll", 71, out), "lua": run_mission("lua", 72, out), "out": out}
        r["cached"] = sorted(os.listdir(missions)) if os.path.isdir(missions) else []
        yield r
    finally:
        if not had_missions:
            shutil.rmtree(missions, ignore_errors=True)
        if not had_cache:
            shutil.rmtree(cache, ignore_errors=True)


def test_lua_run_translated_and_ran_the_mission(runs):
    lines = runs["lua"]["log"]
    ms = [l for l in lines if "Mission source" in l]
    for l in ms:
        print("[m2l]", l)
    assert any("[ok]   Mission source: Dev\\MissionSource = lua" in l for l in lines)
    assert any("Mission1.dll translated by mission2lua" in l for l in ms), "no translation logged"
    assert any("Mission1.dll runs from Lua" in l for l in ms), "the translated mission didn't start"
    assert not any("[fail] Mission source" in l for l in ms)
    assert any("DLL_PROCESS_ATTACH" in l or "running from Lua" in l for l in lines)
    assert "Mission1.lua" in runs["cached"] and "Mission1.stamp" in runs["cached"], runs["cached"]
    assert runs["lua"]["alive"] and runs["lua"]["advancing"]
    dll_lines = runs["dll"]["log"]
    assert not any("runs from Lua" in l for l in dll_lines), "MissionSource = dll ran the Lua"


def mission_block(path):
    """The trace from the mission's own LevelSetup on (the last version check, slot 0 at frame 0): the front end before
    it is a DLL in both runs, and how long it sat at the main menu is real time."""
    lines = open(path, encoding="latin-1").read().splitlines()
    start = max((k for k, l in enumerate(lines) if len(l.split()) > 3 and l.split()[1] == "0" and int(l.split()[3], 16) == 0),
                default=0)
    out = path[:-4] + "_mission.txt"
    with open(out, "w", encoding="latin-1", newline="\n") as f:
        f.write("\n".join(lines[start:]) + "\n")
    return out


def test_api_trace_equals_the_dll(runs):
    a, b = mission_block(runs["dll"]["trace"]), mission_block(runs["lua"]["trace"])
    r = subprocess.run([sys.executable, os.path.join(PARITY, "compare.py"), a, b, "--frames"], capture_output=True, text=True)
    print(r.stdout[-3000:], r.stderr[-2000:])
    assert r.returncode == 0
    counts = [l for l in r.stdout.splitlines() if l.startswith("mission calls:")][0]
    same = [l for l in r.stdout.splitlines() if l.startswith("identical mission calls")][0]
    # "mission calls: dll A, lua B; anim registrations: dll X, lua Y"
    nums = [int(x.strip(",;()")) for x in counts.replace(";", " ").split() if x.strip(",;()").isdigit()]
    assert nums[0] > 1000 and abs(nums[0] - nums[1]) <= 8, counts   # (the games are killed a frame apart)
    assert nums[2] == nums[3] and nums[2] >= 196, "animation registrations differ: " + counts
    assert "lua anims not in dll: []" in r.stdout
    k = [int(x) for x in same.replace("/", " ").split() if x.isdigit()]
    assert k[0] == min(k[1], k[2]), same   # every call both traces have is identical, frames included
    # "identical up to frame F (rebased; dll trace ends at frame A, lua at B)": only the last frame may be cut off
    up = [l for l in r.stdout.splitlines() if l.startswith("identical up to frame")][0]
    f = [int(x.strip(",;()")) for x in up.split() if x.strip(",;()").lstrip("-").isdigit()]
    assert f[0] >= min(f[1], f[2]) - 1, up


def test_world_hashes_equal(runs):
    r = subprocess.run([sys.executable, os.path.join(PARITY, "detcmp.py"), runs["dll"]["det"], runs["lua"]["det"]],
                       capture_output=True, text=True)
    print(r.stdout[-2000:], r.stderr[-1000:])
    assert r.returncode == 0
    assert "first different world hash: None" in r.stdout


def test_save_block_is_the_dlls(runs):
    a, b = runs["dll"]["save"], runs["lua"]["save"]
    print("[m2l] save block: dll %d bytes (save wrote %d), lua %d (%d)" % (a[0], a[1], b[0], b[1]))
    assert a[0] == b[0] and a[1] == b[1] == a[0]
    assert a[2] == b[2], "save blocks differ"
