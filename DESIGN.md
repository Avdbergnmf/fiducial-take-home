# Design

## Overview

Every drone holds a picket slot and spends itself on at most one inbound it
can catch before the asset cylinder. Classification is a 3D miss. Allocation
is radio-free: unique facing owner, then a handoff to whoever can actually
finish, then a sitting-wall check so a parked interceptor is not rammed from
behind. The ring is sized for radio, thinned over the live roster, and shrunk
until the unique-owner inbound at picket height still has leftover reach.

On the eight named scenarios (`s0, s1, s2, x1-a, x1-b, x1-c, x2-a, x2-b`)
the live brain is mean **152.3**, worst **x1-a −7.7** (two civilians, not
a breach; D44 stays). s0 **96.6**, s1 6/6 **188.0**, s2 6/6 **180.5**,
x1-b **237.6**, x1-c **136.3**, x2-a 4/4 **201.2**, x2-b 3/3 **185.6**.
Detection is unused: `declare_identity` is never called. The git ladder
and plots are in RESULTS.md and `notes/ablation-*.png`.

The decision this document still defends is the catchable-only commit rule.
It turned s1 from 1 kill and 5 breaches into 6/6. Hops, hopped heartbeats,
handoff, orbit, and closed-cover slack are what then closed s2 / x2-b
without undoing that bar. Classification is a 3D miss, mates are
heartbeat-matched, and a second drone on the same inbound is traffic unless
it is clearly the better shot.

s0–s2 are the job that was finished. Encryption, overheard traffic, and
insiders are listed in `notes/GAPS.md` rather than gestured at in the
binary.

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
`Outbox` and `SeenSet` are buffers. Mission constants come from `SwBootInfo`
once, in `Config::From` — including `sense_radius`, which varies between
generated missions and is **not** the 60 m s1 default (D74).

**What I threw away.** The skeleton used a 24 m miss gate, committed on
`closing > -2 || ttg < 12`, and let every drone that could see a hostile go.
Those three local decisions chained into civilians dead before t=8 on s1.

**Where the split does not hold.** A tier-5 insider is about *peers*, not
*aircraft*. It is not a class in `belief` and not a stance in `policy`. A
per-peer trust model is a fifth module. I have not built it. The `Accuse`
type and the `declare_identity` hook are reserved; they are empty.

## Wire protocol

Little-endian, bounds-checked, versioned from byte 0. Explicit shifts, not
`memcpy` of a struct. `test_protocol.cpp` covers round-trip, truncation, wrong
version, unknown type, overflow, and garbage that must not read past `len`.

**Header (10 bytes):** version, type, origin, hops, seq, sent_time.
`hops` is incremented by each relay; origin/seq/`sent_time` stay the author's.
Cap 4 on TrackReport, Ray, and Heartbeat. Hop-0 heartbeats are still identity
(`MarkFriendly`); relays only update the live roster.

- **Heartbeat** — claimed position and velocity, 0.125 m quantised. Identity
  on hop 0; hopped so the far side of the ring shares who is still alive.
- **Track report** — pose, belief, confidence. No `track_id` (observer-local).
  Association is geometry plus time. Says *there is a Hostile here*, not *I
  am going after it*.
- **Claim** — defined, **not composed**. Unread claims won the one frame per
  tick; interceptors went silent; neighbours stacked. Allocation is
  radio-free. Death is silent: a drone that claimed and then died never
  retracts. The ram itself times out; UniqueOwner does not ask permission.
- **Accuse** — type 4 is accepted by the header and dropped. No payload.
- **Ray** — fitted inbound cone `(h0, slope, weight)`. Seer originates;
  everyone else relays. Fleet geometry, not an assignment.

Writer stamps `kProtocolVersion = 1`. Reader refuses any other version, any
type outside 1–5, and any short frame. No negotiation. A frame at the antenna
proves only that something transmitted it.

