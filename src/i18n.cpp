// i18n.cpp
#include "i18n.h"

#include <windows.h>

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "resource.h"
#include "util.h"

namespace i18n {
namespace {

std::wstring g_lang;
std::unordered_map<std::string, std::wstring> g_map;

// Built-in English (master table), all user-visible strings registered here
struct Entry {
    const char* key;
    const wchar_t* en;
};

const Entry kDefaults[] = {
    // Tray menu
    {"tray.show_popup", L"Open Quick Paste"},
    {"tray.settings", L"Settings"},
    {"tray.autostart", L"Start with Windows"},
    {"tray.clear_history", L"Clear History (keep pinned)"},
    {"tray.about", L"About"},
    {"tray.exit", L"Exit"},

    // Quick paste popup
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

    // Settings dialog
    {"settings.title", L"ClipWiz Settings"},
    {"settings.tab.general", L"General"},
    {"settings.tab.types", L"Supported Types"},
    {"settings.tab.shortcuts", L"Shortcuts"},
    {"settings.autostart", L"Start with Windows"},
    {"settings.autostart_yes", L"Yes"},
    {"settings.max_history", L"Max saved items:"},
    {"settings.expiry_days", L"Item expiry (days):"},
    {"settings.language", L"Language:"},
    {"settings.lang_auto", L"Follow system"},
    {"settings.theme", L"Theme:"},
    {"settings.theme_auto", L"Follow system"},
    {"settings.theme_light", L"Light"},
    {"settings.theme_dark", L"Dark"},
    {"settings.popup_pos", L"Popup position:"},
    {"settings.pos_mouse", L"At mouse pointer"},
    {"settings.pos_caret", L"At text cursor"},
    {"settings.pos_last", L"Last opened position"},
    {"settings.font", L"Display font:"},
    {"settings.font_default", L"Restore default"},
    {"settings.font_default_val", L"(System default)"},
    {"settings.data_dir", L"Data storage:"},
    {"settings.data_installed", L"User directory (recommended)"},
    {"settings.data_portable", L"Program directory (portable)"},
    {"settings.popup_hotkey", L"Open quick paste:"},
    {"settings.pinned_group", L"Pinned item shortcuts"},
    {"settings.position", L"Pos"},
    {"settings.win_key", L"Win"},
    {"settings.ok", L"OK"},
    {"settings.cancel", L"Cancel"},

    // Supported type descriptions
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

    // Messages
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

    // Preview
    {"preview.image", L"[Image %u\x00D7%u]"},
    {"preview.files", L"%d files: %s"},
    {"preview.empty", L"[Empty content]"},

    // About
    {"about.text", L"ClipWiz %s\nLightweight clipboard history tool.\nData stored locally, nothing "
                   L"leaves your PC."},

    // Hotkey dialog (deprecated, key kept for safety)
    {"hotkey.clear", L"Unbind"},
};

// UTF-8 -> UTF-16
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
    // Strip BOM
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

        // Strip \r
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        // Trim key whitespace
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        if (!key.empty() && !val.empty()) {
            // Process escape sequences: \n -> newline, \t -> tab
            std::string processed;
            processed.reserve(val.size());
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '\\' && i + 1 < val.size()) {
                    char next = val[i + 1];
                    if (next == 'n') { processed += '\n'; ++i; continue; }
                    if (next == 't') { processed += '\t'; ++i; continue; }
                    if (next == '\\') { processed += '\\'; ++i; continue; }
                }
                processed += val[i];
            }
            g_map[key] = Utf8ToWide(processed);
        }
    }
}

// Load language data from a compiled-in RCDATA resource
void LoadLngResource(int resourceId) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!hRes) {
        return;
    }
    HGLOBAL hMem = LoadResource(hMod, hRes);
    if (!hMem) {
        return;
    }
    const char* data = static_cast<const char*>(LockResource(hMem));
    DWORD size = SizeofResource(hMod, hRes);
    if (!data || size == 0) {
        return;
    }
    // Strip BOM if present
    size_t start = 0;
    if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF &&
        static_cast<unsigned char>(data[1]) == 0xBB &&
        static_cast<unsigned char>(data[2]) == 0xBF) {
        start = 3;
    }
    std::string content(data + start, static_cast<size_t>(size) - start);

    // Parse key=value lines (same logic as LoadLngFile)
    size_t pos = 0;
    while (pos < content.size()) {
        size_t eol = content.find('\n', pos);
        std::string line = content.substr(pos, eol == std::string::npos ? std::string::npos
                                                                        : eol - pos);
        pos = (eol == std::string::npos) ? content.size() : eol + 1;

        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
            key.pop_back();
        }
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
            val.erase(val.begin());
        }
        if (!key.empty() && !val.empty()) {
            std::string processed;
            processed.reserve(val.size());
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '\\' && i + 1 < val.size()) {
                    char next = val[i + 1];
                    if (next == 'n') { processed += '\n'; ++i; continue; }
                    if (next == 't') { processed += '\t'; ++i; continue; }
                    if (next == '\\') { processed += '\\'; ++i; continue; }
                }
                processed += val[i];
            }
            g_map[key] = Utf8ToWide(processed);
        }
    }
}

}  // namespace

void Init(const std::wstring& langCode) {
    g_map.clear();
    g_lang = langCode;

    // Explicitly set to English
    if (langCode == L"en" || langCode == L"English") {
        g_lang = L"";
        return;
    }

    std::wstring effective = langCode;

    // Empty string = follow system language
    if (effective.empty()) {
        wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0) {
            effective = locale;  // e.g. "zh-CN", "en-US", "ja-JP"
        }
    }

    if (effective.empty() || effective.compare(0, 2, L"en") == 0) {
        g_lang = L"";
        return;
    }

    // Try to load lang/<locale>.lng from disk (external file takes priority)
    std::wstring path = util::ExeDir() + L"\\lang\\" + effective + L".lng";
    LoadLngFile(path);

    // If full locale not found (e.g. "zh-TW"), try matching language prefix only ("zh")
    if (g_map.empty()) {
        size_t dash = effective.find(L'-');
        if (dash != std::wstring::npos) {
            std::wstring prefix = effective.substr(0, dash);
            // Try prefix again: zh -> zh-CN (conventional primary variant)
            std::wstring fallback = prefix + L"-CN";
            path = util::ExeDir() + L"\\lang\\" + fallback + L".lng";
            LoadLngFile(path);
        }
    }

    // If still empty and language is Chinese, fall back to built-in resource
    if (g_map.empty() && effective.compare(0, 2, L"zh") == 0) {
        LoadLngResource(IDR_LNG_ZHCN);
    }

    g_lang = g_map.empty() ? L"" : effective;
}

const wchar_t* T(const char* key) {
    // Check loaded translations first
    auto it = g_map.find(key);
    if (it != g_map.end()) {
        return it->second.c_str();
    }
    // Fall back to built-in English
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
