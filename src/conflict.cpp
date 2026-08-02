// conflict.cpp — Store.dat conflict resolution dialog
#include "conflict.h"

#include <commctrl.h>
#include <windowsx.h>

#include <vector>

#include "i18n.h"
#include "store.h"
#include "util.h"

namespace {

constexpr int kBtnUseLeft = 9001;
constexpr int kBtnUseRight = 9002;
constexpr int kBtnMerge = 9003;

struct ConflictState {
    HWND hwnd = nullptr;
    HWND listLeft = nullptr;
    HWND listRight = nullptr;
    HFONT font = nullptr;
    HFONT fontBold = nullptr;
    ConflictChoice result = ConflictChoice::Cancel;
    std::wstring leftDir;
    std::wstring rightDir;
    std::vector<Item> leftItems;
    std::vector<Item> rightItems;
};

ConflictState* g_state = nullptr;

void PopulateList(HWND list, const std::vector<Item>& items) {
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const Item& item : items) {
        std::wstring text;
        if (item.pinned) text += L"\x2605 ";  // ★
        text += item.preview;
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(text.c_str()));
    }
}

std::wstring FormatFileInfo(const std::wstring& dir, const std::vector<Item>& items) {
    std::wstring path = dir + L"\\store.dat";
    uint64_t size = util::FileSizeOf(path);
    std::wstring sizeStr;
    if (size > 1024 * 1024)
        sizeStr = util::Format(L"%.1f MB", static_cast<double>(size) / (1024.0 * 1024.0));
    else if (size > 1024)
        sizeStr = util::Format(L"%.1f KB", static_cast<double>(size) / 1024.0);
    else
        sizeStr = util::Format(L"%llu B", size);

    int pinned = 0;
    for (const Item& it : items) {
        if (it.pinned) ++pinned;
    }
    return util::Format(L"%s | %d %s, %d %s",
                        sizeStr.c_str(),
                        static_cast<int>(items.size()), i18n::T("conflict.items"),
                        pinned, i18n::T("conflict.pinned"));
}

