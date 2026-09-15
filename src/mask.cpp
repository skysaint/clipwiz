// mask.cpp — hand-written sensitive-span scanners. See mask.h for the contract.
#include "mask.h"

#include <algorithm>
#include <cwchar>
#include <vector>

namespace mask {
namespace {

// The redaction marker. Fixed length on purpose: it does not grow with the
// secret, so the masked form leaks neither the value nor its length. Asterisks
// rather than TieZ's "..." because clipwiz already uses "..." for truncation in
// the one-line preview — reusing it here would make a redacted phone number
// look like a merely shortened one.
constexpr const wchar_t* kMask = L"****";

// At least this many characters of a span are always hidden, even for a span
// too short to honour the requested prefix/suffix. Ported from TieZ.
constexpr size_t kMinMasked = 2;

// How many characters to keep visible at each end of a masked span.
constexpr size_t kKeepFront = 3;
constexpr size_t kKeepBack = 3;

// Text longer than this is not scanned at all (guard rail ported from TieZ).
constexpr size_t kMaxScanLen = 5000;

// A span [begin, end) of sensitive text. `at` is the index of '@' for an email
// span (its local part is masked but the domain is kept) and npos otherwise.
constexpr size_t kNoAt = static_cast<size_t>(-1);
struct Span {
    size_t begin;
    size_t end;
    size_t at;
};

bool IsUpper(wchar_t c) { return c >= L'A' && c <= L'Z'; }
bool IsLower(wchar_t c) { return c >= L'a' && c <= L'z'; }
bool IsDigit(wchar_t c) { return c >= L'0' && c <= L'9'; }
bool IsAlpha(wchar_t c) { return IsUpper(c) || IsLower(c); }
bool IsAlnum(wchar_t c) { return IsAlpha(c) || IsDigit(c); }
bool IsWordChar(wchar_t c) { return IsAlnum(c) || c == L'_'; }  // regex \w, ASCII

// email local-part charset: [A-Za-z0-9._%+-]
bool IsLocalChar(wchar_t c) {
    return IsAlnum(c) || c == L'.' || c == L'_' || c == L'%' || c == L'+' || c == L'-';
}
// email domain charset: [A-Za-z0-9.-]
bool IsDomainChar(wchar_t c) { return IsAlnum(c) || c == L'.' || c == L'-'; }
// separators tolerated inside/around a phone number: +, space, dash, parentheses
bool IsPhoneSep(wchar_t c) {
    return c == L' ' || c == L'-' || c == L'(' || c == L')' || c == L'+';
}
bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'\v' || c == L'\f';
}

wchar_t ToLowerAscii(wchar_t c) { return IsUpper(c) ? static_cast<wchar_t>(c - L'A' + L'a') : c; }

// Case-insensitive compare of text[pos .. pos+len) against an ASCII literal.
bool MatchNoCase(const std::wstring& text, size_t pos, const wchar_t* lit, size_t len) {
    if (pos + len > text.size()) {
        return false;
    }
    for (size_t k = 0; k < len; ++k) {
        if (ToLowerAscii(text[pos + k]) != ToLowerAscii(lit[k])) {
            return false;
        }
    }
    return true;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}

// Keep up to `front` leading and `back` trailing characters, hide the middle
// behind kMask, and always hide at least kMinMasked characters. When the span
// is too short to honour both requests the kept counts shrink proportionally.
// Faithful port of TieZ's maskMiddleChars.
std::wstring MaskMiddle(const std::wstring& v, size_t front, size_t back) {
    const size_t len = v.size();
    if (len <= kMinMasked) {
        return kMask;
    }
    const size_t available = len - kMinMasked;
    const size_t totalReq = front + back;
    size_t keepFront, keepBack;
    if (totalReq <= available) {
        keepFront = front;
        keepBack = back;
    } else {
        keepFront = (totalReq > 0) ? (available * front) / totalReq : 0;
        keepBack = std::min(back, available - keepFront);
    }
    std::wstring out = v.substr(0, keepFront);
    out += kMask;
    out += v.substr(len - keepBack);  // keepBack == 0 → empty tail
    return out;
}

// ---------------------------------------------------------------- Email

void ScanEmail(const std::wstring& t, std::vector<Span>& out) {
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] != L'@') {
            continue;
        }
        size_t b = i;
        while (b > 0 && IsLocalChar(t[b - 1])) --b;
        if (b == i) {
            continue;  // empty local part
        }
        size_t e = i + 1;
        while (e < t.size() && IsDomainChar(t[e])) ++e;
        while (e > i + 1 && !IsAlpha(t[e - 1])) --e;  // TLD must end on a letter
        if (e <= i + 1) {
            continue;  // empty domain
        }
        size_t dot = kNoAt;
        for (size_t k = i + 1; k < e; ++k) {
            if (t[k] == L'.') dot = k;  // last dot in the domain
        }
        if (dot == kNoAt || dot <= i + 1) {
            continue;  // need a dot, with a non-empty label before it
        }
        if (e - dot - 1 < 2) {
            continue;  // TLD at least 2 letters
        }
        bool tldAllAlpha = true;
        for (size_t k = dot + 1; k < e; ++k) {
            if (!IsAlpha(t[k])) { tldAllAlpha = false; break; }
        }
        if (!tldAllAlpha) {
            continue;
        }
        out.push_back(Span{b, e, i});
    }
}

