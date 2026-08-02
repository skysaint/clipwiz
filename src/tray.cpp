// tray.cpp
#include "tray.h"

#include <shellapi.h>

#include "i18n.h"
#include "log.h"
#include "resource.h"
#include "util.h"

namespace tray {
namespace {

NOTIFYICONDATAW g_icon = {};
bool g_added = false;

}  // namespace

bool Add(HWND owner, UINT callbackMsg, UINT iconId) {
    LOG_INFO("Adding tray icon, owner=%p, callbackMsg=%u, iconId=%u", owner, callbackMsg, iconId);
    
    // Zero out and set basic parameters
    ZeroMemory(&g_icon, sizeof(g_icon));
    g_icon.cbSize = sizeof(NOTIFYICONDATAW);
    g_icon.hWnd = owner;
    g_icon.uID = iconId;
    g_icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_icon.uCallbackMessage = callbackMsg;
    
    // Load icon
    g_icon.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(iconId));
    
    if (!g_icon.hIcon) {
        LOG_ERROR("Failed to load icon resource ID=%u, error=%u", iconId, GetLastError());
        return false;
    }
    
    LOG_INFO("Icon loaded successfully");
    
    // Set tooltip text
    wcscpy_s(g_icon.szTip, L"ClipWiz");
    
    // Add tray icon
    g_added = Shell_NotifyIconW(NIM_ADD, &g_icon) != FALSE;
    
    if (!g_added) {
        DWORD error = GetLastError();
        LOG_ERROR("Shell_NotifyIconW(NIM_ADD) failed, error=%u", error);
        DestroyIcon(g_icon.hIcon);
        g_icon.hIcon = nullptr;
    } else {
        LOG_INFO("Tray icon added successfully");
    }
    
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

UINT ShowMenu(HWND owner, const std::vector<PinnedEntry>& pinned, bool autostartOn,
              const std::wstring& popupHotkeyText) {
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
                break;  // Tray menu shouldn't be too long, find more in quick paste dialog
            }
        }
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }

    // Show quick paste dialog (with hotkey display)
    std::wstring showPopupText = i18n::T("tray.show_popup");
    if (!popupHotkeyText.empty()) {
        showPopupText += L"\t" + popupHotkeyText;
    }
    AppendMenuW(menu, MF_STRING, CmdShowPopup, showPopupText.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdSettings, i18n::T("tray.settings"));
    AppendMenuW(menu, MF_STRING | (autostartOn ? MF_CHECKED : MF_UNCHECKED), CmdAutostart,
                i18n::T("tray.autostart"));
    AppendMenuW(menu, MF_STRING, CmdClearHistory, i18n::T("tray.clear_history"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CmdAbout, i18n::T("tray.about"));
    AppendMenuW(menu, MF_STRING, CmdExit, i18n::T("tray.exit"));

    // No longer force SetForegroundWindow to avoid other app flickering
    // If menu doesn't disappear issue reappears, consider other solutions
    POINT pt = {};
    GetCursorPos(&pt);
    const UINT flags = TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY |
                       static_cast<UINT>(GetSystemMetrics(SM_MENUDROPALIGNMENT) != 0
                                             ? TPM_RIGHTALIGN
                                             : TPM_LEFTALIGN);
    const int chosen = TrackPopupMenuEx(menu, flags, pt.x, pt.y, owner, nullptr);
    DestroyMenu(menu);
    PostMessageW(owner, WM_NULL, 0, 0);  // Old issue: add empty message after menu close to avoid hanging
    return chosen > 0 ? static_cast<UINT>(chosen) : 0u;
}

}  // namespace tray
