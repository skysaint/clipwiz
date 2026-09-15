// clipboard.cpp
#include "clipboard.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstring>
#include <cwctype>

#include "imagecodec.h"
#include "log.h"
#include "privacy.h"
#include "raii.h"
#include "util.h"

namespace clip {
namespace {

DWORD g_selfSeq = 0;

UINT g_fmtRtf = 0;
UINT g_fmtRtfNoObj = 0;
UINT g_fmtHtml = 0;
UINT g_fmtPng = 0;
// Registered format ids of the exclusion markers, parallel to privacy::kMarkers.
UINT g_fmtMarker[privacy::kMarkerCount] = {};

void EnsureFormats() {
    if (g_fmtRtf == 0) {
        g_fmtRtf = RegisterClipboardFormatW(L"Rich Text Format");
        g_fmtRtfNoObj = RegisterClipboardFormatW(L"Rich Text Format Without Objects");
        g_fmtHtml = RegisterClipboardFormatW(L"HTML Format");
        g_fmtPng = RegisterClipboardFormatW(L"PNG");
        for (size_t i = 0; i < privacy::kMarkerCount; ++i) {
            g_fmtMarker[i] = RegisterClipboardFormatW(privacy::kMarkers[i].name);
        }
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
    bool ok = false;
    ok |= SetFormatBytes(CF_DIBV5, dibV5.data(), dibV5.size());
    ok |= SetFormatBytes(CF_DIB, dib.data(), dib.size());
    if (g_fmtPng) {
        ok |= SetFormatBytes(g_fmtPng, png.data(), png.size());
    }
    return ok;
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

// Full lowercased image path of the process `pid`, e.g. "c:\windows\notepad.exe",
// or empty when it cannot be resolved.
//
// QueryFullProcessImageNameW rather than GetModuleFileNameExW: it is in
// kernel32 and needs only PROCESS_QUERY_LIMITED_INFORMATION, so this reaches
// elevated and protected-process-light targets that GetModuleFileNameExW
// fails on, and it costs no new import library.
std::wstring ProcessPathOfPid(DWORD pid) {
    if (pid == 0) {
        return {};
    }
    raii::HandleGuard proc(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!proc) {
        return {};
    }
    wchar_t path[MAX_PATH * 2] = {};
    DWORD len = static_cast<DWORD>(sizeof(path) / sizeof(path[0]));
    if (!QueryFullProcessImageNameW(proc.get(), 0, path, &len) || len == 0) {
        return {};
    }
    std::wstring full(path, len);
    for (wchar_t& c : full) {
        c = static_cast<wchar_t>(towlower(c));
    }
    return full;
}

// Lowercased file name of the process `pid`, e.g. "notepad.exe", or empty.
std::wstring ProcessNameOfPid(DWORD pid) {
    const std::wstring full = ProcessPathOfPid(pid);
    if (full.empty()) {
        return {};
    }
    const size_t slash = full.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? full : full.substr(slash + 1);
}

// A UWP app's visible window belongs to ApplicationFrameHost.exe, which hosts
// the real app's window as a child. Reporting the frame would label every
// Store app copy "applicationframehost.exe", which tells the user nothing, so
// when that is what we land on, look one level down for a child window owned
// by a different process.
const wchar_t kUwpFrameName[] = L"applicationframehost.exe";

struct ForeignChildSearch {
    DWORD framePid = 0;
    DWORD found = 0;
};

BOOL CALLBACK FindForeignChild(HWND hwnd, LPARAM lparam) {
    auto* search = reinterpret_cast<ForeignChildSearch*>(lparam);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0 && pid != search->framePid) {
        search->found = pid;
        return FALSE;  // stop enumerating, first one wins
    }
    return TRUE;
}

// Process id whose window this really is, resolving a UWP frame down to the
// hosted child process (see kUwpFrameName above). Returns 0 for a null window.
DWORD EffectivePidOfWindow(HWND hwnd) {
    if (!hwnd) {
        return 0;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (ProcessNameOfPid(pid) != kUwpFrameName) {
        return pid;
    }
    ForeignChildSearch search{pid, 0};
    EnumChildWindows(hwnd, FindForeignChild, reinterpret_cast<LPARAM>(&search));
    return (search.found == 0) ? pid : search.found;
}

// Build the full blocklist match target for a window: process name, full image
// path, and window title, all lowercased. Name and path come from the same
// effective process (UWP-aware) so they never disagree; if that process cannot
// be resolved, both fall back to the window's own process.
blocklist::Target TargetOfWindow(HWND hwnd) {
    blocklist::Target t;
    if (!hwnd) {
        return t;
    }
    DWORD pid = EffectivePidOfWindow(hwnd);
    t.processName = ProcessNameOfPid(pid);
    t.processPath = ProcessPathOfPid(pid);
    if (t.processName.empty()) {
        DWORD own = 0;
        GetWindowThreadProcessId(hwnd, &own);
        if (own != 0 && own != pid) {
            t.processName = ProcessNameOfPid(own);
            t.processPath = ProcessPathOfPid(own);
        }
    }
    // Title is best-effort: GetWindowTextW on a foreign window can be slow if
    // that window is hung, but Windows bounds the wait, and an empty title just
    // means title-based rules cannot fire for it.
    const int len = GetWindowTextLengthW(hwnd);
    if (len > 0) {
        std::wstring title(static_cast<size_t>(len) + 1, L'\0');
        GetWindowTextW(hwnd, title.data(), len + 1);
        title.resize(static_cast<size_t>(len));
        for (wchar_t& c : title) {
            c = static_cast<wchar_t>(towlower(c));
        }
        t.windowTitle = std::move(title);
    }
    return t;
}

// Decide whether the clipboard currently carries an exclusion marker. Called
// with the clipboard open; `seqAtEntry` is the sequence number captured right
// after opening it.
//
// The sequence number is re-checked once the payloads are in hand. Holding the
// clipboard open does not stop another thread that already had it from
// replacing the content, so without the re-check a marker read from the new
// content could end up judging a payload taken from the old one. Dropping the
// capture on mismatch costs nothing: the replacement fires its own
// WM_CLIPBOARDUPDATE and gets captured on its own terms.
bool ExcludedByMarker(DWORD seqAtEntry) {
    privacy::Seen seen[privacy::kMarkerCount];
    for (size_t i = 0; i < privacy::kMarkerCount; ++i) {
        const UINT fmt = g_fmtMarker[i];
        if (fmt == 0 || !IsClipboardFormatAvailable(fmt)) {
            continue;
        }
        seen[i].present = true;
        // Only lock and copy the payload for markers whose rule reads it. A
        // presence marker is decided by IsClipboardFormatAvailable alone, and
        // 64 bytes is far more than any documented boolean spelling needs.
        if (privacy::kMarkers[i].rule == privacy::Rule::OnlyFalseValueExcludes) {
            GetFormatBytes(fmt, seen[i].payload, 64);
        }
    }

    if (GetClipboardSequenceNumber() != seqAtEntry) {
        LOG_INFO("Capture: clipboard changed while reading exclusion markers, dropping");
        return true;
    }
    return privacy::ShouldSkip(seen);
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

blocklist::Target ResolveTarget(HWND hwnd) {
    return TargetOfWindow(hwnd);
}

bool Capture(ItemKind& kind, std::vector<uint8_t>& data, uint32_t& imgW, uint32_t& imgH,
             std::wstring& sourceApp, const blocklist::RuleSet& block, uint32_t maxTextBytes,
             uint32_t maxImagePixels) {
    EnsureFormats();
    kind = ItemKind::Text;
    data.clear();
    imgW = 0;
    imgH = 0;
    sourceApp.clear();

    bool got = false;
    std::vector<uint8_t> dibRaw;  // Raw DIB bytes, converted to PNG once the clipboard is closed

    {
        // Scoped deliberately: the clipboard must be provably closed before the
        // PNG conversion below. Converting a large DIB takes tens of
        // milliseconds, and holding the clipboard that long blocks every other
        // application on the desktop from copying.
        //
        // Four attempts 20ms apart because Office and some installer UIs still
        // hold the clipboard right after the copy that woke us up, and losing
        // that copy outright is worse than waiting for it.
        raii::ClipboardOpenGuard clip(nullptr, 4, 20);
        if (!clip) {
            LOG_WARNING("Capture: OpenClipboard failed after retries (clipboard busy)");
            return false;
        }
        if (clip.retries() > 0) {
            LOG_INFO("Capture: OpenClipboard succeeded after %d retries", clip.retries());
        }

        // Where did this copy come from? Resolved while the clipboard is open:
        // GetClipboardOwner() only describes the content we are about to read,
        // and the owner window can be destroyed the moment we let go. The owner
        // is the program that put the data there, which is what the user means
        // by "copied from"; the foreground window is the fallback for the cases
        // where ownership was claimed with a NULL window.
        blocklist::Target target = TargetOfWindow(GetClipboardOwner());
        if (target.processName.empty()) {
            target = TargetOfWindow(GetForegroundWindow());
        }
        sourceApp = target.processName;

        // Blocklist first: an app the user has suppressed should cost one window
        // lookup, not a full RTF/HTML/image extraction that gets thrown away.
        // Any matching rule — NoCapture or FullDisable — drops the copy here;
        // FullDisable additionally gates the hotkeys in App::OnHotkey.
        if (blocklist::Classify(block, target) != blocklist::Action::None) {
            LOG_INFO("Capture: source app is on the blocklist, dropping");
            return false;
        }

        // Exclusion markers are checked before any content is read: an excluded
        // copy should cost one clipboard round trip, not a full RTF/HTML
        // extraction that gets thrown away afterwards.
        if (ExcludedByMarker(GetClipboardSequenceNumber())) {
            return false;
        }

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
    }

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
    raii::ClipboardOpenGuard clip(owner);
    if (!clip) {
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

    // Update self sequence number (changed after write). Read before the guard
    // closes the clipboard: this is the number the resulting WM_CLIPBOARDUPDATE
    // will carry, and IsSelfWrite() matches on it to ignore our own paste.
    g_selfSeq = GetClipboardSequenceNumber();
    return ok;
}

}  // namespace clip
