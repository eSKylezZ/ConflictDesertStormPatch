// The game follows the device in use (tgefix.ingameinput, with Controller on) - DesertStormFix's ingame_input.cpp for
// TGE:
//   - input mode: player 1's block devices (0x7DCAB0 +0x14 = [0x7DCAC4]: 1 keyboard / mouse, 2 pad - one or the other,
//     OPTIONS > CONTROLS > INPUT SELECTION's choice; the game tests the bits, and with both set the mouse-only aim scale
//     at 0x56DE3A would apply to the pad too) follows the last device used: a pad button / stick, or a key, mouse button
//     or mouse movement in the game window. INPUT SELECTION still stores its own choice (0x5833F1 writes the registry);
//     we never do.
//   - the menu cursor (drawn by 0x5CCAB0 when [0x66A590] is set) is hidden while a pad drives, and in the front end the
//     pointer reads far outside the window then (hover would beat the pad); cursor moves the game makes itself
//     (SetCursorPos) don't count as mouse input;
//   - menu prompts ("return: select" ...) name the pad's buttons while a pad drives (catalog hashes below; the key file's
//     MENU_FORWARD = button 2, MENU_BACK = button 0, PAUSE = 11, OBJECTIVES = 8);
//   - key names (0x5B0F80, ecx = binding: type << 16 | code; 1 pad axis (3 codes per axis), 2 pad button) say "cross",
//     "r2", "left stick" ... instead of "joy 3" (PlayStation or Xbox names, the modern swap applied);
//   - tutorial texts (script API +0x3D0 = 0x5A54C0: the action's first binding) took the first keyboard / mouse binding
//     only (pad types skipped at 0x5A551C) - now the first binding of the device in use;
//   - player 1's pad pulled out in a mission pauses the game (the PAUSE read at 0x58FD98 answers "pressed" once) with a
//     notice until the pad is back or a key is pressed (hotplug.cpp -> PlayerPadChanged).
#include <windows.h>

#include <algorithm>
#include <cstdint>

#include "core/log.h"
#include "core/patch.h"
#include "features/features.h"
#include "features/overlay.h"
#include "core/pse_host.h"
#include "loader.h"

