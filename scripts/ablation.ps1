# Rebuild one RESULTS.md ladder version from git, without touching brain/src.
#
#     powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -List
#     powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7
#     powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Commit 2522c14
#     powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7 -Trace
#     powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -All
#     powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Current
#
# The version key and expected scores live in scripts\versions.csv. This
# archives brain/src at that commit into a scratch tree, compiles it against
# today's SDK, and sweeps the same 8 ids. Output is runs\ablation\<ver>\ —
# gitignored; do not copy into StreamingAssets.
#
# Commit WORKING (versions.csv V23, or -Current) copies the live working-tree
# brain/src instead of git archive, so uncommitted D71+ appear on the ladder.
#
# Plots: python tools\plot_ablation.py
# Iterate log (not this): scripts\history.ps1
#
# -Trace records the worst scenario (jsonl + viewer sidecar). -TraceAll does
# every id. Default is reports only, which is enough to check the numbers.
# -SkipExisting leaves a version alone if summary.csv is already there.
# -All continues after a version that fails to build or sweep.

[CmdletBinding()]
param(
    [string]$Version,
    [string]$Commit,
    [switch]$All,
    [switch]$Current,
    [switch]$List,
    [switch]$Trace,
    [switch]$TraceAll,
    [switch]$SkipExisting,
    [int]$Jobs = 4,
    [string]$Manifest,
    [string]$Config = "Release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
. "$PSScriptRoot\common.ps1"

if (-not $Manifest) { $Manifest = Join-Path $PSScriptRoot "versions.csv" }
$Manifest = ConvertTo-AbsolutePath $Manifest
if (-not (Test-Path -LiteralPath $Manifest)) {
    throw "No version table at $Manifest"
}

$AblationRoot = Join-Path $RepoRoot "runs\ablation"
$SdkInclude = Join-Path $RepoRoot "pkg\sdk\include"
$SweepIds = @("s0", "s1", "s2", "x1-a", "x1-b", "x1-c", "x2-a", "x2-b")
$ScoreTol = 0.15

function Normalize-VersionKey {
    param([string]$Raw)
    if ([string]::IsNullOrWhiteSpace($Raw)) { return $null }
    $k = $Raw.Trim().ToUpperInvariant()
    if ($k -match '^V?\d+$') {
        $n = $k.TrimStart('V')
        return "V$n"
    }
    return $k
}

function Read-VersionTable {
    @(Import-Csv -LiteralPath $Manifest)
}

function Resolve-Rows {
    $table = Read-VersionTable
    if ($Current) {
        $hit = @($table | Where-Object {
            $_.commit -eq "WORKING" -or $_.version -eq "V23"
        })
        if ($hit.Count -gt 0) { return , $hit[0] }
        return , [pscustomobject]@{
            version = "V23"
            commit  = "WORKING"
            label   = "live working tree"
            min     = ""
            mean    = ""
            max     = ""
            worst   = ""
        }
    }
    if ($All) { return $table }

    $key = Normalize-VersionKey $Version
    $wantCommit = $Commit

    if ($key) {
        $hit = @($table | Where-Object { $_.version -eq $key })
        if ($hit.Count -eq 0) {
            throw "No row for $key in $Manifest. Known: $((($table | ForEach-Object version) -join ', '))"
        }
        $row = $hit[0]
        if ($wantCommit) { $row.commit = $wantCommit }
        return , $row
    }

    if ($wantCommit -eq "WORKING" -or $wantCommit -eq ".") {
        $hit = @($table | Where-Object { $_.commit -eq "WORKING" })
        if ($hit.Count -gt 0) { return , $hit[0] }
        return , [pscustomobject]@{
            version = "V23"
            commit  = "WORKING"
            label   = "live working tree"
            min     = ""
            mean    = ""
            max     = ""
            worst   = ""
        }
    }

    if ($wantCommit) {
        $full = git -C $RepoRoot rev-parse --verify "$wantCommit^{commit}"
        if ($LASTEXITCODE -ne 0) { throw "git cannot resolve $wantCommit" }
        $short = git -C $RepoRoot rev-parse --short $full
        $hit = @($table | Where-Object {
            $c = git -C $RepoRoot rev-parse --verify "$($_.commit)^{commit}" 2>$null
            $c -and ($c -eq $full)
        })
        if ($hit.Count -gt 0) { return , $hit[0] }
        return , [pscustomobject]@{
            version = $short
            commit  = $short
            label   = "(not in versions.csv)"
            min     = ""
            mean    = ""
            max     = ""
            worst   = ""
        }
    }

    throw "Pass -Version V7, -Commit <hash>, -Current, -All, or -List."
}

function Write-Log {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Message,
        [string]$Color = "Gray"
    )
    $line = "[{0}] {1}" -f (Get-Date -Format "HH:mm:ss"), $Message
    Add-Content -LiteralPath $Path -Value $line
    Write-Host $Message -ForegroundColor $Color
}

