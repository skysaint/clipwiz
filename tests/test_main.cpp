// test_main.cpp — entry point for clipwiz_tests
//
// Redirects util::DataDir() into a scratch directory before anything touches
// the store, so a test run can never read, rewrite, or "preserve as corrupt"
// the user's real %APPDATA%\ClipWiz\store.dat.
//
// Usage:  clipwiz_tests [scratchDir]
// Default scratchDir: %TEMP%\clipwiz-tests-<pid>
// Exit code: 0 = all checks passed, 1 = at least one failed, 2 = setup failed.
#include <windows.h>

#include <cstdio>
#include <string>

#include "i18n.h"
#include "testfw.h"
#include "tests.h"
#include "util.h"

namespace {

std::wstring DefaultScratchDir() {
    wchar_t tmp[MAX_PATH] = {};
    DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    std::wstring dir(tmp, n);
    if (dir.back() != L'\\') {
        dir += L'\\';
    }
    wchar_t suffix[48] = {};
    swprintf_s(suffix, L"clipwiz-tests-%lu", static_cast<unsigned long>(GetCurrentProcessId()));
    return dir + suffix;
}

// Remove only the files the store can create inside the scratch dir. The list
// is explicit on purpose: cleanup must never widen into "delete everything in
// whatever directory the caller passed".
void CleanScratchDir(const std::wstring& dir) {
    DeleteFileW((dir + L"\\store.dat").c_str());
    DeleteFileW((dir + L"\\store.dat.tmp").c_str());

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\store.corrupt.*.dat").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// The plan flagged this as "verify, don't assume": MakeItemPreview() calls
// i18n::T(), and the test exe links no .lng resource. If T() needed Init()
// first, every preview assertion below would be testing "???" instead of a
// real string.
void I18nIsSafeUninitialized() {
    CWZ_CHECK(std::wstring(i18n::T("preview.empty")) == L"[Empty content]");
    CWZ_CHECK(std::wstring(i18n::T("preview.image")) == L"[Image %u\x00D7%u]");
    // Unknown key must not crash, and must be visibly wrong rather than "".
    CWZ_CHECK(std::wstring(i18n::T("no.such.key")) == L"???");
}

// popup.empty_filtered sat in the table with no reader, so a list filtered
// down to zero rows told the user they had no clipboard history at all. The
// two empty states are different situations and must stay different strings.
void EmptyStateMessagesAreDistinct() {
    const std::wstring noHistory = i18n::T("popup.empty");
    const std::wstring noMatch = i18n::T("popup.empty_filtered");
    CWZ_CHECK(!noHistory.empty() && noHistory != L"???");
    CWZ_CHECK(!noMatch.empty() && noMatch != L"???");
    CWZ_CHECK(noHistory != noMatch);
}

}  // namespace

int main(int argc, char** argv) {
    const std::wstring scratch = (argc > 1) ? util::Widen(argv[1]) : DefaultScratchDir();
    if (scratch.empty() || !util::EnsureDir(scratch)) {
        std::printf("Cannot create scratch directory\n");
        return 2;
    }
    util::SetDataDir(scratch);
    std::printf("clipwiz_tests — scratch dir: %s\n\n", util::Narrow(scratch).c_str());

    CWZ_RUN(I18nIsSafeUninitialized);
    CWZ_RUN(EmptyStateMessagesAreDistinct);
    RunTextConvTests();
    RunFilterTests();
    RunBlocklistTests();
    RunMaskTests();
    RunMergeTests();
    RunTransformTests();
    RunPrivacyTests();
    RunStoreTests();  // last: the only suite that touches the scratch directory

    CleanScratchDir(scratch);
    RemoveDirectoryW(scratch.c_str());
    return testfw::Summary();
}
