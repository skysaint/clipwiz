// store.cpp
#include "store.h"

#include <algorithm>
#include <cstring>

#include "i18n.h"
#include "log.h"
#include "util.h"

namespace {

constexpr uint32_t kStoreVersion = 2;   // v2: added per-item `order` field (uint32)
constexpr size_t kHeaderSize = 32;
const char kMagic[4] = {'C', 'L', 'P', 'W'};

constexpr uint32_t kFlagPinned = 0x1;

// Order value space (single global sort key, pinned + unpinned sections are
// defined purely by numeric range — filtering/sorting never needs to consult
// the `pinned` bool separately for order-related questions).
constexpr uint32_t kMinPinnedOrder    = 1u;
constexpr uint32_t kMaxPinnedOrder    = 100000u;        // Match kMaxItemCount so a fully-pinned
                                                        // library (100k items) has unique orders
constexpr uint32_t kUnpinnedOrderBase = kMaxPinnedOrder + 1u;  // [100001..4e9] keeps sections numericall
// (wrapped comment tail — see line above)
constexpr uint32_t kMaxUnpinnedOrder  = 0xFFFFFFFFu - 1u;  // leave one slot so +1 never wraps

inline bool IsPinnedOrder(uint32_t o)  { return o >= kMinPinnedOrder && o <= kMaxPinnedOrder; }
inline bool IsPinnedOrderLegit(uint32_t o) { return o != 0 && (IsPinnedOrder(o) || o >= kUnpinnedOrderBase); }

// Hard cap per item data to prevent corrupted files from exhausting memory (64 MB)
constexpr uint32_t kMaxDataLen = 64u * 1024u * 1024u;
constexpr uint32_t kMaxItemCount = 100000u;

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

// Extract plain text first line from "HTML Format" raw data (rough: strip tags)
std::wstring HtmlToPlainText(const std::vector<uint8_t>& data) {
    // HTML Format is UTF-8 text; decode properly
    std::string raw(reinterpret_cast<const char*>(data.data()), data.size());
    // Stop at NUL terminator
    size_t nulPos = raw.find('\0');
    if (nulPos != std::string::npos) {
        raw.resize(nulPos);
    }
    // Find content after <body>
    size_t start = 0;
    size_t bodyPos = raw.find("<body");
    if (bodyPos == std::string::npos) {
        bodyPos = raw.find("<BODY");
    }
    if (bodyPos != std::string::npos) {
        size_t gt = raw.find('>', bodyPos);
        start = (gt != std::string::npos) ? gt + 1 : bodyPos;
    }

    // Helper: append a Unicode code point as UTF-8
    auto appendUtf8 = [](std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };

    // Strip tags, decode HTML entities
    std::string utf8;
    bool inTag = false;
    for (size_t i = start; i < raw.size() && utf8.size() < 1500; ++i) {
        char ch = raw[i];
        if (ch == '<') {
            inTag = true;
            continue;
        }
        if (ch == '>') {
            inTag = false;
            continue;
        }
        if (inTag) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (!utf8.empty() && utf8.back() != ' ') {
                utf8 += ' ';
            }
            continue;
        }
        if (ch == '&') {
            // Try to decode HTML entity
            size_t semi = raw.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 12) {
                std::string ent = raw.substr(i + 1, semi - i - 1);
                bool decoded = true;
                if (ent == "nbsp" || ent == "#160") {
                    utf8 += ' ';
                } else if (ent == "amp") {
                    utf8 += '&';
                } else if (ent == "lt") {
                    utf8 += '<';
                } else if (ent == "gt") {
                    utf8 += '>';
                } else if (ent == "quot") {
                    utf8 += '"';
                } else if (ent == "apos") {
                    utf8 += '\'';
                } else if (ent == "mdash") {
                    appendUtf8(utf8, 0x2014);
                } else if (ent == "ndash") {
                    appendUtf8(utf8, 0x2013);
                } else if (ent == "hellip") {
                    appendUtf8(utf8, 0x2026);
                } else if (!ent.empty() && ent[0] == '#') {
                    uint32_t cp = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        cp = static_cast<uint32_t>(strtoul(ent.c_str() + 2, nullptr, 16));
                    } else {
                        cp = static_cast<uint32_t>(strtoul(ent.c_str() + 1, nullptr, 10));
                    }
                    if (cp > 0 && cp < 0x110000) {
                        appendUtf8(utf8, cp);
                    }
                } else {
                    decoded = false;
                }
                if (decoded) {
                    i = semi;  // Skip past entity
                    continue;
                }
            }
            // Not a recognized entity, output '&' literally
            utf8 += '&';
            continue;
        }
        utf8 += ch;
    }
    // Decode UTF-8 to wide string
    if (utf8.empty()) {
        return {};
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                   nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), text.data(), wlen);
    return text;
}

