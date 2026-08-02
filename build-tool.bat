@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
title ClipWiz Build Tool
cd /d "%~dp0"

:: ============================================================
:: Detect: double-click (explorer) vs command-line
:: ============================================================
set "double_clicked=false"
echo %cmdcmdline% | find /i "cmd /c" >nul
if %errorlevel%==0 (
    echo %cmdcmdline% | find /i "%~0" >nul
    if %errorlevel%==0 set "double_clicked=true"
)
echo %cmdcmdline% | find /i "cmd.exe" >nul
if %errorlevel%==0 (
    if "%~0"=="%~dpnx0" set "double_clicked=true"
)

:: CLI mode: no args = usage, with args = dispatch
if "%~1"=="" (
    if "!double_clicked!"=="false" (
        goto :USAGE
    )
)
if not "%~1"=="" goto :CLI_DISPATCH

:: ============================================================
:: Interactive menu (double-click only)
:: ============================================================
:MENU_START
cls
echo.
echo  ================================================
echo   ClipWiz  Build Tool (Win32 / MSVC)
echo  ================================================

:MENU
echo.
echo   [1] Init       - cmake configure (VS 2022 x64)
echo   [2] Build      - Release build
echo   [3] Debug      - Debug build
echo   [4] Rebuild    - Clean + Release build
echo   [5] Clean      - Remove build directory
echo   [6] Run        - Run Release exe
echo   [0] Exit
echo.

:PROMPT
set "choice="
set /p "choice=  Select (0-6): "
if "!choice!"=="" goto :PROMPT

if "!choice!"=="1" goto :DO_INIT
if "!choice!"=="2" goto :DO_BUILD
if "!choice!"=="3" goto :DO_DEBUG
if "!choice!"=="4" goto :DO_REBUILD
if "!choice!"=="5" goto :DO_CLEAN
if "!choice!"=="6" goto :DO_RUN
if "!choice!"=="0" exit /b 0
goto :PROMPT

:: ============================================================
:: Init
:: ============================================================
:DO_INIT
echo.
echo  [Init] cmake configure (Visual Studio 17 2022, x64) ...
echo.
call "%ComSpec%" /c "chcp 65001 >nul && cmake -S . -B build -G "Visual Studio 17 2022" -A x64"
if !ERRORLEVEL! neq 0 (
    echo.
    echo  [Init] FAILED.
    if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
    pause
    goto :MENU_START
)
echo.
echo  [Init] Done. Build dir: build\
if "!double_clicked!"=="false" if not "%~1"=="" exit /b 0
pause
goto :MENU_START

:: ============================================================
:: Build Release
:: ============================================================
:DO_BUILD
echo.
echo  [Build] Building Release ...
echo.
if not exist build\CMakeCache.txt (
    echo  [Build] Not configured yet. Running init first ...
    echo.
    call "%ComSpec%" /c "chcp 65001 >nul && cmake -S . -B build -G "Visual Studio 17 2022" -A x64"
    if !ERRORLEVEL! neq 0 (
        echo.
        echo  [Init] FAILED.
        if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
        pause
        goto :MENU_START
    )
    echo.
)
call "%ComSpec%" /c "chcp 65001 >nul && cmake --build build --config Release"
if !ERRORLEVEL! neq 0 (
    echo.
    echo  [Build] FAILED.
    if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
    pause
    goto :MENU_START
)
echo.
echo  [Build] Done. Output: build\Release\clipwiz.exe
echo.
echo  [Deploy] Copying lang\ to build\Release\ ...
if not exist build\Release\lang mkdir build\Release\lang
xcopy /Y /Q lang\*.lng build\Release\lang\ >nul
echo  [Deploy] Done. build\Release\ is ready to use.
if "!double_clicked!"=="false" if not "%~1"=="" exit /b 0
pause
goto :MENU_START

