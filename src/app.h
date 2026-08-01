// app.h — 程序主体：隐藏主窗口 + 消息分发 + 各模块的粘合
#pragma once

#include <windows.h>

#include <string>

#include "asyncwriter.h"
#include "hotkey.h"
#include "popup.h"
#include "settings.h"
#include "store.h"
#include "util.h"

UINT SingleInstanceMessage();

class App final : public popup::Host {
public:
    bool Init(HINSTANCE inst);
    int Run();

    // popup::Host
    Store& GetStore() override { return store_; }
    const util::Theme& GetTheme() const override { return theme_; }
    int RowsVisible() const override { return cfg_.rowsVisible; }
    int PopupPosition() const override { return cfg_.popupPosition; }
    void GetLastPos(int& x, int& y) const override { x = cfg_.lastPopupX; y = cfg_.lastPopupY; }
    void SaveLastPos(int x, int y) override;
    void PasteItem(uint64_t id) override;
    void CopyItem(uint64_t id) override;
    void TogglePin(uint64_t id) override;
    void DeleteItem(uint64_t id) override;
    void MovePinned(uint64_t id, int delta) override;
    void OpenSettings() override;

private:
    static LRESULT CALLBACK WndProcThunk(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
    LRESULT WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

    void OnClipboardUpdate();
    void OnHotkey(int id);
    void OnTrayMessage(UINT mouseMsg);
    void OnCommand(UINT cmd);
    void OnTimerSave();
    void OnTimerWriteCheck();

    void ApplyTheme();
    void ApplyConfig();
    void RegisterAllHotkeys(bool reportFailures);
    void ScheduleSave();
    void SaveNow();
    void CheckStoreSize();
    void ShowAbout();
    void ClearHistory();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    Store store_;
    settings::Config cfg_;
    hotkey::Manager hotkeys_;
    util::Theme theme_ = {};
    AsyncWriter writer_;
    bool savePending_ = false;
    bool settingsOpen_ = false;
    bool sizeWarned_ = false;
    std::wstring hotkeyFailures_;
};
