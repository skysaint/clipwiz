// test_merge.cpp — guard rails for src/merge.cpp
//
// A merge is discovered late: the click produces a new entry that looks fine in
// the list, and the damage — two paths glued onto one line, a rich body dropped,
// an image flattened to junk — only surfaces at paste time, far from the cause.
// So every rule is pinned here headless, from both directions: CanMerge accepts
// exactly the mergeable sets and refuses the rest, the text join honors each
// separator, and the file-list join lands on one clean newline boundary.
#include <string>
#include <vector>

#include "merge.h"
#include "store.h"
#include "testfw.h"

namespace {

// ASCII bytes, no NUL terminator — for raw Html/Rtf/PNG payloads.
std::vector<uint8_t> Bytes(const char* s) {
    std::vector<uint8_t> out;
    for (const char* p = s; *p != '\0'; ++p) {
        out.push_back(static_cast<uint8_t>(*p));
    }
    return out;
}

// UTF-16LE bytes, exactly how Text / FileDrop payloads are stored.
std::vector<uint8_t> WideBytes(const std::wstring& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return std::vector<uint8_t>(p, p + s.size() * sizeof(wchar_t));
}

Item Text(const std::wstring& body) {
    Item it; it.kind = ItemKind::Text; it.data = WideBytes(body); return it;
}
Item Html(const std::vector<uint8_t>& raw) {
    Item it; it.kind = ItemKind::Html; it.data = raw; return it;
}
Item Rtf(const std::vector<uint8_t>& raw) {
    Item it; it.kind = ItemKind::Rtf; it.data = raw; return it;
}
Item Files(const std::wstring& paths) {
    Item it; it.kind = ItemKind::FileDrop; it.data = WideBytes(paths); return it;
}
Item Image() {
    Item it; it.kind = ItemKind::Image; it.data = Bytes("\x89PNG\x0d\x0a\x1a\n"); return it;
}

// Decode a merged Text/FileDrop payload back to the wstring it represents.
std::wstring Wide(const std::vector<uint8_t>& data) {
    return std::wstring(reinterpret_cast<const wchar_t*>(data.data()),
                        data.size() / sizeof(wchar_t));
}

// ---------------- CanMerge: what is allowed ----------------

void CanMergeNeedsTwoItems() {
    const Item a = Text(L"x");
    CWZ_CHECK(!merge::CanMerge({}));
    CWZ_CHECK(!merge::CanMerge({&a}));
}

void CanMergeAcceptsTwoTexts() {
    const Item a = Text(L"one");
    const Item b = Text(L"two");
    CWZ_CHECK(merge::CanMerge({&a, &b}));
}

void CanMergeAcceptsMixedTextKinds() {
    // Html and Rtf flatten to plain text, so they combine with Text and with
    // each other; only kind matters here, content is textconv's concern.
    const Item t = Text(L"a");
    const Item h = Html(Bytes("<body>b</body>"));
    const Item r = Rtf(Bytes("{\\rtf1 c}"));
    CWZ_CHECK(merge::CanMerge({&t, &h}));
    CWZ_CHECK(merge::CanMerge({&h, &r}));
    CWZ_CHECK(merge::CanMerge({&t, &h, &r}));
}

void CanMergeAcceptsTwoFileLists() {
    const Item a = Files(L"C:\\x.txt\n");
    const Item b = Files(L"C:\\y.txt\n");
    CWZ_CHECK(merge::CanMerge({&a, &b}));
}

// ---------------- CanMerge: what is refused ----------------

void CanMergeRejectsImages() {
    const Item img = Image();
    const Item img2 = Image();
    const Item t = Text(L"a");
    CWZ_CHECK(!merge::CanMerge({&img, &t}));    // image mixed with text
    CWZ_CHECK(!merge::CanMerge({&img, &img2})); // two images: no canvas compositing
}

void CanMergeRejectsFileTextMix() {
    const Item f = Files(L"C:\\x.txt\n");
    const Item t = Text(L"prose");
    CWZ_CHECK(!merge::CanMerge({&f, &t}));
    CWZ_CHECK(!merge::CanMerge({&t, &f}));
}

void CanMergeRejectsNull() {
    const Item a = Text(L"a");
    CWZ_CHECK(!merge::CanMerge({&a, nullptr}));
}

// ---------------- Merge: text ----------------

void MergeTextsWithBlankLineSeparator() {
    const Item a = Text(L"alpha");
    const Item b = Text(L"beta");
    ItemKind kind = ItemKind::Image;  // sentinel: Merge must overwrite it
    std::vector<uint8_t> data;
    const std::wstring sep = merge::SeparatorText(merge::Separator::BlankLine);
    CWZ_CHECK(merge::Merge({&a, &b}, sep, kind, data));
    CWZ_CHECK(kind == ItemKind::Text);
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"alpha\n\nbeta"));
}

