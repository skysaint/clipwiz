// main.cpp — Entry point: single-instance check, then delegate to App
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <csignal>
#include <dbghelp.h>
#include <time.h>

#include "app.h"
#include "log.h"
#include "popup.h"
#include "resource.h"
#include "util.h"

#pragma comment(lib, "dbghelp.lib")

namespace {

const wchar_t kMutexName[] = L"Local\\ClipWiz.SingleInstance";

// Global App pointer, assigned for the life of the App block so crash handlers
// can snapshot a limited read-only view of runtime state without pulling in
// the full App header or risking reentrant memory allocations.
const App* g_appForCrash = nullptr;

// Best-effort snapshot of the last dequeued (or currently being processed)
// message, for crash post-mortem only.
struct LastMsg {
    UINT msg = 0;
    WPARAM wparam = 0;
    LPARAM lparam = 0;
    DWORD tick = 0;
    HWND hwnd = nullptr;
};

LastMsg g_lastMsg;

void RecordLastMsg(const MSG& m) {
    if (m.message == WM_NULL) return;
    g_lastMsg.msg = m.message;
    g_lastMsg.wparam = m.wParam;
    g_lastMsg.lparam = m.lParam;
    g_lastMsg.hwnd = m.hwnd;
    g_lastMsg.tick = GetTickCount();
}

// Write a time-prefixed mini-dump alongside clipwiz.log. Best-effort only.
bool WriteMiniDump(EXCEPTION_POINTERS* ep, const char* tag) {
    if (!ep) return false;

    std::wstring dir = util::DataDir();
    util::EnsureDir(dir);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t fname[128];
    _snwprintf_s(fname, _TRUNCATE,
                 L"%s\\clipwiz-crash-%04d%02d%02d-%02d%02d%02d-%03d-%s.dmp",
                 dir.c_str(),
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                 (tag && tag[0]) ? util::Widen(tag).c_str() : L"crash");

    HANDLE file = CreateFileW(fname, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    MINIDUMP_EXCEPTION_INFORMATION mei;
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;

    const DWORD flags = MiniDumpWithDataSegs | MiniDumpWithHandleData |
                        MiniDumpWithIndirectlyReferencedMemory |
                        MiniDumpScanMemory | MiniDumpWithProcessThreadData |
                        MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules;

    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(),
                                file, static_cast<MINIDUMP_TYPE>(flags),
                                &mei, nullptr, nullptr);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(fname);
        return false;
    }
    return true;
}


const char* MessageNameHint(UINT msg) {
    switch (msg) {
        case 0x0000: return "WM_NULL";
        case 0x0001: return "WM_CREATE";
        case 0x0002: return "WM_DESTROY";
        case 0x0003: return "WM_MOVE";
        case 0x0005: return "WM_SIZE";
        case 0x0006: return "WM_ACTIVATE";
        case 0x0007: return "WM_SETFOCUS";
        case 0x0008: return "WM_KILLFOCUS";
        case 0x000A: return "WM_ENABLE";
        case 0x000B: return "WM_SETREDRAW";
        case 0x000F: return "WM_PAINT";
        case 0x0010: return "WM_CLOSE";
        case 0x0011: return "WM_QUERYENDSESSION";
        case 0x0012: return "WM_QUIT";
        case 0x0013: return "WM_QUERYOPEN";
        case 0x0014: return "WM_ERASEBKGND";
        case 0x0018: return "WM_SHOWWINDOW";
        case 0x0019: return "WM_WININICHANGE";
        case 0x001B: return "WM_DEVMODECHANGE";
        case 0x001C: return "WM_ACTIVATEAPP";
        case 0x0020: return "WM_SETCURSOR";
        case 0x0021: return "WM_MOUSEACTIVATE";
        case 0x0022: return "WM_CHILDACTIVATE";
        case 0x0023: return "WM_QUEUESYNC";
        case 0x0024: return "WM_GETMINMAXINFO";
        case 0x0046: return "WM_WINDOWPOSCHANGING";
        case 0x0047: return "WM_WINDOWPOSCHANGED";
        case 0x0050: return "WM_DPICHANGED";
        case 0x007F: return "WM_GETICON";
        case 0x0080: return "WM_SETICON";
        case 0x0084: return "WM_NCHITTEST";
        case 0x0085: return "WM_NCPAINT";
        case 0x0086: return "WM_NCACTIVATE";
        case 0x00A0: return "WM_NCMOUSEMOVE";
        case 0x00A1: return "WM_NCLBUTTONDOWN";
        case 0x00A2: return "WM_NCLBUTTONUP";
        case 0x00A3: return "WM_NCLBUTTONDBLCLK";
        case 0x00A4: return "WM_NCRBUTTONDOWN";
        case 0x00A5: return "WM_NCRBUTTONUP";
        case 0x0200: return "WM_MOUSEMOVE";
        case 0x0201: return "WM_LBUTTONDOWN";
        case 0x0202: return "WM_LBUTTONUP";
        case 0x0203: return "WM_LBUTTONDBLCLK";
        case 0x0204: return "WM_RBUTTONDOWN";
        case 0x0205: return "WM_RBUTTONUP";
        case 0x0206: return "WM_RBUTTONDBLCLK";
        case 0x0207: return "WM_MBUTTONDOWN";
        case 0x0208: return "WM_MBUTTONUP";
        case 0x020A: return "WM_MOUSEWHEEL";
        case 0x020B: return "WM_MOUSEHWHEEL";
        case 0x0100: return "WM_KEYDOWN";
        case 0x0101: return "WM_KEYUP";
        case 0x0102: return "WM_CHAR";
        case 0x0104: return "WM_SYSKEYDOWN";
        case 0x0105: return "WM_SYSKEYUP";
        case 0x0106: return "WM_SYSCHAR";
        case 0x0110: return "WM_INITDIALOG";
        case 0x0111: return "WM_COMMAND";
        case 0x0112: return "WM_SYSCOMMAND";
        case 0x0113: return "WM_TIMER";
        case 0x0114: return "WM_HSCROLL";
        case 0x0115: return "WM_VSCROLL";
        case 0x0308: return "WM_DRAWCLIPBOARD";
        case 0x031D: return "WM_CLIPBOARDUPDATE";
        case 0x031E: return "WM_HOTKEY";
        default:   return "";
    }
}

