// main.cpp — Entry point: single-instance check, then delegate to App
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>

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

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int showCmd) {
    (void)prev;
    (void)cmdLine;
    (void)showCmd;

    // Initialize logging system
    logger::Init();
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