// ---------------------------------------------------------------- Phone

// True when the national number starting at `i` is really the tail of a longer
// digit run, so it must not be read as a phone number. An immediately preceding
// "+86" / "86" country code (separators allowed) is fine; any other preceding
// digit is not.
bool PrecededByLongerNumber(const std::wstring& t, size_t i) {
    size_t p = i;
    while (p > 0 && IsPhoneSep(t[p - 1])) --p;
    if (p == 0 || !IsDigit(t[p - 1])) {
        return false;  // nothing, or a non-digit, before the number
    }
    size_t dEnd = p;
    size_t dBegin = p;
    while (dBegin > 0 && IsDigit(t[dBegin - 1])) --dBegin;
    if (t.compare(dBegin, dEnd - dBegin, L"86") == 0) {
        size_t s = dBegin;
        if (s > 0 && t[s - 1] == L'+') --s;
        return s > 0 && IsDigit(t[s - 1]);  // "+86" is fine; "586..." is not
    }
    return true;  // preceded by digits that are not a country code
}

void ScanPhone(const std::wstring& t, std::vector<Span>& out) {
    for (size_t i = 0; i + 1 < t.size(); ++i) {
        if (t[i] != L'1' || t[i + 1] < L'3' || t[i + 1] > L'9') {
            continue;  // national number starts 1[3-9]
        }
        if (i > 0 && IsDigit(t[i - 1])) {
            // A digit immediately before means this '1' is mid-run, not a start.
            continue;
        }
        if (PrecededByLongerNumber(t, i)) {
            continue;
        }
        // Collect exactly 11 digits, skipping separators between them.
        size_t p = i;
        int digits = 0;
        while (p < t.size() && digits < 11) {
            if (IsDigit(t[p])) {
                ++digits;
                ++p;
            } else if (IsPhoneSep(t[p])) {
                ++p;
            } else {
                break;
            }
        }
        if (digits != 11) {
            continue;
        }
        if (p < t.size() && IsDigit(t[p])) {
            continue;  // more digits follow → longer number, not a phone
        }
        out.push_back(Span{i, p, kNoAt});
        i = p - 1;  // do not rescan inside the number we just took
    }
}

// ---------------------------------------------------------------- ID card

bool IsLeap(int y) { return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0); }

int DaysInMonth(int y, int m) {
    static const int kDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m < 1 || m > 12) {
        return 0;
    }
    if (m == 2 && IsLeap(y)) {
        return 29;
    }
    return kDays[m - 1];
}

// GB 11643 / ISO 7064:1983 MOD 11-2 check digit over the first 17 digits.
bool IdChecksumOk(const std::wstring& t, size_t i) {
    static const int kWeight[17] = {7, 9, 10, 5, 8, 4, 2, 1, 6, 3, 7, 9, 10, 5, 8, 4, 2};
    static const wchar_t kMap[] = L"10X98765432";  // indexed by sum % 11 → 0..10
    int sum = 0;
    for (int k = 0; k < 17; ++k) {
        sum += (t[i + static_cast<size_t>(k)] - L'0') * kWeight[k];
    }
    wchar_t expect = kMap[sum % 11];
    wchar_t got = t[i + 17];
    if (got == L'x') {
        got = L'X';
    }
    return got == expect;
}

bool IsIdCardAt(const std::wstring& t, size_t i) {
    for (int k = 0; k < 17; ++k) {
        if (!IsDigit(t[i + static_cast<size_t>(k)])) {
            return false;
        }
    }
    const wchar_t last = t[i + 17];
    if (!IsDigit(last) && last != L'X' && last != L'x') {
        return false;
    }
    if (t[i] < L'1' || t[i] > L'9') {
        return false;  // region code first digit non-zero
    }
    if (t[i + 6] < L'1' || t[i + 6] > L'9') {
        return false;  // year first digit non-zero
    }
    const int year = (t[i + 6] - L'0') * 1000 + (t[i + 7] - L'0') * 100 + (t[i + 8] - L'0') * 10 +
                     (t[i + 9] - L'0');
    const int month = (t[i + 10] - L'0') * 10 + (t[i + 11] - L'0');
    const int day = (t[i + 12] - L'0') * 10 + (t[i + 13] - L'0');
    if (month < 1 || month > 12) {
        return false;
    }
    if (day < 1 || day > DaysInMonth(year, month)) {
        return false;
    }
    return IdChecksumOk(t, i);
}

void ScanIdCard(const std::wstring& t, std::vector<Span>& out) {
    for (size_t i = 0; i + 18 <= t.size(); ++i) {
        if (i > 0 && IsWordChar(t[i - 1])) {
            continue;  // word boundary before
        }
        if (i + 18 < t.size() && IsWordChar(t[i + 18])) {
            continue;  // word boundary after
        }
        if (IsIdCardAt(t, i)) {
            out.push_back(Span{i, i + 18, kNoAt});
            i += 17;  // do not rescan inside the id we just took
        }
    }
}

