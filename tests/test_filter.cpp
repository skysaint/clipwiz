// test_filter.cpp — guard rails for src/filter.cpp
//
// clipwiz has no tags, no groups and no folders, so the filter box is the only
// way to organize anything. These cases pin down two behaviours that are easy
// to get subtly wrong and painful when wrong:
//
//   1. Token parsing stays lenient. "app:设计" and "https://example.com" are
//      ordinary searches, not queries, and must not lose a word to a colon.
//   2. Matching is AND over keywords, so narrowing down never re-widens.
//
// Everything here runs headless: no window, no clipboard, no disk. The one
// case that builds a real Store only calls Add(), which stays in memory.
#include <cstring>
#include <string>
#include <vector>

#include "filter.h"
#include "store.h"
#include "testfw.h"

namespace {

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

// Flatten the keyword list so a mismatch prints the actual words instead of
// "<value>". One case is about which words survive parsing, so this is the
// part of the result worth seeing.
std::wstring Keywords(const filter::Query& q) {
    std::wstring out;
    for (const std::wstring& kw : q.keywords) {
        if (!out.empty()) {
            out += L"|";
        }
        out += kw;
    }
    return out;
}

// ------------------------------------------------------------- Parse: basics

void ParseEmptyFilterIsEmptyQuery() {
    const filter::Query q = filter::Parse(L"");
    CWZ_CHECK(q.Empty());
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK(q.app.empty());
    CWZ_CHECK(q.keywords.empty());
}

void ParseWhitespaceOnlyIsEmptyQuery() {
    // A box containing just spaces must behave like an empty box, or the popup
    // filters everything out the moment the user hits space by accident.
    const filter::Query q = filter::Parse(L"   \t  ");
    CWZ_CHECK(q.Empty());
    CWZ_CHECK(q.keywords.empty());
}

void ParseSingleWordIsOneKeyword() {
    const filter::Query q = filter::Parse(L"hello");
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"hello"));
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK(q.app.empty());
}

void ParseSplitsOnWhitespace() {
    // Tabs and newlines count as separators too: the filter box is a
    // single-line edit control, but pasted text can still carry them.
    const filter::Query q = filter::Parse(L"  alpha\tbeta\r\n gamma  ");
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"alpha|beta|gamma"));
}

void ParseLowercasesKeywords() {
    // Lowercasing happens once at parse time so Matches() can compare against
    // the already-lowercase searchText with no per-item allocation.
    const filter::Query q = filter::Parse(L"MiXeD CaSe");
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"mixed|case"));
}

// --------------------------------------------------------------- Parse: kind

void ParseKindTokenAtStart() {
    const filter::Query text = filter::Parse(L"kind:text");
    CWZ_CHECK(text.kind == filter::Kind::Text);
    CWZ_CHECK(text.keywords.empty());
    CWZ_CHECK(!text.Empty());  // a type constraint alone is a real constraint

    CWZ_CHECK(filter::Parse(L"kind:rich").kind == filter::Kind::Rich);
    CWZ_CHECK(filter::Parse(L"kind:image").kind == filter::Kind::Image);
    CWZ_CHECK(filter::Parse(L"kind:file").kind == filter::Kind::File);
}

void ParseKindTokenIsCaseInsensitive() {
    CWZ_CHECK(filter::Parse(L"KIND:TEXT").kind == filter::Kind::Text);
    CWZ_CHECK(filter::Parse(L"Kind:Image").kind == filter::Kind::Image);
}

void ParseKindTokenWithKeywords() {
    const filter::Query q = filter::Parse(L"kind:rich invoice 2024");
    CWZ_CHECK(q.kind == filter::Kind::Rich);
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"invoice|2024"));
}

void ParseUnknownKindValueStaysAKeyword() {
    // A typo must be visible. Swallowing "kind:pdf" into a no-op constraint
    // would show the user the full list and let them believe the filter worked.
    const filter::Query q = filter::Parse(L"kind:pdf report");
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"kind:pdf|report"));
}

