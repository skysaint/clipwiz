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

// Order value space: pinned [1..9999], unpinned [10001..10001+N-1].
// Total items capped at 9999, so ranges never overlap.
constexpr uint32_t kUnpinnedOrderBase = 10001u;

// Hard cap per item data to prevent corrupted files from exhausting memory (64 MB)
constexpr uint32_t kMaxDataLen = 64u * 1024u * 1024u;
constexpr uint32_t kMaxItemCount = 9999u;

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

// Extract plain text from "HTML Format" raw data (rough: strip tags).
// maxChars limits output length (in UTF-8 bytes before final decode); 0 = unlimited.
std::wstring HtmlToPlainText(const std::vector<uint8_t>& data, size_t maxBytes = 0) {
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
    for (size_t i = start; i < raw.size() && (maxBytes == 0 || utf8.size() < maxBytes); ++i) {
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
// maxChars limits output length (in wchar_t); 0 = unlimited.
std::wstring RtfToPlainText(const std::vector<uint8_t>& data, size_t maxChars = 0) {
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

    while (i < raw.size() && (maxChars == 0 || text.size() < maxChars)) {
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

// Canonical dedup hash.
//
// Two entries are considered "the same content" when their *meaningful*
// content matches, not their raw clipboard bytes. Rich text (RTF/HTML) from
// editors like Word embeds volatile bytes (revision ids, timestamps, font
// tables, GUIDs) that differ on every copy of the exact same passage, so
// hashing raw bytes wrongly treats them as distinct. Instead we hash the
// extracted plain-text body.
//
// A per-kind prefix keeps different kinds distinct: a plain-text "hello" and
// an RTF "hello" must remain two separate entries (one carries formatting).
// Images have no text body, so they hash their raw (PNG) bytes.

// The meaningful body used for dedup: extracted plain text for text kinds,
// raw bytes for images. Empty for rich text that carries no extractable text
// (formatting-only or unparseable) — callers must NOT treat two empty bodies
// as duplicates; fall back to raw-byte comparison in that case.
static std::vector<uint8_t> CanonicalBody(ItemKind kind, const std::vector<uint8_t>& data) {
    if (kind == ItemKind::Image) {
        return data;  // No text body: dedup images by raw bytes.
    }
    std::wstring text;
    switch (kind) {
        case ItemKind::Text:
        case ItemKind::FileDrop:
            text.assign(reinterpret_cast<const wchar_t*>(data.data()),
                        data.size() / sizeof(wchar_t));
            break;
        case ItemKind::Html: text = HtmlToPlainText(data); break;
        case ItemKind::Rtf:  text = RtfToPlainText(data);  break;
        default: break;
    }
    return std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()),
        reinterpret_cast<const uint8_t*>(text.data()) + text.size() * sizeof(wchar_t));
}

static const char* CanonicalPrefix(ItemKind kind) {
    switch (kind) {
        case ItemKind::Text:     return "TXT:";
        case ItemKind::FileDrop: return "FILE:";
        case ItemKind::Html:     return "HTML:";
        case ItemKind::Rtf:      return "RTF:";
        case ItemKind::Image:    return "IMG:";
        default:                 return "?:";
    }
}

static uint64_t CanonicalHash(ItemKind kind, const std::vector<uint8_t>& data) {
    const std::vector<uint8_t> body = CanonicalBody(kind, data);
    const char* prefix = CanonicalPrefix(kind);
    std::vector<uint8_t> buf;
    const size_t plen = std::strlen(prefix);
    buf.reserve(plen + body.size());
    buf.insert(buf.end(), prefix, prefix + plen);
    buf.insert(buf.end(), body.begin(), body.end());
    return util::Hash64(buf.data(), buf.size());
}

// The single shared "these two are the same content" predicate, used by Add(),
// Load() dedup, and conversion merges. Two items match when they are the same
// kind and same canonical hash AND either:
//   - the incoming canonical body is non-empty (a real plain-text/image match), or
//   - their raw bytes are byte-equal (fallback for empty-body rich text and as
//     a hash-collision safety belt — prevents unrelated formatting-only RTF/HTML
//     from merging just because both extract to empty text).
// `incomingBodyEmpty` is precomputed once by the caller to avoid re-extracting
// the incoming plain text on every comparison.
static bool SameCanonicalContent(const Item& existing, ItemKind kind,
                                 const std::vector<uint8_t>& data, uint64_t hash,
                                 bool incomingBodyEmpty) {
    if (existing.kind != kind || existing.hash != hash) return false;
    if (!incomingBodyEmpty) return true;     // real body match
    return existing.data == data;            // empty body: require exact bytes
}

// Convenience overload: extracts the body itself (single-comparison callers).
static bool SameCanonicalContent(const Item& existing, ItemKind kind,
                                 const std::vector<uint8_t>& data, uint64_t hash) {
    return SameCanonicalContent(existing, kind, data, hash,
                                CanonicalBody(kind, data).empty());
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
                    text = HtmlToPlainText(item.data, kPreviewLimit * 3);  // UTF-8 bytes
                    break;
                case ItemKind::Rtf:
                    text = RtfToPlainText(item.data, kPreviewLimit);
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

uint64_t Store::Add(ItemKind kind, std::vector<uint8_t> data, uint32_t imgW, uint32_t imgH) {
    if (data.empty()) return 0;
    // Dedup by canonical content (plain-text body + type prefix), NOT raw bytes:
    // Word/browsers emit different RTF/HTML bytes for the exact same passage.
    const uint64_t hash = CanonicalHash(kind, data);
    const bool bodyEmpty = CanonicalBody(kind, data).empty();
    const uint64_t now = util::NowFileTime();

    // Dedup: same kind + same canonical content. On a hit we KEEP THE NEW
    // content (the user may have re-edited formatting/images before recopying,
    // and expects to paste the latest version), then apply 选择性前置操作.
    //
    // NOTE: items_ is NOT guaranteed to hold at most one entry per canonical
    // hash. ConvertToPlainText's "both pinned" branch and Load() may leave two
    // identical-hash entries coexisting. This loop refreshes the FIRST match in
    // display order, which is the intended behavior (the topmost duplicate).
    for (Item& item : items_) {
        if (SameCanonicalContent(item, kind, data, hash, bodyEmpty)) {
            const uint64_t dupId = item.id;
            // Refresh the stored bytes to the newest copy (same canonical hash).
            item.data = std::move(data);
            item.imgW = imgW;
            item.imgH = imgH;
            item.preview = MakeItemPreview(item);
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
    item.hash = hash;
    item.pinned = false;
    item.preview = MakeItemPreview(item);
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
        item.preview = MakeItemPreview(item);
    }
}

void Store::DedupAll() {
    // Merge duplicate canonical-content items. Keep the one earlier in display
    // order (smaller index), drop the later one; but never remove a pinned item
    // when its duplicate is also pinned (pinned = user-only deletion). Assumes
    // items_ is already normalized so indices reflect display order.
    bool changed = false;
    for (size_t i = 0; i < items_.size(); ++i) {
        const bool bodyEmpty = CanonicalBody(items_[i].kind, items_[i].data).empty();
        for (size_t j = i + 1; j < items_.size();) {
            if (SameCanonicalContent(items_[j], items_[i].kind, items_[i].data,
                                     items_[i].hash, bodyEmpty)) {
                if (items_[i].pinned && items_[j].pinned) {
                    ++j;  // Both pinned: keep both, leave the duplicate in place.
                    continue;
                }
                items_.erase(items_.begin() + static_cast<ptrdiff_t>(j));
                changed = true;
                // don't advance j: the erased slot now holds the next item
            } else {
                ++j;
            }
        }
    }
    if (changed && !items_.empty()) Normalize(items_);
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
    const uint64_t newHash = CanonicalHash(ItemKind::Text, bytes);

    LOG_INFO("ConvertToPlainText: id=%llu oldKind=%u idx=%zu -> Text, textLen=%zu newHash=%llu",
             (unsigned long long)id, (unsigned)items_[idx].kind, idx, text.size(),
             (unsigned long long)newHash);

    // In-place: keep id, pinned, order, position, createdAt/usedAt untouched.
    // This is a content edit, not a "use", so we never reorder.
    items_[idx].kind = ItemKind::Text;
    items_[idx].data = std::move(bytes);
    items_[idx].hash = newHash;
    items_[idx].preview = MakeItemPreview(items_[idx]);

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
        if (SameCanonicalContent(items_[i], ItemKind::Text, items_[idx].data, newHash)) {
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
        item.order = haveOrder ? order : 0u;  // Legacy (v1): order=0, fixed by Normalize
        item.data.assign(buf.data() + pos, buf.data() + pos + dataLen);
        pos += dataLen;

        item.hash = CanonicalHash(item.kind, item.data);
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
    // For legacy files (no order field), stable_partition preserves file order;
    // for v2 files, SortByOrder arranges by persisted order values.
    // Either way, Normalize produces contiguous [1..P] + [10001..10001+U-1].
    Normalize(items_);
    // A store written before canonical hashing (v1.1.0 raw-byte dedup) can hold
    // items that now hash identically (e.g. rich text that flattens alike).
    // Merge them so the loaded state honors the same "no duplicates" invariant
    // that Add() maintains.
    DedupAll();
    Evict();
    return LoadResult::Ok;
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
        Append<uint32_t>(buf, item.order);
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

