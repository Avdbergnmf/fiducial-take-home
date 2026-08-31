# Copy a sidecar run into the Unity StreamingAssets folder.
#
#     powershell -ExecutionPolicy Bypass -File scripts\sync_viewer_data.ps1
#     powershell -ExecutionPolicy Bypass -File scripts\sync_viewer_data.ps1 -Rebuild
#     powershell -ExecutionPolicy Bypass -File scripts\sync_viewer_data.ps1 -Stem last
#
# The viewer loads a COPY. Regenerating runs/<stem>.* without this step is how
# the bin length and the meta drift apart.

[CmdletBinding()]
param(
    [string]$Stem = "fixture",
    [switch]$Rebuild
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

$runsDir = Join-Path $RepoRoot "runs"
$destDir = Join-Path $RepoRoot "viewer\fiducial-swarm-viz\Assets\StreamingAssets"
$stemPath = Join-Path $runsDir $Stem

if ($Rebuild) {
    $jsonl = "$stemPath.jsonl"
    if (-not (Test-Path $jsonl)) {
        throw "No trace at $jsonl. Record it first."
    }
    Push-Location $RepoRoot
    try {
        python tools\build_viewer_data.py $jsonl $stemPath
        if ($LASTEXITCODE -ne 0) { throw "build_viewer_data.py failed" }
    }
    finally {
        Pop-Location
    }
}

$bin = "$stemPath.bin"
$meta = "$stemPath.meta.json"
if (-not (Test-Path $bin))  { throw "Missing $bin. Run the sidecar or pass -Rebuild." }
if (-not (Test-Path $meta)) { throw "Missing $meta. Run the sidecar or pass -Rebuild." }

New-Item -ItemType Directory -Force -Path $destDir | Out-Null
Copy-Item -Force $bin  (Join-Path $destDir "$Stem.bin")
Copy-Item -Force $meta (Join-Path $destDir "$Stem.meta.json")

$copiedBin = Get-Item (Join-Path $destDir "$Stem.bin")
Write-Host "synced $Stem -> $destDir" -ForegroundColor Green
Write-Host ("  {0}  {1} bytes" -f $copiedBin.Name, $copiedBin.Length)
Write-Host ("  {0}" -f "$Stem.meta.json")
