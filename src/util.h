// util.h — 路径、原子写文件、主题色、DPI、字体、开机自启等杂项工具
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace util {

// 界面配色，只有浅色和深色两套
struct Theme {
    COLORREF bg;      // 列表背景
    COLORREF bgAlt;   // 分区背景（置顶区）
    COLORREF fg;      // 正文
    COLORREF dim;     // 次要文字（序号、快捷键、尺寸说明）
    COLORREF sel;     // 选中行背景
    COLORREF selFg;   // 选中行文字
    COLORREF line;    // 分隔线
    COLORREF accent;  // 置顶标记
};

const wchar_t* AppName();

std::wstring ExePath();
std::wstring ExeDir();

// 数据目录默认 = exe 所在目录（绿色）；可通过 SetDataDir 改为别处
const std::wstring& DataDir();
void SetDataDir(const std::wstring& dir);
std::wstring ConfigPath();
std::wstring StorePath();
bool EnsureDir(const std::wstring& dir);

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out);
// 写临时文件 → 刷盘 → 原子替换，断电最坏回到上一个完整版本
bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size);
uint64_t FileSizeOf(const std::wstring& path);

uint64_t NowFileTime();
std::wstring TimeStampForFileName();
uint64_t Hash64(const void* data, size_t size);

std::wstring Format(const wchar_t* fmt, ...);
// 把多行文本压成一行摘要，控制字符换成空格，超长截断
std::wstring OneLinePreview(const std::wstring& text, size_t maxLen);

bool IsSystemDarkMode();
Theme MakeTheme(bool dark);

int DpiOf(HWND hwnd);
int Scale(int dip, int dpi);
HFONT CreateUiFont(int dpi, int pointDelta);

bool SetAutostart(bool enable);
bool GetAutostart();

void ErrorBox(HWND owner, const std::wstring& text);
void InfoBox(HWND owner, const std::wstring& text);
bool ConfirmBox(HWND owner, const std::wstring& text);

}  // namespace util
