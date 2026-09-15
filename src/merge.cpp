#include "merge.h"

#include "textconv.h"  // CanonicalBody

namespace merge {
namespace {

// CanonicalBody already flattens every mergeable kind to the same UTF-16 wchar
// buffer: Text and FileDrop are stored as UTF-16 text verbatim, Html and Rtf
// come back as their extracted plain text. So "the text to join" is one
// reinterpret_cast away, and this file never needs to branch on which rich kind
// it is holding — that decision already lives in textconv.
std::wstring BodyOf(const Item& item) {
    const std::vector<uint8_t> body = textconv::CanonicalBody(item.kind, item.data);
    return std::wstring(reinterpret_cast<const wchar_t*>(body.data()),
                        body.size() / sizeof(wchar_t));
}

// The inverse: a merged wstring goes back to Store as UTF-16LE bytes with no
// NUL terminator, matching how clipboard.cpp writes Text and FileDrop payloads.
std::vector<uint8_t> ToUtf16(const std::wstring& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return std::vector<uint8_t>(p, p + s.size() * sizeof(wchar_t));
}

bool IsFileList(ItemKind k) { return k == ItemKind::FileDrop; }

bool IsText(ItemKind k) {
    return k == ItemKind::Text || k == ItemKind::Html || k == ItemKind::Rtf;
}

// A captured file list ends every path with '\n' (clipboard.cpp GetFileDrop),
// though a hand-built one may use '\r\n' or omit the last newline. Trim the
// tail so two lists join on exactly one newline boundary — no blank line
// between them, no two paths glued onto one line — whatever each body did.
void TrimTrailingNewlines(std::wstring& s) {
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) {
        s.pop_back();
    }
}

}  // namespace

std::wstring SeparatorText(Separator sep, const std::wstring& custom) {
    switch (sep) {
        case Separator::BlankLine: return L"\n\n";
        case Separator::Newline:   return L"\n";
        case Separator::Space:     return L" ";
        case Separator::Custom:    return custom;
        default:                   return L"\n\n";  // a bad config.ini value lands here
    }
}

bool CanMerge(const std::vector<const Item*>& items) {
    if (items.size() < 2) return false;
    bool anyFile = false;
    bool anyText = false;
    for (const Item* it : items) {
        if (it == nullptr) return false;
        if (IsFileList(it->kind)) {
            anyFile = true;
        } else if (IsText(it->kind)) {
            anyText = true;
        } else {
            return false;  // Image or unknown kind: refuse the whole set
        }
    }
    return !(anyFile && anyText);  // no mixing file lists with prose
}

bool Merge(const std::vector<const Item*>& items, const std::wstring& sep,
           ItemKind& outKind, std::vector<uint8_t>& outData) {
    if (!CanMerge(items)) return false;

    if (IsFileList(items[0]->kind)) {
        // File-list merge: concatenate path lines and keep the fixed "one path
        // per line, newline-terminated" shape. The user separator is a text
        // notion and does not apply here. CanMerge ruled out any text item, so
        // every body below is a path list.
        std::wstring joined;
        for (const Item* it : items) {
            std::wstring body = BodyOf(*it);
            TrimTrailingNewlines(body);
            if (body.empty()) continue;  // defensive: skip an empty path list
            if (!joined.empty()) joined += L'\n';
            joined += body;
        }
        // A drop with zero paths is structurally invalid (SetFileDrop rejects
        // count==0), so refuse rather than store an un-pasteable entry.
        if (joined.empty()) return false;
        joined += L'\n';  // re-terminate exactly like a fresh capture
        outKind = ItemKind::FileDrop;
        outData = ToUtf16(joined);
        return true;
    }

    // Text merge: flatten each body to plain text and join with the configured
    // separator. Empty bodies are kept (joining "a" and "" with "\n\n" is what
    // the user asked for); only the shape of the join is decided here, never
    // the content, which is textconv's job.
    std::wstring joined;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i != 0) joined += sep;
        joined += BodyOf(*items[i]);
    }
    outKind = ItemKind::Text;
    outData = ToUtf16(joined);
    return true;
}

}  // namespace merge
