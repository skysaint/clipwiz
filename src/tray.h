// tray.h — 托盘图标与右键菜单
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace tray {

// 右键菜单命令 ID，和 WM_COMMAND 共用一套编号空间
enum Command : UINT {
    CmdShowPopup = 40001,
    CmdSettings = 40002,
    CmdClearHistory = 40003,
    CmdAutostart = 40004,
    CmdAbout = 40005,
    CmdExit = 40006,
    CmdPinnedBase = 41000,  // 菜单里直接列出的置顶项，加上序号
};

struct PinnedEntry {
    std::wstring text;    // 已经压成一行的摘要
    std::wstring hotkey;  // 显示在右边的快捷键文本，可为空
};

bool Add(HWND owner, UINT callbackMsg, UINT iconId);
void Remove();
void SetTip(const std::wstring& tip);
// 图标被资源管理器重启冲掉之后重新加回来
bool Restore();

// 返回用户选的命令 ID，取消返回 0
UINT ShowMenu(HWND owner, const std::vector<PinnedEntry>& pinned, bool autostartOn);

}  // namespace tray
