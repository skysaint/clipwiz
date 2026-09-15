// app.cpp
#include "app.h"

#include <algorithm>
#include <commdlg.h>
#include <shlobj.h>

#include "clipboard.h"
#include "i18n.h"
#include "imagecodec.h"
#include "log.h"
#include "merge.h"
#include "paste.h"
#include "resource.h"
#include "textconv.h"
#include "transform.h"
#include "tray.h"

namespace {

const wchar_t kMainClass[] = L"ClipWizMain";
// File-dialog filter for .clpw backups, in the "label\0pattern\0\0" form that
// OPENFILENAME.lpstrFilter expects. An array literal is used deliberately: it
// keeps the embedded NULs, whereas a wchar_t* / std::wstring built from one
// would stop at the first NUL and hand the dialog a truncated filter.
const wchar_t kClpwFilter[] = L"ClipWiz Backup (*.clpw)\0*.clpw\0";
// Save-single-item filters, same double-NUL form as kClpwFilter above.
const wchar_t kTxtFilter[] = L"Text File (*.txt)\0*.txt\0";
const wchar_t kPngFilter[] = L"PNG Image (*.png)\0*.png\0";
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

// wstring -> UTF-16LE bytes with no NUL terminator, the exact shape store.cpp and
// clipboard.cpp use for Text payloads. The transform entry points build a Text
// item out of a wstring and hand it to Add() / WriteItem().
std::vector<uint8_t> Utf16Bytes(const std::wstring& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return std::vector<uint8_t>(p, p + s.size() * sizeof(wchar_t));
}

// "YYYY-MM-DD HH:MM:SS" in local time for the AppendDateTime transform. Formatted
// here, not in transform.cpp, so that unit never reads the clock and stays a pure,
// testable function — Apply just concatenates whatever string it is handed.
std::wstring CurrentDateTimeStamp() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    return util::Format(L"%04u-%02u-%02u %02u:%02u:%02u",
                        static_cast<unsigned>(st.wYear), static_cast<unsigned>(st.wMonth),
                        static_cast<unsigned>(st.wDay), static_cast<unsigned>(st.wHour),
                        static_cast<unsigned>(st.wMinute), static_cast<unsigned>(st.wSecond));
}

// Reveal a captured file list in Explorer. A FileDrop item stores its paths as
// UTF-16LE, one per line (clipboard.cpp GetFileDrop). Explorer selects within a
// single folder, so this reveals the first path's folder with that file
// selected — the honest choice for a drop that may span several folders. COM is
// already STA-initialized on this thread (main.cpp), which SHParseDisplayName
// and SHOpenFolderAndSelectItems require.
void RevealFirstPath(HWND owner, const Item& item) {
    const std::wstring all(reinterpret_cast<const wchar_t*>(item.data.data()),
                           item.data.size() / sizeof(wchar_t));
    const size_t nl = all.find(L'\n');
    std::wstring first = (nl == std::wstring::npos) ? all : all.substr(0, nl);
    if (!first.empty() && first.back() == L'\r') {
        first.pop_back();
    }
    if (first.empty()) {
        util::ErrorBox(owner, i18n::T("msg.reveal_failed"));
        return;
    }
    PIDLIST_ABSOLUTE full = nullptr;
    if (FAILED(SHParseDisplayName(first.c_str(), nullptr, &full, 0, nullptr)) || full == nullptr) {
        util::ErrorBox(owner, i18n::T("msg.reveal_failed"));
        return;
    }
    // pidlFolder = NULL with one absolute child pidl: Explorer opens the file's
    // parent folder and selects the file. apidl borrows `full`, freed after.
    PCUITEMID_CHILD apidl[1] = {reinterpret_cast<PCUITEMID_CHILD>(full)};
    const HRESULT hr = SHOpenFolderAndSelectItems(nullptr, 1, apidl, 0);
    CoTaskMemFree(full);
    if (FAILED(hr)) {
        util::ErrorBox(owner, i18n::T("msg.reveal_failed"));
    }
}

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
    blockRules_ = blocklist::Parse(cfg_.blockRules);

    // Apply configured log level (default Error if unset/invalid)
    logger::SetMinLevel(logger::ParseLevel(cfg_.logLevel.empty() ? "error"
                                          : util::Narrow(cfg_.logLevel)));

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

    // Set limits before loading so Load's internal Evict uses the user's configured limit,
    // not the default 50. This prevents truncating history on startup.
    store_.SetLimits(cfg_.maxHistory, cfg_.expiryDays);
    // Likewise set the mask config before Load, so Load's FillDerived masks each
    // item's preview/searchText as it is read. No separate refresh needed here.
    store_.SetMaskConfig(cfg_.mask);

    // Load data
    Store::LoadResult lr = store_.Load();
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
            // Best-effort synchronous snapshot so the dat file is consistent
            // even if the user later confirms shutdown. Do NOT stop the
            // writer here — Windows can still cancel shutdown after
            // QUERYENDSESSION returns TRUE and the app keeps running.
            KillTimer(hwnd, kTimerSave);
            KillTimer(hwnd, kTimerWriteCheck);
            saveDirty_ = false;
            store_.ExpireCheck();
            {
                std::vector<uint8_t> buf = store_.Serialize();
                util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
            }
            saveState_ = SaveState::NoSaveNeeded;
            return TRUE;

        case WM_ENDSESSION:
            if (wparam) {
                // System confirmed shutdown/end session. Write synchronously
                // again, and do NOT rely on async writers or WM_TIMER which
                // may never be dispatched during fast shutdown.
                KillTimer(hwnd, kTimerSave);
                KillTimer(hwnd, kTimerWriteCheck);
                saveDirty_ = false;
                store_.ExpireCheck();
                {
                    std::vector<uint8_t> buf = store_.Serialize();
                    util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
                }
                saveState_ = SaveState::NoSaveNeeded;
                writer_.Stop();
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
            
            // Clean non-pinned items if configured
            if (cfg_.cleanOnExit) {
                LOG_INFO("Cleaning non-pinned items on exit");
                store_.ClearNonPinned();
            }
            
            // Save final state (important for cleanOnExit)
            LOG_INFO("Saving final state");
            store_.ExpireCheck();
            {
                std::vector<uint8_t> buf = store_.Serialize();
                bool saved = util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
                if (saved) {
                    LOG_INFO("Final state saved successfully");
                } else {
                    LOG_ERROR("Failed to save final state");
                }
            }
            
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
    std::wstring sourceApp;
    if (!clip::Capture(kind, data, imgW, imgH, sourceApp, blockRules_, cfg_.maxTextBytes,
                       cfg_.maxImagePixels)) {
        return;
    }
    store_.Add(kind, std::move(data), imgW, imgH, sourceApp);
    popup::OnDataChanged();
    ScheduleSave();
    CheckStoreSize();
}

