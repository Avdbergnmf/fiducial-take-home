# Build the PACKAGED example brain and run it to produce tonight's fixture
# recording. This is the first thing to get working: it proves the toolchain,
# the simulator and the trace format all work before any of my code exists.
#
#     powershell -ExecutionPolicy Bypass -File scripts\example.ps1
#
# Flags verified against `bin\swarm_sim.exe --help` (v0.6.0).

[CmdletBinding()]
param(
    [string]$Scenario = "s1",
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

$examplesDir = Join-Path $RepoRoot "pkg\examples"
if (-not (Test-Path $examplesDir)) {
    throw "No pkg\examples. Unzip the challenge package into pkg\ first."
}

$buildDir = Join-Path $examplesDir "build"

cmake -S $examplesDir -B $buildDir -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for pkg\examples" }

cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "cmake build failed for pkg\examples" }

$lib = Find-BrainLibrary -BuildDir $buildDir -Config $Config
if (-not $lib) {
    throw "Built pkg\examples but found no shared library under $buildDir. Look in there and pass the path to run.ps1 -Brain."
}
Write-Host "example brain: $lib" -ForegroundColor Green

$trace  = Join-Path $RepoRoot "runs\fixture.jsonl"
$report = Join-Path $RepoRoot "runs\fixture.json"
New-Item -ItemType Directory -Force -Path (Join-Path $RepoRoot "runs") | Out-Null

Invoke-Sim -RepoRoot $RepoRoot -SimArgs @(
    "--scenario", $Scenario,
    "--brain", $lib,
    "--trace", $trace,
    "--report", $report
)

Write-Host ""
Write-Host "fixture trace : $trace" -ForegroundColor Green
Write-Host "fixture report: $report" -ForegroundColor Green
Write-Host "next: python tools\inspect_trace.py runs\fixture.jsonl --write"
