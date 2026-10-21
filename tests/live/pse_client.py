r"""Client for the DesertStormFix dev server (src/features/devserver.cpp, docs/internal/devserver.md). Stdlib only.

    from pse_client import Client
    with Client() as c:                       # newest \\.\pipe\pse-dev-<pid>, or Client(pid=1234)
        c.run("return dev.state()")           # Lua on the game thread -> Python value
        c.run("return dev.u32(0x617C18)", async_=True)   # on the pipe thread (works while the game is loading)

Errors in the Lua chunk raise PseError (the game keeps running).
"""
import json, os, re, struct, time

PIPE_DIR = "\\\\.\\pipe\\"
STATE_MAIN_MENU, STATE_IN_GAME = 0x1, 0xE


class PseError(RuntimeError):
    """The Lua chunk raised an error (message = the error with its Lua traceback)."""
    def __init__(self, message, reply=None):
        super().__init__(message)
        self.reply = reply or {}


def pipes():
    """pids that have a dev server pipe, newest process first (pid order is not age order - sorted by pipe listing,
    then by pid as a fallback)."""
    try:
        names = os.listdir(PIPE_DIR)
    except OSError:
        return []
    pids = [int(m.group(1)) for n in names if (m := re.fullmatch(r"pse-dev-(\d+)", n))]
    return sorted(set(pids), key=lambda p: -p)


def wait_for_pipe(pid=None, timeout=60.0):
    """Waits until a pse-dev pipe exists (that pid's, or any) and returns its pid."""
    t0 = time.time()
    while time.time() - t0 < timeout:
        found = pipes()
        if pid is None and found: return found[0]
        if pid is not None and pid in found: return pid
        time.sleep(0.2)
    raise TimeoutError("no pse-dev pipe%s after %.0f s" % ("" if pid is None else " for pid %d" % pid, timeout))


