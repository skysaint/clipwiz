// test_transform.cpp — guard rails for src/transform.cpp
//
// A transform runs on the way to the clipboard, and the source text is often
// already gone by the time the user reads the output — so a wrong result is
// discovered late and far from its cause: a title-cased word that swallowed a
// hyphen, a slug wearing a leading dash, a timestamp glued to the wrong end, an
// accent folded into a letter the user never typed. Each rule is pinned here
// headless, from both directions, so the exact input -> output contract cannot
// drift when the wiring changes around it.
#include <string>

#include "testfw.h"
#include "transform.h"

namespace {

using transform::Kind;
using transform::Options;

// Named Tx, not Apply: the first argument is a transform::Kind, so ADL would
// pull transform::Apply into the candidate set and tie with a local 2-arg Apply.
std::wstring Tx(Kind k, const std::wstring& text) {
    return transform::Apply(k, text);
}

// ---------------- whitespace / line transforms ----------------

void TrimStripsLeadingAndTrailingWhitespace() {
    CWZ_CHECK_EQ(Tx(Kind::Trim, L"  hi \t\n"), std::wstring(L"hi"));
    CWZ_CHECK_EQ(Tx(Kind::Trim, L"no pad"), std::wstring(L"no pad"));
    CWZ_CHECK_EQ(Tx(Kind::Trim, L"   "), std::wstring(L""));
    CWZ_CHECK_EQ(Tx(Kind::Trim, L""), std::wstring(L""));
    // interior whitespace is untouched
    CWZ_CHECK_EQ(Tx(Kind::Trim, L" a  b "), std::wstring(L"a  b"));
}

void RemoveLineBreaksDeletesEveryCrLf() {
    CWZ_CHECK_EQ(Tx(Kind::RemoveLineBreaks, L"a\r\nb\nc"), std::wstring(L"abc"));
    CWZ_CHECK_EQ(Tx(Kind::RemoveLineBreaks, L"one line"), std::wstring(L"one line"));
    CWZ_CHECK_EQ(Tx(Kind::RemoveLineBreaks, L"\n\n"), std::wstring(L""));
    // spaces and tabs survive; only line breaks go
    CWZ_CHECK_EQ(Tx(Kind::RemoveLineBreaks, L"a \t\nb"), std::wstring(L"a \tb"));
}

void OneLineBetweenCollapsesParagraphRuns() {
    CWZ_CHECK_EQ(Tx(Kind::OneLineBetween, L"a\nb"), std::wstring(L"a\nb"));
    CWZ_CHECK_EQ(Tx(Kind::OneLineBetween, L"a\n\n\nb"), std::wstring(L"a\nb"));
    CWZ_CHECK_EQ(Tx(Kind::OneLineBetween, L"a\r\nb"), std::wstring(L"a\nb"));
    // each paragraph is trimmed and empties are dropped
    CWZ_CHECK_EQ(Tx(Kind::OneLineBetween, L"  a  \n\n  b  "), std::wstring(L"a\nb"));
    CWZ_CHECK_EQ(Tx(Kind::OneLineBetween, L"\n\n\n"), std::wstring(L""));
    CWZ_CHECK_EQ(Tx(Kind::OneLineBetween, L"only"), std::wstring(L"only"));
}

void TwoLinesBetweenJoinsOnBlankLine() {
    CWZ_CHECK_EQ(Tx(Kind::TwoLinesBetween, L"a\nb"), std::wstring(L"a\n\nb"));
    CWZ_CHECK_EQ(Tx(Kind::TwoLinesBetween, L"a\n\n\n\nb"), std::wstring(L"a\n\nb"));
    CWZ_CHECK_EQ(Tx(Kind::TwoLinesBetween, L"a\r\nb\r\nc"), std::wstring(L"a\n\nb\n\nc"));
    CWZ_CHECK_EQ(Tx(Kind::TwoLinesBetween, L"single"), std::wstring(L"single"));
}

// ---------------- case transforms ----------------

void UpperLowerMapEveryCharacter() {
    CWZ_CHECK_EQ(Tx(Kind::Upper, L"Hello World"), std::wstring(L"HELLO WORLD"));
    CWZ_CHECK_EQ(Tx(Kind::Lower, L"Hello World"), std::wstring(L"hello world"));
    CWZ_CHECK_EQ(Tx(Kind::Upper, L"already 123"), std::wstring(L"ALREADY 123"));
    CWZ_CHECK_EQ(Tx(Kind::Lower, L""), std::wstring(L""));
    // line breaks are preserved by a pure case map
    CWZ_CHECK_EQ(Tx(Kind::Upper, L"a\nb"), std::wstring(L"A\nB"));
}

void CapitalizeTitlesEachWord() {
    CWZ_CHECK_EQ(Tx(Kind::Capitalize, L"hello world"), std::wstring(L"Hello World"));
    CWZ_CHECK_EQ(Tx(Kind::Capitalize, L"HELLO WORLD"), std::wstring(L"Hello World"));
    CWZ_CHECK_EQ(Tx(Kind::Capitalize, L"hELLO wORLD"), std::wstring(L"Hello World"));
    // a hyphen is not a word boundary: only the leading letter is raised
    CWZ_CHECK_EQ(Tx(Kind::Capitalize, L"hello-world"), std::wstring(L"Hello-world"));
    CWZ_CHECK_EQ(Tx(Kind::Capitalize, L""), std::wstring(L""));
}

void SentenceCaseRaisesAfterEachTerminator() {
    CWZ_CHECK_EQ(Tx(Kind::SentenceCase, L"hello. world!"), std::wstring(L"Hello. World!"));
    CWZ_CHECK_EQ(Tx(Kind::SentenceCase, L"HELLO WORLD. FOO BAR"),
                 std::wstring(L"Hello world. Foo bar"));
    CWZ_CHECK_EQ(Tx(Kind::SentenceCase, L"one sentence only"),
                 std::wstring(L"One sentence only"));
    // a terminator with no following letter just ends the string
    CWZ_CHECK_EQ(Tx(Kind::SentenceCase, L"done."), std::wstring(L"Done."));
    CWZ_CHECK_EQ(Tx(Kind::SentenceCase, L""), std::wstring(L""));
}

void CamelCaseJoinsWordsAndDropsBoundaries() {
    CWZ_CHECK_EQ(Tx(Kind::CamelCase, L"some text here"), std::wstring(L"someTextHere"));
    CWZ_CHECK_EQ(Tx(Kind::CamelCase, L"hello, world_foo"), std::wstring(L"helloWorldFoo"));
    CWZ_CHECK_EQ(Tx(Kind::CamelCase, L"ALREADY Camel"), std::wstring(L"alreadyCamel"));
    // leading separators do not create an empty first word
    CWZ_CHECK_EQ(Tx(Kind::CamelCase, L"  leading"), std::wstring(L"leading"));
    CWZ_CHECK_EQ(Tx(Kind::CamelCase, L"single"), std::wstring(L"single"));
    CWZ_CHECK_EQ(Tx(Kind::CamelCase, L""), std::wstring(L""));
}

void InvertCaseSwapsEveryLetter() {
    CWZ_CHECK_EQ(Tx(Kind::InvertCase, L"Hello World"), std::wstring(L"hELLO wORLD"));
    CWZ_CHECK_EQ(Tx(Kind::InvertCase, L"abc XYZ"), std::wstring(L"ABC xyz"));
    // digits, spaces and punctuation pass through untouched
    CWZ_CHECK_EQ(Tx(Kind::InvertCase, L"123 ! "), std::wstring(L"123 ! "));
    CWZ_CHECK_EQ(Tx(Kind::InvertCase, L""), std::wstring(L""));
}

// ---------------- charset / slug transforms ----------------

void AsciiOnlyDropsHighCodePoints() {
    CWZ_CHECK_EQ(Tx(Kind::AsciiOnly, L"caf\x00e9"), std::wstring(L"caf"));  // é dropped
    CWZ_CHECK_EQ(Tx(Kind::AsciiOnly, L"plain ascii 123"), std::wstring(L"plain ascii 123"));
    CWZ_CHECK_EQ(Tx(Kind::AsciiOnly, L"\x4e2d\x6587"), std::wstring(L""));  // all CJK dropped
    // a surrogate pair (emoji) is two high code units, both dropped
    std::wstring emoji = L"hi";
    emoji.push_back(static_cast<wchar_t>(0xD83D));
    emoji.push_back(static_cast<wchar_t>(0xDE00));
    CWZ_CHECK_EQ(Tx(Kind::AsciiOnly, emoji), std::wstring(L"hi"));
    CWZ_CHECK_EQ(Tx(Kind::AsciiOnly, L""), std::wstring(L""));
}

void SlugifyBuildsLowerHyphenSlug() {
    CWZ_CHECK_EQ(Tx(Kind::Slugify, L"Hello, World!"), std::wstring(L"hello-world"));
    CWZ_CHECK_EQ(Tx(Kind::Slugify, L"  leading and trailing  "),
                 std::wstring(L"leading-and-trailing"));
    // a run of non-word characters collapses to a single separator
    CWZ_CHECK_EQ(Tx(Kind::Slugify, L"a -- b"), std::wstring(L"a-b"));
    CWZ_CHECK_EQ(Tx(Kind::Slugify, L"already-slug"), std::wstring(L"already-slug"));
    // digits are word characters; a string with no word characters slugs to empty
    CWZ_CHECK_EQ(Tx(Kind::Slugify, L"v1.2.3"), std::wstring(L"v1-2-3"));
    CWZ_CHECK_EQ(Tx(Kind::Slugify, L"!!!"), std::wstring(L""));
    CWZ_CHECK_EQ(Tx(Kind::Slugify, L""), std::wstring(L""));
}

void SlugifyHonorsCustomSeparator() {
    Options o;
    o.slugSep = L"_";
    CWZ_CHECK_EQ(transform::Apply(Kind::Slugify, L"Hello World", o), std::wstring(L"hello_world"));
    o.slugSep = L"+";
    CWZ_CHECK_EQ(transform::Apply(Kind::Slugify, L"a b c", o), std::wstring(L"a+b+c"));
}

void SlugifyEmptySeparatorFallsBackToHyphen() {
    Options o;
    o.slugSep = L"";  // a cleared config field must never glue words together
    CWZ_CHECK_EQ(transform::Apply(Kind::Slugify, L"Hello World", o), std::wstring(L"hello-world"));
}

// ---------------- timestamp meta-op ----------------

void AppendDateTimeAppendsVerbatim() {
    Options o;
    o.dateTime = L" 2026-09-14 15:30";
    CWZ_CHECK_EQ(transform::Apply(Kind::AppendDateTime, L"log entry", o),
                 std::wstring(L"log entry 2026-09-14 15:30"));
    // the caller owns the separator, so a newline-prefixed stamp lands as given
    o.dateTime = L"\n[stamp]";
    CWZ_CHECK_EQ(transform::Apply(Kind::AppendDateTime, L"body", o), std::wstring(L"body\n[stamp]"));
    // empty text yields just the timestamp
    o.dateTime = L"X";
    CWZ_CHECK_EQ(transform::Apply(Kind::AppendDateTime, L"", o), std::wstring(L"X"));
    // an unset stamp appends nothing
    CWZ_CHECK_EQ(Tx(Kind::AppendDateTime, L"body"), std::wstring(L"body"));
}

// ---------------- cross-cutting contract ----------------

void EmptyTextStaysEmptyExceptTimestamp() {
    // Every transform but AppendDateTime maps "" -> "", so a blank clipboard
    // never turns into surprise content.
    const Kind kinds[] = {
        Kind::Trim, Kind::RemoveLineBreaks, Kind::OneLineBetween, Kind::TwoLinesBetween,
        Kind::Upper, Kind::Lower, Kind::Capitalize, Kind::SentenceCase, Kind::CamelCase,
        Kind::InvertCase, Kind::AsciiOnly, Kind::Slugify,
    };
    for (Kind k : kinds) {
        CWZ_CHECK_EQ(Tx(k, L""), std::wstring(L""));
    }
}

void UnknownEnumReturnsTextUnchanged() {
    // A bad config value cast into Kind must be inert, not destructive.
    CWZ_CHECK_EQ(Tx(static_cast<Kind>(999), L"keep me"), std::wstring(L"keep me"));
}

void KindCountMatchesEnumRange() {
    // The popup builds its submenu by iterating [0, kKindCount); this pins the
    // constant to the real enum so the two can never silently disagree.
    CWZ_CHECK_EQ(transform::kKindCount, 13);
    CWZ_CHECK_EQ(static_cast<int>(Kind::AppendDateTime), transform::kKindCount - 1);
    CWZ_CHECK_EQ(static_cast<int>(Kind::Trim), 0);
}

}  // namespace