// All state reads in this function must be side-effect free. It runs inside a
// crash context: no (re)allocations, no calling into unknown code.
void EmitCrashSnapshot(const char* tag) {
    popup::RuntimeState ps{};
    popup::SnapshotState(ps);

    int maxHistory = 0, expiryDays = 0, popupHotkey = 0, rowsVisible = 0, popupPosition = 0, fontSize = 0;
    std::string logLevelUtf8;
    int storeCount = 0, storePinned = 0, storeHistory = 0;
    uint64_t storeTotalBytes = 0;
    if (g_appForCrash) {
        g_appForCrash->DumpConfigForCrash(maxHistory, expiryDays, popupHotkey, rowsVisible,
                                          popupPosition, fontSize, logLevelUtf8,
                                          storeCount, storePinned, storeHistory,
                                          storeTotalBytes);
    }

    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof(mem);
    bool memOk = GlobalMemoryStatusEx(&mem) != FALSE;

    const char* lastHint = MessageNameHint(g_lastMsg.msg);
    char msgHintBuf[32];
    if (lastHint[0]) {
        _snprintf_s(msgHintBuf, sizeof(msgHintBuf), _TRUNCATE, "%s(0x%04X)", lastHint, g_lastMsg.msg);
    } else {
        _snprintf_s(msgHintBuf, sizeof(msgHintBuf), _TRUNCATE, "0x%04X", g_lastMsg.msg);
    }

    LOG_ERROR(
        "%s-SNAP popup: hwnd=0x%p vis=%d modal=%d drag=%d rows=%d "
        "sel=%d widthDip=%d heightDip=%d dpi=%d pinned=%d",
        tag, ps.hwnd, ps.visible, ps.modalDepth, ps.reorderDrag, ps.rowsCount,
        ps.lastSelRow, ps.widthDip, ps.heightDip, ps.dpi, ps.pinnedCount);
    LOG_ERROR(
        "%s-SNAP cfg: maxHistory=%d expiryDays=%d popupHk=0x%08X rowsVisible=%d "
        "popupPos=%d fontSize=%d logLevel=%s storeCount=%d storePinned=%d storeHistory=%d storeMB=%llu",
        tag, maxHistory, expiryDays, popupHotkey, rowsVisible, popupPosition,
        fontSize, logLevelUtf8.empty() ? "(unset)" : logLevelUtf8.c_str(),
        storeCount, storePinned, storeHistory,
        static_cast<unsigned long long>(storeTotalBytes / (1024ULL * 1024ULL)));
    LOG_ERROR(
        "%s-SNAP msg: last=%s wp=0x%p lp=0x%p hwnd=0x%p tick=%u memPhysPct=%u availPhysMB=%llu availVirtualMB=%llu",
        tag, msgHintBuf,
        reinterpret_cast<void*>(static_cast<uintptr_t>(g_lastMsg.wparam)),
        reinterpret_cast<void*>(static_cast<uintptr_t>(g_lastMsg.lparam)),
        g_lastMsg.hwnd, g_lastMsg.tick,
        memOk ? static_cast<unsigned>(mem.dwMemoryLoad) : 0u,
        memOk ? static_cast<unsigned long long>(mem.ullAvailPhys / (1024ULL * 1024ULL)) : 0ULL,
        memOk ? static_cast<unsigned long long>(mem.ullAvailVirtual / (1024ULL * 1024ULL)) : 0ULL);
}