void ParseBareKindTokenStaysAKeyword() {
    const filter::Query q = filter::Parse(L"kind:");
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"kind:"));
}

void ParseKindTokenOnlyCountsAtStart() {
    // "kind:" mid-query is text someone is looking for, not a command.
    const filter::Query q = filter::Parse(L"report kind:text");
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"report|kind:text"));
}

void ParseScanningStopsAtFirstNonToken() {
    // Once a word fails to parse as a token, nothing after it is a token
    // either. Otherwise "foo app:bar" would quietly gain an app constraint
    // the user never asked for.
    const filter::Query q = filter::Parse(L"kind:text foo app:bar");
    CWZ_CHECK(q.kind == filter::Kind::Text);
    CWZ_CHECK(q.app.empty());
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"foo|app:bar"));
}

// ---------------------------------------------------------------- Parse: app

void ParseAppTokenAtStart() {
    const filter::Query q = filter::Parse(L"app:notepad");
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK_EQ(q.app, std::wstring(L"notepad"));
    CWZ_CHECK(q.keywords.empty());
    CWZ_CHECK(!q.Empty());
}

void ParseAppTokenLowercasesValue() {
    // Process names arrive in whatever case the OS reports; the stored
    // sourceApp is lowercased to match.
    CWZ_CHECK_EQ(filter::Parse(L"app:Notepad++").app, std::wstring(L"notepad++"));
}

void ParseAppAndKindTokensTogether() {
    const filter::Query q = filter::Parse(L"kind:file app:explorer download zip");
    CWZ_CHECK(q.kind == filter::Kind::File);
    CWZ_CHECK_EQ(q.app, std::wstring(L"explorer"));
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"download|zip"));
}

void ParseBareAppTokenStaysAKeyword() {
    const filter::Query q = filter::Parse(L"app:");
    CWZ_CHECK(q.app.empty());
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"app:"));
}

void ParseNonAsciiAppValueIsStillAToken() {
    // At the very start of the query "app:微信" is a real app constraint, and
    // rejecting it because the value is not ASCII would lock out exactly the
    // users who name their windows in Chinese. The leniency that protects the
    // same text as an ordinary search is positional, not a charset rule — see
    // ParseTokenWordsMidQueryStayKeywords.
    const filter::Query q = filter::Parse(L"app:设计");
    CWZ_CHECK_EQ(q.app, std::wstring(L"设计"));
    CWZ_CHECK(q.keywords.empty());
}

void ParseTokenWordsMidQueryStayKeywords() {
    // This is what "lenient" buys: someone searching their history for the
    // literal text "app:设计" types it after a word, and both halves must
    // remain searchable instead of one being eaten as a command.
    const filter::Query q = filter::Parse(L"规范 app:设计");
    CWZ_CHECK(q.app.empty());
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"规范|app:设计"));
}

// ---------------------------------------------------- Parse: colons in text

void ParseColonInsideWordIsNotAToken() {
    // No known prefix, so the whole thing is one keyword — including the
    // slashes and the colon. Splitting URLs would break the most common
    // thing people paste into a search box.
    const filter::Query q = filter::Parse(L"https://example.com/path");
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"https://example.com/path"));
    CWZ_CHECK(q.app.empty());
    CWZ_CHECK(q.kind == filter::Kind::Any);
}

void ParseTrailingColonWordIsKept() {
    const filter::Query q = filter::Parse(L"note: remember this");
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"note:|remember|this"));
}

void ParseFullWidthColonIsNotAToken() {
    // U+FF1A is what a Chinese IME produces. It is not ASCII ':' so no prefix
    // can match, and the word stays searchable.
    const filter::Query q = filter::Parse(L"kind：text");
    CWZ_CHECK(q.kind == filter::Kind::Any);
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"kind：text"));
}

// -------------------------------------------------------------- Matches: kind

bool MatchesKind(filter::Kind k, ItemKind item) {
    filter::Query q;
    q.kind = k;
    return filter::Matches(q, item, L"", L"");
}

