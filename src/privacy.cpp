// privacy.cpp
#include "privacy.h"

#include <cstring>
#include <string>

namespace privacy {
namespace {

// ASCII-lowercase one byte. Deliberately not towlower: the text path compares
// against a fixed set of ASCII words, and pulling in the wide character tables
// for that would be cost without benefit.
char AsciiLower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// The two rules as separately named predicates, because "present at all" and
// "present and false" are different claims about the clipboard. Folding them
// into one condition is how an opt-out marker ends up excluding every copy
// that carries it — including the ones where the app said recording was fine.
bool PresenceIsExclusion(const Seen& seen) {
    return seen.present;
}

bool FalseValueIsExclusion(const Seen& seen) {
    return seen.present && PayloadMeansFalse(seen.payload);
}

}  // namespace

bool PayloadMeansFalse(const std::vector<uint8_t>& payload) {
    // The rule is "must positively read a false value to exclude". An absent or
    // unread payload is not an opt-out, and guessing otherwise would throw away
    // ordinary copies whenever a marker was bigger than we bother to read.
    if (payload.empty()) {
        return false;
    }

    // Text first, but only when the bytes can only be text. Width alone cannot
    // tell the cases apart: a WORD 0 and the C string "0" are both two bytes,
    // and "no" is two bytes that must not be read as the integer 0x6F6E.
    // Printable ASCII padded with NULs is text; anything holding a control byte
    // is an integer. No application writes 0x20202020 as a boolean.
    bool looksLikeText = true;
    std::string text;
    text.reserve(payload.size());
    for (uint8_t b : payload) {
        if (b == 0) {
            continue;  // string terminator, or the high byte of UTF-16LE
        }
        if (b < 0x20 || b > 0x7E) {
            looksLikeText = false;
            break;
        }
        text += AsciiLower(static_cast<char>(b));
    }
    if (looksLikeText && !text.empty()) {
        // Dropping every NUL rather than stopping at the first one is what lets
        // ASCII "false" and UTF-16LE L"false" collapse to the same word here.
        return text == "0" || text == "false" || text == "no";
    }

    // Integer: a DWORD is what the format is documented as carrying, and a WORD
    // shows up often enough in the wild to be worth accepting.
    if (payload.size() == sizeof(uint32_t) || payload.size() == sizeof(uint16_t)) {
        uint32_t v = 0;
        memcpy(&v, payload.data(), payload.size());
        return v == 0;
    }

    // Neither a recognised spelling nor a recognised width.
    return false;
}

bool ShouldSkip(const Seen* seen) {
    for (size_t i = 0; i < kMarkerCount; ++i) {
        const bool excluded = (kMarkers[i].rule == Rule::AnyPresenceExcludes)
                                  ? PresenceIsExclusion(seen[i])
                                  : FalseValueIsExclusion(seen[i]);
        if (excluded) {
            return true;
        }
    }
    return false;
}

}  // namespace privacy
