// clipboard.cpp
#include "clipboard.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstring>

#include "imagecodec.h"
#include "log.h"
#include "raii.h"
#include "util.h"

namespace clip {
namespace {

DWORD g_selfSeq = 0;

UINT g_fmtRtf = 0;
UINT g_fmtRtfNoObj = 0;
UINT g_fmtHtml = 0;
UINT g_fmtPng = 0;
UINT g_fmtIgnore1 = 0;
UINT g_fmtIgnore2 = 0;

void EnsureFormats() {
    if (g_fmtRtf == 0) {
        g_fmtRtf = RegisterClipboardFormatW(L"Rich Text Format");
        g_fmtRtfNoObj = RegisterClipboardFormatW(L"Rich Text Format Without Objects");
        g_fmtHtml = RegisterClipboardFormatW(L"HTML Format");
        g_fmtPng = RegisterClipboardFormatW(L"PNG");
        g_fmtIgnore1 = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
        g_fmtIgnore2 = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    }
}

// Read all bytes of a registered format
bool GetFormatBytes(UINT fmt, std::vector<uint8_t>& out, uint32_t maxBytes) {
    HANDLE h = GetClipboardData(fmt);
    if (!h) {
        return false;
    }
    SIZE_T size = GlobalSize(h);
    if (size == 0 || size > maxBytes) {
        return false;
    }
    
    raii::GlobalLockGuard lock(h);
    if (!lock) {
        return false;
    }
    
    const void* ptr = lock.get();
    out.assign(static_cast<const uint8_t*>(ptr), static_cast<const uint8_t*>(ptr) + size);
    return !out.empty();
}

// Read CF_UNICODETEXT
bool GetText(std::wstring& out, uint32_t maxBytes) {
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        return false;
    }
    SIZE_T size = GlobalSize(h);
    if (size == 0 || size > maxBytes) {
        return false;
    }
    
    raii::GlobalLockGuard lock(h);
    if (!lock) {
        return false;
    }
    
    const wchar_t* ptr = static_cast<const wchar_t*>(lock.get());
    out.assign(ptr);  // up to NUL terminator
    return !out.empty();
}

// Read CF_HDROP -> path list (one per line)
bool GetFileDrop(std::vector<uint8_t>& out) {
    HANDLE h = GetClipboardData(CF_HDROP);
    if (!h) {
        return false;
    }
    
    raii::GlobalLockGuard lock(h);
    if (!lock) {
        return false;
    }
    
    HDROP drop = static_cast<HDROP>(lock.get());
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0 || count > 10000) {
        return false;
    }
    std::wstring paths;
    for (UINT i = 0; i < count; ++i) {
        UINT len = DragQueryFileW(drop, i, nullptr, 0);
        if (len == 0) {
            continue;
        }
        std::wstring file(len, L'\0');
        DragQueryFileW(drop, i, file.data(), len + 1);
        paths += file;
        paths += L'\n';
    }
    if (paths.empty()) {
        return false;
    }
    // Store as UTF-16LE bytes
    out.assign(reinterpret_cast<const uint8_t*>(paths.data()),
               reinterpret_cast<const uint8_t*>(paths.data()) + paths.size() * sizeof(wchar_t));
    return true;
}

// Read image (CF_DIBV5 / CF_DIB / CF_BITMAP) -> PNG
bool GetImage(std::vector<uint8_t>& png, uint32_t& w, uint32_t& h, uint32_t maxPixels) {
    // Prefer DIBV5 / DIB
    HANDLE hMem = GetClipboardData(CF_DIBV5);
    if (!hMem) {
        hMem = GetClipboardData(CF_DIB);
    }
    if (hMem) {
        SIZE_T size = GlobalSize(hMem);
        raii::GlobalLockGuard lock(hMem);
        if (lock && size > 0) {
            const void* ptr = lock.get();
            bool ok = imagecodec::DibToPng(ptr, size, png, w, h);
            if (ok) {
                if (static_cast<uint64_t>(w) * h > maxPixels) {
                    png.clear();
                    return false;
                }
                return true;
            }
        }
    }
    // Fallback: CF_BITMAP
    HANDLE hb = GetClipboardData(CF_BITMAP);
    if (!hb) {
        return false;
    }
    HBITMAP bitmap = static_cast<HBITMAP>(hb);
    if (!imagecodec::HBitmapToPng(bitmap, png, w, h)) {
        return false;
    }
    if (static_cast<uint64_t>(w) * h > maxPixels) {
        png.clear();
        return false;
    }
    return true;
}

// Write a registered format to clipboard
bool SetFormatBytes(UINT fmt, const void* data, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!h) {
        return false;
    }
    
    raii::GlobalLockGuard lock(h);
    if (!lock) {
        GlobalFree(h);
        return false;
    }
    
    void* ptr = lock.get();
    memcpy(ptr, data, size);
    
    if (!SetClipboardData(fmt, h)) {
        GlobalFree(h);
        return false;
    }
    return true;
}

