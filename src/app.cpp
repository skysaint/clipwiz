// app.cpp
#include "app.h"

#include <algorithm>

#include "clipboard.h"
#include "i18n.h"
#include "imagecodec.h"
#include "log.h"
#include "paste.h"
#include "resource.h"
#include "tray.h"

namespace {

const wchar_t kMainClass[] = L"ClipWizMain";
constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTimerSave = 1;
constexpr UINT kTimerWriteCheck = 2;
constexpr UINT kTimerClipboard = 3;
constexpr UINT kClipboardDelayMs = 150;  // Debounce: wait for multi-stage copy (Excel) to settle
constexpr int kSaveDelayMs = 800;
constexpr int kWriteCheckMs = 200;

// Prompt cleanup when total exceeds this value (100 MB)
constexpr uint64_t kSizeWarnBytes = 100ULL * 1024 * 1024;

}  // namespace

UINT SingleInstanceMessage() {
    static UINT msg = RegisterWindowMessageW(L"ClipWiz.ShowPopup.7A1C");
    return msg;
}

// ------------------------------------------------------------------ Initialization

bool App::Init(HINSTANCE inst) {
    inst_ = inst;

    // Load config
    settings::Load(cfg_);
    settings::Clamp(cfg_);

    // Data directory is auto-detected (portable vs installed)
    // No manual override needed

    // i18n
    i18n::Init(cfg_.language);

    // WIC
    if (!imagecodec::Init()) {
        LOG_ERROR("WIC initialization failed");
        util::ErrorBox(nullptr, L"Error ERR_WIC_INIT: Failed to initialize image codec (WIC).");
        return false;
    }

    // Theme
    ApplyTheme();

    // Register window class
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    if (!RegisterClassExW(&wc)) {
        LOG_ERROR("Failed to register window class, error=%u", GetLastError());
        util::ErrorBox(nullptr, L"Error ERR_WINDOW_CLASS: Failed to register window class.");
        return false;
    }

    // Hidden main window (message carrier)
    hwnd_ = CreateWindowExW(0, kMainClass, L"ClipWiz", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) {
        LOG_ERROR("Failed to create main window, error=%u", GetLastError());
        util::ErrorBox(nullptr, L"Error ERR_WINDOW_CREATE: Failed to create main window.");
        return false;
    }

    // Load data
    Store::LoadResult lr = store_.Load();
    store_.SetLimits(cfg_.maxHistory, cfg_.expiryDays);
    if (lr == Store::LoadResult::Corrupt) {
        util::ErrorBox(nullptr, i18n::T("msg.corrupt_found"));
    }

    // Clipboard listener
    if (!clip::StartListening(hwnd_)) {
        LOG_ERROR("Failed to start clipboard listener");
        util::ErrorBox(nullptr, L"Error ERR_CLIPBOARD_LISTENER: Failed to start clipboard listener.");
        return false;
    }

    // Foreground window tracking
    if (!paste::InstallHook()) {
        LOG_WARNING("Failed to install foreground window hook, paste target may be degraded");
        // Failure doesn't break main flow, paste target just degrades to current foreground window
    }

    // Tray - critical component, exit on failure
    if (!tray::Add(hwnd_, kMsgTray, IDI_APPICON)) {
        LOG_ERROR("Failed to add tray icon");
        util::ErrorBox(nullptr, L"Error ERR_TRAY_ICON: Failed to create system tray icon.");
        return false;
    }
    tray::SetTip(L"ClipWiz");

    // Hotkeys
    hotkeys_.Attach(hwnd_);
    RegisterAllHotkeys(true);

    // Async disk writer thread
    if (!writer_.Start()) {
        LOG_ERROR("Failed to start async writer thread");
        util::ErrorBox(nullptr, L"Error ERR_ASYNC_WRITER: Failed to start background writer.");
        return false;
    }

    // Popup window
    if (!popup::Init(inst, this)) {
        LOG_ERROR("Failed to initialize popup window");
        util::ErrorBox(nullptr, L"Error ERR_POPUP_INIT: Failed to initialize popup window.");
        return false;
    }

    return true;
}

