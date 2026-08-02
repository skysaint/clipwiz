// log.h — Lightweight logging system
#pragma once

#include <string>
#include <cstdint>

namespace logger {

enum class Level : uint8_t {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3
};

// Initialize the logging system (call at program startup)
// Log file located in data directory, filename: clipwiz.log
void Init();

// Write log entry
void Write(Level level, const char* file, int line, const char* func, const char* fmt, ...);

// Shutdown the logging system (call at program exit)
void Shutdown();

// Convenience macros
#define LOG_DEBUG(fmt, ...)   logger::Write(logger::Level::Debug,   __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)    logger::Write(logger::Level::Info,    __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_WARNING(fmt, ...) logger::Write(logger::Level::Warning, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   logger::Write(logger::Level::Error,   __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

}  // namespace logger
