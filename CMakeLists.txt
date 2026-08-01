cmake_minimum_required(VERSION 3.20)
project(clipwiz CXX)

if(NOT MSVC)
    message(FATAL_ERROR "ClipWiz targets Windows and requires the MSVC toolchain.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 静态链接 CRT，产物是单个 exe，拷走就能跑，不依赖 VC 运行库
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
    src/clipwiz.rc
)

target_include_directories(clipwiz PRIVATE src)

target_compile_definitions(clipwiz PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

# 警告当错误，构建输出必须一条警告都没有
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

# manifest 由 clipwiz.rc 以资源方式嵌入，禁止链接器再生成一份
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
)

set_target_properties(clipwiz PROPERTIES
    OUTPUT_NAME "clipwiz"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
