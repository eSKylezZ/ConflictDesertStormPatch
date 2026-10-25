"""DesertStormFix features split out of dsfix.dll into their own plugin DLLs on the dsfix kit (games/ds1/plugins/kit,
2026-10-23): each loads from <game>\\PSE\\Plugins as an external plugin, owns its hooks in the broker, keeps its
setting (if any) in its own pse.settings key (imported once from dsfix's) and follows it live. dsfix.limits publishes
interface dsfix.limits, which dsfix.memory uses for the Global Heap size.

The user's settings are put back as they were."""
import time, winreg

import pytest

from test_game_interface import ensure_mission1

INTERNAL, SIM, CLIENT = 0x80000000, 0x1, 0x2
SPLIT = {
    # plugin: (kind flag, setting or None, hooks it must own: (va, kind))
    "dsfix.shadows": (CLIENT, "FeatShadowFix", [(0x5230D0, "function"), (0x523440, "function"), (0x503333, "call_site")]),
    "dsfix.audio": (CLIENT, "FeatAimAudio", [(0x40EEFA, "call_site")]),
    "dsfix.widescreen": (CLIENT, None, [(0x4488B3, "bytes")]),
    "dsfix.textfit": (CLIENT, None, [(0x456040, "function"), (0x456A70, "function")]),
    "dsfix.mouse": (CLIENT, "MouseAcceleration", [(0x415F44, "bytes"), (0x45C862, "bytes")]),
    "dsfix.menus": (CLIENT, None, [(0x5D97D0, "pointer")]),
    "dsfix.units": (SIM, None, [(0x42220F, "bytes")]),
    "dsfix.limits": (SIM, None, [(0x4FE4C0, "function"), (0x4104A8, "call_site"), (0x4156AB, "call_site")]),
    # 2026-10-24, on interface dsfix.services (core/dsfix_services.h)
    "dsfix.difficulty": (SIM, "FeatVeryHard", [(0x4F0080, "function"), (0x459190, "function"), (0x4341A2, "call_site")]),
    "dsfix.jpfixes": (SIM, "FeatJpFixes", [(0x50CCAA, "call_site")]),
    "dsfix.handling": (SIM, None, [(0x514514, "call_site")]),
    "dsfix.tooltips": (CLIENT, "FeatTooltips", [(0x4545EB, "call_site")]),
    "dsfix.rumble": (CLIENT, "FeatRumble", [(0x4A1E38, "call_site")]),
    "dsfix.bodies": (SIM, None, [(0x503384, "bytes")]),
    "dsfix.followers": (SIM, "FeatFollowers", [(0x44B6C6, "call_site")]),
    "dsfix.hud": (CLIENT, None, [(0x4570D0, "function"), (0x507B42, "call_site")]),
    # weather + weather sound + footprints in one DLL; publishes dsfix.weather (plugins/weather/dsfix_weather.h)
    "dsfix.weather": (SIM, None, [(0x40E1EE, "call_site"), (0x40FC02, "call_site"), (0x424E7C, "call_site")]),
    # the script API's time slots SetTOD / GetTOD / SetSpeed / RunUntil; publishes dsfix.timeofday
    "dsfix.timeofday": (SIM, None, [(0x5ED2B0, "pointer"), (0x5ED2B4, "pointer"), (0x5ED2B8, "pointer"),
                                    (0x5ED2C8, "pointer")]),
    # dev builds: the sound play counter (the voices themselves are a per-frame bank switch); publishes dsfix.voices
    "dsfix.voices": (SIM, None, [(0x55DE20, "function")]),
    # last subscriber of the shared glyph bind 0x53CC9B (dsfix.dll's barcolour / weaponicons are on it too: see
    # test_sharp_text_follows_its_settings and test_pse.py test_glyph_bind_is_a_shared_chain)
    "dsfix.fontsharp": (CLIENT, "FeatSharpText", []),
    # the two movie-cancel queries' flag reads; when to skip is dsfix.dll's (services MissionCutsceneSkip)
    "dsfix.cinematics": (SIM, None, [(0x48676E, "call_site"), (0x4867ED, "call_site")]),
    # ClipCursor replacement; publishes dsfix.display (the live mode change in OPTIONS > VIDEO)
    "dsfix.display": (CLIENT, None, [(0x4112B0, "function")]),
    # online co-op group (2026-10-25): own view's per-view draw / after-loop / listener calls (nameplates and the
    # lockstep / authority hooks come and go with the setting / a session); publishes dsfix.online
    "dsfix.online": (SIM, None, [(0x40F0CB, "call_site"), (0x40F16F, "call_site"), (0x40EF60, "call_site")]),
    # mission scripts (2026-10-25): the text-manifest level load (.lua levels) + the level archive open (--@archive)
    "dsfix.missions": (SIM, None, [(0x40C405, "call_site"), (0x40C19C, "call_site")]),
    # briefing / loadout group (2026-10-25): MovieEnd's message 0x12 (the overview); the skin apply entry is
    # test_skin_apply_chain's (off with the Mods folder and BRIEFING & LOADOUT both off)
    "dsfix.loadout": (SIM, None, [(0x486FDE, "bytes")]),
    # split screen / co-op / controls / pads / versus (2026-10-25): the analog input (the pad poll's fix-up only with
    # the Controller setting: test_broker_features)
    "dsfix.coop": (SIM, None, [(0x450920, "function")]),
    # graphics + renderers + OPTIONS > VIDEO (2026-10-25): the Direct3DCreate8 import slot (the device hooks follow at
    # run time: test_d3d_hooks)
    "dsfix.graphics": (CLIENT, None, [(0x5D8308, "pointer")]),
    # frame timing (2026-10-25): the frame timer, the mover's timescale call (landing fix), the Karma world step
    "dsfix.timing": (SIM, None, [(0x4BA5B0, "function"), (0x50E177, "call_site"), (0x4FE61C, "call_site")]),
    # menu bar / HUD colour (2026-10-25): a stretched image draw's texture bind, the menu item text colour (the shared
    # glyph bind 0x53CC9B chain: test_pse.py test_glyph_bind_is_a_shared_chain)
    "dsfix.barcolour": (CLIENT, None, [(0x53D868, "call_site"), (0x456BAA, "call_site")]),
}
ENVIRONMENT, ATMOSPHERE, HOUR = 0x63C948, 0x1A58, 0x3C   # [[0x63C948]+0x1A58]+0x3C = the atmosphere's hour
TRANSPARENTS = 0x40E356       # chained: nameplates (dsfix.online, priority 50) + dsfix.weather (100)
HUD_SCALE = 0x5D7B00          # hud: the cave's HudScale float = HudScalePercent / 100 (dsfix's own setting)
SERVICES = {"dsfix.difficulty", "dsfix.jpfixes", "dsfix.handling", "dsfix.tooltips", "dsfix.rumble", "dsfix.bodies",
            "dsfix.followers", "dsfix.hud", "dsfix.weather", "dsfix.timeofday", "dsfix.voices", "dsfix.fontsharp",
            "dsfix.cinematics", "dsfix.display"}
