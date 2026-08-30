# What the fixture actually proves

Source: `runs/fixture.jsonl` (1004 lines, 1.6 MB), `runs/fixture.json`, and
`cd pkg && bin/swarm_sim --dump-params --scenario s1`.

The fixture is an s1 run of the **example** brain (`examples\build\Release\brain.dll`),
which hovers and floods. It scores −1166.9 and stops nothing. It is still the only
ground truth we have about the recording format.

---

## a. Record types and the header

`inspect_trace.py` on the fixture:

| type | count |
|---|---|
| `frame` | 890 |
| `telemetry` | 89 |
| `log` | 16 |
| `links` | 7 |
| `header` | 1 |
| `report` | 1 |

**`beliefs` is absent** — the example brain declares nothing, so no delta was ever emitted.
A run of our brain does produce them (33 records), so the type exists and is reachable.

The `header` record, in full:

```json
{"type": "header", "schema": 1, "sim_version": "swarm_sim 0.6.0", "scenario": "s1",
 "brain": "examples\\build\\Release\\brain.dll", "dt": 0.01, "trace_hz": 10, "fleet_size": 16,
 "arena": {"min": [-200.0, -200.0, -120.0], "max": [200.0, 200.0, 0.0]},
 "asset": {"position": [0.0, 0.0, 0.0], "radius": 30.0},
 "kill_radius": 1.0,
 "entity_columns": ["id","cls","drone","px","py","pz","vx","vy","vz","qw","qx","qy","qz","compromised"],
 "entity_classes": ["friendly","enemy","neutral","wreckage"],
 "belief_classes": ["unknown","friendly","enemy","neutral","compromised"],
 "frame_of_reference": "NED, metres, z down",
 "record_types": ["header","frame","links","beliefs","log","telemetry","report"]}
```

**The frame column names are `entity_columns`** — 14 columns, in that order. A `frame`
record is `{"type","tick","t","entities"}` where `entities` is a list of 14-element rows:

```json
{"type": "frame", "tick": 1, "t": 0.01,
 "entities": [[1, 0, 0, 60, 0, -30, 0, 0, 0, 1, 0, -0.0002, 0, 0], ...]}
```

Other record shapes observed:

- `links` — `{"type","t","add","remove"}`, `add`/`remove` are lists of `[drone_a, drone_b]` pairs.
- `beliefs` — `{"type","t","set"}`, `set` is a list of `[observer_drone, entity_id, belief_class]`.
- `telemetry` — per drone, 1 Hz.
- `report` — the whole `--report` document as the last line.

Note `arena.min.z = -120` (ceiling) and `arena.max.z = 0` (ground), which **confirms the
comment at `flight.cpp:122`** — in NED, `arena_min.z` really is the ceiling.

---

## b. Wreckage — settled

**Yes. Wreckage is entity class 3, named `"wreckage"` in `entity_classes`.**

The fixture alone **cannot** show it: `mission.wreckage_spawned = 0`, because the example
brain never collides with anything. A run of our brain does spawn wreckage, and it appears
in `frame` rows with `cls == 3` and `drone == -1`:

```
t=7.8   [25, 3, -1, -32.88, 82.69, -38.41, -0.01, -2.33, -1.46, 0.5179, 0.0188, 0.0401, 0.8543, 0]
t=7.9   [25, 3, -1, -32.89, 82.46, -38.51, -0.01, -2.32, -0.47, ...]
t=8.0   [25, 3, -1, -32.89, 82.23, -38.50, -0.01, -2.31,  0.51, ...]
t=8.1   [25, 3, -1, -32.89, 82.00, -38.40, -0.01, -2.30,  1.49, ...]
```

`vz` goes −1.46 → −0.47 → 0.51 → 1.49 over 0.3 s: **9.8 m/s² downward**. That empirically
confirms the premise of `LooksBallistic` and puts real g comfortably inside its 6.0–14.0
window.

### How it is scored — settled by experiment

