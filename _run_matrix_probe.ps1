param([string]$Tag = 'MPROBE')
$ErrorActionPreference = 'Stop'
$root = 'E:\Xbox 360 Games\Torrent\Minerva_Myrient\Redump\Microsoft - Xbox 360\Rex Glue TDU 2 Project'
$build = Join-Path $root 'out\build\win-amd64-perf'
Get-Process -Name 'tdu2' -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
$gameRoot = (Resolve-Path (Join-Path $build '..\..\..\TDU2')).Path -replace '\\', '/'
$proc = Start-Process -FilePath (Join-Path $build 'tdu2.exe') -ArgumentList @('--game_data_root', ('"' + $gameRoot + '"')) -WorkingDirectory $build -PassThru
Start-Sleep 12
if ($proc.HasExited) { Write-Host "EXITED $($proc.ExitCode)"; exit 1 }
& (Join-Path $root '_capture_intro.ps1') -Tag $Tag -Offsets @(150,180,200,220,240,260)
if (-not $proc.HasExited) {
  $null = $proc.CloseMainWindow()
  if (-not $proc.WaitForExit(20000)) { Stop-Process -Id $proc.Id -Force; Start-Sleep 2 }
}
Write-Host 'run complete'