SKIN_APPLY = 0x435BE0         # the game's skin apply: dsfix.loadout's mid-level skins + dsfix.bodies chain on its entry
TEXT_VERY_HARD, TEXT_VERY_EASY = 0xC0110001, 0xC0110002
DROP_JUMP = 0x431EC0          # jpfixes: item drop jp rel8 (0x56 the game's, 0x1A safe drop)
RUMBLE_TABLE = 0x5EB540       # rumble: effect row 0 (strong i16): the game's 0, ours 20000
SHOT_SITE = 0x4A1D71   # AUDIO byte group: je -> jmp (own shots stay positional)
MOUSE_DEFAULT_IMM = 0x44AAAC   # block set-up: mov [esi+0x448], <acceleration>
HEAP = 0x63C8B4                # Global Heap object: +0x28 size


def plugins(c):
    return {p["name"]: p for p in c.run("return dev.pse_plugins()")}


def reg(plugin, name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\PSE\ds1\Plugins" + "\\" + plugin) as k:
            return winreg.QueryValueEx(k, name)[0]
    except OSError:
        return None


def log_text(c, n=6000):
    lines = c.log(n)
    return "\n".join(lines) if isinstance(lines, list) else str(lines)


def wait_log(c, text, timeout):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if text in log_text(c): return True
        time.sleep(0.3)
    return False


@pytest.fixture
def split(c):
    ps = plugins(c)
    missing = [n for n in SPLIT if n not in ps]
    if missing:
        pytest.skip("not installed in <game>\\PSE\\Plugins: %s (deploy the build tree)" % ", ".join(missing))
    return ps


def test_loaded_as_external_plugins(c, split):
    for name, (kind, _setting, _hooks) in SPLIT.items():
        p = split[name]
        assert p["external"] and not p["host"] and not p["disabled"], (name, p)
        assert not p["flags"] & INTERNAL and p["flags"] & kind, (name, hex(p["flags"]))
        assert p["version"] == "1.0.0"
    text = log_text(c, 20000)
    assert "[dsfix.shadows] INFO [ok]   Shadows: volumes end below the caster" in text or \
           "[dsfix.shadows] INFO [off]  Shadows" in text
    assert "[dsfix.audio] INFO [ok]   Aim audio" in text or "[dsfix.audio] INFO [off]  Aim audio" in text
    assert "[dsfix.limits] INFO Limits: raised (bodies 2048" in text
    assert "[dsfix.mouse] INFO [ok]   Mouse acceleration" in text
    bad = [l for l in text.splitlines() if any("[%s]" % n in l for n in SPLIT) and ("[fail]" in l or "[warn]" in l)]
    assert not bad, bad


def test_hooks_owned_by_the_plugins(c, split):
    hooks = c.run("return dev.pse_hooks()")
    for name, (_kind, _setting, wanted) in SPLIT.items():
        for va, kind in wanted:
            found = [h for h in hooks if h["va"] == va and h["kind"] == kind]
            assert found and name in [h["owner"] for h in found], (name, hex(va), kind, found)
    # nothing of them is left in dsfix.dll: no dsfix-owned record at their sites
    sites = {va for _k, _s, wanted in SPLIT.values() for va, _kind in wanted} | {SHOT_SITE}
    for h in hooks:
        if h["va"] in sites:
            # dev builds: dsfix.missions' API trace chains on every script API slot (priority 1000) - a tracer, not a leftover;
            # the game layer shares some sites (pse.game.ds1's Direct3DCreate8 hook: device events) - not dsfix.dll either
            assert h["owner"] in SPLIT or h["owner"] == "pse.game.ds1" or \
                (h["owner"] == "dsfix.missions" and h["kind"] == "pointer"), h


def test_limits_interface_sizes_the_heap(c, split):
    # dsfix.memory added dsfix.limits' HeapExtra (2.5 MB) to the game's 1 024 000-byte Global Heap (more with big mods)
    heap = c.read(HEAP, "u32")
    assert heap
    size = c.read(heap + 0x28, "u32")
    print("\n[live] Global Heap %.1f MB" % (size / 1048576.0))
    assert size >= 1024000 + 2560 * 1024, size


def test_settings_in_their_own_keys(c, split):
    for name, (_kind, setting, _hooks) in SPLIT.items():
        if not setting:
            continue
        assert reg(name, "ImportedFromDsfix") == 1, name
        v = c.run('return dev.plugin_setting("%s", "%s")' % (name, setting))
        assert v in (0, 1), (name, v)
        stored = reg(name, setting)
        assert stored is None or stored == v, (name, stored, v)
        assert c.run('return dev.setting("%s")' % setting) is None, "%s still a DesertStormFix core setting" % setting


def _toggle(c, name, setting, check):
    old = c.run('return dev.plugin_setting("%s", "%s")' % (name, setting))
    stored = reg(name, setting)
    try:
        for v in (1 - old, old):
            assert c.run('return dev.set_plugin_setting("%s", "%s", %d)' % (name, setting, v))
            assert reg(name, setting) == v
            check(v)
            assert c.frames_advancing(1.0)
    finally:
        c.run('return dev.set_plugin_setting("%s", "%s", %d)' % (name, setting, old))
        if stored is None:
            try:
                key = r"Software\PSE\ds1\Plugins" + "\\" + name
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_SET_VALUE) as k:
                    winreg.DeleteValue(k, setting)
            except OSError:
                pass
    assert c.run('return dev.plugin_setting("%s", "%s")' % (name, setting)) == old


