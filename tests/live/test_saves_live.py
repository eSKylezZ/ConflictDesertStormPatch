"""PSE co-save wired into the game's save / load (core/pse_saves.cpp, docs/sdk/reference/serialization.md "DS1").

A game save "<name>.sav" gets "<name>.pse" next to it with the plugins' chunks (dsfix.loadout 'KITS', 'GEAR', 'VEHS' -
until 2026-10-25 GEAR was dsfix.gear's and VEHS dsfix.vehicles'; dsfix.loadout adopts those names); loading the save
hands them back; chunks of plugins that aren't installed survive a rewrite; chunks under the old names come back and
are rewritten once under dsfix.loadout; a save from before the co-save (".sav.dsfix" sidecar only) is still read.

The saves are made / loaded by calling what the game's own call sites call (the save menu 0x475404 and the load menu
0x449B73 are hooked: the call target there is the hook chain) - no menu driving. Only files named "PSE Live Test*"
are created and they are deleted at the end; the user's saves are only listed (before == after)."""
import ctypes, os, shutil, struct, time

import pytest

SAVE_SITE, LOAD_SITE = 0x475404, 0x449B73
SAVER, CAMPAIGN, DIRECT_LEVEL, NEXT_LEVEL, DISPATCH = 0x60EDC4, 0x60EDC0, 0x60EE14, 0x5E7028, 0x60EC8C
CONNECTORS = ["BIP01 HAT CONNECTOR", "BIP01 FACE CONNECTOR", "BIP01 SPINE2 CONNECTOR", "BIP01 SPINE2L CONNECTOR",
              "BIP01 SPINE2R CONNECTOR", "BIP01 HIPL CONNECTOR", "BIP01 HIPR CONNECTOR", "BIP01 CHESTL CONNECTOR",
              "BIP01 CHESTR CONNECTOR"]
PREFIX = "PSE Live Test"


def save_dir():
    buf = ctypes.create_unicode_buffer(260)
    ctypes.windll.shell32.SHGetFolderPathW(0, 5, 0, 0, buf)   # CSIDL_PERSONAL
    return os.path.join(buf.value, "SCi", "DesertStorm")


def tag(s):   # PSE_TAG('G','E','A','R') = 'G' in the high byte
    return struct.unpack(">I", s.encode())[0]


def parse_pse(path):
    """[(plugin, tag, version, data)] - the format of docs/sdk/reference/serialization.md."""
    b = open(path, "rb").read()
    assert b[:4] == b"PSE\x01", b[:8]
    n = struct.unpack_from("<I", b, 4)[0]
    p, out = 8, []
    for _ in range(n):
        ln = b[p]; p += 1
        name = b[p:p + ln].decode(); p += ln
        t, v, size = struct.unpack_from("<III", b, p); p += 12
        out.append((name, t, v, b[p:p + size])); p += size
    assert p == len(b), "trailing bytes"
    return out


def write_pse(path, chunks):
    b = b"PSE\x01" + struct.pack("<I", len(chunks))
    for name, t, v, data in chunks:
        b += bytes([len(name)]) + name.encode() + struct.pack("<III", t, v, len(data)) + data
    open(path, "wb").write(b)


def decode_gear(data):
    def s(p):
        n = struct.unpack_from("<H", data, p)[0]
        return data[p + 2:p + 2 + n].decode("latin-1"), p + 2 + n
    roles, p, out = struct.unpack_from("<I", data, 0)[0], 4, {}
    for _ in range(roles):
        role, n = struct.unpack_from("<iI", data, p); p += 8
        pieces = []
        for _ in range(n):
            conn, p = s(p)
            model, p = s(p)
            pieces.append((conn, model))
        out[role] = pieces
    assert p == len(data)
    return out