class Client:
    def __init__(self, pid=None, connect_timeout=10.0):
        self.pid = pid if pid is not None else wait_for_pipe(timeout=connect_timeout)
        path = PIPE_DIR + "pse-dev-%d" % self.pid
        t0 = time.time()
        while True:
            try:
                self.f = open(path, "r+b", buffering=0)
                break
            except OSError:   # all instances busy (ERROR_PIPE_BUSY) for a moment - the server makes a new one
                if time.time() - t0 > connect_timeout: raise
                time.sleep(0.05)
        self.last = None   # last full reply (dict): ms, frame, thread, print

    def close(self):
        try: self.f.close()
        except OSError: pass

    def __enter__(self): return self
    def __exit__(self, *exc): self.close()

    def _read(self, n):
        out = b""
        while len(out) < n:
            chunk = self.f.read(n - len(out))
            if not chunk: raise ConnectionError("dev server closed the pipe (game exited?)")
            out += chunk
        return out

    def request(self, lua, async_=False, limit_ms=None, wait_ms=None):
        """Sends one chunk, returns the whole reply dict ({"result"|"error", "print", "ms", "frame", "thread"})."""
        head = ""
        if async_: head += "--@async\n"
        if limit_ms is not None: head += "--@limit %d\n" % limit_ms
        if wait_ms is not None: head += "--@wait %d\n" % wait_ms
        data = (head + lua).encode("utf-8")
        self.f.write(struct.pack("<I", len(data)) + data)
        n = struct.unpack("<I", self._read(4))[0]
        reply = json.loads(self._read(n).decode("utf-8"))
        self.last = reply
        return reply

    def run(self, lua, async_=False, timeout=None, limit_ms=None):
        """Runs a Lua chunk and returns its value (several return values -> list). timeout = seconds a sync request
        may wait for the game thread (default 60)."""
        reply = self.request(lua, async_, limit_ms, None if timeout is None else int(timeout * 1000))
        if "error" in reply: raise PseError(reply["error"], reply)
        return reply.get("result")

    # ---- convenience ----
    def read(self, addr, kind="u32", async_=True):
        return self.run("return dev.%s(%d)" % (kind, addr), async_=async_)

    def write(self, addr, value, kind="u32"):
        return self.run("dev.write_%s(%d, %r)" % (kind, addr, value))

    def call(self, addr, conv="cdecl", *args, float_result=False, async_=False):
        def lua_arg(a):
            if isinstance(a, bool): return "true" if a else "false"
            if isinstance(a, int): return str(a)
            if isinstance(a, float): return repr(a) if "." in repr(a) or "e" in repr(a) else repr(a) + ".0"
            if isinstance(a, str): return lua_string(a)
            if a is None: return "nil"
            raise TypeError("can't pass %r" % (a,))
        fn = "callf" if float_result else "call"
        return self.run("return dev.%s(%d, %s%s)" % (fn, addr, lua_string(conv), "".join(", " + lua_arg(a) for a in args)),
                        async_=async_)

    def state(self):
        """{frame, fe_state, dispatch, level, focused, split, players, window} (async: works while loading)."""
        return self.run("return dev.state()", async_=True)

    def frame(self):
        return self.run("return dev.frame()", async_=True)

    def player(self, n=0, async_=True):
        return self.run("return dev.player(%d)" % n, async_=async_)

    def press(self, key, hold_ms=80):
        """Posts WM_KEYDOWN / WM_KEYUP to the game window (async: works in PLEASE PRESS ANY KEY too)."""
        k = str(key) if isinstance(key, int) else lua_string(key)
        self.run("return dev.key(%s, %d)" % (k, hold_ms), async_=True)
        time.sleep(hold_ms / 1000.0 + 0.05)

    def console(self, line):
        """A developer console command (game thread); returns the lines it printed."""
        return self.run("return dev.console(%s)" % lua_string(line))

    def log(self, n=50):
        return self.run("return dev.log(%d)" % n, async_=True)

    def wait_until(self, predicate, timeout=30.0, interval=0.1, async_=True):
        """Polls a Lua expression (e.g. "dev.fe_state() == 0xE") until it is truthy; returns its value."""
        t0 = time.time()
        last = None
        while time.time() - t0 < timeout:
            last = self.run("return (%s)" % predicate, async_=async_)
            if last: return last
            time.sleep(interval)
        raise TimeoutError("%s still %r after %.0f s" % (predicate, last, timeout))

    def frames_advancing(self, within=1.0):
        f0 = self.frame()
        t0 = time.time()
        while time.time() - t0 < within:
            time.sleep(0.05)
            if self.frame() != f0: return True
        return False

    def wait_for_play(self, timeout=120.0, player=0):
        """After a level start: waits for in-game play (front-end state 0xE, frames running, player n has a soldier).
        Presses Space whenever the game thread stalls (PLEASE PRESS ANY KEY waits for a key inside the load)."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            st = self.state()
            if st["dispatch"] == 0 and st["fe_state"] == STATE_IN_GAME and self.frames_advancing(0.5):
                p = self.player(player)
                if p: return p
            elif not self.frames_advancing(1.0):
                self.press("space")
            time.sleep(0.2)
        raise TimeoutError("no play after %.0f s (state %r)" % (timeout, self.state()))

    def start_level(self, name, wait=True, timeout=180.0):
        """'level <name>' in the console (Mission1.dll, mission3, a mod .lua ...); with wait: until play."""
        f0 = self.frame()
        out = self.console("level " + name)
        if not any(l.startswith("starting ") for l in out): raise PseError("console: " + " / ".join(out))
        if not wait: return out
        t0 = time.time()   # the load starts on a later frame: dispatch mode 1, or the game thread stops framing
        while time.time() - t0 < 30:
            if self.state()["dispatch"] != 0 or not self.frames_advancing(0.3): break
        return self.wait_for_play(timeout)


def lua_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'


if __name__ == "__main__":
    import sys
    with Client(int(sys.argv[2]) if len(sys.argv) > 2 else None) as c:
        print(json.dumps(c.request(sys.argv[1] if len(sys.argv) > 1 else "return dev.state()",
                                   async_=os.environ.get("PSE_ASYNC") == "1"), indent=2))
