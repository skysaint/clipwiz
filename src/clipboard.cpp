// clipboard.cpp
#include "clipboard.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cstring>

#include "imagecodec.h"
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

// 读一个注册格式的全部字节
bool GetFormatBytes(UINT fmt, std::vector<uint8_t>& out, uint32_t maxBytes) {
    HANDLE h = GetClipboardData(fmt);
    if (!h) {
        return false;
    }
    SIZE_T size = GlobalSize(h);
    if (size == 0 || size > maxBytes) {
        return false;
    }
    const void* ptr = GlobalLock(h);
    if (!ptr) {
        return false;
    }
    out.assign(static_cast<const uint8_t*>(ptr), static_cast<const uint8_t*>(ptr) + size);
    GlobalUnlock(h);
    return !out.empty();
}

// 读 CF_UNICODETEXT
bool GetText(std::wstring& out, uint32_t maxBytes) {
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (!h) {
        return false;
    }
    SIZE_T size = GlobalSize(h);
    if (size == 0 || size > maxBytes) {
        return false;
    }
    const wchar_t* ptr = static_cast<const wchar_t*>(GlobalLock(h));
    if (!ptr) {
        return false;
    }
    out.assign(ptr);  // 到 NUL 为止
    GlobalUnlock(h);
    return !out.empty();
}

// 读 CF_HDROP → 路径列表（每行一个）
bool GetFileDrop(std::vector<uint8_t>& out) {
    HANDLE h = GetClipboardData(CF_HDROP);
    if (!h) {
        return false;
    }
    HDROP drop = static_cast<HDROP>(GlobalLock(h));
    if (!drop) {
        return false;
    }
    UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    if (count == 0 || count > 10000) {
        GlobalUnlock(h);
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
    GlobalUnlock(h);
    if (paths.empty()) {
        return false;
    }
    // 存为 UTF-16LE 字节
    out.assign(reinterpret_cast<const uint8_t*>(paths.data()),
               reinterpret_cast<const uint8_t*>(paths.data()) + paths.size() * sizeof(wchar_t));
    return true;
}

// 读图片（CF_DIBV5 / CF_DIB / CF_BITMAP）→ PNG
bool GetImage(std::vector<uint8_t>& png, uint32_t& w, uint32_t& h, uint32_t maxPixels) {
    // 优先 DIBV5 / DIB
    HANDLE hMem = GetClipboardData(CF_DIBV5);
    if (!hMem) {
        hMem = GetClipboardData(CF_DIB);
    }
    if (hMem) {
        SIZE_T size = GlobalSize(hMem);
        const void* ptr = GlobalLock(hMem);
        if (ptr && size > 0) {
            bool ok = imagecodec::DibToPng(ptr, size, png, w, h);
            GlobalUnlock(hMem);
            if (ok) {
                if (static_cast<uint64_t>(w) * h > maxPixels) {
                    png.clear();
                    return false;
                }
                return true;
            }
        } else {
            if (ptr) {
                GlobalUnlock(hMem);
            }
        }
    }
    // 兜底 CF_BITMAP
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

// 往剪贴板写一个注册格式
bool SetFormatBytes(UINT fmt, const void* data, size_t size) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!h) {
        return false;
    }
    void* ptr = GlobalLock(h);
    if (!ptr) {
        GlobalFree(h);
        return false;
    }
    memcpy(ptr, data, size);
    GlobalUnlock(h);
    if (!SetClipboardData(fmt, h)) {
        GlobalFree(h);
        return false;
    }
    return true;
}

bool SetUnicodeText(const std::wstring& text) {
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!h) {
        return false;
    }
    wchar_t* ptr = static_cast<wchar_t*>(GlobalLock(h));
    if (!ptr) {
        GlobalFree(h);
        return false;
    }
    memcpy(ptr, text.data(), bytes);
    GlobalUnlock(h);
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
    // data 是 UTF-16LE 路径列表（每行一个）
    std::wstring paths(reinterpret_cast<const wchar_t*>(data.data()), data.size() / sizeof(wchar_t));

    // 数文件数
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

    // 构造 DROPFILES
    // DROPFILES 结构后面跟双 NUL 结尾的多字符串
    size_t charsLen = 0;
    pos = 0;
    while (pos < paths.size()) {
        size_t eol = paths.find(L'\n', pos);
        if (eol == std::wstring::npos) {
            eol = paths.size();
        }
        size_t lineLen = eol - pos;
        // 去掉 \r
        while (lineLen > 0 && paths[pos + lineLen - 1] == L'\r') {
            --lineLen;
        }
        if (lineLen > 0) {
            charsLen += lineLen + 1;  // 每条后面一个 NUL
        }
        pos = eol + 1;
    }
    charsLen += 1;  // 最终额外 NUL

    const size_t totalSize = sizeof(DROPFILES) + charsLen * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSize);
    if (!h) {
        return false;
    }
    DROPFILES* df = static_cast<DROPFILES*>(GlobalLock(h));
    if (!df) {
        GlobalFree(h);
        return false;
    }
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
    GlobalUnlock(h);
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

    if (!OpenClipboard(nullptr)) {
        return false;
    }

    // 排除标记：有些程序（密码管理器）不想被记录
    if ((g_fmtIgnore1 && IsClipboardFormatAvailable(g_fmtIgnore1)) ||
        (g_fmtIgnore2 && IsClipboardFormatAvailable(g_fmtIgnore2))) {
        CloseClipboard();
        return false;
    }

    bool got = false;

    // 优先级 1: RTF
    if (!got && g_fmtRtf && IsClipboardFormatAvailable(g_fmtRtf)) {
        if (GetFormatBytes(g_fmtRtf, data, maxTextBytes)) {
            kind = ItemKind::Rtf;
            got = true;
        }
    }
    // 优先级 2: HTML
    if (!got && g_fmtHtml && IsClipboardFormatAvailable(g_fmtHtml)) {
        if (GetFormatBytes(g_fmtHtml, data, maxTextBytes)) {
            kind = ItemKind::Html;
            got = true;
        }
    }
    // 优先级 3: 图片
    if (!got && (IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB) ||
                 IsClipboardFormatAvailable(CF_BITMAP))) {
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
    // 优先级 4: 文件列表
    if (!got && IsClipboardFormatAvailable(CF_HDROP)) {
        if (GetFileDrop(data)) {
            kind = ItemKind::FileDrop;
            got = true;
        }
    }
    // 优先级 5: 纯文本
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
    return got;
}

bool WriteItem(HWND owner, const Item& item) {
    EnsureFormats();
    if (!OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    g_selfSeq = GetClipboardSequenceNumber();  // 先记一下，SetClipboardData 后会变

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
            // 附带纯文本 fallback
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
            // 附带路径文本
            std::wstring text(reinterpret_cast<const wchar_t*>(item.data.data()),
                              item.data.size() / sizeof(wchar_t));
            if (!text.empty()) {
                SetUnicodeText(text);
            }
            break;
        }
    }

    // 更新自我序列号（写完后序列号变了）
    g_selfSeq = GetClipboardSequenceNumber();
    CloseClipboard();
    return ok;
}

}  // namespace clip
