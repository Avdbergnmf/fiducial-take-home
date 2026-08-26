# Swarm Defence Challenge

Onboard C++ brain for a swarm of interceptor drones, built as a shared library
against the challenge SDK, plus a 3D viewer for the simulator's JSON Lines
recording and the design write-up.

    pkg/      the challenge package (simulator, sdk, scenarios, examples)
    brain/    my brain: brain/src/brain.cpp -> brain/build/brain.so
    tools/    trace inspection
    runs/     simulator output (only runs/fixture.* is committed)
    viewer/   3D visualiser for the recording

**Read [SETUP.md](SETUP.md) first** — everything runs inside a linux/amd64
container, on both the Mac and the Windows box. Design write-up in
[DESIGN.md](DESIGN.md), scores in [RESULTS.md](RESULTS.md).
