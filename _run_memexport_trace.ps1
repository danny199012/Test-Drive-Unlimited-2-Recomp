# Runs the intro with memexport_debug_log = true so the SDK logs vertex-fetch
# palette requests, shared-memory uploads/invalidations and readback activity.
#
# Lessons baked in:
# - Kills any running tdu2.exe first: two instances sharing the GPU is the
#   leading suspect for the "1 fps" observed in an earlier trace run.
# - log_flush_interval = 1: without periodic flushing the log lives in a
#   buffer and a hard-killed game loses everything after startup (an earlier
#   trace produced a 0.4-second log for a 5-minute session).
# - Close is graceful first (WM_CLOSE, then taskkill, then force) so the game
#   flushes its own log on shutdown.

param(
  [string]$Tag = 'TRACE',
  [int[]]$Offsets = @(25, 45, 70, 100, 140, 180, 220, 260)
)

$ErrorActionPreference = 'Stop'
$root = 'E:\Xbox 360 Games\Torrent\Minerva_Myrient\Redump\Microsoft - Xbox 360\Rex Glue TDU 2 Project'
$build = Join-Path $root 'out\build\win-amd64-perf'
$toml = Join-Path $build 'tdu2.toml'
$prev = Join-Path $build 'tdu2.toml.prev'

if (-not (Test-Path (Join-Path $build 'tdu2.exe'))) {
  throw "tdu2.exe not found in $build"
}

$running = Get-Process -Name 'tdu2' -ErrorAction SilentlyContinue
if ($running) {
  Write-Host ("killing leftover instance(s): " + ($running.Id -join ', '))
  $running | Stop-Process -Force
  Start-Sleep -Seconds 3
}

Copy-Item $toml $prev -Force

$lines = Get-Content $toml
$lines = $lines |
  Where-Object { $_ -notmatch '^\s*(memexport_debug_log|memwatch_debug_log|log_flush_interval|log_max_files)\s*=' }
$lines += 'memexport_debug_log = true'
$lines += 'log_flush_interval = 1'
# The session logs gigabytes at ~40k lines/s; the default 20x5MB window rotates
# the first seconds (where once-per-shader diagnostics land) out of existence.
$lines += 'log_max_files = 400'

# ASCII without BOM - a BOM makes the runtime's TOML parser discard the whole
# file (see _run_intro_test.ps1).
[System.IO.File]::WriteAllLines($toml, $lines, (New-Object System.Text.UTF8Encoding($false)))

$gameRoot = (Resolve-Path (Join-Path $build '..\..\..\TDU2')).Path -replace '\\', '/'
$argList = @('--game_data_root', ('"' + $gameRoot + '"'))
$exe = Join-Path $build 'tdu2.exe'
Write-Host "launching $exe $($argList -join ' ')"

# Snapshot the log dir before launch so the run's own log (the newest file
# created after this point) can be identified, then COPY it to a tag-named
# file when the game exits. A second game instance launched later would
# otherwise rewrite the same tdu2_NNN.log and destroy this run's data - that
# already happened twice.
$knownLogs = @(Get-ChildItem (Join-Path $build 'logs') -Filter '*.log' | Select-Object -ExpandProperty FullName)
$proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $build -PassThru
Start-Sleep -Seconds 6
$runLog = Get-ChildItem (Join-Path $build 'logs') -Filter '*.log' |
  Where-Object { $knownLogs -notcontains $_.FullName } |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($runLog) {
  Write-Host "this run writes $($runLog.Name)"
} else {
  Write-Host 'WARNING: no new log file detected; the run may reuse the newest existing one'
  $runLog = Get-ChildItem (Join-Path $build 'logs') -Filter '*.log' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

Start-Sleep -Seconds 4
if ($proc.HasExited) {
  Copy-Item $prev $toml -Force
  $logDir = Join-Path $build 'logs'
  $newest = Get-ChildItem $logDir -Filter '*.log' |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
  Write-Host "ERROR: tdu2.exe exited with code $($proc.ExitCode) after ~10s."
  if ($newest) {
    Write-Host "newest log: $($newest.Name)"
    Get-Content $newest.FullName |
      Select-String 'error|ERROR|FATAL' |
      Select-Object -First 10 | ForEach-Object { "  $($_.Line)" }
  }
  exit 1
}

& (Join-Path $root '_capture_intro.ps1') -Tag $Tag -Offsets $Offsets

Write-Host "capture done; stopping game (pid $($proc.Id))"
Start-Sleep -Seconds 2
if (-not $proc.HasExited) {
  $null = $proc.CloseMainWindow()
  if (-not $proc.WaitForExit(20000)) {
    taskkill /PID $proc.Id 2>$null | Out-Null
    if (-not $proc.WaitForExit(10000)) {
      Stop-Process -Id $proc.Id -Force
    }
    Start-Sleep -Seconds 2
  }
}

# The game rewrote tdu2.toml on shutdown; put the pre-test config back now.
Copy-Item $prev $toml -Force
Write-Host 'config restored from tdu2.toml.prev'

# Preserve this run's log under a unique name before anything else can touch it.
if ($runLog -and (Test-Path $runLog.FullName)) {
  $snapshot = Join-Path $root ("_trace_{0}_{1}.log" -f $Tag, (Get-Date -Format 'HHmmss'))
  Copy-Item $runLog.FullName $snapshot -Force
  Write-Host "log snapshot: $snapshot"
}
