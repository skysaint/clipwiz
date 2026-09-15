// test_store.cpp — guard rails for src/store.cpp
//
// store.dat is the only data clipwiz holds that a bug could destroy, so the two
// properties that matter most are covered hardest here:
//
//   1. Round-trip fidelity — the bytes Serialize() writes are exactly the bytes
//      Load() reads back.
//   2. Rejected files are preserved, never cleared — every rejection path must
//      rename the original aside byte-for-byte and report where it went.
//
// Note what is deliberately NOT covered: reading an older layout. Load() accepts
// exactly one version and treats anything else as damage, so the case below is
// OldVersionIsRejectedNotMigrated rather than a migration round trip.
//
// main() has already pointed util::DataDir() at a scratch directory, so no
// case below can reach the user's real store.
#include <windows.h>

#include <cstring>
#include <string>
#include <vector>

#include "store.h"
#include "testfw.h"
#include "util.h"

namespace {

// On-disk layout constants, restated independently of store.cpp on purpose: if
// the writer and the test both derived them from the same source, a change to
// the format would move the test along with the code and guard nothing.
constexpr size_t kHeaderSize = 32;
constexpr uint32_t kFlagPinned = 0x1;
constexpr uint32_t kUnpinnedOrderBase = 10001u;
constexpr uint32_t kMaxItemCount = 9999u;
constexpr uint32_t kMaxDataLen = 64u * 1024u * 1024u;
// The one version Load() accepts. Restated here rather than shared with
// store.cpp: a format change must break this test loudly instead of quietly
// dragging it along.
constexpr uint32_t kCurrentVersion = 3u;
constexpr uint32_t kMaxSourceAppChars = 1024u;

// Field offsets of the first item record, relative to the end of the header.
constexpr size_t kItemKindOffset = 8;
constexpr size_t kItemDataLenOffset = 44;

std::vector<uint8_t> Bytes(const char* s) {
    const size_t n = std::strlen(s);
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(s[i]);
    }
    return out;
}

std::vector<uint8_t> WideBytes(const std::wstring& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return std::vector<uint8_t>(p, p + s.size() * sizeof(wchar_t));
}

std::wstring Num(int i) {
    wchar_t buf[16] = {};
    swprintf_s(buf, L"%d", i);
    return buf;
}

void PutU32(std::vector<uint8_t>& b, uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

void PutU64(std::vector<uint8_t>& b, uint64_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + sizeof(v));
}

void PutU32At(std::vector<uint8_t>& b, size_t offset, uint32_t v) {
    memcpy(b.data() + offset, &v, sizeof(v));
}

struct ItemSpec {
    uint64_t id = 0;
    uint32_t kind = 0;
    bool pinned = false;
    uint64_t createdAt = 0;
    uint64_t usedAt = 0;
    uint32_t imgW = 0;
    uint32_t imgH = 0;
    uint32_t order = 0;
    std::vector<uint8_t> data;
    std::wstring sourceApp;
};

// Build a store.dat image by hand in the layout kCurrentVersion describes.
// `version` is written into the header verbatim so a case can lie about it.
std::vector<uint8_t> BuildStoreFile(uint32_t version, uint64_t nextId,
                                    const std::vector<ItemSpec>& items) {
    std::vector<uint8_t> b;
    b.push_back('C');
    b.push_back('L');
    b.push_back('P');
    b.push_back('W');
    PutU32(b, version);
    PutU32(b, static_cast<uint32_t>(items.size()));
    PutU64(b, nextId);
    b.resize(kHeaderSize, 0);  // reserved tail

    for (const ItemSpec& it : items) {
        PutU64(b, it.id);
        PutU32(b, it.kind);
        PutU32(b, it.pinned ? kFlagPinned : 0u);
        PutU64(b, it.createdAt);
        PutU64(b, it.usedAt);
        PutU32(b, it.imgW);
        PutU32(b, it.imgH);
        PutU32(b, it.order);
        PutU32(b, static_cast<uint32_t>(it.data.size()));
        b.insert(b.end(), it.data.begin(), it.data.end());
        const std::vector<uint8_t> app = WideBytes(it.sourceApp);
        PutU32(b, static_cast<uint32_t>(app.size()));
        b.insert(b.end(), app.begin(), app.end());
    }
    return b;
}

ItemSpec MakeTextItem(uint64_t id, const std::wstring& text, bool pinned, uint32_t order,
                      const std::wstring& sourceApp = {}) {
    ItemSpec s;
    s.id = id;
    s.kind = 0;  // ItemKind::Text
    s.pinned = pinned;
    s.createdAt = 133000000000000000ULL + id;
    s.usedAt = 133000000000000000ULL + id * 10;
    s.order = order;
    s.data = WideBytes(text);
    s.sourceApp = sourceApp;
    return s;
}