bool SetUnicodeText(const std::wstring& text) {
    if (text.empty()) {
        return false;
    }
    
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        return false;
    }
    
    raii::GlobalLockGuard lock(h);
    if (!lock) {
        GlobalFree(h);
        return false;
    }
    
    wchar_t* ptr = static_cast<wchar_t*>(lock.get());
    memcpy(ptr, text.data(), bytes);
    // Ensure null-terminated
    ptr[text.size()] = L'\0';
    
    if (!SetClipboardData(CF_UNICODETEXT, h)) {
        GlobalFree(h);
        return false;
    }
    return true;
}

bool SetImagePng(const std::vector<uint8_t>& png) {
    std::vector<uint8_t> dibV5;
    std::vector<uint8_t> dib;
    uint32_t w = 0;
    uint32_t h = 0;
    if (!imagecodec::PngToDibs(png.data(), png.size(), dibV5, dib, w, h)) {
        return false;
    }
    SetFormatBytes(CF_DIBV5, dibV5.data(), dibV5.size());
    SetFormatBytes(CF_DIB, dib.data(), dib.size());
    if (g_fmtPng) {
        SetFormatBytes(g_fmtPng, png.data(), png.size());
    }
    return true;
}

bool SetFileDrop(const std::vector<uint8_t>& data) {
    // data is UTF-16LE path list (one per line)
    std::wstring paths(reinterpret_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t));

    // Count files
    UINT count = 0;
    size_t pos = 0;
    while (pos < paths.size()) {
        size_t eol = paths.find(L'\n', pos);
        if (eol == std::wstring::npos) {
            eol = paths.size();
        }
        if (eol > pos) {
            ++count;
        }
        pos = eol + 1;
    }
    if (count == 0) {
        return false;
    }

    // Build DROPFILES structure
    // DROPFILES followed by double-NUL terminated multi-string
    size_t charsLen = 0;
    pos = 0;
    while (pos < paths.size()) {
        size_t eol = paths.find(L'\n', pos);
        if (eol == std::wstring::npos) {
            eol = paths.size();
        }
        size_t lineLen = eol - pos;
        // Strip \r
        while (lineLen > 0 && paths[pos + lineLen - 1] == L'\r') {
            --lineLen;
        }
        if (lineLen > 0) {
            charsLen += lineLen + 1;  // NUL after each entry
        }
        pos = eol + 1;
    }
    charsLen += 1;  // Final extra NUL

    const size_t totalSize = sizeof(DROPFILES) + charsLen * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSize);
    if (!h) {
        return false;
    }
    
    raii::GlobalLockGuard lock(h);
    if (!lock) {
        GlobalFree(h);
        return false;
    }
    
    DROPFILES* df = static_cast<DROPFILES*>(lock.get());
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    wchar_t* dst = reinterpret_cast<wchar_t*>(reinterpret_cast<uint8_t*>(df) + sizeof(DROPFILES));
    pos = 0;
    while (pos < paths.size()) {
        size_t eol = paths.find(L'\n', pos);
        if (eol == std::wstring::npos) {
            eol = paths.size();
        }
        size_t lineLen = eol - pos;
        while (lineLen > 0 && paths[pos + lineLen - 1] == L'\r') {
            --lineLen;
        }
        if (lineLen > 0) {
            memcpy(dst, paths.data() + pos, lineLen * sizeof(wchar_t));
            dst += lineLen;
            *dst++ = L'\0';
        }
        pos = eol + 1;
    }
    *dst = L'\0';
    
    if (!SetClipboardData(CF_HDROP, h)) {
        GlobalFree(h);
        return false;
    }
    return true;
}

}  // namespace

bool StartListening(HWND hwnd) {
    EnsureFormats();
    return AddClipboardFormatListener(hwnd) != 0;
}

void StopListening(HWND hwnd) {
    RemoveClipboardFormatListener(hwnd);
}

bool IsSelfWrite() {
    DWORD seq = GetClipboardSequenceNumber();
    if (g_selfSeq != 0 && seq == g_selfSeq) {
        return true;
    }
    return false;
}

