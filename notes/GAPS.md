# Gaps and backlog

What we did not get to. CHALLENGE.md §12 asks for this list. Depth on
s0–s2 was the choice (§8 / §14: *depth on a subset beats breadth*;
*only then consider authentication*).

Nothing here is a surprise we discovered late. Each item was visible in
the brief or in a measured hole, and was left on purpose.

---

## Later tiers (too much for this pass)

### Encryption / s3 — hostiles impersonate and replay

`sign`, `verify`, `agree`, `kdf`, `seal`, `unseal` are unused. Crypto
primitives are in the host; group/session keys are **not** on boot
(“deriving those is part of the problem”). Hopped TrackReports are
trusted on freshness and geometry. A replay with a plausible pose pulls
UniqueOwner off the ring.

Heartbeats already compare claimed range to measured range. That is the
start of a defence, used early because it is free. It does not authenticate
the payload and it does not apply to a hopped frame (no measured range to
the *author*).

### s4 — hostiles overhear

Claim stays off the wire on purpose (D11 / D15). Heartbeats, TrackReport
and Ray still leak liveness, believed Hostile pose, and picket height.
Encryption would shrink the leak; it would not remove the fact that a
visible ram is itself information. Closer-chaser abort is the backstop
when two drones disagree for a tick.

### s5 — insiders

`declare_identity` is never called. `Accuse` is dropped. Detection is 0
by construction. A plausible heartbeat from a valid key marks the sender
Friendly. `P_false_accuse` is 120 per identity and **unclamped**, so a
jumpy detector is an unbounded bill.

A per-peer trust model is a fifth module (not a class in `belief`, not a
stance in `policy`). Naming someone has to stop latching them Friendly,
stop merging their reports, and stop forwarding their frames — in the
same tick — or it changed a line in the report.

---

## Cheap leftovers we still would not spend the night on

These are small in code and large in judgement. They are on the list
because they are the next honest days, not because they were forgotten.

| Item | Why it is still standing |
|---|---|
| Hop-0 TrackReport range-vs-claim | Heartbeats already do this. A hop-0 report could use the same `|claimed_sender_range − measured|` gate. Cheap, s3-adjacent, can drop a valid report if the sender pose is coarse. |
| Range-vs-claim on hopped reports | No measured range to the author. Needs crypto or a multi-observer residual. Not cheap. |
| Adaptive `ω(R)` | D70: fat rings want slower, tight-sense wants more. Integrating ω while R shrinks phase-jumps the ring. Left at 0.06. Curves: `notes/orbit-cover-tradeoff.md`. |
| Horizon AND into `CoverCloses` | D58: a six-picket ring never tiles el=0. Would abort shrink and leave the radio radius. |
| Always-cover an 11 m inbound | 11 m at R=86 is 7.3°, under the 10° bracelet. Sitting at 11 m is under the D68 floor. Needs shrink-R or the rejected AND. |
| `x2-25893f` 1/3 | Same 2 breaches at slack 0 / 5 / 10. Not this knob. |
| Reverse D44 (x1-a 2 civ) | Frozen. Early scramble catches generated inbounds that never wait 0.6 s. |

---

## Measured holes we are living with

- **x1-a, 2 civilians.** All hostiles stopped, 0 breaches. Discriminant
  plus scramble still fires on a chord that passes very close to the
  asset. Awareness is also lowest here. Not a leak of the catchable bar.
- **Cover does not invent time.** If even the asset-cylinder floor is
  open, we leave the radio/spawn caps. Some generated layouts will still
  breach.
- **Two drones, one tick of roster lag.** UniqueOwner plus closer-chaser
  plus sitting-wall is the substitute for Claim. Hopped heartbeats keep
  that lag to a missed beat, not a radio horizon. Two drones can still
  both think they own the inbound for one tick.

Closed, so they are *not* gaps: s2 late pair (D56), “x2-b never commits”
(handoff + orbit + hops). Those sentences in the V10 DESIGN.md are stale.

---

## Guessed constants left for time

`notes/provenance.md` labelled every tuning number. The ones below are
still **GUESS**: no dump-params, no sweep, no derivation. They were not
costing the identity bar hard enough to spend a grid on, and several are
internally inconsistent (two clamps on the same variable). Interesting
to look at; not a reason to reopen the brain this week.

| Where | Name | What it is | Why it was left |
|---|---|---|---|
| belief.cpp | `kScoreDecay` 0.6 /s | Evidence decay vs +1.0/s accrual | Bias toward false positives; D36 measured the *call* window instead. |
| belief.cpp | ballistic lateral 4.0 m/s² | Wreckage vs powered dive | Plausible drag-only; no wreckage-only sweep. |
| belief.cpp | ballistic clamp 1.5 vs decay clamp 3.0 | Same variable, two caps | The 3.0 bound is unreachable. Harmless, sloppy. |
| belief.cpp | wreckage threshold 0.4 | Latch debris | Too high: fly into debris. Too low: descending friendly. |
| belief.cpp | `kSureHit` / `kShrink` 5 m / 3 m | Hostile miss gates | Shape is D2/D7; values were never re-gridded after 3D miss. |
| belief.cpp | alignment 0.8, closing 4.0 m/s | Aimed-at-asset extras | Civilians already clear 4 m/s. Discriminant is miss, not these. |
| belief.cpp | closing_score clamps 3 vs 4 | Same inconsistency class as ballistic | |
| policy.cpp | `kMinClosing` 1.0, `kCatchSlack` 0.5 s | Commit floor | D11; lowering catch-slack is late `W_kill`, not a named breach. |
| policy.cpp | `kRecommitHold` 2.0 s | Anti-flicker | Low-risk. |
| policy.cpp | slot-reached 8 m | Forming → picket | Low-risk. |
| flight.h | GoTo gains 0.8 / 1.6 | ζ ≈ 0.89 | Ratio is defensible; values are not. |
| flight.cpp | cruise 14 m/s, arena `kEdge` 20 m | Below max_speed; 10% border | Never left the arena on named ids. |
| brain.cpp | cruise/goto switch 25 m | | Low-risk. |
| protocol | confidence `score·120`, outbox 2 s, tx headroom 64 B | Encode / queue | Never binds on s1. |

