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

// 单条 data 的硬上限，防损坏文件把内存撑爆（64 MB）
constexpr uint32_t kMaxDataLen = 64u * 1024u * 1024u;
constexpr uint32_t kMaxItemCount = 100000u;

// FILETIME 一天的 tick 数
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

// 从 "HTML Format" 原始数据里提取纯文本首行（粗略：去标签）
std::wstring HtmlToPlainText(const std::vector<uint8_t>& data) {
    // HTML Format 是 UTF-8 文本
    std::string raw(reinterpret_cast<const char*>(data.data()), data.size());
    // 找 <html> 或 <body> 之后的内容
    size_t start = 0;
    size_t bodyPos = raw.find("<body");
    if (bodyPos == std::string::npos) {
        bodyPos = raw.find("<BODY");
    }
    if (bodyPos != std::string::npos) {
        size_t gt = raw.find('>', bodyPos);
        start = (gt != std::string::npos) ? gt + 1 : bodyPos;
    }
    // 去标签、解码基本实体
    std::wstring text;
    bool inTag = false;
    for (size_t i = start; i < raw.size() && text.size() < 500; ++i) {
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
            if (!text.empty() && text.back() != L' ') {
                text += L' ';
            }
            continue;
        }
        // 只处理 ASCII 范围，UTF-8 多字节先按单字节放（预览用，不求精确）
        text += static_cast<wchar_t>(static_cast<unsigned char>(ch));
    }
    return text;
}

// 从 RTF 原始数据里提取纯文本（粗略：去控制字和分组）
std::wstring RtfToPlainText(const std::vector<uint8_t>& data) {
    std::string raw(reinterpret_cast<const char*>(data.data()), data.size());
    std::wstring text;
    int depth = 0;
    bool skipDest = false;  // 跳过 \fonttbl \colortbl 等目标组
    size_t i = 0;
    while (i < raw.size() && text.size() < 500) {
        char ch = raw[i];
        if (ch == '{') {
            ++depth;
            ++i;
            // 看紧跟着是不是 \fonttbl \colortbl \stylesheet \pict 之类的
            if (i < raw.size() && raw[i] == '\\') {
                size_t wordStart = i + 1;
                size_t wordEnd = wordStart;
                while (wordEnd < raw.size() && isalpha(static_cast<unsigned char>(raw[wordEnd]))) {
                    ++wordEnd;
                }
                std::string word = raw.substr(wordStart, wordEnd - wordStart);
                if (word == "fonttbl" || word == "colortbl" || word == "stylesheet" ||
                    word == "pict" || word == "object" || word == "themedata" ||
                    word == "listtable" || word == "listoverridetable" || word == "rsidtbl" ||
                    word == "generator" || word == "info" || word == "latentstyles") {
                    skipDest = true;
                }
            }
            continue;
        }
        if (ch == '}') {
            if (depth > 0) {
                --depth;
            }
            skipDest = false;
            ++i;
            continue;
        }
        if (skipDest) {
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
                text += static_cast<wchar_t>(next);
                ++i;
                continue;
            }
            if (next == '\'') {
                // \'xx 十六进制字符
                ++i;
                if (i + 1 < raw.size()) {
                    char hex[3] = {raw[i], raw[i + 1], 0};
                    int val = strtol(hex, nullptr, 16);
                    if (val >= 0x20) {
                        text += static_cast<wchar_t>(val);
                    }
                    i += 2;
                }
                continue;
            }
            if (next == '\r' || next == '\n') {
                ++i;
                continue;
            }
            // 控制字
            size_t wordStart = i;
            while (i < raw.size() && isalpha(static_cast<unsigned char>(raw[i]))) {
                ++i;
            }
            std::string word = raw.substr(wordStart, i - wordStart);
            // 跳过可选的数字参数
            if (i < raw.size() && (raw[i] == '-' || isdigit(static_cast<unsigned char>(raw[i])))) {
                while (i < raw.size() &&
                       (raw[i] == '-' || isdigit(static_cast<unsigned char>(raw[i])))) {
                    ++i;
                }
            }
            // 控制字后的空格是分隔符，吃掉
            if (i < raw.size() && raw[i] == ' ') {
                ++i;
            }
            if (word == "par" || word == "line") {
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
        text += static_cast<wchar_t>(static_cast<unsigned char>(ch));
        ++i;
    }
    return text;
}

}  // namespace

// ------------------------------------------------------------------ 公共工具

std::wstring Store::TextOf(const Item& item) {
    switch (item.kind) {
        case ItemKind::Text:
        case ItemKind::FileDrop:
            // data 就是 UTF-16LE
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
            // 数行数
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
            // 只取文件名部分
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

// 置顶区保持手动顺序不动；非置顶区按 usedAt 降序
void Store::Reorder() {
    std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) {
        if (a.pinned != b.pinned) {
            return a.pinned;  // 置顶在前
        }
        if (a.pinned) {
            return false;  // 置顶区内部保持原有相对顺序（stable_sort 保证）
        }
        if (a.usedAt != b.usedAt) {
            return a.usedAt > b.usedAt;
        }
        return a.id > b.id;
    });
}

void Store::Evict() {
    // 条数上限只统计未置顶
    for (;;) {
        if (HistoryCount() <= maxHistory_) {
            break;
        }
        // 找 usedAt 最小的非置顶条目
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

    // 去重：同类型同内容 → 刷新 usedAt，提到非置顶区最前
    for (Item& item : items_) {
        if (item.kind == kind && item.hash == hash && item.data == data) {
            item.usedAt = now;
            if (!item.pinned) {
                Reorder();
            }
            return item.id;
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
        Reorder();  // 非置顶的刷新后提到前面
    }
    // 置顶的不动位置
    return true;
}

bool Store::MovePinned(uint64_t id, int delta) {
    // 找当前在置顶区里的下标
    int pinnedIdx = -1;
    int count = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        if (!items_[i].pinned) {
            break;  // 置顶区在前面
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
    // 在 items_ 里交换位置
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
    // 找到当前条目
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

// ------------------------------------------------------------------ 持久化

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
