// log.h — Lightweight logging system
#pragma once

#include <cstdint>
#include <string>

namespace logger {

enum class Level : uint8_t {
    Off = 0,
    Debug = 1,
    Info = 2,
    Warning = 3,
    Error = 4
};

// Initialize the logging system (call at program startup)
// Log file located in data directory, filename: clipwiz.log
void Init();

// Set minimum log level at runtime. Default is Error (only warnings and above are off).
void SetMinLevel(Level level);
Level GetMinLevel();

// Parse a level name like "off","debug","info","warning","error" (case-insensitive)
// Returns Level::Error on unknown names.
Level ParseLevel(const std::string& name);

// Write log entry
void Write(Level level, const char* file, int line, const char* fmt, ...);

// Shutdown the logging system (call at program exit)
void Shutdown();

// Close and reopen the log file using current DataDir().
// Call after data directory migration to release the old file handle.
void Reopen();

// Convenience macros
#define LOG_DEBUG(fmt, ...)   logger::Write(logger::Level::Debug,   __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    logger::Write(logger::Level::Info,    __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) logger::Write(logger::Level::Warning, __FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   logger::Write(logger::Level::Error,   __FILE__, __LINE__, fmt, ##__VA_ARGS__)

}  // namespace logger
