// test_blocklist.cpp — guard rails for src/blocklist.cpp
//
// The blocklist is a privacy feature: when it is wrong, it is wrong silently.
// A pattern that fails to match leaks a password into the history; a pattern
// that matches too much makes clipwiz stop recording ordinary copies. Neither
// announces itself, so both directions are pinned down here.
//
// Everything runs headless: no window, no clipboard, no config file. The
// caller in real life resolves the process name / path / title and hands them
// over as strings; these tests hand over the same strings directly.
#include <string>
#include <vector>

#include "blocklist.h"
#include "testfw.h"

namespace {

using blocklist::Action;
using blocklist::RuleSet;
using blocklist::Target;

Target MakeTarget(std::wstring name, std::wstring path, std::wstring title) {
    Target t;
    t.processName = std::move(name);
    t.processPath = std::move(path);
    t.windowTitle = std::move(title);
    return t;
}

RuleSet One(Action action, const std::wstring& pattern) {
    return RuleSet{{action, pattern}};
}

// ---------------- PatternMatches ----------------

void SubstringMatchesInsideLongerText() {
    CWZ_CHECK(blocklist::PatternMatches(L"keepass", L"keepass.exe"));
    CWZ_CHECK(blocklist::PatternMatches(L"keepass", L"my keepass database"));
    CWZ_CHECK(!blocklist::PatternMatches(L"keepass", L"lastpass.exe"));
}

void EmptyPatternNeverMatches() {
    // An empty pattern must not become "match everything"; Parse already drops
    // such lines, but PatternMatches defends the invariant on its own.
    CWZ_CHECK(!blocklist::PatternMatches(L"", L"keepass.exe"));
    CWZ_CHECK(!blocklist::PatternMatches(L"", L""));
}

void GlobStarWrapsSubstring() {
    CWZ_CHECK(blocklist::PatternMatches(L"*secret*", L"my secret db"));
    CWZ_CHECK(blocklist::PatternMatches(L"*secret*", L"secret"));
    CWZ_CHECK(!blocklist::PatternMatches(L"*secret*", L"public db"));
}

void GlobIsAnchoredNotSubstring() {
    // With a '*', the pattern must span the whole string. "keep*" anchors at
    // the start, so it matches "keepass.exe" but not "my keepass".
    CWZ_CHECK(blocklist::PatternMatches(L"keep*", L"keepass.exe"));
    CWZ_CHECK(!blocklist::PatternMatches(L"keep*", L"my keepass"));
    CWZ_CHECK(blocklist::PatternMatches(L"*pass", L"keepass"));
    CWZ_CHECK(!blocklist::PatternMatches(L"*pass", L"keepass.exe"));
}

void GlobMatchesPathWithBackslashes() {
    CWZ_CHECK(blocklist::PatternMatches(L"c:\\tools\\*", L"c:\\tools\\secret.exe"));
    CWZ_CHECK(!blocklist::PatternMatches(L"c:\\tools\\*", L"d:\\tools\\secret.exe"));
}

void GlobMultipleStars() {
    CWZ_CHECK(blocklist::PatternMatches(L"*a*b*", L"xxayybzz"));
    CWZ_CHECK(!blocklist::PatternMatches(L"*a*b*", L"xxbyyazz"));
}

void GlobLoneStarMatchesAnyNonEmpty() {
    CWZ_CHECK(blocklist::PatternMatches(L"*", L"anything"));
    CWZ_CHECK(blocklist::PatternMatches(L"*", L""));  // glob '*' spans empty too
}

// ---------------- Parse ----------------

void ParseEmptyTextYieldsNoRules() {
    CWZ_CHECK_EQ(blocklist::Parse(L"").size(), static_cast<size_t>(0));
    CWZ_CHECK_EQ(blocklist::Parse(L"\n\n  \n").size(), static_cast<size_t>(0));
}

void ParsePlainLineIsNoCapture() {
    const RuleSet r = blocklist::Parse(L"keepass.exe");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(1));
    CWZ_CHECK(r[0].action == Action::NoCapture);
    CWZ_CHECK(r[0].pattern == L"keepass.exe");
}

void ParseBangLineIsFullDisable() {
    const RuleSet r = blocklist::Parse(L"!keepass.exe");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(1));
    CWZ_CHECK(r[0].action == Action::FullDisable);
    CWZ_CHECK(r[0].pattern == L"keepass.exe");
}

void ParseBangWithSpaceAfterIsStillFullDisable() {
    const RuleSet r = blocklist::Parse(L"!   secret app");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(1));
    CWZ_CHECK(r[0].action == Action::FullDisable);
    CWZ_CHECK(r[0].pattern == L"secret app");
}

void ParseSkipsBlankAndCommentLines() {
    const RuleSet r = blocklist::Parse(L"# a comment\n\nkeepass.exe\n   # indented comment\n");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(1));
    CWZ_CHECK(r[0].pattern == L"keepass.exe");
}

void ParseTrimsSurroundingWhitespace() {
    const RuleSet r = blocklist::Parse(L"   keepass.exe   ");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(1));
    CWZ_CHECK(r[0].pattern == L"keepass.exe");
}

void ParseLowercasesPattern() {
    // Matching is case-insensitive because Parse folds the pattern once here;
    // the caller folds the target the same way.
    const RuleSet r = blocklist::Parse(L"KeepAss.EXE");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(1));
    CWZ_CHECK(r[0].pattern == L"keepass.exe");
}

