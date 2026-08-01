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
    LeaveCriticalSection(&cs_);
    busy_.store(true, std::memory_order_release);
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
            LeaveCriticalSection(&cs_);
            break;
        }
        // 取走待写数据
        std::wstring path = std::move(pendingPath_);
        std::vector<uint8_t> data = std::move(pendingData_);
        hasPending_ = false;
        LeaveCriticalSection(&cs_);

        // 实际写盘（阻塞的是这个后台线程，不影响 UI）
        if (!data.empty()) {
            util::WriteFileAtomic(path, data.data(), data.size());
        }

        // 写完后看看有没有新任务排队
        EnterCriticalSection(&cs_);
        if (hasPending_) {
            // 还有新数据，继续循环
            LeaveCriticalSection(&cs_);
            SetEvent(event_);
        } else {
            busy_.store(false, std::memory_order_release);
            LeaveCriticalSection(&cs_);
        }
    }
}