def test_aim_audio_follows_its_setting(c, split):
    ensure_mission1(c)
    on_byte = None

    def check(v):
        nonlocal on_byte
        b = c.read(SHOT_SITE, "u8")
        if v:
            on_byte = b
            assert b == 0xEB, hex(b)            # jmp: the positional branch always
        else:
            assert b == 0x74, hex(b)            # the game's je back
        assert wait_log(c, "[dsfix.audio] INFO %s" % ("[ok]   Aim audio" if v else "[off]  Aim audio"), 5)

    _toggle(c, "dsfix.audio", "FeatAimAudio", check)


def test_mouse_acceleration_follows_its_setting(c, split):
    def check(v):
        assert c.read(MOUSE_DEFAULT_IMM, "u32") == v

    _toggle(c, "dsfix.mouse", "MouseAcceleration", check)


def test_services_interface(c, split):
    text = log_text(c, 20000)
    assert "[ok]   dsfix.services 1: registered" in text
    for name in SERVICES:
        assert "%s: no dsfix.services" % name not in text, name


def test_difficulty_texts_through_the_provider(c, split):
    # dsfix's text lookup asks dsfix.difficulty's provider first (the DIFFICULTY screen's two extra rows)
    assert c.run("return dev.game_text(%d)" % TEXT_VERY_HARD) == "VERY HARD"
    assert c.run("return dev.game_text(%d)" % TEXT_VERY_EASY) == "VERY EASY"


