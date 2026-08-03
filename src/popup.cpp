// popup.cpp
#include "popup.h"

#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "hotkey.h"
#include "i18n.h"
#include "imagecodec.h"
#include "paste.h"

namespace popup {
namespace {

struct Row {
    uint64_t id = 0;
    bool pinned = false;
};

struct Thumb {
    HBITMAP bmp = nullptr;
    int w = 0;
    int h = 0;
};

struct State {
    HINSTANCE inst = nullptr;
    Host* host = nullptr;
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    WNDPROC editOrig = nullptr;
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;
    HBRUSH editBg = nullptr;
    int dpi = 96;

    // Metrics (pixels)
    int width = 0;
    int titleH = 0;
    int pad = 0;
    int editH = 0;
    int rowH = 0;
    int hintH = 0;
    int indexW = 0;
    int thumbW = 0;
    int hotkeyW = 0;
    int pinW = 0;  // Pin icon reserved width

    std::vector<Row> rows;
    int sel = -1;
    int top = 0;
    int modalDepth = 0;
    bool hideRestoreFocus = true;  // Whether Hide() should restore focus to previous window

    // Thumbnail cache
    std::vector<std::pair<uint64_t, Thumb>> thumbs;

    // Pinned item drag reorder
    bool reorderDrag = false;
    int reorderFrom = -1;
    int reorderInsert = -1;

