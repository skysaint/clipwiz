// blocklist.cpp
#include "blocklist.h"

#include <cwctype>

namespace blocklist {
namespace {

// Anchored glob match, '*' = any run of characters including none. Classic
// two-pointer scan with backtracking to the last '*'; linear in the common
// case and no recursion, so a pathological pattern cannot blow the stack.
bool GlobMatch(const std::wstring& pat, const std::wstring& s) {
    size_t p = 0;   // cursor in pat
    size_t i = 0;   // cursor in s
    size_t star = std::wstring::npos;  // pat index just after the last '*'
    size_t mark = 0;                   // s index to resume from on backtrack
    while (i < s.size()) {
        if (p < pat.size() && pat[p] == s[i]) {
            ++p;
            ++i;
        } else if (p < pat.size() && pat[p] == L'*') {
            star = p++;
            mark = i;
        } else if (star != std::wstring::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == L'*') {
        ++p;
    }
    return p == pat.size();
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return s;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && iswspace(s[b]) != 0) ++b;
    while (e > b && iswspace(s[e - 1]) != 0) --e;
    return s.substr(b, e - b);
}

// True when `text` holds no usable value. An unresolved field is empty, and an
// empty field must never match — otherwise a rule as broad as "*" would fire
// against a window whose title we could not read.
bool Usable(const std::wstring& text) {
    return !text.empty();
}

}  // namespace

bool PatternMatches(const std::wstring& pattern, const std::wstring& text) {
    if (pattern.empty()) {
        return false;  // an empty pattern is not a match-everything rule
    }
    if (pattern.find(L'*') == std::wstring::npos) {
        return text.find(pattern) != std::wstring::npos;  // substring
    }
    return GlobMatch(pattern, text);
}

RuleSet Parse(const std::wstring& rulesText) {
    RuleSet rules;
    size_t pos = 0;
    while (pos <= rulesText.size()) {
        size_t eol = rulesText.find(L'\n', pos);
        std::wstring line = rulesText.substr(
            pos, eol == std::wstring::npos ? std::wstring::npos : eol - pos);
        pos = (eol == std::wstring::npos) ? rulesText.size() + 1 : eol + 1;

        line = Trim(line);
        if (line.empty() || line[0] == L'#') {
            continue;  // blank or comment
        }
        Action action = Action::NoCapture;
        if (line[0] == L'!') {
            action = Action::FullDisable;
            line = Trim(line.substr(1));
        }
        if (line.empty()) {
            continue;  // a bare "!" with no pattern would match everything
        }
        rules.push_back({action, Lower(line)});
    }
    return rules;
}

Action Classify(const RuleSet& rules, const Target& target) {
    Action worst = Action::None;
    for (const Rule& rule : rules) {
        if (rule.pattern.empty()) {
            continue;
        }
        const bool hit = (Usable(target.processName) &&
                          PatternMatches(rule.pattern, target.processName)) ||
                         (Usable(target.processPath) &&
                          PatternMatches(rule.pattern, target.processPath)) ||
                         (Usable(target.windowTitle) &&
                          PatternMatches(rule.pattern, target.windowTitle));
        if (hit && rule.action > worst) {
            worst = rule.action;
            if (worst == Action::FullDisable) {
                break;  // nothing is stronger; stop early
            }
        }
    }
    return worst;
}

}  // namespace blocklist