`belief_classes` is `[unknown, friendly, enemy, neutral, compromised]`. There is **no
wreckage belief class**, so no declaration about a wreckage entity can ever match the truth
label. That is suggestive but not proof, so it was measured directly: the same brain, with
the single line `ToSwClass(Belief::Wreckage)` changed from `SW_CLASS_UNKNOWN` to
`SW_CLASS_NEUTRAL`, same scenario, same seed:

| | UNKNOWN (as shipped) | NEUTRAL |
|---|---|---|
| `correct_declarations` | 99 | 99 |
| `wrong_declarations` | **229** | **239** |
| `samples` | 4261 | 4261 |
| `state_hash` | `0xfaf716b92428bf5d` | `0xfaf716b92428bf5d` |

Identical state hash confirms the change altered nothing but declarations. Declaring
wreckage NEUTRAL produced **10 more wrong calls and zero more correct ones**.

**Conclusion: wreckage declared as anything scores wrong. `SW_CLASS_UNKNOWN` is correct.**
The TODO at `world.h:189` is resolved and the comment now records this.

---

## c. Report field names — all three confirmed

| Referenced as | Real? | Actual location and value in `runs/fixture.json` |
|---|---|---|
| `friendlies_lost_wasted` | **Yes** | `mission.friendlies_lost_wasted` = 0 |
| `asset_survival_time_s` | **Yes** | `mission.asset_survival_time_s` = 18.96 |
| `comms.propagation_p95_s` | **Yes** | `comms.propagation_p95_s` = 0.09 |

One correction to carry into RESULTS.md: CHALLENGE.md §9.2 says `friendlies_lost_wasted`
"breaks it out per cause". It does **not** — it is a scalar. The per-cause breakdown is a
separate field, `mission.losses_by_cause` (e.g. `{"pair_neutral": 2}`), alongside
`mission.friendly_losses`, a list of `{t, drone, cause, credited}`.

Full top-level report keys: `awareness`, `brain`, `comms`, `compute`, `events`, `integrity`,
`mission`, `outcome`, `realtime_factor`, `scenario`, `schema`, `score`, `sim_time_s`,
`sim_version`, `ticks`, `wall_time_s`.

The scoring formula for comms is confirmed arithmetically: `bytes_per_drone_per_s = 704.1`,
and `40 · (1 − 704.1/4096) = 33.1` = `score.comms`. Mission `−1200` = 6 breaches × `P_breach`
200.

---

## d. s1 parameters per `--dump-params`

| Parameter | Value | Also in |
|---|---|---|
| `drone.kill_radius` | **1.0** | `fixture.jsonl` header `"kill_radius": 1.0` |
| `drone.max_tilt` | **0.600000024** rad | — |
| `drone.max_accel` | **15** | — |
| `sense.radius` | **60** | `s1.json` `sense.radius` |
| `comm.radius` | **90** | `s1.json` `comm.radius` |

Derived: `lateral_limit = 9.81 · tan(0.6) = 6.7117 m/s²`, matching CHALLENGE.md §5.4
("around 6.7") and `s1.json`'s `_evasion` note ("g*tan(max_tilt) = 6.7 m/s^2 and not
max_accel"). `world.h:161` computes exactly this. **The one constant in the brain with two
independent citations.**

Other constants worth having: `sense.track_drop_time=2.0`, `sense.fix_sigma=0.35`,
`sense.fix_bias_sigma=1.2`, `comm.latency_ticks=2`, `comm.jitter_ticks=1`,
`comm.packet_loss=0.02`, `comm.mtu=256`, `comm.tx_budget_bytes_per_s=4096`,
`adv.evasion_trigger_radius=40`, `adv.evasion_A=3.5`, `adv.enemy_dash_speed=16`,
`sim.time_limit=180`, and the full scoring block (`W_kill=100`, `P_waste=40`,
`P_civilian=150`, `P_breach=200`, `W_aware=60`, `W_detect=80`, `detect_deadline=30`,
`P_false_accuse=120`, `W_comms=40`, `W_learn=60`, `score.awareness_sample_period=1`).

