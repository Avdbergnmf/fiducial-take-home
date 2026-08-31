# Did that change help? Two reports in, the deltas out.
#
#     powershell -ExecutionPolicy Bypass -File scripts\compare.ps1 runs\before.json runs\last.json
#     powershell -ExecutionPolicy Bypass -File scripts\compare.ps1 -Before runs\sweep_before -After runs\sweep
#
# Reach for this the moment a number moves and you are not sure the move is
# real. Pointed at two DIRECTORIES it pairs reports by scenario id, which is how
# you compare two sweeps rather than two single runs.
#
# Keep a baseline before you start editing:
#     Copy-Item runs\last.json runs\before.json

[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$Before,
    [Parameter(Position = 1)][string]$After
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

if (-not $Before -or -not $After) {
    throw "Usage: compare.ps1 <before.json|dir> <after.json|dir>"
}
if (-not (Test-Path $Before)) { throw "No such path: $Before" }
if (-not (Test-Path $After)) { throw "No such path: $After" }

function Get-Set {
    param([string]$Path)
    $map = @{}
    if ((Get-Item -LiteralPath $Path).PSIsContainer) {
        foreach ($f in Get-ChildItem -LiteralPath $Path -Filter *.json -File) {
            $s = Get-RunSummary -Path $f.FullName
            $map[$s.Id] = $s
        }
    }
    else {
        $s = Get-RunSummary -Path $Path
        $map[$s.Id] = $s
    }
    return $map
}

$a = Get-Set $Before
$b = Get-Set $After

function Show-Delta {
    param([string]$Label, [double]$Old, [double]$New, [bool]$HigherIsBetter = $true, [int]$Decimals = 1)
    $d = $New - $Old
    if ([math]::Abs($d) -lt 0.05) {
        $colour = "DarkGray"; $arrow = "  ="
    }
    elseif (($d -gt 0) -eq $HigherIsBetter) {
        $colour = "Green"; $arrow = "  +"
    }
    else {
        $colour = "Red"; $arrow = "  -"
    }
    $f = "{0,-14} {1,10:N$Decimals} -> {2,10:N$Decimals}  {3}{4:N$Decimals}"
    Write-Host ($f -f $Label, $Old, $New, $arrow, [math]::Abs($d)) -ForegroundColor $colour
}

$ids = ($a.Keys + $b.Keys | Sort-Object -Unique)
$totalOld = 0.0
$totalNew = 0.0
$common = 0

foreach ($id in $ids) {
    Write-Host ""
    if (-not $a.ContainsKey($id)) {
        Write-Host ("=== {0}: only in AFTER (total {1:N1}) ===" -f $id, $b[$id].Total) -ForegroundColor Yellow
        continue
    }
    if (-not $b.ContainsKey($id)) {
        Write-Host ("=== {0}: only in BEFORE (total {1:N1}) -- did a run fail? ===" -f $id, $a[$id].Total) -ForegroundColor Red
        continue
    }
    $x = $a[$id]; $y = $b[$id]
    $common++
    $totalOld += $x.Total
    $totalNew += $y.Total

    Write-Host ("=== {0} ===" -f $id) -ForegroundColor Cyan
    Show-Delta "total"      $x.Total     $y.Total
    Show-Delta "mission"    $x.Mission   $y.Mission
    Show-Delta "awareness"  $x.Awareness $y.Awareness
    Show-Delta "comms"      $x.Comms     $y.Comms
    Show-Delta "kills"      $x.Kills     $y.Kills     $true  0
    Show-Delta "breaches"   $x.Breaches  $y.Breaches  $false 0
    Show-Delta "wasted"     $x.Wasted    $y.Wasted    $false 0
    Show-Delta "civilians"  $x.Civilians $y.Civilians $false 0
    Show-Delta "wrong decl" $x.Wrong     $y.Wrong     $false 0
    Show-Delta "right decl" $x.Correct   $y.Correct   $true  0
    if ($x.Causes -ne $y.Causes) {
        Write-Host ("  causes  {0}  ->  {1}" -f $x.Causes, $y.Causes) -ForegroundColor Yellow
    }
}

if ($common -gt 1) {
    Write-Host ""
    Write-Host ("=== {0} scenarios in common ===" -f $common) -ForegroundColor Cyan
    Show-Delta "sum total" $totalOld $totalNew
    $worstOld = ($a.Values | Measure-Object -Property Total -Minimum).Minimum
    $worstNew = ($b.Values | Measure-Object -Property Total -Minimum).Minimum
    Show-Delta "worst run" $worstOld $worstNew
    Write-Host "The worst run is the one that matters. CHALLENGE.md 11.2." -ForegroundColor DarkGray
}
