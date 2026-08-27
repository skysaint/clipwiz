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
#include "resource.h"

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
    HWND sb = nullptr;  // Vertical scrollbar (child SBS_VERT)
    WNDPROC editOrig = nullptr;
    HFONT font = nullptr;
    HFONT fontSmall = nullptr;
    HBRUSH editBg = nullptr;
    int dpi = 96;

    // Metrics (pixels)
    int width = 0;
    int scrollW = 0;  // Vertical scrollbar width
    int titleH = 0;
    int pad = 0;
    int editH = 0;
    int rowH = 0;
    int hintH = 0;
    int indexW = 0;
    int thumbW = 0;
    int hotkeyW = 0;
    int pinW = 0;  // Pin icon reserved width
    int typeW = 0;    // Per-row type icon slot (square)
    int iconGap = 0;  // Uniform gap between pin / type / index / text
    int leadW = 0;    // Total leading strip width (gap+pin+gap+type+gap+index+gap)
    int fontMainH = 0;
    int fontSmallH = 0;
    int sortBtnsW = 0;  // Width for 4 pinned-item sort icons at row end

    // Interaction state
    bool closeHover = false;  // Mouse hovering X button
    bool mouseTracking = false;
    POINT lastPt = {};       // Last mouse pos in client coords
    int hoverRow = -1;       // Which row is mouse over (for sort button hover feedback)
    int hoverSortBtn = -1;   // 0..3: which sort icon is hovered, -1 none

    std::vector<Row> rows;
    int sel = -1;
    int top = 0;
    int modalDepth = 0;
    bool hideRestoreFocus = true;  // Whether Hide() should restore focus to previous window

    // Thumbnail cache
    std::vector<std::pair<uint64_t, Thumb>> thumbs;

    // Per-row type icons: [kind*2 + dark] -> HICON, loaded at current typeW.
    // Non-shared handles; destroyed and reloaded on DPI change.
    HICON typeIcons[10] = {};
    int typeIconSize = 0;  // size the cached icons were loaded at

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

// ---------------- Pinned-order helpers (shared by click + paint + hit-test) ----------------
//
// Route directly to Store's order-aware helpers so button disabled-state,
// click behavior, and MovePinnedTo() share a single coordinate system:
// the pinned index sorted by Item.order (which Store::CompactOrders keeps
// contiguous as 1..N).  No more drift between g.rows local indices and
// what MovePinnedTo() actually expects.

static inline int PinnedIndexOf(uint64_t rowId) {
    return g.host ? g.host->GetStore().PinnedIndexOf(rowId) : -1;
}

static inline int TotalPinnedCount() {
    return g.host ? g.host->GetStore().PinnedCount() : 0;
}

static inline bool IsSortBtnEnabled(int pinnedIdx, int pinnedCount, int btn) {
    if (pinnedIdx < 0 || pinnedCount <= 1) return false;
    switch (btn) {
        case 0: return pinnedIdx > 0;                 // to top
        case 1: return pinnedIdx > 0;                 // up one
        case 2: return pinnedIdx < pinnedCount - 1;   // down one
        case 3: return pinnedIdx < pinnedCount - 1;   // to bottom
    }
    return false;
}

// ---------------- Metrics ----------------

HFONT CreatePopupFont(int pointDelta) {
    HFONT font = util::CreateUiFont(g.dpi, g.host->PopupFontName(), g.host->PopupFontSize(),
                                    pointDelta);
    if (!font) {
        font = util::CreateUiFont(g.dpi, pointDelta);
    }
    return font;
}

int MeasureFontHeight(HFONT font) {
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    return tm.tmHeight;
}

int MeasureTextWidth(HFONT font, const std::wstring& text) {
    if (text.empty()) {
        return 0;
    }
    HDC dc = GetDC(g.hwnd);
    HGDIOBJ old = SelectObject(dc, font);
    SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(dc, old);
    ReleaseDC(g.hwnd, dc);
    return size.cx;
}

void RecreateFonts() {
    if (g.font) { DeleteObject(g.font); g.font = nullptr; }
    if (g.fontSmall) { DeleteObject(g.fontSmall); g.fontSmall = nullptr; }
    g.font = CreatePopupFont(0);
    g.fontSmall = CreatePopupFont(-1);
    if (!g.fontSmall) {
        g.fontSmall = CreatePopupFont(0);
    }
    g.fontMainH = g.font ? MeasureFontHeight(g.font) : util::Scale(16, g.dpi);
    g.fontSmallH = g.fontSmall ? MeasureFontHeight(g.fontSmall) : g.fontMainH;
}

void CalcMetrics() {
    g.dpi = util::DpiOf(g.hwnd);
    int titleTextW = MeasureTextWidth(g.font, i18n::T("popup.title"));
    int hintTextW = MeasureTextWidth(g.fontSmall, i18n::T("popup.hint"));
    int filterHintW = MeasureTextWidth(g.font, i18n::T("popup.filter_hint"));

    g.scrollW = GetSystemMetrics(SM_CXVSCROLL);
    if (g.scrollW <= 0) g.scrollW = util::Scale(16, g.dpi);
    g.scrollW = std::max(g.scrollW, util::Scale(14, g.dpi));

    g.pad = std::max(util::Scale(8, g.dpi), g.fontMainH / 2);
    g.titleH = std::max(util::Scale(28, g.dpi), g.fontMainH + g.pad);
    g.editH = std::max(util::Scale(26, g.dpi), g.fontMainH + g.pad + util::Scale(2, g.dpi));
    g.rowH = std::max(util::Scale(30, g.dpi), g.fontMainH + g.pad + util::Scale(6, g.dpi));
    g.hintH = std::max(util::Scale(18, g.dpi), g.fontSmallH + util::Scale(6, g.dpi));
    g.indexW = std::max(util::Scale(20, g.dpi),
                        MeasureTextWidth(g.fontSmall, L"99") + util::Scale(4, g.dpi));
    g.thumbW = std::max(util::Scale(56, g.dpi), g.rowH + util::Scale(16, g.dpi));
    g.hotkeyW = std::max(util::Scale(90, g.dpi),
                         MeasureTextWidth(g.fontSmall, L"Ctrl+Shift+Alt+PgDn") +
                             util::Scale(12, g.dpi));
    g.pinW = std::max(util::Scale(14, g.dpi), util::Scale(12, g.dpi));
    // Uniform spacing unit between the leading elements (pin / type / index)
    // and before the text. Keeps pin, type icon and index equally spaced and
    // everything left-aligned across every row.
    g.iconGap = std::max(util::Scale(5, g.dpi), g.pad / 2);
    // Square slot for the per-row type icon (TXT/RTF/HTML/IMG/FILE), sized to
    // the small font height so it lines up with the index digits.
    g.typeW = std::max(util::Scale(14, g.dpi), g.fontSmallH);
    // 4 sort icons (MoveToTop / MoveUp / MoveDown / MoveToBottom) on pinned rows only.
    // Keep icons compact so they don't eat too much row text space.
    int btnSize = util::Scale(12, g.dpi);
    int btnCap = g.rowH - g.pad;
    if (btnSize > btnCap) btnSize = btnCap;
    int btnGap = util::Scale(2, g.dpi);
    int btnMargin = util::Scale(3, g.dpi);
    g.sortBtnsW = btnSize * 4 + btnGap * 3 + btnMargin * 2;

    int minTextW = std::max({util::Scale(240, g.dpi), titleTextW + util::Scale(36, g.dpi),
                             hintTextW + util::Scale(20, g.dpi), filterHintW + util::Scale(20, g.dpi)});
    // NOTE: sortBtnsW is NOT added to the overall popup width here. The 4
    // reorder buttons live inside a pinned row's horizontal space, between
    // the truncated row text and the right edge (scrollbar side). i.e. they
    // *eat into* the text area of pinned rows only (exactly as UX asked:
    // pinned rows get a shorter text display + ellipsis, unpinned rows
    // keep full width).
    // NOTE 2: hotkeyW is also NOT added to the layout sum because per-row
    // hotkey labels are NOT rendered on the right edge of individual rows
    // (per-row hotkeys are shown only in the bottom global hint bar).
    // Adding either on top would force the whole popup wider and leave
    // ugly empty trailing space on the right for unpinned rows.
    // Leading strip before the text: gap, pin, gap, type, gap, index, gap.
    // Same formula used by DrawRow so width and drawing never drift.
    g.leadW = g.iconGap + g.pinW + g.iconGap + g.typeW + g.iconGap + g.indexW + g.iconGap;
    g.width = std::max(util::Scale(520, g.dpi),
                       g.pad + g.leadW + g.thumbW + minTextW) + g.scrollW;
}

