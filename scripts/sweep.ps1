# Run many scenarios and summarise them WORST FIRST.
#
#     powershell -ExecutionPolicy Bypass -File scripts\sweep.ps1
#     powershell -ExecutionPolicy Bypass -File scripts\sweep.ps1 -Scenarios s1,s2,x1-a
#     powershell -ExecutionPolicy Bypass -File scripts\sweep.ps1 -Tier 1 -Count 6
#     powershell -ExecutionPolicy Bypass -File scripts\sweep.ps1 -NoRun    # re-table an old sweep
#
# Reach for this after the tight loop says a change helped, to find out whether
# it helped everywhere or only on the one layout you were staring at.
#
# CHALLENGE.md 11.2: read the COMPLETION COUNT before any score -- a run that
# crashed or hung matters more than a bad number -- and read the WORST run, not
# the mean, because a mean hides the one layout the brain breaks on.

[CmdletBinding()]
param(
    [string[]]$Scenarios = @("s0", "s1", "s2", "x1-a", "x1-b", "x1-c", "x2-a", "x2-b"),
    [int]$Tier = -1,
    [int]$Count = 0,
    [int]$Jobs = 4,
    [string]$OutDir,
    [string]$Config = "Release",
    [switch]$NoRun
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

if (-not $OutDir) { $OutDir = Join-Path $RepoRoot "runs\sweep" }
$OutDir = ConvertTo-AbsolutePath $OutDir

# -File hands "s1,s2" over as one string; normalise before anything counts it.
$Scenarios = Expand-IdList $Scenarios

# -Tier N -Count K draws K fresh generated ids for that tier. CHALLENGE.md 11.1
# is explicit that a token you chose is a layout you will quietly fit to, so the
# tokens come from --new-token rather than from your head.
if ($Tier -ge 0 -and $Count -gt 0) {
    $drawn = @()
    for ($i = 0; $i -lt $Count; $i++) {
        Push-Location (Join-Path $RepoRoot "pkg")
        try {
            $exe = if (Test-Path "bin\swarm_sim.exe") { "bin\swarm_sim.exe" } else { "bin/swarm_sim" }
            $token = (& $exe --new-token) | Select-Object -First 1
        }
        finally { Pop-Location }
        $drawn += ("x{0}-{1}" -f $Tier, $token.Trim())
    }
    $Scenarios = $drawn
    Write-Host ("drew {0} fresh tier-{1} ids: {2}" -f $Count, $Tier, ($drawn -join ",")) -ForegroundColor DarkGray
}

if (-not $NoRun) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
    Get-ChildItem -LiteralPath $OutDir -Filter *.json -File -ErrorAction SilentlyContinue |
        Remove-Item -Force   # stale reports from a previous sweep would be read as fresh

    $brain = Find-BrainLibrary -BuildDir (Join-Path $RepoRoot "brain\build") -Config $Config
    if (-not $brain) { throw "No brain library. Run scripts\build.ps1 first." }

    # Real syntax is `--sweep --scenarios LIST`, NOT `--sweep LIST`.
    $code = Invoke-SimRaw -RepoRoot $RepoRoot -SimArgs @(
        "--sweep",
        "--scenarios", ($Scenarios -join ","),
        "--brain", (ConvertTo-AbsolutePath $brain),
        "--report-dir", $OutDir,
        "--jobs", "$Jobs"
    )
}
else {
    $code = 0
}

# --- completion count, before any score ----------------------------------
$reports = @(Get-ChildItem -LiteralPath $OutDir -Filter *.json -File -ErrorAction SilentlyContinue)
$expected = $Scenarios.Count
$got = $reports.Count

Write-Host ""
if ($NoRun) {
    # Re-tabling an existing directory: the -Scenarios list is not what produced
    # it, so a count mismatch here would mean nothing.
    Write-Host ("re-tabling {0} existing report(s) in {1}" -f $got, $OutDir) -ForegroundColor DarkGray
}
elseif ($got -ne $expected -or $code -ne 0) {
    Write-Host "*******************************************************" -ForegroundColor Red
    Write-Host ("*** {0}/{1} RUNS COMPLETED  (sweep exit code {2})" -f $got, $expected, $code) -ForegroundColor Red
    $missing = $Scenarios | Where-Object { -not (Test-Path (Join-Path $OutDir "$_.json")) }
    if ($missing) {
        Write-Host ("*** no report for: {0}" -f ($missing -join ", ")) -ForegroundColor Red
        Write-Host "*** that is a crash or a hang. Fix it before reading any score." -ForegroundColor Red
    }
    Write-Host "*******************************************************" -ForegroundColor Red
}
else {
    Write-Host ("{0}/{1} runs completed" -f $got, $expected) -ForegroundColor Green
}
if ($got -eq 0) { return }

# --- the table, worst first ----------------------------------------------
$rows = foreach ($f in $reports) { Get-RunSummary -Path $f.FullName }
$rows = $rows | Sort-Object Total

Write-Host ""
$fmt = "{0,-10} {1,10} {2,7} {3,7} {4,7} {5,5} {6,7} {7,6} {8,9} {9}"
Write-Host ($fmt -f "id", "total", "kills", "breach", "wasted", "civ", "aware", "comms", "detect", "losses by cause") -ForegroundColor Cyan
Write-Host ("-" * 100) -ForegroundColor DarkGray
foreach ($r in $rows) {
    $colour = if ($r.Crashes -gt 0 -or $r.AbiViolations -gt 0) { "Red" }
    elseif ($r.Total -lt 0) { "Yellow" }
    else { "Green" }
    Write-Host ($fmt -f `
            $r.Id,
        ("{0:N1}" -f $r.Total),
        ("{0}/{1}" -f $r.Kills, $r.HostilesTotal),
        $r.Breaches,
        $r.Wasted,
        $r.Civilians,
        ("{0:N1}" -f $r.Awareness),
        ("{0:N1}" -f $r.Comms),
        ("{0}/{1}" -f $r.Detected, $r.Compromises),
        $r.Causes) -ForegroundColor $colour
}

$totals = $rows | Measure-Object -Property Total -Average -Minimum -Maximum
Write-Host ("-" * 100) -ForegroundColor DarkGray
Write-Host ("worst {0:N1} ({1})   mean {2:N1}   best {3:N1}" -f `
        $totals.Minimum, $rows[0].Id, $totals.Average, $totals.Maximum)
Write-Host "The worst number is the one that gets graded. CHALLENGE.md 11.2." -ForegroundColor DarkGray

$anyFalse = ($rows | Measure-Object -Property FalseAccuse -Sum).Sum
if ($anyFalse -gt 0) {
    Write-Host ("FALSE ACCUSATIONS: {0} -- that term is NOT clamped, see 9.3" -f $anyFalse) -ForegroundColor Red
}
