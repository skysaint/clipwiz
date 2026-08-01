// asyncwriter.h — 后台文件写入
//
// 保证：
//   1. 写盘在独立线程，不阻塞 UI
//   2. 同一时刻最多一个写操作在执行
//   3. 前一个没写完时新的请求排队（只保留最新一份，旧的被覆盖）
//   4. 线程安全：Submit 从主线程调，Done 从主线程调
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

    // 提交一次写任务。如果上一次还没写完，新数据会替换掉排队的旧数据。
    void Submit(std::wstring path, std::vector<uint8_t> data);

    // 主线程调用：上一次写盘是否已完成（含从未提交过的情况）
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