int App::Run() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

// ------------------------------------------------------------------ WndProc

LRESULT CALLBACK App::WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    App* self = nullptr;
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) {
        return self->WndProc(hwnd, msg, wparam, lparam);
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT App::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == SingleInstanceMessage()) {
        popup::Toggle();
        return 0;
    }

    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {
        tray::Restore();
        return 0;
    }

    switch (msg) {
        case WM_CLIPBOARDUPDATE:
            // Debounce: apps like Excel open/close the clipboard several times
            // during a single copy. Restart the timer on each update and only
            // capture after things settle, so we never grab mid-copy.
            SetTimer(hwnd_, kTimerClipboard, kClipboardDelayMs, nullptr);
            return 0;

        case WM_HOTKEY:
            OnHotkey(static_cast<int>(wparam));
            return 0;

        case kMsgTray:
            OnTrayMessage(static_cast<UINT>(lparam));
            return 0;

        case WM_COMMAND:
            OnCommand(LOWORD(wparam));
            return 0;

        case WM_CLOSE:
            LOG_INFO("WM_CLOSE received, initiating shutdown");
            DestroyWindow(hwnd_);
            return 0;

        case WM_TIMER:
            if (wparam == kTimerSave) {
                OnTimerSave();
            } else if (wparam == kTimerWriteCheck) {
                OnTimerWriteCheck();
            } else if (wparam == kTimerClipboard) {
                KillTimer(hwnd_, kTimerClipboard);
                OnClipboardUpdate();
            }
            return 0;

        case WM_SETTINGCHANGE: {
            // Dark mode switch
            if (lparam && wcscmp(reinterpret_cast<const wchar_t*>(lparam), L"ImmersiveColorSet") == 0) {
                ApplyTheme();
                popup::OnThemeChanged();
            }
            return 0;
        }

        case WM_QUERYENDSESSION:
            SaveNow();
            return TRUE;

        case WM_ENDSESSION:
            if (wparam) {
                SaveNow();
            }
            return 0;

        case WM_DESTROY:
            LOG_INFO("WM_DESTROY received, starting shutdown sequence");
            
            // Decide exit strategy based on save state
            if (saveState_ == SaveState::NoSaveNeeded) {
                LOG_INFO("No save needed, data is already up to date");
            } else if (saveState_ == SaveState::PendingSave) {
                LOG_INFO("Pending save detected, cancelling timer and saving synchronously");
                KillTimer(hwnd, kTimerSave);
                saveState_ = SaveState::SavingInProgress;
                store_.ExpireCheck();
                {
                    std::vector<uint8_t> buf = store_.Serialize();
                    bool saved = util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
                    if (saved) {
                        LOG_INFO("Data saved synchronously successfully");
                    } else {
                        LOG_ERROR("Failed to save data synchronously during shutdown");
                    }
                }
            } else if (saveState_ == SaveState::SavingInProgress) {
                LOG_INFO("Async save in progress, waiting for completion");
                // Wait for async save to complete (max 5 seconds)
                int waitCount = 0;
                while (!writer_.Done() && waitCount < 50) {
                    Sleep(100);
                    waitCount++;
                }
                if (writer_.Done()) {
                    LOG_INFO("Async save completed");
                } else {
                    LOG_WARNING("Async save did not complete in time, forcing shutdown");
                }
            }
            
            // Stop async writer thread (data already handled)
            writer_.Stop();
            LOG_INFO("Async writer stopped");
            
            // Clean up other resources
            clip::StopListening(hwnd);
            paste::RemoveHook();
            hotkeys_.UnregisterAll();
            tray::Remove();
            popup::Shutdown();
            imagecodec::Shutdown();
            
            LOG_INFO("All resources cleaned up, posting quit message");
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------------ Event handling

void App::OnClipboardUpdate() {
    if (clip::IsSelfWrite()) {
        return;
    }
    ItemKind kind;
    std::vector<uint8_t> data;
    uint32_t imgW = 0, imgH = 0;
    if (!clip::Capture(kind, data, imgW, imgH, cfg_.maxTextBytes, cfg_.maxImagePixels)) {
        return;
    }
    store_.Add(kind, std::move(data), imgW, imgH);
    popup::OnDataChanged();
    ScheduleSave();
    CheckStoreSize();
}

void App::OnHotkey(int id) {
    if (id == hotkey::kIdPopup) {
        paste::CaptureCurrentForeground();
        popup::Toggle();
        return;
    }
    // Positional: id = kIdItemBase + position index
    int index = id - hotkey::kIdItemBase;
    if (index < 0 || index >= 10) {
        return;
    }
    // Find the Nth pinned item
    int seen = 0;
    for (const Item& item : store_.Items()) {
        if (!item.pinned) {
            break;  // Pinned section is at the front
        }
        if (seen == index) {
            paste::CaptureCurrentForeground();
            PasteItem(item.id);
            return;
        }
        ++seen;
    }
}

void App::OnTrayMessage(UINT mouseMsg) {
    switch (mouseMsg) {
        case WM_LBUTTONUP:
            // Single click to show popup
            paste::CaptureCurrentForeground();
            popup::Toggle();
            break;
        case WM_RBUTTONUP: {
            // Right-click context menu
            std::vector<tray::PinnedEntry> pinned;
            int idx = 0;
            for (const Item& item : store_.Items()) {
                if (!item.pinned) {
                    break;
                }
                tray::PinnedEntry e;
                e.text = item.preview;
                if (idx < 10 && cfg_.pinnedHotkeys[idx] != 0) {
                    e.hotkey = hotkey::ToText(cfg_.pinnedHotkeys[idx]);
                }
                pinned.push_back(std::move(e));
                ++idx;
                if (idx >= 20) {
                    break;
                }
            }
            UINT cmd = tray::ShowMenu(hwnd_, pinned, util::GetAutostart(),
                                      hotkey::ToText(cfg_.popupHotkey));
            if (cmd != 0) {
                OnCommand(cmd);
            }
            break;
        }
        default:
            break;
    }
}

void App::OnCommand(UINT cmd) {
    if (cmd == tray::CmdShowPopup) {
        paste::CaptureCurrentForeground();
        popup::Toggle();
        return;
    }
    if (cmd == tray::CmdSettings) {
        OpenSettings();
        return;
    }
    if (cmd == tray::CmdClearHistory) {
        ClearHistory();
        return;
    }
    if (cmd == tray::CmdAutostart) {
        util::SetAutostart(!util::GetAutostart());
        return;
    }
    if (cmd == tray::CmdAbout) {
        ShowAbout();
        return;
    }
    if (cmd == tray::CmdExit) {
        LOG_INFO("Exit command received");
        DestroyWindow(hwnd_);
        return;
    }
    // Pinned item direct paste from menu
    if (cmd >= tray::CmdPinnedBase && cmd < tray::CmdPinnedBase + 20) {
        int index = static_cast<int>(cmd - tray::CmdPinnedBase);
        int seen = 0;
        for (const Item& item : store_.Items()) {
            if (!item.pinned) {
                break;
            }
            if (seen == index) {
                paste::CaptureCurrentForeground();
                PasteItem(item.id);
                return;
            }
            ++seen;
        }
    }
}

void App::OnTimerSave() {
    KillTimer(hwnd_, kTimerSave);
    saveState_ = SaveState::SavingInProgress;
    SaveNow();
}

void App::OnTimerWriteCheck() {
    if (writer_.Done()) {
        KillTimer(hwnd_, kTimerWriteCheck);
        saveState_ = SaveState::NoSaveNeeded;
    }
}

// ------------------------------------------------------------------ Host implementation

void App::PasteItem(uint64_t id) {
    const Item* item = store_.Find(id);
    if (!item) {
        return;
    }
    popup::Hide();
    if (!clip::WriteItem(hwnd_, *item)) {
        return;
    }
    store_.Touch(id);
    popup::OnDataChanged();
    ScheduleSave();
    paste::Execute(cfg_.pasteDelayMs);
}

void App::CopyItem(uint64_t id) {
    const Item* item = store_.Find(id);
    if (!item) {
        return;
    }
    clip::WriteItem(hwnd_, *item);
    store_.Touch(id);
    popup::OnDataChanged();
    ScheduleSave();
}

void App::TogglePin(uint64_t id) {
    const Item* item = store_.Find(id);
    if (!item) return;
    store_.SetPinned(id, !item->pinned);
    popup::OnDataChanged();
    SaveNow();
    RegisterAllHotkeys(false);
}

void App::DeleteItem(uint64_t id) {
    const Item* item = store_.Find(id);
    if (!item) {
        return;
    }
    if (item->pinned) {
        const bool overPopup = popup::IsVisible();
        if (overPopup) {
            popup::BeginModal();
        }
        bool ok = util::ConfirmBox(overPopup ? popup::Window() : nullptr,
                                   i18n::T("msg.confirm_delete_pinned"));
        if (overPopup) {
            popup::EndModal();
            SetForegroundWindow(popup::Window());
            SetFocus(popup::Window());
        }
        if (!ok) {
            return;
        }
    }
    store_.Remove(id);
    popup::OnDataChanged();
    SaveNow();
    RegisterAllHotkeys(false);
}

void App::MovePinned(uint64_t id, int delta) {
    if (store_.MovePinned(id, delta)) {
        popup::OnDataChanged();
        SaveNow();
    }
}

void App::SaveLastPos(int x, int y) {
    cfg_.lastPopupX = x;
    cfg_.lastPopupY = y;
    settings::Save(cfg_);
}

void App::OpenSettings() {
    if (settingsOpen_) {
        settings::ActivateExisting();
        return;
    }
    settingsOpen_ = true;
    hotkeys_.UnregisterAll();

    settings::Config old = cfg_;
    if (settings::ShowDialog(hwnd_, inst_, cfg_)) {
        settings::Clamp(cfg_);
        settings::Save(cfg_);
        ApplyConfig();
    } else {
        cfg_ = old;
    }

    RegisterAllHotkeys(true);
    settingsOpen_ = false;
}

// ------------------------------------------------------------------ Internal

void App::ApplyTheme() {
    bool dark = false;
    switch (cfg_.theme) {
        case settings::ThemeMode::Auto:
            dark = util::IsSystemDarkMode();
            break;
        case settings::ThemeMode::Dark:
            dark = true;
            break;
        default:
            break;
    }
    theme_ = util::MakeTheme(dark);
}

void App::ApplyConfig() {
    ApplyTheme();
    store_.SetLimits(cfg_.maxHistory, cfg_.expiryDays);
    i18n::Init(cfg_.language);
    popup::OnThemeChanged();
    popup::OnDataChanged();
}

void App::RegisterAllHotkeys(bool reportFailures) {
    hotkeys_.UnregisterAll();
    hotkeyFailures_.clear();

    if (cfg_.popupHotkey != 0) {
        if (!hotkeys_.Register(hotkey::kIdPopup, cfg_.popupHotkey)) {
            hotkeyFailures_ += hotkey::ToText(cfg_.popupHotkey) + L"\n";
        }
    }
    for (int i = 0; i < 10; ++i) {
        if (cfg_.pinnedHotkeys[i] != 0) {
            if (!hotkeys_.Register(hotkey::kIdItemBase + i, cfg_.pinnedHotkeys[i])) {
                hotkeyFailures_ += hotkey::ToText(cfg_.pinnedHotkeys[i]) + L"\n";
            }
        }
    }
    if (reportFailures && !hotkeyFailures_.empty()) {
        std::wstring msg = std::wstring(i18n::T("msg.hotkey_conflict")) + L"\n" + hotkeyFailures_ +
                           L"\n" + i18n::T("msg.hotkey_suggest");
        util::ErrorBox(nullptr, msg);
    }
}

void App::ScheduleSave() {
    if (saveState_ == SaveState::NoSaveNeeded) {
        saveState_ = SaveState::PendingSave;
        SetTimer(hwnd_, kTimerSave, kSaveDelayMs, nullptr);
    }
    // If currently PendingSave or SavingInProgress, do nothing
    // PendingSave: timer already set, no need to set again
    // SavingInProgress: async save in progress, new data will be overwritten
}

void App::SaveNow() {
    // Cancel timer
    if (saveState_ == SaveState::PendingSave) {
        KillTimer(hwnd_, kTimerSave);
    }
    
    // Mark as saving in progress
    saveState_ = SaveState::SavingInProgress;
    
    store_.ExpireCheck();
    std::vector<uint8_t> buf = store_.Serialize();
    writer_.Submit(util::StorePath(), std::move(buf));
    
    // Timer to check write completion
    SetTimer(hwnd_, kTimerWriteCheck, kWriteCheckMs, nullptr);
}

void App::ShowAbout() {
    // Build about dialog with supported types in a left-right layout
    // Use an in-memory dialog template (same approach as settings dialog)
    struct AboutState {
        static INT_PTR CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
            (void)lparam;
            switch (msg) {
                case WM_INITDIALOG: {
                    int dpi = util::DpiOf(hwnd);
                    auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };

                    // Get system font
                    NONCLIENTMETRICSW ncm{};
                    ncm.cbSize = sizeof(ncm);
                    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                               static_cast<UINT>(dpi));
                    HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

                    auto MkCtrl = [&](const wchar_t* cls, const wchar_t* text, DWORD style,
                                      int x, int y, int w, int h, int id, DWORD ex = 0) -> HWND {
                        HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style,
                            S(x), S(y), S(w), S(h), hwnd,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            GetModuleHandleW(nullptr), nullptr);
                        if (c && font) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(font), FALSE);
                        return c;
                    };

                    // About text
                    std::wstring aboutStr = util::Format(i18n::T("about.text"), L"1.1.0");
                    MkCtrl(L"STATIC", aboutStr.c_str(), SS_LEFT, 12, 10, 420, 50, -1);

                    // Separator
                    MkCtrl(L"STATIC", L"", SS_ETCHEDHORZ, 12, 62, 420, 2, -1);

                    // Types section header
                    MkCtrl(L"STATIC", i18n::T("settings.tab.types"), SS_LEFT, 12, 70, 420, 16, -1);

                    // Left: type list (height for 6 rows, 5 items + 1 empty row)
                    constexpr int kListY = 90;
                    constexpr int kListH = 108;
                    HWND list = MkCtrl(L"LISTBOX", L"",
                        LBS_NOTIFY | WS_TABSTOP, 12, kListY, 160, kListH, 2001, WS_EX_CLIENTEDGE);
                    const char* typeKeys[] = {"type.text.name", "type.image.name",
                        "type.html.name", "type.rtf.name", "type.filedrop.name"};
                    for (const char* k : typeKeys)
                        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(i18n::T(k)));
                    SendMessageW(list, LB_SETCURSEL, 0, 0);

                    // Right: description (same height as list)
                    MkCtrl(L"EDIT", i18n::T("type.text.desc"),
                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                        178, kListY, 254, kListH, 2002, WS_EX_CLIENTEDGE);

                    // OK button
                    MkCtrl(L"BUTTON", i18n::T("settings.ok"),
                        BS_DEFPUSHBUTTON | WS_TABSTOP, 180, kListY + kListH + 10, 80, 26, IDOK);

                    // Resize and center
                    constexpr int kDlgW = 450;
                    constexpr int kDlgH = kListY + kListH + 10 + 26 + 12;
                    RECT rc = {0, 0, S(kDlgW), S(kDlgH)};
                    AdjustWindowRectEx(&rc, GetWindowLongW(hwnd, GWL_STYLE), FALSE,
                                       GetWindowLongW(hwnd, GWL_EXSTYLE));
                    int w = rc.right - rc.left;
                    int h = rc.bottom - rc.top;
                    RECT wa;
                    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
                    SetWindowPos(hwnd, nullptr,
                        wa.left + (wa.right - wa.left - w) / 2,
                        wa.top + (wa.bottom - wa.top - h) / 2,
                        w, h, SWP_NOZORDER);

                    // Store font handle for cleanup
                    SetPropW(hwnd, L"Font", font);
                    return TRUE;
                }
                case WM_COMMAND: {
                    int id = LOWORD(wparam);
                    int notif = HIWORD(wparam);
                    if (id == IDOK || id == IDCANCEL) {
                        EndDialog(hwnd, IDOK);
                        return TRUE;
                    }
                    if (id == 2001 && notif == LBN_SELCHANGE) {
                        int sel = static_cast<int>(
                            SendMessageW(GetDlgItem(hwnd, 2001), LB_GETCURSEL, 0, 0));
                        const char* descKeys[] = {"type.text.desc", "type.image.desc",
                            "type.html.desc", "type.rtf.desc", "type.filedrop.desc"};
                        if (sel >= 0 && sel < 5)
                            SetDlgItemTextW(hwnd, 2002, i18n::T(descKeys[sel]));
                        return TRUE;
                    }
                    break;
                }
                case WM_CLOSE:
                    EndDialog(hwnd, IDOK);
                    return TRUE;
                case WM_DESTROY: {
                    HFONT font = static_cast<HFONT>(RemovePropW(hwnd, L"Font"));
                    if (font) DeleteObject(font);
                    return TRUE;
                }
            }
            return FALSE;
        }
    };

    // Build in-memory dialog template
    std::vector<WORD> buf;
    DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_SETFONT;
    buf.push_back(LOWORD(style));
    buf.push_back(HIWORD(style));
    buf.push_back(0); buf.push_back(0);  // exstyle
    buf.push_back(0);                     // cdit
    buf.push_back(0); buf.push_back(0);   // x, y
    buf.push_back(100); buf.push_back(100); // cx, cy (resized in initdialog)
    buf.push_back(0);                     // menu
    buf.push_back(0);                     // class
    // Title
    const wchar_t* title = i18n::T("tray.about");
    while (*title) { buf.push_back(static_cast<WORD>(*title)); ++title; }
    buf.push_back(0);
    buf.push_back(9);                     // font size
    const wchar_t* fn = L"Segoe UI";
    while (*fn) { buf.push_back(static_cast<WORD>(*fn)); ++fn; }
    buf.push_back(0);

    DialogBoxIndirectParamW(inst_, reinterpret_cast<LPCDLGTEMPLATEW>(buf.data()),
                            hwnd_, AboutState::Proc, 0);
}

