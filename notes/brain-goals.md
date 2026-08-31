# Goals, in the order that earns score

Ordered by points per hour, not by tier number. A day and a half is roughly
12 working hours; this ladder spends about 10 and leaves 2 for the write-up.

## The numbers this is built on

The scoring weights (`--dump-params`, and CHALLENGE.md §9): `W_kill` 100,
`P_breach` 200, `P_waste` 40, `P_civilian` 150, `W_aware` 60, `W_comms` 40,
`W_detect` 80, `P_false_accuse` 120.

**Measured baseline of the current skeleton** (sweep, `brain.so`, this repo):

| id | total | kills | breach | wasted | civ | aware | comms | losses by cause |
|---|---|---|---|---|---|---|---|---|
| s1 | **−1546.9** | 0/6 | 6 | 2 | 2 | 0.0 | 33.1 | `pair_neutral=2` |
| s2 | −1308.5 | 1/6 | 5 | 5 | 1 | 0.1 | 36.1 | `pair_friendly=4 pair_neutral=1` |
| x2-a | −1185.1 | 0/4 | 4 | 3 | 2 | 0.0 | 34.9 | `pair_neutral=2 wreckage=1` |
| x1-a | −1135.2 | 1/4 | 3 | 3 | 3 | 0.0 | 34.8 | `pair_friendly=2 pair_hostile=1 pair_neutral=1` |
| x1-c | −792.5 | 0/3 | 3 | 2 | 1 | 0.0 | 37.5 | `pair_neutral=1 wreckage=1` |
| x2-b | −647.7 | 1/3 | 2 | 7 | 0 | 0.0 | 31.3 | `ground=1 pair_friendly=6 pair_hostile=1` |
| x1-b | −586.2 | 1/4 | 3 | 1 | 0 | 0.0 | 36.3 | `ground=1 pair_hostile=1` |
| s0 | **+39.6** | 0/0 | 0 | 0 | 0 | 0.0 | 39.6 | — |
| s5 | −1426.3 | 1/6 | — | — | — | 0.0 | 36.3 | 1 compromise, 0 detected |

**The number that should focus the day: the example brain scores −1166.9 on s1
and does nothing but hover.** The skeleton scores −1546.9. It is currently
**380 points worse than not flying at all**, and the whole gap is two civilian
collisions (2 × 150 + 2 × 40). Everything in G1 is about getting back to zero
before trying to be clever.

Two things are already fine and need no work: **comms is 31–40 out of 40 on
every scenario**, and **determinism passes 4/4**. Do not spend time there.

---

## G0 — Know your numbers (30 min)

Run the baseline and keep it. Commands in `notes/brain-loop.md` §0.