// Wipe store.dat and any leftover corrupt copies so each disk case starts from
// a known-empty scratch dir and produces exactly one backup of its own.
void ResetScratch() {
    DeleteFileW(util::StorePath().c_str());
    DeleteFileW((util::StorePath() + L".tmp").c_str());

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((util::DataDir() + L"\\store.corrupt.*.dat").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        DeleteFileW((util::DataDir() + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool WriteStoreFile(const std::vector<uint8_t>& buf) {
    return util::WriteFileAtomic(util::StorePath(), buf.data(), buf.size());
}

bool FileMissing(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES;
}

// Shared assertion block for every corrupt-input path: Load must report
// Corrupt, run with an empty database, move the original aside untouched, and
// say where it went.
void ExpectPreservedCorrupt(Store& s, const std::vector<uint8_t>& original) {
    CWZ_CHECK(s.Load() == Store::LoadResult::Corrupt);
    CWZ_CHECK_EQ(s.TotalCount(), 0);

    const std::wstring backup = s.CorruptBackupPath();
    CWZ_CHECK(!backup.empty());
    CWZ_CHECK(backup != util::StorePath());
    CWZ_CHECK(FileMissing(util::StorePath()));
    CWZ_CHECK(!FileMissing(backup));

    // The whole point: the damaged file is renamed, never overwritten, never
    // truncated. Whatever was in it is still recoverable by hand.
    std::vector<uint8_t> preserved;
    CWZ_CHECK(util::ReadWholeFile(backup, preserved));
    CWZ_CHECK_EQ(preserved, original);
}

// ---------------------------------------------------------------- round trip

void LoadMissingFileIsOkAndEmpty() {
    ResetScratch();
    Store s;
    CWZ_CHECK(s.Load() == Store::LoadResult::Ok);
    CWZ_CHECK_EQ(s.TotalCount(), 0);
    CWZ_CHECK(s.CorruptBackupPath().empty());
}

void RoundTripIsByteIdentical() {
    ResetScratch();
    const std::vector<ItemSpec> specs = {
        MakeTextItem(1, L"pinned entry", true, 1u, L"notepad.exe"),
        // Empty sourceApp is legal and common: the owner is not always
        // resolvable, and "unknown" must survive the round trip as unknown.
        MakeTextItem(2, L"unpinned entry", false, kUnpinnedOrderBase),
    };
    const std::vector<uint8_t> image = BuildStoreFile(kCurrentVersion, 3, specs);
    CWZ_CHECK(WriteStoreFile(image));

    Store s;
    CWZ_CHECK(s.Load() == Store::LoadResult::Ok);
    CWZ_CHECK_EQ(s.TotalCount(), 2);
    CWZ_CHECK_EQ(s.PinnedCount(), 1);
    CWZ_CHECK_EQ(s.Items()[0].id, static_cast<uint64_t>(1));
    CWZ_CHECK(s.Items()[0].pinned);
    CWZ_CHECK_EQ(s.Items()[1].id, static_cast<uint64_t>(2));
    CWZ_CHECK_EQ(Store::TextOf(s.Items()[1]), std::wstring(L"unpinned entry"));
    // Preview is derived data: recomputed on load, never read from the file.
    CWZ_CHECK_EQ(s.Items()[1].preview, std::wstring(L"unpinned entry"));
    // sourceApp is the opposite — a fact about the capture, read back verbatim.
    CWZ_CHECK_EQ(s.Items()[0].sourceApp, std::wstring(L"notepad.exe"));
    CWZ_CHECK(s.Items()[1].sourceApp.empty());

    // Already-normalized input must come back out unchanged, byte for byte.
    CWZ_CHECK_EQ(s.Serialize(), image);
}

void OldVersionIsRejectedNotMigrated() {
    // clipwiz reads exactly one on-disk layout. A file from an older build is
    // not upgraded, not partially parsed, not quietly dropped: it is renamed
    // aside byte-for-byte exactly like a damaged file, and the run starts
    // empty. This case pins that decision down — if a migration path is ever
    // added, it fails here and forces the choice to be made deliberately.
    ResetScratch();
    const std::vector<ItemSpec> specs = {
        MakeTextItem(1, L"perfectly good content", true, 1u, L"notepad.exe"),
    };
    const std::vector<uint8_t> old = BuildStoreFile(kCurrentVersion - 1u, 2, specs);
    CWZ_CHECK(WriteStoreFile(old));

    Store s;
    ExpectPreservedCorrupt(s, old);
}

void SaveThenLoadRoundTrips() {
    ResetScratch();
    Store s;
    s.SetLimits(50, 0);
    const uint64_t a = s.Add(ItemKind::Text, WideBytes(L"alpha"));
    const uint64_t b = s.Add(ItemKind::Text, WideBytes(L"beta"));
    const uint64_t c = s.Add(ItemKind::Image, Bytes("\x89PNG\x0d\x0a\x1a\nfakedata"), 32, 16);
    CWZ_CHECK(s.SetPinned(b, true));
    CWZ_CHECK(s.Save());

    Store t;
    t.SetLimits(50, 0);
    CWZ_CHECK(t.Load() == Store::LoadResult::Ok);
    CWZ_CHECK_EQ(t.Serialize(), s.Serialize());
    CWZ_CHECK_EQ(t.TotalCount(), 3);
    CWZ_CHECK(t.Find(a) != nullptr);
    CWZ_CHECK(t.Find(b) != nullptr);
    CWZ_CHECK(t.Find(c) != nullptr);
    const Item* img = t.Find(c);
    CWZ_CHECK(img != nullptr && img->kind == ItemKind::Image);
    CWZ_CHECK_EQ(img->imgW, static_cast<uint32_t>(32));
    CWZ_CHECK_EQ(img->imgH, static_cast<uint32_t>(16));
    // Image previews come from the localized format string, not from the data.
    // The "x" is split into its own literal: \x consumes hex digits greedily,
    // so "\x00d716" would be read as one out-of-range code point.
    CWZ_CHECK_EQ(img->preview, std::wstring(L"[Image 32" L"\x00d7" L"16]"));
    ResetScratch();
}

// ---------------------------------------------------------------- corrupt inputs

void CorruptBadMagicIsPreserved() {
    ResetScratch();
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 1, {});
    buf[0] = 'X';
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

void CorruptShortHeaderIsPreserved() {
    ResetScratch();
    // Magic and version only: 8 bytes, well under the 32-byte header.
    std::vector<uint8_t> buf = Bytes("CLPW");
    PutU32(buf, 2u);
    CWZ_CHECK(buf.size() < kHeaderSize);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

void CorruptTruncatedTailIsPreserved() {
    ResetScratch();
    std::vector<ItemSpec> specs = {
        MakeTextItem(1, L"pinned entry", true, 1u),
        MakeTextItem(2, L"unpinned entry", false, kUnpinnedOrderBase),
    };
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 3, specs);
    // Cut into the last item's payload: the declared dataLen no longer fits.
    buf.resize(buf.size() - 10);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

void CorruptUnknownVersionIsPreserved() {
    ResetScratch();
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 1, {});
    PutU32At(buf, 4, 99u);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

void CorruptItemCountOverCapIsPreserved() {
    ResetScratch();
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 1, {});
    PutU32At(buf, 8, kMaxItemCount + 1u);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

void CorruptUnknownItemKindIsPreserved() {
    ResetScratch();
    std::vector<ItemSpec> specs = {MakeTextItem(1, L"x", false, kUnpinnedOrderBase)};
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 2, specs);
    // ItemKind tops out at FileDrop (4).
    PutU32At(buf, kHeaderSize + kItemKindOffset, 9u);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

void CorruptOversizedDataLenIsPreserved() {
    ResetScratch();
    std::vector<ItemSpec> specs = {MakeTextItem(1, L"x", false, kUnpinnedOrderBase)};
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 2, specs);
    // A lying length field must be rejected before any allocation happens,
    // otherwise a 4-byte edit could make the process try to reserve 64 MB+.
    PutU32At(buf, kHeaderSize + kItemDataLenOffset, kMaxDataLen + 1u);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

// Offset of the first item's appLen prefix, given that item's payload size.
// 48 is the fixed part of a record (id/kind/flags/createdAt/usedAt/imgW/imgH/
// order/dataLen); the prefix sits immediately after the payload.
size_t FirstAppLenOffset(size_t dataLen) {
    return kHeaderSize + 48 + dataLen;
}

void CorruptOddSourceAppLenIsPreserved() {
    ResetScratch();
    const std::vector<ItemSpec> specs = {
        MakeTextItem(1, L"x", false, kUnpinnedOrderBase, L"notepad.exe"),
    };
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 2, specs);
    // sourceApp is UTF-16LE, so a byte count that is not a multiple of two
    // cannot describe any string. Reading it anyway would split a wchar_t down
    // the middle and hand back garbage that looks like a process name.
    PutU32At(buf, FirstAppLenOffset(specs[0].data.size()), 5u);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

void CorruptOversizedSourceAppLenIsPreserved() {
    ResetScratch();
    const std::vector<ItemSpec> specs = {MakeTextItem(1, L"x", false, kUnpinnedOrderBase)};
    std::vector<uint8_t> buf = BuildStoreFile(kCurrentVersion, 2, specs);
    PutU32At(buf, FirstAppLenOffset(specs[0].data.size()), (kMaxSourceAppChars + 1u) * 2u);
    CWZ_CHECK(WriteStoreFile(buf));
    Store s;
    ExpectPreservedCorrupt(s, buf);
}

// ---------------------------------------------------------------- dedup

const char kRtfHelloA[] =
    "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl{\\f0 Arial;}}\\pard Hello}";
const char kRtfHelloB[] =
    "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl{\\f1 Times New Roman;}{\\f2 Courier New;}}"
    "{\\*\\rsidtbl\\rsid1234\\rsid5678}{\\info{\\author Someone}}\\pard Hello}";
const char kRtfEmptyX[] = "{\\rtf1\\ansi{\\fonttbl{\\f0 a;}}}";
const char kRtfEmptyY[] = "{\\rtf1\\ansi{\\fonttbl{\\f1 b;}}}";

void AddRejectsEmptyData() {
    Store s;
    CWZ_CHECK_EQ(s.Add(ItemKind::Text, std::vector<uint8_t>()), static_cast<uint64_t>(0));
    CWZ_CHECK_EQ(s.TotalCount(), 0);
}

void AddDedupsIdenticalPlainText() {
    Store s;
    const uint64_t first = s.Add(ItemKind::Text, WideBytes(L"hello"));
    const uint64_t again = s.Add(ItemKind::Text, WideBytes(L"hello"));
    CWZ_CHECK(first != 0);
    CWZ_CHECK_EQ(first, again);
    CWZ_CHECK_EQ(s.TotalCount(), 1);
}

void AddDedupsRtfByExtractedTextNotRawBytes() {
    Store s;
    const uint64_t first = s.Add(ItemKind::Rtf, Bytes(kRtfHelloA));
    const uint64_t again = s.Add(ItemKind::Rtf, Bytes(kRtfHelloB));
    CWZ_CHECK_EQ(first, again);
    CWZ_CHECK_EQ(s.TotalCount(), 1);
    // On a dedup hit the stored bytes are refreshed to the newest copy: the
    // user may have re-edited the formatting and expects to paste that.
    const Item* it = s.Find(first);
    CWZ_CHECK(it != nullptr && it->data == Bytes(kRtfHelloB));
}

void AddKeepsTextAndRtfOfSameWordsApart() {
    Store s;
    CWZ_CHECK(s.Add(ItemKind::Text, WideBytes(L"Hello")) != 0);
    CWZ_CHECK(s.Add(ItemKind::Rtf, Bytes(kRtfHelloA)) != 0);
    // One carries formatting, one does not; they must stay two entries.
    CWZ_CHECK_EQ(s.TotalCount(), 2);
}

void AddKeepsEmptyBodyRichTextApart() {
    Store s;
    const uint64_t x = s.Add(ItemKind::Rtf, Bytes(kRtfEmptyX));
    const uint64_t y = s.Add(ItemKind::Rtf, Bytes(kRtfEmptyY));
    CWZ_CHECK(x != 0);
    CWZ_CHECK(y != 0);
    CWZ_CHECK(x != y);
    // Both extract to no text at all, so they share a canonical hash. Merging
    // them would silently delete one of the two.
    CWZ_CHECK_EQ(s.TotalCount(), 2);
}

void AddDedupsImageByRawBytesOnly() {
    Store s;
    const std::vector<uint8_t> png1 = Bytes("\x89PNG\x0d\x0a\x1a\nAAAA");
    const std::vector<uint8_t> png2 = Bytes("\x89PNG\x0d\x0a\x1a\nBBBB");
    const uint64_t a = s.Add(ItemKind::Image, png1);
    CWZ_CHECK_EQ(s.Add(ItemKind::Image, png1), a);
    CWZ_CHECK(s.Add(ItemKind::Image, png2) != a);
    CWZ_CHECK_EQ(s.TotalCount(), 2);
}

// ---------------------------------------------------------------- eviction

void EvictionRejectsNewContentWhenAllPinned() {
    Store s;
    s.SetLimits(5, 0);
    std::vector<uint64_t> ids;
    for (int i = 0; i < 5; ++i) {
        const uint64_t id = s.Add(ItemKind::Text, WideBytes(L"item" + Num(i)));
        CWZ_CHECK(id != 0);
        ids.push_back(id);
    }
    CWZ_CHECK_EQ(s.TotalCount(), 5);
    for (const uint64_t id : ids) {
        CWZ_CHECK(s.SetPinned(id, true));
    }
    CWZ_CHECK_EQ(s.PinnedCount(), 5);

    // The pinned invariant outranks capturing new content: nothing may be
    // evicted, so the new item is refused instead.
    CWZ_CHECK_EQ(s.Add(ItemKind::Text, WideBytes(L"one more")), static_cast<uint64_t>(0));
    CWZ_CHECK_EQ(s.TotalCount(), 5);
    CWZ_CHECK_EQ(s.PinnedCount(), 5);
}

void EvictionKeepsPinnedAndStaysAtCap() {
    Store s;
    s.SetLimits(5, 0);
    const uint64_t pinned = s.Add(ItemKind::Text, WideBytes(L"keep me"));
    CWZ_CHECK(s.SetPinned(pinned, true));
    for (int i = 0; i < 20; ++i) {
        s.Add(ItemKind::Text, WideBytes(L"filler" + Num(i)));
    }
    CWZ_CHECK_EQ(s.TotalCount(), 5);
    CWZ_CHECK_EQ(s.PinnedCount(), 1);
    CWZ_CHECK(s.Find(pinned) != nullptr);
    CWZ_CHECK(s.Items()[0].pinned);
}

void SetLimitsClampsToSupportedRange() {
    Store s;
    for (int i = 0; i < 10; ++i) {
        s.Add(ItemKind::Text, WideBytes(L"item" + Num(i)));
    }
    CWZ_CHECK_EQ(s.TotalCount(), 10);
    s.SetLimits(5, 0);  // shrink below current count -> evict unpinned
    CWZ_CHECK_EQ(s.TotalCount(), 5);
    s.SetLimits(1, 0);  // below the floor of 5 is clamped, not honored
    CWZ_CHECK_EQ(s.TotalCount(), 5);
}

void ExpiryNeverTouchesPinned() {
    Store s;
    s.SetLimits(50, 0);
    const uint64_t pinnedOld = s.Add(ItemKind::Text, WideBytes(L"pinned and ancient"));
    const uint64_t looseOld = s.Add(ItemKind::Text, WideBytes(L"unpinned and ancient"));
    CWZ_CHECK(s.SetPinned(pinnedOld, true));

    // Rebuild both entries on disk with an ancient usedAt so the 30-day cutoff
    // is unambiguous regardless of clock resolution. Only the pinned flag
    // differs between them, which is exactly what the rule keys on.
    std::vector<ItemSpec> specs;
    for (const Item& it : s.Items()) {
        ItemSpec sp;
        sp.id = it.id;
        sp.kind = static_cast<uint32_t>(it.kind);
        sp.pinned = it.pinned;
        sp.createdAt = it.createdAt;
        sp.usedAt = 1ULL;
        sp.imgW = it.imgW;
        sp.imgH = it.imgH;
        sp.order = it.order;
        sp.data = it.data;
        sp.sourceApp = it.sourceApp;
        specs.push_back(sp);
    }
    ResetScratch();
    CWZ_CHECK(WriteStoreFile(BuildStoreFile(kCurrentVersion, 100, specs)));

    Store t;
    CWZ_CHECK(t.Load() == Store::LoadResult::Ok);
    CWZ_CHECK_EQ(t.TotalCount(), 2);
    t.SetLimits(50, 30);  // SetLimits runs ExpireCheck
    CWZ_CHECK(t.Find(pinnedOld) != nullptr);   // pinned is exempt from expiry
    CWZ_CHECK(t.Find(looseOld) == nullptr);    // unpinned and ancient -> gone
    CWZ_CHECK_EQ(t.TotalCount(), 1);
    CWZ_CHECK_EQ(t.PinnedCount(), 1);
    ResetScratch();
}

// ---------------------------------------------------------------- ordering

void PromoteToFrontMovesUnpinnedOnly() {
    Store s;
    const uint64_t u1 = s.Add(ItemKind::Text, WideBytes(L"u1"));
    const uint64_t u2 = s.Add(ItemKind::Text, WideBytes(L"u2"));
    const uint64_t p1 = s.Add(ItemKind::Text, WideBytes(L"p1"));
    const uint64_t p2 = s.Add(ItemKind::Text, WideBytes(L"p2"));
    CWZ_CHECK(s.SetPinned(p2, true));
    CWZ_CHECK(s.SetPinned(p1, true));
    // Display order now: p1, p2 (pinned block), then u2, u1 (newest first).
    CWZ_CHECK_EQ(s.Items()[0].id, p1);
    CWZ_CHECK_EQ(s.Items()[1].id, p2);
    CWZ_CHECK_EQ(s.Items()[2].id, u2);
    CWZ_CHECK_EQ(s.Items()[3].id, u1);

    s.PromoteToFront(u1);
    CWZ_CHECK_EQ(s.Items()[0].id, p1);
    CWZ_CHECK_EQ(s.Items()[1].id, p2);
    CWZ_CHECK_EQ(s.Items()[2].id, u1);
    CWZ_CHECK_EQ(s.Items()[3].id, u2);

    // Pinned items are nailed down: promoting one must be a no-op.
    s.PromoteToFront(p2);
    CWZ_CHECK_EQ(s.Items()[0].id, p1);
    CWZ_CHECK_EQ(s.Items()[1].id, p2);
    CWZ_CHECK_EQ(s.Items()[2].id, u1);
}

void TouchPromotesUnpinnedToFront() {
    Store s;
    const uint64_t a = s.Add(ItemKind::Text, WideBytes(L"a"));
    const uint64_t b = s.Add(ItemKind::Text, WideBytes(L"b"));
    CWZ_CHECK_EQ(s.Items()[0].id, b);
    CWZ_CHECK(s.Touch(a));
    CWZ_CHECK_EQ(s.Items()[0].id, a);
    CWZ_CHECK_EQ(s.Items()[1].id, b);
    CWZ_CHECK(!s.Touch(999999));
}

void RemoveAndClearNonPinned() {
    Store s;
    const uint64_t keep = s.Add(ItemKind::Text, WideBytes(L"keep"));
    const uint64_t drop = s.Add(ItemKind::Text, WideBytes(L"drop"));
    CWZ_CHECK(s.SetPinned(keep, true));
    CWZ_CHECK_EQ(s.TotalCount(), 2);

    CWZ_CHECK(s.Remove(drop));
    CWZ_CHECK_EQ(s.TotalCount(), 1);
    CWZ_CHECK(!s.Remove(drop));
    CWZ_CHECK(!s.Remove(999999));

    const uint64_t other = s.Add(ItemKind::Text, WideBytes(L"other"));
    CWZ_CHECK_EQ(s.TotalCount(), 2);
    s.ClearNonPinned();
    CWZ_CHECK_EQ(s.TotalCount(), 1);
    CWZ_CHECK_EQ(s.PinnedCount(), 1);
    CWZ_CHECK(s.Find(keep) != nullptr);
    CWZ_CHECK(s.Find(other) == nullptr);
}

void PinnedReorderingMovesWithinPinnedBlockOnly() {
    Store s;
    const uint64_t a = s.Add(ItemKind::Text, WideBytes(L"a"));
    const uint64_t b = s.Add(ItemKind::Text, WideBytes(L"b"));
    const uint64_t c = s.Add(ItemKind::Text, WideBytes(L"c"));
    const uint64_t loose = s.Add(ItemKind::Text, WideBytes(L"loose"));
    for (const uint64_t id : {a, b, c}) {
        CWZ_CHECK(s.SetPinned(id, true));
    }
    // SetPinned moves to the head of the pinned block, so pinning a, b, c in
    // that order leaves them as c, b, a.
    CWZ_CHECK_EQ(s.Items()[0].id, c);
    CWZ_CHECK_EQ(s.Items()[1].id, b);
    CWZ_CHECK_EQ(s.Items()[2].id, a);
    CWZ_CHECK_EQ(s.Items()[3].id, loose);

    CWZ_CHECK(s.MovePinned(c, 1));  // c down one -> b, c, a
    CWZ_CHECK_EQ(s.Items()[0].id, b);
    CWZ_CHECK_EQ(s.Items()[1].id, c);
    CWZ_CHECK_EQ(s.Items()[2].id, a);
    CWZ_CHECK_EQ(s.Items()[3].id, loose);  // unpinned block untouched

    CWZ_CHECK(!s.MovePinned(c, 0));
    CWZ_CHECK(!s.MovePinned(loose, 1));       // not pinned
    CWZ_CHECK(!s.MovePinned(b, -99));         // out of range
    CWZ_CHECK(s.MovePinnedTo(b, 2));          // drag to end of pinned block
    CWZ_CHECK_EQ(s.Items()[0].id, c);
    CWZ_CHECK_EQ(s.Items()[1].id, a);
    CWZ_CHECK_EQ(s.Items()[2].id, b);
    CWZ_CHECK_EQ(s.Items()[3].id, loose);
}

// ---------------------------------------------------------------- plain-text conversion

void ConvertToPlainTextMergesIntoExistingTextEntry() {
    Store s;
    const uint64_t rich = s.Add(ItemKind::Rtf, Bytes(kRtfHelloA));
    const uint64_t plain = s.Add(ItemKind::Text, WideBytes(L"Hello"));
    CWZ_CHECK_EQ(s.TotalCount(), 2);

    // Converting makes the rich entry identical to the plain one, so the two
    // must merge; the earlier one in display order survives.
    const uint64_t survivor = s.ConvertToPlainText(rich);
    CWZ_CHECK_EQ(s.TotalCount(), 1);
    CWZ_CHECK_EQ(survivor, plain);
    const Item* it = s.Find(plain);
    CWZ_CHECK(it != nullptr && it->kind == ItemKind::Text);
    CWZ_CHECK(Store::TextOf(*it) == L"Hello");
}

void ConvertToPlainTextKeepsEntryWhenNoDuplicate() {
    Store s;
    const uint64_t rich = s.Add(ItemKind::Html, Bytes("<body>Unique words</body>"));
    const uint64_t survivor = s.ConvertToPlainText(rich);
    CWZ_CHECK_EQ(survivor, rich);
    CWZ_CHECK_EQ(s.TotalCount(), 1);
    const Item* it = s.Find(rich);
    CWZ_CHECK(it != nullptr && it->kind == ItemKind::Text);
    CWZ_CHECK(Store::TextOf(*it) == L"Unique words");
}

void ConvertToPlainTextIsNoOpForNonRichText() {
    Store s;
    const uint64_t text = s.Add(ItemKind::Text, WideBytes(L"already plain"));
    const uint64_t image = s.Add(ItemKind::Image, Bytes("\x89PNG\x0d\x0a\x1a\nx"));
    CWZ_CHECK_EQ(s.ConvertToPlainText(text), static_cast<uint64_t>(0));
    CWZ_CHECK_EQ(s.ConvertToPlainText(image), static_cast<uint64_t>(0));
    CWZ_CHECK_EQ(s.ConvertToPlainText(999999), static_cast<uint64_t>(0));
    CWZ_CHECK_EQ(s.TotalCount(), 2);
}

void ConvertToPlainTextKeepsBothWhenBothPinned() {
    Store s;
    const uint64_t rich = s.Add(ItemKind::Rtf, Bytes(kRtfHelloA));
    const uint64_t plain = s.Add(ItemKind::Text, WideBytes(L"Hello"));
    CWZ_CHECK(s.SetPinned(rich, true));
    CWZ_CHECK(s.SetPinned(plain, true));
    const uint64_t survivor = s.ConvertToPlainText(rich);
    // Pinned items are user-owned: a duplicate is tolerated rather than
    // auto-deleted, and the converted entry keeps its own id.
    CWZ_CHECK_EQ(survivor, rich);
    CWZ_CHECK_EQ(s.TotalCount(), 2);
    CWZ_CHECK_EQ(s.PinnedCount(), 2);
}

// ---------------------------------------------------------------- text and preview

void TextOfDecodesEveryKind() {
    Item text;
    text.kind = ItemKind::Text;
    text.data = WideBytes(L"plain");
    CWZ_CHECK_EQ(Store::TextOf(text), std::wstring(L"plain"));

    Item files;
    files.kind = ItemKind::FileDrop;
    files.data = WideBytes(L"C:\\dir\\alpha.txt\r\nC:\\dir\\beta.txt\r\n");
    CWZ_CHECK_EQ(Store::TextOf(files), std::wstring(L"C:\\dir\\alpha.txt\r\nC:\\dir\\beta.txt\r\n"));

    Item html;
    html.kind = ItemKind::Html;
    html.data = Bytes("<body>rich <i>text</i></body>");
    CWZ_CHECK_EQ(Store::TextOf(html), std::wstring(L"rich text"));

    Item rtf;
    rtf.kind = ItemKind::Rtf;
    rtf.data = Bytes("{\\rtf1 word}");
    CWZ_CHECK_EQ(Store::TextOf(rtf), std::wstring(L"word"));

    Item image;
    image.kind = ItemKind::Image;
    image.data = Bytes("\x89PNG\x0d\x0a\x1a\n");
    CWZ_CHECK(Store::TextOf(image).empty());
}

void PreviewSingleFileDropIsJustTheFileName() {
    Item it;
    it.kind = ItemKind::FileDrop;
    it.data = WideBytes(L"C:\\some\\long\\path\\alpha.txt\r\n");
    CWZ_CHECK_EQ(MakeItemPreview(it), std::wstring(L"alpha.txt"));
}

void PreviewMultiFileDropShowsCountAndFirstName() {
    Item it;
    it.kind = ItemKind::FileDrop;
    it.data = WideBytes(L"C:\\dir\\alpha.txt\r\nC:\\dir\\beta.txt\r\nD:\\gamma.txt\r\n");
    // Built-in English "%d files: %s" (verified in test_main.cpp).
    CWZ_CHECK_EQ(MakeItemPreview(it), std::wstring(L"3 files: alpha.txt"));
}

void PreviewImageShowsDimensions() {
    Item it;
    it.kind = ItemKind::Image;
    it.imgW = 1920;
    it.imgH = 1080;
    it.data = Bytes("\x89PNG\x0d\x0a\x1a\n");
    CWZ_CHECK_EQ(MakeItemPreview(it), std::wstring(L"[Image 1920" L"\x00d7" L"1080]"));
}

void PreviewEmptyContentIsLocalizedPlaceholder() {
    Item it;
    it.kind = ItemKind::Text;
    CWZ_CHECK_EQ(MakeItemPreview(it), std::wstring(L"[Empty content]"));
}

void PreviewIsOneLineAndBounded() {
    std::wstring longText;
    for (int i = 0; i < 60; ++i) {
        longText += L"0123456789\n";
    }
    Item it;
    it.kind = ItemKind::Text;
    it.data = WideBytes(longText);
    const std::wstring preview = MakeItemPreview(it);
    // OneLinePreview(text, 160) caps at 160 chars plus the ellipsis, and turns
    // every newline into a collapsed space so a row never wraps.
    CWZ_CHECK_EQ(preview.size(), static_cast<size_t>(161));
    CWZ_CHECK_EQ(preview.back(), static_cast<wchar_t>(0x2026));
    CWZ_CHECK(preview.find(L'\n') == std::wstring::npos);
    CWZ_CHECK(preview.find(L'\r') == std::wstring::npos);
}

void PreviewOfRichTextComesFromExtractedBody() {
    Item it;
    it.kind = ItemKind::Rtf;
    it.data = Bytes(kRtfHelloA);
    CWZ_CHECK_EQ(MakeItemPreview(it), std::wstring(L"Hello"));
}

// ---------------------------------------------------------------- search text

// The character budget MakeItemSearchText puts on rich content, restated here
// instead of read from store.cpp — for the same reason the layout constants at
// the top are restated. A test that imports the value it is checking cannot
// fail when that value changes.
constexpr size_t kExpectedSearchTextCap = 4096;

bool ContainsBytes(const std::vector<uint8_t>& haystack, const std::vector<uint8_t>& needle) {
    if (needle.empty() || needle.size() > haystack.size()) {
        return false;
    }
    const size_t last = haystack.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        if (memcmp(haystack.data() + i, needle.data(), needle.size()) == 0) {
            return true;
        }
    }
    return false;
}

void SearchTextIsLowercased() {
    Item it;
    it.kind = ItemKind::Text;
    it.data = WideBytes(L"MiXeD CaSe TEXT");
    CWZ_CHECK_EQ(MakeItemSearchText(it), std::wstring(L"mixed case text"));
}

void SearchTextKeepsContentPastThePreviewCut() {
    // The defect this field exists to fix: preview is OneLinePreview(160), so
    // anything past that cut was invisible to the filter box.
    std::wstring text(500, L'x');
    text += L"NEEDLE-AT-500";
    Item it;
    it.kind = ItemKind::Text;
    it.data = WideBytes(text);

    const std::wstring preview = MakeItemPreview(it);
    CWZ_CHECK(preview.find(L"NEEDLE") == std::wstring::npos);
    CWZ_CHECK(preview.find(L"needle") == std::wstring::npos);

    const std::wstring search = MakeItemSearchText(it);
    CWZ_CHECK(search.find(L"needle-at-500") != std::wstring::npos);
    // Full content, not a summary: nothing is elided and no newline is folded.
    CWZ_CHECK_EQ(search.size(), text.size());
}

void SearchTextOfFileDropCoversEveryPath() {
    // A multi-file preview reads "N files: <first name>", so the other paths
    // used to be unsearchable no matter how short the list was.
    Item it;
    it.kind = ItemKind::FileDrop;
    it.data = WideBytes(L"C:\\dir\\alpha.txt\r\nC:\\dir\\beta.txt\r\nD:\\deep\\gamma.md\r\n");

    const std::wstring preview = MakeItemPreview(it);
    CWZ_CHECK(preview.find(L"beta") == std::wstring::npos);
    CWZ_CHECK(preview.find(L"gamma") == std::wstring::npos);

    const std::wstring search = MakeItemSearchText(it);
    CWZ_CHECK(search.find(L"alpha.txt") != std::wstring::npos);
    CWZ_CHECK(search.find(L"beta.txt") != std::wstring::npos);
    CWZ_CHECK(search.find(L"gamma.md") != std::wstring::npos);
    // Whole paths, not just file names: searching by folder has to work too.
    CWZ_CHECK(search.find(L"c:\\dir") != std::wstring::npos);
    CWZ_CHECK(search.find(L"d:\\deep") != std::wstring::npos);
}

void SearchTextOfImageIsItsPreview() {
    // An image carries no text of its own. The localized "[Image W×H]" summary
    // is the only thing a user could type to find one, so that becomes the
    // search text — lowercased like everything else. The multiplication sign
    // is split into its own literal because \x eats hex digits greedily.
    Item it;
    it.kind = ItemKind::Image;
    it.imgW = 1920;
    it.imgH = 1080;
    it.data = Bytes("\x89PNG\x0d\x0a\x1a\nfakedata");
    CWZ_CHECK_EQ(MakeItemPreview(it), std::wstring(L"[Image 1920" L"\x00d7" L"1080]"));
    CWZ_CHECK_EQ(MakeItemSearchText(it), std::wstring(L"[image 1920" L"\x00d7" L"1080]"));
}

void SearchTextOfRichTextIsExtractedBody() {
    Item it;
    it.kind = ItemKind::Rtf;
    it.data = Bytes(kRtfHelloA);
    // Markup and font tables are not content; searching for "Arial" or "rtf1"
    // must not match an entry whose visible text is "Hello".
    const std::wstring search = MakeItemSearchText(it);
    CWZ_CHECK_EQ(search, std::wstring(L"hello"));
}

void SearchTextOfRichTextIsCapped() {
    // The cap is a latency guard: one multi-megabyte RTF must not turn a
    // single Add() into a full-document parse. Html counts its budget in UTF-8
    // bytes, so both rich kinds are checked to land on the same character count.
    std::string rtf = "{\\rtf1\\ansi\\pard ";
    rtf.append(kExpectedSearchTextCap * 4, 'a');
    rtf += "}";
    Item rtfItem;
    rtfItem.kind = ItemKind::Rtf;
    rtfItem.data = Bytes(rtf.c_str());
    CWZ_CHECK_EQ(MakeItemSearchText(rtfItem).size(), kExpectedSearchTextCap);

    std::string html = "<body>";
    html.append(kExpectedSearchTextCap * 4, 'b');
    html += "</body>";
    Item htmlItem;
    htmlItem.kind = ItemKind::Html;
    htmlItem.data = Bytes(html.c_str());
    CWZ_CHECK_EQ(MakeItemSearchText(htmlItem).size(), kExpectedSearchTextCap);

    // Under the cap nothing is trimmed.
    Item shortItem;
    shortItem.kind = ItemKind::Rtf;
    shortItem.data = Bytes("{\\rtf1 short}");
    CWZ_CHECK_EQ(MakeItemSearchText(shortItem), std::wstring(L"short"));
}

void SearchTextOfEmptyContentIsEmpty() {
    // MakeItemPreview substitutes a localized "[Empty content]" placeholder for
    // display. searchText must not: the placeholder is chrome, not content, and
    // making it searchable would let the word "empty" match every blank row.
    Item it;
    it.kind = ItemKind::Text;
    CWZ_CHECK_EQ(MakeItemPreview(it), std::wstring(L"[Empty content]"));
    CWZ_CHECK(MakeItemSearchText(it).empty());
}

void SearchTextIsNotPersisted() {
    // Derived data does not go to disk. Proof by content: build an entry whose
    // searchText cannot occur in its own data, then show the serialized bytes
    // hold no trace of it — in either encoding.
    ResetScratch();
    Store s;
    s.SetLimits(50, 0);
    const uint64_t id = s.Add(ItemKind::Html, Bytes("<body>Mixed CASE Words</body>"));
    const Item* it = s.Find(id);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK_EQ(it->searchText, std::wstring(L"mixed case words"));

    const std::vector<uint8_t> blob = s.Serialize();
    CWZ_CHECK(!ContainsBytes(blob, WideBytes(L"mixed case words")));
    CWZ_CHECK(!ContainsBytes(blob, Bytes("mixed case words")));
    // The payload itself is untouched — nothing was rewritten on the way out.
    CWZ_CHECK(ContainsBytes(blob, Bytes("<body>Mixed CASE Words</body>")));
    ResetScratch();
}

void LoadRepopulatesSearchText() {
    // store.dat has never carried searchText, not even after this change. Load
    // must rebuild it, or opening an existing history would leave every row
    // unsearchable until the next capture.
    ResetScratch();
    Store s;
    s.SetLimits(50, 0);
    const uint64_t textId = s.Add(ItemKind::Text, WideBytes(L"Findable Content"));
    const uint64_t imgId = s.Add(ItemKind::Image, Bytes("\x89PNG\x0d\x0a\x1a\nfakedata"), 32, 16);
    CWZ_CHECK(s.Save());

    Store t;
    t.SetLimits(50, 0);
    CWZ_CHECK(t.Load() == Store::LoadResult::Ok);
    const Item* text = t.Find(textId);
    const Item* img = t.Find(imgId);
    CWZ_CHECK(text != nullptr);
    CWZ_CHECK(img != nullptr);
    if (text == nullptr || img == nullptr) return;

    CWZ_CHECK_EQ(text->searchText, std::wstring(L"findable content"));
    CWZ_CHECK_EQ(img->searchText, std::wstring(L"[image 32" L"\x00d7" L"16]"));
    // preview keeps its display case while searchText is folded; both come from
    // the same pass, so neither can be left describing older bytes.
    CWZ_CHECK_EQ(text->preview, std::wstring(L"Findable Content"));
    CWZ_CHECK_EQ(img->preview, std::wstring(L"[Image 32" L"\x00d7" L"16]"));
    ResetScratch();
}

void AddDedupHitRefreshesSearchText() {
    // On a dedup hit the stored bytes are replaced with the newest copy even
    // though no new row appears, so both derived fields have to be recomputed.
    Store s;
    s.SetLimits(50, 0);
    const uint64_t first = s.Add(ItemKind::Rtf, Bytes(kRtfHelloA));
    const uint64_t second = s.Add(ItemKind::Rtf, Bytes(kRtfHelloB));
    CWZ_CHECK_EQ(second, first);
    CWZ_CHECK_EQ(s.TotalCount(), 1);
    const Item* it = s.Find(first);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK_EQ(it->data, Bytes(kRtfHelloB));
    CWZ_CHECK_EQ(it->preview, std::wstring(L"Hello"));
    CWZ_CHECK_EQ(it->searchText, std::wstring(L"hello"));
}

void ConvertToPlainTextRefreshesSearchText() {
    // ConvertToPlainText rewrites kind/data/hash in place. Recomputing only the
    // preview would leave searchText describing markup that is no longer there.
    Store s;
    s.SetLimits(50, 0);
    const uint64_t id = s.Add(ItemKind::Rtf, Bytes(kRtfHelloA));
    CWZ_CHECK_EQ(s.ConvertToPlainText(id), id);
    const Item* it = s.Find(id);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK(it->kind == ItemKind::Text);
    CWZ_CHECK_EQ(it->preview, std::wstring(L"Hello"));
    CWZ_CHECK_EQ(it->searchText, std::wstring(L"hello"));
}

void RefreshPreviewsRebuildsSearchText() {
    // Called after a language switch. An image's searchText comes from its
    // localized preview, so a rebuild that touched only the preview would leave
    // the two disagreeing about what the same row says.
    Store s;
    s.SetLimits(50, 0);
    const uint64_t id = s.Add(ItemKind::Image, Bytes("\x89PNG\x0d\x0a\x1a\nfakedata"), 32, 16);
    s.RefreshPreviews();
    const Item* it = s.Find(id);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK_EQ(it->preview, std::wstring(L"[Image 32" L"\x00d7" L"16]"));
    CWZ_CHECK_EQ(it->searchText, std::wstring(L"[image 32" L"\x00d7" L"16]"));
}

// ---------------------------------------------------------------- source app

void AddRecordsSourceApp() {
    Store s;
    s.SetLimits(50, 0);
    const uint64_t id = s.Add(ItemKind::Text, WideBytes(L"hello"), 0, 0, L"notepad.exe");
    const Item* it = s.Find(id);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK_EQ(it->sourceApp, std::wstring(L"notepad.exe"));
    // The source is metadata, not content. It must not make an entry findable
    // by a word that appears only in the process name.
    CWZ_CHECK(it->searchText.find(L"notepad") == std::wstring::npos);
}

void AddDefaultsToUnknownSourceApp() {
    // Not every capture can name its origin. "Unknown" is stored as empty,
    // never as a placeholder string that would then be displayed and matched.
    Store s;
    s.SetLimits(50, 0);
    const uint64_t id = s.Add(ItemKind::Text, WideBytes(L"hello"));
    const Item* it = s.Find(id);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK(it->sourceApp.empty());
}

void AddDedupHitRefreshesSourceApp() {
    // Re-copying the same passage from a different program replaces the stored
    // bytes and promotes the entry. The recorded origin has to follow, or the
    // row would keep naming a program that has nothing to do with the content
    // now sitting in it.
    Store s;
    s.SetLimits(50, 0);
    const uint64_t first = s.Add(ItemKind::Text, WideBytes(L"same words"), 0, 0, L"notepad.exe");
    const uint64_t second = s.Add(ItemKind::Text, WideBytes(L"same words"), 0, 0, L"code.exe");
    CWZ_CHECK_EQ(second, first);
    CWZ_CHECK_EQ(s.TotalCount(), 1);
    const Item* it = s.Find(first);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK_EQ(it->sourceApp, std::wstring(L"code.exe"));
}

void SourceAppSurvivesSaveAndLoad() {
    ResetScratch();
    Store s;
    s.SetLimits(50, 0);
    const uint64_t id = s.Add(ItemKind::Text, WideBytes(L"persisted"), 0, 0, L"winword.exe");
    CWZ_CHECK(s.Save());

    Store t;
    t.SetLimits(50, 0);
    CWZ_CHECK(t.Load() == Store::LoadResult::Ok);
    const Item* it = t.Find(id);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    // Unlike preview and searchText this comes back off the disk rather than
    // being recomputed: the process that made the copy is gone by reload time,
    // so the file is the only place the answer still exists.
    CWZ_CHECK_EQ(it->sourceApp, std::wstring(L"winword.exe"));
    ResetScratch();
}

void ConvertToPlainTextKeepsSourceApp() {
    // The conversion rewrites kind/data/hash in place. Where the copy came from
    // did not change — same program, same passage, plainer formatting — so the
    // field must be left alone.
    Store s;
    s.SetLimits(50, 0);
    const uint64_t id = s.Add(ItemKind::Rtf, Bytes(kRtfHelloA), 0, 0, L"winword.exe");
    CWZ_CHECK_EQ(s.ConvertToPlainText(id), id);
    const Item* it = s.Find(id);
    CWZ_CHECK(it != nullptr);
    if (it == nullptr) return;
    CWZ_CHECK(it->kind == ItemKind::Text);
    CWZ_CHECK_EQ(it->sourceApp, std::wstring(L"winword.exe"));
}

void TotalDataSizeSumsPayloads() {
    Store s;
    s.Add(ItemKind::Text, WideBytes(L"abcd"));               // 8 bytes
    s.Add(ItemKind::Image, Bytes("\x89PNG\x0d\x0a\x1a\n"));  // 8 bytes
    CWZ_CHECK_EQ(s.TotalDataSize(), static_cast<uint64_t>(16));
}

// ---------------------------------------------------------------- import merge

void ImportMergeBringsInEveryItem() {
    // A backup round-trips through ImportMerge into an empty store: every entry
    // lands with its kind, payload, dimensions and sourceApp intact, and the
    // derived searchText is rebuilt exactly as a live capture would build it.
    Store src;
    src.SetLimits(50, 0);
    src.Add(ItemKind::Text, WideBytes(L"alpha"), 0, 0, L"notepad.exe");
    src.Add(ItemKind::Image, Bytes("\x89PNG\x0d\x0a\x1a\nfakedata"), 32, 16);

    Store dst;
    dst.SetLimits(50, 0);
    CWZ_CHECK_EQ(dst.ImportMerge(src.Serialize()), 2);
    CWZ_CHECK_EQ(dst.TotalCount(), 2);

    bool sawText = false, sawImage = false;
    for (const Item& it : dst.Items()) {
        if (it.kind == ItemKind::Text) {
            sawText = Store::TextOf(it) == L"alpha" && it.sourceApp == L"notepad.exe" &&
                      it.searchText == L"alpha";
        } else if (it.kind == ItemKind::Image) {
            sawImage = it.imgW == 32 && it.imgH == 16;
        }
    }
    CWZ_CHECK(sawText);
    CWZ_CHECK(sawImage);
}

void ImportedPinnedItemsArriveUnpinned() {
    // ImportMerge is a content merge, not a state restore. Entries come back as
    // fresh unpinned items even when they were pinned in the backup: pinning is
    // a decision about the live store, and letting a file silently populate the
    // never-evicted pinned section would be a way to wedge the store full.
    Store src;
    src.SetLimits(50, 0);
    const uint64_t pid = src.Add(ItemKind::Text, WideBytes(L"favorite"));
    CWZ_CHECK(src.SetPinned(pid, true));
    CWZ_CHECK_EQ(src.PinnedCount(), 1);

    Store dst;
    dst.SetLimits(50, 0);
    CWZ_CHECK_EQ(dst.ImportMerge(src.Serialize()), 1);
    CWZ_CHECK_EQ(dst.TotalCount(), 1);
    CWZ_CHECK_EQ(dst.PinnedCount(), 0);
    CWZ_CHECK(!dst.Items()[0].pinned);
}

void ImportMergeDedupsAgainstExisting() {
    // Content already in the live store is not duplicated by the import; new
    // content alongside it is added. That is the "merge" in import-merge, and it
    // falls out of routing every entry through Add().
    Store src;
    src.SetLimits(50, 0);
    src.Add(ItemKind::Text, WideBytes(L"shared"));
    src.Add(ItemKind::Text, WideBytes(L"fresh"));

    Store dst;
    dst.SetLimits(50, 0);
    dst.Add(ItemKind::Text, WideBytes(L"shared"));
    CWZ_CHECK_EQ(dst.TotalCount(), 1);

    // Both backup rows are accounted for (the dup folds onto the existing row,
    // the new one is added); the store ends with two distinct rows, not three.
    CWZ_CHECK_EQ(dst.ImportMerge(src.Serialize()), 2);
    CWZ_CHECK_EQ(dst.TotalCount(), 2);
}

void ImportMergeKeepsExistingPinnedIntact() {
    // Because an import only ever calls Add(), it cannot disturb what is already
    // pinned: the pinned row survives, still pinned, beside the merged content.
    Store src;
    src.SetLimits(50, 0);
    src.Add(ItemKind::Text, WideBytes(L"incoming"));

    Store dst;
    dst.SetLimits(50, 0);
    const uint64_t pin = dst.Add(ItemKind::Text, WideBytes(L"keep me"));
    CWZ_CHECK(dst.SetPinned(pin, true));

    CWZ_CHECK_EQ(dst.ImportMerge(src.Serialize()), 1);
    CWZ_CHECK_EQ(dst.TotalCount(), 2);
    CWZ_CHECK_EQ(dst.PinnedCount(), 1);
    const Item* kept = dst.Find(pin);
    CWZ_CHECK(kept != nullptr && kept->pinned);
}

void ImportMergeRejectsCorruptLeavesStoreUntouched() {
    // The durability rule for import, mirror of PreserveCorrupt's attitude but
    // without the rename: a backup we cannot read changes NOTHING. The live
    // store re-serializes byte-for-byte identical afterwards — no clear, no
    // partial merge. (Nothing is set aside either: this is a file the user
    // pointed us at, not our own store.dat, so renaming it would be rude.)
    Store dst;
    dst.SetLimits(50, 0);
    dst.Add(ItemKind::Text, WideBytes(L"precious"));
    const std::vector<uint8_t> before = dst.Serialize();

    std::vector<uint8_t> badMagic = before;
    badMagic[0] = 'X';
    CWZ_CHECK_EQ(dst.ImportMerge(badMagic), -1);
    CWZ_CHECK_EQ(dst.Serialize(), before);

    std::vector<uint8_t> truncated(before.begin(), before.begin() + (before.size() / 2));
    CWZ_CHECK_EQ(dst.ImportMerge(truncated), -1);
    CWZ_CHECK_EQ(dst.Serialize(), before);
    CWZ_CHECK_EQ(dst.TotalCount(), 1);
}

void ImportMergeRejectsWrongVersion() {
    // A backup from a different layout version is refused, not migrated — the
    // same single-version rule Load() holds (see OldVersionIsRejectedNotMigrated)
    // applied to imports, and the store is left untouched.
    Store dst;
    dst.SetLimits(50, 0);
    dst.Add(ItemKind::Text, WideBytes(L"existing"));
    const std::vector<uint8_t> before = dst.Serialize();

    const std::vector<ItemSpec> specs = {
        MakeTextItem(1, L"old layout content", false, kUnpinnedOrderBase),
    };
    const std::vector<uint8_t> oldVersion = BuildStoreFile(kCurrentVersion - 1u, 2, specs);
    CWZ_CHECK_EQ(dst.ImportMerge(oldVersion), -1);
    CWZ_CHECK_EQ(dst.Serialize(), before);
    CWZ_CHECK_EQ(dst.TotalCount(), 1);
}

void ImportMergeEmptyBackupIsNoOp() {
    // A valid backup holding zero items merges nothing and changes nothing.
    Store dst;
    dst.SetLimits(50, 0);
    dst.Add(ItemKind::Text, WideBytes(L"stays"));
    const std::vector<uint8_t> before = dst.Serialize();

    Store emptySrc;
    emptySrc.SetLimits(50, 0);
    CWZ_CHECK_EQ(dst.ImportMerge(emptySrc.Serialize()), 0);
    CWZ_CHECK_EQ(dst.TotalCount(), 1);
    CWZ_CHECK_EQ(dst.Serialize(), before);
}

void ImportMergeObeysCapacityLimit() {
    // Import obeys the same total cap as a fresh copy: a backup larger than the
    // target's limit evicts the oldest unpinned down to the cap, so restoring a
    // big backup into a small store can never blow past the configured maximum.
    Store src;
    src.SetLimits(50, 0);
    for (int i = 0; i < 60; ++i) {
        src.Add(ItemKind::Text, WideBytes(L"item " + Num(i)));
    }
    const int srcCount = src.TotalCount();  // 50 after its own eviction

    Store dst;
    dst.SetLimits(10, 0);
    dst.ImportMerge(src.Serialize());
    CWZ_CHECK(dst.TotalCount() < srcCount);
    CWZ_CHECK(dst.TotalCount() <= 10);
}

}  // namespace

void RunStoreTests() {
    CWZ_RUN(LoadMissingFileIsOkAndEmpty);
    CWZ_RUN(RoundTripIsByteIdentical);
    CWZ_RUN(OldVersionIsRejectedNotMigrated);
    CWZ_RUN(SaveThenLoadRoundTrips);

    CWZ_RUN(CorruptBadMagicIsPreserved);
    CWZ_RUN(CorruptShortHeaderIsPreserved);
    CWZ_RUN(CorruptTruncatedTailIsPreserved);
    CWZ_RUN(CorruptUnknownVersionIsPreserved);
    CWZ_RUN(CorruptItemCountOverCapIsPreserved);
    CWZ_RUN(CorruptUnknownItemKindIsPreserved);
    CWZ_RUN(CorruptOversizedDataLenIsPreserved);
    CWZ_RUN(CorruptOddSourceAppLenIsPreserved);
    CWZ_RUN(CorruptOversizedSourceAppLenIsPreserved);

    CWZ_RUN(AddRejectsEmptyData);
    CWZ_RUN(AddDedupsIdenticalPlainText);
    CWZ_RUN(AddDedupsRtfByExtractedTextNotRawBytes);
    CWZ_RUN(AddKeepsTextAndRtfOfSameWordsApart);
    CWZ_RUN(AddKeepsEmptyBodyRichTextApart);
    CWZ_RUN(AddDedupsImageByRawBytesOnly);

    CWZ_RUN(EvictionRejectsNewContentWhenAllPinned);
    CWZ_RUN(EvictionKeepsPinnedAndStaysAtCap);
    CWZ_RUN(SetLimitsClampsToSupportedRange);
    CWZ_RUN(ExpiryNeverTouchesPinned);

    CWZ_RUN(PromoteToFrontMovesUnpinnedOnly);
    CWZ_RUN(TouchPromotesUnpinnedToFront);
    CWZ_RUN(RemoveAndClearNonPinned);
    CWZ_RUN(PinnedReorderingMovesWithinPinnedBlockOnly);

    CWZ_RUN(ConvertToPlainTextMergesIntoExistingTextEntry);
    CWZ_RUN(ConvertToPlainTextKeepsEntryWhenNoDuplicate);
    CWZ_RUN(ConvertToPlainTextIsNoOpForNonRichText);
    CWZ_RUN(ConvertToPlainTextKeepsBothWhenBothPinned);

    CWZ_RUN(TextOfDecodesEveryKind);
    CWZ_RUN(PreviewSingleFileDropIsJustTheFileName);
    CWZ_RUN(PreviewMultiFileDropShowsCountAndFirstName);
    CWZ_RUN(PreviewImageShowsDimensions);
    CWZ_RUN(PreviewEmptyContentIsLocalizedPlaceholder);
    CWZ_RUN(PreviewIsOneLineAndBounded);
    CWZ_RUN(PreviewOfRichTextComesFromExtractedBody);

    CWZ_RUN(SearchTextIsLowercased);
    CWZ_RUN(SearchTextKeepsContentPastThePreviewCut);
    CWZ_RUN(SearchTextOfFileDropCoversEveryPath);
    CWZ_RUN(SearchTextOfImageIsItsPreview);
    CWZ_RUN(SearchTextOfRichTextIsExtractedBody);
    CWZ_RUN(SearchTextOfRichTextIsCapped);
    CWZ_RUN(SearchTextOfEmptyContentIsEmpty);
    CWZ_RUN(SearchTextIsNotPersisted);
    CWZ_RUN(LoadRepopulatesSearchText);
    CWZ_RUN(AddDedupHitRefreshesSearchText);
    CWZ_RUN(ConvertToPlainTextRefreshesSearchText);
    CWZ_RUN(RefreshPreviewsRebuildsSearchText);

    CWZ_RUN(AddRecordsSourceApp);
    CWZ_RUN(AddDefaultsToUnknownSourceApp);
    CWZ_RUN(AddDedupHitRefreshesSourceApp);
    CWZ_RUN(SourceAppSurvivesSaveAndLoad);
    CWZ_RUN(ConvertToPlainTextKeepsSourceApp);

    CWZ_RUN(TotalDataSizeSumsPayloads);

    CWZ_RUN(ImportMergeBringsInEveryItem);
    CWZ_RUN(ImportedPinnedItemsArriveUnpinned);
    CWZ_RUN(ImportMergeDedupsAgainstExisting);
    CWZ_RUN(ImportMergeKeepsExistingPinnedIntact);
    CWZ_RUN(ImportMergeRejectsCorruptLeavesStoreUntouched);
    CWZ_RUN(ImportMergeRejectsWrongVersion);
    CWZ_RUN(ImportMergeEmptyBackupIsNoOp);
    CWZ_RUN(ImportMergeObeysCapacityLimit);
}
