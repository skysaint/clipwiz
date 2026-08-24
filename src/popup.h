// popup.h — Quick paste popup window
//
// Owner-drawn list, draggable title bar, Ctrl+hover preview, pinned item drag reorder.
#pragma once

#include <windows.h>

#include <cstdint>

#include "store.h"
#include "util.h"

namespace popup {

class Host {
public:
    virtual Store& GetStore() = 0;
    virtual const util::Theme& GetTheme() const = 0;
    virtual int RowsVisible() const = 0;
    virtual int PopupPosition() const = 0;  // 0=mouse 1=caret 2=last
    virtual const std::wstring& PopupFontName() const = 0;
    virtual int PopupFontSize() const = 0;
    virtual void GetLastPos(int& x, int& y) const = 0;
    virtual void SaveLastPos(int x, int y) = 0;

    virtual void PasteItem(uint64_t id) = 0;
    virtual void CopyItem(uint64_t id) = 0;
    virtual void TogglePin(uint64_t id) = 0;
    virtual void DeleteItem(uint64_t id) = 0;
    virtual void MovePinned(uint64_t id, int delta) = 0;
    virtual void ReorderPinned(uint64_t id, int targetIndex) = 0;
    virtual void OpenSettings() = 0;

protected:
    ~Host() = default;
};

bool Init(HINSTANCE inst, Host* host);
void Shutdown();

void Show();
void Hide();
void Toggle();
bool IsVisible();
HWND Window();

void BeginModal();
void EndModal();

void OnThemeChanged();
void OnDataChanged();
void OnSettingsChanged();

struct RuntimeState {
    HWND hwnd = nullptr;
    int visible = 0;   // 0/1 safe int read
    int modalDepth = 0;
    int reorderDrag = 0;
    int rowsCount = 0;
    int widthDip = 0;
    int heightDip = 0;
    int dpi = 0;
    int lastSelRow = -1;
    int pinnedCount = -1;
};

// Best-effort, side-effect-free snapshot of internal state for crash diagnostics.
void SnapshotState(RuntimeState& out);

}  // namespace popup
