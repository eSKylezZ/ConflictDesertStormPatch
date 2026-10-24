// Direct pad I/O (see padio.h).
//
// The game only knows DirectInput joysticks (IDirectInputDevice8A* at 0x754BC8[j], count 0x754BE8). Each is
// identified once: DIPROP_GUIDANDPATH gives the HID device path ("IG_" = an XInput pad), GetDeviceInfo the
// vendor / product id (054C:0CE6/0DF2 DualSense, 054C:05C4/09CC/0BA0 DualShock 4); Bluetooth when the path carries the
// HID-over-Bluetooth service {00001124-...}.
//
// Input
//   XInput pads: XInputGetState of the k-th connected XInput user for the k-th XInput joystick (neither API names
//     the other's devices; order is the best match) - triggers separate (DirectInput sums them on one axis).
//   DualSense / DualShock 4: a reader thread per pad reads its HID input reports: USB 0x01 (64 bytes), Bluetooth
//     "simple" 0x01 before any output report was sent and the full 0x31 (DualSense) / 0x11 (DualShock 4) after it.
//     Sending output reports over Bluetooth switches the pad to the full report, which DirectInput can't read -
//     that is why the input is read here too. Touchpad click = Create (Objectives).
// Output (worker thread, sends only changes)
//   Rumble: the game's force-feedback table 0x753058 + j*0x1c (FUN_005392d0 fills it: +0 DI effect, +4 start ms,
//     +8 duration ms (0 = endless), +0xc/+0xe strength x/y (+-32767), +0x10 priority, +0x14 type 1 fade / 2 pulse,
//     +0x18 period) is only played by FUN_00538ed0 for pads with a DirectInput effect (none of ours); its envelope is
//     reproduced here and expired entries are cleared like the poll does. XInput motors / DualSense "compatible
//     vibration" / DualShock 4 motors.
//   Xbox One / Series trigger motors: Windows.Gaming.Input Gamepad.Vibration (k-th Microsoft-vendor gamepad).
//   Light bar + player LEDs (DualSense) / light bar (DualShock 4): the pad's player (co-op) colour, PS5 style.
//   Adaptive triggers (DualSense, in missions): section resistance ("gun trigger") on whichever trigger fires in the
//     player's layout, light resistance on a trigger used for aim mode.
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
#include "core/settings.h"
#include "features/features.h"
#include "features/padlayout.h"

namespace {
constexpr uint32_t kJoysticks = 0x754BC8, kJoystickCount = 0x754BE8, kRumbleTable = 0x753058, kRumbleStride = 0x1C;
constexpr uint32_t kLevelName = 0x606880, kGameWindow = 0x606A60;
constexpr int kMax = 8;

enum class Kind { Other, XInput, DualSense, DualShock4 };

struct Input {
    LONG lx, ly, rx, ry;  // raw -1..1 scaled to +-32767 (Y down positive, like DirectInput)
    uint8_t l2, r2;
    uint32_t buttons;  // game numbering bits 0-15
    DWORD time;
    int16_t gyro[3];   // DualSense / DualShock 4 raw gyro (pitch, yaw, roll), 0 when the report has none
    bool hasGyro;
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
    // input (reader thread for HID pads)
    std::mutex lock;
    Input in{};
    bool haveInput = false;
    bool fullReports = false;  // Bluetooth: output sent, pad now sends the full report
    HANDLE readHandle = INVALID_HANDLE_VALUE, writeHandle = INVALID_HANDLE_VALUE;
    std::thread reader;
    std::atomic<bool> dead{false};
    // output
    Output want, sent;
    bool sentOnce = false;
    uint8_t seq = 0;
    int xboxIndex = -1;  // k-th XInput joystick
    uint32_t lastButtons = 0;
    DWORD firePulse = 0;
    bool fireHeld = false;  // DualSense fire trigger past its break point (released again with some hysteresis)
    // gyro drift (reader thread): bias learnt while the pad lies still
    float bias[3] = {};
    DWORD stillSince = 0;
};
Pad* g_pads[kMax] = {};
std::mutex g_padsLock;
std::atomic<bool> g_workerStarted{false};  // the worker is detached: a joinable std::thread left at exit = abort()
std::atomic<bool> g_stop{false};

// ---- XInput (loaded dynamically, like core/gamepad) ----
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
    // offset of LX, of the three button bytes and of L2/R2
    int ax, bt, tr;
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
    // Gyro (le16 x 3): DualSense 15 bytes after LX, DualShock 4 12 - not in the short Bluetooth report.
    const int gy = ax + (p.kind == Kind::DualSense ? 15 : 12);
    in.hasGyro = !(r[0] == 0x01 && n < 64) && static_cast<DWORD>(gy + 6) <= n;
    for (int i = 0; i < 3; ++i) in.gyro[i] = in.hasGyro ? static_cast<int16_t>(r[gy + 2 * i] | r[gy + 2 * i + 1] << 8) : 0;
    return true;
}

