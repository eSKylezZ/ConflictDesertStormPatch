"""The DS1 game layer on its own: a plugin gets frames, render / D3D events, level events and the co-save with NO
DesertStormFix installed (pse/games/ds1/ds1_events.cpp, docs/sdk/reference/game-ds1.md "Events and messages").

Runs its own game (not the session's): dsfix.dll / .pdb / .pse are moved out of PSE\\Plugins for the run and put back
afterwards, test.layer_harness (TEST ONLY, pse/tests/layer_harness: boots Mission1.dll windowed off-screen, unfocused,
muted, then saves into a temp folder, loads that save and reloads its level) is copied in and removed again, and
example.events_logger (deployed by the live runner) logs what it got. No dev server (that lives in dsfix): everything
is read from the two plugins' log files next to their DLLs and from the co-save file.

Opt-in: PSE_NO_DSFIX=1 (it needs the game install to itself - run it in its own pytest call, after / without the
session game) and PSE_PLUGIN_BUILD_DIR = the build's pse_plugins\\<config> folder (for the harness DLL). The live runner
scratchpad run_live_nodsfix.py does both inside the install lock."""
import contextlib, os, shutil, struct, sys, tempfile, time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(HERE, "..", "..", "tools")))

pytestmark = pytest.mark.skipif(os.environ.get("PSE_NO_DSFIX") != "1",
                                reason="opt-in: PSE_NO_DSFIX=1 (runs its own game without dsfix.dll)")
LEVEL = "Mission1.dll"


def _modules(pid):
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)

    class ME(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("a", wintypes.DWORD), ("pid", wintypes.DWORD), ("b", wintypes.DWORD),
                    ("c", wintypes.DWORD), ("base", ctypes.c_void_p), ("size", wintypes.DWORD),
                    ("h", wintypes.HMODULE), ("name", ctypes.c_wchar * 256), ("path", ctypes.c_wchar * 260)]
    k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    snap = k32.CreateToolhelp32Snapshot(0x18, pid)
    out, me = [], ME()
    me.dwSize = ctypes.sizeof(me)
    ok = k32.Module32FirstW(snap, ctypes.byref(me))
    while ok:
        out.append(me.path.lower())
        ok = k32.Module32NextW(snap, ctypes.byref(me))
    k32.CloseHandle(snap)
    return out


def _lines(path):
    return [l.rstrip("\n") for l in open(path, encoding="utf-8", errors="replace")] if os.path.exists(path) else []


def _index(lines, text, start=0):
    for i in range(start, len(lines)):
        if text in lines[i]:
            return i
    return -1


def _chunks(path):
    b = open(path, "rb").read()
    assert b[:4] == b"PSE\x01", b[:8]
    n, p, out = struct.unpack_from("<I", b, 4)[0], 8, []
    for _ in range(n):
        ln = b[p]; p += 1
        name = b[p:p + ln].decode(); p += ln
        t, v, size = struct.unpack_from("<III", b, p); p += 12
        out.append((name, struct.pack(">I", t).decode("latin-1"), v, b[p:p + size])); p += size
    return out


