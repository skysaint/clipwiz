// util.cpp
#include "util.h"

#include <shlobj.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>

#include "i18n.h"
#include "conflict.h"
#include "store.h"

namespace util {
namespace {

std::wstring g_dataDir;

}  // namespace

const wchar_t* AppName() {
    return L"ClipWiz";
}

std::wstring ExePath() {
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return std::wstring();
        }
        if (len < buf.size() - 1) {
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2);
    }
}

std::wstring ExeDir() {
    std::wstring path = ExePath();
    size_t pos = path.find_last_of(L'\\');
    return pos == std::wstring::npos ? std::wstring() : path.substr(0, pos);
}

// Check if any data file exists next to exe (indicating portable mode)
bool HasPortableMarker(const std::wstring& exeDir) {
    // config.ini or store.dat
    if (GetFileAttributesW((exeDir + L"\\config.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;
    if (GetFileAttributesW((exeDir + L"\\store.dat").c_str()) != INVALID_FILE_ATTRIBUTES)
        return true;
    // lang\*.lng
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((exeDir + L"\\lang\\*.lng").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        FindClose(h);
        return true;
    }
    return false;
}

const std::wstring& DataDir() {
    if (!g_dataDir.empty()) {
        return g_dataDir;
    }
    std::wstring exeDir = ExeDir();
    if (HasPortableMarker(exeDir)) {
        g_dataDir = exeDir;
    } else {
        g_dataDir = AppDataDir();
    }
    return g_dataDir;
}

void SetDataDir(const std::wstring& dir) {
    if (!dir.empty()) {
        g_dataDir = dir;
    }
}

std::wstring AppDataDir() {
    wchar_t path[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, path))) {
        return std::wstring(path) + L"\\ClipWiz";
    }
    // Fallback: exe directory
    return ExeDir();
}

bool IsPortable() {
    return DataDir() == ExeDir();
}

bool MigrateDataDir(bool toPortable) {
    std::wstring src = DataDir();
    std::wstring dst = toPortable ? ExeDir() : AppDataDir();
    if (src == dst) {
        return true;  // Already there
    }
    EnsureDir(dst);

    // Handle store.dat conflict: both sides have one
    std::wstring srcDat = src + L"\\store.dat";
    std::wstring dstDat = dst + L"\\store.dat";
    bool srcHas = GetFileAttributesW(srcDat.c_str()) != INVALID_FILE_ATTRIBUTES;
    bool dstHas = GetFileAttributesW(dstDat.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (srcHas && dstHas) {
        std::wstring userDir = AppDataDir();
        std::wstring progDir = ExeDir();
        ConflictChoice choice = ShowConflictDialog(nullptr, userDir, progDir);
        if (choice == ConflictChoice::Cancel) {
            return false;  // User aborted
        }
        if (choice == ConflictChoice::UseMerged) {
            // Merge both, write to dst, remove src's
            std::vector<Item> leftItems, rightItems;
            Store::LoadItemsFrom(userDir + L"\\store.dat", leftItems);
            Store::LoadItemsFrom(progDir + L"\\store.dat", rightItems);
            std::vector<Item> merged;
            // Pinned items are deliberately NOT deduped — each pinned entry in
            // each store represents an explicit user choice in an independent
            // environment, and losing one looks like "a pinned item vanished"
            // after a merge (reported issue scenario). Unpinned history is
            // still deduped by content hash to avoid doubling archive size.
            std::vector<uint64_t> seenUnpinned;
            auto addPinned = [&](const std::vector<Item>& items) {
                for (const Item& it : items) {
                    if (it.pinned) merged.push_back(it);
                }
            };
            auto addUnpinnedUnique = [&](const std::vector<Item>& items) {
                for (const Item& it : items) {
                    if (it.pinned) continue;
                    bool dup = false;
                    for (uint64_t h : seenUnpinned) {
                        if (h == it.hash) { dup = true; break; }
                    }
                    if (!dup) {
                        seenUnpinned.push_back(it.hash);
                        merged.push_back(it);
                    }
                }
            };
            std::vector<Item> lp, rp, lu, ru;
            for (auto& it : leftItems) { if (it.pinned) lp.push_back(std::move(it)); else lu.push_back(std::move(it)); }
            for (auto& it : rightItems) { if (it.pinned) rp.push_back(std::move(it)); else ru.push_back(std::move(it)); }
            addPinned(lp);
            addPinned(rp);
            addUnpinnedUnique(lu);
            addUnpinnedUnique(ru);
            // Reassign all ids monotonically to eliminate potential id
            // collisions between the two independent stores (each side owns
            // its own id counter space).
            uint64_t nextId = 1;
            for (Item& it : merged) {
                it.id = nextId++;
            }
            // Serialize and write to dst
            std::vector<uint8_t> buf = Store::SerializeItems(merged);
            WriteFileAtomic(dstDat, buf.data(), buf.size());
            // Remove src's so it won't be moved over
            DeleteFileW(srcDat.c_str());
        } else {
            // UseLeft = keep user dir, UseRight = keep program dir
            std::wstring keepDir = (choice == ConflictChoice::UseLeft) ? userDir : progDir;
            if (keepDir == dst) {
                // Chosen data is already at destination, delete source's
                DeleteFileW(srcDat.c_str());
            } else {
                // Chosen data is at source, delete destination's (let migration move it)
                DeleteFileW(dstDat.c_str());
            }
        }
    }

    // Build double-null-terminated source file list
    const wchar_t* files[] = {L"config.ini", L"store.dat", L"clipwiz.log"};
    std::wstring fromList;
    for (const wchar_t* f : files) {
        std::wstring full = src + L"\\" + f;
        if (GetFileAttributesW(full.c_str()) != INVALID_FILE_ATTRIBUTES) {
            fromList += full;
            fromList += L'\0';
        }
    }
    if (fromList.empty()) {
        g_dataDir = dst;
        return true;  // Nothing to move
    }
    fromList += L'\0';  // Double-null terminator

    // Destination directory (double-null terminated)
    std::wstring toDir = dst;
    toDir += L'\0';

    SHFILEOPSTRUCTW op = {};
    op.hwnd = nullptr;
    op.wFunc = FO_MOVE;
    op.pFrom = fromList.c_str();
    op.pTo = toDir.c_str();
    op.fFlags = FOF_SIMPLEPROGRESS | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    int result = SHFileOperationW(&op);

    g_dataDir = dst;
    return (result == 0 && !op.fAnyOperationsAborted);
}

std::wstring ConfigPath() {
    return DataDir() + L"\\config.ini";
}

std::wstring StorePath() {
    return DataDir() + L"\\store.dat";
}

bool EnsureDir(const std::wstring& dir) {
    if (dir.empty()) {
        return false;
    }
    DWORD attr = GetFileAttributesW(dir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) {
        return (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    size_t pos = dir.find_last_of(L'\\');
    if (pos != std::wstring::npos && pos > 2) {
        EnsureDir(dir.substr(0, pos));
    }
    return CreateDirectoryW(dir.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>& out) {
    out.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > (256 << 20)) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    bool ok = true;
    while (done < out.size()) {
        DWORD chunk = static_cast<DWORD>((out.size() - done) > (1u << 20) ? (1u << 20)
                                                                         : (out.size() - done));
        DWORD read = 0;
        if (!ReadFile(h, out.data() + done, chunk, &read, nullptr) || read == 0) {
            ok = false;
            break;
        }
        done += read;
    }
    CloseHandle(h);
    if (!ok) {
        out.clear();
    }
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const void* data, size_t size) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t done = 0;
    bool ok = true;
    while (done < size) {
        DWORD chunk = static_cast<DWORD>((size - done) > (1u << 20) ? (1u << 20) : (size - done));
        DWORD written = 0;
        if (!WriteFile(h, p + done, chunk, &written, nullptr) || written != chunk) {
            ok = false;
            break;
        }
        done += written;
    }
    if (ok) {
        ok = FlushFileBuffers(h) != 0;
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

uint64_t FileSizeOf(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        return 0;
    }
    return (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
}

uint64_t NowFileTime() {
    FILETIME ft{};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::wstring TimeStampForFileName() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    return Format(L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                  st.wSecond);
}


uint64_t Hash64(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint64_t h = 1469598103934665603ULL;  // FNV-1a 64
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

std::wstring Format(const wchar_t* fmt, ...) {
    wchar_t stack[512];
    va_list args;
    va_start(args, fmt);
    int need = _vsnwprintf_s(stack, _countof(stack), _TRUNCATE, fmt, args);
    va_end(args);
    if (need >= 0) {
        return std::wstring(stack);
    }
    std::vector<wchar_t> heap(8192);
    va_start(args, fmt);
    _vsnwprintf_s(heap.data(), heap.size(), _TRUNCATE, fmt, args);
    va_end(args);
    return std::wstring(heap.data());
}

std::wstring OneLinePreview(const std::wstring& text, size_t maxLen) {
    std::wstring out;
    out.reserve(text.size() < maxLen ? text.size() : maxLen + 1);
    bool lastWasSpace = true;  // Also eat leading whitespace
    for (wchar_t ch : text) {
        wchar_t c = ch;
        if (c == L'\r' || c == L'\n' || c == L'\t' || c == 0x0b || c == 0x0c) {
            c = L' ';
        } else if (c < 0x20) {
            c = L' ';
        }
        if (c == L' ') {
            if (lastWasSpace) {
                continue;
            }
            lastWasSpace = true;
        } else {
            lastWasSpace = false;
        }
        out.push_back(c);
        if (out.size() >= maxLen) {
            out.push_back(L'…');
            break;
        }
    }
    while (!out.empty() && out.back() == L' ') {
        out.pop_back();
    }
    return out;
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring ws(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), ws.data(), n);
    return ws;
}

std::string Narrow(const std::wstring& ws) {
    if (ws.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), static_cast<int>(ws.size()), s.data(), n, nullptr,
                        nullptr);
    return s;
}

bool IsSystemDarkMode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value,
                     &size) != ERROR_SUCCESS) {
        return false;
    }
    return value == 0;
}

Theme MakeTheme(bool dark) {
    Theme t{};
    if (dark) {
        t.bg = RGB(32, 32, 32);
        t.bgAlt = RGB(40, 48, 62);
        t.fg = RGB(240, 240, 240);
        t.dim = RGB(150, 150, 150);
        t.sel = RGB(0, 95, 184);
        t.selFg = RGB(255, 255, 255);
        t.line = RGB(60, 60, 60);
        t.accent = RGB(120, 175, 255);
    } else {
        t.bg = RGB(252, 252, 252);
        t.bgAlt = RGB(228, 238, 252);  // Pinned row background: #e4eefc
        t.fg = RGB(28, 28, 28);
        t.dim = RGB(120, 120, 120);
        t.sel = RGB(0, 120, 215);
        t.selFg = RGB(255, 255, 255);
        t.line = RGB(226, 226, 226);
        t.accent = RGB(21, 96, 189);
    }
    return t;
}

int DpiOf(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : GetDpiForSystem();
    return dpi == 0 ? 96 : static_cast<int>(dpi);
}

int Scale(int dip, int dpi) {
    return MulDiv(dip, dpi, 96);
}

HFONT CreateUiFont(int dpi, int pointDelta) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                    static_cast<UINT>(dpi))) {
        ncm.cbSize = sizeof(ncm);
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            return nullptr;
        }
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, dpi, 96);
    }
    LOGFONTW lf = ncm.lfMessageFont;
    if (pointDelta != 0) {
        // lfHeight is negative, going down means increasing
        int delta = MulDiv(pointDelta, dpi, 72);
        lf.lfHeight -= delta;
    }
    return CreateFontIndirectW(&lf);
}

