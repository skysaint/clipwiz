// raii.h — Lightweight RAII wrappers, used only where critical
#pragma once

#include <windows.h>

namespace raii {

// GlobalLock wrapper — fixes resource leak issues in clipboard.cpp
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

}  // namespace raii
