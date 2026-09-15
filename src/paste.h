// paste.h — Remember "which window was typing in", paste content back there
#pragma once

#include <windows.h>

namespace paste {

// Which keystroke clipwiz injects to paste. Ctrl+V is the near-universal
// binding and the default; Shift+Insert is the older one and still the right
// answer for the apps that bind Ctrl+V to something else, plus terminal-style
// hosts where Ctrl+V is a literal control character.
enum class Key : int {
    CtrlV = 0,
    ShiftInsert = 1,
};

// Install foreground window change hook; failure doesn't break main flow,
// paste target just degrades to current foreground window
bool InstallHook();
void RemoveHook();

// Most recent foreground window not belonging to this process
HWND Target();
// Manually capture before showing quick paste popup, covers hook install failure
void CaptureCurrentForeground();

// Activate the target window, then simulate `key`. Clipboard content must be
// set by the caller.
bool Execute(int delayMs, Key key);

}  // namespace paste
