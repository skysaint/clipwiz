// raii.h — Lightweight RAII wrappers, used only where critical
#pragma once

#include <windows.h>

namespace raii {

// GlobalLock wrapper — RAII guard for GlobalLock/GlobalUnlock
class GlobalLockGuard {
public:
    explicit GlobalLockGuard(HGLOBAL h) : ptr_(GlobalLock(h)), handle_(h) {}
    ~GlobalLockGuard() { 
        if (ptr_) {
            GlobalUnlock(handle_);
        }
    }
    
    // No copy
    GlobalLockGuard(const GlobalLockGuard&) = delete;
    GlobalLockGuard& operator=(const GlobalLockGuard&) = delete;
    
    void* get() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }
    
private:
    void* ptr_;
    HGLOBAL handle_;
};

// Windows HANDLE wrapper — for HANDLE-type resources
class HandleGuard {
public:
    HandleGuard() : handle_(nullptr) {}
    explicit HandleGuard(HANDLE h) : handle_(h) {}
    ~HandleGuard() { 
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
    }
    
    // No copy
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    
    // Allow move
    HandleGuard(HandleGuard&& other) noexcept : handle_(other.handle_) {
        other.handle_ = nullptr;
    }
    HandleGuard& operator=(HandleGuard&& other) noexcept {
        if (this != &other) {
            if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
                CloseHandle(handle_);
            }
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }
    
    HANDLE get() const { return handle_; }
    HANDLE release() { HANDLE h = handle_; handle_ = nullptr; return h; }
    void reset(HANDLE h = nullptr) { 
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
        }
        handle_ = h; 
    }
    explicit operator bool() const { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    
private:
    HANDLE handle_;
};

// GDI object wrapper — for resources requiring DeleteObject
class GdiObjectGuard {
public:
    GdiObjectGuard() : obj_(nullptr) {}
    explicit GdiObjectGuard(HGDIOBJ obj) : obj_(obj) {}
    ~GdiObjectGuard() { 
        if (obj_) {
            DeleteObject(obj_);
        }
    }
    
    GdiObjectGuard(const GdiObjectGuard&) = delete;
    GdiObjectGuard& operator=(const GdiObjectGuard&) = delete;
    
    GdiObjectGuard(GdiObjectGuard&& other) noexcept : obj_(other.obj_) {
        other.obj_ = nullptr;
    }
    GdiObjectGuard& operator=(GdiObjectGuard&& other) noexcept {
        if (this != &other) {
            if (obj_) {
                DeleteObject(obj_);
            }
            obj_ = other.obj_;
            other.obj_ = nullptr;
        }
        return *this;
    }
    
    HGDIOBJ get() const { return obj_; }
    HGDIOBJ release() { HGDIOBJ o = obj_; obj_ = nullptr; return o; }
    void reset(HGDIOBJ obj = nullptr) { 
        if (obj_) {
            DeleteObject(obj_);
        }
        obj_ = obj; 
    }
    explicit operator bool() const { return obj_ != nullptr; }
    
private:
    HGDIOBJ obj_;
};

// Clipboard open/close wrapper.
//
// Capture() and WriteItem() both have several early returns between opening
// the clipboard and closing it, and every one of them had to remember to
// close. Forgetting is not a local bug: an unclosed clipboard blocks every
// other application on the desktop from copying until this process exits.
// Holding the close in a destructor makes the pairing structural instead of
// something each return path repeats.
class ClipboardOpenGuard {
public:
    // `owner` is the window the clipboard is associated with; nullptr is legal
    // and is what a reader with no clipboard UI of its own uses.
    //
    // Retrying is not decoration. Office and some installer UIs still hold the
    // clipboard a few milliseconds after the copy that woke us up, and losing
    // that copy outright is worse than waiting 20ms for it. Attempts below 1
    // are treated as 1; retryDelayMs of 0 means no waiting between attempts.
    explicit ClipboardOpenGuard(HWND owner = nullptr, int attempts = 1, DWORD retryDelayMs = 0)
        : opened_(false), retries_(0) {
        const int n = (attempts < 1) ? 1 : attempts;
        for (int i = 0; i < n; ++i) {
            if (OpenClipboard(owner)) {
                opened_ = true;
                retries_ = i;
                break;
            }
            if (retryDelayMs != 0) {
                Sleep(retryDelayMs);
            }
        }
    }
    ~ClipboardOpenGuard() {
        if (opened_) {
            CloseClipboard();
        }
    }

    // No copy, no move: two owners of one open clipboard is exactly the
    // double-close this class exists to prevent.
    ClipboardOpenGuard(const ClipboardOpenGuard&) = delete;
    ClipboardOpenGuard& operator=(const ClipboardOpenGuard&) = delete;
    ClipboardOpenGuard(ClipboardOpenGuard&&) = delete;
    ClipboardOpenGuard& operator=(ClipboardOpenGuard&&) = delete;

    explicit operator bool() const { return opened_; }

    // Failed attempts before the successful open; 0 when it worked first try.
    // Only meaningful when operator bool is true.
    int retries() const { return retries_; }

private:
    bool opened_;
    int retries_;
};

}  // namespace raii
