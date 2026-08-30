# Code map: plumbing vs judgement

Every function and type in `brain/src/`, sorted by whether you have to defend it.

- **PLUMBING** — mechanical. Correct or incorrect, not arguable. Defend as "standard, and tested".
- **JUDGEMENT** — someone chose; it could have gone another way. **These are yours to own.**
- **STUB** — declared, not really implemented, or marked TODO.

Tally: **21 plumbing · 17 judgement · 6 stubs.**

---

## PLUMBING

| Item | file | Note |
|---|---|---|
| `FixedVec<T,N>` | world.h:27 | Fixed-capacity vector, order-preserving `erase`. Chosen for determinism (no node containers → no allocation-order iteration). `push` returns `nullptr` when full rather than growing. |
| `Rng` | world.h:79 | Wraps `host->random`. The only randomness in the brain. Currently unused (`policy.h:55` admits it). |
| `Config::From` | world.h:139 | Copies `SwBootInfo` into one struct so no module reads the ABI directly. The two computed fields are judgement — see below. |
| `Belief` enum, `ToSwClass` | world.h:171-194 | Enum mapping. The *wreckage* case was judgement; it is now measured — see below. |
| `Track` | world.h:196 | Per-entity record. The `has_local_id` flag is a real invariant, not decoration. |
| `RangeRate` | belief.cpp:19 | Closing rate, horizontal. Standard. |
| `ApproachAlignment` | belief.cpp:26 | Normalised dot product. Standard. |
| `ClosestApproachDistance` | belief.cpp:36 | Miss distance for two straight lines, clamped to t ≥ 0. Standard and correct. |
| `TimeToTarget` | belief.cpp:51 | Range ÷ closing rate with a sentinel. Standard. |
| `TrackStore::Find` | belief.cpp:69 | Linear search, gated on `has_local_id`. |
| `TrackStore::NearestTo` | belief.cpp:75 | Linear nearest. |
| `TrackStore::Update` | belief.cpp:85 | Fold the sensor picture in, then age out. Iterates `obs.tracks()` (simulator-ordered) and erases backwards — both deliberate for determinism. |
| `Writer` / `Reader` | protocol.h:30, 80 | Bounds-checked, explicit little-endian. Byte order is a property of the code, not the compiler. Well covered by `test_protocol.cpp`. |
| `Q16`/`DQ16` | protocol.h:65, 116 | Fixed-point codec. Round-trips correctly across the full ±4095 m range (verified by hand: 4095 → 65528 → 4095). |
| `Header::Write/Read` | protocol.h:144, 156 | Fixed 10-byte header; rejects wrong version, unknown type, truncation. |
| `HeartbeatMsg`, `TrackReportMsg`, `ClaimMsg` | protocol.h:175-212 | Payload codecs. |
| `SeenSet<N>` | protocol.h:223 | Dedup ring with an explicit `occupied_` flag, fixing the zero-collision the example brain documents in itself (`hover_relay_brain.cpp:140-141` — claim **verified**). |
| `Outbox<N>` | protocol.h:269 | Priority queue with age expiry. `Best()` is a genuine total order (priority, then `queued_at`). |
| `LimitAccel` | flight.cpp:11 | Splits horizontal against `lateral_limit` and vertical against `max_accel`. The one piece of maths the brief explicitly warns about, done right. |
| `RingSlot` | flight.cpp:129 | `drone_id → angle`. No negotiation, so it works before the radio does and instances cannot disagree. |
| `SwarmBrain::Announce` / `NotePeer` | brain.cpp:50, 122 | Startup log (correctly deferred out of `create()`), and a bounds-checked peer table. |

---

## JUDGEMENT

**These are the interview.** Each is: what was decided → what else was on the table → what it costs.

### Architecture

1. **Four modules: belief / policy / flight / protocol** (`brain.cpp:29-46`)
   Decided: a fixed tick order — sensors, then radio, then decide, then declare, then speak,
   then fly — with each module ignorant of the others' concerns. Alternatives: one class; or
   a behaviour tree. Costs: more plumbing for a small brain, and the split is currently
   *unproven* against the tier-5 insider problem, which needs a per-peer trust model that
   belongs in neither `belief` (it is about peers, not aircraft) nor `policy`. Expect to add
   a fifth module, and say so before a reviewer asks.

2. **Own sensors before peer reports** (`brain.cpp:29-35`)
   Decided: `store_.Update()` runs before `ConsumeFrames()`, so hearsay is always matched
   against fresh local truth. Alternative: merge both into one association pass. Costs:
   nothing obvious. Good call; keep it.

