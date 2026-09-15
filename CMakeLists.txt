cmake_minimum_required(VERSION 3.20)
project(clipwiz CXX)

if(NOT MSVC)
    message(FATAL_ERROR "ClipWiz targets Windows and requires the MSVC toolchain.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static link CRT: single exe, runs anywhere, no VC runtime dependency
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# ---------------------------------------------------------------------------
# Shared compile settings, applied identically to the app and the test exe.
# The test target is EXCLUDE_FROM_ALL so the normal build stays fast; build it
# explicitly with:  cmake --build build --config Release --target clipwiz_tests
# ---------------------------------------------------------------------------
set(CWZ_DEFINITIONS
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

# Warnings as errors: build output must have zero warnings
set(CWZ_COMPILE_OPTIONS
    /W4
    /WX
    /permissive-
    /utf-8
    /GR-
    /Gy
    /Zc:__cplusplus
    /Zc:inline
)

set(CWZ_LINK_LIBS
    user32
    gdi32
    msimg32
    shell32
    comctl32
    advapi32
    ole32
    windowscodecs
    comdlg32
)

add_executable(clipwiz WIN32
    src/main.cpp
    src/app.cpp
    src/store.cpp
    src/textconv.cpp
    src/filter.cpp
    src/privacy.cpp
    src/blocklist.cpp
    src/mask.cpp
    src/merge.cpp
    src/transform.cpp
    src/clipboard.cpp
    src/hotkey.cpp
    src/paste.cpp
    src/popup.cpp
    src/settings.cpp
    src/imagecodec.cpp
    src/tray.cpp
    src/util.cpp
    src/i18n.cpp
    src/asyncwriter.cpp
    src/log.cpp
    src/raii.h
    src/clipwiz.rc
)

target_include_directories(clipwiz PRIVATE src)

# RC compiler also needs include path to find resource.h
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /i\"${CMAKE_CURRENT_SOURCE_DIR}/src\"")
set_source_files_properties(src/clipwiz.rc PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")
# The .rc embeds lang/zh-CN.lng as RCDATA, but the resource compiler does not
# report that dependency, so editing the .lng alone would leave a stale string
# baked into the exe. Declare it explicitly to force the .rc to recompile.
set_source_files_properties(src/clipwiz.rc PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/lang/zh-CN.lng")

target_compile_definitions(clipwiz PRIVATE ${CWZ_DEFINITIONS})
target_compile_options(clipwiz PRIVATE ${CWZ_COMPILE_OPTIONS})

# Manifest embedded via clipwiz.rc resource; disable linker-generated manifest
target_link_options(clipwiz PRIVATE
    /MANIFEST:NO
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
    $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
    $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>
)

target_link_libraries(clipwiz PRIVATE ${CWZ_LINK_LIBS})

set_target_properties(clipwiz PROPERTIES
    OUTPUT_NAME "clipwiz"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---------------------------------------------------------------------------
# clipwiz_tests — headless guard rails for the pure logic.
#
# Links only the units that take plain data in and give plain data out.
# Deliberately NOT linked: main / app / popup / settings / tray / hotkey /
# paste / clipboard / asyncwriter — those need a window, a message loop, or
# the real clipboard, and testing them headless would test nothing but mocks.
#
# Console subsystem so results print; no .rc, so i18n falls back to the
# built-in English table (verified safe: i18n::T never needs Init).
# ---------------------------------------------------------------------------
add_executable(clipwiz_tests EXCLUDE_FROM_ALL
    tests/test_main.cpp
    tests/test_textconv.cpp
    tests/test_store.cpp
    tests/test_filter.cpp
    tests/test_privacy.cpp
    tests/test_blocklist.cpp
    tests/test_mask.cpp
    tests/test_merge.cpp
    tests/test_transform.cpp
    src/textconv.cpp
    src/filter.cpp
    src/privacy.cpp
    src/blocklist.cpp
    src/mask.cpp
    src/merge.cpp
    src/transform.cpp
    src/store.cpp
    src/util.cpp
    src/imagecodec.cpp
    src/i18n.cpp
    src/log.cpp
)

target_include_directories(clipwiz_tests PRIVATE src tests)
target_compile_definitions(clipwiz_tests PRIVATE ${CWZ_DEFINITIONS})
target_compile_options(clipwiz_tests PRIVATE ${CWZ_COMPILE_OPTIONS})
target_link_libraries(clipwiz_tests PRIVATE ${CWZ_LINK_LIBS})

set_target_properties(clipwiz_tests PROPERTIES
    OUTPUT_NAME "clipwiz_tests"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
