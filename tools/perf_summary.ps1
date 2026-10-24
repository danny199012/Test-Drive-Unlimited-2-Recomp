# Summarise an unattended TDU2 perf test run.
#
# Primary source: the per-frame CSV written by the runtime's built-in
# perf_log_csv cvar (columns include frame_time_us and fps, one row per frame).
# Fallback: the newest logs\tdu2_*.log, counting the '[perf] fps=' lines that
# the fps-logging cvar emits once per second (present in SDK builds that carry
# that cvar).
#
# Usage: perf_summary.ps1 [-Csv <path>] [-Log <path>] [-Tag <name>]

param(
    [string]$Csv = "",
    [string]$Log = "",
    [string]$Tag = "run",
    [string]$LogDir = ""
)

$ErrorActionPreference = "Stop"

function Format-FpsRow([string]$label, [double]$avg, [double]$med, [double]$p95, [double]$max, [int]$n) {
    "{0,-26} avg {1,6:N1}  median {2,6:N1}  p95 {3,6:N1}  max {4,6:N1}  ({5} samples)" -f `
        $label, $avg, $med, $p95, $max, $n
}

# ---- CSV path: one row per frame -------------------------------------------
if ($Csv -and (Test-Path $Csv)) {
    $rows = Import-Csv $Csv
    if ($rows.Count -ge 2) {
        $ft = @($rows | ForEach-Object { [double]$_.frame_time_us } | Where-Object { $_ -gt 0 })
        $fpsCol = @($rows | ForEach-Object { [double]$_.fps } | Where-Object { $_ -gt 0 })
        $totalSec = ($ft | Measure-Object -Sum).Sum / 1e6
        $frames = $ft.Count
        $overall = if ($totalSec -gt 0) { $frames / $totalSec } else { 0 }
        $sorted = $fpsCol | Sort-Object
        $med = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.5)]
        $p95 = $sorted[[Math]::Min($sorted.Count - 1, [int][Math]::Floor(($sorted.Count - 1) * 0.95))]
        Write-Host "=== TDU2 perf test [$Tag] - per-frame CSV ($Csv) ==="
        Write-Host ("frames {0}  wall {1:N0} s  OVERALL {2:N1} fps" -f $frames, $totalSec, $overall)
        Format-FpsRow "instantaneous fps:" $overall $med $p95 (($fpsCol | Measure-Object -Maximum).Maximum) $fpsCol.Count

        # 10-second buckets make the scene-dependent collapse visible.
        Write-Host "`nper-10s buckets (avg fps, frames):"
        $t = 0.0; $bucket = New-Object System.Collections.Generic.List[double]
        $bucketStart = 0.0
        foreach ($us in $ft) {
            if ($t - $bucketStart -ge 10.0) {
                if ($bucket.Count) {
                    "{0,4:N0}-{1,4:N0} s  {2,6:N1} fps  {3,4} frames" -f $bucketStart, $t, `
                        ($bucket | Measure-Object -Average).Average, $bucket.Count
                }
                $bucketStart = $t; $bucket.Clear()
            }
            $bucket.Add(1e6 / $us); $t += $us / 1e6
        }
        if ($bucket.Count) {
            "{0,4:N0}-{1,4:N0} s  {2,6:N1} fps  {3,4} frames" -f $bucketStart, $t, `
                ($bucket | Measure-Object -Average).Average, $bucket.Count
        }
        exit 0
    }
    Write-Host "CSV $Csv exists but has no usable rows; falling back to the log."
}

# ---- Log path: one '[perf] fps=' line per second ----------------------------
if (-not $Log) {
    if (-not $LogDir) {
        $LogDir = Join-Path $PSScriptRoot "..\out\build\win-amd64-perf\logs"
    }
    $Log = Get-ChildItem "$LogDir\tdu2_*.log" -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not ($Log -and (Test-Path $Log))) {
    Write-Host "No CSV and no log found to summarise."; exit 1
}

$samples = Select-String -Path $Log -Pattern '\[perf\] fps=([0-9.]+)' |
    ForEach-Object { [double]$_.Matches[0].Groups[1].Value }
Write-Host "=== TDU2 perf test [$Tag] - log fps lines ($Log) ==="
if ($samples.Count -eq 0) {
    Write-Host "no '[perf] fps=' lines found - this build has no fps logging."
    exit 2
}
$sorted = $samples | Sort-Object
$med = $sorted[[int][Math]::Floor(($sorted.Count - 1) * 0.5)]
$p95 = $sorted[[Math]::Min($sorted.Count - 1, [int][Math]::Floor(($sorted.Count - 1) * 0.95))]
Format-FpsRow "fps (1-second samples):" (($samples | Measure-Object -Average).Average) $med $p95 `
    (($samples | Measure-Object -Maximum).Maximum) $samples.Count
