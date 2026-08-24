// hotkey.h — Global hotkey encoding, display text, and registration management
//
// Hotkeys are represented as a single uint32_t: high 16 bits = RegisterHotKey MOD_* flags,
// low 16 bits = virtual key code. 0 means unbound.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace hotkey {

// Popup uses fixed ID 1; pinned items start at 1000, sequential
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

// Human-readable form, e.g. "Ctrl+Alt+V"; returns empty string if unbound
std::wstring ToText(uint32_t code);
// Inverse of ToText; config.ini stores this text format; returns 0 on parse failure
uint32_t FromText(const std::wstring& text);

// msctls_hotkey32 control uses HOTKEYF_* flags which differ from MOD_*, conversion needed.
// Win key cannot be represented by the control, passed separately via a checkbox.
uint32_t FromControl(WORD raw, bool win);
WORD ToControl(uint32_t code);

// At least one modifier key required, otherwise it would intercept normal typing
bool IsUsable(uint32_t code);
// Combinations likely occupied by other programs; warn early
bool LooksRisky(uint32_t code, std::wstring& why);

class Manager {
public:
    void Attach(HWND hwnd) { hwnd_ = hwnd; }

    // Returns false on registration failure (usually another program holds it)
    bool Register(int id, uint32_t code);
    void Unregister(int id);
    void UnregisterAll();
    bool Has(int id) const;

private:
    HWND hwnd_ = nullptr;
    std::vector<int> live_;
};

}  // namespace hotkey
