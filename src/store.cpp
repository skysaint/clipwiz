// store.cpp
#include "store.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

#include "i18n.h"
#include "log.h"
#include "mask.h"
#include "textconv.h"
#include "util.h"

namespace {

constexpr uint32_t kStoreVersion = 3;   // v3: added per-item `sourceApp` string
constexpr size_t kHeaderSize = 32;
const char kMagic[4] = {'C', 'L', 'P', 'W'};

constexpr uint32_t kFlagPinned = 0x1;

// Order value space: pinned [1..9999], unpinned [10001..10001+N-1].
// Total items capped at 9999, so ranges never overlap.
constexpr uint32_t kUnpinnedOrderBase = 10001u;

// Hard cap per item data to prevent corrupted files from exhausting memory (64 MB)
constexpr uint32_t kMaxDataLen = 64u * 1024u * 1024u;
constexpr uint32_t kMaxItemCount = 9999u;

// Process image names are short. This only stops a damaged length prefix from
// demanding an arbitrary allocation before the file is rejected.
constexpr uint32_t kMaxSourceAppChars = 1024u;

// FILETIME ticks per day
constexpr uint64_t kTicksPerDay = 24ULL * 60 * 60 * 10000000;

template <typename T>
void Append(std::vector<uint8_t>& buf, T value) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    buf.insert(buf.end(), p, p + sizeof(T));
}

template <typename T>
bool Take(const std::vector<uint8_t>& buf, size_t& pos, T& value) {
    if (pos + sizeof(T) > buf.size()) {
        return false;
    }
    memcpy(&value, buf.data() + pos, sizeof(T));
    pos += sizeof(T);
    return true;
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// Upper bound on how much text MakeItemSearchText extracts from rich content.
// Plain text and file lists are bounded by their own payload size and are not
// capped; this only keeps one pathological multi-megabyte RTF from turning a
// single Add() or Load() into a full-document parse.
//
// It is a latency guard, not a memory guard: extracted text is never longer
// than the markup it came from, so searchText for any kind is bounded by the
// data already in memory.
constexpr size_t kSearchTextLimit = 4096;  // wchars

// preview and searchText are both derived from data and neither is persisted.
// They are always recomputed together, so a content change cannot leave one of
// them describing the old bytes.
//
// Both are masked on the way out. preview keeps its original case, so every
// scanner can fire on it, including the password heuristic (which needs an
// uppercase letter). searchText is already lowercased by MakeItemSearchText, so
// only the case-insensitive structural scanners (email/phone/ID/API-key) match
// there and the password heuristic harmlessly skips it. That split is fine: a
// password is still hidden in the preview and the hover view, searchText is
// never shown on screen, and item.data is never masked — so what gets pasted is
// always the full original.
void FillDerived(Item& item, const mask::Config& mc) {
    item.preview = mask::Apply(MakeItemPreview(item), mc);
    item.searchText = mask::Apply(MakeItemSearchText(item), mc);
}

}  // namespace

// ------------------------------------------------------------------ Public utilities

std::wstring Store::TextOf(const Item& item) {
    switch (item.kind) {
        case ItemKind::Text:
        case ItemKind::FileDrop:
            // data is UTF-16LE
            return std::wstring(reinterpret_cast<const wchar_t*>(item.data.data()),
                                item.data.size() / sizeof(wchar_t));
        case ItemKind::Html:
            return textconv::HtmlToPlainText(item.data);
        case ItemKind::Rtf:
            return textconv::RtfToPlainText(item.data);
        case ItemKind::Image:
            return std::wstring();
        default:
            return std::wstring();
    }
}

