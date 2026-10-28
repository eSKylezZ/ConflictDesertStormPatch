// Direct pad I/O (see padio.h): port of DesertStormFix's padio.cpp (games/ds1/src/features/padio.cpp; DS1 addresses
// in brackets) for The Great Escape - single player, so every pad is player 1 (blue light bar, player LED 1); no
// per-player layouts, health light bar or gyro.
//
// The game only knows DirectInput joysticks (IDirectInputDevice8A* at 0x7A9D24[j] [0x754BC8], count [0x7AA888]
// [0x754BE8]). Each is identified once: DIPROP_GUIDANDPATH gives the HID device path ("IG_" = an XInput pad),
// GetDeviceInfo the vendor / product id (054C:0CE6/0DF2 DualSense, 054C:05C4/09CC/0BA0 DualShock 4); Bluetooth when the
// path carries the HID-over-Bluetooth service {00001124-...}.
//
// Input
//   XInput pads: XInputGetState of the k-th connected XInput user for the k-th XInput joystick - triggers separate
//     (DirectInput sums them on one axis).
//   DualSense / DualShock 4: a reader thread per pad reads its HID input reports: USB 0x01 (64 bytes), Bluetooth
//     "simple" 0x01 before any output report was sent and the full 0x31 (DualSense) / 0x11 (DualShock 4) after it.
//     Sending output reports over Bluetooth switches the pad to the full report, which DirectInput can't read - that
//     is why the input is read here too. Touchpad click = Select (Objectives).
// Output (worker thread, sends only changes)
//   Rumble: the game's force-feedback table 0x7AB0B8 + j*0x1C [0x753058] (filled by FUN_004b8750 [FUN_005392d0] from
//     the rumble request FUN_005b7820 (block, effect, strength), 6 callers: +0 DI effect, +4 start ms, +8 duration ms
//     (0 = endless), +0xC/+0xE strength x/y (+-32767), +0x10 priority, +0x14 type 1 fade / 2 pulse, +0x18 period) is
//     only played by the poll for pads with a DirectInput effect (none of ours); its envelope is reproduced here and
//     expired entries are cleared like the poll does. XInput motors / DualSense "compatible vibration" / DualShock 4
//     motors (setting PadVibration).
//   Xbox One / Series trigger motors: Windows.Gaming.Input Gamepad.Vibration (k-th Microsoft-vendor gamepad): a kick
//     on each pull of the fire trigger + some of the rumble.
//   DualSense adaptive trigger (in missions, setting PadTriggers): a firm point on the fire trigger, which only fires
//     once pulled through it (also on Xbox pads). Fire is on a trigger only with PadModernLayout (R2 / RT; the
//     game's own default.key fires with R1).
#include "features/padio.h"

#include <mmsystem.h>
#include <roapi.h>
#include <windows.gaming.input.h>
#include <wrl/client.h>
#include <xinput.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "core/log.h"
#include "features/features.h"

