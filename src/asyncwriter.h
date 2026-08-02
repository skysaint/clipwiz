// asyncwriter.h — Background file writing
//
// Guarantees:
//   1. Disk writes run on a dedicated thread, never blocking the UI
//   2. At most one write operation executes at any time
//   3. New submissions replace queued (not-yet-started) data (only latest kept)
//   4. Thread-safe: Submit called from main thread, Done called from main thread
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class AsyncWriter {
public:
    AsyncWriter() = default;
    ~AsyncWriter();

    AsyncWriter(const AsyncWriter&) = delete;
    AsyncWriter& operator=(const AsyncWriter&) = delete;

    bool Start();
    void Stop();

    // Submit a write task. If previous write hasn't started yet, new data replaces it.
    void Submit(std::wstring path, std::vector<uint8_t> data);

    // Called from main thread: whether the last write has completed (including never-submitted case)
    bool Done() const { return !busy_.load(std::memory_order_acquire); }

private:
    static DWORD WINAPI ThreadProc(LPVOID param);
    void Run();

    HANDLE thread_ = nullptr;
    HANDLE event_ = nullptr;
    CRITICAL_SECTION cs_;
    std::wstring pendingPath_;
    std::vector<uint8_t> pendingData_;
    bool hasPending_ = false;
    bool stop_ = false;
    std::atomic<bool> busy_{false};
};