def test_very_hard_is_a_restart_setting(c, split):
    # declared without PSE_SETTING_LIVE (1): the DESERTSTORMFIX menu asks for a restart after a change; the rest are live
    flags = lambda p, k: c.run('return dev.plugin_setting_flags("%s", "%s")' % (p, k))
    assert flags("dsfix.difficulty", "FeatVeryHard") & 1 == 0
    for name, key in (("dsfix.jpfixes", "FeatJpFixes"), ("dsfix.rumble", "FeatRumble"), ("dsfix.tooltips", "FeatTooltips")):
        assert flags(name, key) & 1, (name, key)
    pending = lambda: c.run('return dev.plugin_restart_pending("dsfix.difficulty", "FeatVeryHard")')
    assert not pending()

    def check(v):
        changed = v != old
        assert pending() == changed, (v, old)

    old = c.run('return dev.plugin_setting("dsfix.difficulty", "FeatVeryHard")')
    _toggle(c, "dsfix.difficulty", "FeatVeryHard", check)
    assert not pending()


def test_jp_fixes_follow_their_setting(c, split):
    def check(v):
        t0 = time.time()   # the plugin's frame callback writes it
        while time.time() - t0 < 3 and c.read(DROP_JUMP, "u8") != (0x1A if v else 0x56):
            time.sleep(0.1)
        assert c.read(DROP_JUMP, "u8") == (0x1A if v else 0x56), hex(c.read(DROP_JUMP, "u8"))

    _toggle(c, "dsfix.jpfixes", "FeatJpFixes", check)