void ParseDropsBareBang() {
    // "!" alone would be an empty pattern; dropped rather than kept, so it can
    // never be mistaken for a match-everything FullDisable rule.
    CWZ_CHECK_EQ(blocklist::Parse(L"!").size(), static_cast<size_t>(0));
    CWZ_CHECK_EQ(blocklist::Parse(L"!   ").size(), static_cast<size_t>(0));
}

void ParseHandlesNoTrailingNewline() {
    const RuleSet r = blocklist::Parse(L"a.exe\nb.exe");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(2));
    CWZ_CHECK(r[0].pattern == L"a.exe");
    CWZ_CHECK(r[1].pattern == L"b.exe");
}

void ParseKeepsTwoTiersInOneList() {
    const RuleSet r = blocklist::Parse(L"keepass.exe\n!secret.exe");
    CWZ_CHECK_EQ(r.size(), static_cast<size_t>(2));
    CWZ_CHECK(r[0].action == Action::NoCapture);
    CWZ_CHECK(r[1].action == Action::FullDisable);
}

// ---------------- Classify ----------------

void ClassifyNoRulesIsNone() {
    const Target t = MakeTarget(L"keepass.exe", L"c:\\keepass.exe", L"keepass");
    CWZ_CHECK(blocklist::Classify(RuleSet{}, t) == Action::None);
}

void ClassifyMatchesEachField() {
    const RuleSet r = One(Action::NoCapture, L"keepass");
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"keepass.exe", L"", L"")) == Action::NoCapture);
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"", L"c:\\tools\\keepass.exe", L"")) ==
              Action::NoCapture);
    // Target fields are lowercased by the caller (blocklist.h contract), so the
    // title here is already folded; Classify does no case folding of its own.
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"", L"", L"keepass - db")) == Action::NoCapture);
}

void ClassifyNoMatchIsNone() {
    const RuleSet r = One(Action::NoCapture, L"keepass");
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"notepad.exe", L"c:\\notepad.exe", L"untitled")) ==
              Action::None);
}

void ClassifyStrongestActionWins() {
    // Both rules match; the FullDisable one must dominate no matter the order.
    const Target t = MakeTarget(L"keepass.exe", L"", L"");
    const RuleSet a{{Action::NoCapture, L"keepass"}, {Action::FullDisable, L"*.exe"}};
    const RuleSet b{{Action::FullDisable, L"*.exe"}, {Action::NoCapture, L"keepass"}};
    CWZ_CHECK(blocklist::Classify(a, t) == Action::FullDisable);
    CWZ_CHECK(blocklist::Classify(b, t) == Action::FullDisable);
}

void ClassifyEmptyFieldsNeverMatch() {
    // An unresolved target (all fields empty) matches nothing, not even "*".
    // Recording a wrong guess about which app is in play is worse than none.
    const RuleSet r = One(Action::FullDisable, L"*");
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"", L"", L"")) == Action::None);
}

void ClassifyStarMatchesAnyResolvedField() {
    const RuleSet r = One(Action::NoCapture, L"*");
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"anything.exe", L"", L"")) == Action::NoCapture);
}

void ClassifySubstringSpansTitleWords() {
    const RuleSet r = One(Action::NoCapture, L"confidential");
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"winword.exe", L"", L"Q3 confidential report")) ==
              Action::NoCapture);
}

void ParseThenClassifyIsTheRealPath() {
    // End to end: the exact text a user types in Settings, through Parse, to a
    // verdict on a real-looking target.
    const RuleSet r = blocklist::Parse(L"# password managers\nkeepass.exe\n! *secret*");
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"keepass.exe", L"", L"")) == Action::NoCapture);
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"app.exe", L"", L"my secret notes")) ==
              Action::FullDisable);
    CWZ_CHECK(blocklist::Classify(r, MakeTarget(L"notepad.exe", L"", L"todo")) == Action::None);
}

}  // namespace

void RunBlocklistTests() {
    CWZ_RUN(SubstringMatchesInsideLongerText);
    CWZ_RUN(EmptyPatternNeverMatches);
    CWZ_RUN(GlobStarWrapsSubstring);
    CWZ_RUN(GlobIsAnchoredNotSubstring);
    CWZ_RUN(GlobMatchesPathWithBackslashes);
    CWZ_RUN(GlobMultipleStars);
    CWZ_RUN(GlobLoneStarMatchesAnyNonEmpty);

    CWZ_RUN(ParseEmptyTextYieldsNoRules);
    CWZ_RUN(ParsePlainLineIsNoCapture);
    CWZ_RUN(ParseBangLineIsFullDisable);
    CWZ_RUN(ParseBangWithSpaceAfterIsStillFullDisable);
    CWZ_RUN(ParseSkipsBlankAndCommentLines);
    CWZ_RUN(ParseTrimsSurroundingWhitespace);
    CWZ_RUN(ParseLowercasesPattern);
    CWZ_RUN(ParseDropsBareBang);
    CWZ_RUN(ParseHandlesNoTrailingNewline);
    CWZ_RUN(ParseKeepsTwoTiersInOneList);

    CWZ_RUN(ClassifyNoRulesIsNone);
    CWZ_RUN(ClassifyMatchesEachField);
    CWZ_RUN(ClassifyNoMatchIsNone);
    CWZ_RUN(ClassifyStrongestActionWins);
    CWZ_RUN(ClassifyEmptyFieldsNeverMatch);
    CWZ_RUN(ClassifyStarMatchesAnyResolvedField);
    CWZ_RUN(ClassifySubstringSpansTitleWords);
    CWZ_RUN(ParseThenClassifyIsTheRealPath);
}