std::wstring MakeItemPreview(const Item& item) {
    switch (item.kind) {
        case ItemKind::Image:
            return util::Format(i18n::T("preview.image"), item.imgW, item.imgH);
        case ItemKind::FileDrop: {
            std::wstring paths = Store::TextOf(item);
            // Count lines
            int count = 0;
            std::wstring first;
            size_t pos = 0;
            while (pos < paths.size()) {
                size_t eol = paths.find(L'\n', pos);
                std::wstring line = paths.substr(pos, eol == std::wstring::npos ? std::wstring::npos
                                                                                : eol - pos);
                while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    if (count == 0) {
                        first = line;
                    }
                    ++count;
                }
                pos = (eol == std::wstring::npos) ? paths.size() : eol + 1;
            }
            // Extract filename only
            size_t slash = first.find_last_of(L"\\/");
            std::wstring name = (slash != std::wstring::npos) ? first.substr(slash + 1) : first;
            if (count <= 1) {
                return name;
            }
            return util::Format(i18n::T("preview.files"), count, name.c_str());
        }
        default: {
            // For preview, extract only enough text (limit extraction to save time
            // on large RTF/HTML items — we only display ~160 chars anyway).
            std::wstring text;
            constexpr size_t kPreviewLimit = 500;  // wchars, plenty for OneLinePreview(160)
            switch (item.kind) {
                case ItemKind::Html:
                    text = textconv::HtmlToPlainText(item.data, kPreviewLimit * 3);  // UTF-8 bytes
                    break;
                case ItemKind::Rtf:
                    text = textconv::RtfToPlainText(item.data, kPreviewLimit);
                    break;
                default:
                    text = Store::TextOf(item);
                    break;
            }
            std::wstring preview = util::OneLinePreview(text, 160);
            return preview.empty() ? std::wstring(i18n::T("preview.empty")) : preview;
        }
    }
}

std::wstring MakeItemSearchText(const Item& item) {
    std::wstring text;
    switch (item.kind) {
        case ItemKind::Image:
            // An image has no text of its own; the localized "[Image W×H]"
            // summary is the only thing a user could type to find it.
            text = MakeItemPreview(item);
            break;
        case ItemKind::Html: {
            // HtmlToPlainText's budget is counted in UTF-8 bytes, so ASCII
            // content can yield up to three times as many characters as the
            // same budget buys in Rtf. Trim here so both rich kinds land on
            // exactly kSearchTextLimit characters.
            text = textconv::HtmlToPlainText(item.data, kSearchTextLimit * 3);
            if (text.size() > kSearchTextLimit) {
                text.resize(kSearchTextLimit);
            }
            break;
        }
        case ItemKind::Rtf:
            text = textconv::RtfToPlainText(item.data, kSearchTextLimit);
            break;
        default:
            // Text: the whole payload, not the 160-char one-line preview.
            // FileDrop: every path, not just the first file name the preview
            // shows — searching for the third file in a selection must work.
            text = Store::TextOf(item);
            break;
    }
    for (wchar_t& c : text) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return text;
}

// ------------------------------------------------------------------ Store

const Item* Store::Find(uint64_t id) const {
    for (const Item& item : items_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

Item* Store::FindMutable(uint64_t id) {
    for (Item& item : items_) {
        if (item.id == id) {
            return &item;
        }
    }
    return nullptr;
}

int Store::PinnedCount() const {
    int n = 0;
    for (const Item& item : items_) {
        if (item.pinned) {
            ++n;
        }
    }
    return n;
}

int Store::PinnedIndexOf(uint64_t id) const {
    // items_ is always in display order (pinned first by order asc).
    // Just count pinned items before the one with matching id.
    int idx = 0;
    for (const Item& it : items_) {
        if (!it.pinned) break;  // Past pinned section
        if (it.id == id) return idx;
        ++idx;
    }
    return -1;
}

int Store::TotalCount() const {
    return static_cast<int>(items_.size());
}

void Store::SetLimits(int maxTotal, int expiryDays) {
    maxTotal_ = std::clamp(maxTotal, 5, 9999);
    expiryDays_ = std::max(0, expiryDays);
    ExpireCheck();
    Evict();
}

void Store::SetMaskConfig(const mask::Config& mc) {
    maskCfg_ = mc;
}

// ---- Core sorting helpers ----
// SortByOrder: physical sort — pinned group first, then unpinned, each by order asc.
// Renumber: stamp contiguous order values on current physical order (no re-sort).
// Normalize = SortByOrder + Renumber. Use after any structural mutation.
// MoveToHead: set item's order to sentinel 0 (sorts to segment head), then Normalize.

namespace {
void SortByOrder(std::vector<Item>& items) {
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.pinned != b.pinned) return a.pinned;  // pinned before unpinned
        return a.order < b.order;
    });
}

