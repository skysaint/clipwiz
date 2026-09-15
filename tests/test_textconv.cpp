// test_textconv.cpp — guard rails for src/textconv.cpp
//
// Every case here is pure data in / pure data out: no window, no clipboard,
// no config, no i18n. That is what makes this unit worth testing first — it is
// the code the store's dedup and preview paths both depend on, and a silent
// regression here shows up as "duplicates stopped merging" or "previews went
// blank", neither of which is obvious from the UI.
#include <cstring>
#include <string>
#include <vector>

#include "store.h"
#include "testfw.h"
#include "textconv.h"

namespace {

// ASCII bytes, no NUL terminator (callers add one when they want to test that).
std::vector<uint8_t> Bytes(const char* s) {
    const size_t n = std::strlen(s);
    std::vector<uint8_t> out(n);
    for (size_t i = 0; i < n; ++i) {
        out[i] = static_cast<uint8_t>(s[i]);
    }
    return out;
}

// UTF-16LE bytes, exactly how CF_UNICODETEXT / CF_HDROP payloads are stored.
std::vector<uint8_t> WideBytes(const std::wstring& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return std::vector<uint8_t>(p, p + s.size() * sizeof(wchar_t));
}

// ---------------------------------------------------------------- HTML

// A realistic CF_HTML blob: header block with byte offsets, then the fragment
// comment markers, then a full <html><body> document.
//
// Note what this actually pins down: HtmlToPlainText anchors on `<body`, NOT
// on the `StartFragment:`/`EndFragment:` header offsets. The offsets are never
// read, and the `<!--StartFragment-->` / `<!--EndFragment-->` comments vanish
// because they are tags. Anything outside <body> is dropped because scanning
// starts after the `<body>` opening tag.
const char kCfHtml[] =
    "Version:0.9\r\n"
    "StartHTML:00000105\r\n"
    "EndHTML:00000219\r\n"
    "StartFragment:00000141\r\n"
    "EndFragment:00000183\r\n"
    "SourceURL:https://example.invalid/\r\n"
    "<!--StartFragment--><html><body>Copy &amp; paste&nbsp;<b>now</b>"
    "</body></html><!--EndFragment-->";

void HtmlExtractsBodyAndDecodesEntities() {
    const std::wstring text = textconv::HtmlToPlainText(Bytes(kCfHtml));
    CWZ_CHECK_EQ(text, std::wstring(L"Copy & paste now"));
}

void HtmlStopsAtNulTerminator() {
    // Clipboard CF_HTML is NUL-terminated; garbage after the terminator must
    // not leak into the extracted text.
    std::vector<uint8_t> data = Bytes(kCfHtml);
    data.push_back(0);
    const std::vector<uint8_t> tail = Bytes("TRAILING<b>GARBAGE</b>");
    data.insert(data.end(), tail.begin(), tail.end());
    CWZ_CHECK_EQ(textconv::HtmlToPlainText(data), std::wstring(L"Copy & paste now"));
}

void HtmlHonorsMaxBytes() {
    // maxBytes caps the intermediate UTF-8 buffer, so the result is a prefix.
    CWZ_CHECK_EQ(textconv::HtmlToPlainText(Bytes(kCfHtml), 4), std::wstring(L"Copy"));
}

void HtmlDecodesNumericEntities() {
    const std::vector<uint8_t> data = Bytes("<body>&#20320;&#x597D;&mdash;&#8212;</body>");
    // 20320 = U+4F60, 0x597D = U+597D, mdash and #8212 are both U+2014.
    CWZ_CHECK_EQ(textconv::HtmlToPlainText(data),
                 std::wstring(L"\u4f60\u597d\u2014\u2014"));
}

void HtmlCollapsesNewlinesToSingleSpace() {
    CWZ_CHECK_EQ(textconv::HtmlToPlainText(Bytes("<body>a\nb</body>")),
                 std::wstring(L"a b"));
    CWZ_CHECK_EQ(textconv::HtmlToPlainText(Bytes("<body>a\r\n\r\nb</body>")),
                 std::wstring(L"a b"));
}

void HtmlFallsBackToWholeBufferWithoutBody() {
    // Fragment-only payloads (some apps) carry no <body>; scanning then starts
    // at offset 0 and tags are still stripped.
    CWZ_CHECK_EQ(textconv::HtmlToPlainText(Bytes("plain <b>text</b>")),
                 std::wstring(L"plain text"));
}

void HtmlEmptyInputYieldsEmptyText() {
    CWZ_CHECK(textconv::HtmlToPlainText(std::vector<uint8_t>()).empty());
}

// ---------------------------------------------------------------- RTF

void RtfHonorsAnsicpgForHexBytes() {
    // Same \'e9 byte, two declared code pages, two different characters.
    // CP1252 0xE9 = U+00E9 (e-acute); CP437 0xE9 = U+0398 (theta).
    // Both code pages ship with every Windows install, so this is deterministic.
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1\\ansicpg1252 Hello \\'e9 end}")),
                 std::wstring(L"Hello \u00e9 end"));
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1\\ansicpg437 Hello \\'e9 end}")),
                 std::wstring(L"Hello \u0398 end"));
}