// Already have an instance running: tell it to show the popup, then exit quietly
bool HandOffToRunningInstance() {
    PostMessageW(HWND_BROADCAST, SingleInstanceMessage(), 0, 0);
    return true;
}

// Top-level crash handler: log exception info before process dies
LONG WINAPI CrashHandler(EXCEPTION_POINTERS* ep) {
    bool dumped = WriteMiniDump(ep, "CRASH");
    if (ep && ep->ExceptionRecord) {
        DWORD code = ep->ExceptionRecord->ExceptionCode;
        ULONG_PTR info[2] = {0, 0};
        if (ep->ExceptionRecord->NumberParameters >= 2) {
            info[0] = ep->ExceptionRecord->ExceptionInformation[0];
            info[1] = ep->ExceptionRecord->ExceptionInformation[1];
        }
        const wchar_t* extra = L"";
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION:
                extra = (info[0] == 0) ? L" read" : (info[0] == 1 ? L" write" : L" execute");
                LOG_ERROR(
                    "CRASH: ACCESS_VIOLATION ip=0x%p%s addr=0x%p flags=0x%X dump=%d",
                    ep->ExceptionRecord->ExceptionAddress, extra,
                    reinterpret_cast<void*>(info[1]),
                    ep->ExceptionRecord->ExceptionFlags, static_cast<int>(dumped));
                break;
            case EXCEPTION_STACK_OVERFLOW:
                LOG_ERROR("CRASH: STACK_OVERFLOW ip=0x%p flags=0x%X dump=%d",
                          ep->ExceptionRecord->ExceptionAddress,
                          ep->ExceptionRecord->ExceptionFlags, static_cast<int>(dumped));
                break;
            case EXCEPTION_INT_DIVIDE_BY_ZERO:
                LOG_ERROR("CRASH: DIVIDE_BY_ZERO ip=0x%p flags=0x%X dump=%d",
                          ep->ExceptionRecord->ExceptionAddress,
                          ep->ExceptionRecord->ExceptionFlags, static_cast<int>(dumped));
                break;
            case 0xE0000001:
            case 0xE06D7363:
                LOG_ERROR("CRASH: HEAP/CPP-EXCEPTION code=0x%08X ip=0x%p flags=0x%X dump=%d",
                          code, ep->ExceptionRecord->ExceptionAddress,
                          ep->ExceptionRecord->ExceptionFlags, static_cast<int>(dumped));
                break;
            default:
                LOG_ERROR("CRASH: code=0x%08X ip=0x%p flags=0x%X dump=%d",
                          code, ep->ExceptionRecord->ExceptionAddress,
                          ep->ExceptionRecord->ExceptionFlags, static_cast<int>(dumped));
                break;
        }
    } else {
        LOG_ERROR("CRASH: unknown exception (no ExceptionRecord) dump=%d", static_cast<int>(dumped));
    }
    EmitCrashSnapshot("CRASH");
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
            bool dumped = WriteMiniDump(ep, "VEH");
            ULONG_PTR info[2] = {0, 0};
            if (ep->ExceptionRecord->NumberParameters >= 2) {
                info[0] = ep->ExceptionRecord->ExceptionInformation[0];
                info[1] = ep->ExceptionRecord->ExceptionInformation[1];
            }
            if (code == EXCEPTION_ACCESS_VIOLATION) {
                const wchar_t* extra = (info[0] == 0) ? L" read" : (info[0] == 1 ? L" write" : L" execute");
                LOG_ERROR("VEH-CRASH: ACCESS_VIOLATION ip=0x%p%s addr=0x%p dump=%d",
                          ep->ExceptionRecord->ExceptionAddress, extra,
                          reinterpret_cast<void*>(info[1]), static_cast<int>(dumped));
            } else {
                LOG_ERROR("VEH-CRASH: code=0x%08X ip=0x%p dump=%d", code,
                          ep->ExceptionRecord->ExceptionAddress, static_cast<int>(dumped));
            }
            EmitCrashSnapshot("VEH");
            logger::Shutdown();
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;  // Let normal SEH handling proceed
}

}  // namespace

// App::Run() re-implemented in app.cpp; we keep GetMessage()-level tracing here
// to avoid touching the App object layout. This is a tiny thunk only.
int PumpMessagesWithTracing() {
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        RecordLastMsg(msg);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

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
            g_appForCrash = &app;
            LOG_INFO("Application initialized successfully");
            // Replace App::Run GetMessage loop so we can trace last msg without
            // modifying App's own class layout/internals.
            code = PumpMessagesWithTracing();
            g_appForCrash = nullptr;
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
