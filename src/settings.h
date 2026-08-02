// settings.h — config.ini read/write and settings dialog
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "store.h"

namespace settings {

enum class ThemeMode : int { Auto = 0, Light = 1, Dark = 2 };

struct Config {
    int maxHistory = 50;
    int expiryDays = 0;              // 0 = never expire
    uint32_t popupHotkey = 0;        // default Ctrl+Alt+V, see Defaults()
    uint32_t pinnedHotkeys[10] = {}; // positional: hotkey for the Nth pinned item
    int pasteDelayMs = 60;
    ThemeMode theme = ThemeMode::Auto;
    int rowsVisible = 10;
    std::wstring language;           // empty = English, "zh-CN" = Simplified Chinese
    int popupPosition = 0;           // 0=mouse 1=caret 2=last position
    int lastPopupX = -1;
    int lastPopupY = -1;
    std::wstring dataDir;            // empty = exe directory
    std::wstring fontName;           // empty = system default
    int fontSize = 0;                // 0 = default
    uint32_t maxTextBytes = 1024u * 1024u;
    uint32_t maxImagePixels = 33177600u;
    int largeItemThresholdMB = 10;  // threshold for large-item cleanup
};

Config Defaults();
void Load(Config& cfg);
bool Save(const Config& cfg);
void Clamp(Config& cfg);

// Settings dialog. Returns true if user clicked OK.
bool ShowDialog(HWND owner, HINSTANCE inst, Config& cfg);

// If settings dialog is already open, bring it to front. Returns true if it was activated.
bool ActivateExisting();

}  // namespace settings
