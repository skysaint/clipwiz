// i18n.cpp
#include "i18n.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "util.h"

namespace i18n {
namespace {

std::wstring g_lang;
std::unordered_map<std::string, std::wstring> g_map;

// 内置英文（主表），所有用户可见文字都在这里注册
struct Entry {
    const char* key;
    const wchar_t* en;
};

const Entry kDefaults[] = {
    // 托盘菜单
    {"tray.show_popup", L"Open Quick Paste"},
    {"tray.settings", L"Settings"},
    {"tray.autostart", L"Start with Windows"},
    {"tray.clear_history", L"Clear History (keep pinned)"},
    {"tray.about", L"About"},
    {"tray.exit", L"Exit"},

    // 快速粘贴框
    {"popup.title", L"ClipWiz"},
    {"popup.filter_hint", L"Type to filter..."},
    {"popup.hint",
     L"Enter paste \x00B7 Alt+N direct \x00B7 Ctrl+P pin \x00B7 Ctrl+D delete \x00B7 Esc close"},
    {"popup.empty", L"No clipboard history yet"},
    {"popup.empty_filtered", L"No matching items"},
    {"popup.menu.copy", L"Copy"},
    {"popup.menu.paste", L"Paste"},
    {"popup.menu.pin", L"Pin (never auto-clean)"},
    {"popup.menu.unpin", L"Unpin"},
    {"popup.menu.delete", L"Delete"},

    // 设置对话框
    {"settings.title", L"ClipWiz Settings"},
    {"settings.tab.general", L"General"},
    {"settings.tab.types", L"Supported Types"},
    {"settings.tab.shortcuts", L"Shortcuts"},
    {"settings.autostart", L"Start with Windows"},
    {"settings.autostart_yes", L"Yes"},
    {"settings.max_history", L"Max saved items:"},
    {"settings.expiry_days", L"Item expiry (days):"},
    {"settings.language", L"Language:"},
    {"settings.theme", L"Theme:"},
    {"settings.theme_auto", L"Follow system"},
    {"settings.theme_light", L"Light"},
    {"settings.theme_dark", L"Dark"},
    {"settings.popup_pos", L"Popup position:"},
    {"settings.pos_mouse", L"At mouse pointer"},
    {"settings.pos_caret", L"At text cursor"},
    {"settings.pos_last", L"Last opened position"},
    {"settings.font", L"Display font"},
    {"settings.font_default", L"Restore default"},
    {"settings.data_dir", L"Data directory:"},
    {"settings.browse", L"..."},
    {"settings.popup_hotkey", L"Open quick paste:"},
    {"settings.pinned_group", L"Pinned item shortcuts"},
    {"settings.position", L"Pos"},
    {"settings.win_key", L"Win"},
    {"settings.ok", L"OK"},
    {"settings.cancel", L"Cancel"},

    // 支持类型描述
    {"type.text.name", L"Text (CF_UNICODETEXT)"},
    {"type.text.desc",
     L"Plain Unicode text. Produced by virtually all applications when copying text."},
    {"type.image.name", L"Image (CF_DIB / PNG)"},
    {"type.image.desc",
     L"Bitmap image. Produced by screenshot tools, image editors, browsers, etc."},
    {"type.html.name", L"HTML Format"},
    {"type.html.desc",
     L"Rich web content. Produced by browsers and email clients when copying formatted text."},
    {"type.rtf.name", L"Rich Text Format"},
    {"type.rtf.desc",
     L"Formatted document content. Produced by Word, WordPad, and other office applications."},
    {"type.filedrop.name", L"File Drop (CF_HDROP)"},
    {"type.filedrop.desc",
     L"File/folder path list. Produced by Explorer when copying files. Stores paths, not file "
     L"contents."},

    // 消息
    {"msg.select_pinned", L"Please select a pinned item from the list first."},
    {"msg.confirm_delete_pinned", L"This item is pinned. Delete it permanently?"},
    {"msg.confirm_clear", L"Clear all non-pinned history?"},
    {"msg.hotkey_conflict", L"Hotkey registration failed for some shortcuts:"},
    {"msg.hotkey_suggest", L"Tip: Use Ctrl+Alt+number combinations to avoid conflicts."},
    {"msg.save_failed", L"Failed to save data file. Check disk space and permissions."},
    {"msg.corrupt_found", L"Data file was corrupted and has been backed up. Starting fresh."},
    {"msg.need_modifier", L"At least one modifier key (Ctrl/Alt/Win) is required."},
    {"msg.same_as_popup", L"This combination is already used for opening the popup."},
    {"msg.risky_ctrl_num",
     L"%s conflicts with browser/editor tab shortcuts. Use Ctrl+Alt+number instead."},
    {"msg.risky_ctrl_edit",
     L"This is a common editing shortcut used by all programs. Choose another."},
    {"msg.risky_alt_tab", L"Alt+Tab is used by Windows to switch windows. It cannot be registered."},
    {"msg.large_data",
     L"Clipboard data has grown very large. Clean up large non-pinned items now?"},

    // 预览
    {"preview.image", L"[Image %u\x00D7%u]"},
    {"preview.files", L"%d files: %s"},
    {"preview.empty", L"[Empty content]"},

    // 关于
    {"about.text", L"ClipWiz %s\nLightweight clipboard manager.\nData stored locally, nothing "
                   L"leaves your PC."},

    // 快捷键对话框（已废弃，保留键以防万一）
    {"hotkey.clear", L"Unbind"},
};

// UTF-8 → UTF-16
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) {
        return {};
    }
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), w.data(), len);
    return w;
}

void LoadLngFile(const std::wstring& path) {
    std::vector<uint8_t> raw;
    if (!util::ReadWholeFile(path, raw)) {
        return;
    }
    // 去 BOM
    size_t start = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        start = 3;
    }
    std::string content(reinterpret_cast<const char*>(raw.data()) + start, raw.size() - start);

    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos
                                                                        : eol - pos);
        pos = (eol == std::string::npos) ? content.size() : eol + 1;

        // 去 \r
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        // 跳过空行和注释
        if (line.empty() || line[0] == '#') {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // 去 key 两端空白
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        if (!key.empty() && !val.empty()) {
            g_map[key] = Utf8ToWide(val);
        }
    }
}

}  // namespace

void Init(const std::wstring& langCode) {
    g_map.clear();
    g_lang = langCode;
    if (langCode.empty() || langCode == L"en" || langCode == L"English") {
        g_lang = L"";
        return;
    }
    std::wstring path = util::ExeDir() + L"\\lang\\" + langCode + L".lng";
    LoadLngFile(path);
}

const wchar_t* T(const char* key) {
    // 先查加载的翻译
    auto it = g_map.find(key);
    if (it != g_map.end()) {
        return it->second.c_str();
    }
    // 回退到内置英文
    for (const Entry& e : kDefaults) {
        if (strcmp(e.key, key) == 0) {
            return e.en;
        }
    }
    return L"???";
}

const std::wstring& CurrentLang() {
    return g_lang;
}

}  // namespace i18n
