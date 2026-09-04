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

The brain is still mostly guesses. That is not automatically wrong — the brief
refuses to publish loss, latency and track-drop times on purpose. The ranked
list below is the ones that were costing runs; D2/D7/D11/D12/D13/D14 took
the first five.

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
| world.h:167 | `separation_margin` | `kill_radius · 4.0` = **4.0 m** | **DERIVED** as a *floor* | Non-closing unknown/civilian drift (D12). Arrest is `v_close²/(2a)+4·kill` when closing. | Yes as a floor. A static 19 m still collapses the ring (D8). |
| world.h:171 | `friendly_margin` | `14² / (2 · lateral) + 4 · kill` ≈ **18.6 m** on s1 | **DERIVED** | Arrest cruise (14 m/s, `brain.cpp`) with the lateral bound, plus four kill radii. D8. Neighbour chord on the 75 m / 16-drone ring is 29 m, so a picket is not inside a neighbour's bubble. | Yes, jointly with the ring. |

**`separation_margin` — 4.0 m is the floor, not the arrest (D12).** Arresting a
closing speed `v` with lateral authority `a` = 6.7 m/s² needs `v²/(2a)` metres.
That distance is now computed per pair when `closing > 0`, for mates *and*
unknown traffic. The 4 m constant is what we keep around a track that is not
closing, so a 16-drone ring of radius 75 (neighbours 29 m) still fits. A static
19 m around every unknown remains the option D8 refuses.

| closing speed | distance needed to arrest it |
|---|---|
| 16 m/s | 256 / 13.4 = **19.1 m** |
| 30 m/s | 900 / 13.4 = **67.2 m** |

---

## Classification — `belief.cpp`

| file:line | name | value | category | source | defensible? |
|---|---|---|---|---|---|
| belief.cpp:6 | `kHearsayDrop` | 2.0 s | **DERIVED** (weakly) | Local tracks drop when absent from `obs.tracks()` (D13). Hearsay only: four missed 0.5 s reports (`kReportEvery`). | Not the sim's unpublished `track_drop_time`. |
| belief.cpp:8 | `kEvidenceForCall` | 0.6 | **MEASURED**, G3 | Seconds of aimed geometry. Swept 0.1–0.7 against the fixed 8 plus ae01dd / 814 / b403 (D36). Plateau is 0.58–0.60. Was 1.2, then 0.6 as a guess (D11). | *Too low (≤0.55):* b403 re-leaks hostile_1 at 37.11 (D35 skip). ≤0.5 also drops x1-c's third. *Too high (≥0.65):* 814 misses hostile_1 (t_free 5.74). Civilian FPs did not move on these 11. |
| belief.cpp:8 | `kScoreDecay` | 0.6 /s | **GUESS** | none | Asymmetric with the +1.0/s accrual, so evidence builds ~1.7× faster than it decays. That bias is toward false positives. *Too high:* flickering beliefs. *Too low:* a stale hostile call never clears. |
| belief.cpp:29 | alignment speed deadband | 0.5 m/s | **GUESS** | none | Low-risk. Prevents a divide-by-noise on a hovering track. |
| belief.cpp:41 | closest-approach deadband | 0.25 (=0.5 m/s)² | **GUESS** | none | Low-risk, same reason. |
| belief.cpp:54 | `TimeToTarget` closing floor | 0.1 m/s | **GUESS** | none | Low-risk sentinel. |
| belief.cpp:67 | `TimeToCylinder` | `(ground − radius) / closing` | **DERIVED** | D10: breach is the cylinder, not 3D range to the origin. | Yes — this is the clock `P_breach` actually uses. |
| belief.cpp:79 | `ClosingSpeed` | relative, horizontal | **DERIVED** | ProNav's relative velocity, flattened like `RangeRate`. D11. | Yes. `RangeRate(them, them_vel, us)` was the wrong quantity for commit. |
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
| belief.cpp | `kGate` peer association | 8.0 m | **DERIVED**, then **MEASURED** | After extrapolating by `now−sent_time` (D14). 2·bias 1.2 + 3·√2·σ 0.35 + 0.2 s·16 m/s ≈ 7 m. | 4 m duplicated tracks (s1 comms 26, 2089 calls). 12 m merged distinct aircraft. 8 m holds s1 and gained a kill on s2. |
| belief.cpp:215 | confidence scale | `/255.0` | **DERIVED** | `TrackReportMsg::confidence` is `uint8_t` | Yes. |
| belief.cpp:234 | tie-break weight | `d · 0.001` | **GUESS** | none | 1000 m of range = 1 s of time-to-go. Effectively a tiebreak only. See code-map: the comment claims more than the code does. |