def test_rumble_follows_its_setting(c, split):
    # effect row 0 (AK rifles): ours 20000 strong, the game's 0 - rewritten when the setting changes
    def check(v):
        assert c.read(RUMBLE_TABLE, "u16") == (20000 if v else 0), c.read(RUMBLE_TABLE, "u16")

    _toggle(c, "dsfix.rumble", "FeatRumble", check)


def test_skin_apply_chain(c, split):
    # dsfix.loadout has the mid-level skin work (textures, saved skins on every side) the loadout screen needs; the mod
    # bodies chain on the same entry. Both only with the Mods folder (bodies) / it or BRIEFING & LOADOUT (skins) on.
    owners = [h["owner"] for h in c.run("return dev.pse_hooks()") if h["va"] == SKIN_APPLY]
    text = log_text(c, 20000)
    if "[off]  Skins:" not in text:
        assert "dsfix.loadout" in owners, owners
    if "[off]  Bodies: body swap off" not in text:
        assert "dsfix.bodies" in owners, owners
    assert set(owners) <= {"dsfix.loadout", "dsfix.bodies"}, owners


def test_followers_follow_their_setting(c, split):
    # live: the plugin logs [ok] / [off] the next time it is asked - its frame callback asks in plain play
    ensure_mission1(c)

    def check(v):
        assert wait_log(c, "[dsfix.followers] INFO %s" % ("[ok]   Followers" if v else "[off]  Followers"), 5)

    _toggle(c, "dsfix.followers", "FeatFollowers", check)


def test_hud_uses_dsfix_settings(c, split):
    # the plugin reads HudScaling / HudScalePercent (DesertStormFix's own settings) through dsfix.services
    if not c.run('return dev.setting("HudScaling")'):
        pytest.skip("HUD scaling is off in this install")
    percent = c.run('return dev.setting("HudScalePercent")')
    assert abs(c.read(HUD_SCALE, "f32") - percent / 100.0) < 1e-4, (c.read(HUD_SCALE, "f32"), percent)
    # the launcher's OK (OnSettingsChanged) re-applies it through the plugin's settings listener
    stored = reg("dsfix", "HudScalePercent")
    other = percent + 10 if percent <= 390 else percent - 10
    try:
        assert c.run('return dev.set_setting("HudScalePercent", %d)' % other)
        assert c.run("return dev.settings_changed()")
        assert abs(c.read(HUD_SCALE, "f32") - other / 100.0) < 1e-4, c.read(HUD_SCALE, "f32")
    finally:
        c.run('return dev.set_setting("HudScalePercent", %d)' % percent)
        c.run("return dev.settings_changed()")
        if stored is None:
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\PSE\ds1\Plugins\dsfix", 0, winreg.KEY_SET_VALUE) as k:
                    winreg.DeleteValue(k, "HudScalePercent")
            except OSError:
                pass
    assert abs(c.read(HUD_SCALE, "f32") - percent / 100.0) < 1e-4


def test_weather_interface_and_shared_draw_call(c, split):
    # dsfix.dll reaches the plugin through dsfix.weather (the loadout WEATHER row, Lua game.weather, online sync)
    w = c.run("return dev.weather()")
    assert w and 0 <= w["choice"] < 13 and w["label"], w
    # the call after each view's transparents is shared: nameplates (dsfix.online) and weather (this plugin) chain on it
    owners = {h["owner"] for h in c.run("return dev.pse_hooks()") if h["va"] == TRANSPARENTS}
    assert "dsfix.weather" in owners, owners
    if "[ok]   Nameplates" in log_text(c, 20000):
        assert "dsfix.online" in owners, owners


