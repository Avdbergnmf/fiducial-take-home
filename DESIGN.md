# Design

<!--
Skeleton only. Every line below is a prompt to myself, not content. Delete the
prompts as each section gets written. Target 2-5 pages; this document is graded
as heavily as the code, so it gets real time, not the last hour.
-->

## Overview

- What does the brain do, in five sentences, for someone who has not read the code?
- What is the single design decision this whole document is defending?

## Module structure and why

- What are the boundaries between flight, protocol, policy and belief, and what
  exactly crosses each one?
- Which module owns time, and which ones are pure functions of their inputs?
- Does this split survive the tier-5 insider problem, or does the insider case
  force policy and belief to merge? Say which, and why.
- What did I try first and throw away?

## Wire protocol

- What is on the wire, field by field, and what is deliberately not on it?
- How is a version negotiated, and what happens when two versions meet?
- What does a receiver do with a message it does not understand — drop, relay, or
  quarantine?

## How I decide what an aircraft is

Classification is a 3D miss, not a heading. Alignment and closing in the
horizontal plane look the same for a dash at the origin and a civilian chord
that happens to point that way; the discriminant is whether closest-approach
to the origin is a sure hit (< 5 m) or has shrunk by more than sensor noise
while still inside `asset_radius`. A level overflight's miss is its altitude
and does not shrink. A hostile dive's miss goes to zero. That is G1, and it
is deliberately conservative: a false Hostile is a drone spent on a
bystander (−150) plus a hole in the ring for the next real inbound.

