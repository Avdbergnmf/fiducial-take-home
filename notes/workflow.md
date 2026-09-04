# How we tested (and what a slide should not invent)

Live brain is **V24** (D75 connect-first abort). **V23** is a frozen
sitting-wall snapshot, not the live tree. The 12 m ring-gap is rejected
(D71, superseded by D75). x1-a’s two civilians are **D44**, frozen.
Detection is 0: `declare_identity` is never called.

The paper trail is part of the method, not a write-up after the fact:
`notes/decisions.md` (why, numbered), `notes/provenance.md` (every
constant: measured / derived / guess), `scripts/versions.csv` +
`runs/ablation/` (the ladder), `RESULTS.md` (the numbers), `DESIGN.md`
(present tense). This file is the loop that produced them.

---

## The loop

One change, one question, then widen. Do not skip inward. A score that
cannot be replayed, or a knob that is not in provenance, is not a result.

1. **Unit tests.** `brain/build/Release/test_*.exe` (or `ctest`). Geometry
   and ownership pins — facing slot, catchable ram, `InterceptScore`,
   connect-first — live here so a sweep does not rediscover a broken
   inequality. Fail the pin, do not retune the eight.
2. **One id, with a picture.** `scripts/iterate.ps1` builds, runs
   `--trace` + `--report`, writes `runs/history.csv`, and (unless `-Fast`)
   converts the jsonl for the Unity viewer. Look at the number **and** the
   moment it was earned.
3. **The eight named.** `scripts/sweep.ps1` on
   `s0,s1,s2,x1-a,x1-b,x1-c,x2-a,x2-b`. CHALLENGE.md §11.2: read the
   **worst** total, not the mean. A mean that hides one breach is a lie.
4. **Hard generated ids.** Canary `x1-06b926af3deab4600494418d7ece5944`
   and fa56 `x2-fa56ef171718281fef54383d2dba36d6` are named because they
   broke a story the eight had already accepted. They are not in the
   ladder. Do not treat a fix that only moves the eight as done.
5. **Write it down in the same pass.** New D-number if the reason changed;
   provenance row if a constant moved; versions.csv row when the eight
   (or the floor) moved enough to be a ladder step. Sitting-wall stayed
   on the ladder as **V23 SNAPSHOT** so D75 could replace it without
   pretending the 12 m gap was never shipped.
6. **The git ladder.** `scripts/ablation.ps1` rebuilds each
   `scripts/versions.csv` commit (WORKING copies live `brain/src`; SNAPSHOT
   is on disk only). `python tools/plot_ablation.py` writes
   `notes/ablation-*.png`. This is how V8’s −179 floor showed up: s1 was
   fine; s2 was not.

`scripts/history.ps1` is the iterate CSV. It is not the ladder.

---

## Determinism

`scripts/determinism.ps1`: `--threads 1 --record` then `--threads 8
--replay`, then a second identical run with compute timings stripped
(CHALLENGE.md §9.3: wall-clock is not scored). A mismatch is usually a
clock read, unseeded RNG, uninitialised memory, or iterating a container
whose order depends on allocation. Run after anything that touches tick
order or surviving state. A pretty score that fails replay is not a
score.

---

## The viewer is a debugger

The Unity viewer is how a low score becomes a decision, not a knob.

Typical path: sweep finds the worst id → iterate with `--trace` → load
the sidecar → scrub to the event time in the report JSON (`breach`,
`intercept`, `friendly_lost`) → read that drone’s `commit` / `abort` /
`hand=` / `state ram` lines → watch who left a slot and who did not.

That is D11 (commit that never meets), D15 (two owners), D56 (s2 late
pair, ghost interceptor), D60/D67 (fa56 1.4 m pass), D71/D75 (canary
dual-ram). The overlay (kill envelope, leftover Reach, stations) is for
cover and orbit, not for classifying a craft.

What we look for, concretely:

- **Breach with nobody committed** — catchable bar, or ownership pointing
  at a receding facing drone.
- **Two friendlies on one inbound** — duplicate abort failed, or split
  ownership for a tick (hearsay vs local).
- **Wreckage (−40) after a kill** — a second ram on a body that was
  already dead. That was the canary; SittingWall papered it with a 12 m
  gap; D75 scores the ram.
- **Civilian at t≈1** — discriminant, not intercept. D44 is the known
  remainder on x1-a.

---

## What we refuse

- Fitting a constant to a generated token we will be graded on
  (`--dump-params` is refused on those ids).
- Reversing D44 to buy x1-a civilians.
- Growing R (D21). AND-ing the 0° horizon into `CoverCloses` (D58).
- Lowering `kChasingToward` to −5 (parks look like interceptors; s1/s2/fa56
  die).
- Treating a parked picket that *could* connect from rest as “I’ve got
  this” (handoff dies). D75’s parked rule needs a clearly-sooner hit.

---

## For whoever makes the slides

- Identity eight live: min **−7.7** (x1-a, 2 civ, 0 breach), mean
  **152.3**, max **237.6**. 0 breaches on this set. Mean is half-up of
  152.25; do not write **152.2** (banker's rounding) as a V24 regression.
- Per-id live: s0 96.6, s1 188.4, s2 180.3, x1-a −7.7, x1-b 237.6,
  x1-c 136.2, x2-a 201.1, x2-b 185.5. Awareness on x1-a is **42.2**.
- Canary 3/3 wasted 0 (+87.1); fa56 3/3 (+83.2). Not in the eight.
- Floor stopped being a breach bill at **V19** (hopped heartbeats, s2 6/6).
- Biggest floor jumps: V1 discriminant (+454), V7 catchable commit (+426),
  V13 radio ring (+207), V17 state machine (+165). V8 is the warning: a
  local-track drop that helped nothing and cost s2.
- V21 mean dip (orbit without enough handoff payoff on this eight) is
  real; do not flatten it.
- V23 sitting-wall and V24 connect-first are a **wash on min / mean / max**
  of the eight. Per-id cells shuffle by ≤ 0.4; that is the last column of
  `ablation-delta.png`, not a second win. D75 exists for the canary, and
  because the 12 m gap was not a reason.
- Do not cite SittingWall, 12 m closer, or “live V23” as current policy.
  Do not cite a git hash as V24 — it is `WORKING` (uncommitted tree).
  Do not cite README’s old `notes/brain-loop` names; this file replaced
  them.
- Hop-check s2 in RESULTS (**180.7**) is that five-id table vs V10, not
  the identity-eight cell (**180.3**).
- `notes/ablation-ids.png` / `ablation-delta.png` / `ablation-waterfall.png`
  are the attribution figures. Delta stack height is **Δmean**, not the
  sum of eight totals. `ablation-timeline.png` is the ladder on git
  commit time (V1–V14 is one day; V23/V24 diamonds are sweep times).
  `ablation-heatmap-x.png` is the generated x1/x2 rows on their own
  colour scale (s1’s −1500 flattens them on the eight-id heatmap).
  `orbit-cover-tradeoff.png` is a D70 plant at
  H=25; live picket is 20 m (D72).