LUA_HELPERS = r"""
function sv_conn(inst, name)
  return dev.call(0x502C30, "thiscall", inst, dev.call(0x4B9C30, "cdecl", name))
end
function sv_worn(name)   -- player 1's soldier: model pointer on the connector (0 none, nil no connector)
  local s = dev.u32(0x60F5B8 + 0x310); if s == 0 then return nil end
  local inst = dev.u32(s + 0x2C); local i = sv_conn(inst, name)
  if i == 0xFFFFFFFF then return nil end
  return dev.call(0x502C10, "thiscall", inst, i)
end
function sv_take_off(name)
  local s = dev.u32(0x60F5B8 + 0x310); local inst = dev.u32(s + 0x2C)
  dev.call(0x502BF0, "thiscall", inst, sv_conn(inst, name), 0)
end
function sv_role()
  local s = dev.u32(0x60F5B8 + 0x310); return dev.i8(dev.u32(s + 0x24) + 0x1C7)
end
"""


@pytest.fixture(scope="module")
def saves(game):
    c = game.client
    d = save_dir()
    if not os.path.isdir(d):
        pytest.skip("no save folder %s" % d)
    before = {f: os.path.getmtime(os.path.join(d, f)) for f in os.listdir(d) if not f.startswith(PREFIX)}
    for f in os.listdir(d):   # leftovers of an aborted run of this test (only our own names)
        if f.startswith(PREFIX): os.remove(os.path.join(d, f))
    c.run(LUA_HELPERS)
    c.run("dev.log_write('saves fixture start')")
    c.start_level("Mission1.dll")
    close_loadout_screen(c)
    yield d
    for f in os.listdir(d):
        if f.startswith(PREFIX): os.remove(os.path.join(d, f))
    after = {f: os.path.getmtime(os.path.join(d, f)) for f in os.listdir(d)}
    assert after == before, "the user's saves changed"


def close_loadout_screen(c, timeout=40):
    """The briefing / loadout screen (customise.cpp) opens 0.5 s into play; the tests save / load with it closed.
    dev.loadout_close() closes it (posted Enter doesn't always reach it in background runs)."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        lines = log_since(c, "saves fixture start")
        opened = [i for i, l in enumerate(lines) if "Customise: opened" in l]
        if opened:   # (a "closed" before it = the old level's screen going with the level change)
            if any("Customise: closed" in l for l in lines[opened[-1]:]):
                time.sleep(1.0)
                return
            if not c.run("return dev.loadout_close()"): c.press("enter")
        time.sleep(0.5)
    pytest.skip("the loadout screen didn't open / close")


def in_mission(c):
    if not (c.state()["fe_state"] == 0xE and c.player(0)):
        c.start_level("Mission1.dll")
    return c.wait_for_play()


def game_save(c, path):
    """The save menu's call (thiscall(saver, path, 0)) through whatever is hooked at its call site. Direct level starts
    (console) set [0x60EE14], which makes the saver refuse in some campaign states - cleared for the call."""
    lua = r"""
    local site = %d
    local target = (site + 5 + dev.i32(site + 1)) & 0xFFFFFFFF
    local buf = dev.alloc(512); dev.write_cstring(buf, %s)
    local direct = dev.u8(0x60EE14); dev.write_u8(0x60EE14, 0)
    local r = dev.call(target, "thiscall", dev.u32(0x60EDC4), buf, 0)
    dev.write_u8(0x60EE14, direct); dev.free(buf)
    return r
    """ % (SAVE_SITE, lua_str(path))
    return c.run(lua)


def game_load(c, path):
    """The load menu's call, then what the menu does after it (0x449B7C..0x449BBE: in-game state, the campaign's level
    into the next-level slot, 0x409F20) and a level load (dispatch mode 1, as its 'return 3' leads to)."""
    lua = r"""
    local site = %d
    local target = (site + 5 + dev.i32(site + 1)) & 0xFFFFFFFF
    local buf = dev.alloc(512); dev.write_cstring(buf, %s)
    local r = dev.call(target, "thiscall", dev.u32(0x60EDC4), buf)   -- (buf kept: the game may hold the path)
    if r == 0 then return 0 end
    local camp = dev.u32(0x60EDC0)
    local name = dev.cstring(camp + (dev.u32(camp + 4) == 2 and 0x37 or 0x14))
    dev.write_cstring(dev.u32(0x5E7028), name)
    dev.call(0x409F20, "cdecl")
    dev.write_u8(0x60EC8C, 1)
    return name
    """ % (LOAD_SITE, lua_str(path))
    return c.run(lua)


def wait_reload(c):
    """After game_load: the level load starts on this frame's dispatch, then play (PLEASE PRESS ANY KEY handled)."""
    t0 = time.time()
    while time.time() - t0 < 30:
        if c.state()["dispatch"] != 0 or not c.frames_advancing(0.3): break
    return c.wait_for_play()


