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

    $code = Invoke-SimRaw -RepoRoot $RepoRoot -SimArgs $SimArgs
    if ($code -ne 0) { throw "swarm_sim exited with code $code" }
}

function Invoke-SimRaw {
    # Same as Invoke-Sim but RETURNS the exit code instead of throwing, for the
    # callers that treat a non-zero exit as data: --replay reports a determinism
    # mismatch that way, and a sweep's exit status reflects failed runs.
    # Exit codes (bin\swarm_sim.exe --help): 0 completed, 1 simulator error,
    # 2 brain failed to load, 3 brain crashed, 4 ABI violation,
    # 5 determinism self-check failed.
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string[]]$SimArgs,
        [switch]$Quiet
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
        if (-not $Quiet) {
            Write-Host "> $exe $($SimArgs -join ' ')" -ForegroundColor DarkGray
        }
        # Out-Host, NOT a bare call: a native command's stdout goes to the
        # pipeline, so a bare `& $exe` would make this function return the
        # simulator's console output AND the exit code as one array, and every
        # `-ne 0` check downstream would be comparing against an array.
        # Out-Host prints it live and returns nothing, so $code is the only
        # thing this function emits.
        & $exe @SimArgs | Out-Host
        $code = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
    return $code
}

function Expand-IdList {
    # Normalise a scenario list into a flat string[].
    #
    # Necessary because `powershell -File script.ps1 -Scenarios s1,s2` passes
    # ONE string "s1,s2", while `& .\script.ps1 -Scenarios s1,s2` passes two
    # elements. Every script here documents the -File form, so both must work or
    # the count checks lie.
    param([string[]]$Ids)
    $out = @()
    foreach ($item in $Ids) {
        if ($null -eq $item) { continue }
        foreach ($part in ($item -split ",")) {
            $trimmed = $part.Trim()
            if ($trimmed) { $out += $trimmed }
        }
    }
    return , $out
}

function Get-RunSummary {
    # One report JSON -> one flat object. Every consumer (sweep, compare,
    # history, iterate) reads reports through here, so the field paths are
    # written down once. Paths verified against a real report; see
    # notes/fixture-findings.md section c.
    param([Parameter(Mandatory)][string]$Path)

    $r = Get-Content -Raw -LiteralPath $Path | ConvertFrom-Json
    $m = $r.mission
    $a = $r.awareness

    # losses_by_cause has scenario-dependent keys (pair_friendly, pair_neutral,
    # pair_hostile, wreckage, ground, arena), so it is read reflectively.
    $causeParts = @()
    if ($m.losses_by_cause) {
        foreach ($p in $m.losses_by_cause.PSObject.Properties) {
            $causeParts += ("{0}={1}" -f $p.Name, $p.Value)
        }
    }

    [pscustomobject]@{
        Id            = $r.scenario
        Outcome       = $r.outcome
        Total         = [double]$r.score.total
        Mission       = [double]$r.score.mission
        Awareness     = [double]$r.score.awareness
        Comms         = [double]$r.score.comms
        Kills         = [int]$m.hostiles_destroyed
        HostilesTotal = [int]$m.hostiles_total
        Breaches      = [int]$m.hostiles_reached_asset
        Wasted        = [int]$m.friendlies_lost_wasted
        Civilians     = [int]$m.civilians_lost
        Causes        = ($causeParts -join " ")
        Compromises   = [int]$a.compromises
        Detected      = [int]$a.compromises_detected
        FalseAccuse   = [int]$a.false_accusations
        Correct       = [int]$a.correct_declarations
        Wrong         = [int]$a.wrong_declarations
        Crashes       = [int]$r.integrity.brain_crashes
        AbiViolations = @($r.integrity.abi_violations).Count
        AssetSurvival = $m.asset_survival_time_s
        Path          = (Resolve-Path -LiteralPath $Path).Path
    }
}
