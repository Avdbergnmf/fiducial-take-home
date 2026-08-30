# Swarm Defence Challenge

Onboard C++ brain for a swarm of interceptor drones, built as a shared library
against `pkg/sdk/include/swarm_abi.h`, plus a 3D viewer for the simulator's JSON
Lines recording and the design write-up.

    pkg/      the challenge package (simulator, sdk, scenarios, examples)
    brain/    my brain: brain/src/*.cpp -> brain/build/[Release/]brain.dll
    scripts/  PowerShell runners: build.ps1, run.ps1, example.ps1
    tools/    inspect_trace.py, build_viewer_data.py, plot_fixture.py,
              check_determinism.sh
    FORMAT.md contract for the two files the 3D viewer loads
    notes/    observed schema, decisions log, and the audit (provenance,
              code-map, fixture-findings)
    runs/     simulator output (fixture jsonl/json/meta.json committed; .bin not)
    viewer/   3D visualiser for the recording

**Read [SETUP.md](SETUP.md) first.** Design write-up in [DESIGN.md](DESIGN.md),
scores in [RESULTS.md](RESULTS.md).