// ---------------------------------------------------------------- API key

void ScanApiKey(const std::wstring& t, std::vector<Span>& out) {
    static const wchar_t* kPrefixes[] = {
        L"sk", L"pk", L"ghp", L"gho", L"github_pat", L"AIza", L"AKIA", L"ya29",
    };
    for (size_t i = 0; i < t.size(); ++i) {
        if (i > 0 && IsWordChar(t[i - 1])) {
            continue;  // left boundary: not glued to a preceding word char
        }
        size_t end = 0;
        for (const wchar_t* pre : kPrefixes) {
            const size_t plen = std::wcslen(pre);
            if (!MatchNoCase(t, i, pre, plen)) {
                continue;
            }
            const size_t sepPos = i + plen;
            if (sepPos >= t.size() || (t[sepPos] != L'-' && t[sepPos] != L'_')) {
                continue;
            }
            size_t p = sepPos + 1;
            const size_t bodyStart = p;
            while (p < t.size() && (IsWordChar(t[p]) || t[p] == L'-')) {
                ++p;
            }
            if (p - bodyStart < 20) {
                continue;  // tail too short to be a key
            }
            end = p;
            break;
        }
        if (end > 0) {
            out.push_back(Span{i, end, kNoAt});
            i = end - 1;  // do not rescan inside the key we just took
        }
    }
}

// ---------------------------------------------------------------- Password

// The whole-text heuristic: the *entire* content is one password-like token.
// This is a classification of the string as a whole, not a scan for a span
// within it — a real password copied on its own is the target, and a long
// document that merely contains one is (correctly) not.
bool WholeTextIsPassword(const std::wstring& raw) {
    const std::wstring t = Trim(raw);
    if (t.size() < 8 || t.size() > 64) {
        return false;
    }
    bool upper = false, lower = false, digit = false, special = false;
    for (wchar_t c : t) {
        if (IsSpace(c)) {
            return false;  // no internal whitespace
        }
        if (IsUpper(c)) {
            upper = true;
        } else if (IsLower(c)) {
            lower = true;
        } else if (IsDigit(c)) {
            digit = true;
        } else {
            special = true;
        }
    }
    return upper && lower && digit && special;
}

// Append text[cursor .. s.begin) verbatim, then the masked form of the span.
void AppendMasked(std::wstring& out, const std::wstring& t, size_t& cursor, const Span& s) {
    out.append(t, cursor, s.begin - cursor);
    if (s.at != kNoAt && s.at > s.begin && s.at < s.end) {
        // Email: mask the local part, keep "@domain" — the domain is not the
        // secret, and seeing it is what lets the user recognise the address.
        out += MaskMiddle(t.substr(s.begin, s.at - s.begin), kKeepFront, kKeepBack);
        out.append(t, s.at, s.end - s.at);
    } else {
        out += MaskMiddle(t.substr(s.begin, s.end - s.begin), kKeepFront, kKeepBack);
    }
    cursor = s.end;
}

}  // namespace

bool AnyEnabled(const Config& cfg) {
    return cfg.email || cfg.phone || cfg.idCard || cfg.apiKey || cfg.password;
}

std::wstring Apply(const std::wstring& text, const Config& cfg) {
    if (!AnyEnabled(cfg)) {
        return text;
    }
    if (text.size() > kMaxScanLen) {
        return text;  // too big to be worth scanning on a UI recompute
    }
    if (text.compare(0, 5, L"data:") == 0) {
        return text;  // a data URI is machine content, not a typed secret
    }

    // Whole-text password first: if the entire content is one password-like
    // token, mask it wholesale — span scanning would only find the same thing.
    if (cfg.password && WholeTextIsPassword(text)) {
        return MaskMiddle(Trim(text), kKeepFront, kKeepBack);
    }

    std::vector<Span> spans;
    if (cfg.email) {
        ScanEmail(text, spans);
    }
    if (cfg.phone) {
        ScanPhone(text, spans);
    }
    if (cfg.idCard) {
        ScanIdCard(text, spans);
    }
    if (cfg.apiKey) {
        ScanApiKey(text, spans);
    }
    if (spans.empty()) {
        return text;
    }

    // Take spans left to right; on the same start prefer the longer one; drop
    // any span overlapping one already taken. Boundary rules in the scanners
    // keep real overlaps rare (an 18-digit ID is never also read as the
    // 11-digit phone inside it), so this is a backstop, not the main defence.
    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
        if (a.begin != b.begin) {
            return a.begin < b.begin;
        }
        return a.end > b.end;
    });

    std::wstring out;
    size_t cursor = 0;
    size_t takenEnd = 0;
    for (const Span& s : spans) {
        if (s.begin < takenEnd) {
            continue;  // overlaps an already-taken span
        }
        AppendMasked(out, text, cursor, s);
        takenEnd = s.end;
    }
    out.append(text, cursor, text.size() - cursor);
    return out;
}

}  // namespace mask