// Decode a byte buffer using the specified code page, append to text
void DecodeBytes(const std::vector<uint8_t>& buf, UINT codePage, std::wstring& text) {
    if (buf.empty()) {
        return;
    }
    int wlen = MultiByteToWideChar(codePage, 0, reinterpret_cast<const char*>(buf.data()),
                                   static_cast<int>(buf.size()), nullptr, 0);
    if (wlen > 0) {
        size_t oldSize = text.size();
        text.resize(oldSize + static_cast<size_t>(wlen));
        MultiByteToWideChar(codePage, 0, reinterpret_cast<const char*>(buf.data()),
                            static_cast<int>(buf.size()), text.data() + oldSize, wlen);
    }
}

// Destination groups whose content should be entirely skipped
static bool IsSkipDestination(const std::string& word) {
    static const char* const kSkip[] = {
        "fonttbl", "colortbl", "stylesheet", "pict", "object",
        "themedata", "listtable", "listoverridetable", "rsidtbl",
        "generator", "info", "latentstyles", "datastore", "mmathPr",
        "wgrffmtfilter", "mso", "xmlnstbl", "pgptbl", "revtbl",
    };
    for (const char* s : kSkip) {
        if (word == s) {
            return true;
        }
    }
    return false;
}

// Extract plain text from RTF raw data
// Handles: \uNNNN (Unicode), \'xx (code page bytes), \ansicpgN, \ucN
// Uses a per-group skip stack so nested destinations are handled correctly.
std::wstring RtfToPlainText(const std::vector<uint8_t>& data) {
    std::string raw(reinterpret_cast<const char*>(data.data()), data.size());
    // Stop at NUL terminator (clipboard RTF is typically NUL-terminated)
    size_t nulPos = raw.find('\0');
    if (nulPos != std::string::npos) {
        raw.resize(nulPos);
    }

    std::wstring text;
    std::vector<bool> groupSkip;  // Per-depth skip flag
    int skipCount = 0;            // Number of active skip levels
    UINT codePage = CP_ACP;       // Declared by \ansicpgN; default = system ANSI
    int ucSkip = 1;               // Fallback chars after \uNNNN (from \ucN)
    std::vector<uint8_t> pendingBytes;
    size_t i = 0;

    auto flushBytes = [&]() {
        if (!pendingBytes.empty()) {
            DecodeBytes(pendingBytes, codePage, text);
            pendingBytes.clear();
        }
    };

    while (i < raw.size() && text.size() < 500) {
        char ch = raw[i];

        if (ch == '{') {
            flushBytes();
            ++i;
            // Detect if this group is a skippable destination
            bool skip = false;
            if (i < raw.size() && raw[i] == '\\') {
                if (i + 1 < raw.size() && raw[i + 1] == '*') {
                    // {\*\xxx} - ignorable destination, always skip
                    skip = true;
                } else {
                    // {\word ...} - check known destinations
                    size_t ws = i + 1;
                    size_t we = ws;
                    while (we < raw.size() && isalpha(static_cast<unsigned char>(raw[we]))) {
                        ++we;
                    }
                    std::string w = raw.substr(ws, we - ws);
                    if (w == "pict" && skipCount == 0) {
                        // Embedded image: extract dimensions for placeholder
                        long picW = 0, picH = 0;
                        long goalW = 0, goalH = 0;
                        size_t scan = we;
                        size_t scanEnd = (std::min)(scan + 400, raw.size());
                        while (scan < scanEnd) {
                            if (raw[scan] == '}') {
                                break;
                            }
                            if (raw[scan] == '\\') {
                                ++scan;
                                size_t cs = scan;
                                while (scan < scanEnd &&
                                       isalpha(static_cast<unsigned char>(raw[scan]))) {
                                    ++scan;
                                }
                                std::string cw = raw.substr(cs, scan - cs);
                                std::string cp;
                                if (scan < scanEnd &&
                                    (raw[scan] == '-' ||
                                     isdigit(static_cast<unsigned char>(raw[scan])))) {
                                    size_t ps = scan;
                                    while (scan < scanEnd &&
                                           (raw[scan] == '-' ||
                                            isdigit(static_cast<unsigned char>(raw[scan])))) {
                                        ++scan;
                                    }
                                    cp = raw.substr(ps, scan - ps);
                                }
                                if (cw == "picwgoal" && !cp.empty()) {
                                    goalW = strtol(cp.c_str(), nullptr, 10);
                                } else if (cw == "pichgoal" && !cp.empty()) {
                                    goalH = strtol(cp.c_str(), nullptr, 10);
                                } else if (cw == "picw" && !cp.empty()) {
                                    picW = strtol(cp.c_str(), nullptr, 10);
                                } else if (cw == "pich" && !cp.empty()) {
                                    picH = strtol(cp.c_str(), nullptr, 10);
                                }
                            } else {
                                ++scan;
                            }
                        }
                        // Prefer display goals (twips); fall back to native size
                        long twipsW = goalW > 0 ? goalW : picW;
                        long twipsH = goalH > 0 ? goalH : picH;
                        int pxW = twipsW > 0 ? static_cast<int>(twipsW / 15) : 0;
                        int pxH = twipsH > 0 ? static_cast<int>(twipsH / 15) : 0;
                        if (pxW > 0 && pxH > 0) {
                            wchar_t buf[32];
                            swprintf_s(buf, L"[\x56fe\x7247 %d\x00d7%d]", pxW, pxH);
                            text += buf;
                        } else {
                            text += L"[\x56fe\x7247]";
                        }
                        skip = true;
                    } else if (IsSkipDestination(w)) {
                        skip = true;
                    }
                }
            }
            groupSkip.push_back(skip);
            if (skip) {
                ++skipCount;
            }
            continue;
        }

        if (ch == '}') {
            flushBytes();
            if (!groupSkip.empty()) {
                if (groupSkip.back()) {
                    --skipCount;
                }
                groupSkip.pop_back();
            }
            ++i;
            continue;
        }

        // Skip content inside destination groups
        if (skipCount > 0) {
            ++i;
            continue;
        }

        if (ch == '\\') {
            ++i;
            if (i >= raw.size()) {
                break;
            }
            char next = raw[i];
            if (next == '\\' || next == '{' || next == '}') {
                flushBytes();
                text += static_cast<wchar_t>(next);
                ++i;
                continue;
            }
            if (next == '\'') {
                // \'xx hexadecimal byte in document code page
                ++i;
                if (i + 1 < raw.size()) {
                    char hex[3] = {raw[i], raw[i + 1], 0};
                    int val = static_cast<int>(strtol(hex, nullptr, 16));
                    pendingBytes.push_back(static_cast<uint8_t>(val));
                    i += 2;
                }
                continue;
            }
            if (next == '\r' || next == '\n') {
                ++i;
                continue;
            }
            if (next == '*') {
                // \* outside group open (shouldn't happen normally), skip char
                ++i;
                continue;
            }
            // Control word
            flushBytes();
            size_t wordStart = i;
            while (i < raw.size() && isalpha(static_cast<unsigned char>(raw[i]))) {
                ++i;
            }
            std::string word = raw.substr(wordStart, i - wordStart);
            // Read optional numeric parameter
            std::string param;
            if (i < raw.size() && (raw[i] == '-' || isdigit(static_cast<unsigned char>(raw[i])))) {
                size_t paramStart = i;
                while (i < raw.size() &&
                       (raw[i] == '-' || isdigit(static_cast<unsigned char>(raw[i])))) {
                    ++i;
                }
                param = raw.substr(paramStart, i - paramStart);
            }
            // Space after control word is a delimiter, consume it
            if (i < raw.size() && raw[i] == ' ') {
                ++i;
            }
            if (word == "u" && !param.empty()) {
                // \uNNNN: Unicode character; skip ucSkip fallback characters
                long cp = strtol(param.c_str(), nullptr, 10);
                if (cp < 0) {
                    cp += 65536;  // RTF uses signed 16-bit
                }
                if (cp >= 0x20) {
                    text += static_cast<wchar_t>(cp);
                }
                // Skip fallback characters (\'xx or literal)
                int skipped = 0;
                while (skipped < ucSkip && i < raw.size()) {
                    if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\'') {
                        i += 4;  // Skip \'xx
                        ++skipped;
                    } else if (raw[i] == '\\') {
                        break;  // Another control word, stop skipping
                    } else {
                        ++i;
                        ++skipped;
                    }
                }
            } else if (word == "ansicpg" && !param.empty()) {
                codePage = static_cast<UINT>(strtoul(param.c_str(), nullptr, 10));
            } else if (word == "uc" && !param.empty()) {
                ucSkip = static_cast<int>(strtol(param.c_str(), nullptr, 10));
                if (ucSkip < 0) {
                    ucSkip = 0;
                }
            } else if (word == "par" || word == "line") {
                text += L' ';
            } else if (word == "tab") {
                text += L'\t';
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            ++i;
            continue;
        }
        // Non-ASCII raw byte: accumulate for code page decoding
        if (static_cast<unsigned char>(ch) >= 0x80) {
            pendingBytes.push_back(static_cast<uint8_t>(ch));
        } else {
            flushBytes();
            text += static_cast<wchar_t>(ch);
        }
        ++i;
    }
    flushBytes();
    return text;
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
            return HtmlToPlainText(item.data);
        case ItemKind::Rtf:
            return RtfToPlainText(item.data);
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
            std::wstring text = Store::TextOf(item);
            std::wstring preview = util::OneLinePreview(text, 160);
            return preview.empty() ? std::wstring(i18n::T("preview.empty")) : preview;
        }
    }
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
    // Both sort and lookup key = order value; pinned section is contiguous by
    // range.  This is O(n) but n < 100 for pinned in practice.
    const Item* me = nullptr;
    uint32_t meOrder = 0;
    for (const Item& it : items_) {
        if (it.id == id && it.pinned) {
            me = &it;
            meOrder = IsPinnedOrderLegit(it.order) ? it.order : 0u;
            break;
        }
    }
    if (!me) return -1;
    // Count strictly-smaller pinned order values -> 0-based index.
    int idx = 0;
    for (const Item& it : items_) {
        if (!it.pinned) continue;
        uint32_t o = IsPinnedOrderLegit(it.order) ? it.order : 0u;
        if (o == 0) continue;
        if (o < meOrder) ++idx;
        else if (o == meOrder && &it != me) ++idx;  // Tie-break: physical order
    }
    // Safety: never exceed logical pinned count - 1.
    int pc = PinnedCount();
    if (idx >= pc) idx = std::max(0, pc - 1);
    return idx;
}

