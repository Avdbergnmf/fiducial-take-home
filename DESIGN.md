# Design

## Overview

Every drone holds a picket slot and spends itself on at most one inbound it can
catch before the asset cylinder. On eight named scenarios the worst score is
**−524.6** (x2-b: 0 of 3 stopped, three breaches), the mean is **−136.4**, the
best is **+158.4**. s1 — the layout we iterated on — is 6/6, **+130.0**,
awareness 54.4 of 60, comms 35.5 of 40; detection is unused because
`declare_identity` is never called. The decision this document defends is the
catchable-only commit rule: it turned s1 from 1 kill and 5 breaches (−947) into
6/6, and it is also why x2-b got 204 points worse. Classification is a 3D miss,
mates are heartbeat-matched, and a second drone on the same inbound is traffic,
not firepower.

## Module structure and why

A new requirement should land in exactly one module.

| Module | Owns | Must not know about |
|---|---|---|
| `belief` | what an aircraft *is* | the radio format, how to fly |
| `flight` | how to get somewhere | what anything is, what it means |
| `protocol` | bytes on the wire | tactics, geometry |
| `policy` | what to *do* | byte layout, control laws |

`brain.cpp` is glue. The tick order is the architecture:

1. own sensors (`TrackStore::Update`)
2. inbound radio (`ConsumeFrames` → `MarkFriendly` / `MergePeerReport`)
3. decide, declare
4. outbound radio (`Compose`, then one `Pump`)
5. fly

Own sensors go in first so hearsay is always matched against a fresh local
picture. That order is load-bearing: the first `Classify` of a tick can still
call a mate Hostile; `MarkFriendly` overwrites it before policy commits.

`obs.time()` is simulator time. Flight helpers are pure. Belief and policy
hold the surviving state (tracks, stance, last-heard). The codec is pure;
`Outbox` and `SeenSet` are buffers. Mission constants come from `SwBootInfo`,
not from defaults that look like measurements.

**What I threw away.** The skeleton used a 24 m miss gate, committed on
`closing > -2 || ttg < 12`, and let every drone that could see a hostile go.
Those three local decisions chained into civilians dead before t=8 on s1, and
a score 380 points worse than the hover example.

**Where the split does not hold.** A tier-5 insider is about *peers*, not
*aircraft*. It is not a class in `belief` and not a stance in `policy`. A
per-peer trust model is a fifth module. I have not built it. The `Accuse` type
and the `declare_identity` hook are reserved; they are empty.

## Wire protocol

Little-endian, bounds-checked, versioned from byte 0. Explicit shifts, not
`memcpy` of a struct. `test_protocol.cpp` covers round-trip, truncation, wrong
version, unknown type, overflow, and garbage that must not read past `len`.

**Header (10 bytes):** version, type, origin, hops, seq, sent_time.

- **Heartbeat** — claimed position and velocity, 0.125 m quantised. Identity.
- **Track report** — pose, belief, confidence. No `track_id` (observer-local).
  Association is geometry plus time (D14).
- **Claim** — defined, **not composed**. Unread claims won the one frame per
  tick; interceptors went silent; neighbours stacked (D11). Allocation is
  radio-free (D15).
- **Accuse** — type 4 is accepted by the header and dropped. No payload.

Writer stamps `kProtocolVersion = 1`. Reader refuses any other version, any
type outside 1–4, and any short frame. No negotiation. A frame at the antenna
proves only that something transmitted it.

Stance is not on the wire. It is piecewise constant and already logged as
`commit` / `abort` / `picket`; a 100 Hz dump would burn the log budget. The
viewer reconstructs it from those verbs and treats silence as Forming (D6).
Log lines stay `verb k=v`; the viewer turns them into English (D4). On one s1
recording: 1,184 log records, 8% of the trace, no overflow. `log` is not
scored; `Decide` / `Fly` never read it.

## How I decide what an aircraft is

Alignment and closing in the horizontal plane look the same for a dash at the
origin and a civilian chord that happens to point that way. The discriminant is
**3D closest-approach miss**, not a heading and not a 24 m gate.