void App::OnHotkey(int id) {
    // A FullDisable app ignores clipwiz hotkeys entirely: neither the popup nor
    // a positional paste should fire while such a window is in front. Guarded by
    // the empty check so the common no-blocklist case pays no window lookup.
    if (!blockRules_.empty() &&
        blocklist::Classify(blockRules_, clip::ResolveTarget(GetForegroundWindow())) ==
            blocklist::Action::FullDisable) {
        return;
    }
    if (id == hotkey::kIdPopup) {
        paste::CaptureCurrentForeground();
        popup::Toggle();
        return;
    }
    if (id == hotkey::kIdPasteQueue) {
        // Sequential paste: pop the front of the queue and paste it. An empty
        // queue is a silent no-op — never an error box. Ids whose item has since
        // been deleted are skipped so one stale entry doesn't swallow a press.
        if (queue_.empty()) {
            return;
        }
        paste::CaptureCurrentForeground();
        while (!queue_.empty()) {
            const uint64_t front = queue_.front();
            queue_.erase(queue_.begin());
            if (store_.Find(front) != nullptr) {
                PasteItem(front);  // re-Finds, writes clipboard, pastes, refreshes popup
                return;
            }
        }
        popup::OnDataChanged();  // held only stale ids; clear any badges
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
    if (cmd == tray::CmdExport) {
        ExportBackup();
        return;
    }
    if (cmd == tray::CmdImport) {
        ImportBackup();
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
        if (saveDirty_) {
            saveDirty_ = false;
            // New changes arrived while the previous write was in flight;
            // immediately schedule another full save to catch them up.
            saveState_ = SaveState::NoSaveNeeded;
            ScheduleSave();
        } else {
            saveState_ = SaveState::NoSaveNeeded;
        }
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
    paste::Execute(cfg_.pasteDelayMs, cfg_.pasteKey);
}

void App::CopyItem(uint64_t id) {
    const Item* item = store_.Find(id);
    if (!item) {
        return;
    }
    if (!clip::WriteItem(hwnd_, *item)) {
        return;
    }
    store_.Touch(id);
    popup::OnDataChanged();
    ScheduleSave();
}

void App::TogglePin(uint64_t id) {
    const Item* item = store_.Find(id);
    if (!item) return;
    store_.SetPinned(id, !item->pinned);
    popup::OnDataChanged();
    ScheduleSave();
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
    ScheduleSave();
    RegisterAllHotkeys(false);
}

void App::MovePinned(uint64_t id, int delta) {
    if (store_.MovePinned(id, delta)) {
        popup::OnDataChanged();
        ScheduleSave();
    }
}

void App::ReorderPinned(uint64_t id, int targetIndex) {
    if (store_.MovePinnedTo(id, targetIndex)) {
        popup::OnDataChanged();
        ScheduleSave();
    }
}

uint64_t App::ConvertToPlainText(uint64_t id) {
    // In-place edit: the same entry becomes plain text, staying at its
    // position. Not a "use", so no reorder / no usedAt change.
    uint64_t survivor = store_.ConvertToPlainText(id);
    if (survivor == 0) {
        return 0;
    }
    popup::OnDataChanged();
    ScheduleSave();
    return survivor;
}

uint64_t App::MergeItems(const std::vector<uint64_t>& ids) {
    // Collect the live items in the order the caller passed them — popup hands
    // them over top-to-bottom as shown, so the merged text reads in list order.
    // An id that vanished since the menu opened is simply skipped.
    std::vector<const Item*> items;
    items.reserve(ids.size());
    for (uint64_t id : ids) {
        if (const Item* it = store_.Find(id)) {
            items.push_back(it);
        }
    }
    if (!merge::CanMerge(items)) {
        return 0;
    }
    const std::wstring sep = merge::SeparatorText(cfg_.mergeSep, cfg_.mergeSepCustom);
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    if (!merge::Merge(items, sep, kind, data)) {
        return 0;
    }
    // A brand-new entry via Add(), never an in-place edit: the sources stay as
    // they were, and Add's canonical dedup means a result that already exists in
    // history surfaces that entry instead of storing a duplicate.
    const uint64_t newId = store_.Add(kind, std::move(data));
    popup::OnDataChanged();
    ScheduleSave();
    return newId;
}

bool App::TransformItemText(uint64_t id, transform::Kind kind, std::wstring& out) {
    const Item* item = store_.Find(id);
    if (!item) {
        return false;
    }
    // Transforms read prose. An image's canonical body is raw PNG bytes and a file
    // list is paths, neither of which a case/whitespace rule can meaningfully
    // touch, so both are refused and the caller stores or pastes nothing. Text,
    // Html and Rtf flatten to their plain body (rich text loses its formatting,
    // which is the whole point of transforming it).
    if (item->kind == ItemKind::Image || item->kind == ItemKind::FileDrop) {
        return false;
    }
    const std::vector<uint8_t> body = textconv::CanonicalBody(item->kind, item->data);
    const std::wstring text(reinterpret_cast<const wchar_t*>(body.data()),
                            body.size() / sizeof(wchar_t));
    transform::Options opts;
    opts.slugSep = cfg_.slugSep;
    opts.dateTime = L" " + CurrentDateTimeStamp();  // leading space: "note 2026-09-14 15:30:00"
    out = transform::Apply(kind, text, opts);
    return true;
}

uint64_t App::CopyTransformed(uint64_t id, transform::Kind kind) {
    std::wstring text;
    if (!TransformItemText(id, kind, text)) {
        return 0;
    }
    // A brand-new entry via Add(), never an in-place edit: the source stays as it
    // was, and Add()'s canonical dedup surfaces an existing twin instead of storing
    // a duplicate — the same contract MergeItems relies on.
    const uint64_t newId = store_.Add(ItemKind::Text, Utf16Bytes(text));
    popup::OnDataChanged();
    ScheduleSave();
    return newId;
}

void App::PasteTransformed(uint64_t id, transform::Kind kind) {
    std::wstring text;
    if (!TransformItemText(id, kind, text)) {
        return;
    }
    Item out;
    out.kind = ItemKind::Text;
    out.data = Utf16Bytes(text);
    // One-shot: the transformed text goes to the clipboard and out to the target
    // window, and history is never modified. Touch() still counts this as a use of
    // the source entry, so it moves to the front exactly like a normal paste.
    popup::Hide();
    if (!clip::WriteItem(hwnd_, out)) {
        return;
    }
    store_.Touch(id);
    popup::OnDataChanged();
    ScheduleSave();
    paste::Execute(cfg_.pasteDelayMs, cfg_.pasteKey);
}

void App::PasteAsPlainText(uint64_t id) {
    const Item* item = store_.Find(id);
    if (!item || item->kind == ItemKind::Image) {
        return;
    }
    // The paste-time cousin of ConvertToPlainText. CanonicalBody already returns
    // the extracted plain text as UTF-16 bytes for Html/Rtf (and the text itself
    // for Text/FileDrop), so the flattened body drops straight into a Text item
    // with no re-encode — and the stored entry is left exactly as it was.
    std::vector<uint8_t> body = textconv::CanonicalBody(item->kind, item->data);
    if (body.empty()) {
        return;
    }
    Item plain;
    plain.kind = ItemKind::Text;
    plain.data = std::move(body);
    popup::Hide();
    if (!clip::WriteItem(hwnd_, plain)) {
        return;
    }
    store_.Touch(id);
    popup::OnDataChanged();
    ScheduleSave();
    paste::Execute(cfg_.pasteDelayMs, cfg_.pasteKey);
}

void App::AddToQueue(const std::vector<uint64_t>& ids) {
    // Append in the given order, skipping ids already queued so re-adding a
    // selection doesn't paste the same item twice. Stale ids are not filtered
    // here — the pop loop in OnHotkey skips them, and an id can go stale at any
    // time after queuing anyway.
    for (uint64_t id : ids) {
        if (std::find(queue_.begin(), queue_.end(), id) == queue_.end()) {
            queue_.push_back(id);
        }
    }
}

int App::QueuePosition(uint64_t id) const {
    for (size_t i = 0; i < queue_.size(); ++i) {
        if (queue_[i] == id) {
            return static_cast<int>(i) + 1;  // 1-based for the badge
        }
    }
    return 0;
}

void App::SaveLastPos(int x, int y) {
    cfg_.lastPopupX = x;
    cfg_.lastPopupY = y;
    settings::Save(cfg_);
}

void App::DumpConfigForCrash(int& maxHistory, int& expiryDays, int& popupHotkey,
                             int& rowsVisible, int& popupPosition, int& fontSize,
                             std::string& logLevelUtf8,
                             int& storeCount, int& storePinned, int& storeHistory,
                             uint64_t& storeTotalBytes) const {
    maxHistory = cfg_.maxHistory;
    expiryDays = cfg_.expiryDays;
    popupHotkey = static_cast<int>(cfg_.popupHotkey);
    rowsVisible = cfg_.rowsVisible;
    popupPosition = cfg_.popupPosition;
    fontSize = cfg_.fontSize;
    logLevelUtf8 = util::Narrow(cfg_.logLevel);
    storeCount = static_cast<int>(store_.Items().size());
    storePinned = store_.PinnedCount();
    storeHistory = store_.TotalCount() - store_.PinnedCount();
    storeTotalBytes = store_.TotalDataSize();
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
    blockRules_ = blocklist::Parse(cfg_.blockRules);
    store_.SetLimits(cfg_.maxHistory, cfg_.expiryDays);
    i18n::Init(cfg_.language);
    // Mask config may have changed; update it before the refresh below so the
    // single recompute applies both the new locale and the new masking.
    store_.SetMaskConfig(cfg_.mask);
    // Language may have changed: cached item previews (image/file localized
    // strings) must be rebuilt under the new locale.
    store_.RefreshPreviews();
    logger::SetMinLevel(logger::ParseLevel(cfg_.logLevel.empty() ? "error"
                                          : util::Narrow(cfg_.logLevel)));
    popup::OnThemeChanged();
    popup::OnDataChanged();
    popup::OnSettingsChanged();
}

void App::RegisterAllHotkeys(bool reportFailures) {
    hotkeys_.UnregisterAll();
    hotkeyFailures_.clear();

    if (cfg_.popupHotkey != 0) {
        if (!hotkeys_.Register(hotkey::kIdPopup, cfg_.popupHotkey)) {
            hotkeyFailures_ += hotkey::ToText(cfg_.popupHotkey) + L"\n";
        }
    }
    if (cfg_.queueHotkey != 0) {
        if (!hotkeys_.Register(hotkey::kIdPasteQueue, cfg_.queueHotkey)) {
            hotkeyFailures_ += hotkey::ToText(cfg_.queueHotkey) + L"\n";
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
    } else if (saveState_ == SaveState::SavingInProgress) {
        // Async write is ongoing; mark dirty so OnTimerWriteCheck triggers a follow-up save
        // once the current write finishes, ensuring no changes between submissions are lost.
        saveDirty_ = true;
    }
    // PendingSave: timer already set, final serialization will use the latest in-memory state
}

void App::SaveNow() {
    // Cancel timer
    if (saveState_ == SaveState::PendingSave) {
        KillTimer(hwnd_, kTimerSave);
    }
    
    // Clear dirty flag (we are about to serialize the current latest state)
    saveDirty_ = false;
    
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
                    std::wstring aboutStr = util::Format(i18n::T("about.text"), L"1.3.0");
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

void App::ExportBackup() {
    // One-shot, user-initiated and modal: the user just picked a path and is
    // waiting, so the write happens here rather than through AsyncWriter (which
    // exists to keep the frequent store.dat saves off the UI thread, not to
    // serialize arbitrary export targets). Serialize() is in-memory and fast.
    wchar_t file[MAX_PATH] = {};
    const std::wstring suggested = L"clipwiz-backup-" + util::TimeStampForFileName() + L".clpw";
    wcsncpy_s(file, suggested.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = kClpwFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"clpw";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) {
        return;  // cancelled
    }

    const std::vector<uint8_t> bytes = store_.Serialize();
    if (!util::WriteFileAtomic(file, bytes.data(), bytes.size())) {
        util::ErrorBox(hwnd_, i18n::T("msg.export_failed"));
        return;
    }
    util::InfoBox(hwnd_, util::Format(i18n::T("msg.export_done"), file));
    LOG_INFO("Exported backup: %d item(s)", store_.TotalCount());
}

void App::ImportBackup() {
    wchar_t file[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = kClpwFilter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"clpw";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&ofn)) {
        return;  // cancelled
    }

    std::vector<uint8_t> bytes;
    if (!util::ReadWholeFile(file, bytes)) {
        util::ErrorBox(hwnd_, i18n::T("msg.import_failed"));
        return;
    }
    // ImportMerge parses with the same v3 reader Load() uses and routes every
    // entry through Add(), so a damaged or wrong-version backup returns -1 and
    // leaves the live store byte-for-byte as it was — an import is never
    // destructive, matching the PreserveCorrupt attitude for our own store.dat.
    const int merged = store_.ImportMerge(bytes);
    if (merged < 0) {
        util::ErrorBox(hwnd_, i18n::T("msg.import_failed"));
        return;
    }
    popup::OnDataChanged();
    SaveNow();  // async, like ClearHistory
    util::InfoBox(hwnd_, util::Format(i18n::T("msg.import_done"), merged));
    LOG_INFO("Imported backup: %d item(s) merged", merged);
}

void App::SaveItemAs(uint64_t id) {
    const Item* item = store_.Find(id);
    if (item == nullptr) {
        return;
    }
    // Dismiss the popup before any dialog or Explorer window opens, so focus
    // moves cleanly to it — the same hide-then-act shape PasteTransformed uses.
    popup::Hide();

    if (item->kind == ItemKind::FileDrop) {
        // A file list points at files that already exist on disk: there is
        // nothing to write, so reveal them instead of offering a save dialog.
        RevealFirstPath(hwnd_, *item);
        return;
    }

    std::vector<uint8_t> bytes;
    const wchar_t* filter = nullptr;
    const wchar_t* defExt = nullptr;
    const wchar_t* ext = nullptr;
    if (item->kind == ItemKind::Image) {
        // Image payloads are stored as PNG on disk (imagecodec), so the bytes go
        // out verbatim — no re-encode, no WIC round trip.
        bytes = item->data;
        filter = kPngFilter;
        defExt = L"png";
        ext = L".png";
    } else {
        // Text/Html/Rtf all save their extracted plain text as UTF-8. Html and Rtf
        // lose their formatting here by design — keeping every original byte is
        // what the whole-history .clpw backup is for.
        const std::string utf8 = util::Narrow(Store::TextOf(*item));
        bytes.assign(utf8.begin(), utf8.end());
        filter = kTxtFilter;
        defExt = L"txt";
        ext = L".txt";
    }

    wchar_t file[MAX_PATH] = {};
    const std::wstring suggested = L"clipwiz-" + util::TimeStampForFileName() + ext;
    wcsncpy_s(file, suggested.c_str(), _TRUNCATE);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd_;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&ofn)) {
        return;  // cancelled
    }

    // Synchronous, like ExportBackup: a one-shot modal action the user waits on,
    // not the frequent automatic store.dat saves AsyncWriter exists for.
    if (!util::WriteFileAtomic(file, bytes.data(), bytes.size())) {
        util::ErrorBox(hwnd_, i18n::T("msg.save_as_failed"));
        return;
    }
    util::InfoBox(hwnd_, util::Format(i18n::T("msg.save_done"), file));
    LOG_INFO("Saved item to %s (%zu byte(s))", util::Narrow(file).c_str(), bytes.size());
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
