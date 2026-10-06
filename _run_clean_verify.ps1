# Runs the game with NO debug logging (clean performance) and captures the
# party scene for visual verification of the forced-vfetch-reupload fix.

param(
  [string]$Tag = 'CLEAN',
  [int[]]$Offsets = @(140, 180, 220, 260)
)

$ErrorActionPreference = 'Stop'
$root = 'E:\Xbox 360 Games\Torrent\Minerva_Myrient\Redump\Microsoft - Xbox 360\Rex Glue TDU 2 Project'
$build = Join-Path $root 'out\build\win-amd64-perf'
$toml = Join-Path $build 'tdu2.toml'
$prev = Join-Path $build 'tdu2.toml.prev'

$running = Get-Process -Name 'tdu2' -ErrorAction SilentlyContinue
if ($running) {
  $running | Stop-Process -Force
  Start-Sleep -Seconds 3
}

Copy-Item $toml $prev -Force
# Leave the config as-is: no debug cvars. Just make sure game_data_root is sane.
$gameRoot = (Resolve-Path (Join-Path $build '..\..\..\TDU2')).Path -replace '\\', '/'
$argList = @('--game_data_root', ('"' + $gameRoot + '"'))
$exe = Join-Path $build 'tdu2.exe'
Write-Host "launching $exe (no debug logging)"
$proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $build -PassThru

Start-Sleep -Seconds 10
if ($proc.HasExited) {
  Copy-Item $prev $toml -Force
  Write-Host "ERROR: game exited with $($proc.ExitCode)"
  exit 1
}

& (Join-Path $root '_capture_intro.ps1') -Tag $Tag -Offsets $Offsets

Write-Host "capture done; stopping game (pid $($proc.Id))"
Start-Sleep -Seconds 2
if (-not $proc.HasExited) {
  $null = $proc.CloseMainWindow()
  if (-not $proc.WaitForExit(20000)) {
    Stop-Process -Id $proc.Id -Force
    Start-Sleep -Seconds 2
  }
}
Copy-Item $prev $toml -Force
Write-Host 'config restored'