HFONT CreateUiFont(int dpi, const std::wstring& faceName, int pointSize, int pointDelta) {
    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0,
                                    static_cast<UINT>(dpi))) {
        ncm.cbSize = sizeof(ncm);
        if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            return nullptr;
        }
        ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, dpi, 96);
    }
    LOGFONTW lf = ncm.lfMessageFont;
    if (!faceName.empty()) {
        wcsncpy_s(lf.lfFaceName, faceName.c_str(), _TRUNCATE);
    }
    if (pointSize > 0) {
        lf.lfHeight = -MulDiv(pointSize, dpi, 72);
    }
    if (pointDelta != 0) {
        int delta = MulDiv(pointDelta, dpi, 72);
        lf.lfHeight -= delta;
    }
    return CreateFontIndirectW(&lf);
}

bool SetAutostart(bool enable) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    bool ok = false;
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --autostart";
        if (cmd.empty() || cmd.find(L"\"") == std::wstring::npos) {
            RegCloseKey(key);
            return false;  // Invalid path
        }
        ok = RegSetValueExW(key, AppName(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(cmd.c_str()),
                            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        LSTATUS st = RegDeleteValueW(key, AppName());
        ok = (st == ERROR_SUCCESS || st == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok;
}

bool GetAutostart() {
    wchar_t buf[1024]{};
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", AppName(),
                        RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS;
}

void ErrorBox(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), AppName(), MB_OK | MB_ICONERROR | MB_TOPMOST);
}

void InfoBox(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), AppName(), MB_OK | MB_ICONINFORMATION | MB_TOPMOST);
}

bool ConfirmBox(HWND owner, const std::wstring& text) {
    return MessageBoxW(owner, text.c_str(), AppName(),
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TOPMOST) == IDYES;
}

}  // namespace util
