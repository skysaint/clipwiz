// blocklist.h — Per-application capture / paste suppression
//
// Privacy markers (see privacy.h) only work on applications that bother to set
// them. Password managers and confidential client software usually do not, so
// the user needs a way to name an application and say "never record what I
// copy here". That list is this unit.
//
// Two strengths, matching how people actually want to suppress an app:
//   NoCapture    - do not record clipboard changes that come from it. Hotkeys
//                  still work, so an item copied elsewhere can still be pasted
//                  into it. This is the common case.
//   FullDisable  - NoCapture, and also ignore clipwiz hotkeys while it is the
//                  foreground window, so nothing can be pasted into it either.
//                  For the apps where even a stray paste is unacceptable.
//
// Pure logic: the caller resolves which window/process is in play and hands
// over the strings. Nothing here opens the clipboard, enumerates windows, or
// reads config, so tests/test_blocklist.cpp can cover every rule headless.
#pragma once

#include <string>
#include <vector>

namespace blocklist {

// What to do with an application a rule matches. Ordered weakest to strongest
// so Classify() can return the single strongest action that applies.
enum class Action {
    None,         // No rule matched; behave normally.
    NoCapture,    // Do not record its clipboard changes.
    FullDisable,  // Do not record, and do not respond to hotkeys while it is foreground.
};

// One user-authored rule: an action plus the pattern that triggers it.
// `pattern` is stored lowercased; matching is case-insensitive.
struct Rule {
    Action action = Action::NoCapture;
    std::wstring pattern;
};

using RuleSet = std::vector<Rule>;

// The strings a rule is tested against, all lowercased. Any field a rule
// matches fires it, so one pattern like "keepass" covers the process name,
// the install path, and a window title that happens to contain it.
//
// A field is left empty when it could not be resolved. An empty field never
// matches: a wrong guess about which app is in play is worse than no guess.
struct Target {
    std::wstring processName;  // e.g. "keepass.exe"
    std::wstring processPath;  // e.g. "c:\program files\keepass\keepass.exe"
    std::wstring windowTitle;  // e.g. "keepass - mydatabase.kdbx"
};

// Match one pattern against one already-lowercased string.
//
// Two behaviours, chosen by whether the pattern contains '*':
//   no '*'  - substring match. "keepass" hits "keepass.exe" and "my keepass db".
//             This is what people mean when they type a name, and it needs no
//             glob syntax to explain.
//   has '*' - anchored glob, '*' matching any run of characters including none.
//             "*secret*" and "c:\tools\*" both work; the pattern must span the
//             whole string, so "keep*" does not match "my keepass".
// Wildcards are the only metacharacters: no regex, by design.
bool PatternMatches(const std::wstring& pattern, const std::wstring& text);

// Parse the multiline rules text the user edits in Settings, one rule per line.
//
// Lenient on purpose: blank lines and lines whose first non-space character is
// '#' are skipped, so the box can carry comments and spacing. A leading '!'
// marks the line FullDisable; anything else is NoCapture. Surrounding
// whitespace is trimmed and the pattern lowercased. A line that is only '!'
// (no pattern) is dropped rather than becoming a match-everything rule, which
// would silently suppress all of clipwiz.
RuleSet Parse(const std::wstring& rulesText);

// Strongest action any rule in `rules` applies to `target`, or Action::None.
// Callers gate on the result: capture is skipped when this is not None, paste
// is skipped only when it is FullDisable.
Action Classify(const RuleSet& rules, const Target& target);

}  // namespace blocklist
