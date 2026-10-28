"""pse.ui + the automatic settings pages, with the example plugin example.ui_demo (pse/examples/ui_demo) deployed in
<game>\\PSE\\Plugins (the live runner copies the build's examples):

  - pse.ui is registered, ui_demo's banner is queued every frame, its registered text answers the game's text lookup;
  - OPTIONS > DESERTSTORMFIX > PLUGINS lists the plugins (DesertStormFix, dsfix.discord, ui_demo ...); ONE test reaches
    ui_demo's page and back with real posted keys (Enter / Esc through the game's own list input, each press retried -
    posted keys don't always arrive in background runs);
  - everything else is deterministic: the dev helpers dev.plugins_menu_row / _activate / _back and dev.colour_picker run
    the screens' own code on the game thread. Every setting type changes from the page: registry value + the plugin's
    OnChange (its log line) - bool, int, choice, colour (wheel: set + OK, Cancel), restart marker / footer; the string
    row is read-only; the hidden setting has no row;
  - the front-end pop-up bar and keyboard capture (as plugin dsfix.dev).

Screenshots of the pages go to PSE_SHOT_DIR (default: %TEMP%\\pse-ui-shots). Every ui_demo value is put back (values
that weren't stored before are deleted again)."""
import os, sys, tempfile, time, types, winreg

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools"))
KEY = r"Software\PSE\ds1\Plugins\example.ui_demo"
PLUGIN = "example.ui_demo"
SHOTS = os.environ.get("PSE_SHOT_DIR") or os.path.join(tempfile.gettempdir(), "pse-ui-shots")
LIST, PAGE, FIXMENU = 0x4D, 0x4E, 0x47


def reg_values():
    out = {}
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, KEY) as k:
            i = 0
            while True:
                try:
                    name, value, kind = winreg.EnumValue(k, i)
                except OSError:
                    break
                out[name] = (value, kind)
                i += 1
    except OSError:
        pass
    return out


def reg(name):
    v = reg_values().get(name)
    return v[0] if v else None


def log_text(c, n=3000):
    lines = c.log(n)
    return "\n".join(lines) if isinstance(lines, list) else str(lines)


def wait_log(c, text, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if text in log_text(c, 400): return True
        time.sleep(0.2)
    return False


def shot(game, name):
    """PrintWindow of the game's client area -> SHOTS/<name>.png (None without tools/bg.py)."""
    try:
        sys.path.insert(0, TOOLS)
        import bg
    except ImportError:
        return None
    os.makedirs(SHOTS, exist_ok=True)
    g = bg.Game.__new__(bg.Game)
    g.p = types.SimpleNamespace(pid=game.pid)
    g.hwnd = bg.find_window(cls="ConflictDesertStorm", pid=game.pid, wait=2)
    if not g.hwnd: return None
    path = os.path.join(SHOTS, name + ".png")
    g.shot(path)
    return path


def menu(c):
    return c.run("return dev.plugins_menu()")


def to_front_end(c):
    st = c.state()
    if st["fe_state"] in (1, LIST, PAGE, FIXMENU, 4) and st["dispatch"] == 0 and not st["level"].lower().startswith("mission"):
        return
    c.console("level FrontEnd.dll")
    t0 = time.time()
    while time.time() - t0 < 120:
        st = c.state()
        if st["fe_state"] == 1 and st["dispatch"] == 0: return
        if not c.frames_advancing(1.0): c.press("space")
    raise AssertionError("no main menu: %r" % st)


def set_state(c, state):
    """The front-end state setter 0x47EF20 (ignored during a transition - retried)."""
    for _ in range(40):
        c.run("dev.call(0x47EF20, 'cdecl', %d)" % state)
        if c.run("return dev.fe_state()") == state:
            time.sleep(0.5)   # the transition (~0.33 s): keys posted meanwhile are ignored
            return
        time.sleep(0.1)
    raise AssertionError("front-end state 0x%X not reached" % state)


def press(c, key, n=1):
    for _ in range(n):
        c.press(key)
        time.sleep(0.12)


@pytest.fixture(scope="module")
def demo(game):
    c = game.client
    plugins = {p["name"]: p for p in c.run("return dev.pse_plugins()")}
    if PLUGIN not in plugins:
        pytest.skip("example.ui_demo not installed in <game>\\PSE\\Plugins (deploy the build's pse_plugins first)")
    before = reg_values()
    live = {k: c.run('return dev.plugin_setting("%s", "%s")' % (PLUGIN, k))
            for k in ("ShowBanner", "BannerY", "BannerFont", "BannerAlign", "BannerColour", "PopupOnLevel", "LogChanges")}
    yield plugins[PLUGIN]
    for k, v in live.items():   # the running game first (its cache), then the registry exactly as it was
        c.run('return dev.set_plugin_setting("%s", "%s", %d)' % (PLUGIN, k, v))
    after = reg_values()
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, KEY) as k:
        for name in after:
            if name not in before: winreg.DeleteValue(k, name)
        for name, (value, kind) in before.items():
            winreg.SetValueEx(k, name, 0, kind, value)
    if not before:   # the key didn't exist (or was empty): leave none behind
        try:
            winreg.DeleteKey(winreg.HKEY_CURRENT_USER, KEY)
        except OSError:
            pass
    try:
        c.run("dev.ui_capture(false)")
    except Exception:
        pass