Also low-risk sentinels (alignment deadband 0.5 m/s, `TimeToTarget` floor
0.1 m/s, yaw deadband, `Outbox<24>`, `SeenSet<256>`, `kMaxFleet` 64).
Those are not interesting; they are plumbing.

**Do not treat 60 m sense as a guess we forgot.** It is on `SwBootInfo`.
The struct default is the s1 fixture. Cover already uses the live value
(D74). `--dump-params` on generated ids is refused, so a sweep mean must
not be compiled in. Sample of 21 generated ids: mean 76.1 m, SD 15.9,
range 46–104 (`notes/sense-sample.json`).

---

## Unity viewer

The viewer is a graded deliverable (CHALLENGE.md §11.3) and it is already
the thing we drive: 3D airframes at recorded attitude, trails, ground-truth
vs per-observer belief (`B`), logs on the drone that wrote them, radio
links, telemetry chips, score ledger, kill-envelope / cover cue, scrub /
pause / step / speed, colour key. This list is what it still will not do
in an interview, not a recap of that.

### Written down

| Item | Why it is still standing |
|---|---|
| **1 m ground grid** | The ground is a scaled Unity plane (`EnvironmentView`), no metre ticks. Distances are guessed from the asset cylinder and kill-radius cue. A 1 m / 10 m grid (and a 10 m altitude tape) is how you read “was that 12 m or 30 m” without selecting a craft. |
| **Standalone player build** | There is no `BuildPipeline` / CI player. Driving it today means the Unity Editor on this machine. §12 says bring the visualiser; an `.exe` (plus a Linux build if the interview box is not Windows) is how someone else opens a run without installing Unity. `StreamingAssets` is already the load path a build would use. |
| **Compass** | Viewer +Z is north (`FORMAT.md`: NED `x` → viewer `z`). Arena borders exist and are **unlabelled**. The only `N` is on the inspector heading preview, which is a 2D chip for one craft, not a world rose. A north pip on the ground and a camera-locked compass are the missing orientation. |
| **Load several runs / a sweep and compare** | The picker lists StreamingAssets **and** an extra folder, but `VisualizerRoot.LoadRun` binds **one** run. Switching V10 ↔ V23 means unloading. There is no linked clocks, no ghost of the other trajectory, no split view, no sweep table of the eight ids inside the viewer. Ablation output sits in `runs/ablation/<ver>/` and is compared by leaving Unity. |

A self-contained player also has to eat runs without this repo’s Python
sidecar. Today: `jsonl` → `tools/build_viewer_data.py` → `sync_viewer_data.ps1`
→ Editor. A zip of `.bin` + `.meta.json` is enough if the extra-folder picker
works in a build (`NativeFolderPicker`); a zip of raw traces is not.

### Also standing

These showed up reading the viewer against §11.3, not from a feature
wishlist.

| Item | Why |
|---|---|
| **Top-down, north-up camera** | Default view is 45° orbit; `R` resets that. No plan-view preset. The grid and compass only pay once the camera is looking down the Z axis. |
| **Sense / comm for the whole fleet** | Those cues draw on the **selected** friendly. Cover (D57) is the fleet belt. Blind gaps between pickets are still a select-and-look exercise. |
| **Measure** | Click-two-points distance (and height). The grid is the cheap version of this; a tape is the precise one. |
| **Bookmarks** | Scrub to t=26.48 from memory, or from a log click. No named marks on the timeline for “handoff”, “sitting wall”, “breach”. |
| **Chase / look-from camera** | Selection follow is still orbit. A ram’s last two seconds are easier from the interceptor. Not required by the brief; it is how we actually debug intercepts. |
| **`near` log volume** | D4: 57% of lines on s1, least informative. Display is English now; the noise is still a brain/viewer filter problem. A default hide of `near` would make Logs usable in an interview. |
| **Attitude check is still visual** | `FORMAT.md`: the NED→Unity quaternion is silent when wrong. No automated “this craft yawed, the mesh yawed” test in `plot_fixture.py`. |

Not gaps, so they stay off this list: belief vs truth, per-drone logs,
links, bandwidth, report/score, time control, kill-envelope, extra-folder
**single** load, compromised magenta.

---

## Graded deliverables vs this list

| §12 item | Standing |
|---|---|
| Brain library | s0–s2 + generated x1/x2. s3–s5 not attempted. |
| DESIGN.md | Rewritten to present tense against D71–D74. |
| Visualiser | Graded Unity viewer, driven from the Editor. Remaining items in **Unity viewer** above. |
| Tests | `ctest` + determinism + identity/canary sweeps. |
| Results | Ablation ladder V0–V23, plots in `notes/`. |
| Honest gaps | This file. |
