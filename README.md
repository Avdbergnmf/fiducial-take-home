# Swarm Defence Challenge

Onboard C++ brain for a swarm of interceptor drones, built as a shared library
against `pkg/sdk/include/swarm_abi.h`, plus a 3D viewer for the simulator's JSON
Lines recording and the design write-up.

    pkg/      the challenge package (simulator, sdk, scenarios, examples)
    brain/    my brain: brain/src/*.cpp -> brain/build/[Release/]brain.dll
    scripts/  PowerShell: build.ps1, run.ps1, example.ps1
    tools/    trace inspection
    notes/    schema.md (observed records), decisions.md (design log)
    runs/     simulator output (only runs/fixture.* is committed)
    viewer/   3D visualiser for the recording

**Read [SETUP.md](SETUP.md) first.** Design write-up in [DESIGN.md](DESIGN.md),
scores in [RESULTS.md](RESULTS.md).
