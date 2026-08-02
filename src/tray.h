// tray.h — Tray icon and right-click context menu
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace tray {

// Right-click menu command IDs, sharing the same numbering space as WM_COMMAND
enum Command : UINT {
    CmdShowPopup = 40001,
    CmdSettings = 40002,
    CmdClearHistory = 40003,
    CmdAutostart = 40004,
    CmdAbout = 40005,
    CmdExit = 40006,
    CmdPinnedBase = 41000,  // Pinned items listed directly in menu, plus index
};

struct PinnedEntry {
    std::wstring text;    // One-line summary
    std::wstring hotkey;  // Hotkey text shown on the right, may be empty
};

bool Add(HWND owner, UINT callbackMsg, UINT iconId);
void Remove();
void SetTip(const std::wstring& tip);
// Re-add the tray icon after Explorer restarts and destroys it
bool Restore();

// Returns the command ID chosen by user, 0 if cancelled
UINT ShowMenu(HWND owner, const std::vector<PinnedEntry>& pinned, bool autostartOn,
              const std::wstring& popupHotkeyText);

}  // namespace tray