void Renumber(std::vector<Item>& items) {
    uint32_t pinNext = 1;
    uint32_t unpNext = kUnpinnedOrderBase;
    for (Item& it : items) {
        if (it.pinned) it.order = pinNext++;
        else           it.order = unpNext++;
    }
}

void Normalize(std::vector<Item>& items) {
    SortByOrder(items);
    Renumber(items);
}

void MoveToHead(std::vector<Item>& items, uint64_t id, bool pinned) {
    size_t idx = items.size();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].id == id) { idx = i; break; }
    }
    if (idx == items.size()) return;
    items[idx].pinned = pinned;
    items[idx].order  = 0;  // Sentinel: smallest in segment → Normalize puts it at segment head
    Normalize(items);
}
}  // namespace


bool Store::EvictOneOldestUnpinned() {
    size_t victim = items_.size();
    uint64_t oldestUsed = UINT64_MAX;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].pinned) continue;
        if (items_[i].usedAt <= oldestUsed) {
            oldestUsed = items_[i].usedAt;
            victim = i;
        }
    }
    if (victim >= items_.size()) return false;
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(victim));
    Normalize(items_);
    return true;
}

void Store::Evict() {
    // Total item limit: only evict unpinned items.
    while (TotalCount() > maxTotal_) {
        if (!EvictOneOldestUnpinned()) break;  // All pinned — cannot evict
    }
    if (!items_.empty()) Normalize(items_);
}

void Store::ExpireCheck() {
    if (expiryDays_ <= 0) return;
    const uint64_t cutoff = util::NowFileTime() - static_cast<uint64_t>(expiryDays_) * kTicksPerDay;
    size_t before = items_.size();
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
                       [cutoff](const Item& item) { return !item.pinned && item.usedAt < cutoff; }),
        items_.end());
    if (items_.size() != before && !items_.empty()) Normalize(items_);
}

uint64_t Store::Add(ItemKind kind, std::vector<uint8_t> data, uint32_t imgW, uint32_t imgH,
                    const std::wstring& sourceApp) {
    if (data.empty()) return 0;
    // Dedup by canonical content (plain-text body + type prefix), NOT raw bytes:
    // Word/browsers emit different RTF/HTML bytes for the exact same passage.
    const uint64_t hash = textconv::CanonicalHash(kind, data);
    const bool bodyEmpty = textconv::CanonicalBody(kind, data).empty();
    const uint64_t now = util::NowFileTime();

    // Dedup: same kind + same canonical content. On a hit we KEEP THE NEW
    // content (the user may have re-edited formatting/images before recopying,
    // and expects to paste the latest version), then apply 选择性前置操作.
    //
    // NOTE: items_ is NOT guaranteed to hold at most one entry per canonical
    // hash: ConvertToPlainText's "both pinned" branch can leave two
    // identical-hash entries coexisting. This loop refreshes the FIRST match in
    // display order, which is the intended behavior (the topmost duplicate).
    for (Item& item : items_) {
        if (textconv::SameCanonicalContent(item, kind, data, hash, bodyEmpty)) {
            const uint64_t dupId = item.id;
            // Refresh the stored bytes to the newest copy (same canonical hash).
            item.data = std::move(data);
            item.imgW = imgW;
            item.imgH = imgH;
            // The source app describes the newest copy as well: re-copying the
            // same passage from a different program should say so.
            item.sourceApp = sourceApp;
            FillDerived(item, maskCfg_);
            if (item.pinned) {
                // Pinned: content updated, but position/pin/usedAt unchanged.
                return dupId;
            }
            item.usedAt = now;
            PromoteToFront(dupId);  // 选择性前置操作 for existing unpinned dup
            return dupId;
        }
    }

    // Capacity check: reject if nextId is exhausted
    if (nextId_ == 0 || nextId_ == UINT64_MAX) return 0;

    // Ensure room for one more item (total limit includes pinned)
    if (TotalCount() > maxTotal_) Evict();
    if (TotalCount() >= maxTotal_ && !EvictOneOldestUnpinned()) {
        return 0;  // All pinned and at capacity — reject new content
    }

    Item item;
    item.id = nextId_++;
    item.kind = kind;
    item.createdAt = now;
    item.usedAt = now;
    item.data = std::move(data);
    item.imgW = imgW;
    item.imgH = imgH;
    item.sourceApp = sourceApp;
    item.hash = hash;
    item.pinned = false;
    FillDerived(item, maskCfg_);
    const uint64_t id = item.id;
    items_.push_back(std::move(item));
    PromoteToFront(id);  // 选择性前置操作: newest at top of unpinned section
    return id;
}