int Store::HistoryCount() const {
    return static_cast<int>(items_.size()) - PinnedCount();
}

void Store::SetLimits(int maxHistory, int expiryDays) {
    maxHistory_ = std::clamp(maxHistory, 5, 2000);
    expiryDays_ = std::max(0, expiryDays);
    ExpireCheck();
    Evict();
}

// Make sure every item's `order` is normalized to (pinned -> [1..pcount]
// contiguous, unpinned -> [kUnpinnedOrderBase..kUnpinnedOrderBase+ucount-1]
// contiguous).  Any move/setPinned/add op that pushes us to either section
// ceiling triggers a renormalization so we never hit the hard uint32 cap.
// This is cheap (<1 us for a few hundred items) and eliminates the risk of
// a "no-op" swap because the neighbor slot was already clamped.
namespace {
void CompactOrders(std::vector<Item>& items) {
    // First sort by (section, existing order, tie-breaker id) so physical order
    // is stable before we stamp new contiguous values.
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        bool ap = a.pinned, bp = b.pinned;
        if (ap != bp) return ap;
        uint32_t ao = IsPinnedOrderLegit(a.order) ? a.order : 0u;
        uint32_t bo = IsPinnedOrderLegit(b.order) ? b.order : 0u;
        if (ao != 0 && bo != 0) {
            if (ao != bo) return ao < bo;
        } else if (ao != 0) return true;
        else if (bo != 0) return false;
        return a.id < b.id;
    });
    uint32_t pinNext = kMinPinnedOrder;
    uint32_t unpNext = kUnpinnedOrderBase;
    for (Item& it : items) {
        if (it.pinned) {
            it.order = pinNext;
            if (pinNext < kMaxPinnedOrder) {
                ++pinNext;
            } else {
                LOG_WARNING("CompactOrders: pinned section exhausted order space (%u items). "
                            "Remaining pinned items share order=%u (ties broken by id).",
                            kMaxPinnedOrder, pinNext);
            }
        } else {
            it.order = unpNext;
            if (unpNext < kMaxUnpinnedOrder) {
                ++unpNext;
            } else {
                LOG_WARNING("CompactOrders: unpinned section exhausted order space (4e9 items). "
                            "Unpinned tail shares order=%u.",
                            unpNext);
            }
        }
    }
}
}

