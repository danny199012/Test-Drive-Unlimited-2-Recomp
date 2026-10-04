@echo off
rem Build the TDU2 (Test Drive Unlimited 2, Xbox 360) ReXGlue project.
rem
rem Toolchain: clang + Ninja + CMake. The SDK headers and the generated
rem recomp code use clang builtins, so plain MSVC cl cannot compile them.
rem Preset: win-amd64-debug (out\build\win-amd64-debug); -release for Release.
rem
rem Usage:
rem   build.cmd                run codegen only (fast; regenerates C++ from the XEX)
rem   build.cmd tdu2           build the full recompiled executable
rem   build.cmd -release [t]   build as Release (-O3)
rem   build.cmd -r [t]         (same, short form)
rem   build.cmd -perf [t]      Release + -O3 + LTO, no debug info. MUCH faster
rem                             at runtime than Debug (-O0) -- use this to play.
rem                             First build is slow (500+ large TUs); after that
rem                             incremental builds are quick.
setlocal EnableDelayedExpansion
cd /d "%~dp0"

rem LLVM: prefer clang++ already on PATH, else the default install location.
where clang++ >nul 2>nul
if errorlevel 1 (
    if exist "C:\Program Files\LLVM\bin\clang++.exe" set "LLVM=C:\Program Files\LLVM\bin"
    if not defined LLVM (
        echo Error: LLVM not found on PATH and no clang++.exe at C:\Program Files\LLVM\bin 1>&2
        echo        Install LLVM - https://llvm.org - or add its bin\ to PATH. 1>&2
        exit /b 1
    )
)
if defined LLVM set "PATH=%LLVM%;%PATH%"

rem Ninja: prefer one already on PATH, else a WinGet / Program Files /
rem chocolatey install. Kept in a subroutine (not an inline block) so the
rem wildcard expansion cannot be mangled by block parsing.
where ninja >nul 2>nul
if errorlevel 1 (
    echo [build.cmd] ninja not on PATH; searching known install dirs...
    call :find_ninja
)
where ninja >nul 2>nul
if errorlevel 1 (
    echo Error: ninja not found on PATH and not under 1>&2
    echo        "%LOCALAPPDATA%\Microsoft\WinGet\Packages" 1>&2
    echo        Install it: winget install Ninja-build.Ninja 1>&2
    exit /b 1
)

rem ReXGlue SDK: this repo ships the SDK under rexglue\win-amd64.
set "REXSDK=%~dp0rexglue\win-amd64"
if not exist "%REXSDK%\lib\cmake\rexglue\rexglueConfig.cmake" (
    echo Error: ReXGlue SDK not found at %REXSDK% 1>&2
    exit /b 1
)

rem Argument parsing: -release / -r selects the Release preset; the first
rem non-flag argument is the CMake target (default: tdu2_codegen).
set "CONFIG=win-amd64-debug"
set "TARGET="
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="-release" (
    set "CONFIG=win-amd64-release"
    shift
    goto parse_args
)
if /i "%~1"=="-r" (
    set "CONFIG=win-amd64-release"
    shift
    goto parse_args
)
if /i "%~1"=="-perf" (
    set "CONFIG=win-amd64-perf"
    shift
    goto parse_args
)
set "TARGET=%~1"
shift
goto parse_args
:args_done
if "%TARGET%"=="" set "TARGET=tdu2_codegen"

cmake --preset %CONFIG% -DCMAKE_PREFIX_PATH="%REXSDK%" -DREXGLUE_SDK_ROOT="%REXSDK%" || exit /b 1
cmake --build out\build\%CONFIG% --target %TARGET%
exit /b %ERRORLEVEL%

rem --- subroutines ---
rem Locate ninja and prepend its directory to PATH. Tried in order: ~/bin,
rem the WinGet package dir, %ProgramFiles%\Ninja, and chocolatey.
rem
rem Two cmd quirks bite here and both fail SILENTLY (so verify if it stops
rem working):
rem   * inside a parenthesised block "%%~dp" of a wildcard match comes back
rem     mangled (a literal "p" glued onto the path), and
rem   * inside a block "%%~d" drops the trailing backslash.
rem Hence the explicit "%%~d\" concatenation below.
:find_ninja
if exist "%USERPROFILE%\bin\ninja.exe" set "PATH=%USERPROFILE%\bin;%PATH%"
for /d %%d in ("%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_*") do (
    if exist "%%~d\ninja.exe" set "PATH=%%~d;%PATH%"
)
for /d %%d in ("%ProgramFiles%\Ninja") do (
    if exist "%%~d\ninja.exe" set "PATH=%%~d;%PATH%"
)
for /d %%d in ("%ProgramData%\chocolatey\lib\ninja\tools") do (
    if exist "%%~d\ninja.exe" set "PATH=%%~d;%PATH%"
)
exit /b 0