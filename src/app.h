// app.h — Application core: hidden main window + message dispatch + module glue
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
    const std::wstring& PopupFontName() const override { return cfg_.fontName; }
    int PopupFontSize() const override { return cfg_.fontSize; }
    void GetLastPos(int& x, int& y) const override { x = cfg_.lastPopupX; y = cfg_.lastPopupY; }
    void SaveLastPos(int x, int y) override;
    void PasteItem(uint64_t id) override;
    void CopyItem(uint64_t id) override;
    void TogglePin(uint64_t id) override;
    void DeleteItem(uint64_t id) override;
    void MovePinned(uint64_t id, int delta) override;
    void OpenSettings() override;

    // Snapshot read-only state for crash diagnostics.
    void DumpConfigForCrash(int& maxHistory, int& expiryDays, int& popupHotkey,
                            int& rowsVisible, int& popupPosition, int& fontSize,
                            std::string& logLevelUtf8,
                            int& storeCount, int& storePinned, int& storeHistory,
                            uint64_t& storeTotalBytes) const;

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

    // Save state machine
    enum class SaveState {
        NoSaveNeeded,       // No save needed (data is up to date)
        PendingSave,        // New data pending save, write not yet started
        SavingInProgress    // Async save in progress
    };
    SaveState saveState_ = SaveState::NoSaveNeeded;
    bool saveDirty_ = false;

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr;
    Store store_;
    settings::Config cfg_;
    hotkey::Manager hotkeys_;
    util::Theme theme_ = {};
    AsyncWriter writer_;
    bool settingsOpen_ = false;
    bool sizeWarned_ = false;
    std::wstring hotkeyFailures_;
};