// Sort vector physically once by order; afterwards items_[0] = pinned-first,
// last = oldest unpinned.  Display / index iteration becomes trivial.  The
// `pinned` bool is still the source of truth for which section an item
// belongs to — order is derived and compacted to match.
void Store::Reorder() {
    CompactOrders(items_);
}

void Store::Evict() {
    // History limit counts only unpinned items.  Unpinned items with the
    // smallest order value = farthest back in time; that's who we kick out.
    for (;;) {
        if (HistoryCount() <= maxHistory_) break;
        size_t victim = items_.size();
        uint64_t oldestUsed = UINT64_MAX;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].pinned) continue;
            if (items_[i].usedAt <= oldestUsed) {
                oldestUsed = items_[i].usedAt;
                victim = i;
            }
        }
        if (victim >= items_.size()) break;
        items_.erase(items_.begin() + static_cast<ptrdiff_t>(victim));
    }
    // After physical erase, recompute contiguous order space.
    if (!items_.empty()) CompactOrders(items_);
}

void Store::ExpireCheck() {
    if (expiryDays_ <= 0) return;
    const uint64_t cutoff = util::NowFileTime() - static_cast<uint64_t>(expiryDays_) * kTicksPerDay;
    size_t before = items_.size();
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
                       [cutoff](const Item& item) { return !item.pinned && item.usedAt < cutoff; }),
        items_.end());
    if (items_.size() != before && !items_.empty()) CompactOrders(items_);
}

