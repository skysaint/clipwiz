// main.cpp — 入口：单实例检查，然后把活全交给 App
#include <windows.h>
#include <objbase.h>

#include "app.h"
#include "util.h"

namespace {

const wchar_t kMutexName[] = L"Local\\ClipWiz.SingleInstance";

// 已经有一个实例在跑：叫它把粘贴框弹出来，自己安静退出
bool HandOffToRunningInstance() {
    PostMessageW(HWND_BROADCAST, SingleInstanceMessage(), 0, 0);
    return true;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdLine, int showCmd) {
    (void)prev;
    (void)cmdLine;
    (void)showCmd;

    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HandOffToRunningInstance();
        CloseHandle(mutex);
        return 0;
    }

    // WIC 要 COM；套件里只有本线程用它，单线程套间够了
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) {
        util::ErrorBox(nullptr, L"COM initialization failed. ClipWiz cannot start.");
        if (mutex) {
            CloseHandle(mutex);
        }
        return 1;
    }

    int code = 1;
    {
        App app;
        if (app.Init(inst)) {
            code = app.Run();
        }
    }

    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return code;
}
