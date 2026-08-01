// hotkey.h — 全局快捷键的编码、显示文本和注册管理
//
// 快捷键统一用一个 uint32_t 表示：高 16 位是 RegisterHotKey 的 MOD_* 组合，
// 低 16 位是虚拟键码。0 表示未绑定。
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hotkey {

// 唤出快速粘贴框固定用 1 号，置顶项从 1000 开始按序号往后排
constexpr int kIdPopup = 1;
constexpr int kIdItemBase = 1000;

inline uint32_t Make(uint32_t mods, uint32_t vk) {
    return ((mods & 0xFFFFu) << 16) | (vk & 0xFFFFu);
}
inline uint32_t ModsOf(uint32_t code) {
    return (code >> 16) & 0xFFFFu;
}
inline uint32_t VkOf(uint32_t code) {
    return code & 0xFFFFu;
}

// 给人看的写法，例如 "Ctrl+Alt+V"；未绑定时返回空串
std::wstring ToText(uint32_t code);
// ToText 的逆运算，config.ini 里存的就是这种文本；解析失败返回 0
uint32_t FromText(const std::wstring& text);

// msctls_hotkey32 控件用的是 HOTKEYF_*，和 MOD_* 不是一套值，得转一道。
// Win 键控件表示不了，单独用一个复选框传进来。
uint32_t FromControl(WORD raw, bool win);
WORD ToControl(uint32_t code);

// 至少要带一个修饰键，否则会把普通打字全吞掉
bool IsUsable(uint32_t code);
// 那些大概率被别的程序占着的组合，提前提醒一句
bool LooksRisky(uint32_t code, std::wstring& why);

class Manager {
public:
    void Attach(HWND hwnd) { hwnd_ = hwnd; }

    // 注册失败返回 false（通常是被别的程序抢先占了）
    bool Register(int id, uint32_t code);
    void Unregister(int id);
    void UnregisterAll();
    bool Has(int id) const;

private:
    HWND hwnd_ = nullptr;
    std::vector<int> live_;
};

}  // namespace hotkey
