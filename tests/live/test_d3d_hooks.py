"""D3D8 COM vtable hooks on the broker (graphics.cpp device / IDirect3D8 slots, memory.cpp texture Release; core/comhooks.h,
docs/sdk/reference/hooks.md "DS1 host" > "COM vtables"): every hooked slot is a broker pointer record owned by its
feature, the features still work (MSAA in the present parameters, anisotropy on the device, frame cap), and the
vtables are d3d8.dll's (Renderer 0) or the D3D9 wrapper's own inside dinput8.dll (Renderer >= 1).

For MSAA / AF values the run needs them on: PSE_ENH="Antialiasing=4;Anisotropy=8" (conftest.py, restored afterwards);
Renderer 1: PSE_ENH="Renderer=1;...".
"""
import re, time

import pytest

RENDERER, DEVICE_GLOBAL = 0x63C924, 0x755050
# IDirect3DDevice8 slots graphics.cpp hooks in every build (dev builds add 16, 25, 28, 30)
DEVICE_SLOTS = {3: "TestCooperativeLevel", 14: "Reset", 15: "Present", 20: "CreateTexture", 31: "SetRenderTarget",
                35: "EndScene", 50: "SetRenderState", 63: "SetTextureStageState", 72: "DrawPrimitiveUP",
                73: "DrawIndexedPrimitiveUP", 76: "SetVertexShader"}
DEV_ONLY_SLOTS = {16: "GetBackBuffer", 25: "CreateRenderTarget", 28: "CopyRects", 30: "GetFrontBuffer"}
CREATE_DEVICE = 15
GET_TSS, TSS_MAXANISOTROPY = 62, 21


def _hooks(c):
    return c.run("return dev.pse_hooks()")


def _pointer(hooks, va):
    return sorted([h for h in hooks if h["va"] == va and h["kind"] == "pointer"], key=lambda h: h["chain"])


def _mapped_file(pid, addr):
    """Base name of the image file mapped at addr in the game process ("" = private memory, e.g. a heap vtable)."""
    import ctypes
    from ctypes import wintypes as wt
    k32, psapi = ctypes.WinDLL("kernel32", use_last_error=True), ctypes.WinDLL("psapi", use_last_error=True)
    k32.OpenProcess.restype = wt.HANDLE
    psapi.GetMappedFileNameW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]
    h = k32.OpenProcess(0x0410, False, pid)
    buf = ctypes.create_unicode_buffer(520)
    n = psapi.GetMappedFileNameW(h, ctypes.c_void_p(addr), buf, 520)
    k32.CloseHandle(h)
    return buf.value[: n].rsplit("\\", 1)[-1].lower() if n else ""


def _graphics_line(c):
    for l in reversed(c.run("return dev.log(5000)", async_=True)):
        m = re.search(r"Graphics: (\d+)x(\d+) (\w+), MSAA (\d+)x \(setting (\d+)\), anisotropic filtering (\d+)x "
                      r"\(setting (\d+)\)", l)
        if m:
            return {"w": int(m[1]), "h": int(m[2]), "msaa": int(m[4]), "msaa_set": int(m[5]), "af": int(m[6]),
                    "af_set": int(m[7])}
    return None