def test_plugins_get_events_and_cosave_without_dsfix():
    import bg
    plugins = os.path.join(bg.GAME, "PSE", "Plugins")
    build = os.environ.get("PSE_PLUGIN_BUILD_DIR", "")
    harness_src = os.path.join(build, "test.layer_harness.dll")
    if not os.path.exists(harness_src):
        pytest.skip("test.layer_harness.dll not built (PSE_PLUGIN_BUILD_DIR=%r)" % build)
    if not os.path.exists(os.path.join(plugins, "example.events_logger.dll")):
        pytest.skip("example.events_logger is not deployed")
    if not os.path.exists(os.path.join(bg.GAME, "PSE", "Games", "pse_ds1.dll")):
        pytest.skip("no game layer installed")
    away = os.path.join(bg.GAME, "PSE", ".no_dsfix_test")
    save_dir = os.path.join(tempfile.gettempdir(), "pse_layer_test")
    save = os.path.join(save_dir, "PSE Layer Test.sav")
    logger_log = os.path.join(plugins, "example.events_logger.log")
    harness_log = os.path.join(plugins, "test.layer_harness.log")
    # Inside a runner that holds the install lock (BG_LOCK_PARENT) bg.Game uses it as it is; a second install_lock here
    # would replace BG_LOCK_PARENT with this process and make bg.Game wait for its own runner.
    lock = contextlib.nullcontext() if os.environ.get("BG_LOCK_PARENT") else bg.install_lock()
    with lock:
        moved = []
        try:
            os.makedirs(away, exist_ok=True)
            for f in os.listdir(plugins):
                if f.lower().startswith("dsfix.") and f.lower().split(".")[1] in ("dll", "pdb", "pse"):
                    shutil.move(os.path.join(plugins, f), os.path.join(away, f))
                    moved.append(f)
            assert "dsfix.dll" in [m.lower() for m in moved], "no dsfix.dll to move away: %r" % os.listdir(plugins)
            for ext in (".dll", ".pse"):
                shutil.copy2(os.path.join(build, "test.layer_harness" + ext), plugins)
            shutil.rmtree(save_dir, ignore_errors=True)
            os.makedirs(save_dir)
            with bg.Game(dev={"PseLayerHarness": LEVEL, "PseLayerHarnessSave": save, "StartLevel": ""}) as g:
                t0, done = time.time(), False
                while time.time() - t0 < 180 and g.alive():
                    if _index(_lines(harness_log), "harness: done") >= 0:
                        done = True
                        break
                    time.sleep(0.5)
                mods = _modules(g.p.pid) if g.alive() else []
                alive = g.alive()
            harness, logger = _lines(harness_log), _lines(logger_log)
            print("\n[live] harness:", *harness, sep="\n    ")
            print("[live] events_logger:", *logger, sep="\n    ")
            assert alive, "the game died (harness: %r)" % harness[-5:]
            assert done, "the harness never finished: %r" % harness[-5:]
            assert not any(m.endswith("\\dsfix.dll") for m in mods), "dsfix.dll was loaded"
            assert any(m.endswith("\\pse_ds1.dll") for m in mods), "no game layer in the process"
            assert "active: level" in " ".join(harness) and "patches ok" in " ".join(harness), harness

            # Start-up + the first level: everything the layer drives, with nothing of DesertStormFix around.
            for text in ("loaded (game ds1", "message POST_LOAD", "message DEVICE_CREATED", "event FRAME first",
                         "event PRE_RENDER first", "event POST_RENDER first", "event END_SCENE first",
                         "event PRESENT first"):
                assert _index(logger, text) >= 0, (text, logger)
            revert = _index(logger, "cosave revert")
            new_game = _index(logger, "message NEW_GAME " + LEVEL)
            begin = _index(logger, "event LEVEL_LOAD_BEGIN " + LEVEL)
            end = _index(logger, "event LEVEL_LOAD_END " + LEVEL)
            assert 0 <= revert < new_game < begin < end, logger

            # Save: SAVE_BEGIN, the plugin's save callback, the .pse next to the save, SAVE_END.
            sb = _index(logger, "message SAVE_BEGIN", end)
            cs = _index(logger, "cosave save frames=", end)
            se = _index(logger, "message SAVE_END", end)
            assert 0 <= sb < cs < se, logger[end:]
            co = os.path.join(save_dir, "PSE Layer Test.pse")
            assert os.path.exists(save) and os.path.exists(co), os.listdir(save_dir)
            chunks = _chunks(co)
            assert [c for c in chunks if c[0] == "example.events_logger" and c[1] == "EVTS" and c[2] == 1], chunks
            saved_frames = struct.unpack("<I", [c for c in chunks if c[0] == "example.events_logger"][0][3])[0]
            assert "cosave save frames=%d" % saved_frames in logger[cs]

            # Load: LOAD_BEGIN, the chunk back through the load callback, LOAD_END; then the save's level loads again
            # (unload + load) with NO new game.
            lb = _index(logger, "message LOAD_BEGIN", se)
            cl = _index(logger, "cosave load frames=%d" % saved_frames, se)
            le = _index(logger, "message LOAD_END", se)
            assert 0 <= lb < cl < le, logger[se:]
            unload = _index(logger, "event LEVEL_UNLOAD", le)
            begin2 = _index(logger, "event LEVEL_LOAD_BEGIN", le)
            end2 = _index(logger, "event LEVEL_LOAD_END", le)
            assert 0 <= unload < begin2 < end2, logger[le:]
            assert _index(logger, "message NEW_GAME", le) < 0, "a loaded save got NEW_GAME"
            assert _index(logger, "cosave revert", le) < 0, logger[le:]
        finally:
            for f in moved:
                shutil.move(os.path.join(away, f), os.path.join(plugins, f))
            shutil.rmtree(away, ignore_errors=True)
            for ext in (".dll", ".pse", ".log"):
                p = os.path.join(plugins, "test.layer_harness" + ext)
                if os.path.exists(p): os.remove(p)
            shutil.rmtree(save_dir, ignore_errors=True)