- **Sure-hit or shrink (D2).** Hostile if miss < 5 m, *or* miss has dropped
  ≥ 3 m since first sight while still inside `asset_radius`. The 24 m gate
  called civilians 22 and 24 `enemy` before t=8 on s1 — both rammed. Shrink-only
  rejects s1 hostiles, who spawn already aimed so miss never shrinks from zero.
  After D2, `pair_neutral` was gone on 6 of 8 scenarios.
- **Miss is 3D; alignment stays horizontal (D7).** x1-a civilian 28 was called
  at t=5.57 (`miss=5.2`) and rammed: a 5.2 m ground chord, level at 30 m. 3D
  miss on that overflight is ~altitude and does not shrink. A dive at the
  origin has miss ~0. Alignment stays in the ground plane on purpose.
- **The asset is a cylinder (D10).** s1 breaches vanish at ground range 30 m
  and altitude ~6.75 m — a vertical wall, not a sphere (that crossing would be
  ~0.10 s later). `TimeToCylinder` is the breach clock. 3D miss is stricter
  than the breach test. Not seen inverted on s1.
- **Declare only what we would bet at 2:1 (D9).** Awareness samples the most
  recent `declare_track` once a second: correct +1, wrong −2, unknown 0,
  clamped to [0, 60]. Local tracks only. Friendly from a heartbeat match;
  Hostile we actually see; everything else UNKNOWN (naming wreckage was −2,
  measured). Intercept still uses `Track.belief` — `score.mission` did not
  move when we changed what we published. After D9, s1 awareness 51.9
  (4146 : 3); x1-b 47.3 (4409 : 0), was 172 : 1016 then clamped to 0.

A heartbeat that matches a local sensor track latches `Friendly` so an
interceptor on the way in — same alignment, same closing, same miss — is not
re-classified as the thing we are defending against. Ballistic wreckage is
checked first and still wins.

`kEvidenceForCall` is 0.6 s of aimed geometry. That is still a guess. Every
0.1 s off it is ~1.6 m more intercept window and a civilian chord that looks
0.1 s more like a dash.

## When I spend a drone

The classification bar and the commit rule pull in opposite directions, and
that tension is the whole of tier 1.

`W_kill` is 100, scaled by how early the intercept is; `P_breach` is −200; a
civilian ram is −150. The same airframe is the weapon and the liability. G1
requires 0.6 s of aimed geometry before we even *name* a Hostile. That 0.6 s
is 10 m of dash. On s1 the first look is at ~60 m of sense when the inbound
is still ~6 s from the cylinder; waiting 0.6 s still leaves a facing picket a
closing intercept. Waiting until they are inside the ring does not: relative
closing flips sign and we are in the stern chase both airframes' 6.7 m/s²
bound cannot win.

Commit used to ignore that. `closing > -2 || ttg < 12` spent drones on
outbound geometry ProNav cannot fly, and hearsay `track_id` 0 left them
"committed" on the ring with a null target. The rule now is: a *fresh* local
Hostile (call younger than 6 s), the unique facing ring slot (first live drone
clockwise if that slot's heartbeat is gone — one successor, not both
neighbours), relative closing ≥ 1 m/s, and arrive 0.5 s before the cylinder.
A Friendly already flying at that hostile is the interceptor; the farther
drone aborts. Pickets step off the *remaining* intercept flight — cruise ×
(time-to-meet + 0.5 s), capped at the 12 s abort — not the whole slot-to-hostile
chord. A picket sitting past the predicted ram is not traffic. The interceptor
does not cancel ProNav to dodge them. ProNav itself is not the leak — the first
s1 intercept finished. A 6 s-old Hostile latch is wreckage; chasing those is how
a spent sector missed the next inbound.

Raising classifier sensitivity is the remaining knob, and it is the expensive
one. The commit rule moved first (local, relative closing, catchable, one
owner, no ghost latches). Then `kEvidenceForCall` 1.2 → 0.6. On s1 that did
not cost a civilian; the sixth kill was the fresh-Hostile gate, not the 0.6 s.
Measured: s1 6/6, wasted 0, civilians 0, total **+130**. `pair_friendly` is 0
on the eight named scenarios.