uint64_t Store::Add(ItemKind kind, std::vector<uint8_t> data, uint32_t imgW, uint32_t imgH) {
    if (data.empty()) return 0;
    const uint64_t hash = util::Hash64(data.data(), data.size());
    const uint64_t now = util::NowFileTime();

    // Dedup: same kind and same content
    for (Item& item : items_) {
        if (item.kind == kind && item.hash == hash && item.data == data) {
            if (item.pinned) {
                return item.id;
            } else {
                // Save id BEFORE CompactOrders (stable_sort swaps vector
                // element VALUES, so a reference bound to physical slot X
                // would end up pointing to a different Item object after).
                const uint64_t dupId = item.id;
                CompactOrders(items_);
                Item* dup = FindMutable(dupId);
                if (!dup) return 0;  // Defensive: id vanished somehow
                dup->usedAt = now;
                // Move to tail of unpinned section.
                uint32_t maxUp = kUnpinnedOrderBase;
                for (const Item& it : items_) {
                    if (!it.pinned && it.order > maxUp) maxUp = it.order;
                }
                dup->order = (maxUp < kMaxUnpinnedOrder) ? (maxUp + 1u) : kUnpinnedOrderBase;
                Reorder();
                return dupId;
            }
        }
    }

    Item item;
    item.id = nextId_++;
    item.kind = kind;
    item.createdAt = now;
    item.usedAt = now;
    item.data = std::move(data);
    item.imgW = imgW;
    item.imgH = imgH;
    item.hash = hash;
    item.preview = MakeItemPreview(item);
    const uint64_t id = item.id;
    item.pinned = false;
    // Assign to end of unpinned section (will be sorted into place below).
    // If the unpinned space is approaching the cap, Reorder compacts first.
    uint32_t maxUp = kUnpinnedOrderBase;
    for (const Item& it : items_) {
        if (!it.pinned && it.order > maxUp) maxUp = it.order;
    }
    item.order = (maxUp < kMaxUnpinnedOrder) ? (maxUp + 1) : kUnpinnedOrderBase;
    items_.push_back(std::move(item));
    Reorder();
    Evict();
    return id;
}

