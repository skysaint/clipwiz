// popup.h — 快速粘贴框
//
// 列表自绘，标题栏可拖动，支持 Ctrl+悬停预览、置顶拖拽排序。
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
    virtual int PopupPosition() const = 0;  // 0=鼠标 1=光标 2=上次
    virtual void GetLastPos(int& x, int& y) const = 0;
    virtual void SaveLastPos(int x, int y) = 0;

    virtual void PasteItem(uint64_t id) = 0;
    virtual void CopyItem(uint64_t id) = 0;
    virtual void TogglePin(uint64_t id) = 0;
    virtual void DeleteItem(uint64_t id) = 0;
    virtual void MovePinned(uint64_t id, int delta) = 0;
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

}  // namespace popup