def test_device_vtable_slots_are_broker_records(c, game):
    dev = c.read(DEVICE_GLOBAL)
    assert dev, "no D3D device"
    vt = c.read(dev)
    hooks = _hooks(c)
    slots = {**DEVICE_SLOTS, **DEV_ONLY_SLOTS}   # live tests run the dev build only (the dev server)
    missing = []
    for index, name in sorted(slots.items()):
        chain = _pointer(hooks, vt + index * 4)
        owners = [h["owner"] for h in chain]
        if "dsfix.graphics" not in owners:
            missing.append("%d %s: %r" % (index, name, owners))
            continue
        ours = [h for h in chain if h["owner"] == "dsfix.graphics"][0]
        assert ours["priority"] == 0, ours
        if index == 15:
            print("\n[live] Present slot 0x%08X chain: %s" % (vt + 60, " -> ".join("%s(%d)" % (h["owner"], h["priority"])
                                                                                  for h in chain)))
    assert not missing, missing
    # The IDirect3D8 object's CreateDevice slot (renderer +0x4052C = the IDirect3D8 the device came from)
    d3d = c.read(c.read(RENDERER) + 0x4052C)
    chain = _pointer(hooks, c.read(d3d) + CREATE_DEVICE * 4)
    assert "dsfix.graphics" in [h["owner"] for h in chain], chain
    # The Direct3DCreate8 import slot stays a pointer hook too
    assert "dsfix.graphics" in [h["owner"] for h in _pointer(hooks, 0x5D8308)]
    # Whose vtables: d3d8.dll's (Renderer 0) or the D3D9 wrapper's own classes in dinput8.dll (Renderer >= 1)
    renderer = c.run("return dev.setting('Renderer')") or 0
    owner = _mapped_file(game.pid, vt)
    print("[live] Renderer %d: device vtable 0x%08X in %s, IDirect3D8 vtable 0x%08X" % (renderer, vt, owner, c.read(d3d)))
    ours = {"dinput8.dll", "dsfix.dll", "pse_ds1.dll"}   # (the DSFix plugin DLL once the build is split)
    if renderer >= 1:   # the D3D9 wrapper's own classes
        assert owner in ours, owner
    else:               # the system's (d3d8.dll keeps the device vtable in private memory on Windows 10/11)
        assert owner not in ours, owner


def test_texture_release_slot_owned_by_memory(c):
    """A font sheet page texture (sheet [0x60EDB0] +0xC [page 0] +0x10): its class's Release slot (2)."""
    tex = c.run("""
        local sheet = dev.u32(0x60EDB0)
        if sheet == 0 then return 0 end
        local page = dev.u32(dev.u32(sheet + 0xC))
        if page == 0 then return 0 end
        return dev.u32(page + 0x10)
    """)
    assert tex, "no font page texture"
    chain = _pointer(_hooks(c), c.read(tex) + 2 * 4)
    print("\n[live] texture Release chain: %r" % [(h["owner"], h["priority"]) for h in chain])
    assert "dsfix.memory" in [h["owner"] for h in chain], chain


def test_no_com_hook_outside_the_broker(c):
    rows = c.run("return dev.log(5000)", async_=True)
    bad = [l for l in rows if ("[fail] Graphics" in l or "[fail] Memory: texture Release" in l or
                               ("pointer hook" in l and ("[fail]" in l or "outside the broker" in l)))]
    assert not bad, bad


def test_msaa_and_anisotropy_still_applied(c):
    g = _graphics_line(c)
    assert g, "no 'Graphics: ... MSAA' line in the log"
    print("\n[live] device %dx%d: MSAA %dx (setting %d), AF %dx (setting %d)" %
          (g["w"], g["h"], g["msaa"], g["msaa_set"], g["af"], g["af_set"]))
    renderer = c.read(RENDERER)
    multisample = c.read(renderer + 0x84 + 16)   # D3DPRESENT_PARAMETERS.MultiSampleType
    assert multisample == g["msaa"], (multisample, g)
    if g["msaa_set"] >= 2:
        assert g["msaa"] >= 2, "MSAA asked for but the device has none: %r" % g
    # MAXANISOTROPY on stage 0 read back from the device (GetTextureStageState through the vtable)
    got = c.run("""
        local d = dev.u32(%d)
        local out = dev.alloc(4)
        local hr = dev.call(dev.u32(dev.u32(d) + %d * 4), "stdcall", d, 0, %d, out)
        local v = dev.u32(out)
        dev.free(out)
        return {hr, v}
    """ % (DEVICE_GLOBAL, GET_TSS, TSS_MAXANISOTROPY))
    print("[live] GetTextureStageState(0, MAXANISOTROPY) = %r" % got)
    assert got[0] == 0
    if g["af"] >= 2:
        assert got[1] == g["af"], (got, g)


def test_frame_cap_still_working(c):
    cap = c.run("return dev.setting('FpsCap')") or 240
    f0, t0 = c.run("return {dev.frame(), dev.time_ms()}", async_=True)
    time.sleep(3.0)
    f1, t1 = c.run("return {dev.frame(), dev.time_ms()}", async_=True)
    fps = (f1 - f0) * 1000.0 / (t1 - t0)
    print("\n[live] %.1f fps (cap %d)" % (fps, cap))
    assert fps > 10
    assert fps <= min(cap, 240) * 1.15, fps
