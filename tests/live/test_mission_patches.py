"""Live: mission patches (docs/sdk/reference/mission-patches.md; dsfix.missions missionsource.cpp, pse/build/mission2lua
m2r_*.cpp) - a mod's <Stem>.patch.lua applied to the readable Lua DesertStormFix makes from the game's own DLL.

Own game (FeatMods 1, MISSIONS FROM LUA off = the players' default): two test mods are put into <game>\\Mods for the run
    zz_PatchTest\\Mission1.patch.lua   an edit block (a game.log after the level-strings load in mission_init), Lua that
                                       logs SCENE / TEXT values, and an override of mission_update that logs its first call
    zz_PatchTest2\\Mission1.patch.lua  an edit block that matches nothing: this patch must be left out, the other kept
then `level Mission1.dll` and some seconds of play. Checks: the readable mission was made (or taken from the cache), the
log says Mission1.dll runs with zz_PatchTest's patch only, the edit and the patch's code ran (SCENE.objects.HERO_FOLEY
and SCENE.cameras.PLAYERVIEW are the scene's numbers, TEXT.M01_BRF_TX1 the catalog hash of that key), the overridden
mission_update was called, the mod report names the refused patch, and the game keeps running. A mod that replaces
Mission1.dll outright (a Mission1.lua with "--@replaces", which would win over patches) is moved aside for the run.
Afterwards the test mods are removed and everything moved is put back. Runner: tools/split/deploy_run.py <tree> <this>.
"""
import os, shutil, sys, time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py (local tooling)", allow_module_level=True)

PLAY_SECONDS = float(os.environ.get("PATCH_PLAY", "20"))

GOOD = """--@patches Mission1.dll
--@readable 1
-- zz_PatchTest: DesertStormFix's live test of mission patches (tests/live/test_mission_patches.py)

--[[@edit
  api.load_level_strings("M01Strs.loc")
--@to
  api.load_level_strings("M01Strs.loc")
  game.log("PATCHTEST edit ran")
]]

game.log(string.format("PATCHTEST code ran foley=%s cam=%s text=%08X", tostring(SCENE.objects.HERO_FOLEY),
                       tostring(SCENE.cameras.PLAYERVIEW), TEXT.M01_BRF_TX1))
local dll_update = mission_update
local logged = false
function mission_update(...)
  if not logged then
    logged = true
    game.log("PATCHTEST update ran")
  end
  return dll_update(...)
end
"""

BAD = """--@patches Mission1.dll
--@readable 1
--[==[@edit
  this line is in no readable mission
--@to
  game.log("PATCHTEST2 must not run")
]==]
game.log("PATCHTEST2 must not run")
"""


def lfsr(s):
    x = 1
    for c in s.encode("latin-1"):
        for b in range(8):
            bit = ((c >> b) ^ x ^ (x >> 1) ^ (x >> 21) ^ (x >> 31)) & 1
            x = ((x << 1) | bit) & 0xFFFFFFFF
    return x


def replaces_mission1(folder):
    for root, _d, files in os.walk(folder):
        for f in files:
            if f.lower() == "mission1.lua":
                try:
                    head = open(os.path.join(root, f), encoding="latin-1").read(4096).lower()
                except OSError:
                    continue
                if "--@replaces mission1.dll" in head:
                    return True
    return False


@pytest.fixture
def mods():
    root = os.path.join(bg.GAME, "Mods")
    moved = []
    os.makedirs(root, exist_ok=True)
    for name in os.listdir(root):
        p = os.path.join(root, name)
        if os.path.isdir(p) and not name.startswith(("_", ".")) and replaces_mission1(p):
            aside = os.path.join(root, "_patchtest_aside_" + name)
            os.rename(p, aside)
            moved.append((aside, p))
    made = []
    for name, text in (("zz_PatchTest", GOOD), ("zz_PatchTest2", BAD)):
        d = os.path.join(root, name)
        if os.path.exists(d):
            shutil.rmtree(d)
        os.makedirs(d)
        with open(os.path.join(d, "Mission1.patch.lua"), "w", encoding="utf-8", newline="\r\n") as f:
            f.write(text)   # (CRLF on purpose: patches made on Windows)
        made.append(d)
    try:
        yield root
    finally:
        for d in made:
            shutil.rmtree(d, ignore_errors=True)
        for aside, p in moved:
            os.rename(aside, p)


