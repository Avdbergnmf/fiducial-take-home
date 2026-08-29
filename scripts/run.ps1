# Run a scenario with my brain, from inside pkg\ so scenarios\ resolves.
#
#     powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Scenario s1
#
# FLAG SPELLINGS: --scenario/--brain/--trace/--report are a best guess, written
# before the package existed. Check `.\bin\swarm_sim.exe --help` from inside
# pkg\ on the first run and correct the argument list at the bottom of this file.

[CmdletBinding()]
param(
    [string]$Scenario = "s1",
    [string]$Brain,
    [string]$Trace,
    [string]$Report,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

if (-not $Brain) {
    $Brain = Find-BrainLibrary -BuildDir (Join-Path $RepoRoot "brain\build") -Config $Config
    if (-not $Brain) {
        throw "No brain library found. Run scripts\build.ps1 first, or pass -Brain <path>."
    }
}

if (-not $Trace)  { $Trace  = Join-Path $RepoRoot "runs\last.jsonl" }
if (-not $Report) { $Report = Join-Path $RepoRoot "runs\last.json" }

# Everything the simulator sees must be absolute: its working directory is pkg\.
$Brain  = ConvertTo-AbsolutePath $Brain
$Trace  = ConvertTo-AbsolutePath $Trace
$Report = ConvertTo-AbsolutePath $Report

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Trace)  | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Report) | Out-Null

Invoke-Sim -RepoRoot $RepoRoot -SimArgs @(
    "--scenario", $Scenario,
    "--brain", $Brain,
    "--trace", $Trace,
    "--report", $Report
)

Write-Host ""
Write-Host "trace : $Trace" -ForegroundColor Green
Write-Host "report: $Report" -ForegroundColor Green
