// app.cpp
#include "app.h"

#include <algorithm>

#include "clipboard.h"
#include "i18n.h"
#include "imagecodec.h"
#include "paste.h"
#include "resource.h"
#include "tray.h"

namespace {

const wchar_t kMainClass[] = L"ClipWizMain";
constexpr UINT kMsgTray = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kTimerSave = 1;
constexpr UINT kTimerWriteCheck = 2;
constexpr int kSaveDelayMs = 800;
constexpr int kWriteCheckMs = 200;

// 总量超过这个值就提示用户清理（100 MB）
constexpr uint64_t kSizeWarnBytes = 100ULL * 1024 * 1024;

}  // namespace

UINT SingleInstanceMessage() {
    static UINT msg = RegisterWindowMessageW(L"ClipWiz.ShowPopup.7A1C");
    return msg;
}

// ------------------------------------------------------------------ 初始化

bool App::Init(HINSTANCE inst) {
    inst_ = inst;

    // 加载配置
    settings::Load(cfg_);
    settings::Clamp(cfg_);

    // 数据目录
    if (!cfg_.dataDir.empty()) {
        util::SetDataDir(cfg_.dataDir);
    }

    // i18n
    i18n::Init(cfg_.language);

    // WIC
    imagecodec::Init();

    // 主题
    ApplyTheme();

    // 注册窗口类
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = inst;
    wc.lpszClassName = kMainClass;
    if (!RegisterClassExW(&wc)) {
        return false;
    }

    // 隐藏主窗口（消息载体）
    hwnd_ = CreateWindowExW(0, kMainClass, L"ClipWiz", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                            nullptr, inst, this);
    if (!hwnd_) {
        return false;
    }

    // 加载数据
    Store::LoadResult lr = store_.Load();
    store_.SetLimits(cfg_.maxHistory, cfg_.expiryDays);
    if (lr == Store::LoadResult::Corrupt) {
        util::ErrorBox(nullptr, i18n::T("msg.corrupt_found"));
    }

    // 剪贴板监听
    clip::StartListening(hwnd_);

    // 前台窗口跟踪
    paste::InstallHook();

    // 托盘
    tray::Add(hwnd_, kMsgTray, IDI_APPICON);
    tray::SetTip(L"ClipWiz");

    // 快捷键
    hotkeys_.Attach(hwnd_);
    RegisterAllHotkeys(true);

    // 异步写盘线程
    writer_.Start();

    // 弹出框
    popup::Init(inst, this);

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
            OnClipboardUpdate();
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

        case WM_TIMER:
            if (wparam == kTimerSave) {
                OnTimerSave();
            } else if (wparam == kTimerWriteCheck) {
                OnTimerWriteCheck();
            }
            return 0;

        case WM_SETTINGCHANGE: {
            // 深色模式切换
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
            SaveNow();
            writer_.Stop();
            clip::StopListening(hwnd);
            paste::RemoveHook();
            hotkeys_.UnregisterAll();
            tray::Remove();
            popup::Shutdown();
            imagecodec::Shutdown();
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// ------------------------------------------------------------------ 事件处理

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
    // 位置式：id = kIdItemBase + 位置索引
    int index = id - hotkey::kIdItemBase;
    if (index < 0 || index >= 10) {
        return;
    }
    // 找第 index 个置顶项
    int seen = 0;
    for (const Item& item : store_.Items()) {
        if (!item.pinned) {
            break;  // 置顶区在前面
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
            // 单击弹出
            paste::CaptureCurrentForeground();
            popup::Toggle();
            break;
        case WM_RBUTTONUP: {
            // 右键菜单
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
            UINT cmd = tray::ShowMenu(hwnd_, pinned, util::GetAutostart());
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
        SaveNow();
        DestroyWindow(hwnd_);
        return;
    }
    // 置顶项菜单直贴
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
    savePending_ = false;
    SaveNow();
}

void App::OnTimerWriteCheck() {
    if (writer_.Done()) {
        KillTimer(hwnd_, kTimerWriteCheck);
    }
}

// ------------------------------------------------------------------ Host 实现

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
    store_.SetPinned(id, !store_.Find(id)->pinned);
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
        bool ok = util::ConfirmBox(overPopup ? popup::Window() : hwnd_,
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

// ------------------------------------------------------------------ 内部

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
    if (!cfg_.dataDir.empty()) {
        util::SetDataDir(cfg_.dataDir);
    }
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
        util::ErrorBox(hwnd_, msg);
    }
}

void App::ScheduleSave() {
    if (!savePending_) {
        savePending_ = true;
        SetTimer(hwnd_, kTimerSave, kSaveDelayMs, nullptr);
    }
}

void App::SaveNow() {
    if (savePending_) {
        KillTimer(hwnd_, kTimerSave);
        savePending_ = false;
    }
    store_.ExpireCheck();
    std::vector<uint8_t> buf = store_.Serialize();
    writer_.Submit(util::StorePath(), std::move(buf));
    // 定时检查写盘完成
    SetTimer(hwnd_, kTimerWriteCheck, kWriteCheckMs, nullptr);
}

void App::ShowAbout() {
    std::wstring text = util::Format(i18n::T("about.text"), L"1.1.0");
    util::InfoBox(hwnd_, text);
}

void App::ClearHistory() {
    if (!util::ConfirmBox(hwnd_, i18n::T("msg.confirm_clear"))) {
        return;
    }
    // 只删非置顶
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
    // 提示用户清理
    if (!util::ConfirmBox(hwnd_, i18n::T("msg.large_data"))) {
        return;
    }
    // 清理超过阈值的非置顶条目
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