3. **`FixedVec` everywhere, no heap after `create`** (world.h:3-8)
   Decided: determinism over convenience. Alternative: `std::vector` and sort by a stable key
   before iterating. Costs: capacity limits become silent drops (`belief.cpp:96`); a
   `push` returning `nullptr` is handled by `continue`, so at 64 tracks new aircraft are
   simply not seen. Nothing reports that this happened.

### Classification — *the tier-1 score lives here*

4. **Behaviour-only classification, integrated over time** (`belief.cpp:122-185`)
   Decided: no single-tick call; evidence accrues at `+dt` while the geometry holds.
   Alternative: instantaneous threshold, or a Bayesian filter. Costs: latency before any call,
   and the accrual/decay asymmetry (1.0/s up, 0.6/s down) biases toward false positives.

5. **Miss distance as the primary discriminant** (`belief.cpp:152-157`)
   Decided: `ClosestApproachDistance < 0.8·asset_radius` is the gate, on the argument that a
   steering hostile's miss distance collapses while a civilian's does not. Alternatives:
   alignment alone (rejected in the comment, correctly); or the *derivative* of miss distance.
   **Costs: this is the single most expensive decision in the brain.** The reasoning is right
   but the implementation tests the *value*, not the *trend* — a civilian on a chord that
   happens to pass within 24 m reads as hostile and stays that way. Measured: 2 of 8 civilians
   misclassified, both rammed, −380. The fix the comment already names (belief.cpp:147-149)
   was never implemented.

6. **Wreckage declared `SW_CLASS_UNKNOWN`** (`world.h:190`)
   Decided: take the 0 rather than bet on NEUTRAL at 1:2 odds. **Now measured, not guessed:**
   a controlled re-run declaring wreckage NEUTRAL moved `wrong_declarations` 229 → 239 with
   `correct_declarations` and `samples` unchanged and an identical `state_hash`. Wreckage
   scores as **wrong**. The original decision was correct; the comment now records the evidence.

7. **Peer reports move the score, never set the verdict** (`belief.cpp:212-220`)
   Decided: a peer's `Hostile` adds `confidence/255` to `closing_score`; only our own threshold
   promotes a belief. Alternative: trust a peer outright, or require N independent reports.
   Costs: a single confident peer (255) contributes 1.0 against a 1.2 threshold — so *two*
   peers can promote a track we have never seen. On tier 3+ that is a replay attack for free,
   and on tier 5 an insider needs one accomplice. **This is the "how far do I trust an
   unverifiable peer" question the brief says is deliberately unanswered — you have answered
   it implicitly at 0.47 peers-per-verdict. Make that explicit and defend the number.**

8. **Geometric association for hearsay, 12 m gate** (`belief.cpp:187-206`)
   Decided: associate peer reports by position because `track_id` is observer-local.
   Alternative: a fleet-wide entity id (needs consensus), or refusing to fuse at all.
   Costs: at 12 m two aircraft in formation merge into one track.

9. **Threat ordering by time-to-go** (`belief.cpp:223-238`)
   Decided: soonest-to-arrive first, not nearest. Correct — it matches how the reward
   normalises by `t_free` (§9.1). Costs: none; but see the comment defect below.

### Policy

10. **Static ring picket from `drone_id`** (`policy.cpp:30`, `flight.cpp:129`)
    Decided: ring radius `asset_radius + comm_radius/2` = 75 m, altitude 30 m, slot by id.
    Alternatives: dynamic assignment by nearest-slot; a sphere rather than a ring; picket at
    the threat bearing. Costs: it does not re-space as drones die (`policy.cpp:27-29` admits
    this), and `s1.json`'s own `_fleet` note says the late arrivals against a holed picket are
    exactly where tier 1 leaks. The ring also sits at 30 m altitude while civilians fly at 50
    and hostiles at 40 — so the picket is *below* all the traffic it is meant to meet.

11. **Commit on our own belief alone** (`policy.cpp:66-87`)
    Decided: any drone that believes a track is hostile and passes the gates commits.
    Alternative: the allocation rule the TODO names (deterministic tie-break so two drones
    reach the same answer without negotiating). Costs: every drone that can see a hostile
    commits to it — the brief's named failure mode ("three drones per hostile is how naive
    swarms bleed"), and two converging interceptors are also converging on each other.

12. **Abort on time and non-convergence** (`policy.cpp:89-100`)
    Decided: 25 s hard, or 6 s without closing. Alternative: abort on predicted miss distance.
    Costs: 25 s spans two hostile arrivals at s1's 14 s spawn interval.

13. **Declare every track, every tick** (`policy.cpp:117-124`)
    Decided: spam the hook; it is free and only the most recent declaration is scored.
    Correct reading of §9.3. Costs: it also declares *hearsay* tracks, which all carry
    `track_id = 0` — see finding below.