void MergeTextsHonorsEachSeparator() {
    const Item a = Text(L"alpha");
    const Item b = Text(L"beta");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;

    CWZ_CHECK(merge::Merge({&a, &b}, L"\n", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"alpha\nbeta"));

    CWZ_CHECK(merge::Merge({&a, &b}, L" ", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"alpha beta"));

    CWZ_CHECK(merge::Merge({&a, &b}, L"", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"alphabeta"));

    CWZ_CHECK(merge::Merge({&a, &b}, L" | ", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"alpha | beta"));
}

void MergeFlattensHtmlToPlainText() {
    const Item a = Text(L"before");
    const Item b = Html(Bytes("<body>after</body>"));
    ItemKind kind = ItemKind::Rtf;  // sentinel
    std::vector<uint8_t> data;
    CWZ_CHECK(merge::Merge({&a, &b}, L"\n", kind, data));
    CWZ_CHECK(kind == ItemKind::Text);  // rich input, plain output
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"before\nafter"));
}

void MergeThreeTextsSeparatesEachPair() {
    const Item a = Text(L"1");
    const Item b = Text(L"2");
    const Item c = Text(L"3");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    CWZ_CHECK(merge::Merge({&a, &b, &c}, L"\n", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"1\n2\n3"));
}

void MergeTextOutputIsUtf16LeNoTerminator() {
    const Item a = Text(L"ab");
    const Item b = Text(L"cd");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    CWZ_CHECK(merge::Merge({&a, &b}, L"", kind, data));
    // "abcd" as UTF-16LE is 8 bytes, no trailing NUL — matches how clipboard
    // payloads are stored and what Store::TextOf reads back.
    CWZ_CHECK_EQ(data.size(), static_cast<size_t>(8));
}

// ---------------- Merge: file lists ----------------

void MergeFileListsJoinsOnSingleNewline() {
    // Each captured list ends every path with \n; the merge must not double it
    // (blank line) or drop it (glued paths), and the user separator is ignored.
    const Item a = Files(L"C:\\dir\\alpha.txt\n");
    const Item b = Files(L"C:\\dir\\beta.txt\n");
    ItemKind kind = ItemKind::Text;  // sentinel
    std::vector<uint8_t> data;
    CWZ_CHECK(merge::Merge({&a, &b}, L"\n\n", kind, data));
    CWZ_CHECK(kind == ItemKind::FileDrop);
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"C:\\dir\\alpha.txt\nC:\\dir\\beta.txt\n"));
}

void MergeFileListsNormalizesCrlf() {
    // A hand-built or \r\n list must still land on one clean \n boundary.
    const Item a = Files(L"C:\\alpha.txt\r\n");
    const Item b = Files(L"C:\\beta.txt\r\n");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    CWZ_CHECK(merge::Merge({&a, &b}, L"\n", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"C:\\alpha.txt\nC:\\beta.txt\n"));
}

void MergeFileListsHandlesMissingTrailingNewline() {
    const Item a = Files(L"C:\\alpha.txt");   // no trailing newline
    const Item b = Files(L"C:\\beta.txt\n");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    CWZ_CHECK(merge::Merge({&a, &b}, L"\n", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"C:\\alpha.txt\nC:\\beta.txt\n"));
}

