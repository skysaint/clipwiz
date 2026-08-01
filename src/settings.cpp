// settings.cpp
#include "settings.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hotkey.h"
#include "i18n.h"
#include "util.h"

namespace settings {
namespace {

// config.ini 用 UTF-8 存储，宽窄转换
std::string Narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0,
                                nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring ws(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), ws.data(), n);
    return ws;
}

// 控件 ID
enum : int {
    IDC_TAB = 500,
    IDC_OK,
    IDC_CANCEL,
    // 常规页 (page 0)
    IDC_AUTOSTART,
    IDC_MAXHISTORY,
    IDC_EXPIRYDAYS,
    IDC_LANGUAGE,
    IDC_THEME,
    IDC_POPUPPOS,
    IDC_FONT_BTN,
    IDC_FONT_RESET,
    IDC_DATADIR,
    IDC_DATADIR_BTN,
    // 支持类型页 (page 1)
    IDC_TYPELIST,
    IDC_TYPEDESC,
    IDC_TYPES_NOTE,
    // 快捷键页 (page 2)
    IDC_POPUP_HK,
    IDC_POPUP_WIN,
    IDC_PIN_HK_BASE = 600,  // 600..609
    IDC_PIN_WIN_BASE = 620, // 620..629
};

struct DlgState {
    Config* cfg = nullptr;
    HWND hwnd = nullptr;
    HWND tab = nullptr;
    HFONT font = nullptr;
    bool resultOk = false;
    // 按页分组的控件句柄，用于 show/hide
    std::vector<HWND> pageCtrls[3];
};

