// textconv.cpp
#include "textconv.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "util.h"

namespace textconv {
namespace {

// Decode a byte buffer using the specified code page, append to text
void DecodeBytes(const std::vector<uint8_t>& buf, UINT codePage, std::wstring& text) {
    if (buf.empty()) {
        return;
    }
    int wlen = MultiByteToWideChar(codePage, 0, reinterpret_cast<const char*>(buf.data()),
                                   static_cast<int>(buf.size()), nullptr, 0);
    if (wlen > 0) {
        size_t oldSize = text.size();
        text.resize(oldSize + static_cast<size_t>(wlen));
        MultiByteToWideChar(codePage, 0, reinterpret_cast<const char*>(buf.data()),
                            static_cast<int>(buf.size()), text.data() + oldSize, wlen);
    }
}

// Destination groups whose content should be entirely skipped
bool IsSkipDestination(const std::string& word) {
    static const char* const kSkip[] = {
        "fonttbl", "colortbl", "stylesheet", "pict", "object",
        "themedata", "listtable", "listoverridetable", "rsidtbl",
        "generator", "info", "latentstyles", "datastore", "mmathPr",
        "wgrffmtfilter", "mso", "xmlnstbl", "pgptbl", "revtbl",
    };
    for (const char* s : kSkip) {
        if (word == s) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::wstring HtmlToPlainText(const std::vector<uint8_t>& data, size_t maxBytes) {
    // HTML Format is UTF-8 text; decode properly
    std::string raw(reinterpret_cast<const char*>(data.data()), data.size());
    // Stop at NUL terminator
    size_t nulPos = raw.find('\0');
    if (nulPos != std::string::npos) {
        raw.resize(nulPos);
    }
    // Find content after <body>
    size_t start = 0;
    size_t bodyPos = raw.find("<body");
    if (bodyPos == std::string::npos) {
        bodyPos = raw.find("<BODY");
    }
    if (bodyPos != std::string::npos) {
        size_t gt = raw.find('>', bodyPos);
        start = (gt != std::string::npos) ? gt + 1 : bodyPos;
    }

    // Helper: append a Unicode code point as UTF-8
    auto appendUtf8 = [](std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    };

    // Strip tags, decode HTML entities
    std::string utf8;
    bool inTag = false;
    for (size_t i = start; i < raw.size() && (maxBytes == 0 || utf8.size() < maxBytes); ++i) {
        char ch = raw[i];
        if (ch == '<') {
            inTag = true;
            continue;
        }
        if (ch == '>') {
            inTag = false;
            continue;
        }
        if (inTag) {
            continue;
        }
        if (ch == '\r' || ch == '\n') {
            if (!utf8.empty() && utf8.back() != ' ') {
                utf8 += ' ';
            }
            continue;
        }
        if (ch == '&') {
            // Try to decode HTML entity
            size_t semi = raw.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 12) {
                std::string ent = raw.substr(i + 1, semi - i - 1);
                bool decoded = true;
                if (ent == "nbsp" || ent == "#160") {
                    utf8 += ' ';
                } else if (ent == "amp") {
                    utf8 += '&';
                } else if (ent == "lt") {
                    utf8 += '<';
                } else if (ent == "gt") {
                    utf8 += '>';
                } else if (ent == "quot") {
                    utf8 += '"';
                } else if (ent == "apos") {
                    utf8 += '\'';
                } else if (ent == "mdash") {
                    appendUtf8(utf8, 0x2014);
                } else if (ent == "ndash") {
                    appendUtf8(utf8, 0x2013);
                } else if (ent == "hellip") {
                    appendUtf8(utf8, 0x2026);
                } else if (!ent.empty() && ent[0] == '#') {
                    uint32_t cp = 0;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        cp = static_cast<uint32_t>(strtoul(ent.c_str() + 2, nullptr, 16));
                    } else {
                        cp = static_cast<uint32_t>(strtoul(ent.c_str() + 1, nullptr, 10));
                    }
                    if (cp > 0 && cp < 0x110000) {
                        appendUtf8(utf8, cp);
                    }
                } else {
                    decoded = false;
                }
                if (decoded) {
                    i = semi;  // Skip past entity
                    continue;
                }
            }
            // Not a recognized entity, output '&' literally
            utf8 += '&';
            continue;
        }
        utf8 += ch;
    }
    // Decode UTF-8 to wide string
    if (utf8.empty()) {
        return {};
    }
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()),
                                   nullptr, 0);
    if (wlen <= 0) {
        return {};
    }
    std::wstring text(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), text.data(), wlen);
    return text;
}

