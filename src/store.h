// store.h — Clipboard item database
//
// Data durability rules:
//   1. Auto-eviction / expiry only affects unpinned items
//   2. Total item limit (maxTotal_) counts ALL items including pinned
//   3. On store.dat validation failure, the file is renamed (never overwritten or cleared)
//
// Design notes:
//   - All content (text/image/HTML/RTF/file list) stored uniformly as binary blob
//   - Hotkeys are not tied to items; managed positionally via config
//   - Pinned section maintains manual order, not reordered by usage
//   - Item::pinned is the sole authority on section membership
//   - order values: pinned [1..9999], unpinned [10001..]; maintained by Normalize
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

enum class ItemKind : uint32_t {
    Text = 0,      // CF_UNICODETEXT plain text (data = UTF-16LE bytes)
    Image = 1,     // Image (data = PNG bytes)
    Html = 2,      // "HTML Format" (data = raw clipboard bytes including Version: header)
    Rtf = 3,       // "Rich Text Format" (data = raw RTF bytes)
    FileDrop = 4,  // CF_HDROP (data = UTF-16LE text, one path per line)
};

struct Item {
    uint64_t id = 0;
    ItemKind kind = ItemKind::Text;
    bool pinned = false;
    uint32_t order = 0;         // Single global sort key:
                                //   pinned items live in [1, 9999]
                                //   unpinned items live in [10001, ...]
                                //   Maintained contiguous by Normalize after every mutation.
                                //   Zero = unassigned (filled in during initial load from
                                //   legacy store files or at Add() time).
    uint64_t createdAt = 0;
    uint64_t usedAt = 0;
    std::vector<uint8_t> data;  // Unified binary content
    uint32_t imgW = 0;          // Only valid for Image kind
    uint32_t imgH = 0;
    std::wstring preview;       // For list display, recomputed at runtime, not persisted
    uint64_t hash = 0;          // For dedup, computed at runtime, not persisted
};

class Store {
public:
    enum class LoadResult {
        Ok,       // Loaded normally (includes empty database)
        Corrupt,  // File corrupted, renamed for preservation, running with empty database
    };

    LoadResult Load();
    bool Save();

    const std::vector<Item>& Items() const { return items_; }
    const Item* Find(uint64_t id) const;

    // Add item, returns id; on dedup hit, refreshes usedAt and returns existing id
    uint64_t Add(ItemKind kind, std::vector<uint8_t> data, uint32_t imgW = 0, uint32_t imgH = 0);

    bool SetPinned(uint64_t id, bool pinned);
    bool Remove(uint64_t id);
    bool Touch(uint64_t id);  // Update usedAt and reorder unpinned section

    // Convert an existing RTF/HTML item into plain text IN PLACE: same id,
    // same position, same pinned/order/timestamps. Only kind/data/preview/hash
    // change. This is a content edit, NOT a "use", so it does not reorder.
    // If the resulting plain text duplicates an existing text entry, the two
    // are merged (keep the earlier one; keep the pinned one across groups;
    // never delete when both are pinned).
    // Returns the id of the surviving entry the caller should select, or 0 if
    // nothing changed (id not found / not rich text).
    uint64_t ConvertToPlainText(uint64_t id);

    // "Selective front" (选择性前置操作): the single shared rule for what
    // "becoming the newest item" means. Pinned items keep their position
    // (they are nailed down); unpinned items move to the very top of the
    // unpinned group, everything else shifts down by one. Newer always sits
    // above older within the unpinned group. Safe to call for any id.
    void PromoteToFront(uint64_t id);
    
    // Clear all non-pinned items (for clean on exit)
    void ClearNonPinned();

    // Recompute every item's cached preview string. Call after a language
    // change: previews for image/file items contain localized words and are
    // cached at Add/Load time, so they must be rebuilt when the locale switches.
    void RefreshPreviews();

    // Pinned item reordering
    bool MovePinned(uint64_t id, int delta);         // Move up/down by delta positions
    bool MovePinnedTo(uint64_t id, int targetIndex); // Drag to target position

    // Query helpers used by popup (both operate on ordered pinned section,
    // consistent with what the user actually sees on screen)
    int PinnedIndexOf(uint64_t id) const;

    void SetLimits(int maxTotal, int expiryDays);
    void ExpireCheck();  // Remove expired unpinned items

    // Serialize to memory (called from main thread, very fast), then hand to AsyncWriter
    std::vector<uint8_t> Serialize();

    // Total byte count of all items' data fields
    uint64_t TotalDataSize() const;

    int PinnedCount() const;
    int TotalCount() const;  // All items (pinned + unpinned)
    const std::wstring& CorruptBackupPath() const { return corruptBackup_; }

    // Utility: extract text from item data (Text/Html/Rtf/FileDrop)
    static std::wstring TextOf(const Item& item);

private:
    Item* FindMutable(uint64_t id);
    void Evict();
    bool EvictOneOldestUnpinned();
    LoadResult PreserveCorrupt();

    std::vector<Item> items_;  // Pinned section first (order asc), then unpinned (order asc)
    uint64_t nextId_ = 1;
    int maxTotal_ = 50;    // Total item limit (pinned + unpinned), hard cap 9999
    int expiryDays_ = 0;
    std::wstring corruptBackup_;
};

// Generate a one-line preview summary for an item
std::wstring MakeItemPreview(const Item& item);
