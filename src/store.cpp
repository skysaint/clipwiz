// store.cpp
#include "store.h"

#include <algorithm>
#include <cstring>

#include "i18n.h"
#include "util.h"

namespace {

constexpr uint32_t kStoreVersion = 1;
constexpr size_t kHeaderSize = 32;
const char kMagic[4] = {'C', 'L', 'P', 'W'};

constexpr uint32_t kFlagPinned = 0x1;

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

int Store::HistoryCount() const {
    return static_cast<int>(items_.size()) - PinnedCount();
}

void Store::SetLimits(int maxHistory, int expiryDays) {
    maxHistory_ = std::clamp(maxHistory, 5, 2000);
    expiryDays_ = std::max(0, expiryDays);
    ExpireCheck();
    Evict();
}

// Pinned section keeps manual order; unpinned section sorted by usedAt descending
void Store::Reorder() {
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        if (a.pinned != b.pinned) {
            return a.pinned;  // Pinned items first
        }
        if (a.pinned) {
            return false;  // Within pinned section, maintain original relative order (stable_sort guarantees)
        }
        if (a.usedAt != b.usedAt) {
            return a.usedAt > b.usedAt;
        }
        return a.id > b.id;
    });
}

void Store::Evict() {
    // History limit counts only unpinned items
    for (;;) {
        if (HistoryCount() <= maxHistory_) {
            break;
        }
        // Find the unpinned item with the smallest usedAt
        size_t victim = items_.size();
        uint64_t oldest = UINT64_MAX;
        for (size_t i = 0; i < items_.size(); ++i) {
            if (items_[i].pinned) {
                continue;
            }
            if (items_[i].usedAt <= oldest) {
                oldest = items_[i].usedAt;
                victim = i;
            }
        }
        if (victim >= items_.size()) {
            break;
        }
        items_.erase(items_.begin() + static_cast<ptrdiff_t>(victim));
    }
}

void Store::ExpireCheck() {
    if (expiryDays_ <= 0) {
        return;
    }
    const uint64_t cutoff = util::NowFileTime() - static_cast<uint64_t>(expiryDays_) * kTicksPerDay;
    items_.erase(
        std::remove_if(items_.begin(), items_.end(),
                       [cutoff](const Item& item) { return !item.pinned && item.usedAt < cutoff; }),
        items_.end());
}

uint64_t Store::Add(ItemKind kind, std::vector<uint8_t> data, uint32_t imgW, uint32_t imgH) {
    if (data.empty()) {
        return 0;
    }
    const uint64_t hash = util::Hash64(data.data(), data.size());
    const uint64_t now = util::NowFileTime();

    // Dedup: same kind and same content
    for (Item& item : items_) {
        if (item.kind == kind && item.hash == hash && item.data == data) {
            if (item.pinned) {
                // Pinned item duplicate: do nothing, preserve manual order
                return item.id;
            } else {
                // Unpinned duplicate: refresh usedAt and move to front of unpinned section
                item.usedAt = now;
                Reorder();
                return item.id;
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
    items_.push_back(std::move(item));
    Reorder();
    Evict();
    return id;
}

bool Store::SetPinned(uint64_t id, bool pinned) {
    Item* item = FindMutable(id);
    if (!item || item->pinned == pinned) {
        return false;
    }
    item->pinned = pinned;
    Reorder();
    Evict();
    return true;
}

bool Store::Remove(uint64_t id) {
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id) {
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool Store::Touch(uint64_t id) {
    Item* item = FindMutable(id);
    if (!item) {
        return false;
    }
    item->usedAt = util::NowFileTime();
    if (!item->pinned) {
        Reorder();  // Reorder unpinned section after usedAt refresh
    }
    // Pinned items don't change position
    return true;
}

bool Store::MovePinned(uint64_t id, int delta) {
    // Find current index within pinned section
    int pinnedIdx = -1;
    int count = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].pinned) {
            break;  // Pinned section is at the front
        }
        if (items_[i].id == id) {
            pinnedIdx = count;
        }
        ++count;
    }
    if (pinnedIdx < 0) {
        return false;
    }
    int target = pinnedIdx + delta;
    if (target < 0 || target >= count) {
        return false;
    }
    // Swap positions in items_
    size_t srcIdx = static_cast<size_t>(pinnedIdx);
    size_t dstIdx = static_cast<size_t>(target);
    Item tmp = std::move(items_[srcIdx]);
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(srcIdx));
    items_.insert(items_.begin() + static_cast<ptrdiff_t>(dstIdx), std::move(tmp));
    return true;
}

bool Store::MovePinnedTo(uint64_t id, int targetIndex) {
    int count = PinnedCount();
    if (targetIndex < 0 || targetIndex >= count) {
        return false;
    }
    // Find the current item
    size_t srcIdx = items_.size();
    for (size_t i = 0; i < items_.size(); ++i) {
        if (items_[i].id == id && items_[i].pinned) {
            srcIdx = i;
            break;
        }
    }
    if (srcIdx >= items_.size()) {
        return false;
    }
    Item tmp = std::move(items_[srcIdx]);
    items_.erase(items_.begin() + static_cast<ptrdiff_t>(srcIdx));
    items_.insert(items_.begin() + targetIndex, std::move(tmp));
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
    if (version != kStoreVersion || count > kMaxItemCount) {
        return PreserveCorrupt();
    }
    pos = kHeaderSize;

    std::vector<Item> loaded;
    loaded.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        Item item;
        uint32_t kind = 0;
        uint32_t flags = 0;
        uint32_t dataLen = 0;
        if (!Take(buf, pos, item.id) || !Take(buf, pos, kind) || !Take(buf, pos, flags) ||
            !Take(buf, pos, item.createdAt) || !Take(buf, pos, item.usedAt) ||
            !Take(buf, pos, item.imgW) || !Take(buf, pos, item.imgH) ||
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
    Reorder();
    Evict();
    return LoadResult::Ok;
}

bool Store::Save() {
    ExpireCheck();
    std::vector<uint8_t> buf = Serialize();
    return util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
}

std::vector<uint8_t> Store::Serialize() {
    std::vector<uint8_t> buf;
    size_t estimate = kHeaderSize;
    for (const Item& item : items_) {
        estimate += 44 + item.data.size();
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