std::wstring RtfToPlainText(const std::vector<uint8_t>& data, size_t maxChars) {
    std::string raw(reinterpret_cast<const char*>(data.data()), data.size());
    // Stop at NUL terminator (clipboard RTF is typically NUL-terminated)
    size_t nulPos = raw.find('\0');
    if (nulPos != std::string::npos) {
        raw.resize(nulPos);
    }

    std::wstring text;
    std::vector<bool> groupSkip;  // Per-depth skip flag
    int skipCount = 0;            // Number of active skip levels
    UINT codePage = CP_ACP;       // Declared by \ansicpgN; default = system ANSI
    int ucSkip = 1;               // Fallback chars after \uNNNN (from \ucN)
    std::vector<uint8_t> pendingBytes;
    size_t i = 0;

    auto flushBytes = [&]() {
        if (!pendingBytes.empty()) {
            DecodeBytes(pendingBytes, codePage, text);
            pendingBytes.clear();
        }
    };

    while (i < raw.size() && (maxChars == 0 || text.size() < maxChars)) {
        char ch = raw[i];

        if (ch == '{') {
            flushBytes();
            ++i;
            // Detect if this group is a skippable destination
            bool skip = false;
            if (i < raw.size() && raw[i] == '\\') {
                if (i + 1 < raw.size() && raw[i + 1] == '*') {
                    // {\*\xxx} - ignorable destination, always skip
                    skip = true;
                } else {
                    // {\word ...} - check known destinations
                    size_t ws = i + 1;
                    size_t we = ws;
                    while (we < raw.size() && isalpha(static_cast<unsigned char>(raw[we]))) {
                        ++we;
                    }
                    std::string w = raw.substr(ws, we - ws);
                    if (w == "pict" && skipCount == 0) {
                        // Embedded image: extract dimensions for placeholder
                        long picW = 0, picH = 0;
                        long goalW = 0, goalH = 0;
                        size_t scan = we;
                        size_t scanEnd = (std::min)(scan + 400, raw.size());
                        while (scan < scanEnd) {
                            if (raw[scan] == '}') {
                                break;
                            }
                            if (raw[scan] == '\\') {
                                ++scan;
                                size_t cs = scan;
                                while (scan < scanEnd &&
                                       isalpha(static_cast<unsigned char>(raw[scan]))) {
                                    ++scan;
                                }
                                std::string cw = raw.substr(cs, scan - cs);
                                std::string cp;
                                if (scan < scanEnd &&
                                    (raw[scan] == '-' ||
                                     isdigit(static_cast<unsigned char>(raw[scan])))) {
                                    size_t ps = scan;
                                    while (scan < scanEnd &&
                                           (raw[scan] == '-' ||
                                            isdigit(static_cast<unsigned char>(raw[scan])))) {
                                        ++scan;
                                    }
                                    cp = raw.substr(ps, scan - ps);
                                }
                                if (cw == "picwgoal" && !cp.empty()) {
                                    goalW = strtol(cp.c_str(), nullptr, 10);
                                } else if (cw == "pichgoal" && !cp.empty()) {
                                    goalH = strtol(cp.c_str(), nullptr, 10);
                                } else if (cw == "picw" && !cp.empty()) {
                                    picW = strtol(cp.c_str(), nullptr, 10);
                                } else if (cw == "pich" && !cp.empty()) {
                                    picH = strtol(cp.c_str(), nullptr, 10);
                                }
                            } else {
                                ++scan;
                            }
                        }
                        // Prefer display goals (twips); fall back to native size
                        long twipsW = goalW > 0 ? goalW : picW;
                        long twipsH = goalH > 0 ? goalH : picH;
                        int pxW = twipsW > 0 ? static_cast<int>(twipsW / 15) : 0;
                        int pxH = twipsH > 0 ? static_cast<int>(twipsH / 15) : 0;
                        if (pxW > 0 && pxH > 0) {
                            wchar_t buf[32];
                            swprintf_s(buf, L"[\x56fe\x7247 %d\x00d7%d]", pxW, pxH);
                            text += buf;
                        } else {
                            text += L"[\x56fe\x7247]";
                        }
                        skip = true;
                    } else if (IsSkipDestination(w)) {
                        skip = true;
                    }
                }
            }
            groupSkip.push_back(skip);
            if (skip) {
                ++skipCount;
            }
            continue;
        }

        if (ch == '}') {
            flushBytes();
            if (!groupSkip.empty()) {
                if (groupSkip.back()) {
                    --skipCount;
                }
                groupSkip.pop_back();
            }
            ++i;
            continue;
        }

        // Skip content inside destination groups
        if (skipCount > 0) {
            ++i;
            continue;
        }

        if (ch == '\\') {
            ++i;
            if (i >= raw.size()) {
                break;
            }
            char next = raw[i];
            if (next == '\\' || next == '{' || next == '}') {
                flushBytes();
                text += static_cast<wchar_t>(next);
                ++i;
                continue;
            }
            if (next == '\'') {
                // \'xx hexadecimal byte in document code page
                ++i;
                if (i + 1 < raw.size()) {
                    char hex[3] = {raw[i], raw[i + 1], 0};
                    int val = static_cast<int>(strtol(hex, nullptr, 16));
                    pendingBytes.push_back(static_cast<uint8_t>(val));
                    i += 2;
                }
                continue;
            }
            if (next == '\r' || next == '\n') {
                ++i;
                continue;
            }
            if (next == '*') {
                // \* outside group open (shouldn't happen normally), skip char
                ++i;
                continue;
            }
            // Control word
            flushBytes();
            size_t wordStart = i;
            while (i < raw.size() && isalpha(static_cast<unsigned char>(raw[i]))) {
                ++i;
            }
            std::string word = raw.substr(wordStart, i - wordStart);
            // Read optional numeric parameter
            std::string param;
            if (i < raw.size() && (raw[i] == '-' || isdigit(static_cast<unsigned char>(raw[i])))) {
                size_t paramStart = i;
                while (i < raw.size() &&
                       (raw[i] == '-' || isdigit(static_cast<unsigned char>(raw[i])))) {
                    ++i;
                }
                param = raw.substr(paramStart, i - paramStart);
            }
            // Space after control word is a delimiter, consume it
            if (i < raw.size() && raw[i] == ' ') {
                ++i;
            }
            if (word == "u" && !param.empty()) {
                // \uNNNN: Unicode character; skip ucSkip fallback characters
                long cp = strtol(param.c_str(), nullptr, 10);
                if (cp < 0) {
                    cp += 65536;  // RTF uses signed 16-bit
                }
                if (cp >= 0x20) {
                    text += static_cast<wchar_t>(cp);
                }
                // Skip fallback characters (\'xx or literal)
                int skipped = 0;
                while (skipped < ucSkip && i < raw.size()) {
                    if (raw[i] == '\\' && i + 1 < raw.size() && raw[i + 1] == '\'') {
                        i += 4;  // Skip \'xx
                        ++skipped;
                    } else if (raw[i] == '\\') {
                        break;  // Another control word, stop skipping
                    } else {
                        ++i;
                        ++skipped;
                    }
                }
            } else if (word == "ansicpg" && !param.empty()) {
                codePage = static_cast<UINT>(strtoul(param.c_str(), nullptr, 10));
            } else if (word == "uc" && !param.empty()) {
                ucSkip = static_cast<int>(strtol(param.c_str(), nullptr, 10));
                if (ucSkip < 0) {
                    ucSkip = 0;
                }
            } else if (word == "par" || word == "line") {
                text += L' ';
            } else if (word == "tab") {
                text += L'\t';
            }
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            ++i;
            continue;
        }
        // Non-ASCII raw byte: accumulate for code page decoding
        if (static_cast<unsigned char>(ch) >= 0x80) {
            pendingBytes.push_back(static_cast<uint8_t>(ch));
        } else {
            flushBytes();
            text += static_cast<wchar_t>(ch);
        }
        ++i;
    }
    flushBytes();
    return text;
}

