#include "transform.h"

#include <cwctype>  // towlower, towupper, iswalnum, iswupper, iswlower

namespace transform {
namespace {

// Case folding follows the rest of clipwiz (store.cpp, filter.cpp, blocklist.cpp):
// the C towlower/towupper, cast back to wchar_t. Locale-independent enough for
// the BMP and identical to what the search filter already does.
wchar_t ToLower(wchar_t c) { return static_cast<wchar_t>(towlower(c)); }
wchar_t ToUpper(wchar_t c) { return static_cast<wchar_t>(towupper(c)); }

// Explicit ASCII whitespace set rather than iswspace, so paragraph/word splitting
// behaves the same in every locale and in tests. Line breaks are the CR/LF subset.
bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\n' || c == L'\r' || c == L'\v' || c == L'\f';
}
bool IsLineBreak(wchar_t c) { return c == L'\n' || c == L'\r'; }

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

std::wstring RemoveLineBreaks(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (!IsLineBreak(c)) out += c;
    }
    return out;
}

// The shared engine behind OneLineBetween and TwoLinesBetween: treat any run of
// line breaks as one paragraph boundary, trim each paragraph, drop the empties,
// then re-join on `sep`. Collapsing runs is what turns "a\n\n\n\nb" and "a\r\nb"
// into the same two paragraphs, so the output spacing is uniform regardless of
// how the source was wrapped.
std::wstring NormalizeParagraphs(const std::wstring& s, const std::wstring& sep) {
    std::wstring out;
    std::wstring para;
    bool havePara = false;  // a paragraph is already in `out`, so the next needs a separator
    auto flush = [&]() {
        std::wstring t = Trim(para);
        para.clear();
        if (t.empty()) return;
        if (havePara) out += sep;
        out += t;
        havePara = true;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        if (IsLineBreak(s[i])) {
            flush();
            while (i + 1 < s.size() && IsLineBreak(s[i + 1])) ++i;  // skip the rest of the run
        } else {
            para += s[i];
        }
    }
    flush();
    return out;
}

std::wstring MapCase(const std::wstring& s, wchar_t (*fn)(wchar_t)) {
    std::wstring out = s;
    for (wchar_t& c : out) c = fn(c);
    return out;
}

// Title case: a word is a run of non-whitespace, so "hello-world" capitalizes
// only the 'h'. That is the predictable reading of "capitalize each word" and
// avoids guessing at hyphen/slash boundaries the user did not ask about.
std::wstring Capitalize(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool atWordStart = true;
    for (wchar_t c : s) {
        if (IsSpace(c)) {
            atWordStart = true;
            out += c;
        } else if (atWordStart) {
            out += ToUpper(c);
            atWordStart = false;
        } else {
            out += ToLower(c);
        }
    }
    return out;
}

// Sentence case: uppercase the first letter after a sentence terminator (. ! ?),
// lowercase everything else. Whitespace right after a terminator stays inside the
// "at start" state so the next real letter is the one that gets raised.
std::wstring SentenceCase(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool atSentenceStart = true;
    for (wchar_t c : s) {
        if (c == L'.' || c == L'!' || c == L'?') {
            atSentenceStart = true;
            out += c;
        } else if (IsSpace(c)) {
            out += c;
        } else if (atSentenceStart) {
            out += ToUpper(c);
            atSentenceStart = false;
        } else {
            out += ToLower(c);
        }
    }
    return out;
}

// camelCase: split on any non-alphanumeric (spaces, punctuation, underscores all
// act as boundaries), lowercase the first word, capitalize each later word, and
// drop the boundaries. Unicode letters count as word characters via iswalnum, so
// an accented word survives intact rather than being treated as a separator.
std::wstring CamelCase(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    bool firstWord = true;
    bool atWordStart = true;
    for (wchar_t c : s) {
        if (iswalnum(c)) {
            if (atWordStart) {
                out += (firstWord ? ToLower(c) : ToUpper(c));
                firstWord = false;
                atWordStart = false;
            } else {
                out += ToLower(c);
            }
        } else {
            atWordStart = true;
        }
    }
    return out;
}

std::wstring InvertCase(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (iswupper(c)) {
            out += ToLower(c);
        } else if (iswlower(c)) {
            out += ToUpper(c);
        } else {
            out += c;
        }
    }
    return out;
}

// Keep ASCII verbatim, drop everything above U+007F. This deletes rather than
// transliterates ("café" -> "caf"): folding accents to ASCII needs a big mapping
// table (see Ditto's slugify) and guessing wrong is worse than dropping. Both
// halves of a surrogate pair are >= 0x80, so emoji vanish cleanly too.
std::wstring AsciiOnly(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (static_cast<unsigned>(c) < 0x80u) out += c;
    }
    return out;
}

std::wstring Slugify(const std::wstring& s, const std::wstring& sep) {
    const std::wstring join = sep.empty() ? std::wstring(L"-") : sep;
    std::wstring out;
    out.reserve(s.size());
    bool pendingSep = false;  // a gap is waiting, emitted only if another word follows
    for (wchar_t c : s) {
        const wchar_t lc = ToLower(c);
        const bool wordChar = (lc >= L'a' && lc <= L'z') || (lc >= L'0' && lc <= L'9');
        if (wordChar) {
            if (pendingSep && !out.empty()) out += join;  // collapse the whole gap to one separator
            pendingSep = false;
            out += lc;
        } else if (!out.empty()) {
            pendingSep = true;  // leading gaps never set this (out is empty), so no head separator
        }
    }
    return out;  // a trailing gap is left in pendingSep and simply never emitted
}

}  // namespace

std::wstring Apply(Kind kind, const std::wstring& text, const Options& opts) {
    switch (kind) {
        case Kind::Trim:            return Trim(text);
        case Kind::RemoveLineBreaks:return RemoveLineBreaks(text);
        case Kind::OneLineBetween:  return NormalizeParagraphs(text, L"\n");
        case Kind::TwoLinesBetween: return NormalizeParagraphs(text, L"\n\n");
        case Kind::Upper:           return MapCase(text, ToUpper);
        case Kind::Lower:           return MapCase(text, ToLower);
        case Kind::Capitalize:      return Capitalize(text);
        case Kind::SentenceCase:    return SentenceCase(text);
        case Kind::CamelCase:       return CamelCase(text);
        case Kind::InvertCase:      return InvertCase(text);
        case Kind::AsciiOnly:       return AsciiOnly(text);
        case Kind::Slugify:         return Slugify(text, opts.slugSep);
        case Kind::AppendDateTime:  return text + opts.dateTime;
        default:                    return text;  // an out-of-range enum value: leave text untouched
    }
}

}  // namespace transform
