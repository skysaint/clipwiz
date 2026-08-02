// log.cpp
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <cstddef>
#include <chrono>
#include <ctime>
#include <mutex>

#include "util.h"

namespace logger {
namespace {

FILE* g_file = nullptr;
std::mutex g_mutex;
Level g_minLevel = Level::Info;  // Release builds only log Info and above
bool g_initialized = false;

const char* LevelToString(Level level) {
    switch (level) {
        case Level::Debug:   return "DEBUG";
        case Level::Info:    return "INFO ";
        case Level::Warning: return "WARN ";
        case Level::Error:   return "ERROR";
        default:             return "?????";
    }
}

// Safe file open function
FILE* SafeOpenFile(const wchar_t* path, const wchar_t* mode) {
    FILE* file = nullptr;
    errno_t err = _wfopen_s(&file, path, mode);
    if (err != 0 || file == nullptr) {
        return nullptr;
    }
    return file;
}

// Safe time retrieval function
void SafeGetTime(char* timeStr, size_t bufferSize) {
    time_t now;
    time(&now);
    struct tm timeInfo;
    localtime_s(&timeInfo, &now);
    strftime(timeStr, bufferSize, "%H:%M:%S", &timeInfo);
}

void WriteToFile(const char* line) {
    if (!g_file) return;
    
    std::lock_guard<std::mutex> lock(g_mutex);
    fprintf(g_file, "%s\n", line);
    fflush(g_file);  // Flush immediately to prevent log loss on crash
}

}  // namespace

void Init() {
    if (g_initialized) return;
    
    std::wstring logPath = util::DataDir() + L"\\clipwiz.log";
    
    // Ensure directory exists
    util::EnsureDir(util::DataDir());
    
    // Use safe file open function
    g_file = SafeOpenFile(logPath.c_str(), L"a");
    
    if (g_file) {
        // Write startup marker
        time_t now;
        time(&now);
        struct tm timeInfo;
        localtime_s(&timeInfo, &now);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &timeInfo);
        fprintf(g_file, "\n=== ClipWiz started at %s ===\n", time_str);
        fflush(g_file);
    }
    
    g_initialized = true;
}

void Write(Level level, const char* file, int line, const char* func, const char* fmt, ...) {
    (void)func;  // Not using function name for now, suppress warning
    if (!g_initialized || level < g_minLevel) return;
    
    // Get current time (safe version)
    char time_str[32];
    SafeGetTime(time_str, sizeof(time_str));
    
    // Extract filename (strip path)
    const char* filename = file;
    const char* lastSlash = file;
    while (*file) {
        if (*file == '/' || *file == '\\') lastSlash = file + 1;
        file++;
    }
    filename = lastSlash;
    
    // Format log message
    char line_buf[1024];
    char msg_buf[512];
    
    va_list args;
    va_start(args, fmt);
    vsnprintf_s(msg_buf, sizeof(msg_buf), _TRUNCATE, fmt, args);
    va_end(args);
    
    _snprintf_s(line_buf, sizeof(line_buf), _TRUNCATE, "[%s] [%s] [%s:%d] %s",
              time_str, LevelToString(level), filename, line, msg_buf);
    
    WriteToFile(line_buf);
}

void Shutdown() {
    if (!g_initialized) return;
    
    if (g_file) {
        fprintf(g_file, "=== ClipWiz shutdown ===\n");
        fclose(g_file);
        g_file = nullptr;
    }
    
    g_initialized = false;
}

}  // namespace logger
