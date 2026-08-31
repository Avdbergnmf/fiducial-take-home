# Prove the brain is deterministic. Run it EARLY and after anything that
# touches iteration order, containers, or state that survives a tick.
#
#     powershell -ExecutionPolicy Bypass -File scripts\determinism.ps1
#     powershell -ExecutionPolicy Bypass -File scripts\determinism.ps1 -Scenarios s1,x1-a
#
# A PowerShell port of tools\check_determinism.sh, which is bash and needs WSL.
# Same two checks:
#   1. thread-order   --threads 1 --record, then --threads 8 --replay
#   2. repeatability  the same command twice, reports compared
#
# CHALLENGE.md 11.2: a mismatch is usually a wall-clock read, unseeded
# randomness, uninitialised memory, or iterating a container whose order depends
# on allocation addresses. It is much cheaper to find now than to debug later.

[CmdletBinding()]
param(
    [string[]]$Scenarios = @("s1", "x1-a", "x2-a"),
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

# -File hands "s1,x1-a" over as one string; normalise before iterating.
$Scenarios = Expand-IdList $Scenarios

$brain = Find-BrainLibrary -BuildDir (Join-Path $RepoRoot "brain\build") -Config $Config
if (-not $brain) { throw "No brain library. Run scripts\build.ps1 first." }
$brain = ConvertTo-AbsolutePath $brain

$work = Join-Path ([System.IO.Path]::GetTempPath()) ("swarmdet_" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Force -Path $work | Out-Null

# These five vary between two identical runs BY DESIGN: CHALLENGE.md 9.3 calls
# compute "a wall-clock reading" and does not score it. Comparing whole reports
# byte for byte reports a failure every time, which hides a real regression.
$timingFields = @("mean_tick_us", "p99_tick_us", "overruns", "realtime_factor", "wall_time_s")

function Get-StableReport {
    param([Parameter(Mandatory)][string]$Path)
    (Get-Content -LiteralPath $Path) |
        Where-Object { $line = $_; -not ($timingFields | Where-Object { $line -match ('"{0}"' -f $_) }) }
}

$pass = 0
$fail = 0

try {
    foreach ($id in $Scenarios) {
        Write-Host ""
        Write-Host ("=== {0} ===" -f $id) -ForegroundColor Cyan

        $hash = Join-Path $work "$id.hash"
        $repA = Join-Path $work "$id.a.json"
        $repB = Join-Path $work "$id.b.json"

        # 1. thread order -- the strongest check.
        $code = Invoke-SimRaw -Quiet -RepoRoot $RepoRoot -SimArgs @(
            "--scenario", $id, "--brain", $brain, "--threads", "1",
            "--record", $hash, "--report", $repA, "--quiet")
        if ($code -ne 0) {
            Write-Host ("  RUN FAILED at --threads 1 (exit {0})" -f $code) -ForegroundColor Red
            $fail++
            continue
        }

        $code = Invoke-SimRaw -Quiet -RepoRoot $RepoRoot -SimArgs @(
            "--scenario", $id, "--brain", $brain, "--threads", "8",
            "--replay", $hash, "--quiet")
        if ($code -eq 0) {
            Write-Host "  thread-order: ok" -ForegroundColor Green
            $pass++
        }
        else {
            Write-Host ("  thread-order: DIVERGED (exit {0})" -f $code) -ForegroundColor Red
            Write-Host "  usual causes: wall-clock read; unseeded randomness; uninitialised"
            Write-Host "  memory; iterating a container whose order depends on allocation"
            Write-Host "  addresses; an unstable sort over equal keys."
            $fail++
            continue
        }

        # 2. repeatability -- catches what --replay cannot, since replay only
        #    compares against a hash of run one.
        $code = Invoke-SimRaw -Quiet -RepoRoot $RepoRoot -SimArgs @(
            "--scenario", $id, "--brain", $brain, "--threads", "1",
            "--report", $repB, "--quiet")
        if ($code -ne 0) {
            Write-Host ("  repeatability: second run failed (exit {0})" -f $code) -ForegroundColor Red
            $fail++
            continue
        }

        $diff = Compare-Object (Get-StableReport $repA) (Get-StableReport $repB)
        if (-not $diff) {
            Write-Host "  repeatability: ok" -ForegroundColor Green
            $pass++
        }
        else {
            Write-Host "  repeatability: reports DIFFER between two identical runs" -ForegroundColor Red
            $diff | Select-Object -First 10 | ForEach-Object {
                Write-Host ("    {0} {1}" -f $_.SideIndicator, $_.InputObject)
            }
            $fail++
        }
    }
}
finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

Write-Host ""
if ($fail -eq 0) {
    Write-Host ("{0} check(s) passed, 0 failed" -f $pass) -ForegroundColor Green
}
else {
    Write-Host ("{0} check(s) passed, {1} FAILED" -f $pass, $fail) -ForegroundColor Red
    exit 1
}