bool Store::SetPinned(uint64_t id, bool pinned) {
    Item* item = FindMutable(id);
    if (!item || item->pinned == pinned) return false;
    item->pinned = pinned;
    // The item moves across sections: set a target order at the tail of its
    // NEW section, then Reorder compacts + sorts physically.
    if (pinned) {
        // Append to pinned tail.
        uint32_t maxP = 0;
        for (const Item& it : items_) {
            if (it.pinned && it.id != id && IsPinnedOrder(it.order) && it.order > maxP) {
                maxP = it.order;
            }
        }
        if (maxP >= kMaxPinnedOrder || maxP == 0) {
            // Need renormalization; Reorder handles it.
            item->order = kMinPinnedOrder;
        } else {
            item->order = maxP + 1;
        }
    } else {
        // Append to unpinned tail.
        uint32_t maxUp = kUnpinnedOrderBase;
        for (const Item& it : items_) {
            if (!it.pinned && it.id != id && it.order >= kUnpinnedOrderBase && it.order > maxUp) {
                maxUp = it.order;
            }
        }
        if (maxUp >= kMaxUnpinnedOrder) {
            item->order = kUnpinnedOrderBase;
        } else {
            item->order = maxUp + 1;
        }
    }
    Reorder();
    Evict();
    return true;
}

bool Store::Remove(uint64_t id) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) {
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(i));
            if (!items_.empty()) CompactOrders(items_);
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
    // Pinned items' physical order didn't change, but renormalize pinned
    // order values to a contiguous [1..N] for defensive hygiene.
    if (!items_.empty()) CompactOrders(items_);
}

bool Store::Touch(uint64_t id) {
    Item* item = FindMutable(id);
    if (!item) return false;
    item->usedAt = util::NowFileTime();
    if (!item->pinned) {
        // Promote to very end of unpinned order space, then Reorder.
        uint32_t maxUp = kUnpinnedOrderBase;
        for (const Item& it : items_) {
            if (!it.pinned && &it != item && it.order >= kUnpinnedOrderBase && it.order > maxUp) {
                maxUp = it.order;
            }
        }
        if (maxUp < kMaxUnpinnedOrder) {
            item->order = maxUp + 1;
        } else {
            item->order = kUnpinnedOrderBase;  // will be sorted to top after compact
        }
        Reorder();
    }
    return true;
}

// ---- Pinned reordering: all logic is now just `order` value shuffling ----
//
// Strategy:
//   * Move up/down one -> swap `order` value with neighbor in pinned section
//   * Move to head/tail    -> assign new head/tail order
//   * If any operation bumps an edge value to its ceiling (unlikely but
//     possible after 1e5 one-way "to top" presses), trigger a full
//     CompactOrders to re-normalize the order space.

bool Store::MovePinned(uint64_t id, int delta) {
    if (delta == 0) return false;
    Item* me = FindMutable(id);
    if (!me || !me->pinned) return false;
    if (!IsPinnedOrder(me->order)) CompactOrders(items_);  // repair bad state
    // After potential compact, re-lookup pointer (compact moves vector!)
    me = FindMutable(id);
    if (!me) return false;
    int total = PinnedCount();
    int myIdx = PinnedIndexOf(id);
    int target = myIdx + delta;
    if (target < 0 || target >= total) return false;
    return MovePinnedTo(id, target);
}

