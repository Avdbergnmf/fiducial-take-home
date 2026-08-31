# Reading the brain

A reading order for `brain/src/`, ~1900 lines across 9 files. Sit down with this
once; it is the piece you have not internalised, and on Tuesday you will be
asked to justify it rather than describe it.

Two companion documents already exist and are not repeated here:

- **`notes/provenance.md`** — every constant, with its source and whether it is
  defensible. The GUESS table and tuning directions live there.
- **`notes/code-map.md`** — the full plumbing / judgement / stub classification,
  with alternatives and costs for each judgement call.

This file is the *reading order* and the *summary*; those two are the reference.

---

## Read in this order

**1. `world.h` (232 lines) — the vocabulary.** Nothing here decides anything.
`FixedVec` (no heap after construction, order-preserving), `Rng` (the only
randomness), `Config` (everything from `SwBootInfo` in one struct), `Belief`,
`Track`. Read the header comment: the determinism rules are stated at the top
and the rest of the file is those rules applied.

**2. `protocol.h` (323 lines) — the wire.** Read `Writer`/`Reader` first, then
`Header`, then the three payloads, then `SeenSet` and `Outbox`. Knows nothing
about tactics. Versioned from byte 0, explicit little-endian, bounds-checked on
every read. This is the most finished file in the brain.

**3. `belief.h` + `belief.cpp` (322 lines) — what is out there.** The four pure
geometry functions first (`RangeRate`, `ApproachAlignment`,
`ClosestApproachDistance`, `TimeToTarget`), then `LooksBallistic`, then
`TrackStore::Classify`. **`Classify` is the most important function in the
brain** and the one currently costing you the most.

**4. `flight.h` + `flight.cpp` (209 lines) — how to move.** `LimitAccel` first
— it encodes the one piece of physics the brief explicitly warns about. Then
`ProNav`, then `EnforceSeparation`. Read `flight.h`'s comment on
`EnforceSeparation`; it is unusually honest about its own limits.

**5. `policy.h` + `policy.cpp` (293 lines) — what to do about it.** `Decide`,
`ShouldCommit`, `ShouldAbort`, `Declare`, `Compose`, `Pump`. This is where the
judgement is concentrated.

**6. `brain.cpp` (173 lines) — the glue and the tick order.** Read last, because
it makes sense only once you know the four modules. The tick order is the
architecture in six lines.

---

## The module boundaries, and whether they hold

The claim the design makes: **a new requirement should land in exactly one
module.**

| Module | Owns | Must not know about |
|---|---|---|
| `belief` | what an aircraft *is* | the radio format, how to fly |
| `flight` | how to get somewhere | what anything is, what it means |
| `protocol` | bytes on the wire | tactics, geometry |
| `policy` | what to *do* | byte layout, control laws |

`brain.cpp`'s tick order is the enforcement: sensors → inbound radio → decide →
declare → outbound radio → fly. Own sensors are folded in **before** peer
reports, deliberately, so hearsay is always matched against fresh local truth.

**Where it does not hold, and be ready for this question:** the tier-5 insider
problem is about *peers*, not *aircraft*, and it fits neither `belief` (which
classifies things you can see) nor `policy` (which decides actions). A per-peer
trust model is a fifth module. Say that before a reviewer finds it — the honest
version reads far better than a claim that the split is universal.

---

## Plumbing you can defend as "standard and tested"

21 items, in `notes/code-map.md`. The ones worth naming out loud:

- **`Writer`/`Reader`** — explicit shifts, not `memcpy` of a struct, so byte
  order is a property of the code and not of the compiler. Covered by
  `test_protocol.cpp` including truncation, wrong version, unknown type and
  random garbage.
- **`LimitAccel`** — splits horizontal against `lateral_limit` and vertical
  against `max_accel`. This is the trap CHALLENGE.md §5.4 flags and `s1.json`'s
  `_evasion` note repeats; the code gets it right.
- **`SeenSet`** — carries an explicit `occupied_` flag, fixing the
  zero-collision the example brain documents in *itself* at
  `hover_relay_brain.cpp:140`.
- **`FixedVec` + iterating simulator-ordered inputs** — why determinism passes
  4/4 today.

If asked "what did you test?", the answer is: the codec against hostile input,
the classifier against synthetic geometry drawn from `s1.json`'s actual spawn
radii, and the whole brain against `--record`/`--replay` at two thread counts.

---

## Judgement calls you have to own

17 items in `notes/code-map.md`; these five are the ones a reviewer will
actually press on.

1. **Miss distance as the discriminant** (`belief.cpp:152`). A steering hostile's
   closest-approach distance collapses; a civilian's does not. The *reasoning is
   right* and the implementation is not: it tests the **value** (< 24 m), not the
   **trend**. A civilian on a chord that happens to pass near the asset reads as
   hostile and stays that way. Measured cost: 2 of 8 civilians, both rammed.
   The fix is in the code's own TODO at `belief.cpp:147`.

