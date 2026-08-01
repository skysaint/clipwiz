// util.cpp
#include "util.h"

#include <cstdarg>
#include <cstdio>

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

const std::wstring& DataDir() {
    if (!g_dataDir.empty()) {
        return g_dataDir;
    }
    // 默认就是 exe 所在目录，绿色到极致
    g_dataDir = ExeDir();
    return g_dataDir;
}

void SetDataDir(const std::wstring& dir) {
    if (!dir.empty()) {
        g_dataDir = dir;
    }
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
    bool lastWasSpace = true;  // 顺手吃掉开头空白
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
        t.bgAlt = RGB(40, 48, 62);   // 置顶区：偏蓝底色，和普通行拉开差距
        t.fg = RGB(240, 240, 240);
        t.dim = RGB(150, 150, 150);
        t.sel = RGB(0, 95, 184);
        t.selFg = RGB(255, 255, 255);
        t.line = RGB(60, 60, 60);
        t.accent = RGB(120, 175, 255);
    } else {
        t.bg = RGB(252, 252, 252);
        t.bgAlt = RGB(228, 238, 252);  // 置顶区：浅蓝底色，一眼能分出来
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
        // lfHeight 是负值，往下加相当于放大
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
    bool ok;
    if (enable) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --autostart";
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
    MessageBoxW(owner, text.c_str(), AppName(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void InfoBox(HWND owner, const std::wstring& text) {
    MessageBoxW(owner, text.c_str(), AppName(), MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

bool ConfirmBox(HWND owner, const std::wstring& text) {
    return MessageBoxW(owner, text.c_str(), AppName(),
                       MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) == IDYES;
}

}  // namespace util