def test_ui_registered_and_drawing(c, demo):
    ui = c.run("return dev.ui()")
    assert ui["registered"], "pse.ui not registered"
    text = log_text(c, 20000)
    assert "pse.ui 1: registered" in text
    assert "ui_demo: loaded, pse.ui present" in text
    c.run('return dev.set_plugin_setting("%s", "ShowBanner", 1)' % PLUGIN)
    assert c.wait_until("dev.ui().texts >= 1", timeout=5), "the banner isn't queued"


def test_registered_text(c, demo):
    import re
    m = re.search(r"ui_demo: loaded, pse\.ui present, text hash ([0-9A-F]{8})", log_text(c, 20000))
    assert m, "no text hash in the log"
    h = int(m.group(1), 16)
    assert h & 0xFF000000 == 0xC4000000, hex(h)
    t = c.run("return dev.game_text(%d)" % h)
    assert t and t.startswith("Hello from ui_demo"), repr(t)


DEMO_ROW = "UI DEMO (EXAMPLE PLUGIN) 1.0.0"


def open_list(c):
    """The PLUGINS list through the dev helper (front-end state setter, retried over transitions)."""
    to_front_end(c)
    for _ in range(40):
        if c.run("return dev.plugins_menu_open()"): break
        time.sleep(0.1)
    c.wait_until("dev.fe_state() == 0x%X" % LIST, timeout=5)
    time.sleep(0.5)   # the transition (~0.33 s)


def open_page(c):
    if c.run("return dev.fe_state()") == PAGE and menu(c).get("plugin") == PLUGIN: return
    to_front_end(c)
    for _ in range(40):
        if c.run('return dev.plugins_menu_open("%s")' % PLUGIN): break
        time.sleep(0.1)
    c.wait_until("dev.fe_state() == 0x%X" % PAGE, timeout=5)
    time.sleep(0.5)


def row_of(c, label):
    m = menu(c)
    assert label in m["labels"], (label, m)
    return m["labels"].index(label)


def act(c, label, direction):
    """What Left (-1) / Enter (0) / Right (+1) do on the row - the screen's own code, on the game thread."""
    assert c.run("return dev.plugins_menu_activate(%d, %d)" % (direction, row_of(c, label))), label