def test_weather_runs_in_a_mission(c, split):
    # RAIN (4) through the plugin's own remembered choice, then a fresh level start: the level's weather is rain and the
    # plugin's dev log reports drops drawn. Needs FeatWeather + FeatLoadout (DesertStormFix's own settings).
    if not (c.run('return dev.setting("FeatWeather")') and c.run('return dev.setting("FeatLoadout")')):
        pytest.skip("FeatWeather / FeatLoadout off in this install")
    old = c.run('return dev.plugin_setting("dsfix.weather", "Weather")')
    stored = reg("dsfix.weather", "Weather")
    try:
        assert c.run('return dev.set_plugin_setting("dsfix.weather", "Weather", 4)')
        c.start_level("Mission1.dll")
        assert c.wait_for_play()
        t0 = time.time()
        while time.time() - t0 < 20 and (c.run("return dev.weather()") or {}).get("kind") != 4:
            time.sleep(0.5)
        w = c.run("return dev.weather()")
        assert w and w["kind"] == 4 and w["label"] == "RAIN", w
        assert wait_log(c, "[dsfix.weather] INFO [dev] Weather:", 15), "no weather dev log from the plugin"
        assert c.frames_advancing(1.0)
    finally:
        c.run('return dev.set_plugin_setting("dsfix.weather", "Weather", %d)' % old)
        if stored is None:
            try:
                key = r"Software\PSE\ds1\Plugins\dsfix.weather"
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, key, 0, winreg.KEY_SET_VALUE) as k:
                    winreg.DeleteValue(k, "Weather")
            except OSError:
                pass


def test_time_of_day_rules_a_mission(c, split):
    # NIGHT (4) through the plugin's own remembered choice: Mission 1 (dawn by its script) runs at 23:00 once play
    # begins. Needs FeatTimeOfDay + FeatLoadout (DesertStormFix's own settings).
    if not (c.run('return dev.setting("FeatTimeOfDay")') and c.run('return dev.setting("FeatLoadout")')):
        pytest.skip("FeatTimeOfDay / FeatLoadout off in this install")
    old = (c.run('return dev.plugin_setting("dsfix.timeofday", "TimeOfDay")'),
           c.run('return dev.plugin_setting("dsfix.timeofday", "TimeFlow")'))
    stored = (reg("dsfix.timeofday", "TimeOfDay"), reg("dsfix.timeofday", "TimeFlow"))

    def hour():
        env = c.read(ENVIRONMENT, "u32")
        atm = c.read(env + ATMOSPHERE, "u32") if env else 0
        return c.read(atm + HOUR, "f32") if atm else None

    try:
        assert c.run('return dev.set_plugin_setting("dsfix.timeofday", "TimeOfDay", 4)')
        assert c.run('return dev.set_plugin_setting("dsfix.timeofday", "TimeFlow", 0)')
        c.start_level("Mission1.dll")
        assert c.wait_for_play()
        t0 = time.time()
        while time.time() - t0 < 20 and not (hour() is not None and abs(hour() - 23.0) < 0.2):
            time.sleep(0.5)
        h = hour()
        assert h is not None and abs(h - 23.0) < 0.2, h
        assert wait_log(c, "[dsfix.timeofday] INFO Time of day: NIGHT", 5)
    finally:
        for key, value, was in (("TimeOfDay", old[0], stored[0]), ("TimeFlow", old[1], stored[1])):
            c.run('return dev.set_plugin_setting("dsfix.timeofday", "%s", %d)' % (key, value))
            if was is None:
                try:
                    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\PSE\ds1\Plugins\dsfix.timeofday", 0,
                                        winreg.KEY_SET_VALUE) as k:
                        winreg.DeleteValue(k, key)
                except OSError:
                    pass


def test_voice_choice_switches_the_bank(c, split):
    # VOICE DELTA (2) for Bradley (role 0) in single player through the plugin's own setting: in Mission 1 he speaks
    # with the American copy of his bank (dsfix.dll's mods.cpp adds Bradley_US.sbk to the level's sound cache).
    if not c.run('return dev.setting("FeatVoices")'):
        pytest.skip("FeatVoices off in this install")
    old = c.run('return dev.plugin_setting("dsfix.voices", "VoiceSP0")')
    stored = reg("dsfix.voices", "VoiceSP0")
    try:
        assert c.run('return dev.set_plugin_setting("dsfix.voices", "VoiceSP0", 2)')
        c.start_level("Mission1.dll")
        assert c.wait_for_play()
        assert wait_log(c, "[dsfix.voices] INFO Voices: Bradley.sbk speaks with Bradley_US.sbk", 20)
    finally:
        c.run('return dev.set_plugin_setting("dsfix.voices", "VoiceSP0", %d)' % old)
        if stored is None:
            try:
                with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\PSE\ds1\Plugins\dsfix.voices", 0,
                                    winreg.KEY_SET_VALUE) as k:
                    winreg.DeleteValue(k, "VoiceSP0")
            except OSError:
                pass