    // Ctrl+hover preview
    HWND previewWnd = nullptr;
    HWND previewEdit = nullptr;   // RichEdit child for RTF preview
    HMODULE richeditDll = nullptr;
    int previewRow = -1;
    UINT_PTR previewTimer = 0;
};

State g;

// ---------------- Metrics ----------------

void CalcMetrics() {
    g.dpi = util::DpiOf(g.hwnd);
    g.width = util::Scale(520, g.dpi);
    g.titleH = util::Scale(26, g.dpi);
    g.pad = util::Scale(8, g.dpi);
    g.editH = util::Scale(24, g.dpi);
    g.rowH = util::Scale(30, g.dpi);
    g.hintH = util::Scale(18, g.dpi);
    g.indexW = util::Scale(20, g.dpi);
    g.thumbW = util::Scale(56, g.dpi);
    g.hotkeyW = util::Scale(90, g.dpi);
    g.pinW = util::Scale(14, g.dpi);
}

int ListTop() { return g.titleH + g.pad + g.editH + g.pad / 2; }
int ListBottom() {
    return ListTop() + g.rowH * g.host->RowsVisible();
}

RECT ListRect() {
    RECT r;
    r.left = g.pad;
    r.top = ListTop();
    r.right = g.width - g.pad;
    r.bottom = ListBottom();
    return r;
}

// ---------------- Thumbnails ----------------

const Thumb* GetThumb(uint64_t id) {
    for (auto& [tid, t] : g.thumbs) {
        if (tid == id) {
            return &t;
        }
    }
    const Item* item = g.host->GetStore().Find(id);
    if (!item || item->kind != ItemKind::Image || item->data.empty()) {
        return nullptr;
    }
    Thumb t{};
    int maxW = util::Scale(48, g.dpi);
    int maxH = util::Scale(24, g.dpi);
    t.bmp = imagecodec::LoadThumbnailFromMemory(item->data.data(), item->data.size(), maxW, maxH,
                                                t.w, t.h);
    if (!t.bmp) {
        return nullptr;
    }
    if (g.thumbs.size() >= 32) {
        DeleteObject(g.thumbs.front().second.bmp);
        g.thumbs.erase(g.thumbs.begin());
    }
    g.thumbs.push_back({id, t});
    return &g.thumbs.back().second;
}

// ---------------- Rebuild list ----------------

void Rebuild() {
    std::wstring filter;
    if (g.edit) {
        int len = GetWindowTextLengthW(g.edit);
        if (len > 0) {
            std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
            GetWindowTextW(g.edit, buf.data(), len + 1);
            buf.resize(static_cast<size_t>(len));
            filter = buf;
        }
    }
    // Convert to lowercase
    for (wchar_t& c : filter) {
        c = static_cast<wchar_t>(towlower(c));
    }

    uint64_t oldSelId = 0;
    if (g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size()) {
        oldSelId = g.rows[static_cast<size_t>(g.sel)].id;
    }

    g.rows.clear();
    for (const Item& item : g.host->GetStore().Items()) {
        if (!filter.empty()) {
            std::wstring lower = item.preview;
            for (wchar_t& c : lower) {
                c = static_cast<wchar_t>(towlower(c));
            }
            if (lower.find(filter) == std::wstring::npos) {
                continue;
            }
        }
        g.rows.push_back({item.id, item.pinned});
    }

    g.sel = -1;
    for (size_t i = 0; i < g.rows.size(); ++i) {
        if (g.rows[i].id == oldSelId) {
            g.sel = static_cast<int>(i);
            break;
        }
    }
    if (g.sel < 0 && !g.rows.empty()) {
        g.sel = 0;
    }
    g.top = 0;
    if (g.sel >= 0) {
        int vis = g.host->RowsVisible();
        if (g.sel >= vis) {
            g.top = g.sel - vis + 1;
        }
    }
}

void Redraw() {
    if (g.hwnd) {
        InvalidateRect(g.hwnd, nullptr, FALSE);
    }
}

void EnsureVisible() {
    int vis = g.host->RowsVisible();
    if (g.sel < g.top) {
        g.top = g.sel;
    }
    if (g.sel >= g.top + vis) {
        g.top = g.sel - vis + 1;
    }
}

// ---------------- Drawing ----------------

void DrawPinIcon(HDC dc, int x, int cy, int size, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, size / 6), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    int headR = size * 3 / 8;
    int headCx = x + size / 3;
    int headCy = cy - size / 5;
    Ellipse(dc, headCx - headR, headCy - headR, headCx + headR, headCy + headR);
    MoveToEx(dc, headCx + headR / 2, headCy + headR / 2, nullptr);
    LineTo(dc, x + size, cy + size * 2 / 5);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawRow(HDC dc, const RECT& rc, int index, const util::Theme& theme) {
    const Row& row = g.rows[static_cast<size_t>(index)];
    const Item* item = g.host->GetStore().Find(row.id);
    if (!item) {
        return;
    }
    const bool selected = index == g.sel;

    HBRUSH back = CreateSolidBrush(selected ? theme.sel : (row.pinned ? theme.bgAlt : theme.bg));
    FillRect(dc, &rc, back);
    DeleteObject(back);
    SetBkMode(dc, TRANSPARENT);

    // Pin area (uniform width for all rows)
    int pinSize = util::Scale(9, g.dpi);
    if (row.pinned) {
        COLORREF pinColor = selected ? theme.selFg : theme.accent;
        DrawPinIcon(dc, rc.left + 2, rc.top + g.rowH / 2, pinSize, pinColor);
    }

    // Unified index (pinned + unpinned continuous)
    std::wstring indexText;
    if (index < 9) {
        indexText = util::Format(L"%d", index + 1);
    }
    RECT indexRc = rc;
    indexRc.left += g.pinW;
    indexRc.right = indexRc.left + g.indexW;
    SelectObject(dc, g.fontSmall);
    SetTextColor(dc, selected ? theme.selFg : (row.pinned ? theme.accent : theme.dim));
    DrawTextW(dc, indexText.c_str(), -1, &indexRc,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    // Text area
    RECT textRc = rc;
    textRc.left = indexRc.right + g.pad / 2;
    textRc.right = rc.right - g.hotkeyW - g.pad / 2;

    if (item->kind == ItemKind::Image) {
        const Thumb* thumb = GetThumb(item->id);
        if (thumb) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, thumb->bmp);
            int y = rc.top + (g.rowH - thumb->h) / 2;
            BLENDFUNCTION blend = {};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(dc, textRc.left, y, thumb->w, thumb->h, mem, 0, 0, thumb->w, thumb->h,
                       blend);
            SelectObject(mem, old);
            DeleteDC(mem);
            textRc.left += thumb->w + g.pad / 2;
        }
    }