DlgState* StateOf(HWND hwnd) {
    return reinterpret_cast<DlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void Track(DlgState* st, int page, HWND ctrl) {
    if (ctrl) {
        st->pageCtrls[page].push_back(ctrl);
    }
}

HWND MkLabel(DlgState* st, int page, int x, int y, int w, int h, const wchar_t* text) {
    HWND c = CreateWindowExW(0, L"STATIC", text, WS_CHILD | SS_LEFT, x, y, w, h, st->hwnd,
                             nullptr, nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    Track(st, page, c);
    return c;
}

HWND MkEdit(DlgState* st, int page, int id, int x, int y, int w, int h, DWORD extra) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_TABSTOP | extra, x, y,
                             w, h, st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                             nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    Track(st, page, c);
    return c;
}

HWND MkCombo(DlgState* st, int page, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, L"COMBOBOX", L"",
                             WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, x, y, w, h,
                             st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                             nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    Track(st, page, c);
    return c;
}

HWND MkButton(DlgState* st, int page, int id, int x, int y, int w, int h, const wchar_t* text,
              DWORD style) {
    HWND c = CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_TABSTOP | style, x, y, w, h,
                             st->hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr,
                             nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    Track(st, page, c);
    return c;
}

HWND MkHotkey(DlgState* st, int page, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(0, HOTKEY_CLASSW, L"", WS_CHILD | WS_TABSTOP, x, y, w, h, st->hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    Track(st, page, c);
    return c;
}

HWND MkListBox(DlgState* st, int page, int id, int x, int y, int w, int h) {
    HWND c = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                             WS_CHILD | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL, x, y, w, h, st->hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), nullptr, nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    Track(st, page, c);
    return c;
}

void ShowPage(DlgState* st, int page) {
    for (int i = 0; i < 3; ++i) {
        for (HWND c : st->pageCtrls[i]) {
            ShowWindow(c, i == page ? SW_SHOW : SW_HIDE);
        }
    }
}

// ---------------- 构建各页控件 ----------------

void BuildGeneral(DlgState* st) {
    const Config& cfg = *st->cfg;
    int y = 40;
    const int lx = 16, rowH = 28;

    HWND cb = MkButton(st, 0, IDC_AUTOSTART, lx, y, 200, 20, i18n::T("settings.autostart"),
                       BS_AUTOCHECKBOX);
    if (util::GetAutostart()) {
        SendMessageW(cb, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += rowH;

    MkLabel(st, 0, lx, y + 3, 190, 20, i18n::T("settings.max_history"));
    HWND edMax = MkEdit(st, 0, IDC_MAXHISTORY, lx + 200, y, 60, 22, ES_NUMBER | ES_AUTOHSCROLL);
    SetWindowTextW(edMax, util::Format(L"%d", cfg.maxHistory).c_str());
    y += rowH;

    MkLabel(st, 0, lx, y + 3, 190, 20, i18n::T("settings.expiry_days"));
    HWND edExp = MkEdit(st, 0, IDC_EXPIRYDAYS, lx + 200, y, 60, 22, ES_NUMBER | ES_AUTOHSCROLL);
    SetWindowTextW(edExp, util::Format(L"%d", cfg.expiryDays).c_str());
    y += rowH + 6;

    MkLabel(st, 0, lx, y + 3, 130, 20, i18n::T("settings.language"));
    HWND cbLang = MkCombo(st, 0, IDC_LANGUAGE, lx + 140, y, 180, 120);
    SendMessageW(cbLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(cbLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"\x7B80\x4F53\x4E2D\x6587"));
    SendMessageW(cbLang, CB_SETCURSEL, cfg.language.empty() ? 0 : 1, 0);
    y += rowH;

    MkLabel(st, 0, lx, y + 3, 130, 20, i18n::T("settings.theme"));
    HWND cbTheme = MkCombo(st, 0, IDC_THEME, lx + 140, y, 180, 120);
    SendMessageW(cbTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.theme_auto")));
    SendMessageW(cbTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.theme_light")));
    SendMessageW(cbTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.theme_dark")));
    SendMessageW(cbTheme, CB_SETCURSEL, static_cast<int>(cfg.theme), 0);
    y += rowH;

    MkLabel(st, 0, lx, y + 3, 130, 20, i18n::T("settings.popup_pos"));
    HWND cbPos = MkCombo(st, 0, IDC_POPUPPOS, lx + 140, y, 180, 120);
    SendMessageW(cbPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.pos_mouse")));
    SendMessageW(cbPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.pos_caret")));
    SendMessageW(cbPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.pos_last")));
    SendMessageW(cbPos, CB_SETCURSEL, cfg.popupPosition, 0);
    y += rowH + 6;

    std::wstring fontLabel = i18n::T("settings.font");
    if (!cfg.fontName.empty()) {
        fontLabel += util::Format(L": %s %dpt", cfg.fontName.c_str(), cfg.fontSize);
    }
    MkButton(st, 0, IDC_FONT_BTN, lx, y, 240, 24, fontLabel.c_str(), BS_PUSHBUTTON);
    MkButton(st, 0, IDC_FONT_RESET, lx + 250, y, 90, 24, i18n::T("settings.font_default"),
             BS_PUSHBUTTON);
    y += rowH + 6;

    MkLabel(st, 0, lx, y + 3, 130, 20, i18n::T("settings.data_dir"));
    std::wstring dirText = cfg.dataDir.empty() ? util::ExeDir() : cfg.dataDir;
    HWND edDir = MkEdit(st, 0, IDC_DATADIR, lx + 140, y, 250, 22, ES_READONLY | ES_AUTOHSCROLL);
    SetWindowTextW(edDir, dirText.c_str());
    MkButton(st, 0, IDC_DATADIR_BTN, lx + 398, y, 30, 22, L"...", BS_PUSHBUTTON);
}

void BuildTypes(DlgState* st) {
    HWND list = MkListBox(st, 1, IDC_TYPELIST, 16, 40, 200, 180);
    const char* keys[] = {"type.text.name", "type.image.name", "type.html.name", "type.rtf.name",
                          "type.filedrop.name"};
    for (const char* k : keys) {
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T(k)));
    }
    SendMessageW(list, LB_SETCURSEL, 0, 0);

    HWND desc = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", i18n::T("type.text.desc"),
                                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | WS_TABSTOP,
                                226, 40, 240, 180, st->hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TYPEDESC)), nullptr,
                                nullptr);
    SendMessageW(desc, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
    Track(st, 1, desc);
}

void BuildShortcuts(DlgState* st) {
    const Config& cfg = *st->cfg;
    int y = 40;
    const int lx = 16;

    MkLabel(st, 2, lx, y + 3, 160, 20, i18n::T("settings.popup_hotkey"));
    HWND hk = MkHotkey(st, 2, IDC_POPUP_HK, lx + 170, y, 130, 22);
    SendMessageW(hk, HKM_SETHOTKEY, hotkey::ToControl(cfg.popupHotkey), 0);
    HWND winCb = MkButton(st, 2, IDC_POPUP_WIN, lx + 310, y, 60, 22, i18n::T("settings.win_key"),
                          BS_AUTOCHECKBOX);
    if (hotkey::ModsOf(cfg.popupHotkey) & MOD_WIN) {
        SendMessageW(winCb, BM_SETCHECK, BST_CHECKED, 0);
    }
    y += 34;

    MkButton(st, 2, 0, lx, y, 450, 168, i18n::T("settings.pinned_group"), BS_GROUPBOX);
    y += 18;

    for (int i = 0; i < 10; ++i) {
        int col = i / 5;
        int row = i % 5;
        int cx = lx + 14 + col * 224;
        int cy = y + row * 28;
        std::wstring label = util::Format(L"%s%d:", i18n::T("settings.position"), i + 1);
        MkLabel(st, 2, cx, cy + 2, 52, 18, label.c_str());
        HWND phk = MkHotkey(st, 2, IDC_PIN_HK_BASE + i, cx + 54, cy, 104, 20);
        SendMessageW(phk, HKM_SETHOTKEY, hotkey::ToControl(cfg.pinnedHotkeys[i]), 0);
        HWND pwin = MkButton(st, 2, IDC_PIN_WIN_BASE + i, cx + 164, cy, 50, 20,
                             i18n::T("settings.win_key"), BS_AUTOCHECKBOX);
        if (hotkey::ModsOf(cfg.pinnedHotkeys[i]) & MOD_WIN) {
            SendMessageW(pwin, BM_SETCHECK, BST_CHECKED, 0);
        }
    }
}

// ---------------- 收集 ----------------

void Collect(DlgState* st) {
    Config& cfg = *st->cfg;
    HWND h = st->hwnd;

    cfg.maxHistory = GetDlgItemInt(h, IDC_MAXHISTORY, nullptr, FALSE);
    cfg.expiryDays = GetDlgItemInt(h, IDC_EXPIRYDAYS, nullptr, FALSE);

    int langSel = static_cast<int>(SendMessageW(GetDlgItem(h, IDC_LANGUAGE), CB_GETCURSEL, 0, 0));
    cfg.language = (langSel == 1) ? L"zh-CN" : L"";

    int themeSel = static_cast<int>(SendMessageW(GetDlgItem(h, IDC_THEME), CB_GETCURSEL, 0, 0));
    cfg.theme = static_cast<ThemeMode>(themeSel);

    cfg.popupPosition = static_cast<int>(
        SendMessageW(GetDlgItem(h, IDC_POPUPPOS), CB_GETCURSEL, 0, 0));

    bool autostart = SendMessageW(GetDlgItem(h, IDC_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED;
    util::SetAutostart(autostart);

    WORD raw = static_cast<WORD>(SendMessageW(GetDlgItem(h, IDC_POPUP_HK), HKM_GETHOTKEY, 0, 0));
    bool win = SendMessageW(GetDlgItem(h, IDC_POPUP_WIN), BM_GETCHECK, 0, 0) == BST_CHECKED;
    cfg.popupHotkey = hotkey::FromControl(raw, win);

    for (int i = 0; i < 10; ++i) {
        WORD r = static_cast<WORD>(
            SendMessageW(GetDlgItem(h, IDC_PIN_HK_BASE + i), HKM_GETHOTKEY, 0, 0));
        bool w = SendMessageW(GetDlgItem(h, IDC_PIN_WIN_BASE + i), BM_GETCHECK, 0, 0) == BST_CHECKED;
        cfg.pinnedHotkeys[i] = hotkey::FromControl(r, w);
    }
}

// ---------------- 字体 ----------------

void DoFont(DlgState* st) {
    Config& cfg = *st->cfg;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    if (!cfg.fontName.empty()) {
        wcsncpy_s(lf.lfFaceName, cfg.fontName.c_str(), _TRUNCATE);
    }
    if (cfg.fontSize > 0) {
        lf.lfHeight = -MulDiv(cfg.fontSize, util::DpiOf(st->hwnd), 72);
    }
    CHOOSEFONTW cf{};
    cf.lStructSize = sizeof(cf);
    cf.hwndOwner = st->hwnd;
    cf.lpLogFont = &lf;
    cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST;
    if (ChooseFontW(&cf)) {
        cfg.fontName = lf.lfFaceName;
        cfg.fontSize = cf.iPointSize / 10;
        std::wstring label = util::Format(L"%s: %s %dpt", i18n::T("settings.font"),
                                          cfg.fontName.c_str(), cfg.fontSize);
        SetWindowTextW(GetDlgItem(st->hwnd, IDC_FONT_BTN), label.c_str());
    }
}

void DoBrowseDir(DlgState* st) {
    wchar_t path[MAX_PATH] = {};
    BROWSEINFOW bi{};
    bi.hwndOwner = st->hwnd;
    bi.pszDisplayName = path;
    bi.lpszTitle = i18n::T("settings.data_dir");
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (pidl) {
        if (SHGetPathFromIDListW(pidl, path)) {
            st->cfg->dataDir = path;
            SetWindowTextW(GetDlgItem(st->hwnd, IDC_DATADIR), path);
        }
        CoTaskMemFree(pidl);
    }
}

void DoTypeDesc(DlgState* st) {
    int sel = static_cast<int>(SendMessageW(GetDlgItem(st->hwnd, IDC_TYPELIST), LB_GETCURSEL, 0, 0));
    const char* descKeys[] = {"type.text.desc", "type.image.desc", "type.html.desc",
                              "type.rtf.desc", "type.filedrop.desc"};
    if (sel >= 0 && sel < 5) {
        SetWindowTextW(GetDlgItem(st->hwnd, IDC_TYPEDESC), i18n::T(descKeys[sel]));
    }
}

// ---------------- WndProc ----------------

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
            DlgState* st = reinterpret_cast<DlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            st->hwnd = hwnd;
            st->font = util::CreateUiFont(util::DpiOf(hwnd), 0);

            SetWindowTextW(hwnd, i18n::T("settings.title"));

            st->tab = CreateWindowExW(0, WC_TABCONTROLW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                      8, 6, 480, 256, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_TAB)),
                                      nullptr, nullptr);
            SendMessageW(st->tab, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
            TCITEMW ti{};
            ti.mask = TCIF_TEXT;
            ti.pszText = const_cast<LPWSTR>(i18n::T("settings.tab.general"));
            TabCtrl_InsertItem(st->tab, 0, &ti);
            ti.pszText = const_cast<LPWSTR>(i18n::T("settings.tab.types"));
            TabCtrl_InsertItem(st->tab, 1, &ti);
            ti.pszText = const_cast<LPWSTR>(i18n::T("settings.tab.shortcuts"));
            TabCtrl_InsertItem(st->tab, 2, &ti);

            BuildGeneral(st);
            BuildTypes(st);
            BuildShortcuts(st);
            ShowPage(st, 0);

            // OK / Cancel（始终可见，不属于任何页）
            HWND ok = CreateWindowExW(0, L"BUTTON", i18n::T("settings.ok"),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 310,
                                      268, 80, 26, hwnd,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OK)), nullptr,
                                      nullptr);
            SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
            HWND cancel = CreateWindowExW(0, L"BUTTON", i18n::T("settings.cancel"),
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 398,
                                          268, 80, 26, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CANCEL)),
                                          nullptr, nullptr);
            SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
            return 0;
        }
        case WM_COMMAND: {
            DlgState* st = StateOf(hwnd);
            if (!st) {
                break;
            }
            int id = LOWORD(wparam);
            int code = HIWORD(wparam);
            if (id == IDC_OK) {
                Collect(st);
                st->resultOk = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_CANCEL) {
                st->resultOk = false;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == IDC_FONT_BTN) {
                DoFont(st);
                return 0;
            }
            if (id == IDC_FONT_RESET) {
                st->cfg->fontName.clear();
                st->cfg->fontSize = 0;
                SetWindowTextW(GetDlgItem(hwnd, IDC_FONT_BTN), i18n::T("settings.font"));
                return 0;
            }
            if (id == IDC_DATADIR_BTN) {
                DoBrowseDir(st);
                return 0;
            }
            if (id == IDC_TYPELIST && code == LBN_SELCHANGE) {
                DoTypeDesc(st);
                return 0;
            }
            break;
        }
        case WM_NOTIFY: {
            NMHDR* nm = reinterpret_cast<NMHDR*>(lparam);
            if (nm->idFrom == IDC_TAB && nm->code == TCN_SELCHANGE) {
                DlgState* st = StateOf(hwnd);
                if (st) {
                    ShowPage(st, TabCtrl_GetCurSel(st->tab));
                }
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            SetBkMode(hdc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
        }
        case WM_CLOSE: {
            DlgState* st = StateOf(hwnd);
            if (st) {
                st->resultOk = false;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

// ------------------------------------------------------------------ 公共接口

Config Defaults() {
    Config cfg;
    cfg.popupHotkey = hotkey::Make(MOD_CONTROL | MOD_ALT, 'V');
    return cfg;
}

void Clamp(Config& cfg) {
    cfg.maxHistory = std::clamp(cfg.maxHistory, 5, 2000);
    cfg.expiryDays = std::clamp(cfg.expiryDays, 0, 3650);
    cfg.pasteDelayMs = std::clamp(cfg.pasteDelayMs, 0, 2000);
    cfg.rowsVisible = std::clamp(cfg.rowsVisible, 4, 25);
    cfg.popupPosition = std::clamp(cfg.popupPosition, 0, 2);
    if (cfg.maxTextBytes < 1024u) {
        cfg.maxTextBytes = 1024u;
    }
    if (cfg.maxTextBytes > 64u * 1024u * 1024u) {
        cfg.maxTextBytes = 64u * 1024u * 1024u;
    }
    if (cfg.maxImagePixels < 65536u) {
        cfg.maxImagePixels = 65536u;
    }
    cfg.largeItemThresholdMB = std::clamp(cfg.largeItemThresholdMB, 1, 500);
}

void Load(Config& cfg) {
    cfg = Defaults();
    std::vector<uint8_t> raw;
    if (!util::ReadWholeFile(util::ConfigPath(), raw)) {
        return;
    }
    size_t start = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) {
        start = 3;
    }
    std::string content(reinterpret_cast<const char*>(raw.data()) + start, raw.size() - start);

    auto getStr = [&](const char* key) -> std::string {
        std::string needle = std::string(key) + "=";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) {
            return {};
        }
        pos += needle.size();
        size_t eol = content.find('\n', pos);
        std::string val =
            content.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) {
            val.pop_back();
        }
        return val;
    };
    auto getInt = [&](const char* key, int def) -> int {
        std::string s = getStr(key);
        return s.empty() ? def : atoi(s.c_str());
    };
    auto getU32 = [&](const char* key, uint32_t def) -> uint32_t {
        std::string s = getStr(key);
        return s.empty() ? def : static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 10));
    };

    cfg.maxHistory = getInt("MaxHistory", cfg.maxHistory);
    cfg.expiryDays = getInt("ExpiryDays", cfg.expiryDays);
    cfg.pasteDelayMs = getInt("PasteDelayMs", cfg.pasteDelayMs);
    cfg.rowsVisible = getInt("RowsVisible", cfg.rowsVisible);
    cfg.theme = static_cast<ThemeMode>(getInt("Theme", 0));
    cfg.popupPosition = getInt("PopupPosition", 0);
    cfg.lastPopupX = getInt("LastPopupX", -1);
    cfg.lastPopupY = getInt("LastPopupY", -1);
    cfg.fontSize = getInt("FontSize", 0);
    cfg.maxTextBytes = getU32("MaxTextBytes", cfg.maxTextBytes);
    cfg.maxImagePixels = getU32("MaxImagePixels", cfg.maxImagePixels);
    cfg.largeItemThresholdMB = getInt("LargeItemThresholdMB", cfg.largeItemThresholdMB);

    std::string hk = getStr("PopupHotkey");
    if (!hk.empty()) {
        cfg.popupHotkey = hotkey::FromText(Widen(hk));
    }
    for (int i = 0; i < 10; ++i) {
        char key[32];
        sprintf_s(key, "PinnedHotkey%d", i + 1);
        std::string s = getStr(key);
        if (!s.empty()) {
            cfg.pinnedHotkeys[i] = hotkey::FromText(Widen(s));
        }
    }
    std::string lang = getStr("Language");
    if (!lang.empty()) {
        cfg.language = Widen(lang);
    }
    std::string dir = getStr("DataDir");
    if (!dir.empty()) {
        cfg.dataDir = Widen(dir);
    }
    std::string font = getStr("FontName");
    if (!font.empty()) {
        cfg.fontName = Widen(font);
    }
    Clamp(cfg);
}

