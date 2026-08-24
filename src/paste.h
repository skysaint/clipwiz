// paste.h — Remember "which window was typing in", paste content back there
#pragma once

#include <windows.h>

namespace paste {

// Install foreground window change hook; failure doesn't break main flow,
// paste target just degrades to current foreground window
bool InstallHook();
void RemoveHook();

// Most recent foreground window not belonging to this process
HWND Target();
// Manually capture before showing quick paste popup, covers hook install failure
void CaptureCurrentForeground();

// Activate target window then simulate Ctrl+V. Clipboard content must be set by caller.
bool Execute(int delayMs);

}  // namespace paste
