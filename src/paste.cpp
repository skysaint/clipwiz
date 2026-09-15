// paste.cpp
#include "paste.h"

#include <cstring>

namespace paste {
namespace {

HWINEVENTHOOK g_hook = nullptr;
HWND g_target = nullptr;

// Desktop, taskbar, start menu are not valid paste targets; recording them would interfere
bool IsShellWindow(HWND hwnd) {
    static const wchar_t* kSkip[] = {
        L"Shell_TrayWnd",
        L"Shell_SecondaryTrayWnd",
        L"Progman",
        L"WorkerW",
        L"NotifyIconOverflowWindow",
        L"TopLevelWindowForOverflowXamlIsland",
        L"Windows.UI.Core.CoreWindow",
        L"XamlExplorerHostIslandWindow",
        L"MultitaskingViewFrame",
        L"ForegroundStaging",
    };
    wchar_t cls[128] = {0};
    if (GetClassNameW(hwnd, cls, 128) == 0) {
        return true;
    }
    for (const wchar_t* name : kSkip) {
        if (wcscmp(cls, name) == 0) {
            return true;
        }
    }
    return false;
}

bool IsOwnWindow(HWND hwnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    return pid == GetCurrentProcessId();
}

bool IsUsableTarget(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd) || !IsWindowVisible(hwnd)) {
        return false;
    }
    if (IsOwnWindow(hwnd) || IsShellWindow(hwnd)) {
        return false;
    }
    return true;
}

void CALLBACK ForegroundProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG objectId,
                             LONG childId, DWORD threadId, DWORD timeMs) {
    (void)hook;
    (void)objectId;
    (void)childId;
    (void)threadId;
    (void)timeMs;
    if (event != EVENT_SYSTEM_FOREGROUND) {
        return;
    }
    if (IsUsableTarget(hwnd)) {
        g_target = hwnd;
    }
}

// SetForegroundWindow gets rejected when we're not the foreground process;
// attach to target's thread input queue to bypass this limitation.
bool ForceForeground(HWND hwnd) {
    if (GetForegroundWindow() == hwnd) {
        return true;
    }
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    }
    if (SetForegroundWindow(hwnd)) {
        return true;
    }
    const DWORD ours = GetCurrentThreadId();
    const DWORD theirs = GetWindowThreadProcessId(hwnd, nullptr);
    if (theirs == 0 || theirs == ours) {
        return false;
    }
    bool ok = false;
    if (AttachThreadInput(ours, theirs, TRUE)) {
        SetForegroundWindow(hwnd);
        BringWindowToTop(hwnd);
        ok = GetForegroundWindow() == hwnd;
        AttachThreadInput(ours, theirs, FALSE);
    }
    return ok || GetForegroundWindow() == hwnd;
}

void SendKey(WORD vk, bool down) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vk;
    input.ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    input.ki.dwFlags = down ? 0u : static_cast<DWORD>(KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(input));
}

// Hotkey is triggered with modifier keys held; Ctrl/Alt/Shift/Win are still physically down.
// Must release them first, otherwise Ctrl+V would become Ctrl+Alt+V or similar.
//
// This deliberately stays virtual-key based while the paste chord below is
// scan-code based: the job here is to lift a modifier the user is physically
// holding, and only the VK identifies which one. A scan code cannot tell left
// from right without the extended flag, and getting that wrong would leave a
// modifier stuck down.
void ReleaseHeldModifiers() {
    static const WORD kMods[] = {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
                                 VK_LSHIFT,   VK_RSHIFT,   VK_LWIN,  VK_RWIN};
    for (WORD vk : kMods) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            SendKey(vk, false);
        }
    }
}

struct ScanKey {
    WORD scan = 0;
    bool extended = false;  // E0-prefixed, e.g. Insert
};

// Resolve a virtual key to the physical key that produces it under the current
// keyboard layout.
//
// MAPVK_VK_TO_VSC gives the scan code but hides whether the key is extended,
// and VK_INSERT is: injecting its 0x52 without KEYEVENTF_EXTENDEDKEY arrives as
// a different key entirely. MAPVK_VK_TO_VSC_EX reports the prefix in bits
// 0xE000, so extended-ness is derived rather than hardcoded per key.
ScanKey MakeScanKey(WORD vk) {
    const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    ScanKey out;
    out.scan = static_cast<WORD>(mapped & 0xFFu);
    out.extended = (mapped & 0xE000u) != 0;
    return out;
}

// Inject a two-key chord: modifier down, key down, key up, modifier up. All
// four go out in one SendInput call so a real keystroke cannot land in the
// middle of the chord.
//
// Scan codes rather than virtual keys. An event carrying only wVk has no
// hardware scan code, and a surprising number of receivers discard those: RDP
// and other remote stacks, virtual machines, raw-input readers, and edit
// controls that use the scan code to tell left from right modifiers. With wVk
// set to zero and KEYEVENTF_SCANCODE, the event is indistinguishable from a
// real keypress at every layer above the driver.
bool SendChord(WORD modVk, WORD keyVk) {
    const ScanKey mod = MakeScanKey(modVk);
    const ScanKey key = MakeScanKey(keyVk);

    // A layout that cannot map one of these falls back to the virtual-key form.
    // That form is what remote desktops tend to drop, but it still works
    // locally, and a paste that reaches most apps beats one that silently
    // reaches none.
    const bool byScan = (mod.scan != 0 && key.scan != 0);

    const WORD vks[4] = {modVk, keyVk, keyVk, modVk};
    const ScanKey chord[4] = {mod, key, key, mod};

    INPUT keys[4] = {};
    for (int i = 0; i < 4; ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wScan = chord[i].scan;
        if (byScan) {
            keys[i].ki.wVk = 0;
            keys[i].ki.dwFlags = KEYEVENTF_SCANCODE |
                                 (chord[i].extended ? KEYEVENTF_EXTENDEDKEY : 0u);
        } else {
            keys[i].ki.wVk = vks[i];
            keys[i].ki.dwFlags = 0;
        }
        if (i >= 2) {
            keys[i].ki.dwFlags |= KEYEVENTF_KEYUP;
        }
    }
    return SendInput(4, keys, sizeof(INPUT)) == 4;
}

}  // namespace

bool InstallHook() {
    if (g_hook) {
        return true;
    }
    g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                             ForegroundProc, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    CaptureCurrentForeground();
    return g_hook != nullptr;
}

void RemoveHook() {
    if (g_hook) {
        UnhookWinEvent(g_hook);
        g_hook = nullptr;
    }
}

void CaptureCurrentForeground() {
    HWND hwnd = GetForegroundWindow();
    if (IsUsableTarget(hwnd)) {
        g_target = hwnd;
    }
}

HWND Target() {
    if (g_target && IsWindow(g_target)) {
        return g_target;
    }
    g_target = nullptr;
    return nullptr;
}

bool Execute(int delayMs, Key key) {
    HWND target = Target();
    if (!target) {
        return false;
    }
    ForceForeground(target);
    if (delayMs > 0) {
        Sleep(static_cast<DWORD>(delayMs));  // Wait for target window to stabilize focus
    }

    // Verify target actually became foreground — if not, don't inject keystrokes
    // into the wrong window
    if (!IsWindow(target) || GetForegroundWindow() != target) {
        return false;
    }

    ReleaseHeldModifiers();

    switch (key) {
        case Key::ShiftInsert:
            return SendChord(VK_SHIFT, VK_INSERT);
        case Key::CtrlV:
            return SendChord(VK_CONTROL, 'V');
    }
    return SendChord(VK_CONTROL, 'V');
}

}  // namespace paste