bool Store::SetPinned(uint64_t id, bool pinned) {
    Item* item = FindMutable(id);
    if (!item || item->pinned == pinned) return false;
    MoveToHead(items_, id, pinned);  // Moves to head of new section
    Evict();  // In case unpinning freed no space but total was already over limit
    return true;
}

bool Store::Remove(uint64_t id) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) {
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(i));
            if (!items_.empty()) Normalize(items_);
            return true;
        }
    }
    return false;
}

void Store::ClearNonPinned() {
    for (int i = static_cast<int>(items_.size()) - 1; i >= 0; --i) {
        if (!items_[i].pinned) {
            items_.erase(items_.begin() + i);
        }
    }
    if (!items_.empty()) Normalize(items_);
}

void Store::RefreshPreviews() {
    for (Item& item : items_) {
        FillDerived(item, maskCfg_);
    }
}

uint64_t Store::ConvertToPlainText(uint64_t id) {
    size_t idx = items_.size();
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) { idx = i; break; }
    }
    if (idx == items_.size()) {
        LOG_INFO("ConvertToPlainText: id=%llu not found", (unsigned long long)id);
        return 0;
    }
    if (items_[idx].kind != ItemKind::Rtf && items_[idx].kind != ItemKind::Html) {
        LOG_INFO("ConvertToPlainText: id=%llu kind=%u not rich text, skip",
                 (unsigned long long)id, (unsigned)items_[idx].kind);
        return 0;
    }

    std::wstring text = TextOf(items_[idx]);  // RtfToPlainText / HtmlToPlainText
    std::vector<uint8_t> bytes(
        reinterpret_cast<const uint8_t*>(text.data()),
        reinterpret_cast<const uint8_t*>(text.data()) + text.size() * sizeof(wchar_t));
    // Canonical hash of the resulting Text entry (same scheme as Add uses),
    // so it dedups correctly against existing plain-text entries.
    const uint64_t newHash = textconv::CanonicalHash(ItemKind::Text, bytes);

    LOG_INFO("ConvertToPlainText: id=%llu oldKind=%u idx=%zu -> Text, textLen=%zu newHash=%llu",
             (unsigned long long)id, (unsigned)items_[idx].kind, idx, text.size(),
             (unsigned long long)newHash);

    // In-place: keep id, pinned, order, position, createdAt/usedAt untouched.
    // This is a content edit, not a "use", so we never reorder.
    items_[idx].kind = ItemKind::Text;
    items_[idx].data = std::move(bytes);
    items_[idx].hash = newHash;
    FillDerived(items_[idx], maskCfg_);

    // Dedup fallout: converting may make this entry identical to an existing
    // plain-text entry. Merge to restore the "no duplicates" invariant.
    //   - Keep whichever sits EARLIER in display order (smaller index). Since
    //     the pinned group renders before the unpinned group, this naturally
    //     keeps a pinned duplicate over an unpinned one, and works cross-group.
    //   - Do NOT promote/front (a conversion is not a use).
    //   - If BOTH are pinned, don't delete either (pinned = user-only removal).
    size_t dupIdx = items_.size();
    for (size_t i = 0; i < items_.size(); ++i) {
        if (i == idx) continue;
        // Shared predicate: hash match with an empty-body / collision safety belt.
        if (textconv::SameCanonicalContent(items_[i], ItemKind::Text, items_[idx].data, newHash)) {
            dupIdx = i;
            break;
        }
    }

    if (dupIdx == items_.size()) {
        // No duplicate: the entry stays put, select itself.
        LOG_INFO("ConvertToPlainText: no duplicate, survivor id=%llu",
                 (unsigned long long)id);
        return id;
    }
    LOG_INFO("ConvertToPlainText: duplicate at idx=%zu id=%llu (pinnedSelf=%d pinnedDup=%d)",
             dupIdx, (unsigned long long)items_[dupIdx].id,
             (int)items_[idx].pinned, (int)items_[dupIdx].pinned);

    if (items_[idx].pinned && items_[dupIdx].pinned) {
        // Both pinned: tolerate the duplicate, don't auto-remove a pinned item.
        LOG_INFO("ConvertToPlainText: both pinned, kept both; survivor id=%llu",
                 (unsigned long long)id);
        return id;
    }

    // Keep the earlier one (smaller index), drop the later one.
    size_t keepIdx = std::min(idx, dupIdx);
    size_t victimIdx = std::max(idx, dupIdx);
    const uint64_t survivorId = items_[keepIdx].id;
    const uint64_t victimId = items_[victimIdx].id;
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(victimIdx));
    if (!items_.empty()) Normalize(items_);
    LOG_INFO("ConvertToPlainText: merged, kept id=%llu (idx=%zu), removed id=%llu (idx=%zu)",
             (unsigned long long)survivorId, keepIdx, (unsigned long long)victimId, victimIdx);
    return survivorId;
}

