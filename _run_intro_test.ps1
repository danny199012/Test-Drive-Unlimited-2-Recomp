# Runs one intro-scene cvar experiment end to end: writes tdu2.toml, launches
# tdu2.exe, screenshots the opening at fixed offsets, then restores the original
# config. Exists because the opening only lasts a short while and every manual
# attempt so far either missed it or tested the wrong cvars.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File _run_intro_test.ps1 -Tag B -Async off
#   powershell -ExecutionPolicy Bypass -File _run_intro_test.ps1 -Tag C -Async on -DepthFloat24Round
#
# The previous tdu2.toml is copied to tdu2.toml.prev before being overwritten and
# restored on exit, so the profile cvars (name/xuid) survive every run.

param(
  [string]$Tag = 'run',
  # 'on'|'off' -> async_shader_compilation. The hypothesis under test.
  # Mapped to a TOML boolean below; writing 'on'/'off' verbatim would be
  # invalid TOML and the line would be silently dropped.
  [ValidateSet('on', 'off')]
  [string]$Async = 'on',
  # Optional extra: depth_float24_round, the next-most-likely vertex-fetch knob.
  [switch]$DepthFloat24Round,
  # Turns on the primitive-processor topology fixes:
  #   force_convert_triangle_fans_to_lists  - Xenos emits triangle FANS for a
  #     lot of skinned character/prop geometry; if the fan decoder mis-assembles
  #     the primitive, triangles span unrelated vertices and you get stretched
  #     flat sheets and hard-edged wedges.
  #   force_convert_quad_lists_to_triangle_lists - same idea for quad lists.
  # This is the current best-supported theory for the party-scene geometry
  # artifact: it is per-draw-call, so it explains why most of the scene is
  # correct while isolated fragments are broken, and it is independent of
  # resolution (confirmed unaffected by the 720p run).
  [switch]$PrimitiveFix,
  # Offsets must reach the character-select party scene, where the artifact
  # actually appears. The early intro (ocean + crowd) renders clean, so short
  # offset lists silently produce a false "it works" result.
  [int[]]$Offsets = @(25, 45, 70, 100, 140, 180, 220, 260),
  # Guest render target. TDU2 is a 720p title; the standing config forces 4K
  # (3840x2160). Native is the single most suspicious setting, because any
  # float16 screen-space value in a vertex/shadow pass loses precision badly
  # above 2048 and 4K pushes such values toward the 65504 limit.
  # '' leaves the current setting alone.
  [ValidateSet('', '720p', '1080p', '1440p', '4k')]
  [string]$Resolution = '',
  [switch]$NoRestore
)

$ErrorActionPreference = 'Stop'
$root = 'E:\Xbox 360 Games\Torrent\Minerva_Myrient\Redump\Microsoft - Xbox 360\Rex Glue TDU 2 Project'
$build = Join-Path $root 'out\build\win-amd64-perf'
$toml = Join-Path $build 'tdu2.toml'
$prev = Join-Path $build 'tdu2.toml.prev'

if (-not (Test-Path (Join-Path $build 'tdu2.exe'))) {
  throw "tdu2.exe not found in $build - build the perf preset first"
}

# --- back up the live config, preserving profile identity ---------------------
# Save the pristine baseline ONCE, keyed by a sentinel file. Re-copying on every
# run is what allowed test cvars to become the permanent baseline: the game
# rewrites tdu2.toml on shutdown including whatever experimental cvars were
# live, so run N+1 then "backed up" run N's leftovers.
$baseline = Join-Path $build 'tdu2.toml.baseline'
if (-not (Test-Path $baseline)) {
  Copy-Item $toml $baseline -Force
  Write-Host "captured pristine baseline -> tdu2.toml.baseline"
} else {
  Write-Host 'reusing existing pristine baseline (tdu2.toml.baseline)'
}
Copy-Item $toml $prev -Force

$lines = Get-Content $toml
$lines = $lines | Where-Object { $_ -notmatch '^\s*#' -and $_.Trim() -ne '' }

function Set-Cvar([string[]]$lines, [string]$name, [string]$value) {
  $found = $false
  for ($i = 0; $i -lt $lines.Count; $i++) {
    if ($lines[$i] -match "^\s*$name\s*=") { $lines[$i] = "$name = $value"; $found = $true }
  }
  if (-not $found) { $lines += "$name = $value" }
  return , $lines
}

$lines = Set-Cvar $lines 'async_shader_compilation' $(if ($Async -eq 'off') { 'false' } else { 'true' })