void RunTransformTests() {
    CWZ_RUN(TrimStripsLeadingAndTrailingWhitespace);
    CWZ_RUN(RemoveLineBreaksDeletesEveryCrLf);
    CWZ_RUN(OneLineBetweenCollapsesParagraphRuns);
    CWZ_RUN(TwoLinesBetweenJoinsOnBlankLine);

    CWZ_RUN(UpperLowerMapEveryCharacter);
    CWZ_RUN(CapitalizeTitlesEachWord);
    CWZ_RUN(SentenceCaseRaisesAfterEachTerminator);
    CWZ_RUN(CamelCaseJoinsWordsAndDropsBoundaries);
    CWZ_RUN(InvertCaseSwapsEveryLetter);

    CWZ_RUN(AsciiOnlyDropsHighCodePoints);
    CWZ_RUN(SlugifyBuildsLowerHyphenSlug);
    CWZ_RUN(SlugifyHonorsCustomSeparator);
    CWZ_RUN(SlugifyEmptySeparatorFallsBackToHyphen);

    CWZ_RUN(AppendDateTimeAppendsVerbatim);

    CWZ_RUN(EmptyTextStaysEmptyExceptTimestamp);
    CWZ_RUN(UnknownEnumReturnsTextUnchanged);
    CWZ_RUN(KindCountMatchesEnumRange);
}