// Raw gyro -> degrees per second (both pads: about 16.4 units per deg/s), drift removed. The bias follows the readings
// while all three axes stay near it for half a second (pad lying still or held very steady).
void LearnBias(Pad& p, const Input& in) {
    if (!in.hasGyro) return;
    bool still = true;
    for (int i = 0; i < 3; ++i) still &= std::fabs(in.gyro[i] - p.bias[i]) < 40.0f;
    if (!still) {
        p.stillSince = 0;
        return;
    }
    if (!p.stillSince) p.stillSince = in.time ? in.time : 1;
    if (in.time - p.stillSince < 500) return;
    for (int i = 0; i < 3; ++i) p.bias[i] += (in.gyro[i] - p.bias[i]) * 0.02f;
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
            LearnBias(*p, in);
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

// ---- Windows.Gaming.Input: Xbox One / Series trigger motors ----
// Windows 10+ only: the WinRT entry points are looked up at run time (combase.dll), so the DLL still loads elsewhere.
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
    if (p && p->device == dev) return p->dead ? nullptr : p;  // dead = unplugged: hotplug.cpp swaps the device
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
    // the k-th XInput joystick <-> the k-th connected XInput user / Xbox gamepad
    int k = 0;
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

LONG Scale(LONG raw) {  // +-32767 -> +-128 with the game's deadzone (percent)
    float v = raw / 32767.0f;
    const float dz = settings::Get().padDeadzone / 100.0f, mag = std::fabs(v);
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

// The game's rumble for joystick j now (0..1 per motor: +0xC strong / low-frequency, +0xE weak / high-frequency),
// expiring finished entries like FUN_00538ed0 does.
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

bool InMission() {
    const char* level = reinterpret_cast<const char*>(kLevelName);
    return level[0] && _strnicmp(level, "frontend", 8) != 0;
}

// DualSense trigger effects (legacy modes, work on every firmware): 0x01 continuous {start, force},
// 0x02 section {start, end, force}. By what the player fires (rumble.cpp).
void GunTrigger(uint8_t (&t)[11], features::TriggerFeel feel) {
    using features::TriggerFeel;
    memset(t, 0, sizeof t);
    switch (feel) {
        case TriggerFeel::None: break;  // grenades, C4, knife, fist: a free trigger
        case TriggerFeel::Light: t[0] = 0x02, t[1] = 0x70, t[2] = 0xA8, t[3] = 0x90; break;  // pistols, SMGs
        case TriggerFeel::Heavy: t[0] = 0x02, t[1] = 0x50, t[2] = 0xC0, t[3] = 0xFF; break;  // snipers, shotguns
        case TriggerFeel::MachineGun: t[0] = 0x01, t[1] = 0x40, t[2] = 0xA0; break;           // steady weight
        case TriggerFeel::Launcher: t[0] = 0x01, t[1] = 0x20, t[2] = 0xFF; break;             // heavy all the way
        default: t[0] = 0x02, t[1] = 0x60, t[2] = 0xB0, t[3] = 0xFF; break;  // rifles: firm point mid-pull
    }
}
// Where the shot breaks (0-255 trigger travel): just past the effect's resistance, so a tap into the resistance
// doesn't fire - the trigger has to be pulled through it. 0 = the game's usual light press.
uint8_t FireBreak(features::TriggerFeel feel) {
    using features::TriggerFeel;
    switch (feel) {
        case TriggerFeel::None: return 0;
        case TriggerFeel::Light: return 0xB4;       // section ends 0xA8
        case TriggerFeel::Heavy: return 0xCC;       // section ends 0xC0
        case TriggerFeel::MachineGun: return 0xB8;  // continuous from 0x40
        case TriggerFeel::Launcher: return 0xD8;    // continuous from 0x20
        default: return 0xBC;                       // rifles: section ends 0xB0
    }
}
constexpr uint8_t kFireRelease = 0x30;  // hysteresis below the break point (automatic fire doesn't stutter)

// In a mission the fire trigger (analog, 0-255) only fires once pulled through the weapon's break point - on the
// DualSense past its resistance (when adaptive triggers are on), on Xbox pads at the same travel.
void FullPull(Pad& p, Input& in, int joystick) {
    if (!InMission()) return;
    const int player = std::clamp(features::PlayerOfJoystick(joystick), 0, 3);
    const int fire = padlayout::ButtonOf(padlayout::Get(player), 0);  // FIRE
    const uint8_t brk = FireBreak(features::PlayerTriggerFeel(player));
    if ((fire != 4 && fire != 5) || !brk) return;
    const uint8_t pull = fire == 4 ? in.l2 : in.r2;
    p.fireHeld = pull >= brk || (p.fireHeld && pull + kFireRelease >= brk);
    if (p.fireHeld) in.buttons |= 1u << fire;
    else in.buttons &= ~(1u << fire);
}

// Xbox trigger motor kick on each pull, by the same feel (0 = none).
uint8_t Kick(features::TriggerFeel feel) {
    using features::TriggerFeel;
    switch (feel) {
        case TriggerFeel::None: return 0;
        case TriggerFeel::Light: return 110;
        case TriggerFeel::Heavy: case TriggerFeel::Launcher: return 230;
        case TriggerFeel::MachineGun: return 180;
        default: return 160;
    }
}
void AimTrigger(uint8_t (&t)[11]) {
    memset(t, 0, sizeof t);
    t[0] = 0x01, t[1] = 0x20, t[2] = 0x60;  // continuous light resistance
}

constexpr uint8_t kColours[4][3] = {{0, 70, 255}, {255, 20, 20}, {20, 220, 40}, {230, 40, 200}};

// Health of the soldier a player controls (input block +0x310 -> soldier +0x54, float; 150 for Bradley at the start
// of Mission 1 - the highest value seen per soldier counts as full), -1 = none.
// Only for a player in this level whose block's soldier points back at the block (soldier +0x23C0) - a block left over
// from the last level, or one a level load is still setting up, can hold a freed soldier (crashed here once).
float SoldierHealth(const uint8_t* block) {
    __try {
        auto* soldier = *reinterpret_cast<uint8_t* const*>(block + 0x310);
        if (!soldier || *reinterpret_cast<const uint8_t* const*>(soldier + 0x23C0) != block) return -1.0f;
        return *reinterpret_cast<const float*>(soldier + 0x54);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}
float PlayerHealth(int player) {
    if (player >= std::max(1, features::SplitScreenPlayers())) return -1.0f;
    const auto* block = reinterpret_cast<const uint8_t*>(0x60F5B8 + player * 0x478);
    const float health = SoldierHealth(block);
    if (health < 0.0f) return -1.0f;
    const void* soldier = *reinterpret_cast<void* const*>(block + 0x310);
    static const void* seen[4];
    static float full[4];
    if (seen[player] != soldier || health > full[player]) seen[player] = soldier, full[player] = std::fmax(health, 1.0f);
    return std::fmax(0.0f, health / full[player]);
}

// Light bar by health: green, yellow at half, red when low; pulses below a quarter, slow red pulse when down.
void HealthColour(float f, uint8_t& r, uint8_t& g, uint8_t& b) {
    const float t = std::fmin(1.0f, std::fmax(0.0f, f));
    float red = t > 0.5f ? (1.0f - t) * 2.0f : 1.0f, green = t > 0.5f ? 1.0f : t * 2.0f;
    float level = 1.0f;
    if (t <= 0.0f) level = 0.25f + 0.75f * (0.5f + 0.5f * std::sin(timeGetTime() * 0.004f));
    else if (t < 0.25f) level = 0.55f + 0.45f * (0.5f + 0.5f * std::sin(timeGetTime() * 0.012f));
    r = static_cast<uint8_t>(255.0f * red * level);
    g = static_cast<uint8_t>(200.0f * green * level);
    b = 0;
}
constexpr uint8_t kPlayerLeds[4] = {0x04, 0x0A, 0x15, 0x1B};
}  // namespace

bool padio::Fill(int joystick, JoyState& s) {
    if (!settings::Get().controller) return false;
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
        FullPull(*p, in, joystick);
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
        if (p->kind != Kind::DualSense || padlayout::AdaptiveTriggers(std::clamp(features::PlayerOfJoystick(joystick), 0, 3)))
            FullPull(*p, in, joystick);
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
    if (!settings::Get().controller) return;
    const bool active = GetForegroundWindow() == *reinterpret_cast<HWND*>(kGameWindow);
    const bool mission = InMission();
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), kMax);
    for (int j = 0; j < count; ++j) {
        Pad* p = PadFor(j);
        if (!p || p->kind == Kind::Other) continue;
        const int player = std::clamp(features::PlayerOfJoystick(j), 0, 3);
        Output o;
        const bool vibration = padlayout::Vibration(player);
        const Motors m = active && vibration ? Rumble(j) : Motors{};
        const float level = m.Max();
        o.strong = static_cast<uint8_t>(m.strong * 255.0f);
        o.weak = static_cast<uint8_t>(m.weak * 255.0f);
        o.r = kColours[player][0], o.g = kColours[player][1], o.b = kColours[player][2];
        o.leds = kPlayerLeds[player];
        // In a mission the light bar shows the soldier's health (the DualSense's player LEDs still give the player;
        // a DualShock 4 has none, so in co-op it keeps the player colour).
        if (mission && (p->kind == Kind::DualSense || features::CoopPlayers() < 2)) {
            const float health = PlayerHealth(player);
            if (health >= 0.0f) HealthColour(health, o.r, o.g, o.b);
        }
        if (mission && active) {
            const padlayout::Layout& layout = padlayout::Get(player);
            const int fire = padlayout::ButtonOf(layout, 0);   // FIRE
            const int aim = padlayout::ButtonOf(layout, 20);   // HEAD_TOGGLE (aim mode)
            const features::TriggerFeel feel = features::PlayerTriggerFeel(player);
#ifndef DS_DIST
            static int lastFeel[4] = {-1, -1, -1, -1};
            if (lastFeel[player] != static_cast<int>(feel)) {
                lastFeel[player] = static_cast<int>(feel);
                static const char* const kNames[] = {"default", "none", "light", "rifle", "heavy", "machine gun", "launcher"};
                dslog::Write("Pad I/O: player %d trigger feel -> %s", player + 1, kNames[lastFeel[player]]);
            }
#endif
            if (p->kind == Kind::DualSense && padlayout::AdaptiveTriggers(player)) {
                if (aim == 4 && fire != 4) AimTrigger(o.trigL);
                if (aim == 5 && fire != 5) AimTrigger(o.trigR);
                if (fire == 4) GunTrigger(o.trigL, feel);
                if (fire == 5) GunTrigger(o.trigR, feel);
            } else if (p->kind == Kind::XInput && vibration && (fire == 4 || fire == 5)) {
                // Xbox trigger motors: a kick when the fire trigger is pulled (sized by the weapon), plus some of the
                // game's rumble.
                const bool pressed = p->lastButtons & (1u << fire);
                static uint32_t prev[kMax];
                if (pressed && !(prev[j] & (1u << fire))) p->firePulse = GetTickCount();
                prev[j] = p->lastButtons;
                uint8_t kick = GetTickCount() - p->firePulse < 90 ? Kick(feel) : 0;
                const uint8_t shake = static_cast<uint8_t>(level * 90.0f);
                (fire == 4 ? o.tmL : o.tmR) = std::max(kick, shake);
                (fire == 4 ? o.tmR : o.tmL) = shake;
            }
        }
        std::lock_guard<std::mutex> guard(g_padsLock);
        p->want = o;
    }
}

bool padio::Gyro(int joystick, float& yawRight, float& pitchDown) {
    yawRight = pitchDown = 0.0f;
    if (!settings::Get().controller || joystick < 0 || joystick >= kMax) return false;
    Pad* p = g_pads[joystick];
    if (!p || p->dead || (p->kind != Kind::DualSense && p->kind != Kind::DualShock4)) return false;
    Input in;
    float bias[3];
    {
        std::lock_guard<std::mutex> guard(p->lock);
        if (!p->haveInput || !p->in.hasGyro || GetTickCount() - p->in.time > 200) return false;
        in = p->in;
        memcpy(bias, p->bias, sizeof bias);
    }
    constexpr float kUnitsPerDps = 16.4f, kDeadDps = 0.8f;
    auto rate = [&](int i) {
        const float v = (in.gyro[i] - bias[i]) / kUnitsPerDps;
        return std::fabs(v) < kDeadDps ? 0.0f : v;
    };
    // Raw x = pitch (nose up +), y = yaw (to the left +), as SDL reads these pads. Not yet checked on a real pad.
    yawRight = -rate(1);
    pitchDown = -rate(0);
    return true;
}
