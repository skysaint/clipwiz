// util.h — Path, atomic file write, theme colors, DPI, fonts, autostart utilities
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace util {

// UI color scheme, only light and dark modes
struct Theme {
    COLORREF bg;      // List background
    COLORREF bgAlt;   // Section background (pinned area)
    COLORREF fg;      // Main text
    COLORREF dim;     // Secondary text (index, hotkey, size description)
    COLORREF sel;     // Selected row background
    COLORREF selFg;   // Selected row text
    COLORREF line;    // Separator line
    COLORREF accent;  // Pin marker
};

const wchar_t* AppName();

std::wstring ExePath();
std::wstring ExeDir();

// Data directory: auto-detects portable vs installed mode.
// If config.ini exists next to exe → portable (exe dir). Otherwise → %APPDATA%\ClipWiz.
const std::wstring& DataDir();
void SetDataDir(const std::wstring& dir);
std::wstring AppDataDir();          // %APPDATA%\ClipWiz
bool IsPortable();                  // true if data lives next to exe
bool MigrateDataDir(bool toPortable);  // Move data files between locations
std::wstring ConfigPath();
std::wstring StorePath();
bool EnsureDir(const std::wstring& dir);

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out);
// Atomic write: write temp file -> flush -> replace. Worst case on power failure reverts to last complete version
bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size);
uint64_t FileSizeOf(const std::wstring& path);

uint64_t NowFileTime();
std::wstring TimeStampForFileName();
uint64_t Hash64(const void* data, size_t size);

std::wstring Format(const wchar_t* fmt, ...);
// Compress multi-line text to a one-line summary, control chars to spaces, truncate if too long
std::wstring OneLinePreview(const std::wstring& text, size_t maxLen);

std::wstring Widen(const std::string& utf8);
std::string Narrow(const std::wstring& ws);

bool IsSystemDarkMode();
Theme MakeTheme(bool dark);

int DpiOf(HWND hwnd);
int Scale(int dip, int dpi);
HFONT CreateUiFont(int dpi, int pointDelta);
HFONT CreateUiFont(int dpi, const std::wstring& faceName, int pointSize, int pointDelta);

bool SetAutostart(bool enable);
bool GetAutostart();

void ErrorBox(HWND owner, const std::wstring& text);
void InfoBox(HWND owner, const std::wstring& text);
bool ConfirmBox(HWND owner, const std::wstring& text);

}  // namespace util
