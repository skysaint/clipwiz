@echo off
chcp 65001 >nul
setlocal enabledelayedexpansion
title ClipWiz Build Tool
cd /d "%~dp0"

:: ============================================================
:: Detect: double-click (explorer) vs command-line (cmd/pwsh)
::
:: Two stages, strictly ordered:
::   Stage 1 — lightweight pure-cmd heuristic (zero external calls)
::     cmdcmdline has "/c"     AND     %~0 == absolute full path
::     -> likely double-click or pwsh; proceed to stage 2.
::     Otherwise -> definitely interactive cmd.exe shell -> CLI.
::
::   Stage 2 — climb 6 levels of parent process, but only the
::     slots that belong to the cmd.exe that owns this .bat.
::     PowerShell [0] -> bat's cmd.exe [1] -> REAL caller [2..4]
::     If any of [1..3] equals explorer.exe        -> real double-click
::     If any of [1..3] equals pwsh / powershell /
::                    WindowsTerminal / conhost    -> CLI invocation
::     Fallback (probe failed, unexpected chain)   -> assume DC so
::                                                      window stays.
:: ============================================================
set "double_clicked=false"

set "_has_slash_c=0"
set "_s=!cmdcmdline!"
if not "!_s!"=="!_s:/c=!" set "_has_slash_c=1"
set "_s="

set "_arg0_abs=0"
if "%~0"=="%~dpnx0" set "_arg0_abs=1"

if not "!_has_slash_c!!_arg0_abs!"=="11" goto STAGE2_DONE
call :PROBE_PARENT_CHAIN
set "_probe_rc=%errorlevel%"
if "!_probe_rc!"=="0" set "double_clicked=true"
set "_probe_rc="
:STAGE2_DONE
set "_has_slash_c="
set "_arg0_abs="

:: CLI mode: no args + shell invocation = usage; with args = dispatch
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
echo   [0] Exit
echo.

:PROMPT
set "choice="
set /p "choice=  Select (0-5): "
if "!choice!"=="" goto :PROMPT

if "!choice!"=="1" goto :DO_INIT
if "!choice!"=="2" goto :DO_BUILD
if "!choice!"=="3" goto :DO_DEBUG
if "!choice!"=="4" goto :DO_REBUILD
if "!choice!"=="5" goto :DO_CLEAN
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
:: Deploy: copy non-built-in language files (zh-CN is compiled into exe)
set "has_extra_lng=false"
for %%f in (lang\*.lng) do (
    if /i not "%%~nxf"=="zh-CN.lng" set "has_extra_lng=true"
)
if "!has_extra_lng!"=="true" (
    echo  [Deploy] Copying extra language files to build\Release\lang\ ...
    if not exist build\Release\lang mkdir build\Release\lang
    for %%f in (lang\*.lng) do (
        if /i not "%%~nxf"=="zh-CN.lng" copy /Y "%%f" build\Release\lang\ >nul
    )
    echo  [Deploy] Done.
) else (
    echo  [Deploy] No extra language files. Release is a single exe.
)
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
:: Deploy: copy non-built-in language files (zh-CN is compiled into exe)
set "has_extra_lng=false"
for %%f in (lang\*.lng) do (
    if /i not "%%~nxf"=="zh-CN.lng" set "has_extra_lng=true"
)
if "!has_extra_lng!"=="true" (
    echo  [Deploy] Copying extra language files to build\Release\lang\ ...
    if not exist build\Release\lang mkdir build\Release\lang
    for %%f in (lang\*.lng) do (
        if /i not "%%~nxf"=="zh-CN.lng" copy /Y "%%f" build\Release\lang\ >nul
    )
    echo  [Deploy] Done.
) else (
    echo  [Deploy] No extra language files. Release is a single exe.
)
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
:: CLI dispatch
:: ============================================================
:CLI_DISPATCH
if /i "%~1"=="init"       goto :DO_INIT
if /i "%~1"=="build"      goto :DO_BUILD
if /i "%~1"=="debug"      goto :DO_DEBUG
if /i "%~1"=="rebuild"    goto :DO_REBUILD
if /i "%~1"=="clean"      goto :DO_CLEAN
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
echo.
echo  No arguments + double-click from explorer = interactive menu.
echo  No arguments + cmd/PowerShell invocation   = show this help.
echo.
exit /b 1

:: ============================================================
:: Probe helper.  Caller must ensure Stage 1 already passed
:: (!_has_slash_c! AND !_arg0_abs! both 1).  exit 0 = DC found,
:: exit 1 = CLI shell (pwsh / cmd / terminal).  Fallback when
:: the chain looks weird / probe fails = exit 0 (assume DC so
:: the window never silently closes).
:: ============================================================
:PROBE_PARENT_CHAIN
set "_ps1=%temp%\cw_build_%random%_%random%.ps1"
> "%_ps1%" echo $id = $PID
>>"%_ps1%" echo for ($i=0; $i -lt 6; $i++) {
>>"%_ps1%" echo   $p = Get-CimInstance Win32_Process -Filter ('ProcessId=' + $id) -ErrorAction SilentlyContinue
>>"%_ps1%" echo   if (-not $p) { break }
>>"%_ps1%" echo   $n = $p.Name
>>"%_ps1%" echo   if ($i -ge 1 -and $i -le 3) {
>>"%_ps1%" echo     if ($n -ieq 'explorer.exe') { Write-Output 'DC'; exit 0 }
>>"%_ps1%" echo     if ($n -ieq 'cmd.exe') { } else {
>>"%_ps1%" echo       if ($n -ieq 'powershell.exe' -or $n -ieq 'pwsh.exe' -or $n -ieq 'WindowsTerminal.exe' -or $n -ieq 'conhost.exe') { Write-Output 'PS'; exit 0 }
>>"%_ps1%" echo     }
>>"%_ps1%" echo   }
>>"%_ps1%" echo   $pp = [int]$p.ParentProcessId
>>"%_ps1%" echo   if ($pp -le 0) { break }
>>"%_ps1%" echo   $id = $pp
>>"%_ps1%" echo }
>>"%_ps1%" echo Write-Output 'DC'

set "_r=PS"
for /f "usebackq delims=" %%I in (`powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%_ps1%" 2^>nul`) do set "_r=%%I"
set "_ec=1"
if /i "%_r%"=="DC" set "_ec=0"
set "_r="
del /q "%_ps1%" 2>nul
set "_ps1="
exit /b %_ec%
