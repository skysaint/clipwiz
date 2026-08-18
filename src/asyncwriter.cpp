// asyncwriter.cpp
#include "asyncwriter.h"

#include "util.h"

AsyncWriter::~AsyncWriter() {
    Stop();
}

bool AsyncWriter::Start() {
    InitializeCriticalSection(&cs_);
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event_) {
        DeleteCriticalSection(&cs_);
        return false;
    }
    thread_ = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    if (!thread_) {
        CloseHandle(event_);
        event_ = nullptr;
        DeleteCriticalSection(&cs_);
        return false;
    }
    return true;
}

void AsyncWriter::Stop() {
    if (!thread_) {
        return;
    }
    EnterCriticalSection(&cs_);
    stop_ = true;
    LeaveCriticalSection(&cs_);
    SetEvent(event_);
    WaitForSingleObject(thread_, 5000);
    CloseHandle(thread_);
    thread_ = nullptr;
    if (event_) {
        CloseHandle(event_);
        event_ = nullptr;
    }
    DeleteCriticalSection(&cs_);
}

void AsyncWriter::Submit(std::wstring path, std::vector<uint8_t> data) {
    EnterCriticalSection(&cs_);
    pendingPath_ = std::move(path);
    pendingData_ = std::move(data);
    hasPending_ = true;
    busy_.store(true, std::memory_order_release);
    LeaveCriticalSection(&cs_);
    SetEvent(event_);
}

DWORD WINAPI AsyncWriter::ThreadProc(LPVOID param) {
    static_cast<AsyncWriter*>(param)->Run();
    return 0;
}

void AsyncWriter::Run() {
    for (;;) {
        WaitForSingleObject(event_, INFINITE);

        EnterCriticalSection(&cs_);
        if (stop_) {
            // Last-ditch flush: if there is pending data when Stop() fires,
            // write it synchronously on our way out so it is never dropped.
            if (hasPending_) {
                std::wstring path = std::move(pendingPath_);
                std::vector<uint8_t> data = std::move(pendingData_);
                hasPending_ = false;
                LeaveCriticalSection(&cs_);
                if (!data.empty()) {
                    util::WriteFileAtomic(path, data.data(), data.size());
                }
                EnterCriticalSection(&cs_);
            }
            busy_.store(false, std::memory_order_release);
            LeaveCriticalSection(&cs_);
            break;
        }
        // Take pending data
        std::wstring path = std::move(pendingPath_);
        std::vector<uint8_t> data = std::move(pendingData_);
        hasPending_ = false;
        LeaveCriticalSection(&cs_);

        // Actual disk write (blocks this background thread, not the UI)
        if (!data.empty()) {
            util::WriteFileAtomic(path, data.data(), data.size());
        }

        // After write, check if new task is queued
        EnterCriticalSection(&cs_);
        if (hasPending_) {
            // More data pending, continue loop
            LeaveCriticalSection(&cs_);
            SetEvent(event_);
        } else {
            busy_.store(false, std::memory_order_release);
            LeaveCriticalSection(&cs_);
        }
    }
}