## How I treat a peer I cannot verify

The brief does not answer this. The default here is **use with a discount**,
never as a commit.

What we trust because we measured it: RF range and bearing on the received
frame. What we do not trust: the payload.

- A heartbeat is a mate only if `|claimed_range − measured_range| ≤ 3σ + 2 m`,
  and the nearest *local* sensor track is within 8 m of the payload
  extrapolated by `now − sent_time` (D8). Hold 2.5 s. Classify will not demote
  `Friendly` while that hold lasts. A replay from the wrong side of the arena
  fails the range check — that is the start of a tier-3 defence, used early
  because it is free. s1 hostiles do not transmit.
- A track report older than 2 s is dropped. Younger ones are extrapolated the
  same way and associated at 8 m (D14). 12 m fused two aircraft; 4 m
  duplicated one (2,089 `call` lines, comms 26 vs 36). Two craft inside 8 m
  still merge.
- Hearsay never becomes an intercept. `MostUrgentHostile` requires a local
  track. A peer-reported Hostile at `track_id` 0 used to park a picket for 25 s
  through the next arrival.

A peer we can hear but not see is noted alive (UniqueOwner uses 1.5 s of
silence as death) and is not yet kept out of. At 1 m they are inside
`sense_radius`. Failed verification is a range mismatch: we ignore the
heartbeat, we do not latch Friendly, we do not accuse. "Not yet verified" is
simply no matching local track.

**Cost I accepted.** A hostile that transmits a plausible heartbeat (tier 3
that also matches range, or a tier-5 insider) is marked Friendly and not
rammed. Multi-hop is still missing; D14 only makes the one-hop fuse honest.

## What I do about a compromised member

**Not implemented.** `declare_identity` is never called. `MsgType::Accuse` is
dropped. Detection is 0 on every run by construction.

