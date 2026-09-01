# Decisions

Running log, newest at the bottom. DESIGN.md gets assembled from these at the
end — writing the reasoning down once, when it is fresh, beats reconstructing it
on the last evening.

One entry per decision that a reviewer could reasonably have made differently.
Not every choice needs an entry; the ones with a real cost do.

---

## D0 — TEMPLATE

**Options considered**

**Chosen:**

**Why:**

**Cost accepted:**


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

## D2 — Hostile discriminant: sure-hit or shrink, not a 24 m gate

**Options considered**

1. Tighten `asset_radius · 0.8` (24 m) to a smaller fraction.
2. Require miss to be shrinking, with no sure-hit clause.
3. Sure-hit (`miss < 5 m`) **or** miss has dropped ≥ 3 m since first sight while still inside `asset_radius`.

**Chosen:** 3. Implemented as `AimedAtAsset` in `belief.cpp`; `Classify` latches `Track.miss_at_first`.

**Why:** The 24 m gate is what called civilians `enemy` before t=8 on s1 (entities 22 and 24). Alignment and closing look identical for a near-miss chord and an aimed dash; CPA miss is the discriminant, but a static radius that is 80% of the asset still accepts ~16% of civilian chords. Tightening the fraction (option 1) still calls any chord inside the new radius. Shrink-only (option 2) rejects s1 hostiles, who spawn already aimed at the origin so their miss never shrinks from zero. Option 3 is the comment already on `ClosestApproachDistance`.

**Cost accepted:** A civilian whose chord misses by less than 5 m still looks like a dash. First-sight CPA is noisier than `fix_sigma` because it extrapolates velocity, so a 3 m drop can fire on a constant chord when `asset_radius` is small (generated x2-a is 20.6 m). Sweep after the change: `pair_neutral` gone on 6/8 scenarios; remains on x1-a (1, miss 5.3 m) and x2-a (2, miss 6.2 m and 18 m). Policy commit and the 4 m separation margin were left alone.

---

## D3 — Diagnostic logs: transitions and boot params, not per-tick

**Options considered**

1. Leave `host->log` as the boot banner only. Debug civilian rams from geometry, `beliefs[]`, and events.
2. Log every tick (stance, target, accel, every track). Full flight recorder.
3. Log on *transitions* only: class change, commit/abort/picket, proximity band steps (12 / 6 / 3 m), plus one `params` line at boot with the `SwBootInfo` numbers the trace header does not carry.

**Chosen:** 3. `LogBeliefChanges` / `LogProximity` in `brain.cpp`; `Policy::last_log_` for commit/abort/picket; `Announce` writes `params sense=… comm=… sep=… ring=…`.

**Why:** The recording already has `beliefs[]` as declaration deltas and events for deaths. That is not enough to tell a false-positive *commit* from “saw it, unknown, 4 m margin lost” from “never in the track list.” Those three are different remaining-ram causes and they look identical in the kill event. Per-tick lines (option 2) would hit the finite log budget (`drone == -1` and the rest of the run goes silent — CHALLENGE.md §11.3) and drown the Logs window. Option 1 leaves the viewer unable to answer the question the overlay was built for. Transitions are the same shape as `beliefs[]` / `links[]`: hold the last value, emit a row when it changes. The `params` line is the honest channel for `sense_radius` / `comm_radius` / `separation_margin` / picket ring: they exist on `SwBootInfo` but are **not** in the trace header, and inventing 60 / 90 in the viewer is a lie on generated ids (FORMAT.md).

**Cost accepted:**

- **Score: none.** `log` is not a scoring term. Compute is measured and not scored. Comms scores broadcasts, not log text. `Decide` / `Fly` do not read `logged_belief`, `near_band`, or `last_log_`, so the command is the same bits as before this patch. No sweep: the command path was not touched.
- **Log budget: the real one.** Generous but finite. A late collision after overflow is exactly the line you needed and did not get. Volume is bounded — boot is 2 lines × fleet, class/stance changes are rare, proximity is at most three steps per track per close pass, not 100 Hz. Still the thing to watch on a long generated run; a `drone == -1` line means stop adding verbs, not add a 2 Hz heartbeat.
- **Per-tick work:** `LogProximity` still walks `tracks()` every 100 Hz tick to keep `near_band`, even when it writes nothing. Cheap next to ProNav, paid whether or not anyone is watching. `LogBeliefChanges` is a compare per track and only does CPA/align math on an actual class change.
- **Redundant `params`:** every drone writes the same `SwBootInfo` line. Waste of 16 identical rows so the viewer can parse the first one it sees; not worth a leader-election just to log constants.
- **Glue-file leak:** `brain.cpp`'s own header says tactics do not live there. The observers sit there anyway so belief/policy/flight stay free of `host->log`. A fifth `diag` module would be the tidy version; not justified at three call sites.
- **What this is not:** it does not fix remaining civilian rams, and it does not record commanded accel (the trace stride has no command column; kinematic Δv is a viewer-side stand-in).

---