void MatchesKindTextSelectsOnlyText() {
    CWZ_CHECK(MatchesKind(filter::Kind::Text, ItemKind::Text));
    CWZ_CHECK(!MatchesKind(filter::Kind::Text, ItemKind::Html));
    CWZ_CHECK(!MatchesKind(filter::Kind::Text, ItemKind::Rtf));
    CWZ_CHECK(!MatchesKind(filter::Kind::Text, ItemKind::Image));
    CWZ_CHECK(!MatchesKind(filter::Kind::Text, ItemKind::FileDrop));
}

void MatchesKindRichCoversHtmlAndRtf() {
    // "rich" is deliberately both: from the user's side there is one idea of
    // "formatted text", and which of the two clipwiz stored is an accident of
    // what the source application put on the clipboard.
    CWZ_CHECK(MatchesKind(filter::Kind::Rich, ItemKind::Html));
    CWZ_CHECK(MatchesKind(filter::Kind::Rich, ItemKind::Rtf));
    CWZ_CHECK(!MatchesKind(filter::Kind::Rich, ItemKind::Text));
    CWZ_CHECK(!MatchesKind(filter::Kind::Rich, ItemKind::Image));
    CWZ_CHECK(!MatchesKind(filter::Kind::Rich, ItemKind::FileDrop));
}

void MatchesKindImageAndFile() {
    CWZ_CHECK(MatchesKind(filter::Kind::Image, ItemKind::Image));
    CWZ_CHECK(!MatchesKind(filter::Kind::Image, ItemKind::Text));
    CWZ_CHECK(MatchesKind(filter::Kind::File, ItemKind::FileDrop));
    CWZ_CHECK(!MatchesKind(filter::Kind::File, ItemKind::Image));
}

void MatchesKindAnyAcceptsEveryKind() {
    CWZ_CHECK(MatchesKind(filter::Kind::Any, ItemKind::Text));
    CWZ_CHECK(MatchesKind(filter::Kind::Any, ItemKind::Image));
    CWZ_CHECK(MatchesKind(filter::Kind::Any, ItemKind::Html));
    CWZ_CHECK(MatchesKind(filter::Kind::Any, ItemKind::Rtf));
    CWZ_CHECK(MatchesKind(filter::Kind::Any, ItemKind::FileDrop));
}

// ---------------------------------------------------------- Matches: keywords

void MatchesEmptyQueryAcceptsEverything() {
    const filter::Query q = filter::Parse(L"");
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"anything at all", L""));
    CWZ_CHECK(filter::Matches(q, ItemKind::Image, L"", L""));
}

void MatchesSingleKeywordIsSubstring() {
    const filter::Query q = filter::Parse(L"invo");
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"quarterly invoice total", L""));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"quarterly total", L""));
}

void MatchesAllKeywordsMustBePresent() {
    // AND, not OR. Narrowing a search must never bring rows back.
    const filter::Query q = filter::Parse(L"alpha beta");
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"alpha then beta", L""));
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"beta first, alpha later", L""));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"only alpha here", L""));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"only beta here", L""));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"neither word", L""));
}

void MatchesKeywordMaySpanWholeContentNotJustFirstLine() {
    // The bug this whole batch exists to fix: matching used to run against the
    // 160-char one-line preview, so anything past the cut was unfindable.
    std::wstring longText(400, L'x');
    longText += L"needle-at-the-end";
    const filter::Query q = filter::Parse(L"needle-at-the-end");
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, longText, L""));
}

void MatchesEveryPathInAFileListIsFindable() {
    // searchText for FileDrop is the full path list, so the third file is as
    // searchable as the first. Previously only the first name reached preview.
    const std::wstring paths =
        L"c:\\docs\\report.txt\nc:\\docs\\budget.xlsx\nc:\\docs\\notes.md";
    CWZ_CHECK(filter::Matches(filter::Parse(L"report"), ItemKind::FileDrop, paths, L""));
    CWZ_CHECK(filter::Matches(filter::Parse(L"budget"), ItemKind::FileDrop, paths, L""));
    CWZ_CHECK(filter::Matches(filter::Parse(L"notes.md"), ItemKind::FileDrop, paths, L""));
    CWZ_CHECK(filter::Matches(filter::Parse(L"docs budget"), ItemKind::FileDrop, paths, L""));
    CWZ_CHECK(!filter::Matches(filter::Parse(L"missing"), ItemKind::FileDrop, paths, L""));
}