    // Rich text indicator badge for RTF/HTML items
    if (item->kind == ItemKind::Rtf || item->kind == ItemKind::Html) {
        SelectObject(dc, g.fontSmall);
        SetTextColor(dc, selected ? theme.selFg : theme.accent);
        RECT badgeRc = textRc;
        badgeRc.right = badgeRc.left + g.indexW;
        DrawTextW(dc, L"RTF", -1, &badgeRc,
                  DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        textRc.left = badgeRc.right + 2;
    }

    SelectObject(dc, g.font);
    SetTextColor(dc, selected ? theme.selFg : theme.fg);
    DrawTextW(dc, item->preview.c_str(), -1, &textRc,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void PaintAll(HDC target, const RECT& client) {
    const util::Theme& theme = g.host->GetTheme();

    // Double buffering
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(theme.bg);
    FillRect(mem, &client, bgBrush);
    DeleteObject(bgBrush);

    // Title bar
    RECT titleRc = {0, 0, client.right, g.titleH};
    HBRUSH titleBrush = CreateSolidBrush(theme.bgAlt);
    FillRect(mem, &titleRc, titleBrush);
    DeleteObject(titleBrush);
    SetBkMode(mem, TRANSPARENT);
    SelectObject(mem, g.fontSmall);
    SetTextColor(mem, theme.fg);
    RECT titleText = {g.pad, 0, client.right - g.titleH, g.titleH};
    DrawTextW(mem, i18n::T("popup.title"), -1, &titleText, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    // X button
    int xSize = util::Scale(12, g.dpi);
    int xPad = (g.titleH - xSize) / 2;
    HPEN xPen = CreatePen(PS_SOLID, util::Scale(1, g.dpi), theme.dim);
    HGDIOBJ oldPen = SelectObject(mem, xPen);
    int xRight = client.right - xPad;
    MoveToEx(mem, xRight - xSize, xPad, nullptr);
    LineTo(mem, xRight, xPad + xSize);
    MoveToEx(mem, xRight, xPad, nullptr);
    LineTo(mem, xRight - xSize, xPad + xSize);
    SelectObject(mem, oldPen);
    DeleteObject(xPen);

    // List area
    RECT list = ListRect();
    int vis = g.host->RowsVisible();
    int end = std::min(g.top + vis, static_cast<int>(g.rows.size()));
    for (int i = g.top; i < end; ++i) {
        RECT rowRc;
        rowRc.left = list.left;
        rowRc.right = list.right;
        rowRc.top = list.top + (i - g.top) * g.rowH;
        rowRc.bottom = rowRc.top + g.rowH;
        DrawRow(mem, rowRc, i, theme);
    }

    // Drag reorder insert line
    if (g.reorderDrag && g.reorderInsert >= 0) {
        int y = list.top + (g.reorderInsert - g.top) * g.rowH;
        HPEN linePen = CreatePen(PS_SOLID, 2, theme.accent);
        HGDIOBJ op = SelectObject(mem, linePen);
        MoveToEx(mem, list.left, y, nullptr);
        LineTo(mem, list.right, y);
        SelectObject(mem, op);
        DeleteObject(linePen);
    }

    // Empty state
    if (g.rows.empty()) {
        RECT emptyRc = list;
        SelectObject(mem, g.font);
        SetTextColor(mem, theme.dim);
        const wchar_t* msg = i18n::T("popup.empty");
        DrawTextW(mem, msg, -1, &emptyRc, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    }

    // Bottom hint
    RECT hintRc = {g.pad, ListBottom() + 2, client.right - g.pad, client.bottom};
    SelectObject(mem, g.fontSmall);
    SetTextColor(mem, theme.dim);
    DrawTextW(mem, i18n::T("popup.hint"), -1, &hintRc, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    // Border
    HPEN borderPen = CreatePen(PS_SOLID, 1, theme.line);
    HGDIOBJ op2 = SelectObject(mem, borderPen);
    HGDIOBJ ob2 = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, client.right, client.bottom);
    SelectObject(mem, op2);
    SelectObject(mem, ob2);
    DeleteObject(borderPen);

    BitBlt(target, 0, 0, client.right, client.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// ---------------- Preview tooltip window ----------------

// Stream callback for feeding RTF data into RichEdit
struct RtfStreamData {
    const uint8_t* data;
    size_t size;
    size_t pos;
};

static DWORD CALLBACK RtfStreamCallback(DWORD_PTR cookie, LPBYTE buffer, LONG cb, LONG* pcb) {
    auto* sd = reinterpret_cast<RtfStreamData*>(cookie);
    size_t remaining = sd->size - sd->pos;
    LONG toCopy = (static_cast<size_t>(cb) < remaining) ? cb : static_cast<LONG>(remaining);
    if (toCopy > 0) {
        memcpy(buffer, sd->data + sd->pos, static_cast<size_t>(toCopy));
        sd->pos += static_cast<size_t>(toCopy);
    }
    *pcb = toCopy;
    return 0;
}

void DestroyPreviewEdit() {
    if (g.previewEdit) {
        DestroyWindow(g.previewEdit);
        g.previewEdit = nullptr;
    }
}

void HidePreview() {
    DestroyPreviewEdit();
    if (g.previewWnd) {
        DestroyWindow(g.previewWnd);
        g.previewWnd = nullptr;
    }
    g.previewRow = -1;
    if (g.previewTimer) {
        KillTimer(g.hwnd, g.previewTimer);
        g.previewTimer = 0;
    }
}

void ShowPreview(int rowIndex) {
    if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= g.rows.size()) {
        return;
    }
    const Item* item = g.host->GetStore().Find(g.rows[static_cast<size_t>(rowIndex)].id);
    if (!item) {
        return;
    }
    const util::Theme& theme = g.host->GetTheme();
    int pw = util::Scale(400, g.dpi);
    int ph = util::Scale(260, g.dpi);

    // Position to the right of the row, clamped to work area
    RECT winRc;
    GetWindowRect(g.hwnd, &winRc);
    int px = winRc.right + 4;
    int py = winRc.top + ListTop() + (rowIndex - g.top) * g.rowH;

    // Get the monitor work area to clamp position
    HMONITOR mon = MonitorFromWindow(g.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;

    // If not enough space on the right, show on the left
    if (px + pw > wa.right) {
        px = winRc.left - pw - 4;
    }
    // Clamp to work area
    if (px < wa.left) {
        px = wa.left;
    }
    if (py + ph > wa.bottom) {
        py = wa.bottom - ph;
    }
    if (py < wa.top) {
        py = wa.top;
    }

    if (!g.previewWnd) {
        g.previewWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, L"ClipWizPreview", L"",
                                       WS_POPUP | WS_BORDER, px, py, pw, ph, g.hwnd, nullptr,
                                       g.inst, nullptr);
    } else {
        SetWindowPos(g.previewWnd, HWND_TOPMOST, px, py, pw, ph, SWP_NOACTIVATE);
    }
    ShowWindow(g.previewWnd, SW_SHOWNA);

    // RTF: render natively via RichEdit control (shows text + images + formatting)
    if (item->kind == ItemKind::Rtf) {
        if (!g.richeditDll) {
            g.richeditDll = LoadLibraryW(L"Msftedit.dll");
            if (!g.richeditDll) {
                g.richeditDll = LoadLibraryW(L"riched20.dll");
            }
        }
        DestroyPreviewEdit();
        if (g.richeditDll) {
            const wchar_t* cls = (GetModuleHandleW(L"Msftedit.dll") != nullptr)
                                     ? L"RichEdit50W"
                                     : RICHEDIT_CLASSW;
            g.previewEdit = CreateWindowExW(
                0, cls, L"",
                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_READONLY | WS_VSCROLL,
                2, 2, pw - 4, ph - 4, g.previewWnd, nullptr, g.inst, nullptr);
            if (g.previewEdit) {
                SendMessageW(g.previewEdit, EM_SETBKGNDCOLOR, 0,
                             static_cast<LPARAM>(theme.bg));
                RtfStreamData sd = {item->data.data(), item->data.size(), 0};
                EDITSTREAM es = {};
                es.dwCookie = reinterpret_cast<DWORD_PTR>(&sd);
                es.pfnCallback = RtfStreamCallback;
                SendMessageW(g.previewEdit, EM_STREAMIN, SF_RTF,
                             reinterpret_cast<LPARAM>(&es));
                SendMessageW(g.previewEdit, EM_SETSEL, 0, 0);
                SendMessageW(g.previewEdit, EM_SCROLLCARET, 0, 0);
            }
        }
        // If RichEdit failed, fall through to GDI text below
        if (g.previewEdit) {
            g.previewRow = rowIndex;
            return;
        }
    } else {
        DestroyPreviewEdit();
    }

    // GDI fallback: images and text types
    HDC dc = GetDC(g.previewWnd);
    RECT rc = {0, 0, pw, ph};
    HBRUSH bg = CreateSolidBrush(theme.bg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);

    if (item->kind == ItemKind::Image) {
        // Large image
        int outW = 0, outH = 0;
        HBITMAP big = imagecodec::LoadThumbnailFromMemory(item->data.data(), item->data.size(),
                                                          pw - 8, ph - 8, outW, outH);
        if (big) {
            HDC mem = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(mem, big);
            BLENDFUNCTION blend = {};
            blend.BlendOp = AC_SRC_OVER;
            blend.SourceConstantAlpha = 255;
            blend.AlphaFormat = AC_SRC_ALPHA;
            AlphaBlend(dc, (pw - outW) / 2, (ph - outH) / 2, outW, outH, mem, 0, 0, outW, outH,
                       blend);
            SelectObject(mem, old);
            DeleteDC(mem);
            DeleteObject(big);
        }
    } else {
        // Text types: show full content
        std::wstring text = Store::TextOf(*item);
        SelectObject(dc, g.font);
        SetTextColor(dc, theme.fg);
        RECT textRc = {6, 6, pw - 6, ph - 6};
        DrawTextW(dc, text.c_str(), -1, &textRc, DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    ReleaseDC(g.previewWnd, dc);
    g.previewRow = rowIndex;
}

// ---------------- Operations ----------------

void Activate(int index) {
    if (index < 0 || static_cast<size_t>(index) >= g.rows.size()) {
        return;
    }
    g.host->PasteItem(g.rows[static_cast<size_t>(index)].id);
}

bool HandleNavKey(UINT vk, bool ctrl, bool alt) {
    if (alt && vk >= '1' && vk <= '9') {
        Activate(static_cast<int>(vk - '1'));
        return true;
    }
    if (ctrl && (vk == VK_UP || vk == VK_DOWN)) {
        // Pinned item reorder
        if (g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size() &&
            g.rows[static_cast<size_t>(g.sel)].pinned) {
            int delta = (vk == VK_UP) ? -1 : 1;
            g.host->MovePinned(g.rows[static_cast<size_t>(g.sel)].id, delta);
            g.sel += delta;
            EnsureVisible();
            Redraw();
            return true;
        }
        return false;
    }
    if (ctrl && vk == 'P') {
        if (g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size()) {
            g.host->TogglePin(g.rows[static_cast<size_t>(g.sel)].id);
        }
        return true;
    }
    if (ctrl && (vk == 'D' || vk == VK_DELETE)) {
        if (g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size()) {
            g.host->DeleteItem(g.rows[static_cast<size_t>(g.sel)].id);
        }
        return true;
    }
    if (vk == VK_ESCAPE) {
        Hide();
        return true;
    }
    if (vk == VK_RETURN) {
        Activate(g.sel);
        return true;
    }
    int vis = g.host->RowsVisible();
    int count = static_cast<int>(g.rows.size());
    if (vk == VK_UP) {
        if (g.sel > 0) { --g.sel; EnsureVisible(); Redraw(); }
        return true;
    }
    if (vk == VK_DOWN) {
        if (g.sel < count - 1) { ++g.sel; EnsureVisible(); Redraw(); }
        return true;
    }
    if (vk == VK_PRIOR) {
        g.sel = std::max(0, g.sel - vis); EnsureVisible(); Redraw();
        return true;
    }
    if (vk == VK_NEXT) {
        g.sel = std::min(count - 1, g.sel + vis); EnsureVisible(); Redraw();
        return true;
    }
    if (ctrl && vk == VK_HOME) {
        g.sel = 0; EnsureVisible(); Redraw(); return true;
    }
    if (ctrl && vk == VK_END) {
        g.sel = count - 1; EnsureVisible(); Redraw(); return true;
    }
    return false;
}

void ShowRowMenu(int index) {
    if (index < 0 || static_cast<size_t>(index) >= g.rows.size()) {
        return;
    }
    const Row& row = g.rows[static_cast<size_t>(index)];
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, i18n::T("popup.menu.copy"));
    AppendMenuW(menu, MF_STRING, 2, i18n::T("popup.menu.paste"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3,
                row.pinned ? i18n::T("popup.menu.unpin") : i18n::T("popup.menu.pin"));
    AppendMenuW(menu, MF_STRING, 4, i18n::T("popup.menu.delete"));

    POINT pt;
    GetCursorPos(&pt);
    int cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0,
                             g.hwnd, nullptr);
    DestroyMenu(menu);

    switch (cmd) {
        case 1: g.host->CopyItem(row.id); break;
        case 2: g.host->PasteItem(row.id); break;
        case 3: g.host->TogglePin(row.id); break;
        case 4: g.host->DeleteItem(row.id); break;
        default: break;
    }
}

// ---------------- Positioning ----------------

void PositionWindow() {
    int vis = g.host->RowsVisible();
    int h = g.titleH + g.pad + g.editH + g.pad / 2 + g.rowH * vis + g.pad / 2 + g.hintH + g.pad;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);

    int x = 0, y = 0;
    int mode = g.host->PopupPosition();
    if (mode == 2) {
        int lx, ly;
        g.host->GetLastPos(lx, ly);
        if (lx >= 0 && ly >= 0) {
            x = lx;
            y = ly;
        } else {
            mode = 0;
        }
    }
    if (mode == 0) {
        POINT cur;
        GetCursorPos(&cur);
        x = cur.x - g.width / 2;
        y = cur.y + 16;
    } else if (mode == 1) {
        GUITHREADINFO gti{};
        gti.cbSize = sizeof(gti);
        if (GetGUIThreadInfo(0, &gti) && gti.hwndCaret) {
            POINT cp = {gti.rcCaret.left, gti.rcCaret.bottom};
            ClientToScreen(gti.hwndCaret, &cp);
            x = cp.x;
            y = cp.y + 4;
        } else {
            POINT cur;
            GetCursorPos(&cur);
            x = cur.x - g.width / 2;
            y = cur.y + 16;
        }
    }
    // Ensure window stays on screen
    int maxX = std::max(work.left, work.right - g.width);
    int maxY = std::max(work.top, work.bottom - h);
    x = std::clamp(x, static_cast<int>(work.left), maxX);
    y = std::clamp(y, static_cast<int>(work.top), maxY);

    SetWindowPos(g.hwnd, HWND_TOPMOST, x, y, g.width, h, SWP_NOACTIVATE);
}

// ---------------- Edit subclass ----------------

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            UINT vk = static_cast<UINT>(wparam);
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            if (vk == VK_UP || vk == VK_DOWN || vk == VK_PRIOR || vk == VK_NEXT ||
                vk == VK_RETURN || vk == VK_ESCAPE ||
                (alt && vk >= '1' && vk <= '9') ||
                (ctrl && (vk == 'P' || vk == 'D' || vk == VK_DELETE || vk == VK_HOME ||
                          vk == VK_END || vk == VK_UP || vk == VK_DOWN))) {
                if (HandleNavKey(vk, ctrl, alt)) {
                    return 0;
                }
            }
            break;
        }
        case WM_CHAR: {
            // Suppress beep: Edit control beeps on unhandled chars like ESC and Enter
            wchar_t ch = static_cast<wchar_t>(wparam);
            if (ch == L'\x1B' || ch == L'\r' || ch == L'\n') {
                return 0;
            }
            break;
        }
        case WM_MOUSEWHEEL:
            return SendMessageW(g.hwnd, msg, wparam, lparam);
        default:
            break;
    }
    return CallWindowProcW(g.editOrig, hwnd, msg, wparam, lparam);
}

// ---------------- WndProc ----------------

LRESULT CALLBACK PopupProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_CREATE: {
            CalcMetrics();
            g.font = util::CreateUiFont(g.dpi, 0);
            g.fontSmall = util::CreateUiFont(g.dpi, -1);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client;
            GetClientRect(hwnd, &client);
            PaintAll(dc, client);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE && g.modalDepth == 0) {
                g.hideRestoreFocus = false;  // Lost focus passively, don't fight for foreground
                Hide();
            }
            return 0;
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            const util::Theme& theme = g.host->GetTheme();
            SetTextColor(hdc, theme.fg);
            SetBkColor(hdc, theme.bgAlt);
            if (!g.editBg) {
                g.editBg = CreateSolidBrush(theme.bgAlt);
            }
            return reinterpret_cast<LRESULT>(g.editBg);
        }
        case WM_NCHITTEST: {
            // Let Windows handle title bar dragging natively
            POINT pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &pt);
            if (pt.y < g.titleH) {
                // X button area
                int xSize = util::Scale(12, g.dpi);
                int xPad = (g.titleH - xSize) / 2;
                if (pt.x >= g.width - g.titleH && pt.y >= xPad && pt.y <= xPad + xSize + xPad) {
                    return HTCLOSE;
                }
                return HTCAPTION;
            }
            return HTCLIENT;
        }
        case WM_NCLBUTTONDOWN: {
            if (wparam == HTCLOSE) {
                Hide();
                return 0;
            }
            break;  // HTCAPTION: let DefWindowProc handle native dragging
        }
        case WM_EXITSIZEMOVE: {
            // Remember position after drag ends
            RECT rc;
            GetWindowRect(hwnd, &rc);
            g.host->SaveLastPos(rc.left, rc.top);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            int y = GET_Y_LPARAM(lparam);
            int x = GET_X_LPARAM(lparam);
            // List area click
            RECT list = ListRect();
            if (y >= list.top && y < list.bottom && x >= list.left && x < list.right) {
                int idx = g.top + (y - list.top) / g.rowH;
                if (idx >= 0 && static_cast<size_t>(idx) < g.rows.size()) {
                    g.sel = idx;
                    Redraw();
                    // Start drag reorder in pinned area
                    if (g.rows[static_cast<size_t>(idx)].pinned) {
                        g.reorderDrag = true;
                        g.reorderFrom = idx;
                        g.reorderInsert = idx;
                        SetCapture(hwnd);
                    }
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (g.reorderDrag) {
                RECT list = ListRect();
                int y = GET_Y_LPARAM(lparam);
                int rel = y - list.top;
                int insertIdx = g.top + std::clamp(rel / g.rowH, 0, g.host->RowsVisible());
                int pinnedCount = 0;
                for (const auto& r : g.rows) {
                    if (r.pinned) ++pinnedCount;
                }
                insertIdx = std::clamp(insertIdx, 0, pinnedCount);
                g.reorderInsert = insertIdx;
                Redraw();
                return 0;
            }
            // Ctrl+hover preview
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                RECT list = ListRect();
                int x = GET_X_LPARAM(lparam);
                int y = GET_Y_LPARAM(lparam);
                if (x >= list.left && x < list.right && y >= list.top && y < list.bottom) {
                    int idx = g.top + (y - list.top) / g.rowH;
                    if (idx != g.previewRow && idx >= 0 &&
                        static_cast<size_t>(idx) < g.rows.size()) {
                        HidePreview();
                        g.previewTimer = SetTimer(hwnd, 2, 300, nullptr);
                        g.previewRow = -2;  // Mark as waiting
                        // Store target row
                        SetPropW(hwnd, L"PreviewIdx", reinterpret_cast<HANDLE>(static_cast<INT_PTR>(idx)));
                    }
                } else {
                    HidePreview();
                }
            } else {
                HidePreview();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g.reorderDrag) {
                g.reorderDrag = false;
                ReleaseCapture();
                if (g.reorderFrom >= 0 && g.reorderInsert >= 0 && g.reorderFrom != g.reorderInsert) {
                    uint64_t id = g.rows[static_cast<size_t>(g.reorderFrom)].id;
                    g.host->GetStore().MovePinnedTo(id, g.reorderInsert);
                    OnDataChanged();
                }
                g.reorderFrom = -1;
                g.reorderInsert = -1;
                Redraw();
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            int y = GET_Y_LPARAM(lparam);
            RECT list = ListRect();
            if (y >= list.top && y < list.bottom) {
                int idx = g.top + (y - list.top) / g.rowH;
                Activate(idx);
            }
            return 0;
        }
        case WM_RBUTTONUP: {
            int y = GET_Y_LPARAM(lparam);
            RECT list = ListRect();
            if (y >= list.top && y < list.bottom) {
                int idx = g.top + (y - list.top) / g.rowH;
                if (idx >= 0 && static_cast<size_t>(idx) < g.rows.size()) {
                    g.sel = idx;
                    Redraw();
                    ShowRowMenu(idx);
                }
            }
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            int vis = g.host->RowsVisible();
            int count = static_cast<int>(g.rows.size());
            if (delta > 0) {
                g.top = std::max(0, g.top - 3);
            } else {
                g.top = std::min(std::max(0, count - vis), g.top + 3);
            }
            Redraw();
            return 0;
        }
        case WM_TIMER:
            if (wparam == 2) {
                KillTimer(hwnd, 2);
                g.previewTimer = 0;
                HANDLE prop = GetPropW(hwnd, L"PreviewIdx");
                if (prop) {
                    int idx = static_cast<int>(reinterpret_cast<INT_PTR>(prop));
                    RemovePropW(hwnd, L"PreviewIdx");
                    ShowPreview(idx);
                }
            }
            return 0;
        case WM_DPICHANGED:
            CalcMetrics();
            if (g.font) DeleteObject(g.font);
            if (g.fontSmall) DeleteObject(g.fontSmall);
            if (g.editBg) { DeleteObject(g.editBg); g.editBg = nullptr; }
            g.font = util::CreateUiFont(g.dpi, 0);
            g.fontSmall = util::CreateUiFont(g.dpi, -1);
            for (auto& [id, t] : g.thumbs) {
                DeleteObject(t.bmp);
            }
            g.thumbs.clear();
            PositionWindow();
            Redraw();
            return 0;
        case WM_DESTROY:
            HidePreview();
            if (g.edit && g.editOrig) {
                SetWindowLongPtrW(g.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g.editOrig));
                g.editOrig = nullptr;
            }
            if (g.font) { DeleteObject(g.font); g.font = nullptr; }
            if (g.fontSmall) { DeleteObject(g.fontSmall); g.fontSmall = nullptr; }
            if (g.editBg) { DeleteObject(g.editBg); g.editBg = nullptr; }
            for (auto& [id, t] : g.thumbs) {
                DeleteObject(t.bmp);
            }
            g.thumbs.clear();
            return 0;
        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

// ------------------------------------------------------------------ Public interface

bool Init(HINSTANCE inst, Host* host) {
    g.inst = inst;
    g.host = host;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = PopupProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"ClipWizPopup";
    RegisterClassExW(&wc);

    // Preview window class
    WNDCLASSEXW wc2{};
    wc2.cbSize = sizeof(wc2);
    wc2.lpfnWndProc = DefWindowProcW;
    wc2.hInstance = inst;
    wc2.lpszClassName = L"ClipWizPreview";
    RegisterClassExW(&wc2);

    g.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"ClipWizPopup", L"",
                             WS_POPUP, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) {
        return false;
    }

    CalcMetrics();
    g.font = util::CreateUiFont(g.dpi, 0);
    g.fontSmall = util::CreateUiFont(g.dpi, -1);

    // Filter edit box
    g.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             g.pad, g.titleH + g.pad / 2, g.width - g.pad * 2, g.editH, g.hwnd,
                             nullptr, inst, nullptr);
    SendMessageW(g.edit, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    SendMessageW(g.edit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(i18n::T("popup.filter_hint")));
    g.editOrig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));

