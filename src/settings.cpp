// settings.cpp — 原有设置分组改为 Tab，保持页内控件布局与行为
#include "settings.h"

#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <array>

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

enum PageIndex { General, Shortcuts, Privacy, PageCount };
struct PageControl {
    HWND hwnd;
    RECT bounds;  // 原有布局的 DIP 坐标，不随滚动或 DPI 变化累积误差。
};
struct Page {
    HWND hwnd = nullptr;
    std::vector<PageControl> controls;
    int contentHeight = 0;
    int scrollX = 0;
    int scrollY = 0;
    int wheelDelta = 0;
};
std::array<Page, PageCount> g_pages;
HWND g_tabs = nullptr;
int g_activePage = General;
bool g_layout = false;
bool g_pagesReady = false;
constexpr UINT kEnsureFocus = WM_APP + 31;
LRESULT CALLBACK ControlProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

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
    for (Page& page : g_pages) {
        if (hwnd && page.hwnd == parent) {
            page.controls.push_back({hwnd, {x, y, x + w, y + h}});
            if (style & WS_TABSTOP) SetWindowSubclass(hwnd, ControlProc, 1, 0);
            break;
        }
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

// 保存和事件处理仅改为查找控件所属页，不改变原有设置行为。
HWND SettingParent(int id) {
    for (const Page& page : g_pages)
        if (GetDlgItem(page.hwnd, id)) return page.hwnd;
    return nullptr;
}

void ScrollPage(Page& page, int x, int y) {
    RECT client{};
    GetClientRect(page.hwnd, &client);
    page.scrollX = std::clamp(x, 0, std::max(0, Dip(590) - static_cast<int>(client.right)));
    page.scrollY = std::clamp(y, 0, std::max(0, Dip(page.contentHeight) - static_cast<int>(client.bottom)));
    for (const PageControl& ctrl : page.controls) {
        const RECT& rc = ctrl.bounds;
        SetWindowPos(ctrl.hwnd, nullptr, Dip(rc.left) - page.scrollX, Dip(rc.top) - page.scrollY,
                     Dip(rc.right - rc.left), Dip(rc.bottom - rc.top), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    SetScrollPos(page.hwnd, SB_HORZ, page.scrollX, TRUE);
    SetScrollPos(page.hwnd, SB_VERT, page.scrollY, TRUE);
    InvalidateRect(page.hwnd, nullptr, TRUE);
}

void EnsureVisible(HWND ctrl) {
    Page& page = g_pages[g_activePage];
    if (!ctrl || GetParent(ctrl) != page.hwnd) return;
    RECT rc{}, client{};
    GetWindowRect(ctrl, &rc);
    MapWindowPoints(HWND_DESKTOP, page.hwnd, reinterpret_cast<POINT*>(&rc), 2);
    GetClientRect(page.hwnd, &client);
    int x = page.scrollX, y = page.scrollY;
    if (rc.left < 0) x += rc.left;
    else if (rc.right > client.right) x += rc.right - client.right;
    if (rc.top < 0) y += rc.top;
    else if (rc.bottom > client.bottom) y += rc.bottom - client.bottom;
    if (x != page.scrollX || y != page.scrollY) ScrollPage(page, x, y);
}

LRESULT CALLBACK ControlProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (msg == WM_SETFOCUS)
        PostMessageW(g_settingsDlg, kEnsureFocus, reinterpret_cast<WPARAM>(hwnd), 0);
    // 不截获键盘输入，保留热键控件原来的录制行为。
    return DefSubclassProc(hwnd, msg, wp, lp);
}

INT_PTR CALLBACK PageProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_COMMAND) return SendMessageW(GetParent(hwnd), msg, wp, lp);
    Page* page = nullptr;
    for (Page& p : g_pages) if (p.hwnd == hwnd) page = &p;
    if (!page) return FALSE;
    if (msg == WM_VSCROLL || msg == WM_HSCROLL) {
        const bool vertical = msg == WM_VSCROLL;
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd, vertical ? SB_VERT : SB_HORZ, &si);
        int pos = vertical ? page->scrollY : page->scrollX;
        switch (LOWORD(wp)) {
            case SB_TOP: pos = 0; break;
            case SB_BOTTOM: pos = si.nMax; break;
            case SB_LINEUP: pos -= Dip(28); break;
            case SB_LINEDOWN: pos += Dip(28); break;
            case SB_PAGEUP: pos -= static_cast<int>(si.nPage); break;
            case SB_PAGEDOWN: pos += static_cast<int>(si.nPage); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        }
        ScrollPage(*page, vertical ? page->scrollX : pos, vertical ? pos : page->scrollY);
        return TRUE;
    }
    if (msg == WM_MOUSEWHEEL) {
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        page->wheelDelta += GET_WHEEL_DELTA_WPARAM(wp);
        const int steps = page->wheelDelta / WHEEL_DELTA;
        page->wheelDelta %= WHEEL_DELTA;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int distance = lines == WHEEL_PAGESCROLL ? static_cast<int>(rc.bottom)
                                               : Dip(20) * static_cast<int>(lines);
        ScrollPage(*page, page->scrollX, page->scrollY - steps * distance);
        return TRUE;
    }
    return FALSE;
}