A heartbeat that matches a local sensor track latches `Friendly` so an
interceptor on the way in — same alignment, same closing, same miss — is not
re-classified as the thing we are defending against.

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
Hostile, the unique facing ring slot (first live drone clockwise if that
slot's heartbeat is gone — one successor, not both neighbours), relative
closing ≥ 1 m/s, and arrive before the cylinder. A Friendly already flying
at that hostile is the interceptor; the farther drone aborts. Pickets step
off the owner's corridor, and the interceptor does not cancel ProNav to
dodge them. ProNav itself is not the leak — the first s1 intercept finished.
A 6 s-old Hostile latch is wreckage; chasing those is how a spent sector
missed the next inbound. Raising classifier sensitivity is the remaining knob, and it is the
expensive one. Every 0.1 s taken off `kEvidenceForCall` is 1.6 m more
intercept window and a civilian chord that looks 0.1 s more like a dash. That
is not a tuning detail. It is the brief's own contradiction — telling a
threat from a bystander, and paying one drone per kill and not two — written
as a number.

## How I treat a peer I cannot verify

- What is the default posture towards an unverified peer: trust, ignore, or use
  with discount?
- What can an unverified peer still usefully contribute?
- Where is the line between "not yet verified" and "failed verification"?

## What I do about a compromised member

- How is a member named as compromised, and by whom?
- What happens after naming? Note: continuing to relay its traffic after naming
  it is not a response — say what actually changes.
- Is the action reversible, and what would reverse it?
- What stops a healthy member from being named by a malicious one?

## Bandwidth policy

- What is the budget, and what is sent when the budget is tight?
- What is dropped first, and who decides?
- How does the policy degrade — gracefully, or off a cliff?

## Testing approach

- What is tested deterministically, and what is only tested by running scenarios?
- What does the fixture trace prove?
- How do I know a change is an improvement and not noise?

## What I would do with another week

- The three things, in priority order, with the reason each is not done.

## Known gaps and what I did not get to

- Be specific and honest. A named gap costs less than a discovered one.

---

# Notes for DESIGN.md

Drop these in as sections. Trim to taste — they are written to be pasted, not
edited heavily.

---

## Viewer pipeline

```
swarm_sim --trace run.jsonl  ->  build_viewer_data.py  ->  run.bin + run.meta.json  ->  Unity
```

The Python sidecar owns all data work: parsing, delta expansion, coordinate
conversion, event derivation. Unity owns rendering and interaction only — it
never parses the trace, never sees NED, never expands a delta.

The split is deliberate. Coordinate handedness and delta accumulation are the
two things that cost hours when they go wrong, and they are far cheaper to debug
against a matplotlib plot than inside a game engine. It also keeps the viewer
replaceable: the same two files would feed a three.js front end unchanged.

### Why two files

A run has two shapes of data and they want different treatment.

Geometry is dense and uniform — every entity has a position, velocity and
attitude at every frame, always the same 11 floats. That goes in `run.bin` as a
fixed-stride binary array, frame-major. Roughly 600,000 numbers for a four-minute
run: 2.6 MB binary against 15–20 MB of JSON, loaded with one `ReadAllBytes` and
one block copy instead of a parse.

The decisive property is not size but random access. Any sample is arithmetic:

```
offset(frame, slot) = ((frame * slot_count) + slot) * stride
```

One multiply and an add, so seeking backwards costs exactly what playing forwards
costs. Scrubbing a timeline is the core interaction of this viewer, and a format
requiring a parse or a scan would make it stutter.

Everything else — events, logs, radio links, belief changes, telemetry, the score
report — is sparse and irregular: variable-length text, different fields per
record, absent at most timestamps. That goes in `run.meta.json`, where JSON's
overhead is irrelevant at a few thousand records and its flexibility is the point.

`run.meta.json` is also the instruction manual for `run.bin`: it declares
`stride`, `frame_count` and `slot_count`, so the reader is told the layout rather
than assuming it. Unity validates `bin_length == frame_count * slot_count *
stride * 4` on load, which turns a stale `.bin` beside a fresh `.meta.json` into
an immediate error instead of silently wrong geometry.

**Known gap:** stride says how many floats, not which. The field order lives in
FORMAT.md and in comments on both sides. A `columns` array in the meta would make
the layout fully self-describing — which is what the simulator's own `header`
record does for frame rows, and it is the better design. Not done for time.

---

## Forward compatibility

The same rule applies to both formats I control, the wire protocol and the viewer
format: **a reader must tolerate fields it does not know, and a writer must never
silently change the meaning of an existing one.**

On the wire, that is a version byte first and a message-type byte second, with a
frame whose version does not match being dropped rather than guessed at.

In the viewer format, it is that readers treat a missing field as *unknown*, not
as zero. The distinction matters more than it looks. A `kill_radius` absent from
an older meta file, defaulted to `0`, produces a viewer that draws a
zero-radius sphere and separation logic that believes nothing can ever collide —
wrong, and invisible. Treated as unknown, it falls back to a configured value and
says so. The failure being avoided is a default that looks like a measurement.

The same reasoning is why `Config` in the brain reads every mission constant from
`SwBootInfo` rather than carrying defaults. A plausible wrong constant is worse
than a loud absence, and every one of those values varies between missions.

### Provenance

Each `run.meta.json` records where it came from: source trace, scenario id, brain
binary, simulator version, and generation timestamp. Most of it is copied
straight from the trace's own `header`, which already carries it.

The reason is mundane and immediate — after a day of sweeps there are a dozen
`.bin` files and a filename does not say which scenario or which build produced
one. A run that describes itself removes a category of mistake rather than
requiring discipline to avoid it.

---

## Validating a loaded run

Coordinate bugs are silent. Nothing throws, nothing renders red; the picture is
simply wrong in a way that can go unnoticed for an hour. So the viewer validates
every run at load, in one pass that reports all problems together rather than
throwing on the first.

Checks fall into three tiers by where their truth comes from:

**Universal** — properties of a correct conversion, true for any run and needing
no configuration. Altitude positive at t=0 (NED z is down, so a negative Unity y
means the conversion was applied twice or not at all); attitude quaternions
unit-length; no NaN or infinity; slot indices matching array positions; entity
lifetimes within the frame range.

**Self-describing** — read from the meta file, which already carries arena
bounds, asset, fleet size and duration. Positions inside the arena, friendly
count matching fleet size, frame count consistent with duration times trace rate.

**Behaviour expectations** — opt-in per run. "Friendlies hold a 60 m ring at 30 m"
is a property of the *example brain* on s1, not of s1 itself; my own brain breaks
formation to intercept and the assertion stops holding. An earlier version keyed
this off the filename `fixture`, which conflated a name with a property. It is now
an explicit toggle, off by default.

**What this cannot catch:** a quaternion can be unit-length and still rotated
wrongly. No automated check finds a handedness error. That is verified manually —
a gizmo drawing an entity's forward vector against its velocity vector, which
should agree in straight transit — and it is documented as a manual step rather
than pretended to be covered.