// settings.cpp — Settings dialog: single centered window with grouped sections
#include "settings.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>

#include "resource.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "hotkey.h"
#include "i18n.h"
#include "log.h"
#include "util.h"

namespace settings {
namespace {

// Settings-local aliases (no need to duplicate util conversions)
using util::Narrow;
using util::Widen;

// Dialog state
Config* g_cfg = nullptr;
bool g_resultOk = false;
HFONT g_font = nullptr;
HFONT g_fontBold = nullptr;
HWND g_settingsDlg = nullptr;
int g_dpi = 96;
HICON g_winKeyIcon = nullptr;
HWND g_tooltip = nullptr;

// Lazily create the shared tooltip control (owned by the dialog) and return it.
HWND EnsureTooltip(HWND owner) {
    if (!g_tooltip) {
        g_tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                                    WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    CW_USEDEFAULT, CW_USEDEFAULT,
                                    owner, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (g_tooltip) {
            SetWindowPos(g_tooltip, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SendMessageW(g_tooltip, TTM_SETMAXTIPWIDTH, 0, 300);
        }
    }
    return g_tooltip;
}

// Attach a hover tooltip to a control. TTF_SUBCLASS lets the tooltip handle
// mouse messages itself, so we don't need to forward from the dialog proc.
void AddTooltip(HWND owner, HWND ctrl, const wchar_t* text) {
    HWND tip = EnsureTooltip(owner);
    if (!tip || !ctrl) return;
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = reinterpret_cast<UINT_PTR>(ctrl);
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

int Dip(int value) {
    return MulDiv(value, g_dpi, 96);
}

HWND MakeCtrl(HWND parent, const wchar_t* cls, const wchar_t* text, DWORD style,
              int x, int y, int w, int h, int id, DWORD exStyle = 0) {
    HWND hwnd = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style,
                                Dip(x), Dip(y), Dip(w), Dip(h),
                                parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                GetModuleHandleW(nullptr), nullptr);
    if (hwnd && g_font) {
        SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    }
    return hwnd;
}

HWND MakeLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    return MakeCtrl(parent, L"STATIC", text, SS_LEFT, x, y, w, h, -1);
}

HICON LoadWinKeyIcon() {
    if (!g_winKeyIcon) {
        g_winKeyIcon = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_WINBADGE), IMAGE_ICON,
                       Dip(16), Dip(16), LR_DEFAULTCOLOR | LR_SHARED));
    }
    return g_winKeyIcon;
}

HWND MakeWinKeyCheckbox(HWND parent, int x, int y, int w, int h, int id, bool checked) {
    HICON icon = LoadWinKeyIcon();
    HWND hwnd = MakeCtrl(parent, L"BUTTON", icon ? L"" : i18n::T("settings.win_key"),
                         BS_AUTOCHECKBOX | BS_ICON | WS_TABSTOP, x, y, w, h, id);
    if (hwnd && icon) {
        SendMessageW(hwnd, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(icon));
    }
    if (checked) {
        CheckDlgButton(parent, id, BST_CHECKED);
    }
    if (hwnd) {
        AddTooltip(parent, hwnd, i18n::T("settings.win_key_tip"));
    }
    return hwnd;
}

// Returns next Y position after group header
int MakeGroupHeader(HWND parent, const wchar_t* text, int y, int width) {
    HWND label = MakeCtrl(parent, L"STATIC", text, SS_LEFT, 10, y, width, 14, -1);
    if (label && g_fontBold) {
        SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(g_fontBold), FALSE);
    }
    MakeCtrl(parent, L"STATIC", L"", SS_ETCHEDHORZ, 10, y + 16, width, 2, -1);
    return y + 22;
}

