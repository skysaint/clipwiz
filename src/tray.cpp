// tray.cpp
#include "tray.h"

#include <shellapi.h>

#include "i18n.h"
#include "resource.h"
#include "util.h"

namespace tray {
namespace {

NOTIFYICONDATAW g_icon = {};
bool g_added = false;

}  // namespace

bool Add(HWND owner, UINT callbackMsg, UINT iconId) {
    g_icon = NOTIFYICONDATAW{};
    g_icon.cbSize = sizeof(g_icon);
    g_icon.hWnd = owner;
    g_icon.uID = iconId;
    g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_icon.uCallbackMessage = callbackMsg;
    g_icon.hIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                                 MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON),
                                                 GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(g_icon.szTip, L"ClipWiz");
    g_added = Shell_NotifyIconW(NIM_ADD, &g_icon) != FALSE;
    return g_added;
}

void Remove() {
    if (g_added) {
        Shell_NotifyIconW(NIM_DELETE, &g_icon);
        g_added = false;
    }
    if (g_icon.hIcon) {
        DestroyIcon(g_icon.hIcon);
        g_icon.hIcon = nullptr;
    }
}

void SetTip(const std::wstring& tip) {
    if (!g_added) {
        return;
    }
    wcsncpy_s(g_icon.szTip, tip.c_str(), _TRUNCATE);
    g_icon.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_icon);
    g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
}

bool Restore() {
    if (g_icon.hWnd == nullptr) {
        return false;
    }
    g_added = Shell_NotifyIconW(NIM_ADD, &g_icon) != FALSE;
    return g_added;
}

UINT ShowMenu(HWND owner, const std::vector<PinnedEntry>& pinned, bool autostartOn) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return 0;
    }

    if (!pinned.empty()) {
        UINT index = 0;
        for (const PinnedEntry& entry : pinned) {
            std::wstring label = util::Format(L"%u. %s", index + 1, entry.text.c_str());
            if (!entry.hotkey.empty()) {
                label += L"\t" + entry.hotkey;
            }
            AppendMenuW(menu, MF_STRING, CmdPinnedBase + index, label.c_str());
            ++index;
            if (index >= 20) {
                break;  // 托盘菜单不适合太长，多的去快速粘贴框里找
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    AppendMenuW(menu, MF_STRING, CmdShowPopup, i18n::T("tray.show_popup"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdSettings, i18n::T("tray.settings"));
    AppendMenuW(menu, MF_STRING | (autostartOn ? MF_CHECKED : MF_UNCHECKED), CmdAutostart,
                i18n::T("tray.autostart"));
    AppendMenuW(menu, MF_STRING, CmdClearHistory, i18n::T("tray.clear_history"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdAbout, i18n::T("tray.about"));
    AppendMenuW(menu, MF_STRING, CmdExit, i18n::T("tray.exit"));

    // 托盘菜单要求所在窗口是前台窗口，否则点别处菜单不消失
    SetForegroundWindow(owner);
    POINT pt = {};
    GetCursorPos(&pt);
    const UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY |
                       static_cast<UINT>(GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0
                                             ? TPM_RIGHTALIGN
                                             : TPM_LEFTALIGN);
    const int chosen = TrackPopupMenuEx(menu, flags, pt.x, pt.y, owner, nullptr);
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);  // 老问题：菜单关掉后补一条空消息才不卡
    return chosen > 0 ? static_cast<UINT>(chosen) : 0u;
}

}  // namespace tray