**s4 — hostiles listen.** Everything we send is in the clear and in range of
the inbound. Traffic that names which drone is going after which target is
usable against us. That is why Claim stays off the wire: UniqueOwner is a
local function of bearing and the live roster. Hopped heartbeats keep that
roster in agreement across the radio horizon so s2 can close. The cost is
leaking *that a friendly is still alive somewhere*, plus TrackReport and Ray
leaking *where we think a Hostile is* and *how high the fence sits*.
Closer-chaser abort is the backstop if two drones still disagree for a tick.
Encryption is a later-tier job; the trade-off is the design, not a missing
flag.

**Loss and latency are unpublished.** Each drone measures them on **hop-0**
frames: sequence gaps per origin are loss; `obs.time − sent_time` is delay.
A seq jump larger than 32 is leaving range, not a burst of loss. Relays are
ignored — they are not the radio to the author. Stale TrackReport / Ray drop
uses `40 × mean latency`, clamped to [0.5, 2] s. We do not retransmit:
TrackReport already hops and bursts after a call.

Stance is not on the wire. Logs stay `verb k=v`; the viewer turns them into
English. `log` is not scored; `Decide` / `Fly` never read it.

## How I decide what an aircraft is

Alignment and closing in the horizontal plane look the same for a dash at the
origin and a civilian chord that happens to point that way. The discriminant is
**3D closest-approach miss**, not a heading and not a 24 m gate.

- **Sure-hit or shrink.** Hostile if miss < 5 m, *or* miss has dropped ≥ 3 m
  since first sight while still inside `asset_radius`. The 24 m gate called
  civilians `enemy` before t=8 on s1.
- **Miss is 3D; alignment stays horizontal.** A 5.2 m ground chord, level at
  30 m, is an overflight. A dive at the origin has miss ~0.
- **The asset is a cylinder.** A hostile breaches when ground range hits
  `asset_radius`, at whatever altitude. `TimeToCylinder` is the breach clock.
- **Declare only what we would bet at 2:1.** Awareness samples the most
  recent `declare_track` once a second. Local tracks only. Friendly from a
  heartbeat match; Hostile we actually see; everything else UNKNOWN (naming
  wreckage was −2, measured).

A heartbeat that matches a local sensor track latches `Friendly` so an
interceptor on the way in is not re-classified as the thing we are defending
against. Ballistic wreckage is checked first and still wins.

`kEvidenceForCall` is 0.6 s of aimed geometry. That is measured (D36): the
working window is 0.58–0.60 s. On a clean layout like s1, each 0.1 s is ~5
points of earlier `W_kill`. Early scramble (D44) leaves the ring *before*
the Hostile latch when the ground track has already crossed the cylinder
for 0.1 s — that is how some generated inbounds that never wait 0.6 s still
get a ram. It also costs x1-a two civilians. That trade is frozen.

## When I spend a drone

`W_kill` is 100, scaled by how early the intercept is; `P_breach` is −200; a
civilian ram is −150. The same airframe is the weapon and the liability.

Commit: a *fresh* Hostile (call younger than 6 s) — local or fused from a
TrackReport — the unique facing ring slot, relative closing ≥ 1 m/s, and
arrive 0.5 s before the cylinder. Sense range is not a gate: on s2 the owner
hears the inbound 100 m out. After a nearby death the survivors re-space on
the same radius so the next dash on that bearing is not met from a hole.
Opposite-side radio loss is not a death.

**Handoff, not just facing.** Facing-slot ownership with an orbiting ring
hands the inbound to a drone whose tangential velocity is square across the
corridor — the worst person to accelerate from rest. Stations orbit at
ω = 0.06 rad/s (shared function of sim time, nothing on the wire). A
challenger with a better `InterceptScore` takes the inbound. Approaching
incumbent: must win by 0.25 s of score. Receding incumbent, same t_go bin:
also if closing faster by 0.5 m/s. Orbit without that handoff is a cost
(D23). Leftover Reach vs ω: `notes/orbit-cover-tradeoff.md`.

