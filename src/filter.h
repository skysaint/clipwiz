// filter.h — Popup filter parsing and matching
//
// Pure logic: raw filter-box text in, a verdict per item out. No window, no
// store access, no i18n — which is why it lives here instead of inside
// popup.cpp, and why tests/test_filter.cpp can cover it headless.
//
// Search is the only way to organize entries in clipwiz (no tags, no groups,
// no folders, by design), so this is the most load-bearing UI logic there is.
#pragma once

#include <string>
#include <vector>

#include "store.h"

namespace filter {

// Value of a `kind:` token. Any = the user did not constrain the type.
enum class Kind {
    Any,
    Text,   // ItemKind::Text
    Rich,   // ItemKind::Html or ItemKind::Rtf
    Image,  // ItemKind::Image
    File,   // ItemKind::FileDrop
};

struct Query {
    Kind kind = Kind::Any;
    std::wstring app;                    // Lowercased; empty = no `app:` constraint
    std::vector<std::wstring> keywords;  // Lowercased; all must match (AND)

    bool Empty() const { return kind == Kind::Any && app.empty() && keywords.empty(); }
};

// Parse the raw contents of the filter box.
//
// Deliberately lenient, because this is a search field first and a query
// language second. A word is only treated as a token when it sits at the START
// of the filter, its prefix is one we know, and something follows the colon.
// Everything else stays a keyword — including any colon later in the query,
// which is what "https://example.com" and "注意:kind" look like. Swallowing
// those would silently return zero results for perfectly ordinary searches.
Query Parse(const std::wstring& rawFilter);

// Both `searchText` and `sourceApp` must already be lowercase; Parse()
// lowercases the query, so matching is a plain substring search with no
// per-keystroke allocation or case folding.
//
// `sourceApp` may be empty for items captured before source tracking existed,
// in which case an `app:` token matches nothing — an honest answer rather than
// a false positive.
bool Matches(const Query& q, ItemKind kind, const std::wstring& searchText,
             const std::wstring& sourceApp);

}  // namespace filter
