# THE tight loop. One command from an edited source file to a score and a
# picture you can drive. Reach for this after every single change.
#
#     powershell -ExecutionPolicy Bypass -File scripts\iterate.ps1
#     powershell -ExecutionPolicy Bypass -File scripts\iterate.ps1 -Scenario x1-a
#     powershell -ExecutionPolicy Bypass -File scripts\iterate.ps1 -Note "tighter miss gate"
#     powershell -ExecutionPolicy Bypass -File scripts\iterate.ps1 -Fast
#
# It builds, runs one scenario with --trace and --report, converts the trace for
# the viewer, copies it into StreamingAssets, appends a row to runs\history.csv
# and prints the score. -Fast skips the viewer half when you only want a number.
#
# Every path is quoted throughout: the repo path contains spaces.

[CmdletBinding()]
param(
    [string]$Scenario = "s1",
    [string]$Stem = "last",
    [string]$Note = "",
    [string]$Config = "Release",
    [switch]$Fast,
    [switch]$NoBuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

$runsDir = Join-Path $RepoRoot "runs"
New-Item -ItemType Directory -Force -Path $runsDir | Out-Null
$stemPath = Join-Path $runsDir $Stem
$trace = "$stemPath.jsonl"
$report = "$stemPath.json"

# --- 1. build -------------------------------------------------------------
if (-not $NoBuild) {
    & (Join-Path $PSScriptRoot "build.ps1") -Config $Config
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
}

$brain = Find-BrainLibrary -BuildDir (Join-Path $RepoRoot "brain\build") -Config $Config
if (-not $brain) { throw "No brain library. Run scripts\build.ps1 first." }

# --- 2. run ---------------------------------------------------------------
# Absolute paths, because the simulator's working directory is pkg\.
$code = Invoke-SimRaw -RepoRoot $RepoRoot -SimArgs @(
    "--scenario", $Scenario,
    "--brain", (ConvertTo-AbsolutePath $brain),
    "--trace", (ConvertTo-AbsolutePath $trace),
    "--report", (ConvertTo-AbsolutePath $report)
)
if ($code -ne 0) {
    # A crash matters more than any score. Say so loudly and stop.
    Write-Host ""
    Write-Host "RUN FAILED, exit code $code" -ForegroundColor Red
    Write-Host "  2 = brain failed to load, 3 = brain crashed, 4 = ABI violation" -ForegroundColor Red
    throw "swarm_sim exited with code $code"
}

# --- 3. the number --------------------------------------------------------
$s = Get-RunSummary -Path $report

Write-Host ""
Write-Host ("=== {0}  total {1,10:N1}   {2} ===" -f $s.Id, $s.Total, $s.Outcome) -ForegroundColor Cyan
if ($s.CivPairCredit -gt 0) {
    Write-Host ("  adjusted total includes civilian-pair floor credit +{0:N0} (raw {1:N1})" -f `
            $s.CivPairCredit, $s.RawTotal) -ForegroundColor DarkGray
}
Write-Host ("  mission {0,9:N1}   awareness {1,6:N1}   comms {2,6:N1}" -f $s.Mission, $s.Awareness, $s.Comms)
Write-Host ("  kills {0}/{1}   breaches {2}   wasted {3}   civilians {4}" -f `
        $s.Kills, $s.HostilesTotal, $s.Breaches, $s.Wasted, $s.Civilians)
if ($s.Causes) {
    # CHALLENGE.md 9.2: this is where a naive swarm's bleed shows up. Look here
    # first when a score surprises you.
    Write-Host ("  losses by cause: {0}" -f $s.Causes) -ForegroundColor Yellow
}
Write-Host ("  declarations {0} correct / {1} wrong" -f $s.Correct, $s.Wrong)
$p95 = if ($null -eq $s.P95 -or $s.P95 -eq '') { "null" } else { ("{0:N2} s" -f [double]$s.P95) }
Write-Host ("  hops {0}   p95 {1}" -f $s.MaxHops, $p95)
if ($s.Crashes -gt 0 -or $s.AbiViolations -gt 0) {
    Write-Host ("  INTEGRITY: {0} crash(es), {1} ABI violation(s)" -f $s.Crashes, $s.AbiViolations) -ForegroundColor Red
}

# --- 4. history -----------------------------------------------------------
# Appended every run so the day has a visible trend rather than a feeling.
$histPath = Join-Path $runsDir "history.csv"
$row = [pscustomobject]@{
    when      = (Get-Date).ToString("s")
    scenario  = $s.Id
    total     = [math]::Round($s.Total, 1)
    mission   = [math]::Round($s.Mission, 1)
    awareness = [math]::Round($s.Awareness, 1)
    comms     = [math]::Round($s.Comms, 1)
    kills     = $s.Kills
    breaches  = $s.Breaches
    wasted    = $s.Wasted
    civilians = $s.Civilians
    causes    = $s.Causes
    correct   = $s.Correct
    wrong     = $s.Wrong
    note      = $Note
}
if (Test-Path $histPath) {
    $row | Export-Csv -LiteralPath $histPath -NoTypeInformation -Append
}
else {
    $row | Export-Csv -LiteralPath $histPath -NoTypeInformation
}

# --- 5. the picture -------------------------------------------------------
if ($Fast) {
    Write-Host ""
    Write-Host "-Fast: skipped the viewer build. Drop -Fast when you want to look." -ForegroundColor DarkGray
}
else {
    Push-Location $RepoRoot
    try {
        python "tools\build_viewer_data.py" "$trace" "$stemPath"
        if ($LASTEXITCODE -ne 0) { throw "build_viewer_data.py failed" }
    }
    finally {
        Pop-Location
    }
    & (Join-Path $PSScriptRoot "sync_viewer_data.ps1") -Stem $Stem
}

Write-Host ""
Write-Host ("history: {0}" -f $histPath) -ForegroundColor Green