def lua_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def log_since(c, marker):
    lines = c.log(800)
    for i in range(len(lines) - 1, -1, -1):
        if marker in lines[i]:
            return lines[i:]
    return lines


def test_new_game_sent_when_a_game_starts(c):
    in_mission(c)
    assert any("PSE saves: new game" in l for l in c.log(5000)), "no NEW_GAME at the level start from the front end"


def test_save_writes_cosave_with_chunks(c, saves):
    in_mission(c)
    role = c.run("return sv_role()")
    # Something to recognise: the first piece Bradley wears, taken off (the level's default puts it back on a load).
    worn = c.run("local t = {} for _, n in ipairs(%s) do local m = sv_worn(n) if m and m ~= 0 then t[#t+1] = n end end"
                 " return t" % ("{" + ",".join(lua_str(n) for n in CONNECTORS) + "}"))
    assert worn, "player 1 wears no gear"
    removed = worn[0]
    c.run("sv_take_off(%s)" % lua_str(removed))
    assert c.run("return sv_worn(%s)" % lua_str(removed)) == 0

    path = os.path.join(saves, PREFIX + ".sav")
    c.run("dev.log_write('save test start')")
    assert game_save(c, path) == 1
    assert os.path.exists(path)
    co = os.path.join(saves, PREFIX + ".pse")
    assert os.path.exists(co), log_since(c, "save test start")
    assert not os.path.exists(path + ".dsfix"), "an old sidecar was written"
    chunks = parse_pse(co)
    print("\n[live] co-save chunks:", [(n, struct.pack(">I", t).decode(), v, len(d)) for n, t, v, d in chunks])
    gear = [d for n, t, v, d in chunks if n == "dsfix.loadout" and t == tag("GEAR") and v == 1]
    assert len(gear) == 1
    pieces = dict(decode_gear(gear[0])[role])
    assert pieces.get(removed) == "", (removed, pieces)   # saved as "nothing on it"
    assert any(m for m in pieces.values()), pieces
    assert any("PSE saves: co-save" in l and "written" in l for l in log_since(c, "save test start"))


def test_unknown_plugin_chunks_survive_load_and_resave(c, saves):
    path = os.path.join(saves, PREFIX + ".sav")
    co = os.path.join(saves, PREFIX + ".pse")
    chunks = parse_pse(co)
    ghost = ("test.ghost", tag("GHST"), 3, b"kept across a load + save")
    write_pse(co, chunks + [ghost])
    c.run("dev.log_write('load test start')")
    level = game_load(c, path)
    assert level, "the game refused to load the test save"
    wait_reload(c)
    lines = log_since(c, "load test start")
    assert any("PSE saves: co-save" in l and "loaded" in l for l in lines), lines[-30:]
    assert not any("PSE saves: new game" in l for l in lines), "a loaded save must not send NEW_GAME"
    # Bradley's taken-off piece stays off: the gear chunk came back through dsfix.loadout's load callback.
    removed = [conn for conn, m in decode_gear([d for n, t, v, d in chunks if t == tag("GEAR")][0])[c.run("return sv_role()")]
               if m == ""]
    assert removed
    deadline = time.time() + 10
    while time.time() < deadline and c.run("return sv_worn(%s)" % lua_str(removed[0])) != 0:
        time.sleep(0.2)
    assert c.run("return sv_worn(%s)" % lua_str(removed[0])) == 0, "the saved gear wasn't put back"
    assert any("saved gear back on" in l for l in log_since(c, "load test start"))
    # Rewrite the same slot: the ghost plugin isn't installed, its chunk is kept.
    assert game_save(c, path) == 1
    again = parse_pse(co)
    assert ghost in again, again
    assert sum(1 for x in again if x[1] == tag("GEAR")) == 1


