// transform.h — Text transforms (whitespace, case, slug, timestamp)
//
// Borrows the *shape* of Ditto's CSpecialPasteOptions (src/SpecialPasteOptions.h):
// a fixed set of named operations, not a rule engine. That is a deliberate fit
// with clipwiz's "no regex, stay light" boundary — each transform is one small
// pure function and picking one is an enum, never a pattern to compile.
//
// Everything here is std::wstring in -> std::wstring out: no window, no
// clipboard, no config, no i18n, no logging. Like textconv and merge, that is
// the whole point — it can be exercised headless from tests/ and keeps the
// paste/popup paths about orchestration only. The caller turns an Item into text
// (textconv::CanonicalBody) before calling, so this unit never sees store.h.
//
// Deliberately NOT borrowed from Ditto: Typoglycemia (a letter-shuffling toy),
// pasteAsImage, PosixifyPaths and GUID generation — none are clipboard-management
// essentials, and Ditto's slugify in particular drags in <regex> plus a 200-entry
// accent-folding table. clipwiz's Slugify stays ASCII-simple instead: anything
// that is not [a-z0-9] collapses into the separator, no transliteration.
#pragma once

#include <string>

namespace transform {

// The fixed transform set. Order groups related operations so a menu can list
// them top-to-bottom without re-sorting: whitespace/line, then case, then
// charset/slug, then the timestamp meta-op. Values are contiguous from 0 so the
// popup can iterate [0, kKindCount) to build its submenu.
enum class Kind : int {
    Trim = 0,          // strip leading/trailing whitespace
    RemoveLineBreaks,  // delete every CR/LF, gluing the lines together
    OneLineBetween,    // normalize paragraph breaks to exactly one '\n'
    TwoLinesBetween,   // normalize paragraph breaks to one blank line ("\n\n")
    Upper,             // every character to upper case
    Lower,             // every character to lower case
    Capitalize,        // title case: first letter of each word up, the rest down
    SentenceCase,      // first letter of each sentence up, the rest down
    CamelCase,         // "some text here" -> "someTextHere"
    InvertCase,        // swap the case of every letter
    AsciiOnly,         // drop every code point above U+007F
    Slugify,           // lower-case URL slug: runs of non [a-z0-9] -> one separator
    AppendDateTime,    // append a caller-supplied timestamp string verbatim
};

// One more than the last Kind value, for callers that iterate the whole set.
inline constexpr int kKindCount = 13;

// Per-call knobs. Both have safe defaults, so the common Apply(kind, text) call
// needs no Options at all.
struct Options {
    // Word separator Slugify joins on. Ditto defaults to '-'; clipwiz keeps that
    // and lets the user change it in settings. An empty value falls back to '-'
    // so words never silently glue together.
    std::wstring slugSep = L"-";

    // The exact string AppendDateTime tacks on the end. Injecting it keeps Apply
    // pure and testable: production passes the formatted current time, tests pass
    // a fixed literal. Apply never reads the clock itself. The caller owns the
    // format and any leading separator; Apply appends it verbatim.
    std::wstring dateTime;
};

// Apply one transform. Pure and total: every Kind yields a result, an unknown
// enum value falls through to returning the text unchanged, and empty in -> empty
// out for every Kind except AppendDateTime (which returns the timestamp alone).
std::wstring Apply(Kind kind, const std::wstring& text, const Options& opts = {});

}  // namespace transform
