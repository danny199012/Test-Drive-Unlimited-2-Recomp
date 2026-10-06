# Puts tdu2.toml.prev back over tdu2.toml after a test run.
#
# Run this AFTER closing the game window. tdu2.exe rewrites tdu2.toml itself on
# shutdown (that's what kept writing the unescaped "..\..\..\TDU2" path), so a
# restore performed while the game is still running gets clobbered.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File _restore_toml.ps1

$ErrorActionPreference = 'Stop'
$root = 'E:\Xbox 360 Games\Torrent\Minerva_Myrient\Redump\Microsoft - Xbox 360\Rex Glue TDU 2 Project'
$build = Join-Path $root 'out\build\win-amd64-perf'
$toml = Join-Path $build 'tdu2.toml'
$prev = Join-Path $build 'tdu2.toml.prev'
$flag = Join-Path $root '_pending_restore.txt'

if (Test-Path $flag) {
  Write-Host "a restore is pending for test tag: $((Get-Content $flag -Raw).Trim())"
}

if (-not (Test-Path $prev)) {
  throw "no backup at $prev - nothing to restore"
}

if (Get-Process tdu2 -ErrorAction SilentlyContinue) {
  throw 'tdu2.exe is still running; close the game window first (it rewrites tdu2.toml on exit)'
}

Copy-Item $prev $toml -Force
Write-Host "restored $toml from $prev"

# Strip experimental cvars that must never survive a restore.
#
# tdu2.toml.prev is only a true baseline if it was captured before any test
# mutated it. That assumption broke once already: the PRIM run left
# force_convert_* = true behind, and because tdu2.exe *rewrites* tdu2.toml on
# shutdown from its own live cvar set, the game re-persisted those values into
# the very file that later runs then backed up. The experiment cvars quietly
# became the permanent baseline, so every subsequent A/B comparison was
# measuring nothing.
#
# These are only ever set by _run_intro_test.ps1, so removing them here is safe.
$testCvars = '^\s*(force_convert_\w+|depth_float24_round)\s*='
$lines = Get-Content $toml
$stripped = $lines | Where-Object { $_ -notmatch $testCvars }
if ($stripped.Count -ne $lines.Count) {
  [System.IO.File]::WriteAllLines($toml, $stripped, (New-Object System.Text.UTF8Encoding($false)))
  Write-Host "removed $($lines.Count - $stripped.Count) leftover test cvar line(s)"
}

Get-Content $toml | ForEach-Object { "  $_" }

if (Test-Path $flag) { Remove-Item $flag -Force }