void RtfDecodesMultibyteCodePageRuns() {
    // GBK: C4 E3 = U+4F60, BA C3 = U+597D. The two \'xx bytes must accumulate
    // into one run and be decoded together, not one byte at a time.
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1\\ansicpg936\\'c4\\'e3\\'ba\\'c3}")),
                 std::wstring(L"\u4f60\u597d"));
}

void RtfUnicodeEscapeSkipsUcFallbackChars() {
    // \uc1 -> one fallback char after each \uNNNN; \u-3 is signed 16-bit, so it
    // means U+FFFD (65533 = -3 + 65536).
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1\\uc1\\u20320?\\u-3? done}")),
                 std::wstring(L"\u4f60\ufffd done"));
    // \uc0 -> no fallback to skip; the escapes sit back to back.
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1\\uc0\\u20320\\u22909}")),
                 std::wstring(L"\u4f60\u597d"));
}

void RtfDropsControlCharEscapes() {
    // \uNNNN below 0x20 is a control character and must not reach the text.
    // Control-word parameters are decimal per the RTF spec, so U+0010 is
    // written \u10. A hex-looking "\u9a" would therefore correctly parse as
    // \u9 followed by a literal 'a' — not as one code point.
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1\\uc0\\u10}")), std::wstring());
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1\\uc1\\u10?x}")), std::wstring(L"x"));
}

void RtfSkipsNestedDestinations() {
    // {\fonttbl ... } contains two more nested groups and trailing junk; all of
    // it must vanish without desyncing the brace stack, and {\*\...} is an
    // ignorable destination regardless of its name.
    CWZ_CHECK_EQ(
        textconv::RtfToPlainText(Bytes("{\\rtf1\\ansi A{\\fonttbl{\\f0{\\f1 B;};}C}D"
                                       "{\\*\\unknown E}F}")),
        std::wstring(L"ADF"));
}

void RtfSkipsKnownDestinationTables() {
    CWZ_CHECK_EQ(
        textconv::RtfToPlainText(Bytes("{\\rtf1\\ansi keep{\\info{\\author Drop}}"
                                       "{\\colortbl ;\\red0;}{\\stylesheet{\\s1 Drop2}}"
                                       " tail}")),
        std::wstring(L"keep tail"));
}

void RtfMapsParagraphTabAndEscapes() {
    // \par and \line become a single space, \tab a real tab, and \\ \{ \} are
    // literal escapes. The delimiter space after each control word is eaten by
    // the parser, so "\par b" must not turn into two spaces.
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1 a\\par b\\tab c\\\\d\\{e\\}}")),
                 std::wstring(L"a b\tc\\d{e}"));
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1 a\\line b}")),
                 std::wstring(L"a b"));
}

void RtfPictureBecomesDimensionPlaceholder() {
    // Display goals win over native size; twips / 15 = pixels.
    CWZ_CHECK_EQ(
        textconv::RtfToPlainText(Bytes("{\\rtf1{\\pict\\picwgoal1500\\pichgoal750"
                                       "\\picw100\\pich50 x}after}")),
        std::wstring(L"[\u56fe\u7247 100\u00d750]after"));
    // No usable dimensions -> bare placeholder, still skipped as a destination.
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1{\\pict\\picw0\\pich0 x}after}")),
                 std::wstring(L"[\u56fe\u7247]after"));
}

void RtfHonorsMaxChars() {
    CWZ_CHECK_EQ(textconv::RtfToPlainText(Bytes("{\\rtf1 abcdefgh}"), 3),
                 std::wstring(L"abc"));
}

