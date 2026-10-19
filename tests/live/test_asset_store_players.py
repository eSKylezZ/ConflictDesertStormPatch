"""Live: the asset store as players get it (src/features/assetstore.cpp, docs/sdk/asset-store.md "For players").

The player setting (plugin dsfix.assetstore: SharedAssetStore = 1), no Dev\\PseStore. Three games of its own, one after
the other (tools/bg.py, off-screen):
  1. first boot, cancelled with the pad (Dev\\PadInject = Circle) while the progress window is up: the window is
     screenshotted (PrintWindow), the game starts without the store, no stamp is left behind;
  2. first boot built behind the window (screenshot, build time), then Mission 1, Mission 3 and Mission 9 with
     everything Borrow* used to copy served by the store instead:
       - Mission 3: LoadoutTest "Bradley=US_WPN_SAW_lightmg;Foley=IR_WPN_AKSU" (the SAW's bank is in no Mission 3 cache;
         the AKSU's model and bank are in no Mission 3 file - a FullArsenal-style "id = IR_WPN_AKSU" .weapon makes it
         choosable) + a test mod placing a .unit soldier (Iraqi_Militia, based on Iraqi_Conscript, AKSU) as the
         armoury guard; both guns fire;
       - Mission 9: the vehicle swap Bradley -> BMP-2 (its files are in mission1.dat only);
       - every level cache built from the store, "not in the sound cache" errors = 0;
  3. the same three levels with the store off (Borrow* copies on): level load times / memory to compare.
Afterwards the test mod, the store cache this run created (<game>\\PSE\\Cache) and every value are removed / restored.
Output (screenshots, timings): PSE_STORE_OUT (default: a folder in %TEMP%).
"""
import ctypes, ctypes.wintypes as wt, hashlib, json, os, shutil, sys, tempfile, threading, time, winreg

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools"))
sys.path.insert(0, HERE)
sys.path.insert(0, TOOLS)
import pse_client  # noqa: E402

try:
    import bg  # noqa: E402
    from io_scene_evo import archive  # noqa: E402
    from io_scene_evo.evo import archive_hash  # noqa: E402
except ImportError:
    pytest.skip("needs tools/bg.py (local tooling)", allow_module_level=True)

STORE_KEY = r"Software\PSE\ds1\Plugins\dsfix.assetstore"
SWAP_NAME, SWAP = "VehiclesSPMission9", "US_VEH_M2A1_Bradley=IR_VEH_BMP-2"
LOADOUT = "Bradley=US_WPN_SAW_lightmg;Foley=IR_WPN_AKSU"
MOD = os.path.join(bg.GAME, "Mods", "PseStoreTest")
OUT = os.environ.get("PSE_STORE_OUT") or os.path.join(tempfile.gettempdir(), "pse-store-live")
CACHE = os.path.join(bg.GAME, "PSE", "Cache")
SOLDIER_VTABLE = 0x5D88E0
LEVELS = ["Mission1.dll", "Mission3.dll", "Mission9.dll"]
RESULTS = {}

u = ctypes.windll.user32
g32 = ctypes.windll.gdi32
k32 = ctypes.windll.kernel32


def elf(s):
    h = 0
    for c in s.encode():
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h = (h & 0x0FFFFFFF) ^ (g >> 24)
    return h


# ---- the test mod: a .unit soldier in Mission 3 + a FullArsenal-style weapon (no files) ----
UNIT = """# PSE asset store test: a new soldier type whose gun only another level's archive ships
id         = Iraqi_Militia
based on   = Iraqi_Conscript
health     = 60
experience = Veteran
item       = IR_WPN_AKSU
"""
WEAPON = """# PSE asset store test: the game's AKSU choosable in every level (FullArsenal style, no files)
id = IR_WPN_AKSU
"""