int ListTop() { return g.titleH + g.pad + g.editH + g.pad / 2; }
int ListBottom() {
    return ListTop() + g.rowH * g.host->RowsVisible();
}

int PopupHeight() {
    int vis = g.host->RowsVisible();
    return g.titleH + g.pad + g.editH + g.pad / 2 + g.rowH * vis + g.pad / 2 + g.hintH + g.pad;
}

RECT ListRect() {
    RECT r;
    r.left = g.pad;
    r.top = ListTop();
    r.right = g.width - g.pad - g.scrollW;
    r.bottom = ListBottom();
    return r;
}

void ApplyPopupFontToControls() {
    if (g.edit && g.font) {
        SendMessageW(g.edit, WM_SETFONT, reinterpret_cast<WPARAM>(g.font), TRUE);
    }
}

void RelayoutWindow() {
    if (g.edit) {
        SetWindowPos(g.edit, nullptr, g.pad, g.titleH + g.pad / 2, g.width - g.pad * 2, g.editH,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g.sb) {
        RECT list = ListRect();
        SetWindowPos(g.sb, nullptr, list.right, list.top, g.scrollW, list.bottom - list.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (g.hwnd) {
        UINT flags = SWP_NOZORDER | SWP_NOACTIVATE;
        if (!IsWindowVisible(g.hwnd)) {
            flags |= SWP_NOMOVE;
        }
        SetWindowPos(g.hwnd, nullptr, 0, 0, g.width, PopupHeight(), flags);
    }
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

void UpdateScrollbar();  // forward declaration

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
    UpdateScrollbar();
}

void Redraw(bool fullWindow) {
    if (!g.hwnd) return;
    if (fullWindow) {
        InvalidateRect(g.hwnd, nullptr, FALSE);
    } else {
        RECT list = ListRect();
        list.right += g.scrollW;
        InvalidateRect(g.hwnd, &list, FALSE);
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

void UpdateScrollbar() {
    if (!g.sb) return;
    int count = static_cast<int>(g.rows.size());
    int vis = g.host->RowsVisible();
    SCROLLINFO si = {sizeof(si)};
    si.fMask = SIF_ALL | SIF_DISABLENOSCROLL;
    si.nMin = 0;
    si.nMax = std::max(0, count - 1);
    si.nPage = static_cast<UINT>(std::max(1, vis));
    si.nPos = std::clamp(g.top, 0, std::max(0, count - vis));
    SendMessageW(g.sb, SBM_SETSCROLLINFO, TRUE, reinterpret_cast<LPARAM>(&si));
    g.top = std::clamp(g.top, 0, std::max(0, count - vis));
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

// Per-row content type icon, loaded from an .ico resource. Two variants per
// type (light / dark) so the fixed-colour art keeps contrast on either
// background. Icons are cached at the current slot size and reloaded on DPI
// change (see cache invalidation alongside the thumbnail cache).
int TypeIconResId(ItemKind kind, bool dark) {
    switch (kind) {
        case ItemKind::Text:  return dark ? IDI_TYPE_TEXT_DARK  : IDI_TYPE_TEXT_LIGHT;
        case ItemKind::Rtf:   return dark ? IDI_TYPE_RTF_DARK   : IDI_TYPE_RTF_LIGHT;
        case ItemKind::Html:  return dark ? IDI_TYPE_HTML_DARK  : IDI_TYPE_HTML_LIGHT;
        case ItemKind::Image: return dark ? IDI_TYPE_IMAGE_DARK : IDI_TYPE_IMAGE_LIGHT;
        case ItemKind::FileDrop:
        default:              return dark ? IDI_TYPE_FILE_DARK  : IDI_TYPE_FILE_LIGHT;
    }
}

// Return a cached HICON for (kind, dark) sized to fit `slot`. Loads on demand.
HICON GetTypeIcon(ItemKind kind, bool dark, int slotSize) {
    // If the slot size changed (DPI switch), drop the whole cache and reload.
    if (g.typeIconSize != slotSize) {
        for (HICON& h : g.typeIcons) {
            if (h) { DestroyIcon(h); h = nullptr; }
        }
        g.typeIconSize = slotSize;
    }
    int kindIdx;
    switch (kind) {
        case ItemKind::Text:     kindIdx = 0; break;
        case ItemKind::Rtf:      kindIdx = 1; break;
        case ItemKind::Html:     kindIdx = 2; break;
        case ItemKind::Image:    kindIdx = 3; break;
        case ItemKind::FileDrop:
        default:                 kindIdx = 4; break;
    }
    int slot = kindIdx * 2 + (dark ? 1 : 0);
    if (!g.typeIcons[slot]) {
        // Request the exact size; a multi-size .ico lets Windows pick the best
        // embedded frame to scale from. Non-shared so we own the handle.
        g.typeIcons[slot] = static_cast<HICON>(
            LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(TypeIconResId(kind, dark)),
                       IMAGE_ICON, slotSize, slotSize, LR_DEFAULTCOLOR));
    }
    return g.typeIcons[slot];
}

// Per-row type indicator: draw the .ico centered in `slot`.
void DrawTypeIcon(HDC dc, const RECT& slot, ItemKind kind, bool dark) {
    int side = std::min(slot.right - slot.left, slot.bottom - slot.top);
    if (side < 4) side = 4;
    HICON icon = GetTypeIcon(kind, dark, side);
    if (!icon) return;
    int x = slot.left + (slot.right - slot.left - side) / 2;
    int y = slot.top + (slot.bottom - slot.top - side) / 2;
    DrawIconEx(dc, x, y, icon, side, side, 0, nullptr, DI_NORMAL);
}

void DrawRow(HDC dc, const RECT& rc, int index, const util::Theme& theme) {
    const Row& row = g.rows[static_cast<size_t>(index)];
    const Item* item = g.host->GetStore().Find(row.id);
    if (!item) {
        return;
    }
    const bool selected = index == g.sel;
    const bool dark = theme.bg < RGB(128, 128, 128);

    // Hard-coded pinned-row background per theme, kept in lockstep with
    // PaintAll's palette so we don't accidentally diverge after theme switch.
    const COLORREF K_PIN_BG_LIGHT = RGB(0xe4, 0xee, 0xfc); // #E4EEFC
    const COLORREF K_PIN_BG_DARK  = RGB(0x2a, 0x3a, 0x52); // #2A3A52

    COLORREF rowBg;
    if (selected) {
        rowBg = theme.sel;
    } else if (row.pinned) {
        rowBg = dark ? K_PIN_BG_DARK : K_PIN_BG_LIGHT;
    } else {
        rowBg = theme.bg;
    }
    HBRUSH back = CreateSolidBrush(rowBg);
    FillRect(dc, &rc, back);
    DeleteObject(back);
    SetBkMode(dc, TRANSPARENT);

    // ---- Leading strip: equal-spaced pin slot, type-icon slot, index slot ----
    // Layout (left to right, all left-aligned across every row):
    //   [gap][pin][gap][type][gap][index][gap] text...
    // Pin slot is a placeholder for unpinned rows so type/index/text stay aligned.
    int G = g.iconGap;
    int pinLeft   = rc.left + G;
    int typeLeft  = pinLeft + g.pinW + G;
    int indexLeft = typeLeft + g.typeW + G;

    // Pin icon (only for pinned rows; slot reserved either way)
    int pinSize = std::max(util::Scale(9, g.dpi), g.rowH / 3);
    if (row.pinned) {
        COLORREF pinColor = selected ? theme.selFg : theme.accent;
        DrawPinIcon(dc, pinLeft, rc.top + g.rowH / 2, pinSize, pinColor);
    }

    // Type icon (every row). Pick the light/dark icon variant by the actual
    // row background brightness (selection / pinned rows can differ from the
    // base theme) so the fixed-colour art keeps enough contrast.
    {
        RECT typeSlot = {typeLeft, rc.top, typeLeft + g.typeW, rc.bottom};
        bool bgDark = (GetRValue(rowBg) * 299 + GetGValue(rowBg) * 587 +
                       GetBValue(rowBg) * 114) / 1000 < 128;
        DrawTypeIcon(dc, typeSlot, item->kind, bgDark);
    }

    // Unified index (pinned + unpinned continuous)
    std::wstring indexText;
    if (index < 9) {
        indexText = util::Format(L"%d", index + 1);
    }
    RECT indexRc = rc;
    indexRc.left = indexLeft;
    indexRc.right = indexLeft + g.indexW;
    SelectObject(dc, g.fontSmall);
    SetTextColor(dc, selected ? theme.selFg : (row.pinned ? theme.accent : theme.dim));
    DrawTextW(dc, indexText.c_str(), -1, &indexRc,
              DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_NOPREFIX);

    // Text area
    // IMPORTANT: do NOT reserve a "hotkey strip" (g.hotkeyW) in row layout —
    // the current design does not render per-row hotkey labels on the right
    // edge of rows (confirmed by user screenshot showing blank trailing space
    // whenever we subtract hotkeyW). Hotkeys are documented only in the
    // bottom global hint bar. Only pinned rows give up horizontal space:
    // their text is truncated earlier so the 4 reorder icons fit cleanly
    // between the ellipsis and the right edge of the row (before scrollbar).
    RECT textRc = rc;
    textRc.left = indexRc.right + G;
    textRc.right = rc.right - g.pad / 2;
    if (row.pinned) {
        textRc.right -= g.sortBtnsW;
    }

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

    // (Type is now shown by the per-row type icon in the leading strip, so no
    // more inline "RTF"/"HTML" text badge here.)

    SelectObject(dc, g.font);
    SetTextColor(dc, selected ? theme.selFg : theme.fg);
    DrawTextW(dc, item->preview.c_str(), -1, &textRc,
              DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

// Helpers for hit-testing
static inline bool PtInRect(int x, int y, const RECT& r) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

static RECT CloseButtonRect() {
    RECT r = {g.width - g.titleH, 0, g.width, g.titleH};
    return r;
}

// Returns 0..3 for the 4 pinned-row sort buttons (First/Up/Down/Last), -1 if none
// Indices map to left-to-right visual order (First = leftmost, Last = rightmost).
// `buttonsRight` is the X coordinate of the RIGHT edge of the sort-button slot
// (i.e. the left edge of the hotkey strip minus the gap padding), so icons sit
// cleanly between the truncated text and the hotkey label.
static int HitTestSortButton(const Row& row, int buttonsRight, int rowTop, int x, int y, RECT* outBtnRect = nullptr) {
    if (!row.pinned) return -1;
    int btnSize = util::Scale(12, g.dpi);
    int btnCap = g.rowH - g.pad;
    if (btnSize > btnCap) btnSize = btnCap;
    int cy = rowTop + (g.rowH - btnSize) / 2;
    int btnGap = util::Scale(2, g.dpi);
    int btnMargin = util::Scale(3, g.dpi);
    int slotLeft = buttonsRight - g.sortBtnsW;
    int cx[4];
    cx[0] = slotLeft + btnMargin;
    cx[1] = cx[0] + btnSize + btnGap;
    cx[2] = cx[1] + btnSize + btnGap;
    cx[3] = cx[2] + btnSize + btnGap;
    for (int i = 0; i < 4; ++i) {
        RECT br = {cx[i], cy, cx[i] + btnSize, cy + btnSize};
        if (x >= br.left && x < br.right && y >= br.top && y < br.bottom) {
            if (outBtnRect) *outBtnRect = br;
            return i;
        }
    }
    return -1;
}

// Fills rects[0..3] with the screen-space button rects for a pinned row.
// Left-to-right order: rects[0]=First (to top), rects[1]=Up, rects[2]=Down, rects[3]=Last (to bottom).
// See HitTestSortButton for the `buttonsRight` semantics.
static void SortButtonRects(int buttonsRight, int rowTop, RECT rects[4]) {
    int btnSize = util::Scale(12, g.dpi);
    int btnCap = g.rowH - g.pad;
    if (btnSize > btnCap) btnSize = btnCap;
    int cy = rowTop + (g.rowH - btnSize) / 2;
    int btnGap = util::Scale(2, g.dpi);
    int btnMargin = util::Scale(3, g.dpi);
    int slotLeft = buttonsRight - g.sortBtnsW;
    int cx[4];
    cx[0] = slotLeft + btnMargin;
    cx[1] = cx[0] + btnSize + btnGap;
    cx[2] = cx[1] + btnSize + btnGap;
    cx[3] = cx[2] + btnSize + btnGap;
    for (int i = 0; i < 4; ++i) {
        rects[i].left = cx[i];
        rects[i].top = cy;
        rects[i].right = cx[i] + btnSize;
        rects[i].bottom = cy + btnSize;
    }
}

void PaintAll(HDC target, const RECT& client) {
    const util::Theme& theme = g.host->GetTheme();
    const bool dark = theme.bg < RGB(128, 128, 128);

    // Hard-coded brand palette (asymmetric light/dark, matching UX-specified
    // light-mode colors exactly; dark-mode derived via HSL inversion so the
    // visual hierarchy stays consistent while honoring dark-mode ergonomics:
    //   * Same hue family (cool blue 207-216°) for "brand blue" surfaces,
    //     so the product identity stays stable on theme toggle.
    //   * Luminance inverted: light backgrounds (L≈92-96%) become deep
    //     navy surfaces (L≈22-28%); dark foreground (L≈35%) becomes pale
    //     sky-blue (L≈70%), keeping WCAG AA contrast on every pairing.
    //   * Saturation dialed back ~30% in dark to avoid eye fatigue.
    //   * Close-hover red is the same #E81123 in both modes (OS semantic).
    const COLORREF K_TITLE_LIGHT       = RGB(0xd3, 0xe3, 0xfd);   // HSL(214° 85% 92%)  #D3E3FD
    const COLORREF K_TITLE_DARK        = RGB(0x1f, 0x36, 0x56);   // HSL(214° 55% 22%)  #1F3656
    const COLORREF K_EDIT_LIGHT        = RGB(0xed, 0xf2, 0xfa);   // HSL(216° 50% 96%)  #EDF2FA
    const COLORREF K_EDIT_DARK         = RGB(0x37, 0x4b, 0x63);   // HSL(216° 35% 28%)  #374B63
    const COLORREF K_CLOSE_HOVER       = RGB(0xe8, 0x11, 0x23);   // Semantic red      #E81123
    const COLORREF K_PIN_SORT_FG_LIGHT = RGB(0x2c, 0x5f, 0x8a);   // HSL(207° 51% 35%)  #2C5F8A
    const COLORREF K_PIN_SORT_FG_DARK  = RGB(0x9b, 0xc1, 0xe5);   // HSL(207° 45% 70%)  #9BC1E5
    const COLORREF K_PIN_BTN_HOVER_LIGHT = RGB(0xcc, 0xde, 0xf5); // Hover pill (light) #CCDEF5
    const COLORREF K_PIN_BTN_HOVER_DARK  = RGB(0x38, 0x55, 0x7a); // Hover pill (dark)  #38557A

    // Double buffering
    HDC mem = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);

    // Background
    HBRUSH bgBrush = CreateSolidBrush(theme.bg);
    FillRect(mem, &client, bgBrush);
    DeleteObject(bgBrush);

    // Title bar: UX-specified light-blue for light mode; symmetric deep-navy
    // for dark mode (same hue family, luminance inverted).
    RECT titleRc = {0, 0, client.right, g.titleH};
    COLORREF titleBg = dark ? K_TITLE_DARK : K_TITLE_LIGHT;
    HBRUSH titleBrush = CreateSolidBrush(titleBg);
    FillRect(mem, &titleRc, titleBrush);
    DeleteObject(titleBrush);
    SetBkMode(mem, TRANSPARENT);
    SelectObject(mem, g.font);
    SetTextColor(mem, theme.fg);
    RECT titleText = {g.pad, 0, client.right - g.titleH, g.titleH};
    DrawTextW(mem, i18n::T("popup.title"), -1, &titleText, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);

    // Close button (standard 46px titlebar button style). Hover => red fill
    // + white "X"; normal => text-colored "X" on title background.
    {
        RECT cRc = CloseButtonRect();
        COLORREF fill = g.closeHover ? K_CLOSE_HOVER : titleBg;
        HBRUSH cb = CreateSolidBrush(fill);
        FillRect(mem, &cRc, cb);
        DeleteObject(cb);
        int size = std::max(util::Scale(10, g.dpi), g.titleH / 3);
        int padX = (cRc.right - cRc.left - size) / 2;
        int padY = (cRc.bottom - cRc.top - size) / 2;
        COLORREF xc = g.closeHover ? RGB(255, 255, 255) : theme.fg;
        HPEN xPen = CreatePen(PS_SOLID, std::max(1, util::Scale(1, g.dpi)), xc);
        HGDIOBJ oldPen = SelectObject(mem, xPen);
        int x0 = cRc.left + padX;
        int y0 = cRc.top + padY;
        int x1 = cRc.right - padX;
        int y1 = cRc.bottom - padY;
        MoveToEx(mem, x0, y0, nullptr);
        LineTo(mem, x1, y1);
        MoveToEx(mem, x1, y0, nullptr);
        LineTo(mem, x0, y1);
        SelectObject(mem, oldPen);
        DeleteObject(xPen);
    }

    // Edit box background (matches title hue, one luminance step brighter in
    // light, one step dimmer in dark so the search field reads as a discrete
    // surface rather than a bordered hole).
    {
        RECT er = {g.pad, g.titleH + g.pad / 2, g.width - g.pad, g.titleH + g.pad / 2 + g.editH};
        COLORREF editBg = dark ? K_EDIT_DARK : K_EDIT_LIGHT;
        HBRUSH eb = CreateSolidBrush(editBg);
        FillRect(mem, &er, eb);
        DeleteObject(eb);
    }

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

    // Sort icons + hover highlight for pinned rows (painted on top of rows)
    {
        COLORREF fg = dark ? K_PIN_SORT_FG_DARK : K_PIN_SORT_FG_LIGHT;
        COLORREF fgDisabled = dark ? RGB(0x5b, 0x71, 0x8b) : RGB(0x9e, 0xb3, 0xcc);
        HPEN pn = CreatePen(PS_SOLID, std::max(1, util::Scale(1, g.dpi)), fg);
        HPEN pnDis = CreatePen(PS_SOLID, std::max(1, util::Scale(1, g.dpi)), fgDisabled);
        HGDIOBJ op = SelectObject(mem, pn);
        for (int i = g.top; i < end; ++i) {
            const Row& row = g.rows[static_cast<size_t>(i)];
            if (!row.pinned) continue;
            RECT rowRc;
            rowRc.left = list.left;
            rowRc.right = list.right;
            rowRc.top = list.top + (i - g.top) * g.rowH;
            rowRc.bottom = rowRc.top + g.rowH;
            RECT brs[4];
            int buttonsRight = rowRc.right - g.pad / 2;
            SortButtonRects(buttonsRight, rowRc.top, brs);
            int pinnedIdx = PinnedIndexOf(row.id);
            int pinnedCount = TotalPinnedCount();
            for (int b = 0; b < 4; ++b) {
                RECT br = brs[b];
                bool enabled = IsSortBtnEnabled(pinnedIdx, pinnedCount, b);
                bool hover = enabled && (g.hoverRow == i && g.hoverSortBtn == b);
                if (hover) {
                    HBRUSH hb = CreateSolidBrush(dark ? K_PIN_BTN_HOVER_DARK : K_PIN_BTN_HOVER_LIGHT);
                    RECT hbRc = br;
                    InflateRect(&hbRc, util::Scale(1, g.dpi), util::Scale(1, g.dpi));
                    FillRect(mem, &hbRc, hb);
                    DeleteObject(hb);
                }
                if (!enabled) {
                    SelectObject(mem, pnDis);
                }
                int cx = br.left + (br.right - br.left) / 2;
                int cy = br.top + (br.bottom - br.top) / 2;
                int w = (br.right - br.left) * 3 / 4;
                int h = (br.bottom - br.top) * 3 / 4;
                switch (b) {
                    case 0: {
                        int ay = cy;
                        MoveToEx(mem, cx - w / 2, ay, nullptr);
                        LineTo(mem, cx, ay - h / 2);
                        LineTo(mem, cx + w / 2, ay);
                        MoveToEx(mem, cx - w / 2, ay + h / 3, nullptr);
                        LineTo(mem, cx, ay - h / 6);
                        LineTo(mem, cx + w / 2, ay + h / 3);
                        break;
                    }
                    case 1: {
                        MoveToEx(mem, cx - w / 2, cy + h / 3, nullptr);
                        LineTo(mem, cx, cy - h / 3);
                        LineTo(mem, cx + w / 2, cy + h / 3);
                        break;
                    }
                    case 2: {
                        MoveToEx(mem, cx - w / 2, cy - h / 3, nullptr);
                        LineTo(mem, cx, cy + h / 3);
                        LineTo(mem, cx + w / 2, cy - h / 3);
                        break;
                    }
                    case 3: {
                        int ay = cy;
                        MoveToEx(mem, cx - w / 2, ay - h / 3, nullptr);
                        LineTo(mem, cx, ay + h / 6);
                        LineTo(mem, cx + w / 2, ay - h / 3);
                        MoveToEx(mem, cx - w / 2, ay, nullptr);
                        LineTo(mem, cx, ay + h / 2);
                        LineTo(mem, cx + w / 2, ay);
                        break;
                    }
                }
                if (!enabled) {
                    SelectObject(mem, pn);
                }
            }
        }
        SelectObject(mem, op);
        DeleteObject(pn);
        DeleteObject(pnDis);
    }

    // Drag reorder insert line
    if (g.reorderDrag && g.reorderInsert >= 0) {
        int y = list.top + (g.reorderInsert - g.top) * g.rowH;
        HPEN linePen = CreatePen(PS_SOLID, 2, theme.accent);
        HGDIOBJ op2 = SelectObject(mem, linePen);
        MoveToEx(mem, list.left, y, nullptr);
        LineTo(mem, list.right, y);
        SelectObject(mem, op2);
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
    HGDIOBJ op3 = SelectObject(mem, borderPen);
    HGDIOBJ ob3 = SelectObject(mem, GetStockObject(NULL_BRUSH));
    Rectangle(mem, 0, 0, client.right, client.bottom);
    SelectObject(mem, op3);
    SelectObject(mem, ob3);
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
            uint64_t selId = g.rows[static_cast<size_t>(g.sel)].id;
            int delta = (vk == VK_UP) ? -1 : 1;
            g.host->MovePinned(selId, delta);
            OnDataChanged();
            EnsureVisible();
            UpdateScrollbar();
            Redraw(false);
            return true;
        }
        return false;
    }
    if (ctrl && vk == 'P') {
        if (g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size()) {
            g.host->TogglePin(g.rows[static_cast<size_t>(g.sel)].id);
            OnDataChanged();
            EnsureVisible();
            UpdateScrollbar();
            Redraw(false);
        }
        return true;
    }
    if (ctrl && (vk == 'D' || vk == VK_DELETE)) {
        if (g.sel >= 0 && static_cast<size_t>(g.sel) < g.rows.size()) {
            uint64_t selId = g.rows[static_cast<size_t>(g.sel)].id;
            g.host->DeleteItem(selId);
            OnDataChanged();
            // If delete removed the last row, Rebuild() clamps sel to [0, rows.size()-1];
            // we still want it visible.
            EnsureVisible();
            UpdateScrollbar();
            Redraw(false);
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
        if (g.sel > 0) { --g.sel; EnsureVisible(); UpdateScrollbar(); Redraw(false); }
        return true;
    }
    if (vk == VK_DOWN) {
        if (g.sel < count - 1) { ++g.sel; EnsureVisible(); UpdateScrollbar(); Redraw(false); }
        return true;
    }
    if (vk == VK_PRIOR) {
        g.sel = std::max(0, g.sel - vis); EnsureVisible(); UpdateScrollbar(); Redraw(false);
        return true;
    }
    if (vk == VK_NEXT) {
        g.sel = std::min(count - 1, g.sel + vis); EnsureVisible(); UpdateScrollbar(); Redraw(false);
        return true;
    }
    if (ctrl && vk == VK_HOME) {
        g.sel = 0; EnsureVisible(); UpdateScrollbar(); Redraw(false); return true;
    }
    if (ctrl && vk == VK_END) {
        g.sel = count - 1; EnsureVisible(); UpdateScrollbar(); Redraw(false); return true;
    }
    return false;
}

void ShowRowMenu(int index) {
    if (index < 0 || static_cast<size_t>(index) >= g.rows.size()) {
        return;
    }
    g.sel = index;   // Rebuild() inside App::TogglePin/DeleteItem tracks THIS row via oldSelId.
    const Row& row = g.rows[static_cast<size_t>(index)];
    uint64_t rowId = row.id;
    const Item* item = g.host->GetStore().Find(rowId);
    // Rich-text items (RTF or HTML) carry formatting and can be flattened to
    // plain text. These are exactly the rows drawn with the "RTF" badge.
    const bool isRich = item && (item->kind == ItemKind::Rtf || item->kind == ItemKind::Html);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, i18n::T("popup.menu.copy"));
    AppendMenuW(menu, MF_STRING, 2, i18n::T("popup.menu.paste"));
    AppendMenuW(menu, MF_STRING | (isRich ? MF_ENABLED : MF_GRAYED), 5,
                i18n::T("popup.menu.to_plain"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3,
                row.pinned ? i18n::T("popup.menu.unpin") : i18n::T("popup.menu.pin"));
    AppendMenuW(menu, MF_STRING, 4, i18n::T("popup.menu.delete"));

    POINT pt;
    GetCursorPos(&pt);
    BeginModal();
    int cmd = TrackPopupMenu(menu,
                             TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, g.hwnd, nullptr);
    EndModal();
    DestroyMenu(menu);

    switch (cmd) {
        case 1: g.host->CopyItem(rowId); break;
        case 2: g.host->PasteItem(rowId); break;
        case 3: {
            if (g.host->GetStore().Find(rowId)) {
                g.host->TogglePin(rowId);
                EnsureVisible();
                UpdateScrollbar();
                Redraw(false);
            }
            break;
        }
        case 4: {
            if (g.host->GetStore().Find(rowId)) {
                g.host->DeleteItem(rowId);
                EnsureVisible();
                UpdateScrollbar();
                Redraw(false);
            }
            break;
        }
        case 5: {
            const Item* it = g.host->GetStore().Find(rowId);
            if (it && (it->kind == ItemKind::Rtf || it->kind == ItemKind::Html)) {
                // ConvertToPlainText triggers OnDataChanged -> Rebuild, which
                // may have removed rowId (merge). Select the survivor it returns.
                uint64_t survivorId = g.host->ConvertToPlainText(rowId);
                if (survivorId != 0) {
                    g.sel = -1;
                    for (size_t i = 0; i < g.rows.size(); ++i) {
                        if (g.rows[i].id == survivorId) {
                            g.sel = static_cast<int>(i);
                            break;
                        }
                    }
                    if (g.sel < 0 && !g.rows.empty()) g.sel = 0;
                }
                EnsureVisible();
                UpdateScrollbar();
                Redraw(false);
            }
            break;
        }
        default: break;
    }
}

// ---------------- Positioning ----------------

void PositionWindow() {
    int h = PopupHeight();
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
    // Ensure window stays on screen with at least 8px margin from any edge
    int margin = util::Scale(8, g.dpi);
    int minX = static_cast<int>(work.left) + margin;
    int maxX = std::max(minX, static_cast<int>(work.right) - g.width - margin);
    int minY = static_cast<int>(work.top) + margin;
    int maxY = std::max(minY, static_cast<int>(work.bottom) - h - margin);
    x = std::clamp(x, minX, maxX);
    y = std::clamp(y, minY, maxY);

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
            g.dpi = util::DpiOf(hwnd);
            RecreateFonts();
            CalcMetrics();
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
        case WM_COMMAND:
            if (HIWORD(wparam) == EN_CHANGE && reinterpret_cast<HWND>(lparam) == g.edit) {
                Rebuild();
                Redraw(false);  // Only invalidate list area, not the edit control
                return 0;
            }
            break;
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wparam);
            const util::Theme& theme = g.host->GetTheme();
            bool dark = theme.bg < RGB(128, 128, 128);
            const COLORREF K_EDIT_LIGHT = RGB(0xed, 0xf2, 0xfa); // #EDF2FA
            const COLORREF K_EDIT_DARK  = RGB(0x37, 0x4b, 0x63); // #374B63
            COLORREF bg = dark ? K_EDIT_DARK : K_EDIT_LIGHT;
            SetTextColor(hdc, theme.fg);
            SetBkColor(hdc, bg);
            if (!g.editBg) {
                g.editBg = CreateSolidBrush(bg);
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
        case WM_NCMOUSEMOVE: {
            // Close button is in non-client area (HTCLOSE from WM_NCHITTEST),
            // so hover must be tracked via WM_NCMOUSEMOVE, not WM_MOUSEMOVE.
            bool inClose = (wparam == HTCLOSE);
            if (inClose != g.closeHover) {
                g.closeHover = inClose;
                Redraw(true);
            }
            if (inClose) {
                // Request TME_NONCLIENT leave notification so we clear hover when mouse exits
                TRACKMOUSEEVENT tme = {sizeof(tme)};
                tme.dwFlags = TME_LEAVE | TME_NONCLIENT;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
            }
            return 0;
        }
        case WM_NCMOUSELEAVE: {
            if (g.closeHover) {
                g.closeHover = false;
                Redraw(true);
            }
            return 0;
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
            // Close button click
            RECT closeRc = CloseButtonRect();
            if (PtInRect(x, y, closeRc)) {
                Hide();
                return 0;
            }
            // Sort button click on pinned row
            RECT list = ListRect();
            if (y >= list.top && y < list.bottom && x >= list.left && x < list.right) {
                int idx = g.top + (y - list.top) / g.rowH;
                if (idx >= 0 && static_cast<size_t>(idx) < g.rows.size()) {
                    const Row& r = g.rows[static_cast<size_t>(idx)];
                    RECT rowRc;
                    rowRc.left = list.left;
                    rowRc.right = list.right;
                    rowRc.top = list.top + (idx - g.top) * g.rowH;
                    rowRc.bottom = rowRc.top + g.rowH;
                    int b = HitTestSortButton(r, rowRc.right - g.pad / 2, rowRc.top, x, y);
                    if (b >= 0 && r.pinned) {
                        int pinnedCount = TotalPinnedCount();
                        int current = PinnedIndexOf(r.id);
                        if (current >= 0 && IsSortBtnEnabled(current, pinnedCount, b)) {
                            int target = current;
                            switch (b) {
                                case 0: target = 0; break;
                                case 1: target = current - 1; break;
                                case 2: target = current + 1; break;
                                case 3: target = pinnedCount - 1; break;
                            }
                            if (target != current && target >= 0 && target < pinnedCount) {
                                g.sel = idx;  // so Rebuild() inside OnDataChanged tracks THIS row via oldSelId
                                g.host->ReorderPinned(r.id, target);
                            }
                        } else {
                            g.sel = idx;
                        }
                        Redraw(false);
                        return 0;
                    }
                    g.sel = idx;
                    Redraw(false);
                    // Start drag reorder in pinned area (disabled when filter is
                    // active because filtered row indices don't map 1:1 to the
                    // full pinned order that MovePinnedTo expects).
                    if (g.rows[static_cast<size_t>(idx)].pinned &&
                        !(g.edit && GetWindowTextLengthW(g.edit) > 0)) {
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
            int mx = GET_X_LPARAM(lparam);
            int my = GET_Y_LPARAM(lparam);
            g.lastPt = {mx, my};
            if (!g.mouseTracking) {
                TRACKMOUSEEVENT tme = {sizeof(tme)};
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                g.mouseTracking = true;
            }
            bool newCloseHover = false;
            int newHoverRow = -1;
            int newHoverBtn = -1;
            RECT closeRc = CloseButtonRect();
            if (PtInRect(mx, my, closeRc)) {
                newCloseHover = true;
            } else {
                RECT list = ListRect();
                if (mx >= list.left && mx < list.right && my >= list.top && my < list.bottom) {
                    int idx = g.top + (my - list.top) / g.rowH;
                    if (idx >= 0 && static_cast<size_t>(idx) < g.rows.size()) {
                        const Row& r = g.rows[static_cast<size_t>(idx)];
                        if (r.pinned) {
                            RECT rowRc;
                            rowRc.left = list.left;
                            rowRc.right = list.right;
                            rowRc.top = list.top + (idx - g.top) * g.rowH;
                            rowRc.bottom = rowRc.top + g.rowH;
                            int b = HitTestSortButton(r, rowRc.right - g.pad / 2, rowRc.top, mx, my);
                            if (b >= 0) {
                                int pi = PinnedIndexOf(r.id);
                                int pc = TotalPinnedCount();
                                if (IsSortBtnEnabled(pi, pc, b)) {
                                    newHoverRow = idx;
                                    newHoverBtn = b;
                                }
                            }
                        }
                    }
                }
            }
            if (newCloseHover != g.closeHover || newHoverRow != g.hoverRow || newHoverBtn != g.hoverSortBtn) {
                const bool closeChanged = (newCloseHover != g.closeHover);
                g.closeHover = newCloseHover;
                g.hoverRow = newHoverRow;
                g.hoverSortBtn = newHoverBtn;
                Redraw(closeChanged);  // Full redraw only when close button state changes
            }
            if (g.reorderDrag) {
                RECT list = ListRect();
                int y = my;
                int rel = y - list.top;
                int insertIdx = g.top + std::clamp(rel / g.rowH, 0, g.host->RowsVisible());
                insertIdx = std::clamp(insertIdx, 0, TotalPinnedCount());
                g.reorderInsert = insertIdx;
                Redraw(false);
                return 0;
            }
            // Ctrl+hover preview
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                RECT list = ListRect();
                int x = mx;
                int y = my;
                if (x >= list.left && x < list.right && y >= list.top && y < list.bottom) {
                    int idx = g.top + (y - list.top) / g.rowH;
                    if (idx != g.previewRow && idx >= 0 &&
                        static_cast<size_t>(idx) < g.rows.size()) {
                        HidePreview();
                        g.previewTimer = SetTimer(hwnd, 2, 300, nullptr);
                        g.previewRow = -2;
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
        case WM_MOUSELEAVE: {
            g.mouseTracking = false;
            if (g.closeHover || g.hoverRow >= 0 || g.hoverSortBtn >= 0) {
                bool needFull = g.closeHover;
                g.closeHover = false;
                g.hoverRow = -1;
                g.hoverSortBtn = -1;
                Redraw(needFull);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g.reorderDrag) {
                g.reorderDrag = false;
                ReleaseCapture();
                if (g.reorderFrom >= 0 && g.reorderInsert >= 0 && g.reorderFrom != g.reorderInsert) {
                    uint64_t id = g.rows[static_cast<size_t>(g.reorderFrom)].id;
                    // Track the dragged row after Rebuild: pin g.sel to reorderFrom BEFORE OnDataChanged
                    // (Rebuild's oldSelId mechanism then re-finds it by id at new position).
                    g.sel = g.reorderFrom;
                    g.host->ReorderPinned(id, g.reorderInsert);
                }
                g.reorderFrom = -1;
                g.reorderInsert = -1;
                Redraw(false);
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
                    Redraw(false);
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
            UpdateScrollbar();
            Redraw(false);
            return 0;
        }
        case WM_VSCROLL: {
            HWND sbWnd = reinterpret_cast<HWND>(lparam);
            if (sbWnd != g.sb) {
                return 0;
            }
            int vis = g.host->RowsVisible();
            int count = static_cast<int>(g.rows.size());
            int code = LOWORD(wparam);
            switch (code) {
                case SB_LINEUP:
                    g.top = std::max(0, g.top - 1);
                    break;
                case SB_LINEDOWN:
                    g.top = std::min(std::max(0, count - vis), g.top + 1);
                    break;
                case SB_PAGEUP:
                    g.top = std::max(0, g.top - vis);
                    break;
                case SB_PAGEDOWN:
                    g.top = std::min(std::max(0, count - vis), g.top + vis);
                    break;
                case SB_TOP:
                    g.top = 0;
                    break;
                case SB_BOTTOM:
                    g.top = std::max(0, count - vis);
                    break;
                case SB_THUMBTRACK:
                case SB_THUMBPOSITION: {
                    SCROLLINFO si = {sizeof(si)};
                    si.fMask = SIF_TRACKPOS;
                    if (GetScrollInfo(sbWnd, SB_CTL, &si)) {
                        g.top = std::clamp(static_cast<int>(si.nTrackPos), 0,
                                           std::max(0, count - vis));
                    }
                    break;
                }
                default:
                    return 0;
            }
            UpdateScrollbar();
            Redraw(false);
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
            g.dpi = HIWORD(wparam);
            RecreateFonts();
            CalcMetrics();
            if (g.editBg) { DeleteObject(g.editBg); g.editBg = nullptr; }
            ApplyPopupFontToControls();
            RelayoutWindow();
            for (auto& [id, t] : g.thumbs) {
                DeleteObject(t.bmp);
            }
            g.thumbs.clear();
            PositionWindow();
            UpdateScrollbar();
            Redraw(true);
            return 0;
        case WM_DESTROY:
            HidePreview();
            if (g.sb) {
                DestroyWindow(g.sb);
                g.sb = nullptr;
            }
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
                             WS_POPUP | WS_CLIPCHILDREN, 0, 0, 100, 100, nullptr, nullptr, inst, nullptr);
    if (!g.hwnd) {
        return false;
    }

    g.dpi = util::DpiOf(g.hwnd);
    RecreateFonts();
    CalcMetrics();

    // Filter edit box
    g.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                             g.pad, g.titleH + g.pad / 2, g.width - g.pad * 2, g.editH, g.hwnd,
                             nullptr, inst, nullptr);
    ApplyPopupFontToControls();
    SendMessageW(g.edit, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(i18n::T("popup.filter_hint")));
    g.editOrig = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(g.edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));

    // Vertical scrollbar (child, SBS_VERT) sized exactly to the list area
    {
        RECT list = ListRect();
        g.sb = CreateWindowExW(0, L"SCROLLBAR", L"",
                               WS_CHILD | WS_VISIBLE | SBS_VERT | SBS_RIGHTALIGN,
                               list.right, list.top, g.scrollW, list.bottom - list.top, g.hwnd,
                               nullptr, inst, nullptr);
    }

    return true;
}

void Shutdown() {
    if (g.hwnd) {
        DestroyWindow(g.hwnd);
        g.hwnd = nullptr;
    }
    for (HICON& h : g.typeIcons) {
        if (h) { DestroyIcon(h); h = nullptr; }
    }
    g.typeIconSize = 0;
}

void Show() {
    if (!g.hwnd || !g.host) {
        return;
    }
    // Reset any stale modal/drag/preview state first.
    if (g.modalDepth > 0) {
        g.modalDepth = 0;
    }
    if (g.reorderDrag) {
        g.reorderDrag = false;
        g.reorderFrom = -1;
        g.reorderInsert = -1;
        ReleaseCapture();
    }
    HidePreview();

    Rebuild();
    UpdateScrollbar();
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
    if (g.edit && IsWindow(g.edit)) {
        SetFocus(g.edit);
    }
    if (attached) {
        AttachThreadInput(myThread, fgThread, FALSE);
    }
    // Reset hover state so stale hovered-red/blue backgrounds never carry over
    // from the previous Show() session; also re-enable mouse-leave tracking.
    g.closeHover = false;
    g.hoverRow = -1;
    g.hoverSortBtn = -1;
    if (g.mouseTracking) {
        // Cancel old TME_LEAVE tracking, we will re-register on the first
        // WM_MOUSEMOVE we receive after the window is shown.
        TRACKMOUSEEVENT tme = {sizeof(tme)};
        tme.dwFlags = TME_LEAVE | TME_CANCEL;
        tme.hwndTrack = g.hwnd;
        TrackMouseEvent(&tme);
        g.mouseTracking = false;
    }
    // Probe current cursor position: if the mouse is already over the popup
    // area when it appears, force-sync hover state now so the close button
    // / sort-icon hover renders correctly even before the first WM_MOUSEMOVE.
    POINT pt = {};
    if (GetCursorPos(&pt)) {
        ScreenToClient(g.hwnd, &pt);
        RECT closeRc = CloseButtonRect();
        if (PtInRect(pt.x, pt.y, closeRc)) {
            g.closeHover = true;
        } else {
            RECT list = ListRect();
            if (pt.x >= list.left && pt.x < list.right && pt.y >= list.top && pt.y < list.bottom) {
                int idx = g.top + (pt.y - list.top) / g.rowH;
                if (idx >= 0 && static_cast<size_t>(idx) < g.rows.size()) {
                    const Row& r = g.rows[static_cast<size_t>(idx)];
                    if (r.pinned) {
                        RECT rowRc;
                        rowRc.left = list.left;
                        rowRc.right = list.right;
                        rowRc.top = list.top + (idx - g.top) * g.rowH;
                        rowRc.bottom = rowRc.top + g.rowH;
                        int b = HitTestSortButton(r, rowRc.right - g.pad / 2, rowRc.top, pt.x, pt.y);
                        if (b >= 0) {
                            int pi = PinnedIndexOf(r.id);
                            int pc = TotalPinnedCount();
                            if (IsSortBtnEnabled(pi, pc, b)) {
                                g.hoverRow = idx;
                                g.hoverSortBtn = b;
                            }
                        }
                    }
                }
            }
        }
    }
    Redraw(true);
}

void Hide() {
    HidePreview();
    if (!IsWindowVisible(g.hwnd)) {
        return;
    }
    // Cancel any in-progress drag reorder
    if (g.reorderDrag) {
        g.reorderDrag = false;
        g.reorderFrom = -1;
        g.reorderInsert = -1;
        ReleaseCapture();
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
    g.dpi = util::DpiOf(g.hwnd);
    RecreateFonts();
    CalcMetrics();
    if (g.editBg) {
        DeleteObject(g.editBg);
        g.editBg = nullptr;
    }
    ApplyPopupFontToControls();
    RelayoutWindow();
    // Refresh cue banner text (language may have changed)
    if (g.edit) {
        SendMessageW(g.edit, EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(i18n::T("popup.filter_hint")));
    }
    Redraw(true);
}

void OnDataChanged() {
    if (IsWindowVisible(g.hwnd)) {
        Rebuild();
        Redraw(true);
    }
}

void OnSettingsChanged() {
    // Font name / size and similar popup-affecting settings may have changed.
    RecreateFonts();
    CalcMetrics();
    ApplyPopupFontToControls();
    if (IsWindowVisible(g.hwnd)) {
        RelayoutWindow();
        Redraw(true);
    }
}

void SnapshotState(RuntimeState& out) {
    out = RuntimeState{};
    out.hwnd = g.hwnd;
    out.visible = (g.hwnd && IsWindowVisible(g.hwnd)) ? 1 : 0;
    out.modalDepth = g.modalDepth;
    out.reorderDrag = g.reorderDrag ? 1 : 0;
    out.rowsCount = static_cast<int>(g.rows.size());
    out.widthDip = g.width;
    out.heightDip = g.hwnd ? PopupHeight() : 0;
    out.dpi = static_cast<int>(g.dpi);
    out.lastSelRow = g.sel;
    out.pinnedCount = g.host ? g.host->GetStore().PinnedCount() : -1;
}

}  // namespace popup