14. **Heartbeat 2 Hz + hostile reports 2 Hz + claims** (`policy.cpp:126-206`)
    Decided: a fixed-rate chatter budget. Measured cost 704 B/s of a 4096 B/s budget →
    comms 33.1/40. Alternative: event-driven only, or adaptive to measured budget.
    Costs: heartbeats dominate the traffic and buy nothing yet, because nothing consumes
    them (see stub 3). You are paying 17% of budget for a message no one reads.

15. **Priority queue, not FIFO** (`protocol.h:266`, `policy.cpp:14-17`)
    Decided: a detection outranks a heartbeat; under pressure the heartbeat is dropped.
    Good, and cheap to defend. Costs: none observed — the outbox never filled.

16. **Send at most one frame per tick with 64 B headroom** (`policy.cpp:208-222`)
    Decided: never spend to zero. Costs: none on s1 (`frames_dropped_budget = 0`).

### Flight

17. **Separation as a repulsion term, not a hard constraint** (`flight.cpp:74-107`)
    Decided: add a repulsion vector and re-limit. **The header (flight.h:44-50) is admirably
    honest that this is a tendency, not the guarantee the brief asks for**, and names the two
    real fixes (override inside a critical radius; or project out the closing component).
    Costs: measured — two civilian collisions in 8.4 s. This is the honesty-vs-behaviour gap
    a reviewer will find first, and the write-up should get there before they do.

---

## STUBS

| # | Item | file | State |
|---|---|---|---|
| 1 | `MsgType::Accuse` | protocol.h:20 | Enum value only. No message struct, no send path, no handler (`brain.cpp:110` falls through). Tier 5 is unaddressed. |
| 2 | `declare_identity` | policy.cpp:121-123 | TODO comment only. **Never called.** Per §9.3 an insider only counts if named by key, so tier-5 detection currently scores 0 by construction. |
| 3 | Claim handling | brain.cpp:101-108 | Claims are *sent* (`policy.cpp:188-203`) but the receiver does nothing with them. The whole de-confliction mechanism is one-way, so it has no effect. |
| 4 | Multi-hop relay | brain.cpp:115-118 | TODO. Consequence is measurable: `propagation_p95_s` is **0.09 s** for the example brain (which floods) and **`null`** for ours — we transmit but nothing crosses the fleet. Tier 2 is unreachable without this. |
| 5 | Friendly identification from heartbeats | brain.cpp:89-90, belief.cpp:177-180 | TODO in two places. Friendlies are 16 of 24 entities on s1, so this is the largest single block of free awareness score, and it is unclaimed. |
| 6 | `Rng` | policy.h:55 | Constructed, stored, never used. Honest comment says it is a hook for jittering send times. |

---

## Defects found while classifying

Not changed — these need your call, not mine.

1. **`EnforceSeparation` exempts by `track_id`** (`flight.cpp:81`).
   `if (exempt && t.track_id == exempt->track_id)`. Hearsay tracks never get a local id, so
   they all sit at `track_id == 0` (`belief.cpp:199`). If the committed target is a hearsay
   track, **every** hearsay track is exempted from separation at once. This is exactly the
   collision `world.h:200-203` warns about, reproduced one file over. Compare pointers, or
   compare `(has_local_id, track_id)`.

2. **`Declare` publishes hearsay under `track_id = 0`** (`policy.cpp:118-120`).
   It iterates all tracks and declares `t.track_id` without checking `has_local_id`, so
   beliefs about peer-reported entities are all attributed to track id 0 — which may also be
   a real local track. Measured: 229 wrong declarations against 99 correct. Some fraction of
   that is this. Gate the loop on `has_local_id`.

3. **`MostUrgentHostile` comment overstates the code** (`belief.cpp:231-235`).
   Comment: "Total order: time-to-go, then range to us, then track_id." Code:
   `key = ttg + d·0.001`, and `track_id` is never consulted. It is a weighted sum, not a
   lexicographic order, and two tracks with equal `key` resolve by iteration order — the
   determinism hazard the comment claims to have closed. **Comment corrected; the code was
   left alone**, because changing the ordering changes behaviour.

4. **Unreachable clamp bounds** (`belief.cpp:129`, `:162`). The decay paths clamp to 3.0 and
   4.0 while the accrual paths cap at 1.5 and 3.0. Harmless, but it reads as a copy-paste and
   a reviewer will ask.

5. **Brief vs report schema.** CHALLENGE.md §9.2 says "`friendlies_lost_wasted` breaks it out
   per cause". In the real report `friendlies_lost_wasted` is a scalar and the per-cause
   breakdown is a separate `losses_by_cause` object. Not your bug — but do not write a
   viewer or a results table against the brief's wording.