namespace {
constexpr uint32_t kBlockDevices = 0x7DCAC4;   // player 1's block +0x14
constexpr uint32_t kBlockJoystick = 0x7DCAB0 + 0x2E0;
constexpr uint32_t kJoysticks = 0x7A9D24, kJoystickCount = 0x7AA888;
constexpr uint32_t kButtonStates = 0x7A9DE0, kAxes = 0x7A9E98, kJoyStride = 0x150;
constexpr uint32_t kGameWindow = 0x7DC238;
constexpr uint32_t kCursorDraw = 0x5CCAB0;
constexpr uint8_t kCursorDrawOriginal[] = {0xA1, 0x90, 0xA5, 0x66, 0x00};   // mov eax, [0x66A590]
constexpr uint32_t kCursorFlag = 0x66A590;
constexpr uint32_t kPauseRead = 0x58FD98;
constexpr uint8_t kPauseReadOriginal[] = {0xA0, 0x14, 0xCB, 0x7D, 0x00};    // mov al, [0x7DCB14]
constexpr uint32_t kPausedFlag = 0x7DC068;
constexpr uint32_t kTutorialSkip = 0x5A551C;
constexpr uint8_t kTutorialSkipOriginal[] = {0x83, 0xFA, 0x01, 0x74, 0x2E, 0x83, 0xFA, 0x02, 0x74, 0x29};
constexpr uint32_t kBindingName = 0x5B0F80;
constexpr uint8_t kBindingNamePrologue[] = {0x83, 0xF9, 0xFF, 0x75, 0x13};   // cmp ecx, -1; jne
constexpr uint32_t kNameBuffer = 0x7E1830;
constexpr uint32_t kTextLookup = 0x47EA90;
constexpr uint8_t kTextLookupPrologue[] = {0x8B, 0x41, 0x38, 0x85, 0xC0};

bool g_padDriving = false;   // the last input came from a pad
bool g_padLost = false;      // player 1's pad pulled out while it drove
volatile LONG g_pauseRequest = 0;

bool PadMode() { return (*reinterpret_cast<const uint32_t*>(kBlockDevices) & 3) == 2; }

bool GameFocused() {
    const HWND game = *reinterpret_cast<HWND*>(kGameWindow);
    return game && GetForegroundWindow() == game;
}

// ---- which device is in use ---------------------------------------------------------------------------------------
bool PadActivity() {
    const int count = std::min(*reinterpret_cast<int*>(kJoystickCount), 8);
    for (int j = 0; j < count; ++j) {
        if (!reinterpret_cast<void**>(kJoysticks)[j]) continue;
        const auto* buttons = reinterpret_cast<const uint32_t*>(kButtonStates + j * kJoyStride);
        for (int b = 0; b < 16; ++b)
            if (buttons[b] & 4) return true;
        const auto* axes = reinterpret_cast<const float*>(kAxes + j * kJoyStride);
        for (int a = 0; a < 4; ++a)
            if (axes[a * 3] > 0.5f || axes[a * 3] < -0.5f) return true;
    }
    return false;
}

// The game moves the cursor itself (SetCursorPos import 0x5F5244, called at 0x4B81DC): that move isn't the player's.
volatile LONG g_gameMoved = 0;
POINT g_gameMovedTo{};
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
SetCursorPosFn g_setCursorPos = nullptr;
BOOL WINAPI GameSetCursorPos(int x, int y) {
    g_gameMovedTo = {x, y};
    InterlockedExchange(&g_gameMoved, 1);
    return g_setCursorPos(x, y);
}
// While a pad drives the front end, the GUI's pointer (GetCursorPos import 0x5F5250, the mouse poll loads it at
// 0x4B91FF) reads far outside the window: a hidden pointer resting on a button would win over the pad's up / down.
// Not in missions (mouse look may read it).
using GetCursorPosFn = BOOL(WINAPI*)(POINT*);
GetCursorPosFn g_getCursorPos = nullptr;
BOOL WINAPI GameGetCursorPos(POINT* p) {
    const BOOL ok = g_getCursorPos(p);
    if (ok && p && g_padDriving && PadMode() && !features::InMission()) p->x = p->y = -30000;
    return ok;
}

bool KeyboardMouseActivity() {
    static bool down[256] = {};
    static POINT last{-100000, -100000};
    if (InterlockedExchange(&g_gameMoved, 0) && last.x != -100000) last = g_gameMovedTo;
    bool active = false;
    for (int vk = 1; vk < 256; ++vk) {
        if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU) continue;   // the L / R ones are asked too
        const bool d = (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (d && !down[vk]) active = true;
        down[vk] = d;
    }
    POINT p;
    if (GetCursorPos(&p)) {
        if (last.x != -100000 && (std::abs(p.x - last.x) > 8 || std::abs(p.y - last.y) > 8)) active = true;
        last = p;
    }
    return active;
}

void SetMode(bool pad) {
    auto& devices = *reinterpret_cast<uint32_t*>(kBlockDevices);
    if ((devices & 3) != 1 && (devices & 3) != 2) return;   // something else: leave it
    const uint32_t want = (devices & ~3u) | (pad ? 2u : 1u);
    if (devices == want) return;
    devices = want;
    dslog::Write("Input: player 1 now uses the %s", pad ? "pad" : "keyboard and mouse");
}

int PSE_CALL OnFrame(PSEEventId, void*, void*) {
    if (!features::Get().controller) return 0;
    const bool focused = GameFocused();
    const bool pad = PadActivity();
    const bool kbm = focused && KeyboardMouseActivity();   // read every frame so held keys don't count later
    if (*reinterpret_cast<int*>(kJoystickCount) <= 0 && !g_padLost) return 0;
    // Keyboard / mouse within half a second of pad use is the pad's own doing (Steam Input's desktop layout sends
    // keys and mouse moves for pad presses), not the player switching.
    static DWORD lastPad = 0;
    const DWORD now = GetTickCount();
    if (pad) lastPad = now;
    if (pad) {
        g_padDriving = true;
        SetMode(true);
    } else if (kbm && now - lastPad > 500) {
        g_padDriving = false;
        g_padLost = false;
        SetMode(false);
    }
    return 0;
}

// ---- cursor ---------------------------------------------------------------------------------------------------------
extern "C" uint32_t __cdecl CursorFlag() {
    return g_padDriving && PadMode() ? 0 : *reinterpret_cast<const uint32_t*>(kCursorFlag);
}
__declspec(naked) void CursorStub() {
    __asm {
        push ecx
        push edx
        call CursorFlag
        pop edx
        pop ecx
        ret
    }
}

// ---- pause when the pad goes ------------------------------------------------------------------------------------------
__declspec(naked) void PauseStub() {
    __asm {
        mov al, byte ptr ds:[0x7DCB14]
        cmp g_pauseRequest, 0
        je done
        or al, 2
        mov g_pauseRequest, 0
    done:
        ret
    }
}

void LostNotice() {
    if (!g_padLost) return;
    float w, h;
    overlay::ScreenSize(w, h);
    const float size = h / 22;
    overlay::QueueRect(0, h * 0.40f, w, size * 3.4f, 0xB0000000);
    overlay::QueueText("CONTROLLER DISCONNECTED", w / 2, h * 0.40f + size * 0.4f, size, 0xFFFFFFFF, 1);
    overlay::QueueText("Reconnect it, or press a key to play with the keyboard", w / 2, h * 0.40f + size * 1.8f,
                       size * 0.7f, 0xFFD8D8D8, 1, false);
}

// ---- names ----------------------------------------------------------------------------------------------------------
int Physical(int game) {   // controller.cpp's PadModernLayout swap (game 4 <-> 6, 5 <-> 7)
    if (!features::Get().padModernLayout) return game;
    switch (game) {
    case 4: return 6;
    case 5: return 7;
    case 6: return 4;
    case 7: return 5;
    }
    return game;
}
const wchar_t* ButtonName(int game) {
    static const wchar_t* const ps[16] = {L"triangle", L"circle", L"cross", L"square", L"l2", L"r2", L"l1", L"r1",
                                          L"share", L"l3", L"r3", L"options", L"d-pad up", L"d-pad right",
                                          L"d-pad down", L"d-pad left"};
    static const wchar_t* const xb[16] = {L"y", L"b", L"a", L"x", L"lt", L"rt", L"lb", L"rb", L"view", L"ls", L"rs",
                                          L"menu", L"d-pad up", L"d-pad right", L"d-pad down", L"d-pad left"};
    if (game < 0 || game > 15) return nullptr;
    const int p = Physical(game);
    return features::PadInUseIsXbox() ? xb[p] : ps[p];
}

using BindingNameFn = const wchar_t*(__fastcall*)(uint32_t, void*, uint32_t);
BindingNameFn g_bindingName = nullptr;
const wchar_t* __fastcall BindingName(uint32_t binding, void* edx, uint32_t arg) {
    const wchar_t* name = g_bindingName(binding, edx, arg);
    if (binding == 0xFFFFFFFF) return name;
    const uint32_t type = binding >> 16, code = binding & 0xFFFF;
    const wchar_t* friendly = nullptr;
    if (type == 2) friendly = ButtonName(static_cast<int>(code));
    else if (type == 1) friendly = code / 3 < 2 ? L"left stick" : code / 3 < 4 ? L"right stick" : nullptr;
    if (!friendly) return name;
    auto* out = reinterpret_cast<wchar_t*>(kNameBuffer);
    wcsncpy_s(out, 20, friendly, _TRUNCATE);
    return out;
}

// Tutorial texts: skip the bindings of the other device (edx = binding type); jumped to from 0x5A551C, back to the
// name call (0x5A5526) or the next binding (0x5A554F).
extern "C" int __cdecl SkipBinding(uint32_t type) {
    const bool pad = PadMode();
    return pad ? (type == 0 || type == 4) : (type == 1 || type == 2);
}
__declspec(naked) void TutorialStub() {
    __asm {
        push eax
        push ecx
        push edx
        call SkipBinding
        add esp, 4
        mov edx, eax
        pop ecx
        pop eax
        test edx, edx
        jnz skip
        push 0x5A5526
        ret
    skip:
        push 0x5A554F
        ret
    }
}

// Menu prompts: catalog hash -> what it says with a pad (PlayStation, Xbox).
struct Prompt {
    uint32_t hash;
    const wchar_t *ps, *xb;
};
constexpr Prompt kPrompts[] = {
    {0x240A9A71, L"triangle: back", L"y: back"},
    {0x6A0B539E, L"cross: select", L"a: select"},
    {0x54C77EA1, L"triangle: resume", L"y: resume"},
    {0x1471D8FC, L"triangle: cancel", L"y: cancel"},
    {0x9659879E, L"left/right: select", L"left/right: select"},
    {0xDB41FDEB, L"cross: accept", L"a: accept"},
    {0x512515D0, L"triangle: continue", L"y: continue"},
    {0x77BD7137, L"triangle: return to mission", L"y: return to mission"},
    {0xDC92997B, L"share: return to game", L"view: return to game"},
};

using TextLookupFn = const wchar_t*(__thiscall*)(void*, uint32_t);
TextLookupFn g_textLookup = nullptr;
const wchar_t* __fastcall TextLookup(void* catalog, void*, uint32_t hash) {
    if (g_padDriving && PadMode())
        for (const Prompt& p : kPrompts)
            if (p.hash == hash) return features::PadInUseIsXbox() ? p.xb : p.ps;
    return g_textLookup(catalog, hash);
}
}  // namespace

