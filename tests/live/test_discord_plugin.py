"""dsfix.discord as an EXTERNAL plugin (<game>\\PSE\\Plugins\\dsfix.discord.dll + .pse, plugins/discord/): loaded from
the folder, its own settings key, its snapshot / send loop in the log. Discord may or may not run on the test PC -
with Discord the SET_ACTIVITY reply must carry the mission title, without it the plugin just keeps retrying.

The user's DiscordPresence value is put back as it was."""
import os, time, winreg

import pytest

from test_game_interface import ensure_mission1

KEY = r"Software\PSE\ds1\Plugins\dsfix.discord"
INTERNAL, CLIENT = 0x80000000, 0x2


def plugins(c):
    return {p["name"]: p for p in c.run("return dev.pse_plugins()")}


@pytest.fixture
def discord(c):
    p = plugins(c).get("dsfix.discord")
    if p is None:
        pytest.skip("dsfix.discord not installed in <game>\\PSE\\Plugins (deploy the build's pse_plugins first)")
    return p


def reg(name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as k:
            return winreg.QueryValueEx(k, name)[0]
    except OSError:
        return None


def log_text(c, n=4000):
    lines = c.log(n)
    return "\n".join(lines) if isinstance(lines, list) else str(lines)


def discord_running():
    try:
        return any(n.startswith("discord-ipc-") for n in os.listdir("\\\\.\\pipe\\"))
    except OSError:
        return False


def wait_log(c, text, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if text in log_text(c): return True
        time.sleep(0.5)
    return False


def test_loaded_as_external_plugin(c, discord):
    assert discord["version"] == "1.0.0"
    assert not discord["flags"] & INTERNAL, hex(discord["flags"])
    assert discord["flags"] & CLIENT
    assert "pse.game.ds1" in plugins(c)
    text = log_text(c)
    assert "[dsfix.discord] INFO Discord: loaded" in text
    assert "exe known build" in text


def test_setting_in_its_own_key(c, discord):
    assert reg("ImportedFromDsfix") == 1, "one-time import marker missing"
    v = c.run('return dev.plugin_setting("dsfix.discord", "DiscordPresence")')
    assert v in (0, 1)
    stored = reg("DiscordPresence")
    assert stored is None or stored == v   # nothing stored = the default (1)
    if stored is None: assert v == 1
    # No longer a DesertStormFix core setting.
    assert c.run('return dev.setting("DiscordPresence")') is None


def test_snapshot_and_send_loop(c, discord):
    if c.run('return dev.plugin_setting("dsfix.discord", "DiscordPresence")') != 1:
        pytest.skip("Discord status switched off by the user")
    ensure_mission1(c)
    title = c.run("return dev.game_ds1()")["title"]
    assert title.startswith("Mission 1: ")
    level = c.run("return dev.level()")
    assert wait_log(c, 'Discord: level %s -> "%s"' % (level, title), 10), "no snapshot line for Mission 1"
    if discord_running():
        # The old sender: connected, then a SET_ACTIVITY echo with the mission title (at most every 4 s).
        assert wait_log(c, "Discord: connected", 15)
        assert wait_log(c, '"details":"%s"' % title.replace("\\", "\\\\").replace('"', '\\"'), 20), \
            "no SET_ACTIVITY reply with the title"
        print("\n[live] Discord is running: status %r sent" % title)
    else:
        assert "Discord: connected" not in log_text(c)
        print("\n[live] Discord not running: no connection (the plugin retries every 10 s)")


def test_live_toggle(c, discord):
    old = c.run('return dev.plugin_setting("dsfix.discord", "DiscordPresence")')
    stored = reg("DiscordPresence")
    try:
        assert c.run('return dev.set_plugin_setting("dsfix.discord", "DiscordPresence", %d)' % (1 - old))
        assert reg("DiscordPresence") == 1 - old
        assert wait_log(c, "Discord: status %s" % ("off" if old else "on"), 5)
        assert c.frames_advancing(1.0)
    finally:
        c.run('return dev.set_plugin_setting("dsfix.discord", "DiscordPresence", %d)' % old)
        if stored is None:   # it wasn't stored before: leave no value behind
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY, 0, winreg.KEY_SET_VALUE) as k:
                    winreg.DeleteValue(k, "DiscordPresence")
            except OSError:
                pass
    assert c.run('return dev.plugin_setting("dsfix.discord", "DiscordPresence")') == old