std::vector<uint8_t> CanonicalBody(ItemKind kind, const std::vector<uint8_t>& data) {
    if (kind == ItemKind::Image) {
        return data;  // No text body: dedup images by raw bytes.
    }
    std::wstring text;
    switch (kind) {
        case ItemKind::Text:
        case ItemKind::FileDrop:
            text.assign(reinterpret_cast<const wchar_t*>(data.data()),
                        data.size() / sizeof(wchar_t));
            break;
        case ItemKind::Html: text = HtmlToPlainText(data); break;
        case ItemKind::Rtf:  text = RtfToPlainText(data);  break;
        default: break;
    }
    return std::vector<uint8_t>(
        reinterpret_cast<const uint8_t*>(text.data()),
        reinterpret_cast<const uint8_t*>(text.data()) + text.size() * sizeof(wchar_t));
}

const char* CanonicalPrefix(ItemKind kind) {
    switch (kind) {
        case ItemKind::Text:     return "TXT:";
        case ItemKind::FileDrop: return "FILE:";
        case ItemKind::Html:     return "HTML:";
        case ItemKind::Rtf:      return "RTF:";
        case ItemKind::Image:    return "IMG:";
        default:                 return "?:";
    }
}

uint64_t CanonicalHash(ItemKind kind, const std::vector<uint8_t>& data) {
    const std::vector<uint8_t> body = CanonicalBody(kind, data);
    const char* prefix = CanonicalPrefix(kind);
    std::vector<uint8_t> buf;
    const size_t plen = std::strlen(prefix);
    buf.reserve(plen + body.size());
    buf.insert(buf.end(), prefix, prefix + plen);
    buf.insert(buf.end(), body.begin(), body.end());
    return util::Hash64(buf.data(), buf.size());
}

bool SameCanonicalContent(const Item& existing, ItemKind kind,
                          const std::vector<uint8_t>& data, uint64_t hash,
                          bool incomingBodyEmpty) {
    if (existing.kind != kind || existing.hash != hash) return false;
    if (!incomingBodyEmpty) return true;     // real body match
    return existing.data == data;            // empty body: require exact bytes
}

bool SameCanonicalContent(const Item& existing, ItemKind kind,
                          const std::vector<uint8_t>& data, uint64_t hash) {
    return SameCanonicalContent(existing, kind, data, hash,
                                CanonicalBody(kind, data).empty());
}

}  // namespace textconv