**A sitting wall is a duplicate.** A parked facing picket has toward ≈ 0
along the LOS (orbit tangent ⊥ inbound). Lowering the chase threshold to
treat them as interceptors breaks s1/s2/fa56 — every parked neighbour looks
busy. Instead: Friendly on the live ring, closing, toward ≥ −5, **≥ 12 m
closer**, skip the named facing incumbent. The outer seer on s2 is not a
wall.

Pickets step off the *remaining* intercept flight, not the whole
slot-to-hostile chord. A 6 s-old Hostile latch is wreckage.

## The ring

Radius starts at `asset + 0.625 · comm` (neighbour chord inside radio; D21
does not grow R). Caps: inside the hostile spawn circle, and a reaction
margin so a dash from spawn is still catchable. As the fleet thins, preserve
the full-fleet chord, then cap adjacent stations inside one **live**
`sense_radius`. Then shrink until leftover Reach on the unique-owner
inbound at picket height is ≥ 5 m, falling back to leftover ≥ 0, else leave
the caps (D58 / D70 / D73). Horizon (el = 0) is not AND-ed into that test:
a six-picket ring never catches a ground-level bisector, and that abort
leaves the radio radius.

Altitude is a fitted inbound ray at this radius, else **20 m**, never below
`max(6, R·tan(10°), ½ first-sight Reach)` and never above 30 m. 15 m
re-opens a named canary; 18 m costs s0 awareness; 20 m is the first height
that pays `W_kill` on s1 without those (D72).

`sense_radius` is read once from boot and used in first-sight leftover,
the thinning cap, and the pancake floor. It is not sampled from
`--dump-params` and baked in (D74).

## How I treat a peer I cannot verify

The brief does not answer this. The default here is **use with a discount**.
A fused Hostile is evidence UniqueOwner may spend itself on. It is not a
scoring declaration (`declare_track` is still local-only).

What we trust because we measured it: RF range and bearing on the received
frame. What we do not trust: the payload.

- A heartbeat is a mate only if `|claimed_range − measured_range| ≤ 3σ + 2 m`,
  and the nearest *local* sensor track is within 8 m of the payload
  extrapolated by `now − sent_time`. Hold 2.5 s. A replay from the wrong
  side of the arena fails the range check — that is the start of a tier-3
  defence, used early because it is free. s1 hostiles do not transmit.
- A track report is associated by pose, not `track_id`. Hopped reports have
  no measured range to the *author*; they are trusted on freshness and
  geometry. That is the s3 hole.
- A fused Hostile is a commit if UniqueOwner can catch it. Hearsay used to
  sit at `track_id` 0 and park a picket on the ring; policy now keys on a
  store-local id.

Failed verification is a range mismatch: we ignore the heartbeat, we do not
latch Friendly, we do not accuse.

**Cost I accepted.** A hostile that transmits a plausible heartbeat (tier 3
that also matches range, or a tier-5 insider) is marked Friendly and not
rammed. A hostile that transmits a plausible TrackReport can pull UniqueOwner
off the ring.

## What I do about a compromised member

**Not implemented.** `declare_identity` is never called. `MsgType::Accuse` is
dropped. Detection is 0 on every run by construction.

Naming someone has to change behaviour or it changed one line in a report.
If I named a peer I would, in the same tick: stop latching them Friendly,
stop merging their track reports, and not forward their frames. Relaying a
named insider is the thing the brief says is not a response.

I would not ship a jumpy detector: `P_false_accuse` is 120 per identity and
that term is **not** clamped. Silence scores 0; a wrong accusation is an
unbounded bill.

## Bandwidth policy

s1 budget is 4096 bytes/s rolling; s2 is 3072. One broadcast per tick. `Pump`
will not spend the last 64 bytes.

`Outbox<24>` is a priority queue, not FIFO. Heartbeat is priority 5; a hostile
track report is 3; a relay of someone else's report is 2. A full queue drops
the *lowest* resident, not the newest arrival — tested. Frames older than 2 s
expire unsent.

A TrackReport, Ray, or Heartbeat we have not seen is copied with `hops++`
and queued if `hops+1 ≤ 4`. Hop-0 heartbeats still `MarkFriendly`; relays
only update the live roster. Compose reports local Hostiles only; hearsay
rides the author's frame.

