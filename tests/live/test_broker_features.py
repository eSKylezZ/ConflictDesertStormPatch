"""Features whose patches moved onto the hooks broker (pointer slots, entry detours, call / jmp code groups - 2026-10-19)
still work in the running game: the broker records are there with the right owners and kinds, the hooked code runs
(menu text line height, frame timer, string lookup), and a 2-player split-screen mission plays (panel callback push,
menu-open entry hooks, analog input replacement, panel walk call)."""
import os, time, winreg

import pytest

DEV_KEY = r"Software\DesertStormFix\Dev"
BIG_FONT, FONT_SHEET = 0x60EDB8, 0x60EDB0


def _hooks(c):
    return c.run("return dev.pse_hooks()")


def _at(hooks, va, kind=None):
    return [h for h in hooks if h["va"] == va and (kind is None or h["kind"] == kind)]


# Hooks every start-up installs whatever the settings say: (va, kind, owner).
ALWAYS = [
    (0x4BA5B0, "function", "dsfix.timing"),        # frame timer (was a hand trampoline)
    (0x4F6730, "function", "dsfix.ingame_input"),  # string lookup replacement (was a jmp)
    (0x40F3A5, "bytes", "dsfix.ingame_input"),     # call@ cursor flag (was a hand-written call)
    (0x480930, "function", "dsfix.coop"),          # front-end state -> screen
    (0x5D86C4, "pointer", "dsfix.coop"),           # main menu set-up vtable slot
    (0x5D93DC, "pointer", "dsfix.coop"),           # SP menu set-up vtable slot
    (0x449A90, "pointer", "dsfix.coop"),           # save list callback push imm32
    (0x5D91B4, "pointer", "dsfix.fixmenu"),        # Options create vtable slot
    (0x5D8308, "pointer", "dsfix.graphics"),       # Direct3DCreate8 IAT slot
    (0x4C0420, "function", "dsfix.memory"),        # heap alloc (was a hand trampoline)
    (0x4BFB90, "function", "dsfix.memory"),        # whole-file read
    (0x4B98D0, "function", "dsfix.diagnostics"),   # error box replacement
    (0x450920, "function", "dsfix.splitscreen"),   # analog input (AxisValue)
    (0x4515F0, "function", "dsfix.splitscreen"),   # inventory open (was a 7-byte hand trampoline)
    (0x48D79B, "pointer", "dsfix.splitscreen"),    # soldier panel callback push imm32
    (0x48D6D5, "bytes", "dsfix.splitscreen"),      # call@ panel walk end
    (0x44793D, "pointer", "dsfix.launcher"),       # launcher dialog proc push imm32
    (0x4112B0, "function", "dsfix.display"),       # ClipCursor replacement
    (0x456040, "function", "dsfix.textfit"),       # choice row draw (was a hand trampoline)
    (0x5230D0, "function", "dsfix.shadows"),       # object shadow volume builder
    (0x5D97D0, "pointer", "dsfix.menus"),          # YES / NO screen enter vtable slot
    (0x5D9214, "pointer", "dsfix.pausemenu"),      # pause menu set-up vtable slot
    (0x486FDE, "bytes", "dsfix.overview"),         # call@ MovieEnd message 0x12
]


def test_converted_hooks_are_broker_records(c):
    hooks = _hooks(c)
    missing = []
    for va, kind, owner in ALWAYS:
        found = _at(hooks, va, kind)
        if not found:
            missing.append("0x%08X %s %s: %r" % (va, kind, owner, _at(hooks, va)))
            continue
        assert owner in [h["owner"] for h in found], (hex(va), kind, found)
    assert not missing, missing
    kinds = {}
    for h in hooks:
        kinds[h["kind"]] = kinds.get(h["kind"], 0) + 1
    print("\n[live] broker records by kind: %s" % ", ".join("%s %d" % kv for kv in sorted(kinds.items())))
    assert kinds.get("pointer", 0) >= 15 and kinds.get("function", 0) >= 20
    # Settings-dependent ones: on when the setting is.
    if c.run("return dev.setting('Controller')"):
        group = [h for h in _at(hooks, 0x538F2A, "bytes") if h["name"] == "call@0x00538F2A"]
        assert group and group[0]["owner"] == "dsfix.controller", _at(hooks, 0x538F2A)
        assert c.read(0x538F2A, "u8") == 0xE8 and c.read(0x538F2F, "u8") == 0x90
    if c.run("return dev.setting('HudScaling')"):
        assert [h for h in _at(hooks, 0x4570D0, "function") if h["owner"] == "dsfix.hud"]


def test_overlay_and_probe_share_the_image_draw_entry(c):
    """0x53C9E0: the overlay's icon-glyph detour (priority 100) - one function hook site, the first in its chain."""
    chain = sorted(_at(_hooks(c), 0x53C9E0, "function"), key=lambda h: h["chain"])
    print("\n[live] 0x53C9E0 chain: %s" % " -> ".join("%s(%d)" % (h["owner"], h["priority"]) for h in chain))
    assert chain and chain[0]["owner"] == "dsfix.overlay" and chain[0]["priority"] == 100
    assert c.read(0x53C9E0, "u8") == 0xE9