void MatchesKindAndKeywordsCombine() {
    const filter::Query q = filter::Parse(L"kind:image screenshot");
    CWZ_CHECK(filter::Matches(q, ItemKind::Image, L"[image 1920×1080] screenshot", L""));
    // Right words, wrong type.
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"took a screenshot", L""));
}

// -------------------------------------------------------------- Matches: app

void MatchesAppIsSubstringOfSourceApp() {
    const filter::Query q = filter::Parse(L"app:notepad");
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"", L"notepad.exe"));
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"", L"c:\\windows\\notepad.exe"));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"", L"wordpad.exe"));
}

void MatchesAppTokenRejectsItemsWithoutSourceApp() {
    // Items captured before source tracking existed have no sourceApp. An
    // honest "no match" beats claiming they came from the requested app.
    const filter::Query q = filter::Parse(L"app:notepad");
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"anything", L""));
}

void MatchesWithoutAppTokenIgnoresSourceApp() {
    const filter::Query q = filter::Parse(L"hello");
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"hello", L""));
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"hello", L"chrome.exe"));
}

void MatchesAppAndKeywordsCombine() {
    const filter::Query q = filter::Parse(L"kind:text app:code todo");
    CWZ_CHECK(filter::Matches(q, ItemKind::Text, L"todo list", L"code.exe"));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"todo list", L"notepad.exe"));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Html, L"todo list", L"code.exe"));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"nothing", L"code.exe"));
}

// ----------------------------------------------------- End-to-end through both

void ParseThenMatchesIsTheRealPath() {
    // popup.cpp calls exactly these two functions in this order, so exercise
    // the pair rather than only Matches() with hand-built Query structs.
    const filter::Query q = filter::Parse(L"KIND:Rich  Invoice  Q3 ");
    CWZ_CHECK(q.kind == filter::Kind::Rich);
    CWZ_CHECK_EQ(Keywords(q), std::wstring(L"invoice|q3"));

    CWZ_CHECK(filter::Matches(q, ItemKind::Html, L"q3 invoice summary", L""));
    CWZ_CHECK(filter::Matches(q, ItemKind::Rtf, L"invoice for q3", L""));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Text, L"invoice for q3", L""));
    CWZ_CHECK(!filter::Matches(q, ItemKind::Rtf, L"invoice for q2", L""));
}

// ------------------------------------------- searchText produced by a real Store

void MatchesAgainstSearchTextARealStoreProduces() {
    // popup.cpp calls Matches(query, item.kind, item.searchText, ...). Walking
    // the whole chain keeps the two halves from drifting apart: MakeItemSearchText
    // decides what is searchable, Matches decides whether the query hits it.
    // Add() is in-memory only, so this touches no disk.
    Store s;
    s.SetLimits(50, 0);

    std::wstring longText(400, L'x');
    longText += L"Contract 2024";
    const uint64_t textId = s.Add(ItemKind::Text, WideBytes(longText));
    const uint64_t fileId = s.Add(
        ItemKind::FileDrop,
        WideBytes(L"C:\\a\\first.docx\r\nC:\\a\\second.docx\r\nC:\\a\\third.pdf\r\n"));
    const uint64_t imgId = s.Add(ItemKind::Image, Bytes("\x89PNG\x0d\x0a\x1a\nfake"), 800, 600);
    CWZ_CHECK_EQ(s.TotalCount(), 3);

    auto Hits = [&](const filter::Query& q, uint64_t id) {
        const Item* it = s.Find(id);
        if (it == nullptr) {
            return false;
        }
        return filter::Matches(q, it->kind, it->searchText, std::wstring());
    };

    // Two keywords, both past the 160-char preview cut.
    const filter::Query deep = filter::Parse(L"contract 2024");
    CWZ_CHECK(Hits(deep, textId));
    CWZ_CHECK(!Hits(deep, fileId));

    // The third path in a file list — the preview only ever named the first.
    CWZ_CHECK(Hits(filter::Parse(L"third.pdf"), fileId));
    CWZ_CHECK(!Hits(filter::Parse(L"third.pdf"), textId));

    // Type narrowing stacked on content.
    CWZ_CHECK(Hits(filter::Parse(L"kind:file docx"), fileId));
    CWZ_CHECK(!Hits(filter::Parse(L"kind:file docx"), textId));

    // An image is findable by the dimensions in its summary.
    CWZ_CHECK(Hits(filter::Parse(L"800"), imgId));
    CWZ_CHECK(Hits(filter::Parse(L"kind:image"), imgId));
    CWZ_CHECK(!Hits(filter::Parse(L"kind:image"), fileId));

    // Nothing here has a recorded source app, so app: must match nothing.
    CWZ_CHECK(!Hits(filter::Parse(L"app:explorer"), fileId));
}

}  // namespace