`propagation_p95_s` is the hop metric: a real number means a frame crossed
more than one radio range. It is not scored; on s2 it is the difference
between an intercept and a breach.

Logs are host-side, transitions only — class, commit/abort/picket, proximity,
one `params` line at boot. Per-tick logs would overflow (`drone == -1`).

## Testing approach

- **`ctest`:** `test_protocol` (truncation, version, garbage, outbox
  priority/expiry, seen-set, relay stamps), `test_policy` (facing slot, unique
  owner, live ring, yield corridor, closed cover, sitting wall, default
  picket 20 m), `test_belief` (classifier geometry plus peer-report
  association by pose, not track_id).
- **Determinism:** `scripts\determinism.ps1` — `--threads 1 --record` then
  `--threads 8 --replay`. Compute timings stripped (not scored).
- **Improvement vs noise:** `iterate.ps1` on one id (`runs/history.csv` via
  `scripts\history.ps1`), then `sweep.ps1` on the same eight. The graded
  number is the **worst** total, not the mean. The git ladder is
  `scripts\ablation.ps1` + `scripts\versions.csv`, plotted into `notes/`.

## What I would do with another week

The honest list is `notes/GAPS.md`. The next useful days, in order:

1. **s3 range-vs-claim as a real gate on hop-0 TrackReport.** Heartbeats
   already compare claimed range to measured range. A hopped report has no
   measured range to the author. Freshness from `now − sent_time`, and refuse
   a hop-0 report whose sender pose fails the same check. Crypto is a day on
   its own.
2. **Insider residual.** The origin stamped on a fused track is the start of
   a per-peer model. Naming someone has to change forwarding and merge, not
   just a report line. `P_false_accuse` is unclamped, so the detector has to
   be boring.
3. **The remaining cover hole** (`x2-25893f`, 1/3 at every slack). Slack
   does not close it. That is a geometry we have not named yet, not a knob.

I would not spend the week reversing D44 (x1-a civilians) or AND-ing the
horizon cell into CoverCloses.

## Known gaps

See `notes/GAPS.md` for the deliverable list. In short: no crypto (s3–s5),
no insider handling, hopped TrackReports trusted, Claim unused on purpose,
x1-a two civilians by D44, one generated cover hole slack does not close,
a set of GUESS constants left for time (`notes/provenance.md`), and the
Unity backlog (1 m grid, compass, standalone player, multi-run compare).

s2 late pair and “x2-b never commits” are closed. They were true at V10.

## LLM use

Allowed, and asked for: a short account of what I used it for, and how.
This is that. The test is whether I can tell you which parts I could
rewrite from scratch.

**Harness and sidecar** (`scripts/`, `tools/build_viewer_data.py`). Largely
generated against a spec I wrote. I treated run output as the review, not
the draft. I could rewrite these. I already had to, in pieces, to make them
true.

**Brain.** The *decisions* are mine: sure-hit-or-shrink, friends-only then
local Hostile on the scoring hook, catchable commit, one owner clockwise,
hops, hopped heartbeats, handoff/orbit, closed-cover slack, sitting wall
(D2, D9, D11, D15, D18, D56, D66, D70, D71). Most of the C++ was written by
an LLM from those instructions. A lot of it was already right when I
measured it, and I kept it. That is specify, generate, sweep, keep or
reject. I can reconstruct the rules from scratch. I would not type
`belief.cpp` from memory.

**Constants.** A deliberate LLM pass over the source, then checked against
`--dump-params`. That is `notes/provenance.md`. Guess vs measured is the
point of that file. Leftover guesses are in GAPS, not silently treated as
measurements.

**Viewer.** I can do this from scratch. Agents and Unity's AI assistant made
it take hours instead of days. Interaction and layout I would write myself
if I had to; the glue around UITK I would rather not.

**Not trusted: anything that asserts a scoring rule.** Wreckage on
`declare_track` was settled by a controlled run rather than by asking.
Weights come from CHALLENGE.md §9 and the report JSON, not from a model.