void RtfStopsAtNulTerminator() {
    std::vector<uint8_t> data = Bytes("{\\rtf1 real}");
    data.push_back(0);
    const std::vector<uint8_t> tail = Bytes("{\\rtf1 FAKE}");
    data.insert(data.end(), tail.begin(), tail.end());
    CWZ_CHECK_EQ(textconv::RtfToPlainText(data), std::wstring(L"real"));
}

void RtfEmptyInputYieldsEmptyText() {
    CWZ_CHECK(textconv::RtfToPlainText(std::vector<uint8_t>()).empty());
}

// ---------------------------------------------------------------- Canonical dedup

// Two byte-different RTF payloads that carry the same passage. This is the
// whole reason dedup hashes extracted text instead of raw bytes: editors stamp
// revision ids, font tables and author metadata that change on every copy.
const char kRtfHelloA[] =
    "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl{\\f0 Arial;}}\\pard Hello}";
const char kRtfHelloB[] =
    "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl{\\f1 Times New Roman;}{\\f2 Courier New;}}"
    "{\\*\\rsidtbl\\rsid1234\\rsid5678}{\\info{\\author Someone}}\\pard Hello}";

void CanonicalSameTextDifferentRtfBytesIsOneItem() {
    const std::vector<uint8_t> a = Bytes(kRtfHelloA);
    const std::vector<uint8_t> b = Bytes(kRtfHelloB);
    CWZ_CHECK(a != b);  // precondition: the raw bytes really do differ

    CWZ_CHECK_EQ(textconv::CanonicalHash(ItemKind::Rtf, a),
                 textconv::CanonicalHash(ItemKind::Rtf, b));

    Item existing;
    existing.kind = ItemKind::Rtf;
    existing.data = a;
    existing.hash = textconv::CanonicalHash(ItemKind::Rtf, a);
    CWZ_CHECK(textconv::SameCanonicalContent(existing, ItemKind::Rtf, b, existing.hash,
                                             /*incomingBodyEmpty=*/false));
    // Convenience overload recomputes the body itself; same verdict.
    CWZ_CHECK(textconv::SameCanonicalContent(existing, ItemKind::Rtf, b, existing.hash));
}

void CanonicalKindPrefixKeepsKindsApart() {
    const std::vector<uint8_t> hello = WideBytes(L"Hello");
    const uint64_t asText = textconv::CanonicalHash(ItemKind::Text, hello);
    const uint64_t asFile = textconv::CanonicalHash(ItemKind::FileDrop, hello);
    const uint64_t asHtml = textconv::CanonicalHash(ItemKind::Html, hello);
    // Identical bytes must still be three distinct entries: a plain "Hello", a
    // file list whose path happens to spell "Hello", and rich text.
    CWZ_CHECK(asText != asFile);
    CWZ_CHECK(asText != asHtml);
    CWZ_CHECK(asFile != asHtml);

    CWZ_CHECK_EQ(std::string(textconv::CanonicalPrefix(ItemKind::Text)), std::string("TXT:"));
    CWZ_CHECK_EQ(std::string(textconv::CanonicalPrefix(ItemKind::FileDrop)),
                 std::string("FILE:"));
    CWZ_CHECK_EQ(std::string(textconv::CanonicalPrefix(ItemKind::Html)), std::string("HTML:"));
    CWZ_CHECK_EQ(std::string(textconv::CanonicalPrefix(ItemKind::Rtf)), std::string("RTF:"));
    CWZ_CHECK_EQ(std::string(textconv::CanonicalPrefix(ItemKind::Image)), std::string("IMG:"));
}

void CanonicalImageBodyIsRawBytes() {
    const std::vector<uint8_t> png = {0x89, 0x50, 0x4e, 0x47, 0x01, 0x02};
    CWZ_CHECK_EQ(textconv::CanonicalBody(ItemKind::Image, png), png);
    CWZ_CHECK(textconv::CanonicalHash(ItemKind::Image, png) !=
              textconv::CanonicalHash(ItemKind::Text, png));
}

void CanonicalTextBodyIsUtf16Payload() {
    const std::vector<uint8_t> hello = WideBytes(L"Hello");
    CWZ_CHECK_EQ(textconv::CanonicalBody(ItemKind::Text, hello), hello);
}

