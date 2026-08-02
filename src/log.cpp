// log.cpp
#include "log.h"

#include <cstdarg>
#include <cstdio>

#include "util.h"

namespace logger {
namespace {

FILE* g_file = nullptr;
CRITICAL_SECTION g_cs;
Level g_minLevel = Level::Info;
bool g_inited = false;

constexpr int64_t kMaxLogBytes = 1024 * 1024;  // 1 MB

const char* LevelStr(Level level) {
    switch (level) {
        case Level::Debug:   return "DEBUG";
        case Level::Info:    return "INFO ";
        case Level::Warning: return "WARN ";
        case Level::Error:   return "ERROR";
        default:             return "?????";
    }
}

// Format current time as "HH:MM:SS.mmm"
void FormatTime(char* buf, size_t bufSize) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snprintf_s(buf, bufSize, _TRUNCATE, "%02d:%02d:%02d.%03d",
                st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
}

void WriteToFile(const char* line) {
    EnterCriticalSection(&g_cs);
    if (g_file) {
        fprintf(g_file, "%s\n", line);
        fflush(g_file);
    }
    LeaveCriticalSection(&g_cs);
}

}  // namespace

void Init() {
    if (g_inited) return;

    InitializeCriticalSection(&g_cs);

    std::wstring logPath = util::DataDir() + L"\\clipwiz.log";
    util::EnsureDir(util::DataDir());

    // Truncate if log exceeds size limit
    if (util::FileSizeOf(logPath) > static_cast<uint64_t>(kMaxLogBytes)) {
        DeleteFileW(logPath.c_str());
    }

    FILE* f = _wfsopen(logPath.c_str(), L"a", _SH_DENYNO);
    if (f) {
        g_file = f;
        // Startup marker
        SYSTEMTIME st;
        GetLocalTime(&st);
        fprintf(g_file, "\n=== ClipWiz started %04d-%02d-%02d %02d:%02d:%02d ===\n",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_file);
    }

    g_inited = true;
}

void Write(Level level, const char* file, int line, const char* fmt, ...) {
    if (!g_inited || level < g_minLevel) return;

    char timeBuf[32];
    FormatTime(timeBuf, sizeof(timeBuf));

    // Strip path, keep filename only
    const char* baseName = file;
    for (const char* p = file; *p; ++p) {
        if (*p == '/' || *p == '\\') baseName = p + 1;
    }

    char msgBuf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(msgBuf, sizeof(msgBuf), _TRUNCATE, fmt, args);
    va_end(args);

    char lineBuf[1024];
    _snprintf_s(lineBuf, sizeof(lineBuf), _TRUNCATE, "[%s] [%s] [%s:%d] %s",
                timeBuf, LevelStr(level), baseName, line, msgBuf);

    WriteToFile(lineBuf);
}

void Shutdown() {
    if (!g_inited) return;

    EnterCriticalSection(&g_cs);
    if (g_file) {
        fprintf(g_file, "=== ClipWiz shutdown ===\n");
        fclose(g_file);
        g_file = nullptr;
    }
    LeaveCriticalSection(&g_cs);

    DeleteCriticalSection(&g_cs);
    g_inited = false;
}

}  // namespace logger