Full dump is 104 lines; re-run it rather than trusting a copy.

---

## e. What the recording contradicts

1. **`Config::kill_radius` default was 3.0; the real value is 1.0.** Every other default in
   that block is the s1 value. **Fixed.** It was always overwritten by `Config::From`, so it
   never reached flight — but `separation_margin = kill_radius · 4` reads off it, so anyone
   sanity-checking the margin by eye got 12 m instead of the real 4 m.

2. **`kDropAfter = 3.0 s` vs the sim's `sense.track_drop_time = 2.0 s`.** We hold tracks a
   second longer than the simulator does. Not changed — it may be deliberate — but it is a
   number the brief calls unpublished which is in fact measurable on fixed scenarios, and a
   reacquired entity returns under a *new* `track_id`, so the stale copy becomes a duplicate.

3. **`propagation_p95_s` is `0.09` for the example brain and `null` for ours.** The example
   floods; we removed the flood and did not replace it. Nothing our fleet says crosses more
   than one hop, so the metric has nothing to measure. This is stub #4 in the code map, and
   it is the whole of tier 2.

4. **Awareness is worse than nothing.** Our run: 99 correct, **229 wrong**,
   `belief_accuracy = 0.023`. The term clamps at 0 so it costs no points directly, but a
   classifier wrong 70% of the time is what feeds the commit decision — see below.

5. **The classifier's false positives are lethal, and provably so.** From the `beliefs`
   records of our own run, with the first enemy spawning at t = 8.0 (`s1.json`):

   ```
   t=3.8   beliefs set [[5, 24, 2]]     drone 5 calls entity 24 "enemy"
   t=4.2   beliefs set [[1, 22, 2]]     drone 1 calls entity 22 "enemy"
   ```

   Entities 22 and 24 are both `cls == 2` (**neutral**) in frame 1, and no enemy exists yet.
   The report then records:

   ```
   friendly_losses: [{t: 7.74, drone: 5, cause: "pair_neutral"},
                     {t: 8.41, drone: 1, cause: "pair_neutral"}]
   ```

   The two drones that misclassified a civilian are exactly the two that rammed one. Cost:
   2 × `P_civilian` 150 + 2 × `P_waste` 40 = **380**, which is the entire gap between our
   −1546.9 and the do-nothing example's −1166.9.

   **Our brain is currently worse than hovering**, and the cause is a chain of three
   decisions, each individually defensible: a 24 m miss gate, a commit rule that needs no
   corroboration, and a separation margin of 4 m.

---

## For the viewer adapter

`tools/build_viewer_data.py` does not exist in the repo (see the audit summary), but when it
lands, this is the mapping it needs. Read it from the header at runtime rather than hardcoding
— `entity_columns` is exactly why that record exists.

| Semantic | Trace source |
|---|---|
| entity id | `entity_columns` index of `"id"` → 0 |
| class | `"cls"` → 1, values index `entity_classes` = `[friendly, enemy, neutral, wreckage]` |
| owning drone | `"drone"` → 2, `-1` for non-fleet entities (incl. wreckage) |
| position NED | `"px","py","pz"` → 3,4,5 |
| velocity NED | `"vx","vy","vz"` → 6,7,8 |
| attitude | `"qw","qx","qy","qz"` → 9,10,11,12 |
| compromised (ground truth) | `"compromised"` → 13 |
| frame time | `frame.t` (seconds); `frame.tick` is the sim tick |
| arena | `header.arena.min/max`, z negative = up |
| asset | `header.asset.position`, `header.asset.radius` |
| links | `links.add` / `links.remove`, `[drone_a, drone_b]`, **deltas** |
| beliefs | `beliefs.set`, `[observer_drone, entity_id, belief_class]`, **deltas**, indexes `belief_classes` |
