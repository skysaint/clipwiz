// mask.h — Preview desensitization (hand-written scanners, no regex)
//
// Finds sensitive spans in clipboard text and returns a copy with each span's
// middle characters replaced by a fixed mask. The original item bytes are never
// touched: masking is applied only to the derived preview / searchText and the
// hover preview, so what actually gets pasted is always the full content. The
// point is to keep a secret from being *readable on screen*, not to remove it.
//
// Pure logic — no Win32, no clipboard, no config access. Callers pass plain
// strings in and get plain strings out, so tests/test_mask.cpp runs headless.
//
// Deliberately no std::regex: the patterns below are simple enough to scan by
// hand, a hand scanner is faster than std::regex on every keystroke-driven
// recompute, and it costs none of std::regex's code size.
#pragma once

#include <string>

namespace mask {

// Which scanners run. Each is independent and user-toggleable. The password
// heuristic in particular has a high false-positive rate — plenty of hashes,
// base64 fragments and URLs satisfy "has upper, lower, digit and symbol" — so
// it must be switchable on its own without disturbing the rest.
//
// Defaults follow the plan: phone / ID card / password on, email / API key off.
struct Config {
    bool email = false;    // local@domain.tld
    bool phone = true;     // Chinese mainland mobile: 1[3-9] then 9 digits
    bool idCard = true;    // 18-digit resident ID, birth date and checksum verified
    bool apiKey = false;   // sk-/pk-/ghp_/AKIA/AIza/... prefix + separator + 20+ [\w-]
    bool password = true;  // whole text is 8-64 chars, no whitespace, upper+lower+digit+symbol
};

// True if at least one scanner is enabled. Lets a caller skip the whole pass —
// and any text extraction that pass would need — when masking is switched off,
// so the no-masking path stays exactly as cheap as it was before this existed.
bool AnyEnabled(const Config& cfg);

// Return `text` with every detected sensitive span masked. Two guard rails,
// both carried over from TieZ: text longer than 5000 characters, or starting
// with "data:", is returned unchanged. A huge blob is not worth scanning on a
// UI recompute, and a data URI is machine-generated content, not a secret a
// user typed, so masking it would only mangle something nobody reads anyway.
//
// Detection is case-sensitive where the pattern is (API-key prefixes, and the
// password heuristic's character classes). A caller that lowercases its text —
// searchText does — must therefore mask FIRST and lowercase AFTER; lowercasing
// first would erase the very case the scanners key on.
std::wstring Apply(const std::wstring& text, const Config& cfg);

}  // namespace mask
