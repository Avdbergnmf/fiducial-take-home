# Shared helpers. Dot-source from a script in this directory:
#     . "$PSScriptRoot\common.ps1"

function ConvertTo-AbsolutePath {
    # Resolve against the caller's current directory, NOT pkg\, because the
    # simulator is invoked with pkg\ as its working directory.
    param([Parameter(Mandatory)][string]$Path)
    if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }
    return [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $Path))
}

function Find-BrainLibrary {
    # MSVC is multi-config, so the library lands in <build>\<Config>\.
    # Ninja and MinGW put it directly in <build>\. Probe both.
    param(
        [Parameter(Mandatory)][string]$BuildDir,
        [string]$Config = "Release",
        [string[]]$PreferredNames = @("brain.dll", "brain.so")
    )
    if (-not (Test-Path $BuildDir)) { return $null }

    foreach ($dir in @((Join-Path $BuildDir $Config), $BuildDir)) {
        foreach ($name in $PreferredNames) {
            $candidate = Join-Path $dir $name
            if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
        }
    }

    # Fall back to any single shared library under the build tree: the packaged
    # example may not name its output "brain".
    $libs = @(Get-ChildItem -Path $BuildDir -Recurse -File -Include *.dll, *.so -ErrorAction SilentlyContinue)
    if ($libs.Count -eq 1) { return $libs[0].FullName }
    if ($libs.Count -gt 1) {
        Write-Warning "Several shared libraries under ${BuildDir}; pass -Brain explicitly:"
        foreach ($lib in $libs) { Write-Warning "    $($lib.FullName)" }
    }
    return $null
}

function Invoke-Sim {
    # THE trap this whole script exists for: the simulator resolves scenarios\
    # against the working directory, so it is always run from inside pkg\.
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string[]]$SimArgs
    )
    $pkg = Join-Path $RepoRoot "pkg"
    if (-not (Test-Path $pkg)) {
        throw "No pkg\ directory. Unzip the challenge package into $pkg first."
    }

    $exe = Join-Path $pkg "bin\swarm_sim.exe"
    if (-not (Test-Path $exe)) { $exe = Join-Path $pkg "bin\swarm_sim" }
    if (-not (Test-Path $exe)) {
        throw "No simulator at pkg\bin\swarm_sim.exe or pkg\bin\swarm_sim."
    }

    Push-Location $pkg
    try {
        Write-Host "> $exe $($SimArgs -join ' ')" -ForegroundColor DarkGray
        & $exe @SimArgs
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    if ($code -ne 0) { throw "swarm_sim exited with code $code" }
}