function Show-Table {
    $table = Read-VersionTable
    Write-Host ""
    $fmt = "{0,-5} {1,-10} {2,8} {3,8} {4,8} {5,-8} {6}"
    Write-Host ($fmt -f "ver", "commit", "min", "mean", "max", "worst", "label") -ForegroundColor Cyan
    Write-Host ("-" * 100) -ForegroundColor DarkGray
    foreach ($r in $table) {
        Write-Host ($fmt -f $r.version, $r.commit, $r.min, $r.mean, $r.max, $r.worst, $r.label)
    }
    Write-Host ""
    Write-Host "Rebuild one:  powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7"
    Write-Host "Live tree:    powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Current"
    Write-Host "With viewer:  powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7 -Trace"
    Write-Host "Plots:        python tools\plot_ablation.py"
    Write-Host "Output:       runs\ablation\<ver>\  (gitignored)"
}

function Build-BrainAtCommit {
    param(
        [Parameter(Mandatory)][string]$Commit,
        [Parameter(Mandatory)][string]$WorkDir,
        [Parameter(Mandatory)][string]$LogPath
    )

    $liveTree = ($Commit -eq "WORKING" -or $Commit -eq ".")
    if (-not $liveTree) {
        $resolved = (git -C $RepoRoot rev-parse --verify "$Commit^{commit}").Trim()
        if ($LASTEXITCODE -ne 0 -or -not $resolved) {
            throw "git cannot resolve commit $Commit"
        }
        $short = (git -C $RepoRoot rev-parse --short $resolved).Trim()
        Write-Log $LogPath ("source: {0} ({1})" -f $short, $resolved) "DarkGray"
    }
    else {
        $short = "WORKING"
        $resolved = (git -C $RepoRoot rev-parse HEAD).Trim()
        Write-Log $LogPath ("source: working tree (HEAD {0})" -f $resolved) "DarkGray"
    }

    if (Test-Path -LiteralPath $WorkDir) {
        Remove-Item -LiteralPath $WorkDir -Recurse -Force
    }
    New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

    if ($liveTree) {
        $live = Join-Path $RepoRoot "brain\src"
        if (-not (Test-Path -LiteralPath $live)) {
            throw "no live brain\src at $live"
        }
        Copy-Item -Recurse $live (Join-Path $WorkDir "src")
    }
    else {
        $zip = Join-Path $WorkDir "src.zip"
        git -C $RepoRoot archive --format=zip $resolved brain/src -o $zip
        if ($LASTEXITCODE -ne 0) { throw "git archive $short brain/src failed" }
        Expand-Archive -Force -LiteralPath $zip -DestinationPath $WorkDir
        $src = Join-Path $WorkDir "brain\src"
        if (-not (Test-Path -LiteralPath $src)) {
            throw "git archive did not produce brain/src (commit $short)"
        }
        Copy-Item -Recurse $src (Join-Path $WorkDir "src")
    }

    $sdkCmake = $SdkInclude -replace '\\', '/'
    $cmake = @"
cmake_minimum_required(VERSION 3.16)
project(abl_brain CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
file(GLOB BRAIN_SOURCES "`${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(brain SHARED `${BRAIN_SOURCES})
target_include_directories(brain PRIVATE
  "$sdkCmake"
  `${CMAKE_CURRENT_SOURCE_DIR}/src)
set_target_properties(brain PROPERTIES PREFIX "" OUTPUT_NAME "brain")
if(MSVC)
  target_compile_options(brain PRIVATE /W4 /EHsc)
endif()
"@
    Set-Content -LiteralPath (Join-Path $WorkDir "CMakeLists.txt") -Value $cmake -Encoding utf8

    $build = Join-Path $WorkDir "build"
    Write-Log $LogPath "cmake configure" "DarkGray"
    & cmake -S $WorkDir -B $build | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed for $short" }

    Write-Log $LogPath ("cmake --build --config {0} --target brain" -f $Config) "DarkGray"
    & cmake --build $build --config $Config --target brain | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "cmake build failed for $short" }

    $lib = Find-BrainLibrary -BuildDir $build -Config $Config
    if (-not $lib) { throw "build succeeded but no brain.dll under $build" }
    Write-Log $LogPath ("brain: {0}" -f $lib) "Green"
    return $lib
}

function Write-SweepTable {
    param([object[]]$Rows, [string]$LogPath)
    $Rows = @($Rows | Sort-Object Total)
    $fmt = "{0,-10} {1,10} {2,7} {3,7} {4,7} {5,5} {6,7} {7,6} {8}"
    $header = $fmt -f "id", "total", "kills", "breach", "wasted", "civ", "aware", "comms", "losses"
    Write-Host ""
    Write-Host $header -ForegroundColor Cyan
    Write-Host ("-" * 100) -ForegroundColor DarkGray
    Add-Content -LiteralPath $LogPath -Value $header
    foreach ($r in $Rows) {
        $line = $fmt -f `
            $r.Id,
            ("{0:N1}" -f $r.Total),
            ("{0}/{1}" -f $r.Kills, $r.HostilesTotal),
            $r.Breaches,
            $r.Wasted,
            $r.Civilians,
            ("{0:N1}" -f $r.Awareness),
            ("{0:N1}" -f $r.Comms),
            $r.Causes
        $colour = if ($r.Crashes -gt 0 -or $r.AbiViolations -gt 0) { "Red" }
        elseif ($r.Total -lt 0) { "Yellow" } else { "Green" }
        Write-Host $line -ForegroundColor $colour
        Add-Content -LiteralPath $LogPath -Value $line
    }
}

function Save-SummaryCsv {
    param([object[]]$Rows, [string]$Path)
    $Rows | Select-Object Id, Total, RawTotal, CivPairCredit, Mission, Awareness, Comms, Kills, HostilesTotal,
        Breaches, Wasted, Civilians, Causes, Correct, Wrong, Outcome |
        Export-Csv -LiteralPath $Path -NoTypeInformation
}

function Invoke-ViewerSidecar {
    param([string]$Jsonl, [string]$Stem, [string]$LogPath)
    Write-Log $LogPath ("sidecar {0}" -f $Jsonl) "DarkGray"
    Push-Location $RepoRoot
    try {
        python (Join-Path $RepoRoot "tools\build_viewer_data.py") $Jsonl $Stem
        if ($LASTEXITCODE -ne 0) { throw "build_viewer_data.py failed for $Jsonl" }
    }
    finally { Pop-Location }
}

function Record-Scenario {
    param(
        [string]$Id,
        [string]$Brain,
        [string]$Stem,
        [string]$LogPath,
        [switch]$WithTrace
    )
    $report = "$Stem.json"
    $args = @(
        "--scenario", $Id,
        "--brain", (ConvertTo-AbsolutePath $Brain),
        "--report", (ConvertTo-AbsolutePath $report)
    )
    if ($WithTrace) {
        $trace = "$Stem.jsonl"
        $args += @("--trace", (ConvertTo-AbsolutePath $trace))
    }
    $code = Invoke-SimRaw -RepoRoot $RepoRoot -SimArgs $args
    if ($code -ne 0) { throw "swarm_sim $Id exited $code" }
    if ($WithTrace) {
        Invoke-ViewerSidecar -Jsonl "$Stem.jsonl" -Stem $Stem -LogPath $LogPath
    }
}

function Invoke-Version {
    param($Row)

    $ver = $Row.version
    $outDir = Join-Path $AblationRoot $ver
    $existingSummary = Join-Path $outDir "summary.csv"
    $existingMeta = Join-Path $outDir "meta.json"
    if ($SkipExisting -and (Test-Path -LiteralPath $existingSummary) -and
        (Test-Path -LiteralPath $existingMeta)) {
        Write-Host ("skip {0} (summary exists)" -f $ver) -ForegroundColor DarkGray
        $m = Get-Content -Raw -LiteralPath $existingMeta | ConvertFrom-Json
        return [pscustomobject]@{
            Version = $ver
            Commit  = $Row.commit
            Min     = $m.min
            Mean    = $m.mean
            Max     = $m.max
            Worst   = $m.worst
        }
    }

    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $log = Join-Path $outDir "log.txt"
    Set-Content -LiteralPath $log -Value ("# {0}  {1}  {2}" -f $ver, $Row.commit, $Row.label)

    Write-Host ""
    Write-Log $log ("=== {0}  {1}  {2} ===" -f $ver, $Row.commit, $Row.label) "Cyan"

    $work = Join-Path $AblationRoot ("_work\{0}" -f $ver)
    $brain = Build-BrainAtCommit -Commit $Row.commit -WorkDir $work -LogPath $log

    Get-ChildItem -LiteralPath $outDir -Filter *.json -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -ne "meta.json" } |
        Remove-Item -Force

    if ($TraceAll) {
        foreach ($id in $SweepIds) {
            Write-Log $log ("record {0}" -f $id) "DarkGray"
            Record-Scenario -Id $id -Brain $brain -Stem (Join-Path $outDir $id) `
                -LogPath $log -WithTrace
        }
    }
    else {
        Write-Log $log ("sweep {0}" -f ($SweepIds -join ",")) "DarkGray"
        $code = Invoke-SimRaw -RepoRoot $RepoRoot -SimArgs @(
            "--sweep",
            "--scenarios", ($SweepIds -join ","),
            "--brain", (ConvertTo-AbsolutePath $brain),
            "--report-dir", (ConvertTo-AbsolutePath $outDir),
            "--jobs", "$Jobs"
        )
        if ($code -ne 0) { throw "sweep for $ver exited $code" }
    }

    $reports = @(Get-ChildItem -LiteralPath $outDir -Filter *.json -File |
            Where-Object { $_.BaseName -in $SweepIds })
    if ($reports.Count -ne $SweepIds.Count) {
        throw ("{0}: expected {1} reports, got {2}" -f $ver, $SweepIds.Count, $reports.Count)
    }

    $rows = foreach ($f in $reports) { Get-RunSummary -Path $f.FullName }
    Write-SweepTable -Rows $rows -LogPath $log
    Save-SummaryCsv -Rows $rows -Path (Join-Path $outDir "summary.csv")

    $stats = $rows | Measure-Object -Property Total -Average -Minimum -Maximum
    $worstRow = @($rows | Sort-Object Total)[0]
    $gotMin = [math]::Round($stats.Minimum, 1)
    $gotMean = [math]::Round($stats.Average, 1)
    $gotMax = [math]::Round($stats.Maximum, 1)

    $footer = "worst {0:N1} ({1})   mean {2:N1}   best {3:N1}" -f `
        $gotMin, $worstRow.Id, $gotMean, $gotMax
    Write-Host ("-" * 100) -ForegroundColor DarkGray
    Write-Host $footer
    Add-Content -LiteralPath $log -Value $footer

    if ($Row.min) {
        $expMin = [double]$Row.min
        $expMean = [double]$Row.mean
        $expMax = [double]$Row.max
        $drift = [math]::Abs($gotMin - $expMin) -gt $ScoreTol -or
            [math]::Abs($gotMean - $expMean) -gt $ScoreTol -or
            [math]::Abs($gotMax - $expMax) -gt $ScoreTol
        if ($drift) {
            Write-Log $log ("DRIFT vs versions.csv  expected min/mean/max {0} / {1} / {2}" -f `
                    $Row.min, $Row.mean, $Row.max) "Yellow"
        }
        else {
            Write-Log $log ("match versions.csv  min {0}  mean {1}  max {2}" -f `
                    $Row.min, $Row.mean, $Row.max) "Green"
        }
        if ($Row.worst -and $worstRow.Id -ne $Row.worst) {
            Write-Log $log ("worst id {0}, table says {1}" -f $worstRow.Id, $Row.worst) "Yellow"
        }
    }

    $liveTree = ($Row.commit -eq "WORKING" -or $Row.commit -eq ".")
    $head = (git -C $RepoRoot rev-parse HEAD).Trim()
    $meta = [ordered]@{
        version        = $ver
        label          = $Row.label
        commit_request = $Row.commit
        commit         = if ($liveTree) { $head } else { (git -C $RepoRoot rev-parse --verify "$($Row.commit)^{commit}").Trim() }
        commit_short   = if ($liveTree) { "WORKING" } else { (git -C $RepoRoot rev-parse --short $Row.commit).Trim() }
        working_tree   = [bool]$liveTree
        when           = (Get-Date).ToString("s")
        min            = $gotMin
        mean           = $gotMean
        max            = $gotMax
        worst          = $worstRow.Id
        worst_total    = $gotMin
        expected_min   = $Row.min
        expected_mean  = $Row.mean
        expected_max   = $Row.max
        expected_worst = $Row.worst
        scenarios      = $SweepIds
    }
    $meta | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outDir "meta.json") -Encoding utf8

    if ($Trace -and -not $TraceAll) {
        $id = $worstRow.Id
        Write-Log $log ("trace worst {0}" -f $id) "DarkGray"
        Record-Scenario -Id $id -Brain $brain -Stem (Join-Path $outDir $id) `
            -LogPath $log -WithTrace
        $copy = Join-Path $outDir ("{0}_worst_{1}" -f $ver, $id)
        foreach ($ext in @(".json", ".jsonl", ".bin", ".meta.json")) {
            $src = "{0}{1}" -f (Join-Path $outDir $id), $ext
            if (Test-Path -LiteralPath $src) {
                Copy-Item $src ("{0}{1}" -f $copy, $ext) -Force
            }
        }
    }

    Write-Log $log ("wrote {0}" -f $outDir) "Green"
    return [pscustomobject]@{
        Version = $ver
        Commit  = $Row.commit
        Min     = $gotMin
        Mean    = $gotMean
        Max     = $gotMax
        Worst   = $worstRow.Id
    }
}

# --- entry ----------------------------------------------------------------
if ($List -or (-not $All -and -not $Current -and -not $Version -and -not $Commit)) {
    Show-Table
    if ($List -or (-not $Version -and -not $Commit -and -not $All -and -not $Current)) { return }
}

$targets = Resolve-Rows
$ladder = @()
foreach ($row in $targets) {
    try {
        $ladder += Invoke-Version $row
    }
    catch {
        Write-Host ("FAILED {0}: {1}" -f $row.version, $_) -ForegroundColor Red
        $failDir = Join-Path $AblationRoot $row.version
        New-Item -ItemType Directory -Force -Path $failDir | Out-Null
        Add-Content -LiteralPath (Join-Path $failDir "log.txt") -Value ("FAILED: {0}" -f $_)
        $ladder += [pscustomobject]@{
            Version = $row.version
            Commit  = $row.commit
            Min     = $null
            Mean    = $null
            Max     = $null
            Worst   = "FAILED"
        }
        if (-not $All) { throw }
    }
}

if ($ladder.Count -gt 1) {
    $ladderPath = Join-Path $AblationRoot "ladder.csv"
    $ladder | Export-Csv -LiteralPath $ladderPath -NoTypeInformation
    Write-Host ""
    Write-Host ("ladder: {0}" -f $ladderPath) -ForegroundColor Green
}