void features::PlayerPadChanged(int joystick, bool connected) {
    if (joystick != *reinterpret_cast<const int32_t*>(kBlockJoystick)) return;
    if (connected) {
        if (g_padLost) dslog::Write("Input: player 1's pad is back");
        g_padLost = false;
        return;
    }
    if (!Get().controller || !PadMode() || !InMission()) return;
    g_padLost = true;
    if (!*reinterpret_cast<const int32_t*>(kPausedFlag)) InterlockedExchange(&g_pauseRequest, 1);
    dslog::Write("Input: player 1's pad was pulled out - game paused");
}

void features::ApplyInGameInput() {
    static bool done = false;
    if (done || !Get().controller) return;
    done = true;
    bool ok = patch::WriteCall(kCursorDraw, reinterpret_cast<const void*>(&CursorStub), 5, kCursorDrawOriginal);
    ok = patch::WriteCall(kPauseRead, reinterpret_cast<const void*>(&PauseStub), 5, kPauseReadOriginal) && ok;
    ok = patch::WriteJump(kTutorialSkip, reinterpret_cast<const void*>(&TutorialStub), sizeof kTutorialSkipOriginal,
                          kTutorialSkipOriginal) && ok;
    ok = patch::HookFunction(kBindingName, reinterpret_cast<const void*>(&BindingName),
                             reinterpret_cast<void**>(&g_bindingName), kBindingNamePrologue, sizeof kBindingNamePrologue) && ok;
    ok = patch::HookFunction(kTextLookup, reinterpret_cast<const void*>(&TextLookup), reinterpret_cast<void**>(&g_textLookup),
                             kTextLookupPrologue, sizeof kTextLookupPrologue) && ok;
    ok = patch::HookSlot(0x5F5244, reinterpret_cast<const void*>(&GameSetCursorPos), reinterpret_cast<void**>(&g_setCursorPos)) && ok;
    ok = patch::HookSlot(0x5F5250, reinterpret_cast<const void*>(&GameGetCursorPos), reinterpret_cast<void**>(&g_getCursorPos)) && ok;
    if (const PSEMessagingInterface* m = pse::MessagingInterface())
        m->Subscribe(pse_host::Find("tgefix.ingameinput"), PSE_EVENT_FRAME, &OnFrame, nullptr, 0);
    overlay::Install();
    overlay::AddLateDraw(&LostNotice);
    if (ok) dslog::Write("[ok]   In-game input: follows the device in use (input mode, cursor, prompts, key names)");
    else ModLog("[fail] In-game input: some game code not recognised (see the log)");
}
