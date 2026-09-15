// test_privacy.cpp — guard rails for src/privacy.cpp
//
// Getting these rules wrong fails in opposite directions and both are bad:
// treating an opt-out marker as presence-only silently drops every copy from
// an app that declared itself recordable, and ignoring a real opt-out writes
// somebody's password into a file on disk. Neither shows up as a crash.
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "privacy.h"
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
    return std::vector<uint8_t>(p, p + (s.size() + 1) * sizeof(wchar_t));  // NUL included
}

// Little-endian integer payloads, the widths the marker is seen at in practice.
std::vector<uint8_t> Dword(uint32_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    return std::vector<uint8_t>(p, p + sizeof(v));
}

std::vector<uint8_t> Word(uint16_t v) {
    const auto* p = reinterpret_cast<const uint8_t*>(&v);
    return std::vector<uint8_t>(p, p + sizeof(v));
}

// Index of a marker in the table, or kMarkerCount when it is not there. Used
// both to build Seen arrays positionally and to assert what the table contains.
size_t IndexOf(const wchar_t* name) {
    for (size_t i = 0; i < privacy::kMarkerCount; ++i) {
        if (std::wcscmp(privacy::kMarkers[i].name, name) == 0) {
            return i;
        }
    }
    return privacy::kMarkerCount;
}

// A clipboard observation set with one marker present.
std::vector<privacy::Seen> OnlyPresent(size_t index, std::vector<uint8_t> payload = {}) {
    std::vector<privacy::Seen> seen(privacy::kMarkerCount);
    if (index < privacy::kMarkerCount) {
        seen[index].present = true;
        seen[index].payload = std::move(payload);
    }
    return seen;
}

const wchar_t* kViewerIgnore = L"Clipboard Viewer Ignore";
const wchar_t* kExcludeFromMonitor = L"ExcludeClipboardContentFromMonitorProcessing";
const wchar_t* kCanIncludeHistory = L"CanIncludeInClipboardHistory";
const wchar_t* kCanUploadCloud = L"CanUploadToCloudClipboard";

// ---------------------------------------------------------------- payload values

void PayloadDwordZeroIsFalse() {
    // The documented spelling: CanIncludeInClipboardHistory carries a DWORD and
    // zero is the opt-out. This is the case the old two-marker check missed.
    CWZ_CHECK(privacy::PayloadMeansFalse(Dword(0)));
}

void PayloadDwordNonZeroIsTrue() {
    // One means "recording me is fine" — the opposite of an exclusion. Treating
    // mere presence as exclusion would drop every app that says this.
    CWZ_CHECK(!privacy::PayloadMeansFalse(Dword(1)));
    CWZ_CHECK(!privacy::PayloadMeansFalse(Dword(42)));
}

void PayloadWordIsHonoured() {
    // A WORD zero is the same claim at half the width, and shows up often
    // enough to be worth accepting.
    CWZ_CHECK(privacy::PayloadMeansFalse(Word(0)));
    CWZ_CHECK(!privacy::PayloadMeansFalse(Word(1)));
}

void PayloadTextAndIntegerOfTheSameWidthAreToldApart() {
    // "no" and a WORD are both two bytes, and so are "0\0" and a WORD. Read as
    // integers they are 0x6F6E and 0x0030 — both non-zero, both taken for
    // "recording is fine", which is the opposite of what the app said.
    CWZ_CHECK(privacy::PayloadMeansFalse(Bytes("no")));

    std::vector<uint8_t> zeroText = Bytes("0");
    zeroText.push_back(0);
    CWZ_CHECK_EQ(zeroText.size(), static_cast<size_t>(2));
    CWZ_CHECK(privacy::PayloadMeansFalse(zeroText));

    // The two-byte integers still mean what they say.
    CWZ_CHECK(privacy::PayloadMeansFalse(Word(0)));
    CWZ_CHECK(!privacy::PayloadMeansFalse(Word(1)));
}

void PayloadFourByteTextIsNotReadAsADword() {
    // Same ambiguity one width up. "true" happens to agree under both readings,
    // but a four-byte-padded "no" only comes out right as text.
    CWZ_CHECK(!privacy::PayloadMeansFalse(Bytes("true")));

    std::vector<uint8_t> noPadded = Bytes("no");
    noPadded.resize(4, 0);
    CWZ_CHECK(privacy::PayloadMeansFalse(noPadded));
}

void PayloadUtf16SingleDigitIsFalse() {
    // L"0" is two bytes, 0x30 0x00, which as a WORD is 48 and would look true.
    // Printable-ASCII-plus-NUL wins the ambiguity, so it reads as text.
    CWZ_CHECK(privacy::PayloadMeansFalse(WideBytes(L"0")));
    CWZ_CHECK(!privacy::PayloadMeansFalse(WideBytes(L"1")));
}