void MergeFileListsKeepsInternalPaths() {
    // A multi-path body's internal newlines survive; only the boundary between
    // the two merged lists is normalized.
    const Item a = Files(L"C:\\a1.txt\nC:\\a2.txt\n");
    const Item b = Files(L"C:\\b1.txt\n");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    CWZ_CHECK(merge::Merge({&a, &b}, L"\n", kind, data));
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"C:\\a1.txt\nC:\\a2.txt\nC:\\b1.txt\n"));
}

void MergeFileListsRefusesWhenAllEmpty() {
    const Item a = Files(L"\n");
    const Item b = Files(L"");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    // No real path in either list -> nothing pasteable -> refuse.
    CWZ_CHECK(!merge::Merge({&a, &b}, L"\n", kind, data));
}

// ---------------- SeparatorText ----------------

void SeparatorTextMapsPresets() {
    CWZ_CHECK_EQ(merge::SeparatorText(merge::Separator::BlankLine), std::wstring(L"\n\n"));
    CWZ_CHECK_EQ(merge::SeparatorText(merge::Separator::Newline), std::wstring(L"\n"));
    CWZ_CHECK_EQ(merge::SeparatorText(merge::Separator::Space), std::wstring(L" "));
    CWZ_CHECK_EQ(merge::SeparatorText(merge::Separator::Custom, L"->"), std::wstring(L"->"));
    CWZ_CHECK_EQ(merge::SeparatorText(merge::Separator::Custom), std::wstring(L""));
}

// ---------------- Merge refuses exactly when CanMerge does ----------------

void MergeRefusesImagesAndLeavesOutputUntouched() {
    const Item img = Image();
    const Item t = Text(L"a");
    ItemKind kind = ItemKind::Text;  // sentinel
    std::vector<uint8_t> data = WideBytes(L"SENTINEL");
    CWZ_CHECK(!merge::Merge({&img, &t}, L"\n", kind, data));
    CWZ_CHECK(kind == ItemKind::Text);                     // unchanged
    CWZ_CHECK_EQ(Wide(data), std::wstring(L"SENTINEL"));   // unchanged
}

void MergeRefusesSingleItem() {
    const Item a = Text(L"lonely");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    CWZ_CHECK(!merge::Merge({&a}, L"\n", kind, data));
}

void MergeRefusesFileTextMix() {
    const Item f = Files(L"C:\\x.txt\n");
    const Item t = Text(L"prose");
    ItemKind kind = ItemKind::Text;
    std::vector<uint8_t> data;
    CWZ_CHECK(!merge::Merge({&f, &t}, L"\n", kind, data));
}

}  // namespace

void RunMergeTests() {
    CWZ_RUN(CanMergeNeedsTwoItems);
    CWZ_RUN(CanMergeAcceptsTwoTexts);
    CWZ_RUN(CanMergeAcceptsMixedTextKinds);
    CWZ_RUN(CanMergeAcceptsTwoFileLists);
    CWZ_RUN(CanMergeRejectsImages);
    CWZ_RUN(CanMergeRejectsFileTextMix);
    CWZ_RUN(CanMergeRejectsNull);

    CWZ_RUN(MergeTextsWithBlankLineSeparator);
    CWZ_RUN(MergeTextsHonorsEachSeparator);
    CWZ_RUN(MergeFlattensHtmlToPlainText);
    CWZ_RUN(MergeThreeTextsSeparatesEachPair);
    CWZ_RUN(MergeTextOutputIsUtf16LeNoTerminator);

    CWZ_RUN(MergeFileListsJoinsOnSingleNewline);
    CWZ_RUN(MergeFileListsNormalizesCrlf);
    CWZ_RUN(MergeFileListsHandlesMissingTrailingNewline);
    CWZ_RUN(MergeFileListsKeepsInternalPaths);
    CWZ_RUN(MergeFileListsRefusesWhenAllEmpty);

    CWZ_RUN(SeparatorTextMapsPresets);

    CWZ_RUN(MergeRefusesImagesAndLeavesOutputUntouched);
    CWZ_RUN(MergeRefusesSingleItem);
    CWZ_RUN(MergeRefusesFileTextMix);
}