2. **How much a peer's word is worth** (`belief.cpp:212`). A peer's `Hostile`
   adds `confidence/255` to a score with a 1.2 threshold, so a single maximally
   confident peer contributes 1.0 and **two peers can promote a track you have
   never seen**. The brief says explicitly that how far to trust an unverifiable
   peer is deliberately unanswered (§13). You have answered it at ~0.47
   peers-per-verdict, by accident. Either defend that number or choose it
   deliberately — this is exactly the "judgement under ambiguity" they grade.

3. **Separation as a repulsion term, not a constraint** (`flight.cpp:74`).
   `flight.h:44` is admirably honest that this is a tendency and not the hard
   guarantee the brief asks for, and names the two real fixes. Good comment,
   measured failure. Get to it before the reviewer does.

4. **Commit on own belief alone** (`policy.cpp:66`). Every drone that sees a
   hostile commits to it. The brief names this as how naive swarms bleed.

5. **Static ring picket from `drone_id`** (`policy.cpp:30`). Radius
   `asset_radius + comm_radius/2` = 75 m, altitude 30 m, no re-spacing as drones
   die. Note the picket sits at 30 m while civilians fly at 50 and hostiles at
   40 — it is *below* all the traffic it is meant to meet.

---

## Constants that are guesses

**29 of 41 are guesses.** The full table with citations, tuning direction and
what goes wrong in each direction is `notes/provenance.md` — do not duplicate it
here, read it there before you tune anything.

The short version — the five that matter, in tuning order:

| Constant | Value | Direction |
|---|---|---|
| `belief.cpp:157` miss gate | `asset_radius × 0.8` = 24 m | **down**, and better, test the trend not the value |
| `world.h:162` separation margin | `kill_radius × 4` = 4 m | **up**, but only together with the ring radius |
| `belief.cpp:6` `kDropAfter` | 3.0 s | to 2.0, matching the measured `sense.track_drop_time` |
| `belief.cpp:193` `kGate` | 12 m | **down** to ~3 m, per its own stated reasoning |
| `policy.cpp:86` commit gate | `\|\|` of two conditions | make it `&&`, or justify the stern chase |

Only 12 constants are actually sourced. The best-sourced is
`lateral_limit = 9.81·tan(max_tilt)` = 6.71, which is cited in **two**
independent places (CHALLENGE.md §5.4 and `s1.json`'s `_evasion` note). If you
are asked "which number are you most confident in", that is the one.

---

## The TODO markers

**There are 13, not ~30.** Grouped by the goal they belong to
(`notes/brain-goals.md`):

**G2 — awareness (2)**
- `belief.cpp:177` — friendlies are identifiable once heartbeats run
- `brain.cpp:89` — a track sitting where a peer says it is, is a friendly

*Both describe the same free win: 16 of 24 entities on s1.*

**G1/G3 — classification (2)**
- `belief.cpp:147` — a civilian on a chord holds alignment for seconds; needs
  the miss distance at closest approach or a second observer
- `belief.cpp:193` — size the association gate from `fix_sigma` and measured
  latency rather than a constant

**G4 — allocation (3)**
- `policy.cpp:73` — commits on own belief alone; needs a deterministic tie-break
- `brain.cpp:105` (`TODO(B3)`) — honour peer claims
- `policy.cpp:27` — the ring does not re-space as drones are spent

**G5 — comms (2)**
- `brain.cpp:115` (`TODO(B4)`) — multi-hop relay; the whole of tier 2
- `policy.cpp:157` — rate-limit per track and batch reports into one frame

**G6 — tiers 3–5, out of scope (4)**
- `brain.cpp:69` — claimed position vs the receiver's *measured* range, the
  cheap replay defence with no crypto
- `brain.cpp:76` — freshness window sized from measured latency
- `brain.cpp:79` — per-peer position/range residual, the one thing an insider
  cannot lie about
- `policy.cpp:121` — `declare_identity` is a separate hook; an insider only
  counts if named by key

The tier-3/5 TODOs are worth **reading before you write DESIGN.md** even though
you will not implement them: they are already most of a paragraph each on how
you would treat a peer you cannot verify, which is one of the questions §12
explicitly asks the document to answer.

---

## The one thing to notice before you start

The brain scores **−1546.9 on s1**; the example brain that only hovers scores
**−1166.9**. Every module in here is reasonable in isolation and the composition
is currently worse than doing nothing, because three defensible local decisions —
a loose miss gate, an eager commit rule, and a thin separation margin — chain
into two dead civilians.

That is a genuinely good thing to have noticed yourself, and it is a better
interview answer than a higher score with no explanation.