// Formatting-only rich text extracts to nothing. Two such payloads hash
// identically (both are just the "RTF:" prefix), so the byte-equality fallback
// is the only thing standing between "correctly kept apart" and "wrongly
// merged, losing one of them forever".
const char kRtfEmptyX[] = "{\\rtf1\\ansi{\\fonttbl{\\f0 a;}}}";
const char kRtfEmptyY[] = "{\\rtf1\\ansi{\\fonttbl{\\f1 b;}}}";

void CanonicalEmptyBodyRequiresByteEquality() {
    const std::vector<uint8_t> x = Bytes(kRtfEmptyX);
    const std::vector<uint8_t> y = Bytes(kRtfEmptyY);
    CWZ_CHECK(textconv::CanonicalBody(ItemKind::Rtf, x).empty());
    CWZ_CHECK(textconv::CanonicalBody(ItemKind::Rtf, y).empty());

    const uint64_t h = textconv::CanonicalHash(ItemKind::Rtf, x);
    CWZ_CHECK_EQ(h, textconv::CanonicalHash(ItemKind::Rtf, y));  // same hash...

    Item existing;
    existing.kind = ItemKind::Rtf;
    existing.data = x;
    existing.hash = h;
    // ...but different bytes, so NOT the same content.
    CWZ_CHECK(!textconv::SameCanonicalContent(existing, ItemKind::Rtf, y, h,
                                              /*incomingBodyEmpty=*/true));
    CWZ_CHECK(!textconv::SameCanonicalContent(existing, ItemKind::Rtf, y, h));
    // Byte-identical is the one case that does merge.
    CWZ_CHECK(textconv::SameCanonicalContent(existing, ItemKind::Rtf, x, h,
                                             /*incomingBodyEmpty=*/true));
}

void CanonicalMismatchedKindOrHashNeverMatches() {
    const std::vector<uint8_t> a = Bytes(kRtfHelloA);
    const uint64_t h = textconv::CanonicalHash(ItemKind::Rtf, a);
    Item existing;
    existing.kind = ItemKind::Rtf;
    existing.data = a;
    existing.hash = h;
    // Same bytes, same hash, wrong kind -> reject.
    CWZ_CHECK(!textconv::SameCanonicalContent(existing, ItemKind::Html, a, h, false));
    // Right kind, wrong hash -> reject (no body comparison at all).
    CWZ_CHECK(!textconv::SameCanonicalContent(existing, ItemKind::Rtf, a, h + 1, false));
}

}  // namespace

void RunTextConvTests() {
    CWZ_RUN(HtmlExtractsBodyAndDecodesEntities);
    CWZ_RUN(HtmlStopsAtNulTerminator);
    CWZ_RUN(HtmlHonorsMaxBytes);
    CWZ_RUN(HtmlDecodesNumericEntities);
    CWZ_RUN(HtmlCollapsesNewlinesToSingleSpace);
    CWZ_RUN(HtmlFallsBackToWholeBufferWithoutBody);
    CWZ_RUN(HtmlEmptyInputYieldsEmptyText);

    CWZ_RUN(RtfHonorsAnsicpgForHexBytes);
    CWZ_RUN(RtfDecodesMultibyteCodePageRuns);
    CWZ_RUN(RtfUnicodeEscapeSkipsUcFallbackChars);
    CWZ_RUN(RtfDropsControlCharEscapes);
    CWZ_RUN(RtfSkipsNestedDestinations);
    CWZ_RUN(RtfSkipsKnownDestinationTables);
    CWZ_RUN(RtfMapsParagraphTabAndEscapes);
    CWZ_RUN(RtfPictureBecomesDimensionPlaceholder);
    CWZ_RUN(RtfHonorsMaxChars);
    CWZ_RUN(RtfStopsAtNulTerminator);
    CWZ_RUN(RtfEmptyInputYieldsEmptyText);

    CWZ_RUN(CanonicalSameTextDifferentRtfBytesIsOneItem);
    CWZ_RUN(CanonicalKindPrefixKeepsKindsApart);
    CWZ_RUN(CanonicalImageBodyIsRawBytes);
    CWZ_RUN(CanonicalTextBodyIsUtf16Payload);
    CWZ_RUN(CanonicalEmptyBodyRequiresByteEquality);
    CWZ_RUN(CanonicalMismatchedKindOrHashNeverMatches);
}