def test_real_keys_open_list_and_page(c, game, demo):
    """The one test with real posted keys (Enter / Esc through the game's own list input). Posted keys don't always
    arrive in background runs: each press is retried until the screen changes."""
    def press_until(key, state, tries=6):
        for _ in range(tries):
            press(c, key)
            try:
                c.wait_until("dev.fe_state() == 0x%X" % state, timeout=1.5)
                time.sleep(0.4)
                return True
            except TimeoutError:
                pass
        return False
    to_front_end(c)
    set_state(c, FIXMENU)
    assert press_until("enter", LIST), "Enter on DESERTSTORMFIX > PLUGINS never opened the list"   # PLUGINS = row 0
    assert c.run("return dev.plugins_menu_row(%d)" % row_of(c, DEMO_ROW))
    assert press_until("enter", PAGE), "Enter on the ui_demo row never opened its page"
    assert menu(c)["plugin"] == PLUGIN
    assert press_until("esc", LIST), "Esc on the page never went back to the list"
    m = menu(c)
    assert m["labels"][m["row"]] == DEMO_ROW, m   # the cursor back on the plugin
    assert press_until("esc", FIXMENU), "Esc on the list never went back to DESERTSTORMFIX"


def test_plugins_list(c, game, demo):
    open_list(c)
    m = menu(c)
    assert m.get("screen") == "list", m
    assert DEMO_ROW in m["labels"], m["labels"]
    assert m["values"][m["labels"].index(DEMO_ROW)] == "SETTINGS"
    dsfix = [i for i, l in enumerate(m["labels"]) if l.startswith("DESERTSTORMFIX ")]
    assert dsfix and m["values"][dsfix[0]] == "LOADED", m
    shot(game, "plugins_list")
    assert c.run("return dev.plugins_menu_row(%d)" % m["labels"].index(DEMO_ROW))
    m = menu(c)
    assert m["labels"][m["row"]] == DEMO_ROW
    assert any("SETTING" in f for f in m["footer"]), m["footer"]
    shot(game, "plugins_list_ui_demo")
    assert c.run("return dev.plugins_menu_activate(0)")   # Enter on it: its page
    c.wait_until("dev.fe_state() == 0x%X" % PAGE, timeout=5)


def test_page_rows(c, game, demo):
    open_page(c)
    m = menu(c)
    assert m.get("screen") == "page" and m["plugin"] == PLUGIN, m
    assert m["labels"] == ["- BANNER -", "SHOW BANNER", "BANNER HEIGHT (%)", "BANNER FONT", "BANNER ALIGNMENT",
                           "BANNER COLOUR", "GREETING", "- MESSAGES -", "POP-UP AT LEVEL START", "LOG EVERY CHANGE"], m
    v = dict(zip(m["labels"], m["values"]))
    assert v["BANNER COLOUR"].startswith("#")
    assert v["POP-UP AT LEVEL START"].endswith(" +")
    assert v["LOG EVERY CHANGE"].endswith(" *")
    assert v["GREETING"].startswith("HELLO FROM UI_DEMO")
    assert c.run("return dev.plugins_menu_row(9)")   # the last row: the window scrolls
    m = menu(c)
    assert m["row"] == 9 and m["offset"] == 2, m
    assert c.run("return dev.plugins_menu_row(0)")
    shot(game, "page_top")


def changes(c):
    return log_text(c, 2000).count("ui_demo: changed")


def changed(c, n, key, timeout=5.0):
    t0 = time.time()
    while time.time() - t0 < timeout:
        text = log_text(c, 2000)
        if text.count("ui_demo: changed") > n and ("ui_demo: changed %s =" % key) in text: return True
        time.sleep(0.2)
    return False