void RunFilterTests() {
    CWZ_RUN(ParseEmptyFilterIsEmptyQuery);
    CWZ_RUN(ParseWhitespaceOnlyIsEmptyQuery);
    CWZ_RUN(ParseSingleWordIsOneKeyword);
    CWZ_RUN(ParseSplitsOnWhitespace);
    CWZ_RUN(ParseLowercasesKeywords);

    CWZ_RUN(ParseKindTokenAtStart);
    CWZ_RUN(ParseKindTokenIsCaseInsensitive);
    CWZ_RUN(ParseKindTokenWithKeywords);
    CWZ_RUN(ParseUnknownKindValueStaysAKeyword);
    CWZ_RUN(ParseBareKindTokenStaysAKeyword);
    CWZ_RUN(ParseKindTokenOnlyCountsAtStart);
    CWZ_RUN(ParseScanningStopsAtFirstNonToken);

    CWZ_RUN(ParseAppTokenAtStart);
    CWZ_RUN(ParseAppTokenLowercasesValue);
    CWZ_RUN(ParseAppAndKindTokensTogether);
    CWZ_RUN(ParseBareAppTokenStaysAKeyword);
    CWZ_RUN(ParseNonAsciiAppValueIsStillAToken);
    CWZ_RUN(ParseTokenWordsMidQueryStayKeywords);

    CWZ_RUN(ParseColonInsideWordIsNotAToken);
    CWZ_RUN(ParseTrailingColonWordIsKept);
    CWZ_RUN(ParseFullWidthColonIsNotAToken);

    CWZ_RUN(MatchesKindTextSelectsOnlyText);
    CWZ_RUN(MatchesKindRichCoversHtmlAndRtf);
    CWZ_RUN(MatchesKindImageAndFile);
    CWZ_RUN(MatchesKindAnyAcceptsEveryKind);

    CWZ_RUN(MatchesEmptyQueryAcceptsEverything);
    CWZ_RUN(MatchesSingleKeywordIsSubstring);
    CWZ_RUN(MatchesAllKeywordsMustBePresent);
    CWZ_RUN(MatchesKeywordMaySpanWholeContentNotJustFirstLine);
    CWZ_RUN(MatchesEveryPathInAFileListIsFindable);
    CWZ_RUN(MatchesKindAndKeywordsCombine);

    CWZ_RUN(MatchesAppIsSubstringOfSourceApp);
    CWZ_RUN(MatchesAppTokenRejectsItemsWithoutSourceApp);
    CWZ_RUN(MatchesWithoutAppTokenIgnoresSourceApp);
    CWZ_RUN(MatchesAppAndKeywordsCombine);

    CWZ_RUN(ParseThenMatchesIsTheRealPath);
    CWZ_RUN(MatchesAgainstSearchTextARealStoreProduces);
}
