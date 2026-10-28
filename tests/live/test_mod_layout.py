"""Live: organised mod folders + the mod priority list (docs/internal/modding-parity.md A.3; mods.cpp BuildIndex /
ScanFolder, core/modorder.h).

Test mods made before the game starts (removed afterwards):
  zz_LayoutDeep   organised: evos\\props\\LAYOUT_DEEP.EVO, dds\\LAYOUT_DEEP.DDS, a name it has twice
                  (evos\\LAYOUT_DUP.EVO, old\\LAYOUT_DUP.EVO - the last by sorted path wins: old\\)
  zz_OrderA       evos\\LAYOUT_SHARED.EVO
  zz_OrderB       LAYOUT_SHARED.EVO (flat)
The priority list is set to load zz_OrderB before zz_OrderA (against the alphabet), so zz_OrderA's copy must win.
Checked through the game's own resolver FUN_004BE810(name, paths, flags) -> path (mods.cpp hooks it) and the mod
report lines. The settings value ModOrder is restored (deleted if it wasn't there) afterwards.
Needs tools/bg.py; uses the installed build. Run on its own (bg.Game starts its own game).
"""
import os, shutil, sys, winreg

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "tools"))
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py", allow_module_level=True)

KEY = r"Software\PSE\ds1\Plugins\dsfix"
MODS = ("zz_LayoutDeep", "zz_OrderA", "zz_OrderB")
FILES = {
    "zz_LayoutDeep/evos/props/LAYOUT_DEEP.EVO": b"deep",
    "zz_LayoutDeep/dds/LAYOUT_DEEP.DDS": b"deep",
    "zz_LayoutDeep/evos/LAYOUT_DUP.EVO": b"first",
    "zz_LayoutDeep/old/LAYOUT_DUP.EVO": b"second",
    "zz_OrderA/evos/LAYOUT_SHARED.EVO": b"A",
    "zz_OrderB/LAYOUT_SHARED.EVO": b"B",
}
RESOLVE = 'local p = dev.call(0x4BE810, "cdecl", "%s", 0, 0) if p == 0 then return "" end return dev.cstring(p)'


def read_order():
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as k:
            return winreg.QueryValueEx(k, "ModOrder")[0]
    except OSError:
        return None


def write_order(value):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, KEY) as k:
        if value is None:
            try:
                winreg.DeleteValue(k, "ModOrder")
            except OSError:
                pass
        else:
            winreg.SetValueEx(k, "ModOrder", 0, winreg.REG_SZ, value)


@pytest.fixture(scope="module")
def run():
    res = {}
    with bg.install_lock():
        before = read_order()
        for m in MODS:
            shutil.rmtree(os.path.join(bg.GAME, "Mods", m), ignore_errors=True)
        for rel, data in FILES.items():
            p = os.path.join(bg.GAME, "Mods", rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, "wb") as f:
                f.write(data)
        # zz_OrderB first, zz_OrderA after it: A loads later and wins; the player's own mods keep their saved places
        saved = [n for n in (before or "").split("|") if n and n not in MODS]
        write_order("|".join(saved + ["zz_OrderB", "zz_LayoutDeep", "zz_OrderA"]))
        try:
            dev = {"StartLevel": "", "SplitPlayers": 0, "NoErrorBoxes": 1}
            enh = {"FeatMods": 1, "SkipIntro": 1}
            with bg.Game(dev=dev, enh=enh) as g:
                c = pse_client.Client(pse_client.wait_for_pipe(g.p.pid, timeout=120))
                c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=120)
                for name in ("LAYOUT_DEEP.EVO", "layout_deep.dds", "LAYOUT_DUP.EVO", "LAYOUT_SHARED.EVO"):
                    res[name.upper()] = c.run(RESOLVE % name)
                res["log"] = c.log(4000) or []
                c.close()
            res["order_after"] = read_order()
        finally:
            for m in MODS:
                shutil.rmtree(os.path.join(bg.GAME, "Mods", m), ignore_errors=True)
            write_order(before)
    print("\n[layout]", {k: v for k, v in res.items() if k != "log"})
    for line in res["log"]:
        if "Mods:" in line and ("zz_" in line or "load order" in line):
            print("[layout]", line)
    return res


def norm(p):
    return (p or "").replace("/", "\\").lower()


def test_files_two_folders_deep_are_found(run):
    assert norm(run["LAYOUT_DEEP.EVO"]).endswith(r"mods\zz_layoutdeep\evos\props\layout_deep.evo"), run["LAYOUT_DEEP.EVO"]
    assert norm(run["LAYOUT_DEEP.DDS"]).endswith(r"mods\zz_layoutdeep\dds\layout_deep.dds"), run["LAYOUT_DEEP.DDS"]


def test_same_name_twice_in_one_mod_last_sorted_path_wins(run):
    assert norm(run["LAYOUT_DUP.EVO"]).endswith(r"zz_layoutdeep\old\layout_dup.evo"), run["LAYOUT_DUP.EVO"]
    assert any("[warn] Mods: zz_LayoutDeep has two files named LAYOUT_DUP.EVO" in l and r"old\LAYOUT_DUP.EVO is used" in l
               for l in run["log"])


def test_priority_list_last_loaded_wins(run):
    assert norm(run["LAYOUT_SHARED.EVO"]).endswith(r"zz_ordera\evos\layout_shared.evo"), run["LAYOUT_SHARED.EVO"]
    log = run["log"]
    order = next((l for l in log if "Mods: load order" in l), "")
    assert order.find("zz_OrderB") < order.find("zz_LayoutDeep") < order.find("zz_OrderA"), order
    assert any("Mods: zz_OrderA replaces zz_OrderB's LAYOUT_SHARED.EVO" in l for l in log)


def test_saved_list_kept(run):
    names = (run["order_after"] or "").split("|")
    assert names.index("zz_OrderB") < names.index("zz_LayoutDeep") < names.index("zz_OrderA"), run["order_after"]
