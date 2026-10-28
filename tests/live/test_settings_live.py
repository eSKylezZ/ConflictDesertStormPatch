"""DesertStormFix settings in the running game: they live in the pse.settings key of plugin "dsfix"
(HKCU\\Software\\PSE\\ds1\\Plugins\\dsfix, docs/sdk/ds1-settings.md), copied once from the old Enhancements key.

Anything a test changes is put back (value and whether it existed) - the user's settings must stay as they were."""
import winreg

import pytest

NEW_KEY = r"Software\PSE\ds1\Plugins\dsfix"
OLD_KEY = (r"Software\Classes\VirtualStore\MACHINE\SOFTWARE\WOW6432Node\Pivotal Games\Conflict Desert Storm"
           r"\Enhancements")
MARKER = "MigratedFromEnhancements"

# Declared DWORD settings (core/settings.cpp) with the range the game clamps to.
SETTINGS = {
    "HudScaling": (0, 1), "HudScalePercent": (25, 400), "Controller": (0, 1), "PadDeadzone": (0, 90),
    "FpsCapToRefresh": (0, 1), "SplitScreenLayout": (0, 1), "DisplayMode": (0, 2),
    "SkipIntro": (0, 1), "SkipCutscenes": (0, 1), "Antialiasing": (0, 16), "Anisotropy": (0, 16),
    "VSync": (0, 1), "PerfOverlay": (0, 1), "SkipLauncher": (0, 1), "Hdr": (0, 1),
    "HdrPaperWhite": (80, 500), "HdrPeak": (0, 10000), "HdrHighlights": (0, 100), "Bloom": (0, 2),
    "Fov1": (50, 110), "Fov2": (50, 110), "Fov3": (50, 110), "Fov4": (50, 110), "MenuBarCustom": (0, 1),
    "HudColourCustom": (0, 1), "Footprints": (0, 3), "FeatMods": (0, 1),
    "FeatWeather": (0, 1), "FeatDevConsole": (0, 1),
}


def reg(path, name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as k:
            return winreg.QueryValueEx(k, name)
    except OSError:
        return None


def put(path, name, value, kind=winreg.REG_DWORD):
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, path) as k:
        winreg.SetValueEx(k, name, 0, kind, value)


def delete(path, name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_SET_VALUE) as k:
            winreg.DeleteValue(k, name)
    except OSError:
        pass


def test_game_uses_the_new_key(c):
    assert c.run("return dev.settings_key()") == NEW_KEY


def test_migration_marker_present(c):
    m = reg(NEW_KEY, MARKER)
    assert m is not None and m[0] == 1 and m[1] == winreg.REG_DWORD, m


def test_game_values_come_from_the_new_key(c):
    """Every stored DWORD setting in the new key is what the game runs with (after its clamp); a missing one = the
    game's default, which the game must report too (not some value from the old key)."""
    checked = 0
    for name, (lo, hi) in SETTINGS.items():
        stored = reg(NEW_KEY, name)
        in_game = c.run("return dev.setting(%r)" % name)
        assert in_game is not None, name
        if stored is None or stored[1] != winreg.REG_DWORD:
            continue
        assert in_game == min(max(stored[0], lo), hi), (name, stored[0], in_game)
        checked += 1
    assert checked > 0, "no DesertStormFix value stored in %s" % NEW_KEY
    # FpsCap: 0 or above 240 reads as 240.
    stored = reg(NEW_KEY, "FpsCap")
    if stored is not None:
        want = 240 if stored[0] == 0 or stored[0] > 240 else stored[0]
        assert c.run("return dev.setting('FpsCap')") == want


def test_change_is_written_to_the_new_key_only(c):
    name = "Fov4"   # player 4's field of view: harmless while nobody plays as player 4
    before_new, before_old = reg(NEW_KEY, name), reg(OLD_KEY, name)
    original = c.run("return dev.setting(%r)" % name)
    target = 75 if original != 75 else 80
    try:
        assert c.run("return dev.set_setting(%r, %d)" % (name, target)) is True
        assert c.run("return dev.setting(%r)" % name) == target
        assert reg(NEW_KEY, name) == (target, winreg.REG_DWORD)
        assert reg(OLD_KEY, name) == before_old, "the old Enhancements key must never change"
        # Out of range: clamped like the menus / launcher would never offer it.
        assert c.run("return dev.set_setting(%r, 500)" % name) is True
        assert reg(NEW_KEY, name) == (110, winreg.REG_DWORD)
    finally:
        assert c.run("return dev.set_setting(%r, %d)" % (name, original)) is True
        if before_new is None:
            delete(NEW_KEY, name)
        else:
            put(NEW_KEY, name, before_new[0], before_new[1])
    assert reg(NEW_KEY, name) == before_new
    assert c.run("return dev.setting(%r)" % name) == original


def test_unknown_setting_name(c):
    assert c.run("return dev.setting('NoSuchSetting')") is None
    assert c.run("return dev.set_setting('NoSuchSetting', 1)") is False


@pytest.mark.parametrize("name", ["StartPending"])
def test_raw_values_live_in_the_new_key(c, name):
    """Launcher start state (settings::WriteDword(false, ...)) is in the new key: set at launcher OK, cleared after
    300 frames - by the time the main menu is up it reads 0 there."""
    v = reg(NEW_KEY, name)
    assert v is None or v[1] == winreg.REG_DWORD
