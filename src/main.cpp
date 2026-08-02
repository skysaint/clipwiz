// main.cpp — Entry point: single-instance check, then delegate to App
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <csignal>

#include "app.h"
#include "log.h"
#include "resource.h"
#include "util.h"

namespace {

const wchar_t kMutexName[] = L"Local\\ClipWiz.SingleInstance";

// Already have an instance running: tell it to show the popup, then exit quietly
bool HandOffToRunningInstance() {
    PostMessageW(HWND_BROADCAST, SingleInstanceMessage(), 0, 0);
    return true;
}

// Top-level crash handler: log exception info before process dies
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    if (ep && ep->ExceptionRecord) {
        LOG_ERROR("CRASH: code=0x%08X addr=0x%p flags=0x%X",
                  ep->ExceptionRecord->ExceptionCode,
                  ep->ExceptionRecord->ExceptionAddress,
                  ep->ExceptionRecord->ExceptionFlags);
    } else {
        LOG_ERROR("CRASH: unknown exception (no ExceptionRecord)");
    }
    logger::Shutdown();
    return EXCEPTION_EXECUTE_HANDLER;
}

// Vectored handler: fires before SEH, catches more crash types
LONG WINAPI VectoredCrashHandler(EXCEPTION_POINTERS* ep) {
    // Only log fatal exceptions (access violation, stack overflow, etc.)
    if (ep && ep->ExceptionRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        if (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_STACK_OVERFLOW ||
            code == EXCEPTION_INT_DIVIDE_BY_ZERO || code == 0xE0000001 /*heap corruption*/) {
            LOG_ERROR("VEH-CRASH: code=0x%08X addr=0x%p", code,
                      ep->ExceptionRecord->ExceptionAddress);
            logger::Shutdown();
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;  // Let normal SEH handling proceed
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int showCmd) {
    (void)prev;
    (void)cmdLine;
    (void)showCmd;

    // Initialize logging system
    logger::Init();
    AddVectoredExceptionHandler(1, VectoredCrashHandler);
    SetUnhandledExceptionFilter(CrashHandler);
    LOG_INFO("ClipWiz starting...");

    // Initialize Common Controls (required for PropertySheet, tray icon, modern controls)
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_WIN95_CLASSES | ICC_DATE_CLASSES | ICC_USEREX_CLASSES;
    if (!InitCommonControlsEx(&icc)) {
        LOG_ERROR("Common Controls initialization failed, error=%u", GetLastError());
        util::ErrorBox(nullptr, L"Error ERR_COMMON_CONTROLS: Failed to initialize Windows Common Controls.");
        logger::Shutdown();
        return 1;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_INFO("Another instance is already running, handing off...");
        HandOffToRunningInstance();
        CloseHandle(mutex);
        logger::Shutdown();
        return 0;
    }

    // WIC needs COM; only this thread uses it, apartment-threaded is sufficient
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        LOG_ERROR("COM initialization failed: HRESULT=0x%08X", hr);
        util::ErrorBox(nullptr, L"Error ERR_COM_INIT: COM initialization failed.");
        if (mutex) {
            CloseHandle(mutex);
        }
        logger::Shutdown();
        return 1;
    }

    int code = 1;
    {
        App app;
        if (app.Init(inst)) {
            LOG_INFO("Application initialized successfully");
            code = app.Run();
            LOG_INFO("Application exited with code: %d", code);
        } else {
            LOG_ERROR("Application initialization failed, exiting");
            // Initialization failed, exit immediately
            code = 1;
        }
    }

    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    
    LOG_INFO("ClipWiz shutting down...");
    logger::Shutdown();
    return code;
}