void PayloadUnrecognisedWidthIsNotFalse() {
    // Eight zero bytes is neither a documented width nor readable text, so it is
    // not an opt-out that can be positively confirmed.
    CWZ_CHECK(!privacy::PayloadMeansFalse(std::vector<uint8_t>(8, 0)));
}

void PayloadAsciiWordsAreFalse() {
    CWZ_CHECK(privacy::PayloadMeansFalse(Bytes("0")));
    CWZ_CHECK(privacy::PayloadMeansFalse(Bytes("false")));
    CWZ_CHECK(privacy::PayloadMeansFalse(Bytes("no")));
}

void PayloadAsciiWordsAreCaseInsensitive() {
    CWZ_CHECK(privacy::PayloadMeansFalse(Bytes("FALSE")));
    CWZ_CHECK(privacy::PayloadMeansFalse(Bytes("False")));
    CWZ_CHECK(privacy::PayloadMeansFalse(Bytes("NO")));
    CWZ_CHECK(!privacy::PayloadMeansFalse(Bytes("TRUE")));
}

void PayloadNulTerminatedTextIsFalse() {
    // A C string on the clipboard carries its terminator. Stopping at the first
    // NUL and dropping every NUL both land on the same word here.
    std::vector<uint8_t> v = Bytes("false");
    v.push_back(0);
    CWZ_CHECK(privacy::PayloadMeansFalse(v));
}

void PayloadUtf16TextIsFalse() {
    // Dropping every NUL rather than stopping at the first one is what makes
    // UTF-16LE readable by the same code path: L"false" collapses to "false".
    CWZ_CHECK(privacy::PayloadMeansFalse(WideBytes(L"false")));
    CWZ_CHECK(!privacy::PayloadMeansFalse(WideBytes(L"true")));
}

void PayloadEmptyIsNotFalse() {
    // The rule is "must positively read a false value to exclude". An unread
    // or absent payload is not an opt-out, and guessing otherwise would throw
    // away ordinary copies whenever a marker was larger than we bother to read.
    CWZ_CHECK(!privacy::PayloadMeansFalse({}));
}

void PayloadUnrecognisedIsNotFalse() {
    // Same reasoning as the empty payload: only the recognised false spellings
    // exclude. Anything else is left to the capture path.
    CWZ_CHECK(!privacy::PayloadMeansFalse(Bytes("maybe")));
    CWZ_CHECK(!privacy::PayloadMeansFalse(Bytes("yes")));
    CWZ_CHECK(!privacy::PayloadMeansFalse(Bytes("1")));
    CWZ_CHECK(!privacy::PayloadMeansFalse(Bytes("true")));
}

// ------------------------------------------------------------------- the table

void TableHoldsExactlyTheThreeHonouredMarkers() {
    CWZ_CHECK_EQ(privacy::kMarkerCount, static_cast<size_t>(3));
    CWZ_CHECK(IndexOf(kViewerIgnore) < privacy::kMarkerCount);
    CWZ_CHECK(IndexOf(kExcludeFromMonitor) < privacy::kMarkerCount);
    CWZ_CHECK(IndexOf(kCanIncludeHistory) < privacy::kMarkerCount);
}

void TableAssignsTheRightRuleToEachMarker() {
    // The two long-standing markers exclude by being there at all.
    CWZ_CHECK(privacy::kMarkers[IndexOf(kViewerIgnore)].rule ==
              privacy::Rule::AnyPresenceExcludes);
    CWZ_CHECK(privacy::kMarkers[IndexOf(kExcludeFromMonitor)].rule ==
              privacy::Rule::AnyPresenceExcludes);
    // The third only excludes on a false value.
    CWZ_CHECK(privacy::kMarkers[IndexOf(kCanIncludeHistory)].rule ==
              privacy::Rule::OnlyFalseValueExcludes);
}

void TableDeliberatelyOmitsCloudUploadMarker() {
    // CanUploadToCloudClipboard asks for something clipwiz never does: it is
    // purely local and never uploads. Honouring it would drop ordinary copies
    // to satisfy a restriction that does not apply here. This assertion exists
    // so the omission survives a future "let's be thorough" edit — see
    // doc/technical.md for the reasoning.
    CWZ_CHECK_EQ(IndexOf(kCanUploadCloud), privacy::kMarkerCount);
}

// ----------------------------------------------------------------- the verdict

void ShouldSkipWithNoMarkers() {
    const std::vector<privacy::Seen> seen(privacy::kMarkerCount);
    CWZ_CHECK(!privacy::ShouldSkip(seen.data()));
}