namespace {
constexpr uint32_t kJoysticks = 0x7A9D24, kJoystickCount = 0x7AA888, kRumbleTable = 0x7AB0B8, kRumbleStride = 0x1C;
constexpr uint32_t kGameWindow = 0x7DC238;
constexpr int kMax = 8;

enum class Kind { Other, XInput, DualSense, DualShock4 };

struct Input {
    LONG lx, ly, rx, ry;  // raw -1..1 scaled to +-32767 (Y down positive, like DirectInput)
    uint8_t l2, r2;
    uint32_t buttons;  // game numbering bits 0-15 (physical: L2 = 4, R2 = 5, L1 = 6, R1 = 7)
    DWORD time;
};

struct Output {
    uint8_t strong = 0, weak = 0;  // motors
    uint8_t r = 0, g = 0, b = 0, leds = 0;
    uint8_t trigL[11] = {}, trigR[11] = {};  // DualSense trigger effects
    uint8_t tmL = 0, tmR = 0;                // Xbox trigger motors
    bool operator==(const Output&) const = default;
};

struct Pad {
    void* device = nullptr;
    Kind kind = Kind::Other;
    bool bluetooth = false;
    std::wstring path;
    std::mutex lock;
    Input in{};
    bool haveInput = false;
    bool fullReports = false;  // Bluetooth: output sent, pad now sends the full report
    HANDLE readHandle = INVALID_HANDLE_VALUE, writeHandle = INVALID_HANDLE_VALUE;
    std::thread reader;
    std::atomic<bool> dead{false};
    Output want, sent;
    bool sentOnce = false;
    uint8_t seq = 0;
    int xboxIndex = -1;  // k-th XInput joystick
    uint32_t lastButtons = 0;
    DWORD firePulse = 0;
    bool fireHeld = false;
};
Pad* g_pads[kMax] = {};
std::mutex g_padsLock;
std::atomic<bool> g_workerStarted{false};
std::atomic<bool> g_stop{false};

// ---- XInput (loaded dynamically) ----
using XGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XSetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
XGetState g_xget = nullptr;
XSetState g_xset = nullptr;
bool g_xconnected[4] = {};
DWORD g_xcheck = 0;

void LoadXInput() {
    static bool tried = false;
    if (tried) return;
    tried = true;
    for (const char* dll : {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"})
        if (HMODULE m = LoadLibraryA(dll)) {
            g_xget = reinterpret_cast<XGetState>(GetProcAddress(m, "XInputGetState"));
            g_xset = reinterpret_cast<XSetState>(GetProcAddress(m, "XInputSetState"));
            if (g_xget) return;
        }
}

// The k-th connected XInput user (connection re-checked every 2 s: XInputGetState on an empty slot is slow).
int XUser(int k) {
    LoadXInput();
    if (!g_xget || k < 0) return -1;
    if (GetTickCount() - g_xcheck > 2000) {
        g_xcheck = GetTickCount();
        XINPUT_STATE st;
        for (DWORD u = 0; u < 4; ++u) g_xconnected[u] = g_xget(u, &st) == ERROR_SUCCESS;
    }
    for (int u = 0; u < 4; ++u)
        if (g_xconnected[u] && k-- == 0) return u;
    return -1;
}

// ---- identification ----
Kind Identify(void* dev, std::wstring& path, bool& bt) {
    struct Instance {
        DWORD size;
        GUID instance, product;
        DWORD devType;
        char instanceName[260], productName[260];
        GUID ffDriver;
        WORD usagePage, usage;
    } info{};
    info.size = sizeof info;
    void** vt = *static_cast<void***>(dev);
    reinterpret_cast<HRESULT(__stdcall*)(void*, Instance*)>(vt[15])(dev, &info);
    struct GuidAndPath {
        DWORD size, headerSize, obj, how;
        GUID guidClass;
        WCHAR path[MAX_PATH];
    } prop{sizeof(GuidAndPath), 16, 0, 0, {}, {}};
    constexpr uintptr_t kGuidAndPath = 12;  // MAKEDIPROP(12)
    if (SUCCEEDED(reinterpret_cast<HRESULT(__stdcall*)(void*, uintptr_t, GuidAndPath*)>(vt[5])(dev, kGuidAndPath, &prop)))
        path = prop.path;
    std::wstring lower = path;
    for (wchar_t& c : lower) c = static_cast<wchar_t>(towlower(c));
    bt = lower.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos || lower.find(L"bth") != std::wstring::npos;
    const WORD vid = LOWORD(info.product.Data1), pid = HIWORD(info.product.Data1);
    Kind kind = Kind::Other;
    if (lower.find(L"ig_") != std::wstring::npos) kind = Kind::XInput;
    else if (vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2)) kind = Kind::DualSense;
    else if (vid == 0x054C && (pid == 0x05C4 || pid == 0x09CC || pid == 0x0BA0)) kind = Kind::DualShock4;
    static const char* const names[] = {"DirectInput only", "XInput", "DualSense", "DualShock 4"};
    dslog::Write("Pad I/O: \"%s\" %04X:%04X -> %s%s", info.productName, vid, pid, names[static_cast<int>(kind)],
                 bt ? " (Bluetooth)" : "");
    return kind;
}

// ---- Sony input reports ----
uint32_t SonyButtons(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t l2, uint8_t r2) {
    uint32_t g = 0;
    auto set = [&](int bit, bool on) { if (on) g |= 1u << bit; };
    set(0, b0 & 0x80);  // Triangle
    set(1, b0 & 0x40);  // Circle
    set(2, b0 & 0x20);  // Cross
    set(3, b0 & 0x10);  // Square
    set(4, (b1 & 0x04) || l2 > 40);
    set(5, (b1 & 0x08) || r2 > 40);
    set(6, b1 & 0x01);  // L1
    set(7, b1 & 0x02);  // R1
    set(8, (b1 & 0x10) || (b2 & 0x02));  // Create / Share, touchpad click
    set(9, b1 & 0x40);   // L3
    set(10, b1 & 0x80);  // R3
    set(11, b1 & 0x20);  // Options
    const int hat = b0 & 0x0F;  // 0 up .. 7 up-left, 8 none
    if (hat < 8) {
        set(12, hat == 7 || hat <= 1);
        set(13, hat >= 1 && hat <= 3);
        set(14, hat >= 3 && hat <= 5);
        set(15, hat >= 5 && hat <= 7);
    }
    return g;
}

bool ParseSony(Pad& p, const uint8_t* r, DWORD n, Input& in) {
    int ax, bt, tr;   // offset of LX, of the three button bytes and of L2/R2
    if (r[0] == 0x01 && n >= 64) {  // USB
        ax = 1;
        if (p.kind == Kind::DualSense) bt = 8, tr = 5;
        else bt = 5, tr = 8;
    } else if (r[0] == 0x01 && n >= 10) {  // Bluetooth simple report (both pads)
        ax = 1, bt = 5, tr = 8;
    } else if (r[0] == 0x31 && p.kind == Kind::DualSense && n >= 12) {
        ax = 2, bt = 9, tr = 6;
    } else if (r[0] == 0x11 && p.kind == Kind::DualShock4 && n >= 12) {
        ax = 3, bt = 7, tr = 10;
    } else {
        return false;
    }
    auto axis = [](uint8_t v) { return static_cast<LONG>((static_cast<int>(v) - 128) * 256); };
    in.lx = axis(r[ax]);
    in.ly = axis(r[ax + 1]);
    in.rx = axis(r[ax + 2]);
    in.ry = axis(r[ax + 3]);
    in.l2 = r[tr];
    in.r2 = r[tr + 1];
    in.buttons = SonyButtons(r[bt], r[bt + 1], r[bt + 2], in.l2, in.r2);
    in.time = GetTickCount();
    return true;
}

void ReaderThread(Pad* p) {
    uint8_t buf[1024];
    while (!g_stop && !p->dead) {
        DWORD n = 0;
        if (!ReadFile(p->readHandle, buf, sizeof buf, &n, nullptr)) {
            p->dead = true;
            break;
        }
        Input in;
        if (n && ParseSony(*p, buf, n, in)) {
            if (!p->haveInput)
                dslog::Write("Pad I/O: HID input report 0x%02X (%lu bytes), sticks %ld %ld", buf[0], n, in.lx, in.ly);
            std::lock_guard<std::mutex> guard(p->lock);
            p->in = in;
            p->haveInput = true;
        }
    }
}

// ---- output reports ----
uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc;
}
void PutCrc(uint8_t* report, size_t len) {  // Bluetooth: CRC-32 over 0xA2 + the report, in the last 4 bytes
    const uint8_t header = 0xA2;
    uint32_t crc = Crc32(&header, 1, 0xFFFFFFFF);
    crc = ~Crc32(report, len - 4, crc);
    memcpy(report + len - 4, &crc, 4);
}

bool WriteReport(Pad& p, const uint8_t* data, DWORD len) {
    DWORD written = 0;
    return WriteFile(p.writeHandle, data, len, &written, nullptr) != 0;
}

void SendDualSense(Pad& p, const Output& o) {
    uint8_t report[78] = {};
    uint8_t* c;  // common part (47 bytes)
    DWORD len;
    if (p.bluetooth) {
        report[0] = 0x31;
        report[1] = static_cast<uint8_t>(p.seq << 4);
        p.seq = (p.seq + 1) & 15;
        report[2] = 0x10;
        c = report + 3;
        len = 78;
    } else {
        report[0] = 0x02;
        c = report + 1;
        len = 63;
    }
    c[0] = 0x01 | 0x02 | 0x04 | 0x08;  // compatible vibration, haptics select, right / left trigger effect
    c[1] = 0x04 | 0x10;                // light bar, player LEDs
    c[2] = o.weak;                      // right (high frequency)
    c[3] = o.strong;                    // left (low frequency)
    memcpy(c + 10, o.trigR, 11);
    memcpy(c + 21, o.trigL, 11);
    c[38] = 0x02 | 0x04;  // light bar setup, compatible vibration 2 (newer firmware)
    c[41] = p.sentOnce ? 0 : 0x02;  // first report: fade out the start-up light so ours shows
    c[42] = 0x00;                   // LED brightness: high
    c[43] = o.leds | 0x20;          // player LEDs, no fade
    c[44] = o.r, c[45] = o.g, c[46] = o.b;
    if (p.bluetooth) PutCrc(report, len);
    if (WriteReport(p, report, len) && p.bluetooth) p.fullReports = true;
}

void SendDualShock4(Pad& p, const Output& o) {
    uint8_t report[78] = {};
    DWORD len;
    int off;
    if (p.bluetooth) {
        report[0] = 0x11;
        report[1] = 0xC0;  // HID + CRC
        report[3] = 0x07;  // motors, light bar, flash
        off = 6;
        len = 78;
    } else {
        report[0] = 0x05;
        report[1] = 0x07;
        off = 4;
        len = 32;
    }
    report[off] = o.weak;
    report[off + 1] = o.strong;
    report[off + 2] = o.r, report[off + 3] = o.g, report[off + 4] = o.b;
    if (p.bluetooth) PutCrc(report, len);
    if (WriteReport(p, report, len) && p.bluetooth) p.fullReports = true;
}

// ---- Windows.Gaming.Input: Xbox One / Series trigger motors (WinRT entry points looked up at run time) ----
using RoInitializeFn = HRESULT(WINAPI*)(RO_INIT_TYPE);
using RoUninitializeFn = void(WINAPI*)();
using RoGetActivationFactoryFn = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
using WindowsCreateStringReferenceFn = HRESULT(WINAPI*)(PCWSTR, UINT32, HSTRING_HEADER*, HSTRING*);
RoGetActivationFactoryFn g_roGetFactory = nullptr;
WindowsCreateStringReferenceFn g_stringRef = nullptr;

template <class T>
HRESULT Factory(PCWSTR name, T** out) {
    HSTRING_HEADER header;
    HSTRING str = nullptr;
    if (!g_roGetFactory || !g_stringRef || FAILED(g_stringRef(name, static_cast<UINT32>(wcslen(name)), &header, &str)))
        return E_NOTIMPL;
    return g_roGetFactory(str, __uuidof(T), reinterpret_cast<void**>(out));
}

namespace wgi = ABI::Windows::Gaming::Input;
using Microsoft::WRL::ComPtr;
ComPtr<wgi::IGamepadStatics> g_gamepads;
ComPtr<wgi::IRawGameControllerStatics> g_raw;
ComPtr<wgi::IGamepad> g_xboxGamepads[4];
DWORD g_wgiCheck = 0;

void RefreshWgi() {
    if (GetTickCount() - g_wgiCheck < 2000 && g_wgiCheck) return;
    g_wgiCheck = GetTickCount();
    if (!g_gamepads &&
        (FAILED(Factory(RuntimeClass_Windows_Gaming_Input_Gamepad, g_gamepads.ReleaseAndGetAddressOf())) ||
         FAILED(Factory(RuntimeClass_Windows_Gaming_Input_RawGameController, g_raw.ReleaseAndGetAddressOf()))))
        return;
    for (auto& g : g_xboxGamepads) g.Reset();
    ComPtr<ABI::Windows::Foundation::Collections::IVectorView<wgi::Gamepad*>> list;
    unsigned count = 0;
    if (FAILED(g_gamepads->get_Gamepads(&list)) || FAILED(list->get_Size(&count))) return;
    int k = 0;
    for (unsigned i = 0; i < count && k < 4; ++i) {
        ComPtr<wgi::IGamepad> pad;
        ComPtr<wgi::IGameController> controller;
        ComPtr<wgi::IRawGameController> raw;
        UINT16 vid = 0;
        if (FAILED(list->GetAt(i, &pad)) || FAILED(pad.As(&controller)) ||
            FAILED(g_raw->FromGameController(controller.Get(), &raw)) || !raw || FAILED(raw->get_HardwareVendorId(&vid)))
            continue;
        if (vid == 0x045E) g_xboxGamepads[k++] = pad;
    }
}

void SendXbox(Pad& p, const Output& o) {
    const int user = XUser(p.xboxIndex);
    if (user >= 0 && g_xset) {
        XINPUT_VIBRATION v{static_cast<WORD>(o.strong * 257), static_cast<WORD>(o.weak * 257)};
        g_xset(user, &v);
    }
    RefreshWgi();
    if (p.xboxIndex >= 0 && p.xboxIndex < 4 && g_xboxGamepads[p.xboxIndex]) {
        wgi::GamepadVibration v{o.strong / 255.0, o.weak / 255.0, o.tmL / 255.0, o.tmR / 255.0};
        g_xboxGamepads[p.xboxIndex]->put_Vibration(v);
    }
}

void WorkerThread() {
    HMODULE combase = LoadLibraryA("combase.dll");
    auto roInit = combase ? reinterpret_cast<RoInitializeFn>(GetProcAddress(combase, "RoInitialize")) : nullptr;
    auto roUninit = combase ? reinterpret_cast<RoUninitializeFn>(GetProcAddress(combase, "RoUninitialize")) : nullptr;
    if (combase) {
        g_roGetFactory = reinterpret_cast<RoGetActivationFactoryFn>(GetProcAddress(combase, "RoGetActivationFactory"));
        g_stringRef = reinterpret_cast<WindowsCreateStringReferenceFn>(GetProcAddress(combase, "WindowsCreateStringReference"));
    }
    const HRESULT ro = roInit ? roInit(RO_INIT_MULTITHREADED) : E_NOTIMPL;
    while (!g_stop) {
        for (int j = 0; j < kMax; ++j) {
            Pad* p;
            Output o;
            {
                std::lock_guard<std::mutex> guard(g_padsLock);
                p = g_pads[j];
                if (!p || p->dead) continue;
                o = p->want;
                if (p->sentOnce && o == p->sent) continue;
            }
            switch (p->kind) {
                case Kind::DualSense: SendDualSense(*p, o); break;
                case Kind::DualShock4: SendDualShock4(*p, o); break;
                case Kind::XInput: SendXbox(*p, o); break;
                default: break;
            }
            p->sent = o;
            p->sentOnce = true;
        }
        Sleep(8);
    }
    if (SUCCEEDED(ro) && roUninit) roUninit();
}

// ---- pads per game joystick ----
void Release(Pad* p) {
    if (!p) return;
    p->dead = true;
    if (p->readHandle != INVALID_HANDLE_VALUE) {
        CancelIoEx(p->readHandle, nullptr);
        CloseHandle(p->readHandle);
    }
    if (p->reader.joinable()) p->reader.join();
    if (p->writeHandle != INVALID_HANDLE_VALUE) CloseHandle(p->writeHandle);
    delete p;
}

Pad* PadFor(int j) {
    if (j < 0 || j >= kMax || j >= *reinterpret_cast<int*>(kJoystickCount)) return nullptr;
    void* dev = reinterpret_cast<void**>(kJoysticks)[j];
    Pad* p = g_pads[j];
    if (p && p->device == dev) return p->dead ? nullptr : p;
    if (!dev) return nullptr;
    auto* fresh = new Pad;
    fresh->device = dev;
    fresh->kind = Identify(dev, fresh->path, fresh->bluetooth);
    if ((fresh->kind == Kind::DualSense || fresh->kind == Kind::DualShock4) && !fresh->path.empty()) {
        constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
        fresh->readHandle = CreateFileW(fresh->path.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_EXISTING, 0, nullptr);
        fresh->writeHandle = CreateFileW(fresh->path.c_str(), GENERIC_READ | GENERIC_WRITE, share, nullptr, OPEN_EXISTING, 0, nullptr);
        if (fresh->readHandle == INVALID_HANDLE_VALUE || fresh->writeHandle == INVALID_HANDLE_VALUE)
            dslog::Write("[fail] Pad I/O: HID device not opened (error %lu) - DirectInput only", GetLastError());
        else
            fresh->reader = std::thread(ReaderThread, fresh);
    }
    int k = 0;   // the k-th XInput joystick <-> the k-th connected XInput user / Xbox gamepad
    for (int i = 0; i < j; ++i)
        if (g_pads[i] && g_pads[i]->kind == Kind::XInput) ++k;
    if (fresh->kind == Kind::XInput) fresh->xboxIndex = k;
    {
        std::lock_guard<std::mutex> guard(g_padsLock);
        g_pads[j] = fresh;
    }
    Release(p);
    if (!g_workerStarted.exchange(true)) std::thread(WorkerThread).detach();
    return fresh;
}

LONG Scale(LONG raw) {  // +-32767 -> +-128 with the deadzone (percent)
    float v = raw / 32767.0f;
    const float dz = features::Get().padDeadzone / 100.0f, mag = std::fabs(v);
    if (mag <= dz || dz >= 1.0f) return 0;
    v = (v < 0 ? -1.0f : 1.0f) * std::fmin(1.0f, (mag - dz) / (1.0f - dz));
    return static_cast<LONG>(v * 128.0f);
}

void FillFrom(const Input& in, padio::JoyState& s) {
    memset(&s, 0, sizeof s);
    s.x = Scale(in.lx);
    s.y = Scale(in.ly);
    s.z = Scale(in.rx);
    s.rz = Scale(in.ry);
    for (DWORD& pov : s.pov) pov = 0xFFFFFFFF;
    for (int b = 0; b < 16; ++b)
        if (in.buttons & (1u << b)) s.buttons[b] = 0x80;
}

// The game's rumble for joystick j now (0..1 per motor), expiring finished entries like the poll does.
struct Motors {
    float strong = 0.0f, weak = 0.0f;
    float Max() const { return std::fmax(strong, weak); }
};
Motors Rumble(int j) {
    auto* e = reinterpret_cast<uint8_t*>(kRumbleTable + j * kRumbleStride);
    const DWORD start = *reinterpret_cast<DWORD*>(e + 4);
    if (!start || *reinterpret_cast<void**>(e)) return {};  // none, or the game plays it itself
    const DWORD t = timeGetTime() - start, duration = *reinterpret_cast<DWORD*>(e + 8);
    if (duration && t > duration) {
        *reinterpret_cast<DWORD*>(e + 4) = 0;
        *reinterpret_cast<DWORD*>(e + 0x10) = 0;
        return {};
    }
    float envelope = 1.0f;
    const int type = *reinterpret_cast<int*>(e + 0x14);
    if (type == 1 && duration) envelope = 1.0f - static_cast<float>(t) / duration;
    else if (type == 2) {
        const int period = *reinterpret_cast<int16_t*>(e + 0x18);
        if (period > 0) envelope = std::fabs(std::sin(3.14159265f * static_cast<float>(t % period) / period));
    }
    auto level = [&](int offset) {
        const float v = std::abs(*reinterpret_cast<int16_t*>(e + offset)) / 32768.0f * envelope;
        return std::fmin(1.0f, std::fmax(0.0f, v));
    };
    return {level(0xC), level(0xE)};
}

// Fire = game button 7 (R1 in default.key); with PadModernLayout it comes from the physical R2 / RT (button 5 here).
bool FireOnTrigger() { return features::Get().padModernLayout; }
constexpr int kFireTrigger = 5;
constexpr uint8_t kFireBreak = 0xBC, kFireRelease = 0x30;   // DS1's rifle feel

// In a mission the fire trigger only fires once pulled through the break point (DualSense with the adaptive trigger,
// Xbox pads always), released 0x30 below it.
void FullPull(Pad& p, Input& in) {
    if (!features::InMission() || !FireOnTrigger()) return;
    p.fireHeld = in.r2 >= kFireBreak || (p.fireHeld && in.r2 + kFireRelease >= kFireBreak);
    if (p.fireHeld) in.buttons |= 1u << kFireTrigger;
    else in.buttons &= ~(1u << kFireTrigger);
}

void GunTrigger(uint8_t (&t)[11]) {   // section resistance, firm point mid-pull (DS1 "rifle")
    memset(t, 0, sizeof t);
    t[0] = 0x02, t[1] = 0x60, t[2] = 0xB0, t[3] = 0xFF;
}
}  // namespace

