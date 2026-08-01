// hotkey.cpp
#include "hotkey.h"

#include <commctrl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cstring>

#include "i18n.h"
#include "util.h"

namespace hotkey {
namespace {

struct KeyName {
    uint32_t vk;
    const wchar_t* name;
};

// 只列常用键。不在表里的键走 "Key%02X" 这种兜底写法，保证配置文件能原样读回来。
const KeyName kKeyNames[] = {
    {VK_SPACE, L"Space"},     {VK_RETURN, L"Enter"},   {VK_TAB, L"Tab"},
    {VK_BACK, L"Backspace"},  {VK_ESCAPE, L"Esc"},     {VK_INSERT, L"Insert"},
    {VK_DELETE, L"Delete"},   {VK_HOME, L"Home"},      {VK_END, L"End"},
    {VK_PRIOR, L"PageUp"},    {VK_NEXT, L"PageDown"},  {VK_UP, L"Up"},
    {VK_DOWN, L"Down"},       {VK_LEFT, L"Left"},      {VK_RIGHT, L"Right"},
    {VK_OEM_MINUS, L"Minus"}, {VK_OEM_PLUS, L"Equal"}, {VK_OEM_COMMA, L"Comma"},
    {VK_OEM_PERIOD, L"Period"}, {VK_OEM_1, L"Semicolon"}, {VK_OEM_2, L"Slash"},
    {VK_OEM_3, L"Backquote"}, {VK_OEM_4, L"LBracket"}, {VK_OEM_5, L"Backslash"},
    {VK_OEM_6, L"RBracket"},  {VK_OEM_7, L"Quote"},
};

std::wstring KeyText(uint32_t vk) {
    if (vk >= '0' && vk <= '9') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= 'A' && vk <= 'Z') {
        return std::wstring(1, static_cast<wchar_t>(vk));
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        return util::Format(L"F%u", vk - VK_F1 + 1);
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return util::Format(L"Num%u", vk - VK_NUMPAD0);
    }
    for (const KeyName& entry : kKeyNames) {
        if (entry.vk == vk) {
            return entry.name;
        }
    }
    return util::Format(L"Key%02X", vk);
}

uint32_t KeyFromText(const std::wstring& token) {
    if (token.empty()) {
        return 0;
    }
    if (token.size() == 1) {
        wchar_t ch = token[0];
        if (ch >= L'0' && ch <= L'9') {
            return static_cast<uint32_t>(ch);
        }
        if (ch >= L'a' && ch <= L'z') {
            return static_cast<uint32_t>(ch - L'a' + L'A');
        }
        if (ch >= L'A' && ch <= L'Z') {
            return static_cast<uint32_t>(ch);
        }
        return 0;
    }
    if ((token[0] == L'F' || token[0] == L'f') && token.size() <= 3) {
        int n = _wtoi(token.c_str() + 1);
        if (n >= 1 && n <= 24) {
            return static_cast<uint32_t>(VK_F1 + n - 1);
        }
    }
    if (token.size() == 4 && _wcsnicmp(token.c_str(), L"Num", 3) == 0 && token[3] >= L'0' &&
        token[3] <= L'9') {
        return static_cast<uint32_t>(VK_NUMPAD0 + (token[3] - L'0'));
    }
    if (token.size() == 5 && _wcsnicmp(token.c_str(), L"Key", 3) == 0) {
        uint32_t vk = 0;
        if (swscanf_s(token.c_str() + 3, L"%x", &vk) == 1 && vk > 0 && vk <= 0xFF) {
            return vk;
        }
        return 0;
    }
    for (const KeyName& entry : kKeyNames) {
        if (_wcsicmp(token.c_str(), entry.name) == 0) {
            return entry.vk;
        }
    }
    return 0;
}

}  // namespace

std::wstring ToText(uint32_t code) {
    const uint32_t vk = VkOf(code);
    if (vk == 0) {
        return std::wstring();
    }
    const uint32_t mods = ModsOf(code);
    std::wstring text;
    if (mods & MOD_CONTROL) {
        text += L"Ctrl+";
    }
    if (mods & MOD_ALT) {
        text += L"Alt+";
    }
    if (mods & MOD_SHIFT) {
        text += L"Shift+";
    }
    if (mods & MOD_WIN) {
        text += L"Win+";
    }
    text += KeyText(vk);
    return text;
}

