# Setup

## Prerequisites

- CMake + a C++ compiler (MSVC `cl` or `g++`). No compiler? Install **Visual
  Studio Build Tools** with the **"Desktop development with C++"** workload,
  then run everything from the **"x64 Native Tools Command Prompt for VS"**.
- Python 3, only for the viewer sidecar (not for building/running the brain):
      py -m pip install -r requirements.txt
- Unity **6000.5.10f1**, only to open the 3D viewer — see
  [viewer/fiducial-swarm-viz/README.md](viewer/fiducial-swarm-viz/README.md).

Run every script like this, so the execution policy never gets in the way:

    powershell -ExecutionPolicy Bypass -File scripts\<name>.ps1

## Quick start

`pkg\` (challenge package v0.6.0: `bin\swarm_sim.exe`, `sdk\`, `scenarios\`,
`examples\`) is already committed — nothing to unzip.

1. Build and run the packaged example brain, to prove the toolchain end to end:
       powershell -ExecutionPolicy Bypass -File scripts\example.ps1
   Produces `runs\fixture.jsonl` and `runs\fixture.json`.
2. Build and run my brain (`brain\src\*.cpp`) on a scenario:
       powershell -ExecutionPolicy Bypass -File scripts\build.ps1
       powershell -ExecutionPolicy Bypass -File scripts\run.ps1 -Scenario s1
3. Or do both in one step and push the result straight to the viewer:
       powershell -ExecutionPolicy Bypass -File scripts\iterate.ps1 -Scenario s1
4. Open `viewer\fiducial-swarm-viz` in Unity Hub and press Play — it loads the
   last-viewed run automatically (see the viewer's own README).

## Commands

| Command | What |
|---|---|
| `scripts\iterate.ps1 [-Scenario s1] [-Note "..."] [-Fast] [-NoBuild]` | the tight loop: build, run with `--trace`, convert + sync to the viewer, append a row to `runs\history.csv` |
| `scripts\build.ps1 [-Config Release] [-Clean]` | build `brain\` only |
| `scripts\run.ps1 -Scenario s1 [-Brain ...] [-Trace ...] [-Report ...]` | run my brain on one scenario |
| `scripts\example.ps1 [-Scenario s1]` | build + run the packaged example brain |
| `scripts\sweep.ps1 [-Scenarios s1,s2,...] [-Tier N -Count N] [-NoRun]` | run many scenarios, summarised worst-first |
| `scripts\compare.ps1 <before.json> <after.json>` | diff two reports (or two sweep directories) |
| `scripts\determinism.ps1 [-Scenarios s1,x1-a,x2-a]` | prove bit-identical runs (thread-order + repeat) |
| `scripts\history.ps1 [-Scenario s1] [-Last 25]` | read `runs\history.csv`, iterate.ps1's log |
| `scripts\ablation.ps1 -List` / `-Version V7` / `-Commit <sha>` `[-Trace]` / `-All` / `-Current` | rebuild one RESULTS.md ladder row from git, without touching `brain\src` |
| `scripts\sync_viewer_data.ps1 [-Stem fixture] [-Rebuild]` | copy a sidecar run into the Unity viewer's StreamingAssets |
| `tools\inspect_trace.py runs\fixture.jsonl` | dump record-type counts from a raw trace (stdlib only) |
| `tools\build_viewer_data.py runs\fixture.jsonl runs\fixture` | trace → `run.bin` + `run.meta.json` for the viewer |
| `tools\plot_fixture.py [stem]` | sanity-check plot of a viewer sidecar (ring/altitude/spawn-radius) |
| `tools\check_determinism.sh [brain.so] [id ...]` | bash/WSL equivalent of `determinism.ps1` |

## Gotchas

- **Run the simulator from inside `pkg\`** — it resolves `scenarios\` relative
  to the working directory. Every script above already does this.
- **Don't tail the trace file.** It's created empty when a run starts and
  written only at the end; an empty file mid-run means nothing is wrong.
- **`--dump-params` only works on the fixed scenario ids** (`s0`–`s5`), not
  generated `x<tier>-<token>` ids.
- **Scores are bit-identical across machines and thread counts** — a number
  recorded on one box is valid everywhere.

## WSL instead of native Windows

Both binaries ship in `pkg\bin\`. Build with cmake as usual and run
`bin/swarm_sim` (no `.exe`) — `tools\check_determinism.sh` expects that path.
Everything else (scenarios, scores, trace format) is identical.

## Replaying a RESULTS.md version

The ladder in RESULTS.md is `scripts\versions.csv`. To rebuild one commit's
brain and its 8-id sweep without checking out that commit:

    powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7

Output lands in `runs\ablation\V7\` (gitignored). See RESULTS.md "Reproducing
the ladder".
