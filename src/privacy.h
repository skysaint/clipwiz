// privacy.h — Clipboard exclusion markers
//
// Some applications ask not to be recorded. Windows has no single convention
// for how they ask: two long-standing formats mean "I am present, ignore this
// copy", while a third carries a boolean where only a FALSE value means that.
// Telling those apart is the whole job of this unit.
//
// Pure logic: the caller reads the clipboard and hands over what it found.
// Nothing here opens the clipboard, so tests/test_privacy.cpp can cover every
// rule without a window or a real clipboard.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace privacy {

// How a marker expresses "do not record this".
enum class Rule {
    // The format being on the clipboard at all is the signal. Its payload is
    // never read.
    AnyPresenceExcludes,
    // The format carries a boolean and only a FALSE value excludes. Presence
    // with a true value means the opposite: "history is fine".
    OnlyFalseValueExcludes,
};

struct Marker {
    const wchar_t* name;  // Registered clipboard format name
    Rule rule;
};

// Every exclusion marker clipwiz honours, in one table. Keeping the name and
// its rule adjacent is deliberate: adding a marker cannot add the name and
// forget to say which of the two semantics it uses.
//
// Deliberately ABSENT: "CanUploadToCloudClipboard". It reads like a privacy
// marker and zsclip asserts in a test that it is not one — the claim it makes
// is "do not upload this to a cloud clipboard", and clipwiz never uploads
// anywhere. Honouring it would drop ordinary local copies for a restriction
// that does not apply. See doc/technical.md.
//
// `inline constexpr` rather than an extern definition in the .cpp: callers size
// arrays with kMarkerCount, which only works if both are constant expressions.
inline constexpr Marker kMarkers[] = {
    // The two long-standing "ignore me" formats. Both mean what they say by
    // being there at all; neither is documented as carrying a value.
    {L"Clipboard Viewer Ignore", Rule::AnyPresenceExcludes},
    {L"ExcludeClipboardContentFromMonitorProcessing", Rule::AnyPresenceExcludes},

    // Opt-out by value. Applications put this on the clipboard with 1 to say
    // "recording me is fine" and 0 to ask out, so presence alone means nothing
    // here — reading it as a presence marker would exclude every app that
    // bothered to declare itself recordable.
    {L"CanIncludeInClipboardHistory", Rule::OnlyFalseValueExcludes},
};

inline constexpr size_t kMarkerCount = sizeof(kMarkers) / sizeof(kMarkers[0]);

// One marker as observed on an open clipboard.
struct Seen {
    bool present = false;          // IsClipboardFormatAvailable said yes
    std::vector<uint8_t> payload;  // Only read for OnlyFalseValueExcludes
};

// Interpret a marker payload as a boolean. Windows documents the value as a
// DWORD but puts no type on the clipboard, and in practice it arrives as a
// DWORD, a WORD, ASCII text, or UTF-16LE text. Width alone cannot tell those
// apart — "no" and a WORD are both two bytes — so a payload made only of
// printable ASCII and NULs is read as text and anything else as an integer.
//
// An empty or unrecognised payload is NOT false. The marker is an opt-out and
// the rule is "must positively read a false value to exclude"; guessing from
// bytes we cannot interpret would silently throw away ordinary copies.
bool PayloadMeansFalse(const std::vector<uint8_t>& payload);

// True = drop this capture. `seen` must hold kMarkerCount entries in table
// order, one per kMarkers[i].
bool ShouldSkip(const Seen* seen);

}  // namespace privacy