uint32_t FromText(const std::wstring& text) {
    uint32_t mods = 0;
    uint32_t vk = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t plus = text.find(L'+', pos);
        std::wstring token =
            text.substr(pos, plus == std::wstring::npos ? std::wstring::npos : plus - pos);
        // 顺手把空格吃掉，"Ctrl + V" 这种手写法也能认
        while (!token.empty() && token.front() == L' ') {
            token.erase(token.begin());
        }
        while (!token.empty() && token.back() == L' ') {
            token.pop_back();
        }
        if (!token.empty()) {
            if (_wcsicmp(token.c_str(), L"Ctrl") == 0 || _wcsicmp(token.c_str(), L"Control") == 0) {
                mods |= MOD_CONTROL;
            } else if (_wcsicmp(token.c_str(), L"Alt") == 0) {
                mods |= MOD_ALT;
            } else if (_wcsicmp(token.c_str(), L"Shift") == 0) {
                mods |= MOD_SHIFT;
            } else if (_wcsicmp(token.c_str(), L"Win") == 0) {
                mods |= MOD_WIN;
            } else {
                vk = KeyFromText(token);
                if (vk == 0) {
                    return 0;
                }
            }
        }
        if (plus == std::wstring::npos) {
            break;
        }
        pos = plus + 1;
    }
    if (vk == 0) {
        return 0;
    }
    return Make(mods, vk);
}

uint32_t FromControl(WORD raw, bool win) {
    const uint32_t vk = raw & 0xFFu;
    const uint32_t flags = (raw >> 8) & 0xFFu;
    if (vk == 0) {
        return 0;
    }
    uint32_t mods = 0;
    if (flags & HOTKEYF_CONTROL) {
        mods |= MOD_CONTROL;
    }
    if (flags & HOTKEYF_ALT) {
        mods |= MOD_ALT;
    }
    if (flags & HOTKEYF_SHIFT) {
        mods |= MOD_SHIFT;
    }
    if (win) {
        mods |= MOD_WIN;
    }
    return Make(mods, vk);
}

WORD ToControl(uint32_t code) {
    const uint32_t mods = ModsOf(code);
    uint32_t flags = 0;
    if (mods & MOD_CONTROL) {
        flags |= HOTKEYF_CONTROL;
    }
    if (mods & MOD_ALT) {
        flags |= HOTKEYF_ALT;
    }
    if (mods & MOD_SHIFT) {
        flags |= HOTKEYF_SHIFT;
    }
    return static_cast<WORD>((flags << 8) | (VkOf(code) & 0xFFu));
}

bool IsUsable(uint32_t code) {
    const uint32_t vk = VkOf(code);
    const uint32_t mods = ModsOf(code);
    if (vk == 0 || mods == 0) {
        return false;
    }
    // 只按一个 Shift 当修饰键会把正常输入大写字母全抢走
    if (mods == MOD_SHIFT) {
        return false;
    }
    return true;
}

bool LooksRisky(uint32_t code, std::wstring& why) {
    const uint32_t vk = VkOf(code);
    const uint32_t mods = ModsOf(code);
    why.clear();
    if (mods == MOD_CONTROL && vk >= '1' && vk <= '9') {
        why = util::Format(i18n::T("msg.risky_ctrl_num"), ToText(code).c_str());
        return true;
    }
    if (mods == MOD_CONTROL &&
        (vk == 'C' || vk == 'V' || vk == 'X' || vk == 'Z' || vk == 'A' || vk == 'S')) {
        why = i18n::T("msg.risky_ctrl_edit");
        return true;
    }
    if (mods == MOD_ALT && vk == VK_TAB) {
        why = i18n::T("msg.risky_alt_tab");
        return true;
    }
    return false;
}

bool Manager::Register(int id, uint32_t code) {
    if (!hwnd_ || !IsUsable(code)) {
        return false;
    }
    Unregister(id);
    // MOD_NOREPEAT：按住不放不会连发
    if (!RegisterHotKey(hwnd_, id, ModsOf(code) | MOD_NOREPEAT, VkOf(code))) {
        return false;
    }
    live_.push_back(id);
    return true;
}

void Manager::Unregister(int id) {
    auto it = std::find(live_.begin(), live_.end(), id);
    if (it == live_.end()) {
        return;
    }
    UnregisterHotKey(hwnd_, id);
    live_.erase(it);
}

void Manager::UnregisterAll() {
    for (int id : live_) {
        UnregisterHotKey(hwnd_, id);
    }
    live_.clear();
}

bool Manager::Has(int id) const {
    return std::find(live_.begin(), live_.end(), id) != live_.end();
}

}  // namespace hotkey