bool padio::Fill(int joystick, JoyState& s) {
    if (!features::Get().controller) return false;
    Pad* p = PadFor(joystick);
    if (!p) return false;
    if (p->kind == Kind::XInput) {
        const int user = XUser(p->xboxIndex);
        XINPUT_STATE st;
        if (user < 0 || !g_xget || g_xget(user, &st) != ERROR_SUCCESS) return false;
        if (!p->haveInput) {
            p->haveInput = true;
            dslog::Write("Pad I/O: XInput joystick %d = XInput user %d", joystick, user);
        }
        const WORD w = st.Gamepad.wButtons;
        Input in{st.Gamepad.sThumbLX, static_cast<LONG>(-st.Gamepad.sThumbLY), st.Gamepad.sThumbRX,
                 static_cast<LONG>(-st.Gamepad.sThumbRY), st.Gamepad.bLeftTrigger, st.Gamepad.bRightTrigger, 0, GetTickCount()};
        const struct {
            WORD mask;
            int bit;
        } map[] = {{XINPUT_GAMEPAD_Y, 0}, {XINPUT_GAMEPAD_B, 1}, {XINPUT_GAMEPAD_A, 2}, {XINPUT_GAMEPAD_X, 3},
                   {XINPUT_GAMEPAD_LEFT_SHOULDER, 6}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, 7}, {XINPUT_GAMEPAD_BACK, 8},
                   {XINPUT_GAMEPAD_LEFT_THUMB, 9}, {XINPUT_GAMEPAD_RIGHT_THUMB, 10}, {XINPUT_GAMEPAD_START, 11},
                   {XINPUT_GAMEPAD_DPAD_UP, 12}, {XINPUT_GAMEPAD_DPAD_RIGHT, 13}, {XINPUT_GAMEPAD_DPAD_DOWN, 14},
                   {XINPUT_GAMEPAD_DPAD_LEFT, 15}};
        for (auto& m : map)
            if (w & m.mask) in.buttons |= 1u << m.bit;
        if (in.l2 > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) in.buttons |= 1u << 4;
        if (in.r2 > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) in.buttons |= 1u << 5;
        FullPull(*p, in);
        FillFrom(in, s);
        p->lastButtons = in.buttons;
        return true;
    }
    if (p->kind == Kind::DualSense || p->kind == Kind::DualShock4) {
        Input in;
        {
            std::lock_guard<std::mutex> guard(p->lock);
            if (!p->haveInput) return false;
            in = p->in;
        }
        // Over USB DirectInput keeps working; over Bluetooth ours is the only input once the pad sends full reports.
        if (GetTickCount() - in.time > 1000 && !p->fullReports) return false;
        if (p->kind != Kind::DualSense || features::Get().padTriggers) FullPull(*p, in);
        FillFrom(in, s);
        p->lastButtons = in.buttons;
        return true;
    }
    return false;
}

