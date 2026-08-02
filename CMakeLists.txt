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

add_executable(clipwiz WIN32
    src/main.cpp
    src/app.cpp
    src/store.cpp
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
    src/conflict.cpp
    src/log.cpp
    src/raii.h
    src/clipwiz.rc
)

target_include_directories(clipwiz PRIVATE src)

# RC compiler also needs include path to find resource.h
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} /i\"${CMAKE_CURRENT_SOURCE_DIR}/src\"")
set_source_files_properties(src/clipwiz.rc PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_definitions(clipwiz PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

# Warnings as errors: build output must have zero warnings
target_compile_options(clipwiz PRIVATE
    /W4
    /WX
    /permissive-
    /utf-8
    /GR-
    /Gy
    /Zc:__cplusplus
    /Zc:inline
)

# Manifest embedded via clipwiz.rc resource; disable linker-generated manifest
target_link_options(clipwiz PRIVATE
    /MANIFEST:NO
    $<$<NOT:$<CONFIG:Debug>>:/OPT:REF>
    $<$<NOT:$<CONFIG:Debug>>:/OPT:ICF>
    $<$<NOT:$<CONFIG:Debug>>:/INCREMENTAL:NO>
)

target_link_libraries(clipwiz PRIVATE
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

set_target_properties(clipwiz PROPERTIES
    OUTPUT_NAME "clipwiz"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
