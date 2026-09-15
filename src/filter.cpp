// filter.cpp
#include "filter.h"

#include <cwctype>

namespace filter {
namespace {

const wchar_t kKindPrefix[] = L"kind:";
const wchar_t kAppPrefix[] = L"app:";

bool HasPrefix(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Consume one leading word as a token. Returns false when the word is not a
// token this build understands, which stops token scanning right there: a
// colon anywhere later in the query is then just text.
bool TryParseToken(const std::wstring& word, Query& q) {
    if (HasPrefix(word, kKindPrefix)) {
        const std::wstring value = word.substr(std::wcslen(kKindPrefix));
        if (value == L"text") {
            q.kind = Kind::Text;
            return true;
        }
        if (value == L"rich") {
            q.kind = Kind::Rich;
            return true;
        }
        if (value == L"image") {
            q.kind = Kind::Image;
            return true;
        }
        if (value == L"file") {
            q.kind = Kind::File;
            return true;
        }
        // Unknown type name: leave the word in the keyword list. A typo then
        // shows up as "no results", which the user can see and fix, instead of
        // silently dropping the constraint they meant to apply.
        return false;
    }
    if (HasPrefix(word, kAppPrefix)) {
        const std::wstring value = word.substr(std::wcslen(kAppPrefix));
        if (value.empty()) {
            return false;  // a bare "app:" constrains nothing
        }
        q.app = value;
        return true;
    }
    return false;
}

}  // namespace

Query Parse(const std::wstring& rawFilter) {
    Query q;
    if (rawFilter.empty()) {
        return q;
    }

    // Lowercase once up front. Keywords are stored lowercase so Matches() can
    // run a plain substring search against the already-lowercase searchText
    // without allocating or case-folding per item per keystroke.
    std::wstring s;
    s.reserve(rawFilter.size());
    for (wchar_t c : rawFilter) {
        s.push_back(static_cast<wchar_t>(towlower(c)));
    }

    std::vector<std::wstring> words;
    size_t pos = 0;
    while (pos < s.size()) {
        while (pos < s.size() && iswspace(s[pos]) != 0) {
            ++pos;
        }
        const size_t start = pos;
        while (pos < s.size() && iswspace(s[pos]) == 0) {
            ++pos;
        }
        if (pos > start) {
            words.push_back(s.substr(start, pos - start));
        }
    }

    // Leading tokens only, then everything else is a keyword.
    size_t i = 0;
    while (i < words.size() && TryParseToken(words[i], q)) {
        ++i;
    }
    for (; i < words.size(); ++i) {
        q.keywords.push_back(words[i]);
    }
    return q;
}

bool Matches(const Query& q, ItemKind kind, const std::wstring& searchText,
             const std::wstring& sourceApp) {
    switch (q.kind) {
        case Kind::Text:
            if (kind != ItemKind::Text) return false;
            break;
        case Kind::Rich:
            if (kind != ItemKind::Html && kind != ItemKind::Rtf) return false;
            break;
        case Kind::Image:
            if (kind != ItemKind::Image) return false;
            break;
        case Kind::File:
            if (kind != ItemKind::FileDrop) return false;
            break;
        case Kind::Any:
            break;
    }
    if (!q.app.empty() && sourceApp.find(q.app) == std::wstring::npos) {
        return false;
    }
    // AND semantics: every keyword must appear somewhere in the content.
    for (const std::wstring& kw : q.keywords) {
        if (searchText.find(kw) == std::wstring::npos) {
            return false;
        }
    }
    return true;
}

}  // namespace filter