bool Save(const Config& cfg) {
    std::string ini;
    ini += "# ClipWiz config\n";
    ini += "MaxHistory=" + std::to_string(cfg.maxHistory) + "\n";
    ini += "ExpiryDays=" + std::to_string(cfg.expiryDays) + "\n";
    ini += "PasteDelayMs=" + std::to_string(cfg.pasteDelayMs) + "\n";
    ini += "RowsVisible=" + std::to_string(cfg.rowsVisible) + "\n";
    ini += "Theme=" + std::to_string(static_cast<int>(cfg.theme)) + "\n";
    ini += "PopupPosition=" + std::to_string(cfg.popupPosition) + "\n";
    ini += "LastPopupX=" + std::to_string(cfg.lastPopupX) + "\n";
    ini += "LastPopupY=" + std::to_string(cfg.lastPopupY) + "\n";
    ini += "FontSize=" + std::to_string(cfg.fontSize) + "\n";
    ini += "MaxTextBytes=" + std::to_string(cfg.maxTextBytes) + "\n";
    ini += "MaxImagePixels=" + std::to_string(cfg.maxImagePixels) + "\n";
    ini += "LargeItemThresholdMB=" + std::to_string(cfg.largeItemThresholdMB) + "\n";

    std::wstring hkText = hotkey::ToText(cfg.popupHotkey);
    ini += "PopupHotkey=" + Narrow(hkText) + "\n";
    for (int i = 0; i < 10; ++i) {
        if (cfg.pinnedHotkeys[i] != 0) {
            std::wstring t = hotkey::ToText(cfg.pinnedHotkeys[i]);
            ini += "PinnedHotkey" + std::to_string(i + 1) + "=" + Narrow(t) + "\n";
        }
    }
    if (!cfg.language.empty()) {
        ini += "Language=" + Narrow(cfg.language) + "\n";
    }
    if (!cfg.dataDir.empty()) {
        ini += "DataDir=" + Narrow(cfg.dataDir) + "\n";
    }
    if (!cfg.fontName.empty()) {
        ini += "FontName=" + Narrow(cfg.fontName) + "\n";
    }
    return util::WriteFileAtomic(util::ConfigPath(), ini.data(), ini.size());
}

bool ShowDialog(HWND owner, HINSTANCE inst, Config& cfg) {
    const wchar_t kClass[] = L"ClipWizSettings";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    DlgState st;
    st.cfg = &cfg;

    int dpi = util::DpiOf(owner);
    int w = util::Scale(504, dpi);
    int h = util::Scale(310, dpi);
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, kClass, L"", WS_POPUP | WS_CAPTION |
                                WS_SYSMENU, (sw - w) / 2, (sh - h) / 2, w, h, owner, nullptr,
                                inst, &st);
    if (!hwnd) {
        UnregisterClassW(kClass, inst);
        return false;
    }

    EnableWindow(owner, FALSE);
    ShowWindow(hwnd, SW_SHOW);

    MSG msgLoop{};
    while (GetMessageW(&msgLoop, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msgLoop)) {
            TranslateMessage(&msgLoop);
            DispatchMessageW(&msgLoop);
        }
    }

    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    UnregisterClassW(kClass, inst);
    return st.resultOk;
}

}  // namespace settings