def test_split_screen_weapon_name_fix_stays_in_dsfix(c, split):
    # 0x454558 jne -> nop nop (both name and fire mode with more than one player): dsfix.dll's splitscreen.cpp now
    assert c.read(0x454558, "u16") == 0x9090, hex(c.read(0x454558, "u16"))


def test_shadow_fix_follows_its_setting(c, split):
    ensure_mission1(c)

    def check(v):
        # the builders read the flag per call: the log line comes with the next shadow pass
        assert wait_log(c, "[dsfix.shadows] INFO %s" % ("[ok]   Shadows" if v else "[off]  Shadows"), 5)

    _toggle(c, "dsfix.shadows", "FeatShadowFix", check)


def test_sharp_text_follows_its_settings(c, split):
    # the plugin is the last subscriber (priority 0) of the image draw's texture bind
    chain = sorted((h for h in c.run("return dev.pse_hooks()") if h["va"] == 0x53CC9B), key=lambda h: h["chain"])
    assert chain and chain[-1]["owner"] == "dsfix.fontsharp" and chain[-1]["priority"] == 0, chain
    # FeatSharpText (its own key) and dsfix's HudScaling (launcher; re-read through SetSettingsListener) both count;
    # Enabled() logs each change the next time a glyph is drawn (the front end / mission draws text every frame)
    hud = c.run('return dev.setting("HudScaling")')

    def check(v):
        assert wait_log(c, "[dsfix.fontsharp] INFO %s" % ("[ok]   Sharp text" if v and hud else "[off]  Sharp text"), 5)
    _toggle(c, "dsfix.fontsharp", "FeatSharpText", check)
    if hud and c.run('return dev.plugin_setting("dsfix.fontsharp", "FeatSharpText")'):
        before = log_text(c, 20000).count("[dsfix.fontsharp] INFO [off]  Sharp text")
        try:
            assert c.run('return dev.set_setting("HudScaling", 0)')
            c.run("return dev.settings_changed()")
            t0 = time.time()
            while time.time() - t0 < 5 and log_text(c, 20000).count("[dsfix.fontsharp] INFO [off]  Sharp text") <= before:
                time.sleep(0.3)
            assert log_text(c, 20000).count("[dsfix.fontsharp] INFO [off]  Sharp text") > before
        finally:
            c.run('return dev.set_setting("HudScaling", %d)' % hud)
            c.run("return dev.settings_changed()")


def _main_menu(c):
    st = c.state()
    if st["fe_state"] == 1 and st["dispatch"] == 0 and not st["level"].lower().startswith("mission"):
        return
    c.console("level FrontEnd.dll")
    t0 = time.time()
    while time.time() - t0 < 120:
        st = c.state()
        if st["fe_state"] == 1 and st["dispatch"] == 0: return
        if not c.frames_advancing(1.0): c.press("space")
    raise AssertionError("no main menu: %r" % st)


def _set_state(c, state):
    for _ in range(40):   # the setter 0x47EF20 ignores calls during a menu transition
        c.run("dev.call(0x47EF20, 'cdecl', %d)" % state)
        if c.run("return dev.fe_state()") == state:
            return
        time.sleep(0.1)
    raise AssertionError("front-end state 0x%X not reached" % state)


