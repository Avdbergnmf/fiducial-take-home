# Build brain/ into brain\build and report where the library actually landed.
#
#     powershell -ExecutionPolicy Bypass -File scripts\build.ps1

[CmdletBinding()]
param(
    [string]$Config = "Release",
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

$brainDir = Join-Path $RepoRoot "brain"
$srcDir   = Join-Path $brainDir "src"
$buildDir = Join-Path $brainDir "build"

$sources = @(Get-ChildItem -Path $srcDir -Filter *.cpp -File -ErrorAction SilentlyContinue)
if ($sources.Count -eq 0) {
    throw "No .cpp files in $srcDir. Paste the brain sources in there first."
}
Write-Host "sources: $($sources.Count) file(s) in brain\src" -ForegroundColor DarkGray

if ($Clean -and (Test-Path $buildDir)) {
    Remove-Item -Recurse -Force $buildDir
}

# -DCMAKE_BUILD_TYPE is for single-config generators, --config for MSVC.
# Passing both means this works whichever generator cmake picks.
cmake -S $brainDir -B $buildDir -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

cmake --build $buildDir --config $Config
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$lib = Find-BrainLibrary -BuildDir $buildDir -Config $Config
if (-not $lib) {
    Write-Warning "Build succeeded but no brain library was found under $buildDir."
    Write-Warning "Everything that got built:"
    Get-ChildItem -Path $buildDir -Recurse -File -Include *.dll, *.so -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Warning "    $($_.FullName)" }
    exit 1
}

Write-Host ""
Write-Host "brain library: $lib" -ForegroundColor Green
Write-Host "run it with:   powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Scenario s1"
