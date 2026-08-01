// clipboard.h — 系统剪贴板的监听、读取和写入
//
// 捕获优先级：RTF > HTML > Image > FileDrop > Text
// 写回时按条目类型还原对应格式 + 纯文本 fallback
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "store.h"

namespace clip {

bool StartListening(HWND hwnd);
void StopListening(HWND hwnd);

// 这次剪贴板变化是不是我们自己写进去的（靠剪贴板序列号判断）
bool IsSelfWrite();

// 读当前剪贴板，按优先级取一种主格式。超限或带排除标记时 kind 为 None。
bool Capture(ItemKind& kind, std::vector<uint8_t>& data, uint32_t& imgW, uint32_t& imgH,
             uint32_t maxTextBytes, uint32_t maxImagePixels);

// 把条目内容写回系统剪贴板
bool WriteItem(HWND owner, const Item& item);

}  // namespace clip