void ShouldSkipOnPresenceMarkers() {
    // Neither of these needs a payload; presence alone is the whole signal.
    CWZ_CHECK(privacy::ShouldSkip(OnlyPresent(IndexOf(kViewerIgnore)).data()));
    CWZ_CHECK(privacy::ShouldSkip(OnlyPresent(IndexOf(kExcludeFromMonitor)).data()));
}

void ShouldSkipOnHistoryMarkerWithFalseValue() {
    const size_t i = IndexOf(kCanIncludeHistory);
    CWZ_CHECK(privacy::ShouldSkip(OnlyPresent(i, Dword(0)).data()));
    CWZ_CHECK(privacy::ShouldSkip(OnlyPresent(i, Word(0)).data()));
    CWZ_CHECK(privacy::ShouldSkip(OnlyPresent(i, Bytes("false")).data()));
}

void ShouldNotSkipOnHistoryMarkerWithTrueValue() {
    const size_t i = IndexOf(kCanIncludeHistory);
    CWZ_CHECK(!privacy::ShouldSkip(OnlyPresent(i, Dword(1)).data()));
    CWZ_CHECK(!privacy::ShouldSkip(OnlyPresent(i, Bytes("true")).data()));
}

void ShouldNotSkipOnHistoryMarkerPresenceAlone() {
    // The distinction the whole unit exists to hold: an app that puts the
    // marker on with no readable value has not opted out. Reading presence as
    // exclusion here would drop every app that declares itself recordable.
    const size_t i = IndexOf(kCanIncludeHistory);
    CWZ_CHECK(!privacy::ShouldSkip(OnlyPresent(i).data()));
    CWZ_CHECK(!privacy::ShouldSkip(OnlyPresent(i, Bytes("yes")).data()));
}

void ShouldSkipWhenAnyOneMarkerExcludes() {
    // A recordable marker alongside a real opt-out still excludes: the strictest
    // claim wins, because the cost of over-recording is a leaked secret.
    const size_t history = IndexOf(kCanIncludeHistory);
    const size_t ignore = IndexOf(kViewerIgnore);
    std::vector<privacy::Seen> seen(privacy::kMarkerCount);
    seen[history].present = true;
    seen[history].payload = Dword(1);  // says recording is fine
    seen[ignore].present = true;       // says it is not
    CWZ_CHECK(privacy::ShouldSkip(seen.data()));
}

void PayloadOfPresenceMarkerIsNeverRead() {
    // A presence marker excludes whatever its payload says. Reading it would
    // let an app retract "Clipboard Viewer Ignore" by writing 1 next to it,
    // which is not what that format means.
    const size_t i = IndexOf(kViewerIgnore);
    CWZ_CHECK(privacy::ShouldSkip(OnlyPresent(i, Dword(1)).data()));
    CWZ_CHECK(privacy::ShouldSkip(OnlyPresent(i, Bytes("true")).data()));
}

}  // namespace

void RunPrivacyTests() {
    CWZ_RUN(PayloadDwordZeroIsFalse);
    CWZ_RUN(PayloadDwordNonZeroIsTrue);
    CWZ_RUN(PayloadWordIsHonoured);
    CWZ_RUN(PayloadTextAndIntegerOfTheSameWidthAreToldApart);
    CWZ_RUN(PayloadFourByteTextIsNotReadAsADword);
    CWZ_RUN(PayloadUtf16SingleDigitIsFalse);
    CWZ_RUN(PayloadUnrecognisedWidthIsNotFalse);
    CWZ_RUN(PayloadAsciiWordsAreFalse);
    CWZ_RUN(PayloadAsciiWordsAreCaseInsensitive);
    CWZ_RUN(PayloadNulTerminatedTextIsFalse);
    CWZ_RUN(PayloadUtf16TextIsFalse);
    CWZ_RUN(PayloadEmptyIsNotFalse);
    CWZ_RUN(PayloadUnrecognisedIsNotFalse);

    CWZ_RUN(TableHoldsExactlyTheThreeHonouredMarkers);
    CWZ_RUN(TableAssignsTheRightRuleToEachMarker);
    CWZ_RUN(TableDeliberatelyOmitsCloudUploadMarker);

    CWZ_RUN(ShouldSkipWithNoMarkers);
    CWZ_RUN(ShouldSkipOnPresenceMarkers);
    CWZ_RUN(ShouldSkipOnHistoryMarkerWithFalseValue);
    CWZ_RUN(ShouldNotSkipOnHistoryMarkerWithTrueValue);
    CWZ_RUN(ShouldNotSkipOnHistoryMarkerPresenceAlone);
    CWZ_RUN(ShouldSkipWhenAnyOneMarkerExcludes);
    CWZ_RUN(PayloadOfPresenceMarkerIsNeverRead);
}