def test_menu_text_line_height_hook_runs(c):
    """FUN_004570d0 is detoured to ScaledFontLineHeight with HUD scaling on: 19 x the font sheet's Y scale."""
    if not c.run("return dev.setting('HudScaling')"):
        pytest.skip("HUD scaling is off")
    got, want = c.run("""
        local font = dev.u32(%d)
        local sheet = dev.u32(%d)
        local h = 19.0
        if sheet ~= 0 and (dev.u32(sheet + 0x20) & 1) == 1 then h = h * dev.f32(sheet + 0x28) end
        return {dev.callf(0x4570D0, "stdcall", font), h}
    """ % (BIG_FONT, FONT_SHEET))
    print("\n[live] menu line height %.3f (expected %.3f)" % (got, want))
    assert abs(got - want) < 1e-3


def test_string_lookup_replacement_answers(c):
    """FUN_004f6730 (thiscall catalog, hash) is replaced by Lookup: a known menu text comes back."""
    text = c.run("""
        local p = dev.call(0x4F6730, "thiscall", dev.u32(0x60EDCC), 0xA08F3D1B)
        if p == 0 then return nil end
        return dev.cstring(p, 64)
    """)
    print("\n[live] lookup 0xA08F3D1B -> %r" % text)
    assert isinstance(text, str) and text.strip()


def test_no_raw_hook_writes_in_log(c):
    """No call / jump / pointer / function hook went outside the broker ([warn] ... outside the broker)."""
    lines = c.run("return dev.log(20000)", async_=True)
    rows = (lines if isinstance(lines, list) else str(lines).splitlines())
    raw = [l for l in rows if "[warn]" in l and "outside the broker" in l]
    for l in raw:
        print("    " + l)
    assert not raw, raw
    fails = [l for l in rows if "[fail]" in l and ("function hook" in l or "pointer hook" in l or
                                                  "] jump at" in l or "] call at" in l)]
    assert not fails, fails


def _set_split(players):
    k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, DEV_KEY)
    try:
        old = winreg.QueryValueEx(k, "SplitPlayers")[0]
    except OSError:
        old = None
    winreg.SetValueEx(k, "SplitPlayers", 0, winreg.REG_DWORD, players)
    winreg.CloseKey(k)
    return old


def _restore_split(old):
    k = winreg.CreateKey(winreg.HKEY_CURRENT_USER, DEV_KEY)
    if old is None:
        try: winreg.DeleteValue(k, "SplitPlayers")
        except OSError: pass
    else:
        winreg.SetValueEx(k, "SplitPlayers", 0, winreg.REG_DWORD, old)
    winreg.CloseKey(k)


def test_split_screen_two_players_mission(c):
    """Dev\\SplitPlayers = 2 + console "level Mission1.dll": two views, two players with soldiers, frames running,
    frames still running after the inventory key."""
    old = _set_split(2)
    try:
        t0 = time.time()
        c.console("level Mission1.dll")
        p = c.wait_for_play(timeout=180)
        # The briefing / loadout screen (customise.cpp) shows one full view while it is up: close it (Enter).
        renderer = c.read(0x63C924)
        views = 0
        t1 = time.time()
        while time.time() - t1 < 30:
            views = c.read(renderer + 0x20) if renderer else 0
            if views == 2: break
            # Posted Enter doesn't always reach the screen in background runs: close it directly (dev server).
            if not c.run("return dev.loadout_close()"): c.press("enter")
            time.sleep(1.0)
        st = c.state()
        flag, count = c.read(0x606410), c.read(0x606414)
        p2 = c.player(1)
        print("\n[live] split Mission 1 after %.1f s: views %d, flag %d, players %d, p1 %r, p2 %r" %
              (time.time() - t0, views, flag, count, p, p2))
        assert st["level"].lower().startswith("mission1")
        assert flag == 1 and count == 2 and views == 2
        assert p["soldier"] and p2 and p2["soldier"] and p2["soldier"] != p["soldier"]
        assert c.frames_advancing(2.0)
        # Player 1's inventory key (Shift) -> FUN_004515f0, an entry detour now. Posted keys don't reach the game's
        # DirectInput keyboard in background runs, so whether it opened is printed, not asserted; the frames must run.
        block1 = 0x60F5B8
        before = c.read(block1 + 0x40C, "u8")
        c.press("shift", 150)
        time.sleep(0.5)
        opened = c.read(block1 + 0x40C, "u8")
        c.press("shift", 150)
        time.sleep(0.5)
        print("[live] inventory flag %d -> %d -> %d" % (before, opened, c.read(block1 + 0x40C, "u8")))
        assert c.frames_advancing(1.0), "game stopped after the inventory open / close"
    finally:
        _restore_split(old)
    # Back to the front end (one player) for the later tests, which start there.
    c.console("level FrontEnd.dll")
    t0 = time.time()
    while time.time() - t0 < 120:
        st = c.state()
        if st["fe_state"] == 1 and st["dispatch"] == 0: break
        if not c.frames_advancing(1.0): c.press("space")
        time.sleep(0.5)
    assert c.state()["fe_state"] == 1