def make_mod():
    os.makedirs(MOD, exist_ok=True)
    open(os.path.join(MOD, "Iraqi_Militia.unit"), "w").write(UNIT)
    open(os.path.join(MOD, "AKSU.weapon"), "w").write(WEAPON)
    data = open(os.path.join(bg.GAME, "mission3.dat"), "rb").read()
    entries = {h: (o, s) for h, o, s in archive.entries(data)}
    o, s = entries[archive_hash("MISSION3.ENV")]
    env = data[o:o + s]
    old = b'"Iraqi_Conscript" 1 218 "ARMOURY_GUARD"'
    assert env.count(old) == 1, "Mission 3's armoury guard isn't where the test expects it"
    open(os.path.join(MOD, "MISSION3.ENV"), "wb").write(env.replace(old, b'"Iraqi_Militia" 1 218 "ARMOURY_GUARD"'))


# ---- the progress window ----
def find_progress():
    found = []

    def cb(h, _):
        b = ctypes.create_unicode_buffer(64)
        u.GetClassNameW(h, b, 64)
        if b.value == "PSEAssetStoreBuild":
            found.append(h)
            return False
        return True
    u.EnumWindows(ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)(cb), 0)
    return found[0] if found else 0


def shot_window(hwnd, path):
    from PIL import Image
    r = wt.RECT()
    u.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    hdc = u.GetDC(0)
    mem = g32.CreateCompatibleDC(hdc)
    bmp = g32.CreateCompatibleBitmap(hdc, w, h)
    g32.SelectObject(mem, bmp)
    u.PrintWindow(hwnd, mem, 2)   # PW_RENDERFULLCONTENT, whole window

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", wt.DWORD), ("biWidth", wt.LONG), ("biHeight", wt.LONG), ("biPlanes", wt.WORD),
                    ("biBitCount", wt.WORD), ("biCompression", wt.DWORD), ("biSizeImage", wt.DWORD),
                    ("biXPelsPerMeter", wt.LONG), ("biYPelsPerMeter", wt.LONG), ("biClrUsed", wt.DWORD),
                    ("biClrImportant", wt.DWORD)]
    bih = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    g32.GetDIBits(mem, bmp, 0, h, buf, ctypes.byref(bih), 0)
    g32.DeleteObject(bmp)
    g32.DeleteDC(mem)
    u.ReleaseDC(0, hdc)
    Image.frombuffer("RGBA", (w, h), buf, "raw", "BGRA", 0, 1).convert("RGB").save(path)


class Watcher(threading.Thread):
    """Waits for the progress window; screenshots it at `shots` seconds after it appeared; optionally cancels it with
    the pad (Dev\\PadInject = Circle) at `cancel_at` seconds."""
    def __init__(self, prefix, shots=(1.0,), cancel_at=None):
        super().__init__(daemon=True)
        self.prefix, self.shots, self.cancel_at = prefix, shots, cancel_at
        self.appeared = self.closed = None
        self.files = []
        self.stop = False

    def run(self):
        t0 = time.time()
        hwnd = 0
        while not hwnd and time.time() - t0 < 120 and not self.stop:
            hwnd = find_progress()
            time.sleep(0.02)
        if not hwnd:
            return
        self.appeared = time.time()
        todo = sorted(self.shots)
        cancelled = False
        while u.IsWindow(hwnd) and time.time() - self.appeared < 300:   # (a build is ~5 s)
            el = time.time() - self.appeared
            if todo and el >= todo[0]:
                path = os.path.join(OUT, "%s_%.1fs.png" % (self.prefix, todo.pop(0)))
                try:
                    shot_window(hwnd, path)
                    self.files.append(path)
                except Exception as e:   # noqa: BLE001
                    print("[store] screenshot failed:", e)
            if self.cancel_at is not None and not cancelled and el >= self.cancel_at:
                bg._reg(bg.DEV_KEY, "PadInject", 32)   # Circle / B (gamepad::kBack)
                time.sleep(0.5)
                bg._reg(bg.DEV_KEY, "PadInject", 0)
                cancelled = True
            time.sleep(0.02)
        self.closed = time.time()
        bg._reg(bg.DEV_KEY, "PadInject", 0)