def test_developer_page_from_its_plugin(c):
    # CHEATS > DEVELOPER (front-end state 0x46) and its texts come from dsfix.developer through dsfix.services
    # SetScreenProvider / SetTextProvider; the debug-key dispatcher's flag call is its hook
    ps = plugins(c)
    if "dsfix.developer" not in ps:
        pytest.skip("dsfix.developer not installed")
    p = ps["dsfix.developer"]
    assert p["external"] and not p["disabled"] and p["flags"] & SIM and not p["flags"] & INTERNAL, p
    if not c.run('return dev.setting("FeatCheats")'):
        assert "[dsfix.developer] INFO [off]  Developer: EXTENDED CHEATS is off" in log_text(c, 20000)
        return
    owners = [h["owner"] for h in c.run("return dev.pse_hooks()") if h["va"] == 0x4473B5]
    assert owners == ["dsfix.developer"], owners
    assert c.run("return dev.game_text(%d)" % 0xC00D0001) == "DEVELOPER"
    assert c.run("return dev.game_text(%d)" % 0xC00D0009) == "DEBUG KEYS"
    _main_menu(c)
    try:
        _set_state(c, 0x46)
        assert c.frames_advancing(1.0)
        assert c.run("return dev.fe_state()") == 0x46
        assert "[dsfix.developer] INFO Developer: screen built" in log_text(c, 20000)
    finally:
        _set_state(c, 1)
    assert c.frames_advancing(1.0)


def test_video_page_from_the_graphics_plugin(c):
    # OPTIONS > VIDEO (front-end state 0x4C) is dsfix.graphics' (2026-10-25): dsfix.dll's screen router and Lookup hook
    # reach it through the interface dsfix.graphics; the overlay draws on the game layer's PSE_EVENT_END_SCENE in
    # dsfix.dll, no longer from the plugin's EndScene hook
    ps = plugins(c)
    if "dsfix.graphics" not in ps:
        pytest.skip("dsfix.graphics not installed")
    p = ps["dsfix.graphics"]
    assert p["external"] and not p["disabled"] and p["flags"] & CLIENT and not p["flags"] & INTERNAL, p
    assert "dsfix.restart" in ps and ps["dsfix.restart"]["flags"] & INTERNAL   # the "+restart" wait stays in dsfix.dll
    owners = [h["owner"] for h in c.run("return dev.pse_hooks()") if h["va"] == 0x5D8308]
    assert "dsfix.graphics" in owners, owners   # the Direct3DCreate8 import slot
    text = log_text(c, 20000)
    assert "[fail] Overlay: no PSE_EVENT_END_SCENE" not in text
    assert c.run("return dev.game_text(%d)" % 0xC0120001) == "VIDEO"
    _main_menu(c)
    try:
        _set_state(c, 0x4C)
        t0 = time.time()
        while time.time() - t0 < 5 and "Video menu: screen built" not in log_text(c, 20000):
            time.sleep(0.2)
        assert "Video menu: screen built" in log_text(c, 20000)
        assert c.frames_advancing(1.0)
        # the page's row texts come from the plugin (row 0 = the first row's label)
        assert c.run("return dev.game_text(%d)" % 0xC0120010)
    finally:
        _set_state(c, 1)
    assert c.frames_advancing(1.0)


def test_display_and_intro_from_their_plugins(c, split):
    # the test harness runs the game windowed (DisplayMode 1): the window was placed by dsfix.display
    text = log_text(c, 20000)
    assert "[dsfix.display] INFO [ok]   Display mode: window hooks" in text
    assert "[dsfix.display] INFO Display: windowed" in text, "the window wasn't placed by the plugin"
    assert "[dsfix.cinematics] INFO [ok]   Cutscene skip: movie-cancel hooks" in text
    # SKIP INTRO (DesertStormFix's setting, read through dsfix.services): 0 = logos + intro film, 1 = straight to the menu
    want = 0 if c.run('return dev.setting("SkipIntro")') else 1
    assert c.read(0x5E6C80, "u32") in (0, want)    # the first front-end load clears it after the logos
    assert c.read(0x5ED230, "u32") in (0, want)