void LayoutDialog(HWND hwnd) {
    if (!g_pagesReady || g_layout) return;
    g_layout = true;
    RECT rc{};
    GetClientRect(hwnd, &rc);
    const int pad = Dip(12), footer = Dip(50);
    // 同级页签置底，避免页签背景遮住内容；页面内的布局完全沿用原版。
    SetWindowPos(g_tabs, HWND_BOTTOM, pad, pad, rc.right - 2 * pad, rc.bottom - pad - footer,
                 SWP_NOACTIVATE);
    RECT area{};
    GetClientRect(g_tabs, &area);
    TabCtrl_AdjustRect(g_tabs, FALSE, &area);
    MapWindowPoints(g_tabs, hwnd, reinterpret_cast<POINT*>(&area), 2);
    const int width = area.right - area.left, height = area.bottom - area.top;
    for (Page& page : g_pages) {
        SetWindowPos(page.hwnd, nullptr, area.left, area.top, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        bool horizontal = false, vertical = false;
        int viewW = width, viewH = height;
        for (int pass = 0; pass < 3; ++pass) {
            horizontal = Dip(590) > viewW;
            vertical = Dip(page.contentHeight) > viewH;
            viewW = width - (vertical ? GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi) : 0);
            viewH = height - (horizontal ? GetSystemMetricsForDpi(SM_CYHSCROLL, g_dpi) : 0);
        }
        SCROLLINFO si{sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
        si.nMax = Dip(590) - 1;
        si.nPage = static_cast<UINT>(std::max(1, viewW));
        si.nPos = page.scrollX;
        SetScrollInfo(page.hwnd, SB_HORZ, &si, TRUE);
        si.nMax = Dip(page.contentHeight) - 1;
        si.nPage = static_cast<UINT>(std::max(1, viewH));
        si.nPos = page.scrollY;
        SetScrollInfo(page.hwnd, SB_VERT, &si, TRUE);
        ScrollPage(page, page.scrollX, page.scrollY);
    }
    // 保留原有居中按钮、宽高和按钮间距，仅固定在页签容器下方。
    for (int i = 0; i < 2; ++i)
        SetWindowPos(GetDlgItem(hwnd, i == 0 ? IDOK : IDCANCEL), nullptr,
                     (rc.right - Dip(182)) / 2 + i * Dip(97), rc.bottom - pad - Dip(26), Dip(85), Dip(26),
                     SWP_NOZORDER | SWP_NOACTIVATE);
    g_layout = false;
    InvalidateRect(hwnd, nullptr, TRUE);
}

void SelectPage(int index) {
    if (index < 0 || index >= PageCount) return;
    HWND focus = GetFocus();
    bool moveFocus = IsChild(g_pages[g_activePage].hwnd, focus);
    if (moveFocus) SetFocus(g_tabs);
    g_activePage = index;
    TabCtrl_SetCurSel(g_tabs, index);
    for (int i = 0; i < PageCount; ++i) ShowWindow(g_pages[i].hwnd, i == index ? SW_SHOW : SW_HIDE);
    if (moveFocus) {
        HWND next = GetNextDlgTabItem(g_pages[index].hwnd, nullptr, FALSE);
        if (next) SetFocus(next);
    }
}

void RecreateFonts(HWND hwnd) {
    HFONT oldFont = g_font, oldBold = g_fontBold;
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, static_cast<UINT>(g_dpi));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfWeight = FW_BOLD;
    g_fontBold = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    if (g_pagesReady) {
        SendMessageW(g_tabs, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        SendDlgItemMessageW(hwnd, IDOK, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        SendDlgItemMessageW(hwnd, IDCANCEL, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
        g_winKeyIcon = nullptr;
        for (Page& page : g_pages) {
            for (const PageControl& ctrl : page.controls) {
                bool bold = reinterpret_cast<HFONT>(SendMessageW(ctrl.hwnd, WM_GETFONT, 0, 0)) == oldBold;
                SendMessageW(ctrl.hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(bold ? g_fontBold : g_font), FALSE);
                int id = GetDlgCtrlID(ctrl.hwnd);
                if (id == IDC_POPUP_WIN || id == IDC_QUEUE_WIN || (id >= IDC_PIN_WIN_BASE && id < IDC_PIN_WIN_BASE + 10))
                    SendMessageW(ctrl.hwnd, BM_SETIMAGE, IMAGE_ICON, reinterpret_cast<LPARAM>(LoadWinKeyIcon()));
            }
        }
        LayoutDialog(hwnd);
    }
    if (oldFont) DeleteObject(oldFont);
    if (oldBold) DeleteObject(oldBold);
}

void FitDialog(HWND hwnd, const RECT* suggested = nullptr) {
    RECT current{};
    GetWindowRect(hwnd, &current);
    if (suggested) current = *suggested;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&current, MONITOR_DEFAULTTONEAREST), &mi);
    RECT frame{0, 0, Dip(622), Dip(420)};
    AdjustWindowRectExForDpi(&frame, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                             GetWindowLongW(hwnd, GWL_EXSTYLE), static_cast<UINT>(g_dpi));
    int margin = Dip(8);
    int w = std::min(static_cast<int>(frame.right - frame.left), static_cast<int>(mi.rcWork.right - mi.rcWork.left) - margin * 2);
    int h = std::min(static_cast<int>(frame.bottom - frame.top), static_cast<int>(mi.rcWork.bottom - mi.rcWork.top) - margin * 2);
    int x = suggested ? current.left : mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    int y = suggested ? current.top : mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    x = std::clamp(x, static_cast<int>(mi.rcWork.left) + margin, static_cast<int>(mi.rcWork.right) - margin - w);
    y = std::clamp(y, static_cast<int>(mi.rcWork.top) + margin, static_cast<int>(mi.rcWork.bottom) - margin - h);
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

void PopulateControls(HWND hwnd) {
    HWND dialog = hwnd;
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_HOTKEY_CLASS};
    InitCommonControlsEx(&icc);
    g_tabs = MakeCtrl(dialog, WC_TABCONTROLW, L"", WS_TABSTOP | WS_CLIPSIBLINGS,
                      12, 12, 598, 358, IDC_SETTINGS_TABS);
    const char* titles[] = {"settings.tab.general", "settings.tab.shortcuts", "settings.tab.privacy"};
    struct EmptyPage { DLGTEMPLATE dlg; WORD menu, cls, title; };
    for (int i = 0; i < PageCount; ++i) {
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<wchar_t*>(i18n::T(titles[i]));
        TabCtrl_InsertItem(g_tabs, i, &item);
        EmptyPage tmpl{};
        tmpl.dlg.style = WS_CHILD | DS_CONTROL | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        tmpl.dlg.dwExtendedStyle = WS_EX_CONTROLPARENT;
        g_pages[i].hwnd = CreateDialogIndirectParamW(GetModuleHandleW(nullptr), &tmpl.dlg, dialog, PageProc, 0);
        if (!g_tabs || !g_pages[i].hwnd) {
            EndDialog(dialog, IDCANCEL);
            return;
        }
        SetDialogDpiChangeBehavior(g_pages[i].hwnd, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
    }
    hwnd = g_pages[General].hwnd;
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

    g_pages[General].contentHeight = y;
    hwnd = g_pages[Shortcuts].hwnd;
    y = kPad;

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

    g_pages[Shortcuts].contentHeight = y;
    hwnd = g_pages[Privacy].hwnd;
    y = kPad;

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

    g_pages[Privacy].contentHeight = y;
    MakeCtrl(dialog, L"BUTTON", i18n::T("settings.ok"),
             BS_DEFPUSHBUTTON | WS_TABSTOP, 0, 0, 85, 26, IDOK);
    MakeCtrl(dialog, L"BUTTON", i18n::T("settings.cancel"),
             BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 85, 26, IDCANCEL);
    g_pagesReady = true;
    FitDialog(dialog);
    LayoutDialog(dialog);
    SelectPage(General);
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
    switch (msg) {
        case WM_INITDIALOG: {
            g_settingsDlg = hwnd;
            SetDialogDpiChangeBehavior(hwnd, DDC_DISABLE_ALL, DDC_DISABLE_ALL);
            // 托盘宿主是隐藏窗口；设置应在用户当前操作的显示器打开。
            POINT cursor{};
            GetCursorPos(&cursor);
            SetWindowPos(hwnd, nullptr, cursor.x, cursor.y, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            g_dpi = util::DpiOf(hwnd);
            RecreateFonts(hwnd);
            PopulateControls(hwnd);
            SetFocus(g_tabs);
            return FALSE;
        }
        case WM_SIZE:
            LayoutDialog(hwnd);
            return TRUE;
        case WM_DPICHANGED:
            if (g_tabs) {
                int oldDpi = g_dpi;
                g_dpi = HIWORD(wparam);
                for (Page& page : g_pages) {
                    page.scrollX = MulDiv(page.scrollX, g_dpi, oldDpi);
                    page.scrollY = MulDiv(page.scrollY, g_dpi, oldDpi);
                }
                RecreateFonts(hwnd);
                FitDialog(hwnd, reinterpret_cast<const RECT*>(lparam));
                EnsureVisible(GetFocus());
            }
            return TRUE;
        case WM_DISPLAYCHANGE:
        case WM_SETTINGCHANGE:
            if (g_tabs) FitDialog(hwnd);
            return TRUE;
        case WM_NOTIFY: {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lparam);
            if (hdr->hwndFrom == g_tabs && hdr->code == TCN_SELCHANGE) {
                SelectPage(TabCtrl_GetCurSel(g_tabs));
                return TRUE;
            }
            break;
        }
        case kEnsureFocus:
            if (GetFocus() == reinterpret_cast<HWND>(wparam)) EnsureVisible(GetFocus());
            return TRUE;
        case WM_MOUSEWHEEL:
            SendMessageW(g_pages[g_activePage].hwnd, msg, wparam, lparam);
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
                    SendDlgItemMessageW(SettingParent(IDC_POPUP_HK), IDC_POPUP_HK, HKM_GETHOTKEY, 0, 0));
                bool win = IsDlgButtonChecked(SettingParent(IDC_POPUP_WIN), IDC_POPUP_WIN) == BST_CHECKED;
                uint32_t popupHk = hotkey::FromControl(raw, win);
                WORD qraw = static_cast<WORD>(
                    SendDlgItemMessageW(SettingParent(IDC_QUEUE_HK), IDC_QUEUE_HK, HKM_GETHOTKEY, 0, 0));
                bool qwin = IsDlgButtonChecked(SettingParent(IDC_QUEUE_WIN), IDC_QUEUE_WIN) == BST_CHECKED;
                uint32_t queueHk = hotkey::FromControl(qraw, qwin);
                uint32_t pinnedHk[10] = {};
                for (int i = 0; i < 10; ++i) {
                    WORD r = static_cast<WORD>(
                        SendDlgItemMessageW(SettingParent(IDC_PIN_HK_BASE + i), IDC_PIN_HK_BASE + i, HKM_GETHOTKEY, 0, 0));
                    bool w = IsDlgButtonChecked(SettingParent(IDC_PIN_WIN_BASE + i), IDC_PIN_WIN_BASE + i) == BST_CHECKED;
                    pinnedHk[i] = hotkey::FromControl(r, w);
                }
                if (!HotkeysAcceptable(hwnd, popupHk, queueHk, pinnedHk)) {
                    SelectPage(Shortcuts);
                    return TRUE;
                }
                cfg.popupHotkey = popupHk;
                cfg.queueHotkey = queueHk;
                for (int i = 0; i < 10; ++i) {
                    cfg.pinnedHotkeys[i] = pinnedHk[i];
                }
                cfg.maxHistory = GetDlgItemInt(SettingParent(IDC_MAXHISTORY), IDC_MAXHISTORY, nullptr, FALSE);
                cfg.expiryDays = GetDlgItemInt(SettingParent(IDC_EXPIRYDAYS), IDC_EXPIRYDAYS, nullptr, FALSE);
                cfg.cleanOnExit = IsDlgButtonChecked(SettingParent(IDC_CLEAN_ON_EXIT), IDC_CLEAN_ON_EXIT) == BST_CHECKED;
                cfg.hoverPreview = IsDlgButtonChecked(SettingParent(IDC_HOVER_PREVIEW), IDC_HOVER_PREVIEW) == BST_CHECKED;
                int langSel = static_cast<int>(
                    SendMessageW(GetDlgItem(SettingParent(IDC_LANGUAGE), IDC_LANGUAGE), CB_GETCURSEL, 0, 0));
                if (langSel == 1) cfg.language = L"en";
                else if (langSel == 2) cfg.language = L"zh-CN";
                else cfg.language = L"";
                cfg.theme = static_cast<ThemeMode>(
                    SendMessageW(GetDlgItem(SettingParent(IDC_THEME), IDC_THEME), CB_GETCURSEL, 0, 0));
                cfg.popupPosition = static_cast<int>(
                    SendMessageW(GetDlgItem(SettingParent(IDC_POPUPPOS), IDC_POPUPPOS), CB_GETCURSEL, 0, 0));
                bool autostart = IsDlgButtonChecked(SettingParent(IDC_AUTOSTART), IDC_AUTOSTART) == BST_CHECKED;
                // CB_GETCURSEL returns -1 when nothing is selected; Clamp()
                // turns that back into the default rather than storing it.
                cfg.pasteKey = static_cast<paste::Key>(
                    SendMessageW(GetDlgItem(SettingParent(IDC_PASTE_KEY), IDC_PASTE_KEY), CB_GETCURSEL, 0, 0));
                // Blocklist rules: strip the CR the edit control adds, store LF.
                {
                    HWND edBlock = GetDlgItem(SettingParent(IDC_BLOCKLIST), IDC_BLOCKLIST);
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
                cfg.mask.phone = IsDlgButtonChecked(SettingParent(IDC_MASK_PHONE), IDC_MASK_PHONE) == BST_CHECKED;
                cfg.mask.idCard = IsDlgButtonChecked(SettingParent(IDC_MASK_IDCARD), IDC_MASK_IDCARD) == BST_CHECKED;
                cfg.mask.password = IsDlgButtonChecked(SettingParent(IDC_MASK_PASSWORD), IDC_MASK_PASSWORD) == BST_CHECKED;
                cfg.mask.email = IsDlgButtonChecked(SettingParent(IDC_MASK_EMAIL), IDC_MASK_EMAIL) == BST_CHECKED;
                cfg.mask.apiKey = IsDlgButtonChecked(SettingParent(IDC_MASK_APIKEY), IDC_MASK_APIKEY) == BST_CHECKED;
                // Merge separator: mode from the combo, literal from the custom
                // field. CB_GETCURSEL is -1 if nothing is selected; Clamp()
                // restores the default. The custom edit is single-line, so there
                // is no CR to strip and nothing that could break the ini format.
                cfg.mergeSep = static_cast<merge::Separator>(
                    SendDlgItemMessageW(SettingParent(IDC_MERGE_SEP), IDC_MERGE_SEP, CB_GETCURSEL, 0, 0));
                wchar_t mergeCustom[64] = {};
                GetDlgItemTextW(SettingParent(IDC_MERGE_SEP_CUSTOM), IDC_MERGE_SEP_CUSTOM, mergeCustom, 64);
                cfg.mergeSepCustom = mergeCustom;
                // Data storage mode migration (must succeed before committing)
                int dataSel = static_cast<int>(
                    SendDlgItemMessageW(SettingParent(IDC_DATADIR), IDC_DATADIR, CB_GETCURSEL, 0, 0));
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
                SetDlgItemTextW(SettingParent(IDC_FONT_BTN), IDC_FONT_BTN, t.c_str());
                return TRUE;
            }
            if (id == IDC_FONT_RESET) {
                g_cfg->fontName.clear();
                g_cfg->fontSize = 0;
                SetDlgItemTextW(SettingParent(IDC_FONT_BTN), IDC_FONT_BTN, i18n::T("settings.font_default_val"));
                return TRUE;
            }
            if (id == IDC_DATADIR_OPEN) {
                ShellExecuteW(hwnd, L"open", util::DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return TRUE;
            }
            if (id == IDC_CLEAN_ON_EXIT) {
                bool checked = IsDlgButtonChecked(SettingParent(IDC_CLEAN_ON_EXIT), IDC_CLEAN_ON_EXIT) == BST_CHECKED;
                EnableWindow(GetDlgItem(SettingParent(IDC_EXPIRYDAYS), IDC_EXPIRYDAYS), !checked);
                return TRUE;
            }
            if (id == IDC_MERGE_SEP) {
                // The custom field is only meaningful in Custom mode; grey it out
                // for the three presets so it is clear their separator is fixed.
                const int sel = static_cast<int>(
                    SendDlgItemMessageW(SettingParent(IDC_MERGE_SEP), IDC_MERGE_SEP, CB_GETCURSEL, 0, 0));
                EnableWindow(GetDlgItem(SettingParent(IDC_MERGE_SEP_CUSTOM), IDC_MERGE_SEP_CUSTOM),
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
            g_pagesReady = false;
            g_tabs = nullptr;
            g_pages = {};
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
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN | DS_MODALFRAME | DS_SETFONT;
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
    g_pages = {};
    g_tabs = nullptr;
    g_activePage = General;
    g_layout = false;
    g_pagesReady = false;
    g_winKeyIcon = nullptr;

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