---

## Policy — `policy.cpp`

| file:line | name | value | category | source | defensible? |
|---|---|---|---|---|---|
| policy.cpp:24 | `PublishedClass` | Friendly + Hostile | **DERIVED** | D9. Local only. Wreckage/unknown → UNKNOWN. ENEMY restored: the viewer has no other channel. | x1-b still pays −2 on AimedAtAsset FPs. |
| policy.cpp:11 | `kAbortAfter` | 12.0 s | **DERIVED** | `s1.json` `enemy_spawn_interval=14`. A 25 s pursuit spanned two arrivals (D11). | Yes as an upper bound; 12 s returns a failed interceptor before the next spawn. |
| policy.cpp:15 | `kMinClosing` | 1.0 m/s | **GUESS** | D11. Below this, relative range is not shrinking. | Same number as the old abort floor; now it is the *commit* floor too, and the `\|\|` is gone. |
| policy.cpp:16 | `kCatchSlack` | 0.5 s | **GUESS** | D11. Must arrive this much before the cylinder. | Low-risk. *Too high:* refuse a catchable intercept. *Too low:* kill on the wall for ~0 `W_kill`. |
| policy.cpp | `kFreshHostile` | 6.0 s | **DERIVED** | Real intercepts finish in ~4 s of Hostile. Older latches were ghosts (D11). | s1 hostile_4: neighbours had called it and were still chasing a 12 s-old latch. |
| policy.cpp | `kRecommitHold` | 2.0 s | **GUESS** | D11. Stops Hostile/Unknown flicker re-chase. | Low-risk. |
| policy.cpp:12 | `kReportEvery` | 0.5 s | **GUESS** | none | Cheap. |
| policy.cpp | heartbeat prio | 5 (highest) | **DERIVED** | D8/D11 identity. Unread claims used to starve this. | Yes. Claims are not composed. |
| policy.cpp:44 | `ring_radius_` | radio `asset + comm*0.625` then **closed-cover shrink** (D58 / D70); **86 m** on s1 | **MEASURED** cap + **DERIVED** shrink | Radio F swept with D21. Cover: unique-owner Voronoi-edge inbound at picket elevation, leftover Reach ≥ `kCoverSlack` (5 m), else leftover ≥ 0. Same first-sight / Reach as the kill-envelope bracelet. Shrink only; may pull to the asset cylinder (not a collision). | On s1 leftover is ~26 m at H=20 — slack does not bind. It binds on a 6-picket radio ring and on sense 35 m. Growing to buy time is the D21 F0.75 collapse. D73: more slack does not close frozen-24 breaches; 10 m costs t1 W_kill. |
| policy.h | `kCoverSlack` | **5 m** leftover Reach | **MEASURED** (D70 / D73) | D58 leftover ≈ 0 is the knife-edge belt. Named 10: W_kill 821.7 at 0/2/5, 0 breaches. Frozen 24: s=0 W_kill 1772 / mean 79.9; s=5 1765 / 79.6; s=10 1714 / 77.8. Same 2 breaches at every value (`x2-25893f`). 5 m is overlay margin, not a score win. | Too high: t1 pays W_kill (2cd97dca 112→67 at 10 m) without closing the hole. Too low: bracelet sits on leftover 0 whenever cover binds. |
| policy.h | `kOrbitRate` | **0.06 rad/s** | **MEASURED** (D66 / D70) | Closed-form leftover vs ω peaks ~0.05 on a just-closed 6-picket ring (approaching neighbour, full lat + v). Grid 0.04 / 0.06 / 0.08 after D70 slack: 0.06 holds identity 150.7 and canary 3/3; 0.04 wastes on s1; 0.08 re-opens s2 and fa56. | Fat rings want slower (26 m leftover already). Do not integrate ω(R)·t while R shrinks — phase jumps. |
| flight.cpp:ProNav | midcourse/terminal handover | blend over range **25 → 70 m** | **MEASURED** | Grid over handover band and terminal gain, 8 fixed scenarios + 20 fresh ids (D22) | Yes, but it is a ridge, not a plateau: 25–70 gives sweep mean +99.1, 25–60 gives +76.4, 20–60 gives +23.3. The scale-free form (handover on time-to-go) is *worse*, +39.2 — `closing` jitters, range is monotone. |
| flight.h:ProNav | `navigation_gain` | **10.0** | **MEASURED** | Now the gain of a zero-effort-miss law, not classic PN (D26). Escapes over the fresh ids that lose hostiles: N=3 → 14, N=4 → 13, N=6 → 10, N=8 → 10, N=10 → 9, N=14 → 8 but the sweep mean falls +99 → +71 | Yes. Monotone to N=10 with the fixed-set score flat, then the score breaks. The knee is measured, not picked. |
| flight.cpp:ProNav | interceptor speed | `cfg.max_speed` | **DERIVED** | A kill is a ram and the report credits the drone (`losses_by_cause: pair_hostile`, `friendlies_lost_wasted: 0`) | Yes — there is nothing to hold back for. Measured: 0.85·max_speed scores worse at every handover tested. |
| flight.cpp:TiltSettle | `τ = max_tilt / max_body_rate` | **0.075 s** (s1) | **DERIVED** (D61) | `--dump-params` `drone.max_tilt=0.6`, `drone.max_body_rate=8` | Used by `SlewHorizontal` (`\|ȧ_h\| ≈ g ω`), not subtracted from CollisionCourse t_go (D62). |
| policy.cpp:45 | `ring_altitude_` | **20 m** until a cone, then `h0 + slope · ring_r`, **floor `PicketFloorAltitude`** | **MEASURED** (D72) | Cap 30 m (friendly spawn). Floor is `max(6 m, R·tan(10°), ½·Reach(sense/maxv, lat))` so the kill-envelope bracelet is not the horizon cell and half the Cover pancake is not buried. Re-grid after D71: 25 / 20 / 18 / 15. 20 beats 25 on identity (152.3 vs 150.7) and s1 W_kill; 18 drops s0 awareness; 15 re-opens the canary. Not `spawn.enemy_altitude`. | D54's 20/12 was a wash on a stale brain (canary still 2/3). 15 m is the D68 floor on a fat ring and loses hostile_0 again. |
| policy.cpp:slot | slot-reached radius | 8.0 m | **GUESS** | none | Low-risk. |
| policy.cpp:StationBearing | even live rank | `2π · LiveRank / CountLive` | **DERIVED** (D56) | Shared roster from hopped heartbeats. D21 local bisection + 2-slot cap left s2 clustered. D19 global rank without hops disagreed across the radio horizon. D30 local Laplacian evened the 8 and lost on fresh ids. | Full strength is a fixed point. |
| policy.cpp:OwnsInbound | ring-slot owner | facing slot among **live** stations, then that LiveId; handoff to best `InterceptScore` | **DERIVED** (D56 / D66 / D67) | Same angle as even-spaced StationBearing. Heartbeats hop so the roster agrees. Bisector deadband D38. Receding owner yields one live step (D35). Approaching incumbent: challenger must beat by `kHandoffMargin`. Receding + tied t_go: `kHandoffAspect`. | Unique always. D19 kept original-id ownership because stations were local; they are not anymore. |
| policy.h | `kHandoffMargin` | **0.25 s** of InterceptScore | **MEASURED** (D69) | Grid 0 / 0.25 / 0.5 / 0.75 / 1.0 / 1.5 / 2.0. Identity 8 + canary + fa56 + 02e2 is flat (receding ties are `kHandoffAspect`). Discriminator is 24 fresh ids, one token list: 0.25 mean 68.9 / t2 52.6 / 9 civ / 3 breach; 0.0 adds a breach; 1.0 is 53.6 / t2 29.8 / 11 civ. | 0.25 is less than one t_go bin (~0.4 s). This draw had pf=0 at every value, including 0 — D65's 13-airframe thrash is not disproven. Do not ship 0. |
| policy.h | `kHandoffAspect` | **0.5 m/s** TowardTarget | **DERIVED** (D67) | Heartbeat dead-reckon jitters toward by tenths; 0.5 is above that and well below the ~5 m/s gap on fa56 (facing −1.2 vs neighbour +3.8). Not swept. | Too low: two receding poses flicker. Too high: the orbit neighbour never takes a tied t_go bin. |
| policy.cpp:FacingSlot | `kFacingTie` | 0.08 slots | **DERIVED** | D38. 3·`fix_sigma` 1.25 m at 50 m commit range is 4.3 m ≈ 0.08 of a 14-drone slot. `lround` alone let x1-e1257f's 12 and 13 both own a bisector inbound. | Too wide: a clearly-closer neighbour yields. Too narrow: 1 m of track noise still flips. |
| policy.cpp | similar-range duplicate | ±2 m, then lower fleet id | **DERIVED** | D15 was 2 m closer only. Equal-range spool never trips it (D38). Unidentified mate: range rule only. | |
| policy.cpp:RingAlive | heard-then-silent | dead at any range, **latched** until heartbeat (D56) | **DERIVED** | Heartbeats hop. Far silence was a ghost interceptor (D19) and that left s2's hole open. Never-heard stays alive (boot). | |
| policy.cpp:ShouldCommit | commit gate | fresh Hostile (local or fused), unique owner, no closer chaser, `closing ≥ 1`, cruise-catch vs cylinder | **DERIVED** | D11/D15/D18. Sense-range gate removed: hearsay is out of sense. | s3 can pull UniqueOwner with a lying report. |
| protocol.h | `kMaxHops` | 4 | **DERIVED** | Example flood uses 4. s2: spawn 220 m, comm 75 m, ring diameter ~135 m → ~2 hops; 4 is a spare. | Too high: wrap-around duplicates (SeenSet stops the second copy of the same seq). Too low: opposite side of the ring never hears. |
| protocol.h | `kPrioRelay` | 2 | **DERIVED** | Below track 3 and heartbeat 5. D11 identity first. | Relays wait. Under budget they expire at 2 s. |
| policy.cpp | `kChasingToward` | 5.0 m/s | **DERIVED** | Below cruise 14, above picket station-keeping. D15. | A mate flying at the hostile is the interceptor. Do not lower: −5 converts the canary waste and re-opens s1/s2/fa56 (D71). |
| policy.cpp | `SittingWall` | toward ≥ −5, on ring +10 m, **12 m** closer, skip facing incumbent | **MEASURED** (D71) | Canary d4 was toward −1.7, 14 m closer; fa56 facing incumbent 10.4 m. 2 m / 8 m blocked handoff. | Too tight: parked neighbours steal intercepts. Too loose: canary dual-ram returns. |
| policy.cpp | corridor yield | `friendly_margin` off remaining flight; **skip a mate already peeling off** (D59) | **DERIVED** | D15 keep-out, D17 horizon, D59 first-mover. Same cruise / slack / abort as commit. | Pickets on the remaining path step aside unless the other interceptor has already yielded. Same-tick both-on-the-line still both yield. |
| policy.cpp:AbortReason | non-closing abort | after 6.0 s, `closing < 1.0` | **GUESS** | D11. Immediate receding abort dropped an interceptor 5 m out on a weave. | |
| policy.cpp:AbortReason | `uncatchable` | `!CatchableRam` after 0.4 s | **DERIVED** | D48. ½ a t_cpa² along the miss vs leftover to kill. 0.4 s is one weave beat (D11). | A 10 m miss at 1.3 s aborts; the same miss at 5 s stays. |
| flight.cpp:CatchableRam | catchable ram | `divert ≥ miss − kill`, t_cpa ≥ 0 | **DERIVED** | D48. a is `LimitAccel` along the miss (lateral xy, max_accel z). 2·kill is the obvious leftover at the merge, not a long-range cap. | Past CPA or parked relative is a miss. |
| policy.cpp:confidence | confidence encode | `>2.0 ? 255 : score·120` | **GUESS** | none | `score·120` saturates the `uint8_t` at score 2.125, and the branch caps at 2.0, so the mapping is continuous by luck rather than by construction. |
| policy.cpp:outbox | outbox max age | 2.0 s | **GUESS** | none | Sensible. |
| policy.cpp:tx | tx headroom | `len + 64` | **GUESS** | none | Reserves 64 B. Never binds on s1: measured usage is 704 B/s against a 4096 B/s budget, and `frames_dropped_budget = 0` in both reports. |

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
| flight.cpp:95/97 | avoidance strength | `closing·0.15`, `·lateral·2.0` (unknown) / `·2.5` (mate) | **GUESS** | none | Unknown traffic is still a blend (D8). Pickets cancel closing against a mate; interceptors do not (D15). The `·2.5` is extra repulsion. |
| flight.cpp:98 | mate panic radius | `3 · kill_radius` = 3 m | **GUESS** | D8 hard override | Inside this, intercept is abandoned and we accelerate away. *Too high:* abandon a real intercept because a mate is nearby. *Too low:* still closing at 1 m. D38 skips the mate when time-to-target ≤ time-to-mate. |
| flight.cpp:111 | `kEdge` | 20.0 m | **GUESS** | none | Arena is ±200 m (`fixture.jsonl` header `arena.min/max`), so this is a 10% border. Nothing left the arena in either run. |
| flight.cpp:EnforceArena | hard floor slack | **5.0 m** + v²/(2 max_accel) | **DERIVED** (D68) | Replaces az in-band for every mode except Ramming. Gains 4.0/4.0, never commands down. Soft 0.5/0.8 stays on rams (D64). | Too tight: station at 8 m fights the dirt every tick. Too loose: GoTo toward a low slot still eats dirt (D64 open case). |
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
2. ~~**`world.h:167` `kill_radius · 4.0` (4 m unknown-traffic margin)**~~ **Done (D12).**
   4 m is the non-closing floor. Closing unknown/civilian/wreckage grow to
   `v_close²/(2a)+4·kill`. Static 19 m around everyone remains refused (D8).
3. ~~**`belief.cpp:6` `kDropAfter` 3.0 s**~~ **Done (D13).** Local tracks drop
   when they leave `obs.tracks()`. The sim's hold is unpublished and varies;
   absence is the measurement. Hearsay keeps 2 s.
4. ~~**`belief.cpp` `kGate` 12 m**~~ **Done (D14).** Extrapolate by measured age,
   then 8 m. 4 m (sigmas only) duplicated tracks; 12 m merged them.
5. ~~**`policy.cpp` `|| ttg < 12`**~~ **Done (D11).** Commit is local Hostile + relative closing + catchable vs the cylinder. The remaining G3 knob is `kEvidenceForCall` (row above): raising it late-commits, lowering it re-opens G1.