# game_data_root MUST be written with forward slashes, and without quotes.
#
# The file this script overwrites is written by the game's own "Saved config to
# tdu2.toml" on shutdown, and it writes the path as "..\..\..\TDU2" - single
# backslashes inside a TOML basic string. That is INVALID TOML: '\' starts an
# escape sequence, so the entire file fails to parse and every cvar in it is
# silently discarded, including game_data_root. The symptom is the game's
# "--game_data_root was not provided." dialog despite the value being plainly
# visible in the file. Verified with a TOML parser against the real on-disk file.
#
# An absolute path avoids the escaping question entirely and also stops the
# value depending on the exe's working directory.
$gameRoot = (Resolve-Path (Join-Path $build '..\..\..\TDU2')).Path -replace '\\', '/'
# Quote it: the install path contains spaces ("Rex Glue TDU 2 Project"), and an
# unquoted TOML value may not. Forward slashes mean no escaping is required
# inside the quotes.
$lines = Set-Cvar $lines 'game_data_root' "`"$gameRoot`""
if ($DepthFloat24Round) {
  $lines = Set-Cvar $lines 'depth_float24_round' 'true'
}
if ($PrimitiveFix) {
  $lines = Set-Cvar $lines 'force_convert_triangle_fans_to_lists' 'true'
  $lines = Set-Cvar $lines 'force_convert_quad_lists_to_triangle_lists' 'true'
}
if ($Resolution -ne '') {
  # --resolution sets video_mode_* and the guest mode together, so clear the
  # explicit overrides or they win and the shorthand has no effect.
  $lines = $lines | Where-Object { $_ -notmatch '^\s*(video_mode_width|video_mode_height)\s*=' }
  $lines = Set-Cvar $lines 'resolution' "`"$Resolution`""
}

# Write ASCII, NOT -Encoding UTF8. Windows PowerShell 5.1's UTF8 writes a BOM
# (EF BB BF), and the runtime's TOML parser then fails on the first line, which
# silently drops game_data_root -> "--game_data_root was not provided" and the
# game exits before rendering. All values written here are ASCII anyway.
[System.IO.File]::WriteAllLines($toml, $lines, (New-Object System.Text.UTF8Encoding($false)))

# Parse the file back before launching. A malformed TOML file is discarded
# wholesale by the runtime with no useful diagnostic, so catching it here turns
# a confusing in-game dialog into an obvious local failure.
$verify = @'
import sys, tomllib, pathlib
p = pathlib.Path(sys.argv[1])
try:
    t = tomllib.loads(p.read_text(encoding="utf-8"))
except Exception as e:
    print("INVALID TOML:", e); sys.exit(1)
print("  game_data_root =", t.get("game_data_root"))
print("  resolution     =", t.get("resolution"))
print("  async_shader_compilation =", t.get("async_shader_compilation"))
'@
$verifyFile = Join-Path $env:TEMP '_verify_toml.py'
Set-Content -Path $verifyFile -Value $verify -Encoding ASCII
python $verifyFile $toml
if ($LASTEXITCODE -ne 0) {
  Copy-Item $prev $toml -Force
  throw "generated tdu2.toml is not valid TOML; restored previous config and aborted"
}

Write-Host "--- config for test '$Tag' ---"
Get-Content $toml | ForEach-Object { "  $_" }

# --- launch + capture --------------------------------------------------------
$exe = Join-Path $build 'tdu2.exe'

# Pass --game_data_root on the command line rather than relying on the TOML.
#
# The TOML value is provably valid and the game preserves it across shutdown,
# yet the runtime still reports "--game_data_root was not provided." - so the
# startup check is evidently not reading the key from tdu2.toml. The README's
# own launch line uses the flag, and the documented precedence is
# config < environment < command line, so this is both the proven-working path
# and the highest-precedence one. Keep the TOML entry as well.
# The path contains spaces ("Rex Glue TDU 2 Project"), so it must be quoted or
# the runtime receives a truncated path. Verified by round-tripping an argument
# list containing a space through Process.Start.
$argList = @('--game_data_root', ('"' + $gameRoot + '"'))
Write-Host "launching $exe $($argList -join ' ')"
$proc = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $build -PassThru

# Boot to the title takes ~10s; the first offset must clear it. Poll instead of
# a blind sleep: if the game dies on startup (bad config, missing path) we want
# to say so now, not after four minutes of screenshots of the desktop.
Start-Sleep -Seconds 10
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
  Write-Host 'config restored from tdu2.toml.prev'
  exit 1
}

& (Join-Path $root '_capture_intro.ps1') -Tag $Tag -Offsets $Offsets

Write-Host "test '$Tag' captured; game still running as pid $($proc.Id)"
Write-Host 'Close the game window, then re-run with the same -Tag and no -NoRestore'
Write-Host 'to write the config back, or copy tdu2.toml.prev over tdu2.toml by hand.'

# Restore on a successful run too. Previously the config was only restored on the
# early-exit path, so a successful experiment silently left the mutated toml in
# place and the *next* run inherited the previous test's cvars as its baseline -
# which invalidates any A/B comparison. Restoration is deferred (not immediate)
# because the game rewrites tdu2.toml on shutdown, so copying back now would just
# be overwritten.
if ($NoRestore) {
  Write-Host '-NoRestore given: leaving the test config in place.'
} else {
  Set-Content -Path (Join-Path $root '_pending_restore.txt') -Value $Tag -Encoding ASCII
  Write-Host 'config restore is PENDING (game rewrites tdu2.toml on shutdown).'
  Write-Host 'After closing the game, run _restore_toml.ps1 to put tdu2.toml.prev back.'
}