void padio::Forget(int joystick) {
    if (joystick < 0 || joystick >= kMax) return;
    Pad* p;
    {
        std::lock_guard<std::mutex> guard(g_padsLock);
        p = g_pads[joystick];
        g_pads[joystick] = nullptr;
    }
    Release(p);
}

void padio::Update() {
    const features::Settings& set = features::Get();
    if (!set.controller) return;
    const bool active = GetForegroundWindow() == *reinterpret_cast<HWND*>(kGameWindow);
    const bool mission = features::InMission();
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), kMax);
    for (int j = 0; j < count; ++j) {
        Pad* p = PadFor(j);
        if (!p || p->kind == Kind::Other) continue;
        Output o;
        const Motors m = active && set.padVibration ? Rumble(j) : Motors{};
        o.strong = static_cast<uint8_t>(m.strong * 255.0f);
        o.weak = static_cast<uint8_t>(m.weak * 255.0f);
        o.r = 0, o.g = 70, o.b = 255;   // player 1 blue
        o.leds = 0x04;                  // player LED 1
        if (mission && active && FireOnTrigger()) {
            if (p->kind == Kind::DualSense && set.padTriggers) {
                GunTrigger(o.trigR);
            } else if (p->kind == Kind::XInput && set.padVibration) {
                const bool pressed = p->lastButtons & (1u << kFireTrigger);
                static uint32_t prev[kMax];
                if (pressed && !(prev[j] & (1u << kFireTrigger))) p->firePulse = GetTickCount();
                prev[j] = p->lastButtons;
                const uint8_t kick = GetTickCount() - p->firePulse < 90 ? 160 : 0;
                const uint8_t shake = static_cast<uint8_t>(m.Max() * 90.0f);
                o.tmR = std::max(kick, shake);
                o.tmL = shake;
            }
        }
        std::lock_guard<std::mutex> guard(g_padsLock);
        p->want = o;
    }
}
