# Am I actually improving, or just fiddling?
#
#     powershell -ExecutionPolicy Bypass -File scripts\history.ps1
#     powershell -ExecutionPolicy Bypass -File scripts\history.ps1 -Scenario s1
#     powershell -ExecutionPolicy Bypass -File scripts\history.ps1 -Last 10
#
# Reads runs\history.csv, which iterate.ps1 appends to on every run. Reach for
# it when you have been tuning for an hour and want to know whether the last
# hour was worth it. Filtering by -Scenario is the honest comparison: totals
# from different scenarios are not comparable with each other.
#
# This is NOT the git version ladder. That is scripts\ablation.ps1 plus
# scripts\versions.csv (rebuild brain/src at a commit, sweep the same 8 ids).
# Plots of that ladder land in notes\ (tools\plot_ablation.py).

[CmdletBinding()]
param(
    [string]$Scenario = "",
    [int]$Last = 25
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

$histPath = Join-Path $RepoRoot "runs\history.csv"
if (-not (Test-Path $histPath)) {
    Write-Host "No runs\history.csv yet. It is written by scripts\iterate.ps1." -ForegroundColor Yellow
    return
}

$rows = @(Import-Csv -LiteralPath $histPath)
if ($Scenario) { $rows = @($rows | Where-Object { $_.scenario -eq $Scenario }) }
if ($rows.Count -eq 0) {
    Write-Host "No rows match." -ForegroundColor Yellow
    return
}

$shown = @($rows | Select-Object -Last $Last)

$fmt = "{0,-17} {1,-8} {2,10} {3,7} {4,7} {5,7} {6,5} {7,7} {8}"
Write-Host ($fmt -f "when", "scenario", "total", "delta", "kills", "breach", "civ", "wrong", "note") -ForegroundColor Cyan
Write-Host ("-" * 108) -ForegroundColor DarkGray

$prev = $null
foreach ($r in $shown) {
    $total = [double]$r.total
    # Only meaningful against the previous run of the SAME scenario.
    if ($null -ne $prev -and $prev.scenario -eq $r.scenario) {
        $d = $total - [double]$prev.total
        $deltaText = "{0:+0.0;-0.0;0}" -f $d
        $colour = if ($d -gt 0.05) { "Green" } elseif ($d -lt -0.05) { "Red" } else { "DarkGray" }
    }
    else {
        $deltaText = ""
        $colour = "Gray"
    }
    Write-Host ($fmt -f `
            $r.when, $r.scenario, ("{0:N1}" -f $total), $deltaText,
        $r.kills, $r.breaches, $r.civilians, $r.wrong, $r.note) -ForegroundColor $colour
    $prev = $r
}

Write-Host ("-" * 108) -ForegroundColor DarkGray
Write-Host ("{0} run(s) recorded, showing {1}" -f $rows.Count, $shown.Count)

# Best-so-far per scenario: the number to beat, and the one to quote in RESULTS.md.
Write-Host ""
Write-Host "best so far, per scenario:" -ForegroundColor Cyan
$rows | Group-Object scenario | ForEach-Object {
    $best = $_.Group | Sort-Object { [double]$_.total } -Descending | Select-Object -First 1
    Write-Host ("  {0,-8} {1,10:N1}   {2}" -f $_.Name, [double]$best.total, $best.note)
}