bool Capture(ItemKind& kind, std::vector<uint8_t>& data, uint32_t& imgW, uint32_t& imgH,
             uint32_t maxTextBytes, uint32_t maxImagePixels) {
    EnsureFormats();
    kind = ItemKind::Text;
    data.clear();
    imgW = 0;
    imgH = 0;

    // Retry briefly: Office apps may still hold the clipboard right after a copy
    bool opened = false;
    int attempt = 0;
    for (; attempt < 4; ++attempt) {
        if (OpenClipboard(nullptr)) {
            opened = true;
            break;
        }
        Sleep(20);
    }
    if (!opened) {
        LOG_WARNING("Capture: OpenClipboard failed after retries (clipboard busy)");
        return false;
    }
    if (attempt > 0) {
        LOG_INFO("Capture: OpenClipboard succeeded after %d retries", attempt);
    }

    // Exclusion flags: some programs (password managers) don't want to be recorded
    if ((g_fmtIgnore1 && IsClipboardFormatAvailable(g_fmtIgnore1)) ||
        (g_fmtIgnore2 && IsClipboardFormatAvailable(g_fmtIgnore2))) {
        CloseClipboard();
        return false;
    }

    bool got = false;
    std::vector<uint8_t> dibRaw;  // Raw DIB bytes, converted to PNG after CloseClipboard

    // Priority 1: RTF
    if (!got && g_fmtRtf && IsClipboardFormatAvailable(g_fmtRtf)) {
        if (GetFormatBytes(g_fmtRtf, data, maxTextBytes)) {
            kind = ItemKind::Rtf;
            got = true;
        }
    }
    // Priority 2: HTML
    if (!got && g_fmtHtml && IsClipboardFormatAvailable(g_fmtHtml)) {
        if (GetFormatBytes(g_fmtHtml, data, maxTextBytes)) {
            kind = ItemKind::Html;
            got = true;
        }
    }
    // Priority 3: Image — copy raw DIB bytes now, do heavy PNG conversion after release
    if (!got && (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB) ||
                 IsClipboardFormatAvailable(CF_BITMAP))) {
        HANDLE hMem = GetClipboardData(CF_DIBV5);
        if (!hMem) {
            hMem = GetClipboardData(CF_DIB);
        }
        if (hMem) {
            SIZE_T size = GlobalSize(hMem);
            raii::GlobalLockGuard lock(hMem);
            if (lock && size > 0 && size <= 256u * 1024u * 1024u) {
                const uint8_t* ptr = static_cast<const uint8_t*>(lock.get());
                dibRaw.assign(ptr, ptr + size);
                got = true;
            }
        }
        if (!got && IsClipboardFormatAvailable(CF_BITMAP)) {
            // Rare fallback: HBITMAP is only usable while the clipboard is open
            std::vector<uint8_t> png;
            uint32_t w = 0;
            uint32_t h = 0;
            if (GetImage(png, w, h, maxImagePixels)) {
                kind = ItemKind::Image;
                data = std::move(png);
                imgW = w;
                imgH = h;
                got = true;
            }
        }
    }
    // Priority 4: File list
    if (!got && IsClipboardFormatAvailable(CF_HDROP)) {
        if (GetFileDrop(data)) {
            kind = ItemKind::FileDrop;
            got = true;
        }
    }
    // Priority 5: Plain text
    if (!got && IsClipboardFormatAvailable(CF_UNICODETEXT)) {
        std::wstring text;
        if (GetText(text, maxTextBytes)) {
            kind = ItemKind::Text;
            data.assign(reinterpret_cast<const uint8_t*>(text.data()),
                        reinterpret_cast<const uint8_t*>(text.data()) +
                            text.size() * sizeof(wchar_t));
            got = true;
        }
    }

    CloseClipboard();

    // Convert DIB to PNG outside the clipboard lock (no longer blocking other apps)
    if (!dibRaw.empty()) {
        std::vector<uint8_t> png;
        uint32_t w = 0;
        uint32_t h = 0;
        if (imagecodec::DibToPng(dibRaw.data(), dibRaw.size(), png, w, h) &&
            static_cast<uint64_t>(w) * h <= maxImagePixels) {
            kind = ItemKind::Image;
            data = std::move(png);
            imgW = w;
            imgH = h;
            return true;
        }
        return false;
    }
    return got;
}

bool WriteItem(HWND owner, const Item& item) {
    EnsureFormats();
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();

    bool ok = false;
    switch (item.kind) {
        case ItemKind::Text: {
            std::wstring text(reinterpret_cast<const wchar_t*>(item.data.data()),
                              item.data.size() / sizeof(wchar_t));
            ok = SetUnicodeText(text);
            break;
        }
        case ItemKind::Image:
            ok = SetImagePng(item.data);
            break;
        case ItemKind::Html: {
            ok = SetFormatBytes(g_fmtHtml, item.data.data(), item.data.size());
            // Attach plain text fallback
            std::wstring plain = Store::TextOf(item);
            if (!plain.empty()) {
                SetUnicodeText(plain);
            }
            break;
        }
        case ItemKind::Rtf: {
            ok = SetFormatBytes(g_fmtRtf, item.data.data(), item.data.size());
            if (g_fmtRtfNoObj) {
                SetFormatBytes(g_fmtRtfNoObj, item.data.data(), item.data.size());
            }
            std::wstring plain = Store::TextOf(item);
            if (!plain.empty()) {
                SetUnicodeText(plain);
            }
            break;
        }
        case ItemKind::FileDrop: {
            ok = SetFileDrop(item.data);
            // Attach path text
            std::wstring text(reinterpret_cast<const wchar_t*>(item.data.data()),
                              item.data.size() / sizeof(wchar_t));
            if (!text.empty()) {
                SetUnicodeText(text);
            }
            break;
        }
    }

    // Update self sequence number (changed after write)
    g_selfSeq = GetClipboardSequenceNumber();
    CloseClipboard();
    return ok;
}

}  // namespace clip