:: ============================================================
:: Build Debug
:: ============================================================
:DO_DEBUG
echo.
echo  [Debug] Building Debug ...
echo.
if not exist build\CMakeCache.txt (
    echo  [Debug] Not configured yet. Running init first ...
    echo.
    call "%ComSpec%" /c "chcp 65001 >nul && cmake -S . -B build -G "Visual Studio 17 2022" -A x64"
    if !ERRORLEVEL! neq 0 (
        echo.
        echo  [Init] FAILED.
        if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
        pause
        goto :MENU_START
    )
    echo.
)
call "%ComSpec%" /c "chcp 65001 >nul && cmake --build build --config Debug"
if !ERRORLEVEL! neq 0 (
    echo.
    echo  [Debug] FAILED.
    if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
    pause
    goto :MENU_START
)
echo.
echo  [Debug] Done. Output: build\Debug\clipwiz.exe
if "!double_clicked!"=="false" if not "%~1"=="" exit /b 0
pause
goto :MENU_START

:: ============================================================
:: Rebuild (clean + release)
:: ============================================================
:DO_REBUILD
echo.
echo  [Rebuild] Clean + Release build ...
echo.
if not exist build\CMakeCache.txt (
    echo  [Rebuild] Not configured yet. Running init first ...
    echo.
    call "%ComSpec%" /c "chcp 65001 >nul && cmake -S . -B build -G "Visual Studio 17 2022" -A x64"
    if !ERRORLEVEL! neq 0 (
        echo.
        echo  [Init] FAILED.
        if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
        pause
        goto :MENU_START
    )
    echo.
)
call "%ComSpec%" /c "chcp 65001 >nul && cmake --build build --config Release --clean-first"
if !ERRORLEVEL! neq 0 (
    echo.
    echo  [Rebuild] FAILED.
    if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
    pause
    goto :MENU_START
)
echo.
echo  [Rebuild] Done. Output: build\Release\clipwiz.exe
echo.
echo  [Deploy] Copying lang\ to build\Release\ ...
if not exist build\Release\lang mkdir build\Release\lang
xcopy /Y /Q lang\*.lng build\Release\lang\ >nul
echo  [Deploy] Done. build\Release\ is ready to use.
if "!double_clicked!"=="false" if not "%~1"=="" exit /b 0
pause
goto :MENU_START

:: ============================================================
:: Clean
:: ============================================================
:DO_CLEAN
echo.
echo  [Clean] Removing build directory ...
if exist build rmdir /s /q build
echo  [Clean] Done.
if "!double_clicked!"=="false" if not "%~1"=="" exit /b 0
pause
goto :MENU_START

:: ============================================================
:: Run
:: ============================================================
:DO_RUN
if not exist build\Release\clipwiz.exe (
    echo.
    echo  [Run] build\Release\clipwiz.exe not found. Build first.
    if "!double_clicked!"=="false" if not "%~1"=="" exit /b 1
    pause
    goto :MENU_START
)
echo.
echo  [Run] Starting build\Release\clipwiz.exe ...
start "" "build\Release\clipwiz.exe"
echo  [Run] Launched.
if "!double_clicked!"=="false" if not "%~1"=="" exit /b 0
pause
goto :MENU_START

:: ============================================================
:: CLI dispatch
:: ============================================================
:CLI_DISPATCH
if /i "%~1"=="init"       goto :DO_INIT
if /i "%~1"=="build"      goto :DO_BUILD
if /i "%~1"=="debug"      goto :DO_DEBUG
if /i "%~1"=="rebuild"    goto :DO_REBUILD
if /i "%~1"=="clean"      goto :DO_CLEAN
if /i "%~1"=="run"        goto :DO_RUN
goto :USAGE

:: ============================================================
:: Usage
:: ============================================================
:USAGE
echo.
echo  Usage: %~nx0 ^<command^>
echo.
echo  Commands:
echo    init       cmake configure (Visual Studio 17 2022, x64)
echo    build      Build Release
echo    debug      Build Debug
echo    rebuild    Clean + Release build
echo    clean      Remove build directory
echo    run        Launch Release exe
echo.
echo  No arguments + double-click = interactive menu.
echo.
exit /b 1