    return true;
}

void Shutdown() {
    if (g.hwnd) {
        DestroyWindow(g.hwnd);
        g.hwnd = nullptr;
    }
}

void Show() {
    Rebuild();
    PositionWindow();
    SetWindowPos(g.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    ShowWindow(g.hwnd, SW_SHOW);
    // AttachThreadInput to avoid system beep when SetForegroundWindow fails
    HWND fg = GetForegroundWindow();
    DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
    DWORD myThread = GetCurrentThreadId();
    bool attached = false;
    if (fgThread && fgThread != myThread) {
        attached = AttachThreadInput(myThread, fgThread, TRUE) != FALSE;
    }
    SetForegroundWindow(g.hwnd);
    SetFocus(g.edit);
    if (attached) {
        AttachThreadInput(myThread, fgThread, FALSE);
    }
    Redraw();
}

void Hide() {
    HidePreview();
    if (!IsWindowVisible(g.hwnd)) {
        return;
    }
    // Remember position
    RECT rc;
    GetWindowRect(g.hwnd, &rc);
    g.host->SaveLastPos(rc.left, rc.top);
    ShowWindow(g.hwnd, SW_HIDE);
    SetWindowTextW(g.edit, L"");
    // Return focus to the previous foreground window (only on active user action,
    // not when we lost focus passively to another window/menu)
    if (g.hideRestoreFocus) {
        HWND target = paste::Target();
        if (target) {
            SetForegroundWindow(target);
        }
    }
    g.hideRestoreFocus = true;  // Reset for next time
}

void Toggle() {
    if (IsWindowVisible(g.hwnd)) {
        Hide();
    } else {
        Show();
    }
}

bool IsVisible() {
    return g.hwnd && IsWindowVisible(g.hwnd);
}

HWND Window() {
    return g.hwnd;
}

void BeginModal() { ++g.modalDepth; }
void EndModal() { if (g.modalDepth > 0) --g.modalDepth; }

void OnThemeChanged() {
    if (g.editBg) {
        DeleteObject(g.editBg);
        g.editBg = nullptr;
    }
    // Refresh cue banner text (language may have changed)
    if (g.edit) {
        SendMessageW(g.edit, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(i18n::T("popup.filter_hint")));
    }
    Redraw();
}

void OnDataChanged() {
    if (IsWindowVisible(g.hwnd)) {
        Rebuild();
        Redraw();
    }
}

}  // namespace popup
