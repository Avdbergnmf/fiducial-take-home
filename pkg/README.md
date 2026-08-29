# Swarm Defence Challenge — candidate package

Version 0.6.0.

This archive is everything you need. It contains no simulator source.

## Contents

| Path | What |
|---|---|
| `CHALLENGE.md` | The brief. Start here. |
| `sdk/include/` | `swarm_abi.h` (required) and optional `swarm.hpp` |
| `examples/` | Starting brain and its CMake template |
| `bin/` | Headless simulator, both platforms: `swarm_sim.exe` and `swarm_sim` |
| `scenarios/` | Fixed scenarios `s0`–`s5` |

Platforms in this build: Windows (`bin/swarm_sim.exe`) and Linux x86_64 (`bin/swarm_sim`).

There is no GUI. The simulator writes a score report and an optional `--trace`
recording; building a 3D visualiser for that recording is a graded deliverable
(see CHALLENGE.md §11.3).

## Quick start

Run the simulator **from this directory**, so it finds `scenarios/`. (Elsewhere,
point it with `--scenario-dir`.)

Windows:

```bat
cmake -S examples -B examples\build
cmake --build examples\build --config Release
bin\swarm_sim.exe --scenario s0 --brain examples\build\Release\brain.dll
```

Linux:

```bash
cmake -S examples -B examples/build -DCMAKE_BUILD_TYPE=Release
cmake --build examples/build
bin/swarm_sim --scenario s0 --brain examples/build/brain.so
```

If your CMake generator puts the library somewhere other than the path above,
use wherever it landed.

The Linux binary was built on Ubuntu 24.04 (glibc 2.39). It needs a roughly
contemporary x86_64 Linux; older distributions may refuse to load it.

## Batch testing

Batch mode is built into the simulator:

```bash
bin/swarm_sim --sweep --brain <your brain> --scenarios s1,x1-a,x1-b --report-dir out/ --jobs 4
```

Read `CHALLENGE.md` before you write any code.