**Done when:** `runs\sweep_baseline\` exists, determinism is 4/4, and you have
seen s1 = −1546.9 with your own eyes.

**Why first:** every later claim ("this helped") is a comparison against this.

---

## G1 — Stop paying for your own mistakes (2–3 h) ★ do this first

The single highest-value hours in the whole exercise. No new capability; just
stop losing points you are handing over for free.

**Two separate bugs, in this order:**

**G1a — stop ramming civilians.** `pair_neutral` on 5 of 8 scenarios, −190 each
time (`P_civilian` 150 + `P_waste` 40). Root cause is proven, not guessed: from
the trace of an s1 run, drone 5 declared entity 24 (a *neutral*) as `enemy` at
t=3.8 and drone 1 declared entity 22 (a *neutral*) at t=4.2 — both **before the
first hostile even spawns at t=8.0** — and those two drones are exactly the two
the report lists as lost to `pair_neutral` at t=7.74 and t=8.41.

The chain is: `belief.cpp:157`'s 24 m miss gate calls a civilian hostile →
`policy.cpp:66` commits on our own belief alone → `flight.cpp:74`'s 4 m
separation margin is too small to prevent the contact. Fix the *first* link
first; it is the cheapest and the other two stop mattering as much.

**G1b — stop colliding with each other.** `pair_friendly` on 3 of 8 (s2: 4
drones, x2-b: 6 drones, x1-a: 2). −80 a collision. See `separation_margin` in
`notes/provenance.md`: 4 m cannot arrest a 16 m/s closure with 6.7 m/s² of
lateral authority — that needs ~19 m — but 19 m is incompatible with a 16-drone
ring of radius 75 m, where neighbours sit 29 m apart. **The margin and the ring
geometry have to be chosen together.** That trade is a DESIGN.md paragraph.

**Done when, as report numbers:**
- `mission.civilians_lost == 0` on s0, s1, x1-a, x1-b, x1-c
- `losses_by_cause` has no `pair_friendly` on s1, s2, x1-a, x2-b
- s1 total ≥ **−1170** (i.e. at least matching the do-nothing example)

**Test on:** s1 first (it has the reproducible failure), then s0 (must stay
clean and positive — it is the sanity check), then `-Tier 1 -Count 6`.

---

## G2 — Awareness: 60 points that need no intercept (1–2 h) ★ cheapest score

`score.awareness` is **0.0 on every scenario measured**, including s5. It is
worth up to 60 and comes from `declare_track` alone — no kill, no interception,
no protocol. Per §9.3, once a second, for each aircraft, only your most recent
declaration counts: correct +1, wrong −2, unknown 0, averaged and clamped to
[0, W_aware].

The clamp is why you currently score 0 rather than a negative: on x1-b the raw
average is (172 − 2×1016)/5865 = −0.32, clamped to 0. **A brain that declares
nothing scores exactly the same 0.** So today's declarations are pure downside —
they cost nothing directly but they drive the commit rule that kills civilians.

Two moves, in order:

1. **Declare `UNKNOWN` unless confident.** Wrong costs double what right earns,
   so the break-even is 2:1. Currently you are at 1:6 (172:1016 on x1-b). Being
   quieter is worth points immediately.
2. **Declare friendlies.** They are 16 of 24 entities on s1 and you *know* who
   they are — a peer's heartbeat carries its claimed position, and a track
   sitting there is a friendly. This is `TODO(next)` at `brain.cpp:89` and
   `belief.cpp:177`, and it is the largest block of free accuracy on the board.
   Note that calling a compromised friendly "friendly" is scored as incomplete
   rather than wrong, so it is not a trap.

**Done when:** `score.awareness > 0` on s1 (any positive number proves the sign
flipped), and `correct_declarations > 2 × wrong_declarations`. Target 20–40.

**Test on:** s1, x1-a, x1-b (x1-b currently has the worst ratio at 172:1016).

---

## G3 — Actually stop hostiles (3–4 h) — the biggest single term

`W_kill` 100 per hostile, scaled by how early; `P_breach` −200 for each one you
let through. Every hostile converted from breach to early kill is worth up to
**300 points of swing**. Currently 0–1 of 3–6 per scenario.

Do it in this order:

1. **Confirm the intercept geometry works at all.** `ProNav` is in
   `flight.cpp:50` and is standard proportional navigation. Watch one commit in
   the viewer end to end.
2. **Fix the commit rule** (`policy.cpp:66`). Today `closing > -2.0 || ttg < 12`
   is an `||`, so it commits to stern chases that `flight.h:31` correctly says
   never converge — both airframes share the same 6.7 m/s² bound.
3. **Only then tune the classifier's sensitivity upward** — G1 made it
   conservative; now you need it to still catch real hostiles in time. This
   tension is the heart of tier 1 and is worth a DESIGN.md paragraph in itself.

**Done when:** `hostiles_destroyed ≥ 3` of 6 on s1 with
`civilians_lost == 0`, and s1 total > 0.

**Test on:** s1, then `-Tier 1 -Count 6` — the generated layouts are where an
over-fitted intercept falls apart.

---

## G4 — Allocation: one drone per hostile (1–2 h)

`policy.cpp:73` admits it: every drone that can see a hostile commits to it.
That is the brief's named failure mode — "three drones per hostile intercepts
well and then loses the spare two to each other and to the debris" (§9.2) — and
it is where the remaining `pair_friendly` losses will come from once G3 works.

The mechanism is half-built: `ClaimMsg` is defined and *sent*
(`policy.cpp:188`) but the receiver does nothing with it (`brain.cpp:105`,
`TODO(B3)`). The cheap version needs no negotiation at all: a deterministic
tie-break on (target position, drone_id) that every drone computes identically.

**Done when:** `pair_friendly` is 0 across a tier-1 sweep while
`hostiles_destroyed` does not drop.

---

## G5 — s2: make information travel (2 h)

s2 is where the brief says the core of the challenge is (§8, §14 step 7).
Hostiles arrive far from whoever first sees them, so a report that stops at one
hop never reaches the drone that can act.

`comms.propagation_p95_s` is **`null` for this brain** and `0.09` for the
flooding example — nothing you send crosses the fleet. That is `TODO(B4)` at
`brain.cpp:115`. You have budget to spare (704 of 4096 B/s), so a hop-limited
forward with a budget check is affordable; the example's naive flood is not what
you want, but it proves the mechanism.

**Done when:** `propagation_p95_s` is a real number under ~1.0 s on s2, and s2
`hostiles_reached_asset` drops below its baseline of 5.

---

## G6 — Honest assessment of the top of the ladder

**Out of reach in the time remaining, and I would say so rather than gesture at
it.** §12 asks you to tell them what you did not get to; §8 says depth on a
subset beats breadth.

- **s3 / s4 (crypto, replay, eavesdropping).** The primitives are all there in
  `SwHost` and the protocol is already versioned from the first byte, so the
  design is ready — but signing, freshness windows and key rotation is a day on
  its own. The `TODO(tier 3)` notes at `brain.cpp:69-77` already describe the
  cheap non-crypto defence (compare the claimed position against the receiver's
  *measured* range, which a replay from the wrong side of the arena fails), and
  writing that reasoning up is worth more than a half-built handshake.
- **s5 (the insider).** `declare_identity` is **never called**, so detection
  scores 0 by construction — confirmed on s5: 1 compromise, 0 detected. If you
  have 45 spare minutes, the highest-value tier-5 work is *not* a detector, it
  is the `MsgType::Accuse` plumbing plus a written policy for what naming
  someone actually changes, because §7 is explicit that a fleet which keeps
  relaying a named insider's traffic "has changed a line in the report and
  nothing else". **Do not ship a half-tuned detector**: `P_false_accuse` is 120
  per identity and that term is *not* clamped, so a jumpy detector can run up an
  unbounded bill. Silence scores 0; a wrong accusation scores much worse.

---

## Sequencing summary

| | Goal | Hours | Expected s1 total after |
|---|---|---|---|
| G0 | Baseline + determinism | 0.5 | −1546.9 (measured) |
| G1 | Stop civilian and friendly collisions | 2–3 | ≈ −1170 |
| G2 | Awareness positive | 1–2 | ≈ −1140 |
| G3 | Intercepts working | 3–4 | > 0 |
| G4 | Allocation | 1–2 | higher, and steadier across layouts |
| G5 | s2 relay | 2 | s2 specifically |
| G6 | Write up the gaps | 1 | — |

If you run short, **stop after G3 and write up properly**. A brain that reliably
stops hostiles on tier 1 with an honest document beats a brain that gestures at
tier 5 and cannot explain its own constants.