Naming someone has to change behaviour or it changed one line in a report
(CHALLENGE.md §7). If I named a peer I would, in the same tick: stop latching
them Friendly, stop merging their track reports, and not forward their frames
(we do not forward anyone's frames today). Relaying a named insider is the
thing the brief says is not a response.

I would not ship a jumpy detector: `P_false_accuse` is 120 per identity and
that term is **not** clamped. Silence scores 0; a wrong accusation is an
unbounded bill. Calling a compromised friendly "friendly" on `declare_track`
is incomplete rather than wrong — and it is also not detection.

## Bandwidth policy

s1 budget is 4096 bytes/s rolling; s2 is 3072. One broadcast per tick. `Pump`
will not spend the last 64 bytes, so a drone that has talked itself empty can
still report the thing that matters.

`Outbox<24>` is a priority queue, not FIFO. Heartbeat is priority 5; a hostile
track report is 3. A full queue drops the *lowest* resident, not the newest
arrival — tested. Heartbeat is highest because unread Claims used to starve it
and neighbours stole intercepts. Frames older than 2 s expire unsent.

s1 comms is **35.5 of 40**. Silence scores 0, so this is not "say nothing".
`propagation_p95_s` is `null`: nothing crosses more than one hop. That is not
scored; it is why s2 still takes three breaches.

Tight budget: reports wait, heartbeats still go. Tighter: `Pump` pauses until
headroom returns. No mute-by-policy cliff. Logs are host-side, transitions
only (D3) — class, commit/abort/picket, proximity 12/6/3 m, one `params` line
at boot. Per-tick logs would overflow (`drone == -1`) and drop the late
collision.

## Testing approach

- **`ctest`:** `test_protocol` (truncation, version, garbage, outbox
  priority/expiry, seen-set zero-collision) and `test_policy` (facing slot,
  unique owner is one drone clockwise, yield corridor is remaining flight
  not the full chord). Classifier geometry lives in `test_belief.cpp`; it is
  not on the ctest line.
- **Determinism:** `scripts\determinism.ps1` — `--threads 1 --record` then
  `--threads 8 --replay`, and two identical runs. Compute timings stripped
  (not scored).
- **Improvement vs noise:** `iterate.ps1` on one id, then `sweep.ps1` on the
  same eight. The graded number is the **worst** total, not the mean (§11.2).
  V8 is the exhibit: s1 stayed +130 while s2 / x1-a / x2-a each lost 200+.
  `history.csv` is the live iterate log; it is incomplete, which is why
  RESULTS.md rebuilt the ladder from git.

## What I would do with another week

1. **Multi-hop on s2.** It is the named core of the challenge and our
   floor-adjacent score (−501.9, 3 of 6, `propagation_p95_s` null). Hostiles
   arrive 220 m out; only one drone can see them. Forward with a hop limit and
   a budget check, not the example flood. Same gap on x2-a (−300.5, 2 of 4).
2. **s3 range-vs-claim as a real gate.** `HeartbeatPlausible` already compares
   claimed range to measured range. Size a freshness window from measured
   `now − sent_time`, and refuse a report whose velocity extrapolates the
   wrong way. Crypto is a day on its own; this is the defence the TODOs in
   `brain.cpp` already describe.
3. **A commit fallback when nobody qualifies.** x2-b is the floor (−524.6):
   0 of 3, three × −200, no drones lost — the fleet never commits. The rule
   that won +1078 on s1 cost 204 here. Someone has to go when the bar is
   empty, rather than everyone standing down.

## Known gaps

- **No relay.** Information stops at one hop. s2 and x2-a take breaches the
  fleet had the information to prevent.
- **x2-b never commits.** Strict catchable-local-fresh is a hole, not a
  tuning miss.
- **x1-a civilians.** 4/4 hostiles, 0 breaches, then −450 civilians and −40
  wasted. Best kill term in the sweep (+173.6). The discriminant still fires
  on a chord that passes very close to the asset. Awareness is also lowest
  here (35.3).
- **No insider handling.** `declare_identity` unused; Accuse unused; a
  plausible heartbeat marks the sender Friendly.
- **Claims unused.** UniqueOwner plus closer-chaser abort is the substitute.
  Two drones with disagreeing `heard_[]` can both think they own the inbound.
- **D13 local drop.** Tracks die with the sensor picture. Correct against an
  unpublished `track_drop_time`; it cost ~200 points each on s2, x1-a, x2-a
  until D15 recovered most of it.
- **`kEvidenceForCall` 0.6** is a guess. Tighter civilian chords are where it
  will show.

## LLM use

Allowed, and asked for: a short account of what I used it for, and how.
This is that. The test is whether I can tell you which parts I could
rewrite from scratch.

**Harness and sidecar** (`scripts/`, `tools/build_viewer_data.py`). Largely
generated against a spec I wrote. I treated run output as the review, not
the draft. Three real bugs came out that way: native stdout leaking into a
PowerShell return value, `-File` passing `s1,s2` as one string, and a
repeatability check that compared wall-clock fields and so could never pass.
I could rewrite these. I already had to, in pieces, to make them true.

**Brain.** The *decisions* are mine: sure-hit-or-shrink, friends-only then
local Hostile on the scoring hook, catchable commit, one owner clockwise
(D2, D9, D11, D15). Most of the C++ was written by an LLM from those
instructions. A lot of it was already right when I measured it, and I kept
it. That is not "rubber duck only" — it is specify, generate, sweep, keep
or reject. I can reconstruct the rules from scratch. I would not type
`belief.cpp` from memory.

**Constants.** A deliberate LLM pass over the source, then checked against
`--dump-params`. That is `notes/provenance.md`. Guess vs measured is the
point of that file.

**Viewer.** I can do this from scratch. Agents and Unity's AI assistant made
it take hours instead of days. Interaction and layout I would write myself
if I had to; the glue around UITK I would rather not.

**Not trusted: anything that asserts a scoring rule.** Wreckage on
`declare_track` was settled by a controlled run (same `state_hash`, +10
wrong declarations) rather than by asking. Weights come from CHALLENGE.md
§9 and the report JSON, not from a model.
