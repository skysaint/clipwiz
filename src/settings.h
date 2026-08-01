// settings.h — config.ini 的读写，以及设置对话框（三页签）
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "store.h"

namespace settings {

enum class ThemeMode : int { Auto = 0, Light = 1, Dark = 2 };

struct Config {
    int maxHistory = 50;
    int expiryDays = 0;              // 0 = 不过期
    uint32_t popupHotkey = 0;        // 默认 Ctrl+Alt+V，见 Defaults()
    uint32_t pinnedHotkeys[10] = {}; // 位置式：第 N 个置顶项的快捷键
    int pasteDelayMs = 60;
    ThemeMode theme = ThemeMode::Auto;
    int rowsVisible = 10;
    std::wstring language;           // 空 = English，"zh-CN" = 简体中文
    int popupPosition = 0;           // 0=鼠标处 1=光标处 2=上次位置
    int lastPopupX = -1;
    int lastPopupY = -1;
    std::wstring dataDir;            // 空 = exe 目录
    std::wstring fontName;           // 空 = 系统默认
    int fontSize = 0;                // 0 = 默认
    uint32_t maxTextBytes = 1024u * 1024u;
    uint32_t maxImagePixels = 33177600u;
    int largeItemThresholdMB = 10;  // 清理大条目时的阈值
};

Config Defaults();
void Load(Config& cfg);
bool Save(const Config& cfg);
void Clamp(Config& cfg);

// 设置对话框（PropertySheet 三页签）。返回 true 表示用户点了确定。
bool ShowDialog(HWND owner, HINSTANCE inst, Config& cfg);

}  // namespace settings