def test_change_every_type(c, game, demo):
    open_page(c)
    get = lambda k: c.run('return dev.plugin_setting("%s", "%s")' % (PLUGIN, k))

    def step(label, key, direction=1):
        n = changes(c)
        act(c, label, direction)
        assert changed(c, n, key), "no OnChange for " + key

    old = get("ShowBanner")                          # bool: Right / Enter flip
    step("SHOW BANNER", "ShowBanner")
    assert get("ShowBanner") == 1 - old and reg("ShowBanner") == 1 - old
    step("SHOW BANNER", "ShowBanner", 0)
    assert get("ShowBanner") == old
    if not old:
        step("SHOW BANNER", "ShowBanner")
    y = get("BannerY")                               # int: step 2, stops at 90
    step("BANNER HEIGHT (%)", "BannerY")
    assert get("BannerY") == min(90, y + 2) and reg("BannerY") == get("BannerY")
    step("BANNER HEIGHT (%)", "BannerY", -1)
    assert get("BannerY") == y
    f = get("BannerFont")                            # choice: wraps
    step("BANNER FONT", "BannerFont")
    assert get("BannerFont") == (f + 1) % 2 and reg("BannerFont") == (f + 1) % 2
    time.sleep(0.3)
    shot(game, "page_font_changed")
    step("BANNER FONT", "BannerFont", -1)
    assert get("BannerFont") == f
    lc = get("LogChanges")                           # restart setting: marker + footer
    step("LOG EVERY CHANGE", "LogChanges")
    assert get("LogChanges") == 1 - lc
    time.sleep(0.2)
    assert "RESTART THE GAME TO APPLY" in menu(c)["footer"], menu(c)["footer"]
    shot(game, "page_restart_pending")
    step("LOG EVERY CHANGE", "LogChanges")
    assert get("LogChanges") == lc
    assert "RESTART THE GAME TO APPLY" not in menu(c)["footer"]
    n = changes(c)                                   # string: read-only
    act(c, "GREETING", 0)
    act(c, "GREETING", 1)
    time.sleep(0.4)
    assert changes(c) == n, "the string row changed something"
    col = get("BannerColour")                        # colour: Enter opens the wheel; a colour, OK
    want = 0x3366CC if col != 0x3366CC else 0xCC6633
    act(c, "BANNER COLOUR", 0)
    assert c.run("return dev.colour_picker().open"), "the colour wheel didn't open"
    assert c.run("return dev.colour_picker('set', %d)" % want)
    time.sleep(0.3)
    shot(game, "page_colour_wheel")
    n = changes(c)
    assert c.run("return dev.colour_picker('ok')")
    assert changed(c, n, "BannerColour"), "no OnChange for the colour"
    assert get("BannerColour") == want and reg("BannerColour") == want
    c.wait_until("not dev.colour_picker().open", timeout=3, async_=False)
    act(c, "BANNER COLOUR", 0)                       # Cancel leaves it alone
    assert c.run("return dev.colour_picker('set', %d)" % 0x112233)
    assert c.run("return dev.colour_picker('cancel')")
    c.wait_until("not dev.colour_picker().open", timeout=3, async_=False)
    assert get("BannerColour") == want
    shot(game, "page_after_colour")


def test_back_and_popup(c, game, demo):
    open_page(c)
    assert c.run("return dev.plugins_menu_back()")
    c.wait_until("dev.fe_state() == 0x%X" % LIST, timeout=5)
    time.sleep(0.5)
    m = menu(c)
    assert m["labels"][m["row"]] == DEMO_ROW, m   # the cursor back on the plugin
    assert c.run("return dev.plugins_menu_back()")
    c.wait_until("dev.fe_state() == 0x%X" % FIXMENU, timeout=5)
    to_front_end(c)
    assert c.run('return dev.ui_popup("Pop-up from the test: caf\\195\\169", 3000)') == 0
    c.wait_until("dev.ui().popup ~= ''", timeout=3)   # shown from the next playing frame
    ui = c.run("return dev.ui()")
    assert ui["popup"].startswith("Pop-up from the test: caf"), ui
    time.sleep(0.3)
    shot(game, "popup_front_end")


def test_keyboard_capture(c, demo):
    assert c.run("return dev.ui_capture(true)") == 0
    try:
        assert c.run("return dev.ui().capture") == "dsfix.dev"
        state = c.run("return dev.fe_state()")
        c.press("a")
        c.press("down")
        time.sleep(0.3)
        keys = c.run("return dev.ui_keys()")
        downs = [k["code"] for k in keys if k["type"] == "down"]
        assert 0x41 in downs and 0x28 in downs, keys
        assert c.run("return dev.fe_state()") == state, "a captured key reached the game's menu"
    finally:
        assert c.run("return dev.ui_capture(false)") == 0
    assert c.run("return dev.ui().capture") == ""
