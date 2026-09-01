# Constant provenance

Every tuning constant in `brain/src/`, with where its value actually came from.

Evidence used: `pkg/CHALLENGE.md`, `pkg/sdk/include/swarm_abi.h`, `pkg/scenarios/s1.json`,
`cd pkg && bin/swarm_sim --dump-params --scenario s1`, `runs/fixture.jsonl`,
`runs/fixture.json`, and one s1 run of this brain (`score.total = -1546.9`).

Categories: **SPEC** (stated in the brief/header) · **SCENARIO** (from a scenarios/*.json)
· **DERIVED** (arithmetic on a SPEC/SCENARIO value) · **MEASURED** (confirmed from
`--dump-params` or a report) · **GUESS** (no external justification).

Serialisation widths, enum values and struct sizes are omitted: they are self-justifying
(`Header::kBytes = 10` is checked by `test_protocol.cpp:38`).

---

## Summary

| Category | Count |
|---|---|
| SPEC / SCENARIO / MEASURED / DERIVED | 15 |
| **GUESS** | **31** |

The brain is mostly guesses. That is not automatically wrong — the brief refuses to publish
loss, latency and track-drop times on purpose — but two of the guesses are currently costing
380 points a run. See [Ranked guesses](#ranked-guesses).

---

## Config, resolved from SwBootInfo — `world.h`

| file:line | name | value | category | source | defensible? |
|---|---|---|---|---|---|
| world.h:114 | `sense_radius` default | 60.0 | MEASURED | `--dump-params`: `sense.radius=60` | Yes. Overwritten by `From()`; default matches s1. |
| world.h:115 | `comm_radius` default | 90.0 | MEASURED | `--dump-params`: `comm.radius=90` | Yes, same. |
| world.h:116 | `mtu` default | 256 | SPEC | `swarm_abi.h:62` `SW_MTU = 256` | Yes. |
| world.h:117 | `tx_budget_per_s` default | 4096 | MEASURED | `--dump-params`: `comm.tx_budget_bytes_per_s=4096` | Yes. |
| world.h:119 | `max_speed` default | 20.0 | MEASURED | `--dump-params`: `drone.max_speed=20` | Yes. |
| world.h:120 | `max_accel` default | 15.0 | MEASURED | `--dump-params`: `drone.max_accel=15` | Yes. |
| world.h:121 | `max_tilt` default | 0.6 | MEASURED | `--dump-params`: `drone.max_tilt=0.6` | Yes. |
| world.h:122 | `kill_radius` default | ~~3.0~~ → 1.0 | MEASURED | `--dump-params`: `drone.kill_radius=1`; `fixture.jsonl` header `"kill_radius": 1.0` | **Was wrong — fixed.** Every other default in this block is the s1 value; this one was 3× too large. Overwritten by `From()` so it never reached flight, but it is the number a reader checks the margin against. |
| world.h:125 | `asset_radius` | 30.0 | SCENARIO value, **MEASURED shape: cylinder** | `s1.json` `params.asset.radius = 30`; `--dump-params` has no height/shape. s1 breaches: last pose horiz 30.8 m, alt 6.75 m, 3D 31.5 m; event 0.06 s later (cylinder crossing), not 0.10 s (sphere). D10. | Radius yes. Treating it as a sphere is wrong. |
| world.h:132 | `lateral_limit` default | 6.7 | DERIVED | 9.81 · tan(0.6) = 6.7117 | Yes. |
| world.h:161 | `lateral_limit` computed | `9.81 · tan(max_tilt)` | SPEC | CHALLENGE.md §5.4: "bounded by `g·tan(max_tilt)` — around 6.7 m/s² where `max_accel` reads 15". Independently restated in `s1.json` `_evasion`: "g*tan(max_tilt) = 6.7 m/s^2 and not max_accel" | **Yes — the best-sourced constant in the brain.** Cited in two places and used correctly everywhere. |
| world.h:167 | `separation_margin` | `kill_radius · 4.0` = **4.0 m** | **GUESS** | Unknown/civilian blend only (D8). The `4.0` has no source. | Still too small to arrest traffic; left small *because* 19 m around every track collapses the ring. See below. |
| world.h:171 | `friendly_margin` | `14² / (2 · lateral) + 4 · kill` ≈ **18.6 m** on s1 | **DERIVED** | Arrest cruise (14 m/s, `brain.cpp`) with the lateral bound, plus four kill radii. D8. Neighbour chord on the 75 m / 16-drone ring is 29 m, so a picket is not inside a neighbour's bubble. | Yes, jointly with the ring. |

**`separation_margin` — why 4.0 m cannot work.** Arresting a closing speed `v` with the
lateral authority `a` = 6.7 m/s² needs `v²/(2a)` metres. An s1 civilian crosses at ~16 m/s
(`s1.json` `adv.enemy_dash_speed=16`, civilians "at a similar speed"), and a drone cruising
at 14 m/s (`brain.cpp:141`) can close on one at up to ~30 m/s:

| closing speed | distance needed to arrest it |
|---|---|
| 16 m/s | 256 / 13.4 = **19.1 m** |
| 30 m/s | 900 / 13.4 = **67.2 m** |

At 4 m the drone has 0.25 s to shed 16 m/s, which needs 64 m/s² against 6.7 available —
roughly **10× too late**. The margin is sized for drift, not for traffic.
*Too low:* collisions the avoidance term cannot prevent — observed, twice, in the first
8.4 s. *Too high:* the ring cannot hold station (neighbours 29 m apart on a 16-drone ring of
radius 75) and every drone permanently repels every other, so nothing holds formation.
A margin near 20 m and a ring radius of 75 m are not simultaneously satisfiable — which is
the real finding: **the margin and the ring geometry have to be designed together.**

---

## Classification — `belief.cpp`

| file:line | name | value | category | source | defensible? |
|---|---|---|---|---|---|
| belief.cpp:6 | `kDropAfter` | 3.0 s | **GUESS** | Sim's own value is `sense.track_drop_time=2.0` (`--dump-params`, `s1.json`) | Arguable, and inconsistent with the sim. We hold a track 1 s after the simulator retired it. The brief (§3) says the drop time is "not published" and varies — but on s1 it is measurable and it is 2.0. *Too high:* acting on tracks that no longer exist, and a reacquired entity arrives under a **new** `track_id` and becomes a duplicate. *Too low:* losing a track through a momentary dropout and re-learning its class from zero. |
| belief.cpp:7 | `kEvidenceForCall` | 1.2 | **GUESS** | none | Units are seconds-of-sustained-evidence (score integrates at `+dt`), so this is "1.2 s of continuous approach geometry". *Too high:* hostiles called late, less time to intercept. *Too low:* civilians called hostile — **currently happening**. |
| belief.cpp:8 | `kScoreDecay` | 0.6 /s | **GUESS** | none | Asymmetric with the +1.0/s accrual, so evidence builds ~1.7× faster than it decays. That bias is toward false positives. *Too high:* flickering beliefs. *Too low:* a stale hostile call never clears. |
| belief.cpp:29 | alignment speed deadband | 0.5 m/s | **GUESS** | none | Low-risk. Prevents a divide-by-noise on a hovering track. |
| belief.cpp:41 | closest-approach deadband | 0.25 (=0.5 m/s)² | **GUESS** | none | Low-risk, same reason. |
| belief.cpp:54 | `TimeToTarget` closing floor | 0.1 m/s | **GUESS** | none | Low-risk sentinel. |
| belief.cpp:62 | ballistic accel window | 6.0 .. 14.0 m/s² | **DERIVED**, now **MEASURED** | Brackets g = 9.81. Confirmed against real wreckage: `/tmp` run t=7.8→8.1, `vz` = −1.46, −0.47, 0.51, 1.49 → **9.8 m/s²** | Yes. Comfortably centred; ±40% tolerance absorbs the 10 Hz trace decimation and drag (`wreck.drag_coefficient=0.05`). |
| belief.cpp:64 | ballistic lateral limit | 4.0 m/s² | **GUESS** | none | Plausible: wreckage has no thrust, so lateral accel should be ~drag only. *Too high:* a powered drone in a dive reads as wreckage. *Too low:* tumbling wreckage in wind is missed. |
| belief.cpp:127 | ballistic accrual / clamp | `+dt·2.0`, clamp [0, 1.5] | **GUESS** | none | Reaches the 0.4 threshold in 0.2 s. Fast, but the test is specific. |
| belief.cpp:129 | ballistic decay clamp | [0, **3.0**] | **GUESS** | none | **Inconsistent with :127**, which clamps the same variable to 1.5. The 3.0 upper bound is unreachable. Harmless today, confusing to read. |
| belief.cpp:131 | wreckage threshold | 0.4 | **GUESS** | none | *Too high:* fly into debris. *Too low:* a descending friendly is called wreckage. |
| belief.cpp:9–10 | `kSureHit` / `kShrink` | 5 m / 3 m | **GUESS**, G1a then D7 | `fix_sigma=0.35`; 5 m is well above it; 3 m is meant to beat position noise | Miss is 3D (D7). Horizontal sure-hit was the leftover FP: a 5.2 m ground chord at 30 m altitude. 3D miss is ~altitude and the 2D shrink damps to ~1.5 m. |
| belief.cpp:11 | `kFriendlyGate` | 8.0 m | **GUESS** | Heartbeat claimed (extrapolated) position → nearest local sensor track (D8) | *Too high:* bind a heartbeat to the nearer of two close aircraft (widening to 14 m on s1 cost ~70 extra wrong declarations). *Too low:* a dash between beats never matches. Extrapolation by `now−sent_time` is what keeps 8 m viable. |
| belief.cpp:12 | `kFriendlyHold` | 2.5 s | **DERIVED** (weakly) | 2 Hz heartbeat (`policy.cpp`); covers a few losses | Five missed beats. *Too high:* a dead mate stays Friendly until ballistic wins. *Too low:* a radio dropout demotes a mate and a picket commits to it. |
| belief.cpp:83 | heartbeat range gate | `3σ + 2 m` | **DERIVED** | `f.range` / `f.range_sigma` are receiver measurements (`swarm_abi.h`) | The 2 m covers PosQ (0.125 m) and a few ticks of latency. A far-side replay misses by tens of metres. |
| belief.cpp:159 | alignment threshold | 0.8 | **GUESS** | none | cos⁻¹(0.8) = 37°. A civilian on a chord holds this easily. |
| belief.cpp:159 | closing threshold | 4.0 m/s | **GUESS** | none | Civilians cross at ~16 m/s, so this excludes almost nothing. |
| belief.cpp:160/162 | closing_score clamps | [−2, **3**] and [−2, **4**] | **GUESS** | none | **Inconsistent**: the accrual caps at 3.0 so the 4.0 decay bound is unreachable. Same class of sloppiness as :129. |
| belief.cpp:161 | alignment release | 0.3 | **GUESS** | none | Hysteresis band 0.3–0.8. Reasonable shape, unjustified values. |
| belief.cpp:181 | belief-clear threshold | 0.2 | **GUESS** | none | Low-risk. |
| belief.cpp:193 | `kGate` peer association | 12.0 m | **GUESS** | Comment says to size it from `fix_sigma` + latency; `--dump-params` gives `sense.fix_sigma=0.35`, `sense.fix_bias_sigma=1.2`, `comm.latency_ticks=2`, `jitter 1` | Too loose by its own argument: two fixes at σ=0.35 plus 1.2 m bias plus 3 ticks × 16 m/s ≈ 0.5 m of staleness is ~3 m, not 12. *Too high:* two distinct aircraft merge into one track. *Too low:* the same aircraft becomes two tracks and peer reports never fuse. |
| belief.cpp:215 | confidence scale | `/255.0` | **DERIVED** | `TrackReportMsg::confidence` is `uint8_t` | Yes. |
| belief.cpp:234 | tie-break weight | `d · 0.001` | **GUESS** | none | 1000 m of range = 1 s of time-to-go. Effectively a tiebreak only. See code-map: the comment claims more than the code does. |

---

## Policy — `policy.cpp`

| file:line | name | value | category | source | defensible? |
|---|---|---|---|---|---|
| policy.cpp:24 | `PublishedClass` | Friendly + Hostile | **DERIVED** | D9. Local only. Wreckage/unknown → UNKNOWN. ENEMY restored: the viewer has no other channel. | x1-b still pays −2 on AimedAtAsset FPs. |
| policy.cpp:9 | `kCommitMaxRange` | 120.0 m | **GUESS** | none | Exceeds `sense_radius` (60) — so it only ever binds on hearsay tracks, never on our own sensor tracks. Effectively dead code on s1. |
| policy.cpp:10 | `kAbortAfter` | 25.0 s | **GUESS** | none | Hostiles spawn every 14 s (`s1.json`), so a 25 s pursuit spans two arrivals. *Too high:* a drone is committed to a lost cause while the next hostile transits unopposed. *Too low:* aborting a converging intercept. |
| policy.cpp:11 | `kClaimHold` | 6.0 s | **GUESS** | none | Nothing reads claims yet (`brain.cpp:105` is a TODO), so this is inert. |
| policy.cpp:12 | `kReportEvery` | 0.5 s | **GUESS** | none | Cheap. |
| policy.cpp:14-17 | priorities | 1/3/4/5 | **GUESS** | none | Ordinal only; the ordering is the design, the values are arbitrary. Fine. |
| policy.cpp:30 | `ring_radius_` | `asset_radius + comm_radius·0.5` = **75 m** | **DERIVED** (weakly) | `s1.json` `_fleet`: "the ring is what the radio and the sensors say it is" | Half-justified. At r=75 with 16 drones the neighbour chord is 2·75·sin(π/16) = **29.3 m**, inside both `sense_radius` 60 and `comm_radius` 90 — so the *intent* is met. But the `0.5` is a guess, and 75 m puts the ring well inside the 150 m civilian spawn ring, i.e. in the traffic. |
| policy.cpp:31 | `ring_altitude_` | 30.0 m | **SCENARIO** | `s1.json` `spawn.friendly_altitude = 30.0` | Yes — matches spawn altitude, so no climb is needed. Note civilians fly at 50 m and hostiles at 40 m (`s1.json`), so the ring sits *below* both. |
| policy.cpp:62 | slot-reached radius | 8.0 m | **GUESS** | none | Low-risk. |
| policy.cpp:77 | commit evidence bar | `closing_score < 1.5` | **GUESS** | none | Higher than `kEvidenceForCall` (1.2), so committing is stricter than declaring. Good instinct, unjustified value. |
| policy.cpp:86 | commit geometry gate | `closing > −2.0 \|\| ttg < 12.0` | **GUESS** | none | An `\|\|` — so a target with `ttg < 12` is committed to **even in a stern chase**, which flight.h:31 says never converges. *Too high:* chasing the uncatchable. |
| policy.cpp:95/97 | non-convergence abort | after 6.0 s, `closing < 1.0` | **GUESS** | none | Reasonable shape. |
| policy.cpp:177 | confidence encode | `>2.0 ? 255 : score·120` | **GUESS** | none | `score·120` saturates the `uint8_t` at score 2.125, and the branch caps at 2.0, so the mapping is continuous by luck rather than by construction. |
| policy.cpp:205 | outbox max age | 2.0 s | **GUESS** | none | Sensible. |
| policy.cpp:217 | tx headroom | `len + 64` | **GUESS** | none | Reserves 64 B. Never binds on s1: measured usage is 704 B/s against a 4096 B/s budget, and `frames_dropped_budget = 0` in both reports. |

---

## Flight — `flight.cpp` / `flight.h`

| file:line | name | value | category | source | defensible? |
|---|---|---|---|---|---|
| flight.h:23 | `GoTo` gains | `pos 0.8`, `vel 1.6` | **GUESS** | none | Damping ratio ζ = 1.6/(2·√0.8) = 0.89 — near-critically damped, which is the right shape. The values are unjustified but the *ratio* is defensible. |
| flight.h:38 | `navigation_gain` | 3.5 | **DERIVED** by convention | Classical proportional navigation uses N = 3–5 | Yes, standard. Worth saying so in DESIGN.md. |
| flight.cpp:38 | stop gain | −1.6 | **GUESS** | none | Low-risk. |
| flight.cpp:42 | `stopping` | `√(2·lateral_limit·range)` | **DERIVED** | Kinematics, using the correct limit | Yes. Uses `lateral_limit`, not `max_accel` — the same distinction the brief flags. |
| flight.cpp:47 | cruise velocity gain | 2.0 | **GUESS** | none | Low-risk. |
| flight.cpp:69 | closing floor / boost | `12.0 m/s`, `lateral·0.6` | **GUESS** | none | Adds line-of-sight thrust when closing < 12 m/s. *Too high:* a permanent pursuit bias that spoils the ProNav geometry. |
| flight.cpp:95/97 | avoidance strength | `closing·0.15`, `·lateral·2.0` (unknown) / `·2.5` (mate) | **GUESS** | none | Unknown traffic is still a blend (D8). Mates cancel the closing component of the command first; the `·2.5` is extra repulsion, not the guarantee — the cancel is. |
| flight.cpp:98 | mate panic radius | `3 · kill_radius` = 3 m | **GUESS** | D8 hard override | Inside this, intercept is abandoned and we accelerate away. *Too high:* abandon a real intercept because a mate is nearby. *Too low:* still closing at 1 m. |
| flight.cpp:111 | `kEdge` | 20.0 m | **GUESS** | none | Arena is ±200 m (`fixture.jsonl` header `arena.min/max`), so this is a 10% border. Nothing left the arena in either run. |
| flight.cpp:115-116 | arena gains | 0.5, 0.8 | **GUESS** | none | Low-risk. |
| brain.cpp:140 | cruise/goto switch | 25.0 m | **GUESS** | none | Low-risk. |
| brain.cpp:141 | cruise speed | 14.0 m/s | **GUESS** | none | Below `max_speed` 20. Contributes to closing speed against traffic — see `separation_margin`. |
| brain.cpp:154 | yaw deadband | 1.0 (m/s)² | **GUESS** | none | Cosmetic; yaw is unscored. |

---

## Protocol — `protocol.h`

| file:line | name | value | category | source | defensible? |
|---|---|---|---|---|---|
| protocol.h:14 | `kProtocolVersion` | 1 | **SPEC**-aligned | Brief §12 asks for a versioned protocol | Yes — checked at :164 and covered by `TestWrongVersion`. |
| protocol.h:60-67 | `PosQ` quantisation | step **0.125 m**, range ±4095 m | **DERIVED** | `·8.0` → 1/8 m. Range covers the ±200 m arena 20× over | Step is right; the range is 20× more than the arena needs, which is 2 wasted bits per axis. Comment overstated "far below `fix_sigma`" — measured `fix_sigma = 0.35`, so per-axis truncation error (up to 0.125 m) is ~⅓ of it. **Comment corrected.** |
| protocol.h:142 | `Header::kBytes` | 10 | **DERIVED** | 1+1+1+1+2+4 | Yes, and asserted by `test_protocol.cpp:38`. |
| brain.cpp:162 | `Outbox<24>` | 24 | **GUESS** | none | At 1 send/tick and 100 Hz, 24 frames drains in 0.24 s. Never filled on s1. |
| brain.cpp:163 | `SeenSet<256>` | 256 | **GUESS** | none | 16 drones × 2 Hz heartbeat = 32 frames/s, so 256 slots ≈ 8 s of history. Reasonable; `Seen()` is O(256) linear per frame, which is why `mean_tick_us` is 0.3 (budget 2000) — not a problem. |
| world.h:228 | `kMaxFleet` | 64 | **GUESS** | `fleet_size` is 16 on s1; `SW_MAX_TRACKS` is 64 | Safe over-allocation. `NotePeer` bounds-checks against it (`brain.cpp:123`). |

---

## Ranked guesses

Tune in this order. Guess #1 (24 m miss gate) is **addressed** — `AimedAtAsset`
plus 3D miss (D2, D7). Horizontal leftover chords were the D7 case.

1. ~~**`belief.cpp:157` `asset_radius · 0.8` (24 m miss gate)**~~ **Done (D2, D7).**
   Replaced by `AimedAtAsset`. Horizontal miss ≲ 5 m at altitude is D7 (3D miss).
2. **`world.h:167` `kill_radius · 4.0` (4 m unknown-traffic margin)** — still ~5× too
   small for civilian closing speeds. D8 added a kinematic `friendly_margin` (~19 m)
   for *identified mates* only; raising the unknown margin to 19 m without growing
   the 75 m ring is the option D8 refuses. Remaining ram risk is ID failure and
   civilian chords.
3. **`belief.cpp:6` `kDropAfter` 3.0 s** — contradicts the sim's measured
   `sense.track_drop_time = 2.0`. Cheap to align; ask whether you *want* to outlive it.
4. **`belief.cpp:193` `kGate` 12 m** — too loose by its own stated reasoning (~3 m from
   measured sigmas). Matters more from s2 on, when peer reports carry the load.
5. **`policy.cpp:86`** — the `||` admits stern chases the guidance cannot fly.