def test_mission_patch_applies(mods):
    report = os.path.join(mods, "DesertStormFix-report.txt")
    enh = {"FeatLoadout": 0, "SkipCutscenes": 1, "FpsCap": 60, "FeatMods": 1}
    dev = {"StartLevel": "", "SplitPlayers": 0, "MissionSource": "", "PseRebuild": 1}
    with bg.Game(dev=dev, enh=enh) as g:
        pid = pse_client.wait_for_pipe(g.p.pid, timeout=120)
        c = pse_client.Client(pid)
        c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
        c.start_level("Mission1.dll", wait=False)
        t0 = time.time()
        ran = False
        while time.time() - t0 < 240 and g.alive():   # Space through PLEASE PRESS ANY KEY until the update ran
            log = c.log(3000) or []
            if any("PATCHTEST update ran" in l for l in log):
                ran = True
                break
            c.run('dev.key("space", 40)', async_=True)
            time.sleep(0.5)
        t1 = time.time()
        while ran and time.time() - t1 < PLAY_SECONDS and g.alive():
            time.sleep(1)
        alive = g.alive()
        advancing = c.frames_advancing(1.0) if alive else False
        log = c.log(8000) or []
    text = "\n".join(log)
    rep = open(report, encoding="latin-1").read() if os.path.exists(report) else ""
    print("\n".join(l for l in log if "PATCHTEST" in l or "Mission source" in l or "patch" in l.lower()))
    assert ran, "the patched mission_update never ran"
    assert alive and advancing, "the game stopped"
    assert "runs with mission patches: zz_PatchTest\\Mission1.patch.lua" in text
    assert "PATCHTEST edit ran" in text
    assert "PATCHTEST2" not in text
    line = next(l for l in log if "PATCHTEST code ran" in l)
    assert "foley=nil" not in line and "cam=0" in line, line   # PLAYERVIEW is the scene's camera 0
    assert ("text=%08X" % lfsr("m01_brf_tx1")) in line, line
    assert "zz_PatchTest2\\Mission1.patch.lua" in rep and "matches 0 places" in rep, rep[-2000:]


RESTORED = os.environ.get("RESTORED_MISSIONS", os.path.join(os.path.dirname(REPO), "ConflictDesertStormModKit", "examples",
                                                             "restored_missions", "RestoredMissions"))


@pytest.fixture
def restored():
    """The ModKit's RestoredMissions (four mission patches) in <game>\\Mods for the run; an installed copy (or an
    older one that replaced the DLLs) is moved aside and put back afterwards."""
    if not os.path.isdir(RESTORED):
        pytest.skip("needs the ModKit's examples/restored_missions/RestoredMissions")
    root = os.path.join(bg.GAME, "Mods")
    dst = os.path.join(root, "RestoredMissions")
    moved = []
    for name in ("RestoredMissions", "_RestoredMissions", "TrainingRestored", "_TrainingRestored"):
        p = os.path.join(root, name)
        if os.path.isdir(p):
            aside = os.path.join(root, "_patchtest_aside_" + name.lstrip("_"))
            os.rename(p, aside)
            moved.append((aside, p))
    shutil.copytree(RESTORED, dst)
    try:
        yield dst
    finally:
        shutil.rmtree(dst, ignore_errors=True)
        for aside, p in moved:
            os.rename(aside, p)


def test_restored_missions(restored):
    """Training 1-3 and Mission 1 one after the other (console `level`): each runs with its RestoredMissions patch,
    nothing fails, the game keeps running."""
    enh = {"FeatLoadout": 0, "SkipCutscenes": 0, "FpsCap": 60, "FeatMods": 1}
    dev = {"StartLevel": "", "SplitPlayers": 0, "MissionSource": "", "PseRebuild": 1}
    results = {}
    with bg.Game(dev=dev, enh=enh) as g:
        pid = pse_client.wait_for_pipe(g.p.pid, timeout=120)
        c = pse_client.Client(pid)
        c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
        for m in ("Training1", "Training2", "Training3", "Mission1"):
            c.start_level(m + ".dll", wait=False)
            want = "runs with mission patches: RestoredMissions\\%s.patch.lua" % m
            t0 = time.time()
            seen = False
            while time.time() - t0 < 180 and g.alive():
                if any(want in l for l in (c.log(4000) or [])):
                    seen = True
                    break
                c.run('dev.key("space", 40)', async_=True)
                time.sleep(0.5)
            t1 = time.time()   # through PLEASE PRESS ANY KEY (the log line comes during the load, before it)
            while seen and time.time() - t1 < 60 and g.alive() and not c.frames_advancing(1.0):
                c.run('dev.key("space", 40)', async_=True)
                time.sleep(0.5)
            t1 = time.time()
            while seen and time.time() - t1 < PLAY_SECONDS and g.alive():   # play on (the intro films run)
                time.sleep(1)
            results[m] = (seen, g.alive() and c.frames_advancing(1.0))
            if not g.alive():
                break
        log = c.log(20000) or [] if g.alive() else []
    text = "\n".join(log)
    print("\n".join(l for l in log if "RestoredMissions" in l or "[fail]" in l and "Mission" in l))
    for m, (seen, running) in results.items():
        assert seen, "%s: not run with its patch" % m
        assert running, "%s: the game stopped" % m
    assert len(results) == 4
    bad = [l for l in log if ("[fail]" in l and ("Mission source" in l or "translated mission" in l or "patch" in l))]
    assert not bad, bad[:5]
    assert "RestoredMissions: intro streams" in text   # Training 1's restored intro film started
