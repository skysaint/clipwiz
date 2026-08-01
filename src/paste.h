// paste.h — 记住"刚才在哪个窗口打字"，粘贴时把内容送回那里
#pragma once

#include <windows.h>

namespace paste {

// 装前台窗口变化钩子；失败也不影响主流程，只是粘贴目标会退化成当前前台窗口
bool InstallHook();
void RemoveHook();

// 最近一个不属于本程序的前台窗口
HWND Target();
// 弹出快速粘贴框之前手动记一次，兜住钩子装不上的情况
void CaptureCurrentForeground();

// 激活目标窗口后模拟一次 Ctrl+V。剪贴板内容需要调用方先写好。
bool Execute(int delayMs);

}  // namespace paste