LRESULT CALLBACK ConflictWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
        case WM_COMMAND: {
            int id = LOWORD(wparam);
            if (id == kBtnUseLeft) {
                g_state->result = ConflictChoice::UseLeft;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kBtnUseRight) {
                g_state->result = ConflictChoice::UseRight;
                DestroyWindow(hwnd);
                return 0;
            }
            if (id == kBtnMerge) {
                g_state->result = ConflictChoice::UseMerged;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            g_state->result = ConflictChoice::Cancel;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (g_state->font) { DeleteObject(g_state->font); g_state->font = nullptr; }
            if (g_state->fontBold) { DeleteObject(g_state->fontBold); g_state->fontBold = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

}  // namespace

ConflictChoice ShowConflictDialog(HWND parent, const std::wstring& leftDir, const std::wstring& rightDir) {
    ConflictState state;
    state.leftDir = leftDir;
    state.rightDir = rightDir;
    g_state = &state;

    Store::LoadItemsFrom(leftDir + L"\\store.dat", state.leftItems);
    Store::LoadItemsFrom(rightDir + L"\\store.dat", state.rightItems);

    HINSTANCE inst = GetModuleHandleW(nullptr);
    int dpi = util::DpiOf(parent);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ConflictWndProc;
    wc.hInstance = inst;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ClipWizConflict";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    int dlgW = util::Scale(640, dpi);
    int dlgH = util::Scale(440, dpi);
    int pad = util::Scale(12, dpi);
    int halfW = (dlgW - pad * 3) / 2;
    int btnH = util::Scale(28, dpi);
    int lblH = util::Scale(18, dpi);
    int listH = dlgH - pad * 2 - lblH * 3 - btnH * 2 - util::Scale(30, dpi);

    // Center on parent
    RECT prc;
    GetWindowRect(parent, &prc);
    int x = (prc.left + prc.right - dlgW) / 2;
    int y = (prc.top + prc.bottom - dlgH) / 2;

    HWND hwnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"ClipWizConflict",
                                i18n::T("conflict.title"),
                                WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
                                x, y, dlgW, dlgH, parent, nullptr, inst, nullptr);
    state.hwnd = hwnd;

    // Font
    state.font = util::CreateUiFont(dpi, 0);
    state.fontBold = util::CreateUiFont(dpi, 0);
    LOGFONTW lf;
    GetObjectW(state.fontBold, sizeof(lf), &lf);
    lf.lfWeight = FW_BOLD;
    DeleteObject(state.fontBold);
    state.fontBold = CreateFontIndirectW(&lf);

    int cy = pad;

    // Left panel header
    HWND lblL = CreateWindowExW(0, L"STATIC", i18n::T("conflict.user_dir"),
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                pad, cy, halfW, lblH, hwnd, nullptr, inst, nullptr);
    SendMessageW(lblL, WM_SETFONT, reinterpret_cast<WPARAM>(state.fontBold), TRUE);

    // Right panel header
    HWND lblR = CreateWindowExW(0, L"STATIC", i18n::T("conflict.prog_dir"),
                                WS_CHILD | WS_VISIBLE | SS_LEFT,
                                pad * 2 + halfW, cy, halfW, lblH, hwnd, nullptr, inst, nullptr);
    SendMessageW(lblR, WM_SETFONT, reinterpret_cast<WPARAM>(state.fontBold), TRUE);
    cy += lblH + 2;

    // Left path
    HWND lblLPath = CreateWindowExW(0, L"STATIC", leftDir.c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                                   pad, cy, halfW, lblH, hwnd, nullptr, inst, nullptr);
    SendMessageW(lblLPath, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);

    // Right path
    HWND lblRPath = CreateWindowExW(0, L"STATIC", rightDir.c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_LEFT | SS_PATHELLIPSIS,
                                   pad * 2 + halfW, cy, halfW, lblH, hwnd, nullptr, inst, nullptr);
    SendMessageW(lblRPath, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    cy += lblH + 2;

    // Left info
    std::wstring leftInfo = FormatFileInfo(leftDir, state.leftItems);
    HWND lblLInfo = CreateWindowExW(0, L"STATIC", leftInfo.c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   pad, cy, halfW, lblH, hwnd, nullptr, inst, nullptr);
    SendMessageW(lblLInfo, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);

    // Right info
    std::wstring rightInfo = FormatFileInfo(rightDir, state.rightItems);
    HWND lblRInfo = CreateWindowExW(0, L"STATIC", rightInfo.c_str(),
                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                   pad * 2 + halfW, cy, halfW, lblH, hwnd, nullptr, inst, nullptr);
    SendMessageW(lblRInfo, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    cy += lblH + 4;

    // Left listbox
    state.listLeft = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                     pad, cy, halfW, listH, hwnd, nullptr, inst, nullptr);
    SendMessageW(state.listLeft, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    PopulateList(state.listLeft, state.leftItems);

    // Right listbox
    state.listRight = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                                      WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOINTEGRALHEIGHT,
                                      pad * 2 + halfW, cy, halfW, listH, hwnd, nullptr, inst, nullptr);
    SendMessageW(state.listRight, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    PopulateList(state.listRight, state.rightItems);
    cy += listH + 8;

    // Buttons row 1: left and right
    int btnW = halfW;
    HWND btnL = CreateWindowExW(0, L"BUTTON", i18n::T("conflict.use_left"),
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                pad, cy, btnW, btnH, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnUseLeft)), inst, nullptr);
    SendMessageW(btnL, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);

    HWND btnR = CreateWindowExW(0, L"BUTTON", i18n::T("conflict.use_right"),
                                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                pad * 2 + halfW, cy, btnW, btnH, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnUseRight)), inst, nullptr);
    SendMessageW(btnR, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    cy += btnH + 6;

    // Button row 2: merge (centered)
    int mergeW = util::Scale(240, dpi);
    HWND btnM = CreateWindowExW(0, L"BUTTON", i18n::T("conflict.merge"),
                                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                                (dlgW - mergeW) / 2, cy, mergeW, btnH, hwnd,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kBtnMerge)), inst, nullptr);
    SendMessageW(btnM, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);

    // Modal loop
    EnableWindow(parent, FALSE);
    MSG msgLoop;
    while (GetMessageW(&msgLoop, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hwnd, &msgLoop)) {
            TranslateMessage(&msgLoop);
            DispatchMessageW(&msgLoop);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);

    UnregisterClassW(L"ClipWizConflict", inst);
    g_state = nullptr;
    return state.result;
}
