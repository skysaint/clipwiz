// popup.h — Quick paste popup window
//
// Owner-drawn list, draggable title bar, hover preview, pinned item drag reorder.
#pragma once

#include <windows.h>

#include <cstdint>
#include <vector>

#include "store.h"
#include "transform.h"
#include "util.h"

namespace popup {

class Host {
public:
    virtual Store& GetStore() = 0;
    virtual const util::Theme& GetTheme() const = 0;
    virtual int RowsVisible() const = 0;
    virtual int PopupPosition() const = 0;  // 0=mouse 1=caret 2=last
    // Show a row's preview after the mouse rests on it. Off means preview only
    // on Ctrl+hover, for people who find the popup appearing uninvited.
    virtual bool HoverPreview() const = 0;
    // Which sensitive-pattern scanners mask the hover preview. The list rows
    // already show masked text (Store masks preview/searchText), so the hover
    // view must mask too, or hovering would reveal what the row hides.
    virtual const mask::Config& MaskConfig() const = 0;
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
    // Convert the item to plain text in place; returns the id of the entry
    // that should be selected afterwards (the survivor), or 0 if nothing changed.
    virtual uint64_t ConvertToPlainText(uint64_t id) = 0;
    // Merge the given ids into one new entry (see merge.h). Returns the id to
    // select afterwards — the new entry, or an existing one the result deduped
    // into — or 0 if the set cannot be merged.
    virtual uint64_t MergeItems(const std::vector<uint64_t>& ids) = 0;
    // Text transforms (transform.h). CopyTransformed stores the result as a new
    // entry and returns its id (0 when the row has no transformable text); the
    // source is left untouched and Add()'s dedup applies, exactly like a merge.
    // PasteTransformed sends the result to the clipboard and pastes it without
    // changing history. PasteAsPlainText is the flatten-only, no-store-change
    // cousin of ConvertToPlainText: same canonical extraction, one-shot.
    virtual uint64_t CopyTransformed(uint64_t id, transform::Kind kind) = 0;
    virtual void PasteTransformed(uint64_t id, transform::Kind kind) = 0;
    virtual void PasteAsPlainText(uint64_t id) = 0;
    // Sequential paste queue (runtime-only, never persisted). AddToQueue appends
    // the given ids, skipping any already queued; QueuePosition returns an id's
    // 1-based position in the queue, or 0 when it is not queued — DrawRow reads
    // it to paint the per-row badge.
    virtual void AddToQueue(const std::vector<uint64_t>& ids) = 0;
    virtual int QueuePosition(uint64_t id) const = 0;
    // Save one entry out to a real file (App::SaveItemAs). A text-bearing row
    // writes its extracted text as UTF-8 .txt, an image writes its stored PNG
    // verbatim as .png, and a file list — whose files already exist on disk — is
    // revealed in Explorer rather than saved. A no-op if the id is gone.
    virtual void SaveItemAs(uint64_t id) = 0;
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