void Store::PromoteToFront(uint64_t id) {
    Item* item = FindMutable(id);
    if (!item) return;
    if (item->pinned) {
        // Pinned: nailed down, position unchanged.
        return;
    }
    // Unpinned: move to the head of the unpinned group; others shift down.
    MoveToHead(items_, id, /*pinned=*/false);
}

bool Store::Touch(uint64_t id) {
    Item* item = FindMutable(id);
    if (!item) return false;
    item->usedAt = util::NowFileTime();
    PromoteToFront(id);  // 选择性前置操作 (pinned stays put, unpinned to top)
    return true;
}

// ---- Pinned reordering ----

bool Store::MovePinned(uint64_t id, int delta) {
    if (delta == 0) return false;
    Item* me = FindMutable(id);
    if (!me || !me->pinned) return false;
    int myIdx = PinnedIndexOf(id);
    if (myIdx < 0) return false;
    int target = myIdx + delta;
    int total = PinnedCount();
    if (target < 0 || target >= total) return false;
    return MovePinnedTo(id, target);
}

bool Store::MovePinnedTo(uint64_t id, int targetIndex) {
    const int pc = PinnedCount();
    const int myIdx = PinnedIndexOf(id);
    if (myIdx < 0 || pc <= 1) return false;

    // targetIndex is the insertion slot after removing the source item.
    // Valid range: [0, pc-1]. pc-1 means "insert at end".
    targetIndex = std::clamp(targetIndex, 0, pc - 1);
    if (myIdx == targetIndex) return false;

    // Collect current pinned id order (items_ is already normalized)
    std::vector<uint64_t> ord;
    ord.reserve(static_cast<size_t>(pc));
    for (const Item& it : items_) {
        if (it.pinned) ord.push_back(it.id);
    }

    // Remove source, insert at target
    ord.erase(ord.begin() + myIdx);
    if (targetIndex > static_cast<int>(ord.size())) {
        targetIndex = static_cast<int>(ord.size());
    }
    ord.insert(ord.begin() + targetIndex, id);

    // Re-stamp pinned orders 1..pc
    for (int i = 0; i < static_cast<int>(ord.size()); ++i) {
        if (Item* it = FindMutable(ord[static_cast<size_t>(i)])) {
            it->order = 1u + static_cast<uint32_t>(i);
        }
    }
    SortByOrder(items_);  // Only pinned order changed; re-sort is safe
    return true;
}

// ------------------------------------------------------------------ Persistence

