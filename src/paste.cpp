// paste.cpp
#include "paste.h"

#include <cstring>

namespace paste {
namespace {

HWINEVENTHOOK g_hook = nullptr;
HWND g_target = nullptr;

// 桌面、任务栏、开始菜单这些不是能粘贴的地方，记下来只会干扰
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

// SetForegroundWindow 在不是当前前台进程时会被系统拒掉，
// 借目标窗口线程的输入队列绕过这个限制。
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

// 快捷键是按住修饰键触发的，此刻 Ctrl/Alt/Shift/Win 物理上还按着。
// 不先松开，接下来的 Ctrl+V 会变成 Ctrl+Alt+V 之类的东西。
void ReleaseHeldModifiers() {
    static const WORD kMods[] = {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU,
                                 VK_LSHIFT,   VK_RSHIFT,   VK_LWIN,  VK_RWIN};
    for (WORD vk : kMods) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            SendKey(vk, false);
        }
    }
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

bool Execute(int delayMs) {
    HWND target = Target();
    if (!target) {
        return false;
    }
    ForceForeground(target);
    if (delayMs > 0) {
        Sleep(static_cast<DWORD>(delayMs));  // 等目标窗口把焦点接稳
    }
    ReleaseHeldModifiers();

    INPUT keys[4] = {};
    for (INPUT& key : keys) {
        key.type = INPUT_KEYBOARD;
    }
    keys[0].ki.wVk = VK_CONTROL;
    keys[1].ki.wVk = 'V';
    keys[2].ki.wVk = 'V';
    keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
    keys[3].ki.wVk = VK_CONTROL;
    keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
    for (INPUT& key : keys) {
        key.ki.wScan = static_cast<WORD>(MapVirtualKeyW(key.ki.wVk, MAPVK_VK_TO_VSC));
    }
    return SendInput(4, keys, sizeof(INPUT)) == 4;
}

}  // namespace paste