bool Store::MovePinnedTo(uint64_t id, int targetIndex) {
    int count = PinnedCount();
    if (targetIndex < 0 || targetIndex >= count) return false;
    Item* me = FindMutable(id);
    if (!me || !me->pinned) return false;
    int myIdx = PinnedIndexOf(id);
    if (myIdx == targetIndex) return false;

    // Gather *all* pinned items sorted by (order, id) -> we can directly
    // compute what the new order value for `me` should be by mapping the
    // targetIndex to the neighbor whose slot we're taking.  Simplest safe
    // approach: compact the orders, then swap me into target index.
    CompactOrders(items_);
    // Compact may have invalidated `me` — re-fetch everything by id.
    int pc = PinnedCount();
    if (pc != count) {
        // Item count changed during compact (shouldn't happen); abort safely.
        return false;
    }
    myIdx = PinnedIndexOf(id);
    if (myIdx < 0) return false;
    if (targetIndex < 0 || targetIndex >= pc) targetIndex = std::clamp(targetIndex, 0, pc - 1);
    if (myIdx == targetIndex) return false;

    // After compact, every pinned item's order is exactly kMinPinnedOrder +
    // index_in_pinned_section.  To move item I to position T, simply swap
    // I's order value with whatever item currently sits at T, or shift-
    // assign if a range move (easier: re-stamp contiguous orders for the
    // affected region only, which is O(count) and count < 1e5 max so fine).
    struct PinRef { uint64_t id; uint32_t order; };
    std::vector<PinRef> pins;
    pins.reserve(static_cast<size_t>(pc));
    for (const Item& it : items_) {
        if (it.pinned) pins.push_back({it.id, it.order});
    }
    // Sort pins by existing order (compact guarantees they're 1..pc already
    // but be defensive).
    std::stable_sort(pins.begin(), pins.end(),
                     [](const PinRef& a, const PinRef& b) { return a.order < b.order; });
    // Find me inside pins by id.
    int from = -1;
    for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
        if (pins[i].id == id) { from = i; break; }
    }
    if (from < 0) return false;
    int to = std::clamp(targetIndex, 0, static_cast<int>(pins.size()) - 1);
    if (from == to) return false;

    // Shift pins[from] to position `to` in the ref list.
    PinRef moved = pins[from];
    pins.erase(pins.begin() + from);
    pins.insert(pins.begin() + to, moved);

    // Re-stamp contiguous 1..pc orders onto items matching each pins[i].id.
    for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
        Item* it = FindMutable(pins[i].id);
        if (it) it->order = kMinPinnedOrder + static_cast<uint32_t>(i);
    }
    // Finally physically re-sort the vector to match order.
    Reorder();
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
    if (buf.size() < kHeaderSize || memcmp(buf.data(), kMagic, 4) != 0) {
        return PreserveCorrupt();
    }

    size_t pos = 4;
    uint32_t version = 0;
    uint32_t count = 0;
    uint64_t nextId = 0;
    if (!Take(buf, pos, version) || !Take(buf, pos, count) || !Take(buf, pos, nextId)) {
        return PreserveCorrupt();
    }
    // Accept v1 (legacy, no per-item order) and v2 (current).
    if (version != 1u && version != kStoreVersion) {
        return PreserveCorrupt();
    }
    if (count > kMaxItemCount) {
        return PreserveCorrupt();
    }
    const bool haveOrder = (version >= 2u);
    pos = kHeaderSize;

    std::vector<Item> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Item item;
        uint32_t kind = 0;
        uint32_t flags = 0;
        uint32_t order = 0;
        uint32_t dataLen = 0;
        if (!Take(buf, pos, item.id) || !Take(buf, pos, kind) || !Take(buf, pos, flags) ||
            !Take(buf, pos, item.createdAt) || !Take(buf, pos, item.usedAt) ||
            !Take(buf, pos, item.imgW) || !Take(buf, pos, item.imgH) ||
            (haveOrder && !Take(buf, pos, order)) ||
            !Take(buf, pos, dataLen)) {
            return PreserveCorrupt();
        }
        if (kind > static_cast<uint32_t>(ItemKind::FileDrop) || dataLen > kMaxDataLen) {
            return PreserveCorrupt();
        }
        if (pos + dataLen > buf.size()) {
            return PreserveCorrupt();
        }
        item.kind = static_cast<ItemKind>(kind);
        item.pinned = (flags & kFlagPinned) != 0;
        if (haveOrder && IsPinnedOrderLegit(order)) {
            item.order = order;
        } else {
            item.order = 0;  // Legacy: will be fixed up by CompactOrders below.
        }
        item.data.assign(buf.data() + pos, buf.data() + pos + dataLen);
        pos += dataLen;

        item.hash = util::Hash64(item.data.data(), item.data.size());
        item.preview = MakeItemPreview(item);
        if (item.id >= nextId_) {
            nextId_ = item.id + 1;
        }
        loaded.push_back(std::move(item));
    }

    items_ = std::move(loaded);
    if (nextId > nextId_) {
        nextId_ = nextId;
    }
    Reorder();   // CompactOrders fills in legacy order==0 slots and normalizes values.
    Evict();
    return LoadResult::Ok;
}