Store::LoadResult Store::PreserveCorrupt() {
    items_.clear();
    nextId_ = 1;
    std::wstring backup =
        util::DataDir() + L"\\store.corrupt." + util::TimeStampForFileName() + L".dat";
    if (MoveFileExW(util::StorePath().c_str(), backup.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        corruptBackup_ = backup;
    } else {
        corruptBackup_ = util::StorePath();
    }
    return LoadResult::Corrupt;
}

// The single parser for the Serialize() on-disk layout. Validates framing and
// fills `out` (hash + derived preview/searchText rebuilt) and `outNextId` from
// buf, WITHOUT touching any member state — so Load() can point it at items_ and
// ImportMerge() at a throwaway list, and "what is a valid v3 store" is defined
// exactly once. Returns false on bad magic, wrong version, over-cap count, or
// any truncated / mis-framed item. No version migration: only the layout
// Serialize() writes is accepted (see the note in store.h).
bool Store::ParseBuffer(const std::vector<uint8_t>& buf, std::vector<Item>& out,
                        uint64_t& outNextId) {
    out.clear();
    outNextId = 1;
    if (buf.size() < kHeaderSize || memcmp(buf.data(), kMagic, 4) != 0) {
        return false;
    }

    size_t pos = 4;
    uint32_t version = 0;
    uint32_t count = 0;
    uint64_t headerNextId = 0;
    if (!Take(buf, pos, version) || !Take(buf, pos, count) || !Take(buf, pos, headerNextId)) {
        return false;
    }
    if (version != kStoreVersion) {
        LOG_WARNING("ParseBuffer: unsupported store version %u (expected %u)",
                    (unsigned)version, (unsigned)kStoreVersion);
        return false;
    }
    if (count > kMaxItemCount) {
        return false;
    }
    pos = kHeaderSize;

    out.reserve(count);
    uint64_t computedNextId = 1;
    for (uint32_t i = 0; i < count; ++i) {
        Item item;
        uint32_t kind = 0;
        uint32_t flags = 0;
        uint32_t dataLen = 0;
        uint32_t appLen = 0;
        if (!Take(buf, pos, item.id) || !Take(buf, pos, kind) || !Take(buf, pos, flags) ||
            !Take(buf, pos, item.createdAt) || !Take(buf, pos, item.usedAt) ||
            !Take(buf, pos, item.imgW) || !Take(buf, pos, item.imgH) ||
            !Take(buf, pos, item.order) || !Take(buf, pos, dataLen)) {
            return false;
        }
        if (kind > static_cast<uint32_t>(ItemKind::FileDrop) || dataLen > kMaxDataLen) {
            return false;
        }
        if (pos + dataLen > buf.size()) {
            return false;
        }
        item.kind = static_cast<ItemKind>(kind);
        item.pinned = (flags & kFlagPinned) != 0;
        item.data.assign(buf.data() + pos, buf.data() + pos + dataLen);
        pos += dataLen;

        // sourceApp trails the payload: length prefix in bytes, UTF-16LE, no
        // terminator. It is a persisted fact, so it is read verbatim rather
        // than recomputed — the owning process may be long gone by now.
        if (!Take(buf, pos, appLen) || (appLen % sizeof(wchar_t)) != 0 ||
            appLen > kMaxSourceAppChars * static_cast<uint32_t>(sizeof(wchar_t)) ||
            pos + appLen > buf.size()) {
            return false;
        }
        item.sourceApp.assign(reinterpret_cast<const wchar_t*>(buf.data() + pos),
                              appLen / sizeof(wchar_t));
        pos += appLen;

        item.hash = textconv::CanonicalHash(item.kind, item.data);
        FillDerived(item, maskCfg_);
        if (item.id >= computedNextId) {
            computedNextId = item.id + 1;
        }
        out.push_back(std::move(item));
    }

    outNextId = headerNextId > computedNextId ? headerNextId : computedNextId;
    return true;
}

Store::LoadResult Store::Load() {
    items_.clear();
    nextId_ = 1;
    corruptBackup_.clear();

    const std::wstring path = util::StorePath();
    if (!FileExists(path)) {
        return LoadResult::Ok;
    }
    std::vector<uint8_t> buf;
    if (!util::ReadWholeFile(path, buf)) {
        return PreserveCorrupt();
    }

    std::vector<Item> loaded;
    uint64_t parsedNextId = 1;
    if (!ParseBuffer(buf, loaded, parsedNextId)) {
        return PreserveCorrupt();
    }

    items_ = std::move(loaded);
    if (parsedNextId > nextId_) {
        nextId_ = parsedNextId;
    }
    // SortByOrder arranges by the persisted order values; Normalize then makes
    // them contiguous [1..P] + [10001..10001+U-1].
    Normalize(items_);
    Evict();
    return LoadResult::Ok;
}

int Store::ImportMerge(const std::vector<uint8_t>& buf) {
    std::vector<Item> incoming;
    uint64_t ignoredNextId = 0;
    if (!ParseBuffer(buf, incoming, ignoredNextId)) {
        // Not a readable v3 backup. items_ is untouched and nothing is renamed:
        // this is a file the user pointed us at, not our own store.dat, so the
        // caller just reports the failure and the current history stays put.
        return -1;
    }
    int merged = 0;
    for (Item& src : incoming) {
        // Through Add(), so an imported entry dedups against what is already
        // here, existing pinned items are never disturbed, and the total cap is
        // enforced exactly as for a fresh copy.
        if (Add(src.kind, std::move(src.data), src.imgW, src.imgH, src.sourceApp) != 0) {
            ++merged;
        }
    }
    return merged;
}

bool Store::Save() {
    ExpireCheck();
    std::vector<uint8_t> buf = Serialize();
    return util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
}

// On-disk layout, little-endian throughout. This is the ONLY layout Load()
// accepts, so it is written out here rather than implied by the code below.
//
//   header (32 bytes)
//     magic     "CLPW"            4
//     version   kStoreVersion     4
//     count     item count        4
//     nextId                      8
//     reserved  zero-filled      12
//   per item
//     id                          8
//     kind      ItemKind as u32   4
//     flags     bit0 = pinned     4
//     createdAt FILETIME          8
//     usedAt    FILETIME          8
//     imgW                        4
//     imgH                        4
//     order                       4
//     dataLen                     4
//     data      dataLen bytes
//     appLen    sourceApp bytes   4
//     sourceApp appLen bytes of UTF-16LE, no terminator
std::vector<uint8_t> Store::Serialize() {
    std::vector<uint8_t> buf;
    size_t estimate = kHeaderSize;
    for (const Item& item : items_) {
        estimate += 52 + item.data.size() + item.sourceApp.size() * sizeof(wchar_t);
    }
    buf.reserve(estimate);

    buf.insert(buf.end(), kMagic, kMagic + 4);
    Append<uint32_t>(buf, kStoreVersion);
    Append<uint32_t>(buf, static_cast<uint32_t>(items_.size()));
    Append<uint64_t>(buf, nextId_);
    buf.resize(kHeaderSize, 0);  // reserved

    for (const Item& item : items_) {
        Append<uint64_t>(buf, item.id);
        Append<uint32_t>(buf, static_cast<uint32_t>(item.kind));
        Append<uint32_t>(buf, item.pinned ? kFlagPinned : 0u);
        Append<uint64_t>(buf, item.createdAt);
        Append<uint64_t>(buf, item.usedAt);
        Append<uint32_t>(buf, item.imgW);
        Append<uint32_t>(buf, item.imgH);
        Append<uint32_t>(buf, item.order);
        Append<uint32_t>(buf, static_cast<uint32_t>(item.data.size()));
        buf.insert(buf.end(), item.data.begin(), item.data.end());
        const size_t appBytes = item.sourceApp.size() * sizeof(wchar_t);
        Append<uint32_t>(buf, static_cast<uint32_t>(appBytes));
        const auto* app = reinterpret_cast<const uint8_t*>(item.sourceApp.data());
        buf.insert(buf.end(), app, app + appBytes);
    }
    return buf;
}

uint64_t Store::TotalDataSize() const {
    uint64_t total = 0;
    for (const Item& item : items_) {
        total += item.data.size();
    }
    return total;
}