void PopulateControls(HWND hwnd) {
    const Config& cfg = *g_cfg;

    // Layout constants (DIP) — generous sizing for readability
    constexpr int kLabelW = 120;
    constexpr int kFieldX = 135;
    constexpr int kFieldW = 220;
    constexpr int kRowH = 28;       // Row height for General section (1.5x original)
    constexpr int kHkRowH = 30;     // Row height for Shortcuts section (larger)
    constexpr int kEditH = 22;      // Edit/combo/hotkey control height
    constexpr int kPad = 12;
    constexpr int kDlgW = 590;
    constexpr int kCheckboxH = 16;
    constexpr int kCheckboxOffsetY = (kEditH - kCheckboxH) / 2;
    constexpr int kWinCheckboxW = 34;
    constexpr int kPinnedHkW = 190;
    constexpr int kActionBtnW = 110;
    constexpr int kGroupW = kDlgW - 20;
    constexpr int kFontFieldW = kFieldW + 2;
    constexpr int kFontFieldX = kFieldX - 1;

    int y = kPad;

    // ===================== Section 1: General =====================
    y = MakeGroupHeader(hwnd, i18n::T("settings.tab.general"), y, kGroupW);

    // Autostart checkbox
    MakeCtrl(hwnd, L"BUTTON", i18n::T("settings.autostart"),
             BS_AUTOCHECKBOX | WS_TABSTOP, kPad, y, 280, 16, IDC_AUTOSTART);
    if (util::GetAutostart())
        CheckDlgButton(hwnd, IDC_AUTOSTART, BST_CHECKED);
    y += kRowH;

    // Max saved items
    MakeLabel(hwnd, i18n::T("settings.max_history"), kPad, y + 2, kLabelW, 16);
    MakeCtrl(hwnd, L"EDIT", util::Format(L"%d", cfg.maxHistory).c_str(),
             ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
             kFieldX, y, 60, kEditH, IDC_MAXHISTORY, WS_EX_CLIENTEDGE);
    y += kRowH;

    // Expiry days
    MakeLabel(hwnd, i18n::T("settings.expiry_days"), kPad, y + 2, kLabelW, 16);
    MakeCtrl(hwnd, L"EDIT", util::Format(L"%d", cfg.expiryDays).c_str(),
             ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP,
             kFieldX, y, 60, kEditH, IDC_EXPIRYDAYS, WS_EX_CLIENTEDGE);
    MakeCtrl(hwnd, L"BUTTON", i18n::T("settings.clean_on_exit"),
             BS_AUTOCHECKBOX | WS_TABSTOP,
             kFieldX + 66, y + kCheckboxOffsetY, 120, kCheckboxH, IDC_CLEAN_ON_EXIT);
    if (cfg.cleanOnExit)
        CheckDlgButton(hwnd, IDC_CLEAN_ON_EXIT, BST_CHECKED);
    // Enable/disable expiry days based on cleanOnExit
    EnableWindow(GetDlgItem(hwnd, IDC_EXPIRYDAYS), !cfg.cleanOnExit);
    y += kRowH;

    // Language
    MakeLabel(hwnd, i18n::T("settings.language"), kPad, y + 2, kLabelW, 16);
    HWND cbLang = MakeCtrl(hwnd, L"COMBOBOX", L"",
                           CBS_DROPDOWNLIST | WS_TABSTOP,
                           kFieldX, y, kFieldW, 120, IDC_LANGUAGE);
    SendMessageW(cbLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.lang_auto")));
    SendMessageW(cbLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"English"));
    SendMessageW(cbLang, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"简体中文"));
    int langSel = 0;
    if (cfg.language == L"en") langSel = 1;
    else if (cfg.language == L"zh-CN") langSel = 2;
    SendMessageW(cbLang, CB_SETCURSEL, langSel, 0);
    y += kRowH;

    // Theme
    MakeLabel(hwnd, i18n::T("settings.theme"), kPad, y + 2, kLabelW, 16);
    HWND cbTheme = MakeCtrl(hwnd, L"COMBOBOX", L"",
                            CBS_DROPDOWNLIST | WS_TABSTOP,
                            kFieldX, y, kFieldW, 120, IDC_THEME);
    SendMessageW(cbTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.theme_auto")));
    SendMessageW(cbTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.theme_light")));
    SendMessageW(cbTheme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.theme_dark")));
    SendMessageW(cbTheme, CB_SETCURSEL, static_cast<int>(cfg.theme), 0);
    y += kRowH;

    // Popup position
    MakeLabel(hwnd, i18n::T("settings.popup_pos"), kPad, y + 2, kLabelW, 16);
    HWND cbPos = MakeCtrl(hwnd, L"COMBOBOX", L"",
                          CBS_DROPDOWNLIST | WS_TABSTOP,
                          kFieldX, y, kFieldW, 120, IDC_POPUPPOS);
    SendMessageW(cbPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.pos_mouse")));
    SendMessageW(cbPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.pos_caret")));
    SendMessageW(cbPos, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T("settings.pos_last")));
    SendMessageW(cbPos, CB_SETCURSEL, cfg.popupPosition, 0);
    // Shares the row with the position combo, the way "Clean on Exit" shares
    // the expiry-days row: both are one-checkbox popup behaviours and giving
    // each its own row would stretch the dialog for a single tick box.
    MakeCtrl(hwnd, L"BUTTON", i18n::T("settings.hover_preview"),
             BS_AUTOCHECKBOX | WS_TABSTOP,
             kFieldX + kFieldW + 6, y + kCheckboxOffsetY, 150, kCheckboxH, IDC_HOVER_PREVIEW);
    if (cfg.hoverPreview)
        CheckDlgButton(hwnd, IDC_HOVER_PREVIEW, BST_CHECKED);
    y += kRowH;

    // Merge separator: what goes between two text items combined into one. File
    // lists ignore this and always join on a newline (their format is fixed), so
    // it only governs text merges. Choosing "Custom" enables the field beside the
    // combo; the other three presets carry their own literal separator.
    MakeLabel(hwnd, i18n::T("settings.merge_sep"), kPad, y + 2, kLabelW, 16);
    HWND cbMerge = MakeCtrl(hwnd, L"COMBOBOX", L"",
                            CBS_DROPDOWNLIST | WS_TABSTOP,
                            kFieldX, y, kFieldW, 120, IDC_MERGE_SEP);
    SendMessageW(cbMerge, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(i18n::T("settings.merge_sep_blank")));
    SendMessageW(cbMerge, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(i18n::T("settings.merge_sep_newline")));
    SendMessageW(cbMerge, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(i18n::T("settings.merge_sep_space")));
    SendMessageW(cbMerge, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(i18n::T("settings.merge_sep_custom")));
    SendMessageW(cbMerge, CB_SETCURSEL, static_cast<int>(cfg.mergeSep), 0);
    MakeCtrl(hwnd, L"EDIT", cfg.mergeSepCustom.c_str(),
             ES_AUTOHSCROLL | WS_TABSTOP,
             kFieldX + kFieldW + 6, y, kActionBtnW + 40, kEditH, IDC_MERGE_SEP_CUSTOM,
             WS_EX_CLIENTEDGE);
    EnableWindow(GetDlgItem(hwnd, IDC_MERGE_SEP_CUSTOM),
                 cfg.mergeSep == merge::Separator::Custom);
    y += kRowH;

    // Data storage location
    MakeLabel(hwnd, i18n::T("settings.data_dir"), kPad, y + 2, kLabelW, 16);
    HWND cbData = MakeCtrl(hwnd, L"COMBOBOX", L"",
                           CBS_DROPDOWNLIST | WS_TABSTOP,
                           kFieldX, y, kFieldW, 120, IDC_DATADIR);
    SendMessageW(cbData, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(i18n::T("settings.data_installed")));
    SendMessageW(cbData, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(i18n::T("settings.data_portable")));
    SendMessageW(cbData, CB_SETCURSEL, util::IsPortable() ? 1 : 0, 0);
    MakeCtrl(hwnd, L"BUTTON", i18n::T("settings.open_dir"),
             BS_PUSHBUTTON | WS_TABSTOP, kFieldX + kFieldW + 6, y, kActionBtnW, kEditH, IDC_DATADIR_OPEN);
    y += kRowH;

    // Display font
    MakeLabel(hwnd, i18n::T("settings.font"), kPad, y + 2, kLabelW, 16);
    std::wstring fontText = cfg.fontName.empty()
        ? std::wstring(i18n::T("settings.font_default_val"))
        : util::Format(L"%s  %dpt", cfg.fontName.c_str(), cfg.fontSize);
    MakeCtrl(hwnd, L"BUTTON", fontText.c_str(),
             BS_PUSHBUTTON | WS_TABSTOP, kFontFieldX, y, kFontFieldW, kEditH, IDC_FONT_BTN);
    MakeCtrl(hwnd, L"BUTTON", i18n::T("settings.font_default"),
             BS_PUSHBUTTON | WS_TABSTOP, kFieldX + kFieldW + 6, y, kActionBtnW, kEditH, IDC_FONT_RESET);
    y += kRowH + 8;

    // ===================== Section 2: Shortcuts =====================
    y = MakeGroupHeader(hwnd, i18n::T("settings.tab.shortcuts"), y, kGroupW);

    // Popup hotkey
    MakeLabel(hwnd, i18n::T("settings.popup_hotkey"), kPad, y + 2, kLabelW, 16);
    HWND hkPopup = MakeCtrl(hwnd, HOTKEY_CLASSW, L"",
                            WS_TABSTOP,
                            kFieldX, y, kFieldW, kEditH, IDC_POPUP_HK, WS_EX_CLIENTEDGE);
    SendMessageW(hkPopup, HKM_SETHOTKEY, hotkey::ToControl(cfg.popupHotkey), 0);
    MakeWinKeyCheckbox(hwnd, kFieldX + kFieldW + 6, y + kCheckboxOffsetY,
                       kWinCheckboxW, kCheckboxH, IDC_POPUP_WIN,
                       (hotkey::ModsOf(cfg.popupHotkey) & MOD_WIN) != 0);
    y += kHkRowH + 6;

    // Paste-queue hotkey: each press pastes the next queued item. Sits right
    // under the popup hotkey because both are global action chords (the pinned
    // hotkeys below are positional).
    MakeLabel(hwnd, i18n::T("settings.queue_hotkey"), kPad, y + 2, kLabelW, 16);
    HWND hkQueue = MakeCtrl(hwnd, HOTKEY_CLASSW, L"",
                            WS_TABSTOP,
                            kFieldX, y, kFieldW, kEditH, IDC_QUEUE_HK, WS_EX_CLIENTEDGE);
    SendMessageW(hkQueue, HKM_SETHOTKEY, hotkey::ToControl(cfg.queueHotkey), 0);
    MakeWinKeyCheckbox(hwnd, kFieldX + kFieldW + 6, y + kCheckboxOffsetY,
                       kWinCheckboxW, kCheckboxH, IDC_QUEUE_WIN,
                       (hotkey::ModsOf(cfg.queueHotkey) & MOD_WIN) != 0);
    y += kHkRowH + 6;

    // Paste keystroke. The two entries are key names, not prose — every locale
    // writes them the same way — so they are not run through i18n.
    MakeLabel(hwnd, i18n::T("settings.paste_key"), kPad, y + 2, kLabelW, 16);
    HWND cbPasteKey = MakeCtrl(hwnd, L"COMBOBOX", L"",
                               CBS_DROPDOWNLIST | WS_TABSTOP,
                               kFieldX, y, kFieldW, 120, IDC_PASTE_KEY);
    SendMessageW(cbPasteKey, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ctrl+V"));
    SendMessageW(cbPasteKey, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Shift+Insert"));
    SendMessageW(cbPasteKey, CB_SETCURSEL, static_cast<int>(cfg.pasteKey), 0);
    y += kRowH + 6;
    MakeLabel(hwnd, i18n::T("settings.pinned_group"), kPad, y, 220, 16);
    y += 20;

    // 10 pinned hotkeys in 2 columns (5 per column)
    constexpr int kPinnedGroupW = 22 + kPinnedHkW + 6 + kWinCheckboxW;
    constexpr int kPinnedCol1X = kPad;
    constexpr int kPinnedVisualCompensation = 8;
    constexpr int kPinnedCol2X = kDlgW - kPad - kPinnedGroupW - kPinnedVisualCompensation;
    for (int i = 0; i < 10; ++i) {
        int col = i / 5;
        int row = i % 5;
        int cx = (col == 0) ? kPinnedCol1X : kPinnedCol2X;
        int cy = y + row * kHkRowH;

        std::wstring numLabel = util::Format(L"%d:", i + 1);
        MakeLabel(hwnd, numLabel.c_str(), cx, cy + 2, 22, 16);
        HWND hk = MakeCtrl(hwnd, HOTKEY_CLASSW, L"",
                           WS_TABSTOP,
                           cx + 25, cy, kPinnedHkW, kEditH, IDC_PIN_HK_BASE + i, WS_EX_CLIENTEDGE);
        SendMessageW(hk, HKM_SETHOTKEY, hotkey::ToControl(cfg.pinnedHotkeys[i]), 0);
        MakeWinKeyCheckbox(hwnd, cx + 25 + kPinnedHkW + 6, cy + kCheckboxOffsetY,
                           kWinCheckboxW, kCheckboxH, IDC_PIN_WIN_BASE + i,
                           (hotkey::ModsOf(cfg.pinnedHotkeys[i]) & MOD_WIN) != 0);
    }
    y += 5 * kHkRowH + 10;

    // ===================== Section 3: Privacy =====================
    y = MakeGroupHeader(hwnd, i18n::T("settings.tab.privacy"), y, kGroupW);

    // One rule per line; a leading '!' means "also ignore hotkeys here", '*' is
    // a wildcard, '#' starts a comment. The hint states exactly that so the box
    // is usable without opening the docs.
    MakeLabel(hwnd, i18n::T("settings.blocklist_hint"), kPad, y, kGroupW - 20, 46);
    y += 48;

    HWND edBlock = MakeCtrl(hwnd, L"EDIT", L"",
                            ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | WS_VSCROLL | WS_TABSTOP,
                            kPad, y, kGroupW - 20, 100, IDC_BLOCKLIST, WS_EX_CLIENTEDGE);
    // Stored form is LF; a multiline edit control wants CRLF to show line breaks.
    {
        std::wstring text;
        text.reserve(cfg.blockRules.size() + 16);
        for (wchar_t c : cfg.blockRules) {
            if (c == L'\n') {
                text += L"\r\n";
            } else if (c != L'\r') {
                text += c;
            }
        }
        SetWindowTextW(edBlock, text.c_str());
    }
    y += 100 + kPad;

    // Preview desensitization: hide sensitive spans in the list and the hover
    // preview. Five independent toggles in a two-column grid. What actually gets
    // pasted is never changed — this only governs what is readable on screen.
    // The defaults (phone / ID / password on) come from mask::Config.
    MakeLabel(hwnd, i18n::T("settings.mask_hint"), kPad, y, kGroupW - 20, 30);
    y += 32;
    const int kMaskColW = (kGroupW - 20) / 2;
    auto maskCheck = [&](int col, int row, int id, const wchar_t* label, bool on) {
        MakeCtrl(hwnd, L"BUTTON", label, BS_AUTOCHECKBOX | WS_TABSTOP,
                 kPad + col * kMaskColW, y + row * 22, kMaskColW - 6, 16, id);
        if (on) CheckDlgButton(hwnd, id, BST_CHECKED);
    };
    maskCheck(0, 0, IDC_MASK_PHONE,    i18n::T("settings.mask_phone"),    cfg.mask.phone);
    maskCheck(1, 0, IDC_MASK_IDCARD,   i18n::T("settings.mask_idcard"),   cfg.mask.idCard);
    maskCheck(0, 1, IDC_MASK_PASSWORD, i18n::T("settings.mask_password"), cfg.mask.password);
    maskCheck(1, 1, IDC_MASK_EMAIL,    i18n::T("settings.mask_email"),    cfg.mask.email);
    maskCheck(0, 2, IDC_MASK_APIKEY,   i18n::T("settings.mask_apikey"),   cfg.mask.apiKey);
    y += 3 * 22 + kPad;

    // ===================== Footer: OK / Cancel =====================
    int btnW = 85;
    int footBtnH = 26;
    int btnY = y;
    int btnX = (kDlgW - btnW * 2 - 12) / 2;
    MakeCtrl(hwnd, L"BUTTON", i18n::T("settings.ok"),
             BS_DEFPUSHBUTTON | WS_TABSTOP, btnX, btnY, btnW, footBtnH, IDOK);
    MakeCtrl(hwnd, L"BUTTON", i18n::T("settings.cancel"),
             BS_PUSHBUTTON | WS_TABSTOP, btnX + btnW + 12, btnY, btnW, footBtnH, IDCANCEL);
    y += footBtnH + kPad;

    // Resize dialog to fit content
    RECT rcClient = {0, 0, Dip(kDlgW), Dip(y)};
    AdjustWindowRectEx(&rcClient, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                       GetWindowLongW(hwnd, GWL_EXSTYLE));
    int dlgW = rcClient.right - rcClient.left;
    int dlgH = rcClient.bottom - rcClient.top;

    // Center on screen
    RECT workArea;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    int screenCx = workArea.left + (workArea.right - workArea.left - dlgW) / 2;
    int screenCy = workArea.top + (workArea.bottom - workArea.top - dlgH) / 2;
    SetWindowPos(hwnd, nullptr, screenCx, screenCy, dlgW, dlgH, SWP_NOZORDER);
}

// Validate the shortcuts before any of them is committed. The OK handler reads
// the controls into locals and calls this first: cfg aliases the live global
// config, so a hotkey that fails here must never reach it. An unbound slot
// (vk == 0) is allowed and skipped; a bound one must carry a real modifier, must
// not be a combination other programs depend on, and must not collide with the
// popup hotkey. On the first problem this shows why and returns false so the
// dialog stays open for the user to fix it — nothing has been written yet.
bool HotkeysAcceptable(HWND hwnd, uint32_t popup, uint32_t queue,
                       const uint32_t (&pinned)[10]) {
    const wchar_t* title = i18n::T("settings.title");
    const uint32_t all[12] = {popup,     queue,     pinned[0], pinned[1], pinned[2],
                              pinned[3], pinned[4], pinned[5], pinned[6], pinned[7],
                              pinned[8], pinned[9]};
    for (uint32_t code : all) {
        if (hotkey::VkOf(code) == 0) {
            continue;  // unbound — nothing to register, nothing to reject
        }
        if (!hotkey::IsUsable(code)) {
            MessageBoxW(hwnd, i18n::T("msg.need_modifier"), title, MB_OK | MB_ICONEXCLAMATION);
            return false;
        }
        std::wstring why;
        if (hotkey::LooksRisky(code, why)) {
            MessageBoxW(hwnd, why.c_str(), title, MB_OK | MB_ICONEXCLAMATION);
            return false;
        }
    }
    // The popup hotkey is the anchor: nothing else may reuse it or the press
    // would be ambiguous. Only meaningful when the popup slot is bound.
    if (hotkey::VkOf(popup) != 0) {
        if (queue == popup) {
            MessageBoxW(hwnd, i18n::T("msg.same_as_popup"), title, MB_OK | MB_ICONEXCLAMATION);
            return false;
        }
        for (uint32_t code : pinned) {
            if (code == popup) {
                MessageBoxW(hwnd, i18n::T("msg.same_as_popup"), title, MB_OK | MB_ICONEXCLAMATION);
                return false;
            }
        }
    }
    // Any other duplicate pair is ambiguous too: the queue slot against a pinned
    // slot, or two pinned slots sharing one combination. Collisions with the
    // popup are already caught above with a more specific message. Unbound slots
    // (vk == 0) are skipped so the empty pinned positions don't match each other.
    if (hotkey::VkOf(queue) != 0) {
        for (uint32_t code : pinned) {
            if (code == queue) {
                MessageBoxW(hwnd, i18n::T("msg.hotkey_duplicate"), title, MB_OK | MB_ICONEXCLAMATION);
                return false;
            }
        }
    }
    for (int i = 0; i < 10; ++i) {
        if (hotkey::VkOf(pinned[i]) == 0) {
            continue;
        }
        for (int j = i + 1; j < 10; ++j) {
            if (pinned[j] == pinned[i]) {
                MessageBoxW(hwnd, i18n::T("msg.hotkey_duplicate"), title, MB_OK | MB_ICONEXCLAMATION);
                return false;
            }
        }
    }
    return true;
}

INT_PTR CALLBACK SettingsDlgProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    (void)lparam;
    switch (msg) {
        case WM_INITDIALOG:
            g_settingsDlg = hwnd;
            PopulateControls(hwnd);
            return TRUE;
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (id == IDOK) {
                Config& cfg = *g_cfg;
                // Shortcuts are read into locals and validated before ANY field is
                // committed. cfg aliases the live global config, so a rejected
                // hotkey must not reach it — and neither should the other edits in
                // this dialog. On failure keep the dialog open (return TRUE) with
                // cfg untouched so the user can fix the combination.
                WORD raw = static_cast<WORD>(
                    SendDlgItemMessageW(hwnd, IDC_POPUP_HK, HKM_GETHOTKEY, 0, 0));
                bool win = IsDlgButtonChecked(hwnd, IDC_POPUP_WIN) == BST_CHECKED;
                uint32_t popupHk = hotkey::FromControl(raw, win);
                WORD qraw = static_cast<WORD>(
                    SendDlgItemMessageW(hwnd, IDC_QUEUE_HK, HKM_GETHOTKEY, 0, 0));
                bool qwin = IsDlgButtonChecked(hwnd, IDC_QUEUE_WIN) == BST_CHECKED;
                uint32_t queueHk = hotkey::FromControl(qraw, qwin);
                uint32_t pinnedHk[10] = {};
                for (int i = 0; i < 10; ++i) {
                    WORD r = static_cast<WORD>(
                        SendDlgItemMessageW(hwnd, IDC_PIN_HK_BASE + i, HKM_GETHOTKEY, 0, 0));
                    bool w = IsDlgButtonChecked(hwnd, IDC_PIN_WIN_BASE + i) == BST_CHECKED;
                    pinnedHk[i] = hotkey::FromControl(r, w);
                }
                if (!HotkeysAcceptable(hwnd, popupHk, queueHk, pinnedHk)) {
                    return TRUE;
                }
                cfg.popupHotkey = popupHk;
                cfg.queueHotkey = queueHk;
                for (int i = 0; i < 10; ++i) {
                    cfg.pinnedHotkeys[i] = pinnedHk[i];
                }
                cfg.maxHistory = GetDlgItemInt(hwnd, IDC_MAXHISTORY, nullptr, FALSE);
                cfg.expiryDays = GetDlgItemInt(hwnd, IDC_EXPIRYDAYS, nullptr, FALSE);
                cfg.cleanOnExit = IsDlgButtonChecked(hwnd, IDC_CLEAN_ON_EXIT) == BST_CHECKED;
                cfg.hoverPreview = IsDlgButtonChecked(hwnd, IDC_HOVER_PREVIEW) == BST_CHECKED;
                int langSel = static_cast<int>(
                    SendMessageW(GetDlgItem(hwnd, IDC_LANGUAGE), CB_GETCURSEL, 0, 0));
                if (langSel == 1) cfg.language = L"en";
                else if (langSel == 2) cfg.language = L"zh-CN";
                else cfg.language = L"";
                cfg.theme = static_cast<ThemeMode>(
                    SendMessageW(GetDlgItem(hwnd, IDC_THEME), CB_GETCURSEL, 0, 0));
                cfg.popupPosition = static_cast<int>(
                    SendMessageW(GetDlgItem(hwnd, IDC_POPUPPOS), CB_GETCURSEL, 0, 0));
                bool autostart = IsDlgButtonChecked(hwnd, IDC_AUTOSTART) == BST_CHECKED;
                // CB_GETCURSEL returns -1 when nothing is selected; Clamp()
                // turns that back into the default rather than storing it.
                cfg.pasteKey = static_cast<paste::Key>(
                    SendMessageW(GetDlgItem(hwnd, IDC_PASTE_KEY), CB_GETCURSEL, 0, 0));
                // Blocklist rules: strip the CR the edit control adds, store LF.
                {
                    HWND edBlock = GetDlgItem(hwnd, IDC_BLOCKLIST);
                    const int len = GetWindowTextLengthW(edBlock);
                    std::wstring text;
                    if (len > 0) {
                        text.resize(static_cast<size_t>(len) + 1);
                        GetWindowTextW(edBlock, text.data(), len + 1);
                        text.resize(static_cast<size_t>(len));
                    }
                    std::wstring lf;
                    lf.reserve(text.size());
                    for (wchar_t c : text) {
                        if (c != L'\r') lf += c;
                    }
                    cfg.blockRules = lf;
                }
                // Preview desensitization toggles
                cfg.mask.phone = IsDlgButtonChecked(hwnd, IDC_MASK_PHONE) == BST_CHECKED;
                cfg.mask.idCard = IsDlgButtonChecked(hwnd, IDC_MASK_IDCARD) == BST_CHECKED;
                cfg.mask.password = IsDlgButtonChecked(hwnd, IDC_MASK_PASSWORD) == BST_CHECKED;
                cfg.mask.email = IsDlgButtonChecked(hwnd, IDC_MASK_EMAIL) == BST_CHECKED;
                cfg.mask.apiKey = IsDlgButtonChecked(hwnd, IDC_MASK_APIKEY) == BST_CHECKED;
                // Merge separator: mode from the combo, literal from the custom
                // field. CB_GETCURSEL is -1 if nothing is selected; Clamp()
                // restores the default. The custom edit is single-line, so there
                // is no CR to strip and nothing that could break the ini format.
                cfg.mergeSep = static_cast<merge::Separator>(
                    SendDlgItemMessageW(hwnd, IDC_MERGE_SEP, CB_GETCURSEL, 0, 0));
                wchar_t mergeCustom[64] = {};
                GetDlgItemTextW(hwnd, IDC_MERGE_SEP_CUSTOM, mergeCustom, 64);
                cfg.mergeSepCustom = mergeCustom;
                // Data storage mode migration (must succeed before committing)
                int dataSel = static_cast<int>(
                    SendDlgItemMessageW(hwnd, IDC_DATADIR, CB_GETCURSEL, 0, 0));
                bool wantPortable = (dataSel == 1);
                if (wantPortable != util::IsPortable()) {
                    if (!util::MigrateDataDir(wantPortable, hwnd)) {
                        // Migration failed or user cancelled — don't commit settings
                        return TRUE;
                    }
                }
                // Apply autostart only after migration succeeds (or wasn't needed)
                util::SetAutostart(autostart);
                g_resultOk = true;
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            if (id == IDCANCEL) {
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            }

            if (id == IDC_FONT_BTN) {
                LOGFONTW lf{};
                lf.lfCharSet = DEFAULT_CHARSET;
                if (!g_cfg->fontName.empty())
                    wcsncpy_s(lf.lfFaceName, g_cfg->fontName.c_str(), _TRUNCATE);
                if (g_cfg->fontSize > 0)
                    lf.lfHeight = -MulDiv(g_cfg->fontSize, g_dpi, 72);
                CHOOSEFONTW cf{};
                cf.lStructSize = sizeof(cf);
                cf.hwndOwner = hwnd;
                cf.lpLogFont = &lf;
                constexpr int kFontMinPt = 8;
                constexpr int kFontMaxPt = 28;
                cf.nSizeMin = kFontMinPt;
                cf.nSizeMax = kFontMaxPt;
                cf.Flags = CF_SCREENFONTS | CF_INITTOLOGFONTSTRUCT | CF_FORCEFONTEXIST
                         | CF_LIMITSIZE | CF_TTONLY | CF_NOVERTFONTS;
                for (;;) {
                    if (!ChooseFontW(&cf)) return TRUE;
                    if (lf.lfFaceName[0] != L'@') break;
                    MessageBoxW(hwnd,
                        i18n::T("msg.font_vertical_unsupported"),
                        i18n::T("msg.font_selection_title"),
                        MB_OK | MB_ICONEXCLAMATION);
                }
                g_cfg->fontName = lf.lfFaceName;
                g_cfg->fontSize = cf.iPointSize / 10;
                std::wstring t = util::Format(L"%s  %dpt",
                                              g_cfg->fontName.c_str(), g_cfg->fontSize);
                SetDlgItemTextW(hwnd, IDC_FONT_BTN, t.c_str());
                return TRUE;
            }
            if (id == IDC_FONT_RESET) {
                g_cfg->fontName.clear();
                g_cfg->fontSize = 0;
                SetDlgItemTextW(hwnd, IDC_FONT_BTN, i18n::T("settings.font_default_val"));
                return TRUE;
            }
            if (id == IDC_DATADIR_OPEN) {
                ShellExecuteW(hwnd, L"open", util::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return TRUE;
            }
            if (id == IDC_CLEAN_ON_EXIT) {
                bool checked = IsDlgButtonChecked(hwnd, IDC_CLEAN_ON_EXIT) == BST_CHECKED;
                EnableWindow(GetDlgItem(hwnd, IDC_EXPIRYDAYS), !checked);
                return TRUE;
            }
            if (id == IDC_MERGE_SEP) {
                // The custom field is only meaningful in Custom mode; grey it out
                // for the three presets so it is clear their separator is fixed.
                const int sel = static_cast<int>(
                    SendDlgItemMessageW(hwnd, IDC_MERGE_SEP, CB_GETCURSEL, 0, 0));
                EnableWindow(GetDlgItem(hwnd, IDC_MERGE_SEP_CUSTOM),
                             sel == static_cast<int>(merge::Separator::Custom));
                return TRUE;
            }
            break;
        }
        case WM_CLOSE:
            EndDialog(hwnd, IDCANCEL);
            return TRUE;
        case WM_DESTROY:
            g_settingsDlg = nullptr;
            if (g_tooltip) { DestroyWindow(g_tooltip); g_tooltip = nullptr; }
            if (g_font) { DeleteObject(g_font); g_font = nullptr; }
            if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }
            return TRUE;
    }
    return FALSE;
}

// Build minimal in-memory DLGTEMPLATE (no controls, we add them in WM_INITDIALOG)
std::vector<WORD> BuildEmptyDlgTemplate() {
    std::vector<WORD> buf;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    buf.push_back(LOWORD(style));
    buf.push_back(HIWORD(style));
    buf.push_back(0); buf.push_back(0);  // dwExtendedStyle
    buf.push_back(0);                     // cdit = 0
    buf.push_back(0);                     // x
    buf.push_back(0);                     // y
    buf.push_back(100);                   // cx (placeholder, resized in WM_INITDIALOG)
    buf.push_back(100);                   // cy (placeholder)
    buf.push_back(0);                     // menu = none
    buf.push_back(0);                     // class = default
    // Title string
    const wchar_t* title = i18n::T("settings.title");
    while (*title) { buf.push_back(static_cast<WORD>(*title)); ++title; }
    buf.push_back(0);
    // DS_SETFONT requires font info
    buf.push_back(9);                     // point size
    const wchar_t* fontName = L"Segoe UI";
    while (*fontName) { buf.push_back(static_cast<WORD>(*fontName)); ++fontName; }
    buf.push_back(0);
    return buf;
}

}  // namespace

// ------------------------------------------------------------------ Public interface

Config Defaults() {
    Config cfg;
    cfg.popupHotkey = hotkey::Make(MOD_CONTROL | MOD_ALT, 'K');
    cfg.queueHotkey = hotkey::Make(MOD_CONTROL | MOD_ALT, 'J');
    cfg.pinnedHotkeys[0] = hotkey::Make(MOD_CONTROL | MOD_ALT, '1');
    cfg.pinnedHotkeys[1] = hotkey::Make(MOD_CONTROL | MOD_ALT, '2');
    cfg.expiryDays = 5;
    cfg.logLevel = L"error";
    return cfg;
}

void Clamp(Config& cfg) {
    cfg.maxHistory = std::clamp(cfg.maxHistory, 5, 9999);
    cfg.expiryDays = std::clamp(cfg.expiryDays, 0, 3650);
    cfg.pasteDelayMs = std::clamp(cfg.pasteDelayMs, 0, 2000);
    // A value out of range here would make Execute() fall through to its
    // default, which hides a corrupt config.ini behind working behaviour.
    // Reset it explicitly so what is saved is what runs.
    if (cfg.pasteKey != paste::Key::CtrlV && cfg.pasteKey != paste::Key::ShiftInsert) {
        cfg.pasteKey = paste::Key::CtrlV;
    }
    cfg.rowsVisible = std::clamp(cfg.rowsVisible, 4, 25);
    cfg.popupPosition = std::clamp(cfg.popupPosition, 0, 2);
    if (cfg.maxTextBytes < 1024u) cfg.maxTextBytes = 1024u;
    if (cfg.maxTextBytes > 64u * 1024u * 1024u) cfg.maxTextBytes = 64u * 1024u * 1024u;
    if (cfg.maxImagePixels < 65536u) cfg.maxImagePixels = 65536u;
    cfg.largeItemThresholdMB = std::clamp(cfg.largeItemThresholdMB, 1, 500);
    if (cfg.fontSize > 0) cfg.fontSize = std::clamp(cfg.fontSize, 8, 28);
    if (!cfg.fontName.empty() && cfg.fontName.front() == L'@') cfg.fontName.erase(cfg.fontName.begin());
    // A hand-edited MergeSep could name a separator that does not exist; fall
    // back to the default rather than carry an out-of-range enum into the UI.
    if (cfg.mergeSep != merge::Separator::BlankLine &&
        cfg.mergeSep != merge::Separator::Newline &&
        cfg.mergeSep != merge::Separator::Space &&
        cfg.mergeSep != merge::Separator::Custom) {
        cfg.mergeSep = merge::Separator::BlankLine;
    }
}

void Load(Config& cfg) {
    cfg = Defaults();
    std::vector<uint8_t> raw;
    if (!util::ReadWholeFile(util::ConfigPath(), raw)) return;
    size_t start = 0;
    if (raw.size() >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) start = 3;
    std::string content(reinterpret_cast<const char*>(raw.data()) + start, raw.size() - start);

    auto getStr = [&](const char* key) -> std::string {
        std::string needle = std::string(key) + "=";
        size_t pos = content.find(needle);
        if (pos == std::string::npos) return {};
        pos += needle.size();
        size_t eol = content.find('\n', pos);
        std::string val = content.substr(pos, eol == std::string::npos
                                         ? std::string::npos : eol - pos);
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
        return val;
    };
    auto getInt = [&](const char* key, int def) -> int {
        std::string s = getStr(key);
        return s.empty() ? def : std::atoi(s.c_str());
    };

    cfg.maxHistory = getInt("MaxHistory", cfg.maxHistory);
    cfg.expiryDays = getInt("ExpiryDays", cfg.expiryDays);
    cfg.pasteDelayMs = getInt("PasteDelayMs", cfg.pasteDelayMs);
    cfg.pasteKey = static_cast<paste::Key>(getInt("PasteKey", static_cast<int>(cfg.pasteKey)));
    cfg.rowsVisible = getInt("RowsVisible", cfg.rowsVisible);
    cfg.popupPosition = getInt("PopupPosition", cfg.popupPosition);
    // Defaults to 1, not 0: a config.ini written before this option existed has
    // no HoverPreview line, and the behaviour it describes is the one those
    // users are getting for the first time. Absent means "on".
    cfg.hoverPreview = getInt("HoverPreview", 1) != 0;
    cfg.theme = static_cast<ThemeMode>(getInt("Theme", 0));
    cfg.maxTextBytes = static_cast<uint32_t>(
        getInt("MaxTextBytes", static_cast<int>(cfg.maxTextBytes)));
    cfg.maxImagePixels = static_cast<uint32_t>(
        getInt("MaxImagePixels", static_cast<int>(cfg.maxImagePixels)));
    cfg.largeItemThresholdMB = getInt("LargeItemThresholdMB", cfg.largeItemThresholdMB);
    cfg.lastPopupX = getInt("LastPopupX", -1);
    cfg.lastPopupY = getInt("LastPopupY", -1);
    cfg.language = Widen(getStr("Language"));
    std::string hk = getStr("PopupHotkey");
    if (!hk.empty())
        cfg.popupHotkey = static_cast<uint32_t>(std::strtoul(hk.c_str(), nullptr, 10));
    std::string qhk = getStr("QueueHotkey");
    if (!qhk.empty())
        cfg.queueHotkey = static_cast<uint32_t>(std::strtoul(qhk.c_str(), nullptr, 10));
    for (int i = 0; i < 10; ++i) {
        char key[32];
        std::snprintf(key, sizeof(key), "PinnedHotkey%d", i);
        std::string v = getStr(key);
        if (!v.empty())
            cfg.pinnedHotkeys[i] = static_cast<uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
    }
    cfg.fontName = Widen(getStr("FontName"));
    cfg.fontSize = getInt("FontSize", 0);
    cfg.cleanOnExit = getInt("CleanOnExit", 0) != 0;
    cfg.logLevel = Widen(getStr("LogLevel"));

    // Blocklist rules are stored one line per key behind an explicit count, so
    // blank lines and '#' comments in the user's list survive a round trip. The
    // count is what tells Load when to stop; an empty value is a real blank
    // line, not the end of the list.
    const int ruleCount = getInt("BlockRuleCount", 0);
    if (ruleCount > 0) {
        std::wstring rules;
        for (int i = 0; i < ruleCount; ++i) {
            char key[32];
            std::snprintf(key, sizeof(key), "BlockRule%d", i);
            if (i > 0) rules += L'\n';
            rules += Widen(getStr(key));
        }
        cfg.blockRules = rules;
    }

    // Preview desensitization toggles. An absent key keeps the Defaults() value
    // (phone / ID / password on, email / API-key off), so a config.ini written
    // before this feature existed behaves exactly as the defaults specify.
    cfg.mask.phone = getInt("MaskPhone", cfg.mask.phone ? 1 : 0) != 0;
    cfg.mask.idCard = getInt("MaskIdCard", cfg.mask.idCard ? 1 : 0) != 0;
    cfg.mask.password = getInt("MaskPassword", cfg.mask.password ? 1 : 0) != 0;
    cfg.mask.email = getInt("MaskEmail", cfg.mask.email ? 1 : 0) != 0;
    cfg.mask.apiKey = getInt("MaskApiKey", cfg.mask.apiKey ? 1 : 0) != 0;

    // Merge separator. An absent MergeSep keeps the Defaults() value (blank
    // line); MergeSepCustom is read verbatim (single-line, so no CR to strip).
    cfg.mergeSep = static_cast<merge::Separator>(
        getInt("MergeSep", static_cast<int>(cfg.mergeSep)));
    cfg.mergeSepCustom = Widen(getStr("MergeSepCustom"));

    // Slug separator for the Slugify transform. Absent or empty keeps the
    // Defaults() value ("-"); transform::Slugify also falls back to "-", so a
    // blank separator can never silently glue words together.
    const std::string slug = getStr("SlugSep");
    if (!slug.empty()) cfg.slugSep = Widen(slug);
}

bool Save(const Config& cfg) {
    std::string ini;
    ini += "MaxHistory=" + std::to_string(cfg.maxHistory) + "\n";
    ini += "ExpiryDays=" + std::to_string(cfg.expiryDays) + "\n";
    ini += "PasteDelayMs=" + std::to_string(cfg.pasteDelayMs) + "\n";
    ini += "PasteKey=" + std::to_string(static_cast<int>(cfg.pasteKey)) + "\n";
    ini += "RowsVisible=" + std::to_string(cfg.rowsVisible) + "\n";
    ini += "PopupPosition=" + std::to_string(cfg.popupPosition) + "\n";
    ini += "HoverPreview=" + std::string(cfg.hoverPreview ? "1" : "0") + "\n";
    ini += "Theme=" + std::to_string(static_cast<int>(cfg.theme)) + "\n";
    ini += "MaxTextBytes=" + std::to_string(cfg.maxTextBytes) + "\n";
    ini += "MaxImagePixels=" + std::to_string(cfg.maxImagePixels) + "\n";
    ini += "LargeItemThresholdMB=" + std::to_string(cfg.largeItemThresholdMB) + "\n";
    ini += "LastPopupX=" + std::to_string(cfg.lastPopupX) + "\n";
    ini += "LastPopupY=" + std::to_string(cfg.lastPopupY) + "\n";
    ini += "PopupHotkey=" + std::to_string(cfg.popupHotkey) + "\n";
    ini += "QueueHotkey=" + std::to_string(cfg.queueHotkey) + "\n";
    ini += "CleanOnExit=" + std::string(cfg.cleanOnExit ? "1" : "0") + "\n";
    for (int i = 0; i < 10; ++i) {
        if (cfg.pinnedHotkeys[i] != 0)
            ini += "PinnedHotkey" + std::to_string(i) + "="
                 + std::to_string(cfg.pinnedHotkeys[i]) + "\n";
    }
    if (!cfg.language.empty()) ini += "Language=" + Narrow(cfg.language) + "\n";
    if (cfg.fontSize > 0) ini += "FontSize=" + std::to_string(cfg.fontSize) + "\n";
    if (!cfg.fontName.empty()) ini += "FontName=" + Narrow(cfg.fontName) + "\n";
    if (!cfg.logLevel.empty()) ini += "LogLevel=" + Narrow(cfg.logLevel) + "\n";

    // One key per rule line, behind a count. Splitting on '\n' and dropping any
    // trailing '\r' normalizes the CRLF a multiline edit control hands back,
    // so the stored form is always LF and round-trips through Load unchanged.
    {
        const std::string s = Narrow(cfg.blockRules);
        std::vector<std::string> lines;
        size_t pos = 0;
        while (pos <= s.size()) {
            const size_t eol = s.find('\n', pos);
            std::string line = s.substr(pos, eol == std::string::npos ? std::string::npos
                                                                     : eol - pos);
            while (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            if (eol == std::string::npos) break;
            pos = eol + 1;
        }
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        if (!lines.empty()) {
            ini += "BlockRuleCount=" + std::to_string(lines.size()) + "\n";
            for (size_t i = 0; i < lines.size(); ++i) {
                ini += "BlockRule" + std::to_string(i) + "=" + lines[i] + "\n";
            }
        }
    }

    // Preview desensitization toggles, always written so a round trip is exact.
    ini += "MaskPhone=" + std::string(cfg.mask.phone ? "1" : "0") + "\n";
    ini += "MaskIdCard=" + std::string(cfg.mask.idCard ? "1" : "0") + "\n";
    ini += "MaskPassword=" + std::string(cfg.mask.password ? "1" : "0") + "\n";
    ini += "MaskEmail=" + std::string(cfg.mask.email ? "1" : "0") + "\n";
    ini += "MaskApiKey=" + std::string(cfg.mask.apiKey ? "1" : "0") + "\n";

    // Merge separator: the mode as an int, plus the custom string when set. The
    // custom field is a single-line edit, so it never holds a newline that could
    // break the key=value format — no escaping needed.
    ini += "MergeSep=" + std::to_string(static_cast<int>(cfg.mergeSep)) + "\n";
    if (!cfg.mergeSepCustom.empty())
        ini += "MergeSepCustom=" + Narrow(cfg.mergeSepCustom) + "\n";

    // Slugify separator, written only when it differs from the "-" default so a
    // stock config.ini stays uncluttered. Single-line, so no escaping needed.
    if (!cfg.slugSep.empty() && cfg.slugSep != L"-")
        ini += "SlugSep=" + Narrow(cfg.slugSep) + "\n";
    return util::WriteFileAtomic(util::ConfigPath(), ini.data(), ini.size());
}

bool ActivateExisting() {
    if (g_settingsDlg && IsWindow(g_settingsDlg)) {
        SetForegroundWindow(g_settingsDlg);
        return true;
    }
    return false;
}

bool ShowDialog(HWND owner, HINSTANCE inst, Config& cfg) {
    if (!inst) return false;

    g_cfg = &cfg;
    g_resultOk = false;
    g_dpi = owner ? util::DpiOf(owner) : util::DpiOf(nullptr);

    // Create fonts
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                               static_cast<UINT>(g_dpi));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_fontBold = CreateFontIndirectW(&ncm.lfMessageFont);

    // Build in-memory dialog template
    std::vector<WORD> tmpl = BuildEmptyDlgTemplate();

    INT_PTR ret = DialogBoxIndirectParamW(
        inst,
        reinterpret_cast<LPCDLGTEMPLATEW>(tmpl.data()),
        owner,
        SettingsDlgProc,
        0);

    // Clean up fonts if dialog proc didn't (e.g. if dialog creation failed)
    if (g_font) { DeleteObject(g_font); g_font = nullptr; }
    if (g_fontBold) { DeleteObject(g_fontBold); g_fontBold = nullptr; }

    return ret == IDOK && g_resultOk;
}

}  // namespace settings