bool Store::LoadItemsFrom(const std::wstring& path, std::vector<Item>& out) {
    out.clear();
    std::vector<uint8_t> buf;
    if (!util::ReadWholeFile(path, buf)) return false;
    if (buf.size() < kHeaderSize || memcmp(buf.data(), kMagic, 4) != 0) return false;

    size_t pos = 4;
    uint32_t version = 0, count = 0;
    uint64_t nextId = 0;
    if (!Take(buf, pos, version) || !Take(buf, pos, count) || !Take(buf, pos, nextId))
        return false;
    // Accept v1 or v2 (current).
    if (version != 1u && version != kStoreVersion) return false;
    if (count > kMaxItemCount) return false;
    const bool haveOrder = (version >= 2u);
    pos = kHeaderSize;

    out.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Item item;
        uint32_t kind = 0, flags = 0, order = 0, dataLen = 0;
        if (!Take(buf, pos, item.id) || !Take(buf, pos, kind) || !Take(buf, pos, flags) ||
            !Take(buf, pos, item.createdAt) || !Take(buf, pos, item.usedAt) ||
            !Take(buf, pos, item.imgW) || !Take(buf, pos, item.imgH) ||
            (haveOrder && !Take(buf, pos, order)) ||
            !Take(buf, pos, dataLen))
            return false;
        if (kind > static_cast<uint32_t>(ItemKind::FileDrop) || dataLen > kMaxDataLen)
            return false;
        if (pos + dataLen > buf.size()) return false;
        item.kind = static_cast<ItemKind>(kind);
        item.pinned = (flags & kFlagPinned) != 0;
        if (haveOrder && IsPinnedOrderLegit(order)) item.order = order;
        else item.order = 0;
        item.data.assign(buf.data() + pos, buf.data() + pos + dataLen);
        pos += dataLen;
        item.hash = util::Hash64(item.data.data(), item.data.size());
        item.preview = MakeItemPreview(item);
        out.push_back(std::move(item));
    }
    // LoadItemsFrom returns raw items as they were on disk (merge code compares pinned then unpinned
    // separately); CompactOrders normalizes legacy item.order == 0 to a sane deterministic
    // ordering so the merge output is stable.
    if (!out.empty()) CompactOrders(out);
    (void)nextId;
    return true;
}

bool Store::Save() {
    ExpireCheck();
    std::vector<uint8_t> buf = Serialize();
    return util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
}

std::vector<uint8_t> Store::Serialize() {
    // v2 on-disk format always writes order field per-item.
    std::vector<uint8_t> buf;
    size_t estimate = kHeaderSize;
    for (const Item& item : items_) {
        estimate += 48 + item.data.size();   // 44 original + 4 for order
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
        Append<uint32_t>(buf, IsPinnedOrderLegit(item.order) ? item.order : 0u);
        Append<uint32_t>(buf, static_cast<uint32_t>(item.data.size()));
        buf.insert(buf.end(), item.data.begin(), item.data.end());
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

std::vector<uint8_t> Store::SerializeItems(const std::vector<Item>& items) {
    uint64_t nextId = 1;
    for (const Item& item : items) {
        if (item.id >= nextId) nextId = item.id + 1;
    }

    std::vector<uint8_t> buf;
    size_t estimate = kHeaderSize;
    for (const Item& item : items) {
        estimate += 48 + item.data.size();
    }
    buf.reserve(estimate);

    buf.insert(buf.end(), kMagic, kMagic + 4);
    Append<uint32_t>(buf, kStoreVersion);
    Append<uint32_t>(buf, static_cast<uint32_t>(items.size()));
    Append<uint64_t>(buf, nextId);
    buf.resize(kHeaderSize, 0);

    for (const Item& item : items) {
        Append<uint64_t>(buf, item.id);
        Append<uint32_t>(buf, static_cast<uint32_t>(item.kind));
        Append<uint32_t>(buf, item.pinned ? kFlagPinned : 0u);
        Append<uint64_t>(buf, item.createdAt);
        Append<uint64_t>(buf, item.usedAt);
        Append<uint32_t>(buf, item.imgW);
        Append<uint32_t>(buf, item.imgH);
        Append<uint32_t>(buf, IsPinnedOrderLegit(item.order) ? item.order : 0u);
        Append<uint32_t>(buf, static_cast<uint32_t>(item.data.size()));
        buf.insert(buf.end(), item.data.begin(), item.data.end());
    }
    return buf;
}
