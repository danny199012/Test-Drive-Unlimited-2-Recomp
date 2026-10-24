@echo off
rem Play Test Drive Unlimited 2 (Xbox 360, ReXGlue static recomp).
rem
rem Usage:
rem   play.bat                       play the perf build (-O3 + LTO). Use this.
rem   play.bat -release              play the release (-O3) build
rem   play.bat -debug                play the debug build (slow; only for debugging)
rem   play.bat --fullscreen --log_level debug
rem                                  anything that is not -perf/-release/-debug is
rem                                  forwarded to tdu2.exe unchanged
rem
rem Two flags are always added and are NOT optional:
rem
rem   --game_data_root <dir>
rem     The runtime reads the game_data_root cvar in ReXApp::SetupEnvironment
rem     BEFORE it loads tdu2.toml, so the value in the config file never reaches
rem     the startup check and the game exits with
rem       "--game_data_root was not provided."
rem     The command line is the highest-precedence source, so it always works.
rem
rem   --gpu_plugin xenos
rem     Loads rexgpu-xenos.dll (the Xenos GPU emulator) from next to tdu2.exe.
rem     Without it the log says "no GPU emulation loaded (gpu_plugin not set)"
rem     and there is no graphics at all.
rem
rem The working directory is the build dir: that is where tdu2.toml, the staged
rem runtime/GPU DLLs, logs\ and cvars_full.txt live.
setlocal

set "ROOT=%~dp0"
set "GAMEDATA=%ROOT%TDU2"

rem --- pick the build (-perf / -release / -debug, same names as build.cmd) ------
rem Everything else is collected into PASSTHRU and handed to the game. %* cannot
rem be used here: `shift` does not rewrite it, so a leading -debug would leak
rem through to the game as an unknown flag.
set "CONFIG=win-amd64-perf"
set "PASSTHRU="
:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="-perf" (
    set "CONFIG=win-amd64-perf"
    shift
    goto parse_args
)
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
if /i "%~1"=="-debug" (
    set "CONFIG=win-amd64-debug"
    shift
    goto parse_args
)
set "PASSTHRU=%PASSTHRU% "%~1""
shift
goto parse_args
:args_done

set "BUILD=%ROOT%out\build\%CONFIG%"
set "EXE=%BUILD%\tdu2.exe"
set "GPUDLL=%BUILD%\rexgpu-xenos.dll"
if /i "%CONFIG%"=="win-amd64-debug" set "GPUDLL=%BUILD%\rexgpu-xenosd.dll"

if not exist "%EXE%" (
    echo [play.bat] Error: %EXE% not found. 1>&2
    echo [play.bat] Build it first, e.g. "build.cmd -perf tdu2". 1>&2
    pause
    exit /b 1
)
if not exist "%GAMEDATA%\Default.xex" (
    echo [play.bat] Error: game content not found at "%GAMEDATA%" ^(no Default.xex^). 1>&2
    echo [play.bat] The repo does not ship the game data; point TDU2\ at your own copy. 1>&2
    pause
    exit /b 1
)
if not exist "%GPUDLL%" (
    echo [play.bat] Warning: %GPUDLL% is missing - no graphics emulation will load. 1>&2
)

echo [play.bat] build   : %CONFIG%
echo [play.bat] exe     : %EXE%
echo [play.bat] content : %GAMEDATA%
if defined PASSTHRU echo [play.bat] extra   :%PASSTHRU%
echo.

pushd "%BUILD%"
"%EXE%" --game_data_root "%GAMEDATA%" --gpu_plugin xenos%PASSTHRU%
set "RC=%ERRORLEVEL%"
popd

echo.
echo [play.bat] tdu2.exe exited with code %RC%
if not "%RC%"=="0" echo [play.bat] Newest log: "%BUILD%\logs".
pause
exit /b %RC%
