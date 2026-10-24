@echo off
rem perf_test.bat - run TDU2 unattended for N seconds, then summarise the fps.
rem
rem Usage:
rem   perf_test.bat                 120 s test of the perf build, tag "run"
rem   perf_test.bat 180 party       180 s test, results tagged "party"
rem   perf_test.bat 180 dbg debug   180 s test of the DEBUG build (O0 guest)
rem   perf_test.bat -s              no launch: summarise the newest perf CSV
rem
rem The game runs with the runtime's built-in per-frame CSV logger enabled
rem (perf_log_csv), writing perf_<tag>.csv next to the exe. tools\perf_summary.ps1
rem prints overall/median/p95/max fps and a per-10-second breakdown, so the
rem scene-dependent collapse (intro fine, party scene 0.3 fps) is visible.
rem
rem Notes:
rem   * kills every running tdu2.exe when the timer ends
rem   * the window opens focused; to test the focus behaviour, click away from
rem     it (or press Win) during the run and compare the per-10s buckets
rem   * works for any build of this project: copy perf_test.bat +
rem     tools\perf_summary.ps1 together
setlocal EnableDelayedExpansion
cd /d "%~dp0"

set "CONFIG=perf"
if /i "%~3"=="debug"   set "CONFIG=debug"
if /i "%~3"=="-debug"  set "CONFIG=debug"
if /i "%~3"=="dbg"     set "CONFIG=debug"
if /i "%~3"=="release" set "CONFIG=release"
set "BUILD=out\build\win-amd64-%CONFIG%"
set "EXE=%BUILD%\tdu2.exe"
set "GAMEDATA=%~dp0TDU2"
set "SUMMARY=tools\perf_summary.ps1"

if not exist "%EXE%" (
    echo [perf_test] Error: %EXE% not found. Build it first: build.cmd -%CONFIG% tdu2 1>&2
    exit /b 1
)

rem --- parse args -----------------------------------------------------------
set "SECS=%~1"
set "TAG=%~2"
if "%SECS%"=="-s" goto summary_only
if "%SECS%"=="" set "SECS=120"
if "%TAG%"=="" set "TAG=run"
set "CSV=%~dp0%BUILD%\perf_%TAG%.csv"
if exist "%CSV%" del "%CSV%"

echo [perf_test] launching %EXE% (%CONFIG% build) for %SECS% s (tag "%TAG%", csv perf_%TAG%.csv)
start "" /D "%BUILD%" "%EXE%" --game_data_root "%GAMEDATA%" --gpu_plugin xenos --perf_log_csv "%CSV%"

rem ping-sleep: timeout.exe fails when stdin is redirected, ping does not.
ping -n %SECS% 127.0.0.1 >nul
taskkill /f /t /im tdu2.exe >nul 2>&1
echo [perf_test] stopped; summarising...

:summarise
powershell -NoProfile -ExecutionPolicy Bypass -File "%SUMMARY%" -Csv "%CSV%" -Tag "%TAG%" -LogDir "%~dp0%BUILD%\logs"
exit /b %ERRORLEVEL%

:summary_only
rem newest perf_*.csv in the chosen build dir
if "%TAG%"=="" set "TAG=run"
for /f "delims=" %%f in ('dir /b /o-d "%BUILD%\perf_*.csv" 2^>nul') do (
    if not defined NEWEST set "NEWEST=%BUILD%\%%f"
)
if not defined NEWEST (
    echo [perf_test] No perf_*.csv found in %BUILD%. 1>&2
    exit /b 1
)
echo [perf_test] summarising !NEWEST!
powershell -NoProfile -ExecutionPolicy Bypass -File "%SUMMARY%" -Csv "!NEWEST!" -Tag summary
exit /b %ERRORLEVEL%
