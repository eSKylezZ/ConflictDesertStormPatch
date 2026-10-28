"""pse.game.ds1 in the running game (docs/sdk/reference/game-ds1.md): every value it reports, read through the
registered interface (dev.game_ds1 / dev.game_text - what a plugin's Query returns), against the dev server's own
memory reads (dev.state, dev.player, dev.u32 ...)."""
import pytest

INTERNAL, CLIENT = 0x80000000, 0x2
NONE, FRONTEND, MISSION, OUTRO = 0, 1, 2, 3
KNOWN_SHA256 = "D712F5EC82C14DA66B098332B3CC9C99689091F46317112EA35DFD393DC3E164"


def nice_title(text):
    """The old discord.cpp rule: an all-capitals title -> Title Case; control characters dropped."""
    if not any(ch.islower() for ch in text):
        text = "".join(ch.lower() if i and text[i - 1].isalpha() else ch for i, ch in enumerate(text))
    return "".join(ch for ch in text if ord(ch) >= 0x20)


def ensure_mission1(c):
    st = c.state()
    if not (st["level"].lower().startswith("mission1") and st["dispatch"] == 0 and st["fe_state"] == 0xE):
        c.start_level("Mission1.dll")
    return c.wait_for_play(timeout=180)


def test_registered_as_builtin_plugin(c):
    plugins = {p["name"]: p for p in c.run("return dev.pse_plugins()")}
    assert "pse.game.ds1" in plugins, sorted(plugins)
    p = plugins["pse.game.ds1"]
    assert p["version"] == "1.0.0"
    assert p["flags"] & INTERNAL and p["flags"] & CLIENT, hex(p["flags"])
    g = c.run("return dev.game_ds1()")
    assert g, "interface pse.game.ds1 not found through QueryInterface"


def test_process_values(c):
    g = c.run("return dev.game_ds1()")
    st = c.state()
    assert g["window"] == st["window"] and g["window"] != 0
    assert g["exe_sha256"] == KNOWN_SHA256
    assert g["exe_known"] is True


def test_text_hash_and_lookup(c):
    # TextHash = the game's own lower-case hash FUN_004b9ab0, LookupText = the menus' text.
    assert c.call(0x4B9AB0, "cdecl", "miss1_title") == 0xA90BE3A7
    title = c.run('return dev.game_text("MISS1_TITLE")')
    assert title and title == c.run("return dev.game_text(0xA90BE3A7)")
    print("\n[live] MISS1_TITLE = %r" % title)
    assert c.run("return dev.game_text(0x12345678)") is None


def test_state_matches_memory(c):
    # One Lua chunk = one game frame: both views of the same moment.
    g, st, level, fe, disp, split = c.run(
        "return dev.game_ds1(), dev.state(), dev.cstring(0x606880), dev.u32(0x617C18), dev.u32(0x60EC8C), "
        "dev.u32(0x606414)")
    assert g["level"] == level == st["level"]
    assert g["fe_state"] == fe == st["fe_state"]
    assert g["dispatch"] == disp == st["dispatch"]
    low = level.lower()
    kind = NONE if not level else FRONTEND if low.startswith(("frontend", "intro")) else \
        OUTRO if low.startswith("outro") else MISSION
    assert g["kind"] == kind, (level, g["kind"])
    if kind != MISSION:
        assert g["title"] == "" and g["local_players"] == 0 and g["players"] == []


def test_mission1_values(c):
    p0 = ensure_mission1(c)
    g, st, p, world = c.run("return dev.game_ds1(), dev.state(), dev.player(0), dev.u32(0x63C948)")
    assert g["level"].lower() == "mission1.env"
    assert g["kind"] == MISSION
    assert g["fe_state"] == 0xE and g["dispatch"] == 0
    # The title the old Discord code sent: "Mission 1: " + the catalog title in Title Case.
    raw = c.run('return dev.game_text("MISS1_TITLE")')
    assert g["title"] == "Mission 1: " + nice_title(raw), (g["title"], raw)
    print("\n[live] LevelTitle in Mission 1 = %r" % g["title"])
    # Players: single player here (no split set up by this test run).
    assert g["split"] in (0, 1) or g["split"] == st["players"]
    assert g["local_players"] == max(1, g["split"])
    pl = g["players"][0]
    assert pl["block"] == 0 and pl["index"] == 0
    assert pl["soldier"] == p["soldier"] == p0["soldier"]
    for k in ("x", "y", "z", "health"):
        assert pl[k] == pytest.approx(p[k]), k
    assert pl["health"] > 0
    # Cutscene bit = world +0x1B8C bit 0.
    bit = c.run("return dev.u8(%d) & 1" % (world + 0x1B8C)) if world else 0
    assert g["cutscene"] == bool(bit)
    # Not online in a test run.
    assert g["online"]["active"] == 0 and g["online"]["role"] == 0 and g["online"]["local_player"] == 0