# ---- one game ----
def lines_of(c):
    return c.log(5000) or []


def since(lines, marker):
    idx = max((i for i, l in enumerate(lines) if marker in l), default=-1)
    return lines[idx + 1:] if idx >= 0 else lines


def memory(pid):
    class PMC(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t), ("PrivateUsage", ctypes.c_size_t)]
    h = k32.OpenProcess(0x0410, False, pid)   # QUERY_INFORMATION | VM_READ
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb)
    k32.CloseHandle(h)
    return {"working_set_mb": round(pmc.WorkingSetSize / 1048576, 1), "private_mb": round(pmc.PrivateUsage / 1048576, 1)}


class StoreGame:
    def __init__(self, on, rebuild=False, loadout=False):
        # loadout: FeatLoadout (the vehicle swap needs it; Mission 3's LoadoutTest needs it off, or the user's remembered
        # kits are applied when the loadout screen opens)
        self.on, self.rebuild, self.loadout = on, rebuild, loadout

    def __enter__(self):
        self.saved = {n: bg._reg(STORE_KEY, n) for n in ("SharedAssetStore", "RebuildCache")}
        bg._reg(STORE_KEY, "SharedAssetStore", 1 if self.on else 0)
        bg._reg(STORE_KEY, "RebuildCache", 1 if self.rebuild else 0)
        bg.ensure_settings_migrated()
        self.old_swap = bg._reg(bg.ENH_KEY, SWAP_NAME)
        bg._reg(bg.ENH_KEY, SWAP_NAME, SWAP, winreg.REG_SZ)
        self.g = bg.Game(dev={"StartLevel": "", "SplitPlayers": 0, "PseStore": 0, "PseNoBorrow": 0, "PseRebuild": 0,
                              "LoadoutTest": LOADOUT}, enh={"FeatLoadout": 1 if self.loadout else 0})
        self.t0 = time.time()
        self.g.__enter__()
        pid = pse_client.wait_for_pipe(self.g.p.pid, timeout=180)
        self.pid = pid
        self.c = pse_client.Client(pid)
        self.c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=180)
        self.menu_s = time.time() - self.t0
        return self

    def __exit__(self, *exc):
        try:
            self.c.close()
        except Exception:   # noqa: BLE001
            pass
        self.g.__exit__(None, None, None)
        for n, v in self.saved.items():
            bg._reg(STORE_KEY, n, delete=True) if v is None else bg._reg(STORE_KEY, n, v)
        if self.old_swap is None:
            bg._reg(bg.ENH_KEY, SWAP_NAME, delete=True)
        else:
            bg._reg(bg.ENH_KEY, SWAP_NAME, self.old_swap, winreg.REG_SZ)

    def level(self, name):
        t = time.time()
        p = self.c.start_level(name, timeout=300)
        return p, time.time() - t


SQUAD_LUA = """   -- unit name, held weapon definition name, soldier, weapon of every soldier
local mgr = dev.u32(0x63C9A0)
local tab = mgr ~= 0 and dev.u32(mgr + 0x30) or 0
local out = {}
if tab == 0 then return out end
for i = 0, 699 do
  pcall(function()
    local s = dev.u32(tab + i * 4)
    if s ~= 0 and dev.u32(s) == %d then
      local unit = dev.u32(s + 0x24)
      local w = dev.u32(s + 0x26B0)
      local def = w ~= 0 and dev.u32(w + 0x24) or 0
      local inv = {}   -- inventory array +0x26B8 (25 items): definition names of the weapons (+4 type 6)
      local arr = dev.u32(s + 0x26B8)
      if arr ~= 0 then
        for k = 0, 24 do
          local it = dev.u32(arr + k * 4)
          if it ~= 0 and dev.u32(it + 4) == 6 and dev.u32(it + 0x24) ~= 0 then inv[#inv + 1] = dev.cstring(dev.u32(it + 0x24) + 8, 64) end
        end
      end
      out[#out + 1] = {id = i, unit = unit ~= 0 and dev.cstring(unit + 8, 40) or "?", soldier = s, weapon = w,
                       def = (def ~= 0 and dev.cstring(def + 8, 64) or "") .. " / " .. table.concat(inv, " ")}
    end
  end)
end
return out
""" % SOLDIER_VTABLE

