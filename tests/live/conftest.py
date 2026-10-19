"""Live tests: one running dev game per pytest session (docs/sdk/devserver.md).

The `game` fixture attaches to a running dev game (a pse-dev-<pid> pipe whose game no other test run owns - see the
install lock in tools/bg.py), or starts one off-screen with tools/bg.py (local tooling) and kills it at the end.
  PSE_PID=<pid>    attach to that game
  PSE_START=1      never attach, always start a game
  PSE_ENH=a=1;b=2  DesertStormFix settings for a started game (restored afterwards), e.g. "Antialiasing=4;Renderer=1"
"""
import os, sys, time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.normpath(os.path.join(HERE, "..", "..", "tools"))
sys.path.insert(0, HERE)
import pse_client  # noqa: E402


class Session:
    def __init__(self, client, pid, started, boot_s):
        self.client, self.pid, self.started, self.boot_s = client, pid, started, boot_s

    def connect(self):
        """Another connection to the same game (e.g. for an async request while this one waits)."""
        return pse_client.Client(self.pid)


def _lock_owner():
    """pid of the live test run that holds the game install (tools/bg.py lock), else None."""
    try:
        sys.path.insert(0, TOOLS)
        import bg
        owner = open(bg.LOCK).read().split()[0]
        pid = int(owner)
        return pid if pid != os.getpid() and bg._pid_alive(pid) and str(pid) != os.environ.get("BG_LOCK_PARENT") else None
    except (ImportError, OSError, ValueError, IndexError):
        return None


@pytest.fixture(scope="session")
def game():
    pid = int(os.environ["PSE_PID"]) if os.environ.get("PSE_PID") else None
    if pid is None and os.environ.get("PSE_START") != "1" and pse_client.pipes() and _lock_owner() is None:
        pid = pse_client.pipes()[0]
    if pid is not None:
        c = pse_client.Client(pid)
        yield Session(c, pid, False, 0.0)
        c.close()
        return
    if not os.path.isdir(TOOLS):
        pytest.skip("no running dev game and no tools/bg.py to start one")
    sys.path.insert(0, TOOLS)
    import bg
    t0 = time.time()
    # PSE_ENH="Name=value;..." = DesertStormFix settings for this run (bg.Game restores the user's values afterwards)
    enh = {}
    for part in filter(None, os.environ.get("PSE_ENH", "").split(";")):
        name, _, value = part.partition("=")
        enh[name.strip()] = int(value, 0)
    g = bg.Game(dev={"StartLevel": "", "SplitPlayers": 0}, enh=enh)   # front end, whatever a leftover dev value says
    g.__enter__()
    try:
        pid = pse_client.wait_for_pipe(g.p.pid, timeout=90)
        c = pse_client.Client(pid)
        c.wait_until("dev.fe_state() == 1 and dev.dispatch() == 0", timeout=90)   # main menu
        boot = time.time() - t0
        print("\n[live] game %d started, main menu after %.1f s" % (pid, boot))
        yield Session(c, pid, True, boot)
        c.close()
    finally:
        g.__exit__(None, None, None)


@pytest.fixture
def c(game):
    return game.client
