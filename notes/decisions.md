# Decisions

Running log, newest at the bottom. DESIGN.md gets assembled from these at the
end — writing the reasoning down once, when it is fresh, beats reconstructing it
on the last evening.

One entry per decision that a reviewer could reasonably have made differently.
Not every choice needs an entry; the ones with a real cost do.

---

## D1 — Windows-native build as the primary path

**Options considered**

1. Docker container (linux/amd64) as the only supported path.
2. Native Windows with MSVC, container kept as a documented fallback.
3. WSL2 with the Linux binary.

**Chosen:** 2.

**Why:** the package ships `bin\swarm_sim.exe`, so the native path has the
fewest moving parts and no emulation layer between me and the simulator. Docker
Desktop may not even be installed, and installing it costs an hour I do not
have tonight.

**Cost accepted:** I need MSVC (or MinGW) on the machine, and the build is not
reproducible on a machine that lacks it. The container files stay in the repo so
that fallback is one command away, but they are no longer the tested path — if I
ever need them, expect to spend time revalidating them.

---

## D2 —

**Options considered**

**Chosen:**

**Why:**

**Cost accepted:**