FIRE_LUA = """local w, s = %d, %d
local before = dev.u32(w + 0x80)
dev.call(0x4A1AD0, "thiscall", w, s, 0, 0, 0)   -- the trigger (FUN_004a1ad0)
return before
"""

BANK_LUA = """local mgr = dev.u32(0x63C9AC)
return dev.call(0x55E2B0, "thiscall", mgr, %d)   -- loaded bank by key (FUN_0055e2b0), 0 = not loaded
"""


def sound_errors(lines):
    return [l for l in lines if "not in the sound cache" in l]


def stamp(line):
    try:
        m, sec = line.split("]", 1)[0].strip("[ ").split(":")
        return int(m) * 60 + float(sec)
    except ValueError:
        return None


def init_seconds(lines, name):
    """Level init in the game's own log: "Console: level load <name>" -> "[game] DONE : levelInitialise" (no waits
    for keys / screens in it)."""
    start = max((i for i, l in enumerate(lines) if "Console: level load " + name in l), default=-1)
    if start < 0:
        return None
    for l in lines[start:]:
        if "DONE" in l and "levelInitialise" in l:
            a, b = stamp(lines[start]), stamp(l)
            return round(b - a, 2) if a is not None and b is not None else None
    return None


def close_loadout_screen(c, timeout=40):
    """The briefing / loadout screen (FeatLoadout) opens ~0.5 s into play: closed with dev.loadout_close() (posted keys
    don't reach it in background runs). Bounded wait."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        lines = since(lines_of(c), "Console: level load ")
        opened = [i for i, l in enumerate(lines) if "Customise: opened" in l]
        if opened:
            if any("Customise: closed" in l for l in lines[opened[-1]:]):
                time.sleep(1.0)
                return True
            c.run("return dev.loadout_close and dev.loadout_close()", timeout=10)
        time.sleep(0.5)
    return False


def play_levels(sg, tag, levels):
    """The levels one after the other; returns per-level results."""
    c = sg.c
    res = {}
    for name in levels:
        p, secs = sg.level(name)
        assert p and p["soldier"], name + " didn't reach play"
        time.sleep(3)
        if sg.loadout:
            print("[store] loadout screen closed:", close_loadout_screen(c))
        assert c.frames_advancing(1.0)
        lines = since(lines_of(c), "Console: level load " + name)
        r = {"load_s": round(secs, 2), "sound_errors": len(sound_errors(lines)),
             "store_files": len([l for l in lines if "Asset store:" in l and "(global fallback)" in l]),
             "game_errors": [l for l in lines if "[game Error]" in l][:5]}
        r.update(memory(sg.g.p.pid))
        r["init_s"] = init_seconds(lines_of(c), name)
        if name == "Mission3.dll":
            squad = c.run(SQUAD_LUA, timeout=60) or []
            if isinstance(squad, dict):
                squad = list(squad.values())
            print("[store] Mission 3 soldiers:", len(squad), sorted({x["unit"] for x in squad}))
            # [soldier+0x24]+8 is the body model name: Bradley = Hero01_Armstrong, Foley = Hero04_Foley; the
            # Iraqi_Militia (armoury guard) is the object with script id 218 (it keeps the Iraqi body)
            def role(x):
                return {"hero01_armstrong": "Bradley", "hero04_foley": "Foley"}.get(x["unit"].lower(),
                                                                                    "Iraqi_Militia" if x["id"] == 218 else None)
            pick = [x for x in squad if role(x)]
            r["squad"] = {}   # role -> held weapons (Mission 3 also has cutscene doubles of the heroes, ids 11-14)
            for x in pick:
                r["squad"].setdefault(role(x), []).append(x["def"])
            print("[store] Mission 3 held weapons:", r["squad"])
            for x in pick:
                if x["weapon"]:
                    try:
                        c.run(FIRE_LUA % (x["weapon"], x["soldier"]), timeout=20)
                    except Exception as e:   # noqa: BLE001
                        print("[store] fire", x["unit"], e)
            time.sleep(2)
            r["banks"] = {b: c.run(BANK_LUA % elf(b + ".sbk")) != 0 for b in ("SAW-LIGHTMG", "AKSU")}
            lines = since(lines_of(c), "Console: level load " + name)
            r["sound_errors"] = len(sound_errors(lines))
            r["loadout"] = [l.split("] ", 1)[-1] for l in lines if "Loadout: " in l and ("->" in l or "isn't" in l)]
        if name == "Mission9.dll":
            r["vehicles"] = [l.split("] ", 1)[-1] for l in lines if "Vehicles: IR_VEH_BMP-2 for Mission9" in l]
        r["sch"] = [l.split("] ", 1)[-1] for l in lines if ".sch" in l and ("asset store" in l or "sound bank" in l)][:4]
        res[name] = r
        print("[store %s] %s: %s" % (tag, name, json.dumps(r)))
        assert not r["game_errors"], r["game_errors"]
        assert c.frames_advancing(1.0)
    return res


@pytest.fixture(scope="module", autouse=True)
def setup_module_state():
    os.makedirs(OUT, exist_ok=True)
    had_cache = os.path.isdir(CACHE)
    had_mod = os.path.isdir(MOD)
    assert not had_mod, MOD + " exists already"
    make_mod()
    RESULTS["had_cache"] = had_cache
    try:
        yield
    finally:
        shutil.rmtree(MOD, ignore_errors=True)
        if not had_cache:
            for _ in range(10):
                shutil.rmtree(CACHE, ignore_errors=True)
                if not os.path.isdir(CACHE):
                    break
                time.sleep(0.5)
        bg._reg(bg.DEV_KEY, "PadInject", 0)
        json.dump(RESULTS, open(os.path.join(OUT, "results.json"), "w"), indent=1)
        print("[store] results in", OUT)


def test_first_boot_cancel_with_the_pad():
    w = Watcher("progress_cancel", shots=(0.6,), cancel_at=1.2)
    w.start()
    with StoreGame(True, rebuild=RESULTS["had_cache"]) as sg:
        w.stop = True
        w.join(5)
        lines = lines_of(sg.c)
        assert w.appeared, "no progress window"
        print("[store] window open %.1f s, shots %s" % ((w.closed or time.time()) - w.appeared, w.files))
        for l in lines:
            if "Asset store" in l:
                print("[store]  ", l)
        assert any("Asset store: progress window shown" in l for l in lines)
        assert any("build cancelled by the player" in l for l in lines), "the pad didn't cancel"
        assert any("off for this run" in l for l in lines)
        assert not any("Asset store: ON" in l for l in lines)
        assert not os.path.isfile(os.path.join(CACHE, "assets", "stamp.txt")), "a cancelled build left a stamp"
        RESULTS["cancel"] = {"window_s": round((w.closed or time.time()) - w.appeared, 2), "shots": w.files,
                             "menu_s": round(sg.menu_s, 1)}
        report = os.path.join(bg.GAME, "Mods", "DesertStormFix-report.txt")
        if os.path.isfile(report):
            assert "first-start build was cancelled" in open(report, errors="replace").read()


def test_first_boot_builds_then_levels_play_from_the_store():
    w = Watcher("progress_build", shots=(0.5, 2.0, 3.5))
    w.start()
    with StoreGame(True) as sg:
        w.stop = True
        w.join(5)
        c = sg.c
        lines = lines_of(c)
        for l in lines:
            if "Asset store" in l and ("build" in l or "ON" in l or "open" in l or "asset store:" in l):
                print("[store]  ", l)
        assert w.appeared and w.closed, "no progress window"
        built = [l for l in lines if "first-start build finished in" in l]
        assert built, "no build"
        on = [l for l in lines if "Asset store: ON" in l]
        assert on and "global fallback" in on[-1] and "Borrow* copies off" in on[-1] and "sound caches" in on[-1]
        size = sum(os.path.getsize(os.path.join(r, f)) for r, _, fs in os.walk(os.path.join(CACHE, "assets")) for f in fs)
        RESULTS["build"] = {"window_s": round(w.closed - w.appeared, 2), "log": built[-1].split("] ", 1)[-1],
                            "cache_mb": round(size / 1048576, 1), "shots": w.files, "menu_s": round(sg.menu_s, 1),
                            "menu_memory": memory(sg.g.p.pid)}
        kit = [l for l in lines if "kit weapon sound banks" in l]
        print("[store]", kit[-1] if kit else "(no kit bank line)")
        assert kit and "(asset store)" in kit[-1]
        res = play_levels(sg, "on", ["Mission1.dll", "Mission3.dll"])
        assert any("for mission3.dat from" in l and "AKSU" in l.upper() for l in lines_of(c)),             "the AKSU didn't come from the store"
    with StoreGame(True, loadout=True) as sg:   # the vehicle swap needs FeatLoadout; the store is up to date now
        c = sg.c
        assert any("Asset store: ON" in l for l in lines_of(c))
        assert not any("first-start build finished" in l for l in lines_of(c)), "built again although the stamp matched"
        res.update(play_levels(sg, "on", ["Mission9.dll"]))
        RESULTS["on"] = res
        m3 = res["Mission3.dll"]
        assert any("AKSU" in d.upper() for d in m3["squad"].get("Foley", [])), m3["squad"]
        assert any("SAW" in d.upper() for d in m3["squad"].get("Bradley", [])), m3["squad"]
        assert any("AKSU" in d.upper() for d in m3["squad"].get("Iraqi_Militia", [])), m3["squad"]
        assert all(m3["banks"].values()), m3["banks"]
        for name, r in res.items():
            assert r["sound_errors"] == 0, (name, r["sound_errors"])
        lines = lines_of(c)
        assert any("from the store so far" in l or "for mission9.dat from" in l for l in lines)
        m9 = res["Mission9.dll"]["vehicles"]
        assert m9 and " 0 file(s) copied" in m9[-1] and "asset store" in m9[-1], m9
        assert any("BMP2.EVO (" in l.upper() and "for mission9.dat from" in l for l in lines)
        merged = [l for l in lines if "level cache from the asset store" in l]
        print("[store] level caches from the store:", len(merged))
        assert len(merged) >= 2   # (this game: front end + Mission 9)


def test_store_off_for_comparison():
    with StoreGame(False) as sg:
        lines = lines_of(sg.c)
        assert any("Asset store: off" in l for l in lines)
        RESULTS["off_menu"] = {"menu_s": round(sg.menu_s, 1), "menu_memory": memory(sg.g.p.pid)}
        res = play_levels(sg, "off", ["Mission1.dll", "Mission3.dll"])
    with StoreGame(False, loadout=True) as sg:
        res.update(play_levels(sg, "off", ["Mission9.dll"]))
        RESULTS["off"] = res
        assert " 0 file(s) copied" not in (res["Mission9.dll"]["vehicles"] or ["x"])[-1]
        for name in LEVELS:
            on = RESULTS.get("on", {}).get(name, {})
            print("[store] %s: to play store on %s s / off %s s; level init store on %s s / off %s s"
                  % (name, on.get("load_s"), res[name]["load_s"], on.get("init_s"), res[name]["init_s"]))