void App::ClearHistory() {
    if (!util::ConfirmBox(nullptr, i18n::T("msg.confirm_clear"))) {
        return;
    }
    // Only delete unpinned items
    std::vector<uint64_t> toRemove;
    for (const Item& item : store_.Items()) {
        if (!item.pinned) {
            toRemove.push_back(item.id);
        }
    }
    for (uint64_t id : toRemove) {
        store_.Remove(id);
    }
    popup::OnDataChanged();
    SaveNow();
}

void App::CheckStoreSize() {
    if (sizeWarned_) {
        return;
    }
    uint64_t total = store_.TotalDataSize();
    if (total < kSizeWarnBytes) {
        return;
    }
    sizeWarned_ = true;
    // Prompt user for cleanup
    if (!util::ConfirmBox(nullptr, i18n::T("msg.large_data"))) {
        return;
    }
    // Remove unpinned items exceeding threshold
    uint64_t threshold =
        static_cast<uint64_t>(cfg_.largeItemThresholdMB) * 1024ULL * 1024ULL;
    std::vector<uint64_t> toRemove;
    for (const Item& item : store_.Items()) {
        if (!item.pinned && item.data.size() > threshold) {
            toRemove.push_back(item.id);
        }
    }
    for (uint64_t id : toRemove) {
        store_.Remove(id);
    }
    popup::OnDataChanged();
    SaveNow();
}