def text(s):
    return struct.pack("<H", len(s)) + s.encode()


def test_chunks_under_the_old_plugin_names_are_adopted(c, saves):
    """A save from before the loadout group was one DLL: GEAR under dsfix.gear, VEHS under dsfix.vehicles. Both reach
    dsfix.loadout's parts (manifest "adopts"); the next save writes them under dsfix.loadout only."""
    path = os.path.join(saves, PREFIX + ".sav")
    co = os.path.join(saves, PREFIX + ".pse")
    chunks = []
    for n, t, v, d in parse_pse(co):
        if t == tag("GEAR"): n = "dsfix.gear"
        if t != tag("VEHS"): chunks.append((n, t, v, d))
    fake = "PSE_ADOPT_TEST_UNIT"
    chunks.append(("dsfix.vehicles", tag("VEHS"), 1, struct.pack("<I", 1) + text(fake) + text("US_VEH_M2A1_Bradley")))
    write_pse(co, chunks)
    c.run("dev.log_write('adopt test start')")
    assert game_load(c, path), "the game refused to load the test save"
    wait_reload(c)
    deadline = time.time() + 15
    while time.time() < deadline and not any("saved gear back on" in l for l in log_since(c, "adopt test start")):
        time.sleep(0.3)
    lines = log_since(c, "adopt test start")
    assert any("saved gear back on" in l for l in lines), "GEAR under dsfix.gear didn't come back"
    # the vehicle swap reached the level build: applied, or refused by name (the unit isn't in Mission 1)
    assert any(fake in l or ("swap(s) from the saved game" in l and " 0 swap(s)" not in l) for l in lines), \
        [l for l in lines if "Vehicles:" in l]
    assert game_save(c, path) == 1
    again = parse_pse(co)
    print("\n[live] after the re-save:", [(n, struct.pack(">I", t).decode()) for n, t, v, d in again])
    assert not [x for x in again if x[0] in ("dsfix.gear", "dsfix.vehicles")], again
    ours = [t for n, t, v, d in again if n == "dsfix.loadout"]
    assert tag("GEAR") in ours and len(ours) == len(set(ours)), again


def test_old_sidecar_still_read(c, saves):
    """A save with only "<name>.sav.dsfix" (made before the co-save) loads its gear through the compatibility reader;
    the sidecar is left alone and no .pse appears until the slot is saved again."""
    src = os.path.join(saves, PREFIX + ".sav")
    path = os.path.join(saves, PREFIX + " Compat.sav")
    shutil.copy2(src, path)
    side = path + ".dsfix"
    role = c.run("return sv_role()")
    target = "BIP01 HAT CONNECTOR" if c.run("return sv_worn('BIP01 HAT CONNECTOR')") is not None else CONNECTORS[1]
    open(side, "w").write("G|%d|%s|-\n" % (role, target))
    c.run("dev.log_write('compat test start')")
    assert game_load(c, path)
    wait_reload(c)
    lines = log_since(c, "compat test start")
    assert any("has no co-save - read its old sidecar" in l for l in lines), lines[-30:]
    deadline = time.time() + 10
    while time.time() < deadline and c.run("return sv_worn(%s)" % lua_str(target)) != 0:
        time.sleep(0.2)
    assert c.run("return sv_worn(%s)" % lua_str(target)) == 0
    assert os.path.exists(side) and not os.path.exists(os.path.join(saves, PREFIX + " Compat.pse"))
    # Saved again: a .pse now, the sidecar untouched.
    side_bytes = open(side, "rb").read()
    assert game_save(c, path) == 1
    assert os.path.exists(os.path.join(saves, PREFIX + " Compat.pse"))
    assert open(side, "rb").read() == side_bytes
