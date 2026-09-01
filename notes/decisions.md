# Decisions

Running log, newest at the bottom. DESIGN.md gets assembled from these at the
end — writing the reasoning down once, when it is fresh, beats reconstructing it
on the last evening.

One entry per decision that a reviewer could reasonably have made differently.
Not every choice needs an entry; the ones with a real cost do.

---

## D0 — TEMPLATE

**Options considered**

**Chosen:**

**Why:**

**Cost accepted:**


---

## D1 — Windows-native build as the primary path

**Options considered**

1. Docker container (linux/amd64) as the only supported path.
2. Native Windows with MSVC, container kept as a documented fallback.
3. WSL2 with the Linux binary.

**Chosen:** 2.

**Why:** the package ships `bin\swarm_sim.exe`, so the native path has the
fewest moving parts and no emulation layer between me and the simulator. Docker
Desktop may not even be installed, and installing it costs an hour I do not
have tonight.

**Cost accepted:** I need MSVC (or MinGW) on the machine, and the build is not
reproducible on a machine that lacks it. The container files stay in the repo so
that fallback is one command away, but they are no longer the tested path — if I
ever need them, expect to spend time revalidating them.

---

## D2 — Hostile discriminant: sure-hit or shrink, not a 24 m gate

**Options considered**

1. Tighten `asset_radius · 0.8` (24 m) to a smaller fraction.
2. Require miss to be shrinking, with no sure-hit clause.
3. Sure-hit (`miss < 5 m`) **or** miss has dropped ≥ 3 m since first sight while still inside `asset_radius`.

**Chosen:** 3. Implemented as `AimedAtAsset` in `belief.cpp`; `Classify` latches `Track.miss_at_first`.

**Why:** The 24 m gate is what called civilians `enemy` before t=8 on s1 (entities 22 and 24). Alignment and closing look identical for a near-miss chord and an aimed dash; CPA miss is the discriminant, but a static radius that is 80% of the asset still accepts ~16% of civilian chords. Tightening the fraction (option 1) still calls any chord inside the new radius. Shrink-only (option 2) rejects s1 hostiles, who spawn already aimed at the origin so their miss never shrinks from zero. Option 3 is the comment already on `ClosestApproachDistance`.

**Cost accepted:** A civilian whose chord misses by less than 5 m still looks like a dash. First-sight CPA is noisier than `fix_sigma` because it extrapolates velocity, so a 3 m drop can fire on a constant chord when `asset_radius` is small (generated x2-a is 20.6 m). Sweep after the change: `pair_neutral` gone on 6/8 scenarios; remains on x1-a (1, miss 5.3 m) and x2-a (2, miss 6.2 m and 18 m). Policy commit and the 4 m separation margin were left alone.

---

## D3 — Diagnostic logs: transitions and boot params, not per-tick

**Options considered**

1. Leave `host->log` as the boot banner only. Debug civilian rams from geometry, `beliefs[]`, and events.
2. Log every tick (stance, target, accel, every track). Full flight recorder.
3. Log on *transitions* only: class change, commit/abort/picket, proximity band steps (12 / 6 / 3 m), plus one `params` line at boot with the `SwBootInfo` numbers the trace header does not carry.

**Chosen:** 3. `LogBeliefChanges` / `LogProximity` in `brain.cpp`; `Policy::last_log_` for commit/abort/picket; `Announce` writes `params sense=… comm=… sep=… ring=…`.

**Why:** The recording already has `beliefs[]` as declaration deltas and events for deaths. That is not enough to tell a false-positive *commit* from “saw it, unknown, 4 m margin lost” from “never in the track list.” Those three are different remaining-ram causes and they look identical in the kill event. Per-tick lines (option 2) would hit the finite log budget (`drone == -1` and the rest of the run goes silent — CHALLENGE.md §11.3) and drown the Logs window. Option 1 leaves the viewer unable to answer the question the overlay was built for. Transitions are the same shape as `beliefs[]` / `links[]`: hold the last value, emit a row when it changes. The `params` line is the honest channel for `sense_radius` / `comm_radius` / `separation_margin` / picket ring: they exist on `SwBootInfo` but are **not** in the trace header, and inventing 60 / 90 in the viewer is a lie on generated ids (FORMAT.md).

**Cost accepted:**

- **Score: none.** `log` is not a scoring term. Compute is measured and not scored. Comms scores broadcasts, not log text. `Decide` / `Fly` do not read `logged_belief`, `near_band`, or `last_log_`, so the command is the same bits as before this patch. No sweep: the command path was not touched.
- **Log budget: the real one.** Generous but finite. A late collision after overflow is exactly the line you needed and did not get. Volume is bounded — boot is 2 lines × fleet, class/stance changes are rare, proximity is at most three steps per track per close pass, not 100 Hz. Still the thing to watch on a long generated run; a `drone == -1` line means stop adding verbs, not add a 2 Hz heartbeat.
- **Per-tick work:** `LogProximity` still walks `tracks()` every 100 Hz tick to keep `near_band`, even when it writes nothing. Cheap next to ProNav, paid whether or not anyone is watching. `LogBeliefChanges` is a compare per track and only does CPA/align math on an actual class change.
- **Redundant `params`:** every drone writes the same `SwBootInfo` line. Waste of 16 identical rows so the viewer can parse the first one it sees; not worth a leader-election just to log constants.
- **Glue-file leak:** `brain.cpp`'s own header says tactics do not live there. The observers sit there anyway so belief/policy/flight stay free of `host->log`. A fifth `diag` module would be the tidy version; not justified at three call sites.
- **What this is not:** it does not fix remaining civilian rams, and it does not record commanded accel (the trace stride has no command column; kinematic Δv is a viewer-side stand-in).

---

## D4 — Log lines stay terse on the wire; the viewer reads them back into English

**Options considered**

1. Write readable sentences from the brain: `host->log("track 14 called hostile, passes 5.2 m from the asset, closing 7.7 m/s")`.
2. Structure the wire format — JSON or key/value per line — and render from the fields.
3. Keep `verb trk=N k=v k=v` as written, and expand it at display time in the viewer. `LogPhrase.Humanize` in the viewer; `Raw` toggle on the Logs window; raw text plus a field key in the row tooltip.

**Chosen:** 3.

**Why:** the terse format is unreadable — that is the whole complaint, and it is fair. `drop trk=15 unknown miss=155.2 first=138.4 score=-0.13 align=-0.98 close=-2.6` is nine numbers and no sentence. But the fix belongs on the display side, for three reasons.

The wire format has other readers. `--verbose` puts these lines on stderr, where short and greppable beats prose, and `LogVerb` already drives the verb chips in the Logs window by splitting on the first space. Option 1 breaks both to fix one.

Expansion needs context the brain does not have room to repeat. The worst ambiguity in the raw lines is that `close=` means three different things — closing on the *asset* in the belief lines, closing on *us* in `commit`, closing on *us* again in `near`/`ram` — and a reader cannot tell which from the line. Saying so costs nothing in the viewer and would be repeated on 1,184 lines on the wire.

Option 2 is the principled version and it is what I would do with more time, but it moves the parse into `RunLoader` and buys nothing the split-on-space decoder does not already give, because the brain is the only writer and the grammar is nine verbs wide.

**Measured, so the "is it costly" question has an answer rather than an opinion.** On the current run (s1, 77.3 s, 14 drones): 1,184 log records, 118 KB of a 1.48 MB trace, 8.0% of the file, no overflow marker. Breakdown by verb: `near` 674, `call` 404, `commit` 29, `wreck` 26, boot banner and `params` 14 each, `picket` 14, `ram` 5, `drop` 3, `abort` 1.

- **Behaviour: zero.** `log` is a one-way host service and nothing reads it back. `logged_belief` and `near_band` exist only to decide *whether* a line is emitted; `Decide` and `Fly` never read them, and the log text is not an input to anything. The command bits are identical with logging on or off.
- **Score: zero.** `log` is not a scoring term. The comms budget counts `broadcast` bytes, not log text. Tick cost is measured against 2000 µs and reported, but explicitly not scored.
- **Simulation time: negligible, and flat in verbosity.** The per-tick cost is `LogProximity` walking `tracks()` at 100 Hz to maintain `near_band`, which is paid whether or not a line comes out. The `snprintf` fires only on a transition — 1,184 times across the whole run.
- **Recording size: linear in text, and the only thing longer prose would actually cost.** Log records are the one record type the trace does not decimate. Doubling the text would take the log share from 8% to roughly 15% of the file.
- **Log budget: the real constraint, and the reason this is not free.** Generous but finite; on overflow the file says so with a `log` record at `drone = -1` and takes no more, which means a late collision is exactly the line you lose. This run is nowhere near it, but a long generated run with more traffic is the case to worry about, and prose on 674 `near` lines is how you get there.

**Cost accepted:**

- **A second grammar to keep in step.** `LogPhrase` hard-codes the brain's format strings. Change a `Logf` in `brain.cpp` and the viewer silently falls back to printing the raw line — which is the right failure, but it is silent. The unknown-verb path returns the input unchanged and `Humanize` swallows exceptions, so a phrasing bug can never hide the line it was meant to explain.
- **Rounding.** `passes 155 m from the asset` where the line said `155.2`. Display drops a digit above 100 m; the raw is in the tooltip and behind the `Raw` toggle.
- **Wording is interpretation.** `align=-0.98` renders as "pointed away" against a fixed set of bands. The bands are a judgement about what is worth distinguishing, and someone reading closely should use `Raw`.
- **Not fixed: volume.** `near` is 57% of all log lines and the least informative of them — three bands per track per close pass, mostly on craft that were never a threat. Readability is now a display problem; noise is still a brain problem, and the cheap fix if the budget ever bites is to drop the 12 m band.

---

## D5 — One component owns the floating windows

**The bug:** the Cues button highlighted on hover and did nothing on click, and `C` did nothing either. Aircraft, Events and Logs — the same button, the same `FloatingPanel`, the same UXML — all worked.

**What was wrong:** `CueOverlay` was a second component wiring its own buttons out of the same `UIDocument` that `SceneStateView` wires. Two components racing to query one UI tree, each with its own retry loop and its own idea of when the tree is ready. Whatever the proximate cause on any given frame, the failure is only reachable because that duplication exists: a panel that never opens and a panel whose wiring silently no-ops look identical from the outside, and neither leaves a trace, because a lookup that is never attempted logs nothing.

**Chosen:** `SceneStateView` owns all four windows. `CueOverlay` keeps only what is genuinely its own — the line pool, the cue mask, and what each cue means — and exposes `Specs`, `Toggle`, `IsOn`, `Available`, `FooterText`. `PlaybackInput` owns `C`, next to the other dozen keys it already owns. The panel moved from `MainUI.uxml` into `SceneStateView.uxml` beside its three siblings. Net effect is about 70 lines deleted: `CueOverlay` lost `TryWireUi`, `BuildChips`, `OpenPanel`, `SetOpenButton`, `RefreshChips`, its `FloatingPanel`, its `Update`, and its copy of `IsAnyTextFieldFocused`.

**Why this over finding the frame-level cause:** the wiring is not observable from outside the editor — a component that never runs and a component that runs and finds nothing produce the same evidence, which is none. Removing the second wiring path removes the class of bug rather than the instance, and it is the smaller program. The Cues panel is now built by the code that demonstrably builds the Logs panel, so if one works the other does.

**Cost accepted:**

- **`SceneStateView` grows a fourth responsibility.** It was already the windows file; a fifth window is the point at which the chip-and-panel pattern should be extracted rather than repeated a fifth time.
- **The renderer is created on demand.** `SceneStateView.Cues` adds a `CueOverlay` if the scene has none, so the panel cannot be broken by a missing component. That is defensive, and in a codebase where the scene is under review rather than hand-edited it would just be a serialized reference.
- **Stale serialized field.** The scene still carries `uiDocument` on the `CueOverlay` entry. Unity drops unknown fields on the next save; not worth hand-editing a scene the editor has open.

---

## D6 — Stance stays a reconstruction; do not publish it on a second channel

**What "intent" was:** `Policy::stance_` — Forming, Picketing, or Committed. It is what this drone is doing with *itself*, not a belief about anyone else. The beliefs panel had labelled it Intent and filled it from the last log line that looked tactical.

**Options considered**

1. Keep scraping the last `commit` / `abort` / `picket` / `near` / `ram` line and print it raw.
2. Add a per-tick or heartbeat stance field so the viewer can read the enum directly.
3. Reconstruct stance from the three transition verbs the brain already writes (`commit`, `abort`, `picket`), and treat silence as Forming.

**Chosen:** 3.

**Why:** stance is piecewise constant. The brain already logs every change: `picket` when it arrives within 8 m of the slot, `commit` when it spends itself, `abort` when it returns to the ring. Between those lines nothing happens, so a periodic dump would be 14 drones × 100 Hz against the same finite log budget that already overflows by dropping the *late* collision. Putting it on the heartbeat would spend comms on a fact peers already infer from Claim / silence. A new trace record type is a FORMAT change for a value the existing verbs determine.

Option 1 failed for a narrower reason: `near` is 57% of all log lines. The last "tactical" line is almost always a close-pass, so the panel was showing proximity and calling it intent.

Forming has no verb. That is correct encoding — it is the default, not an event — and the viewer says so rather than inventing a boot line just to have a token.

**Cost accepted:**

- **A reconstruction can be wrong if a transition is lost.** Log overflow drops the newest lines. A drone that committed after the budget died will still read as picketing. That is the same failure a dedicated channel would have, because it would be a log record too.
- **The wording is a gloss.** "On station" is not in the brain; `Stance::Picketing` is. The raw verbs stay in the Log section.

---

## D7 — Hostile miss is 3D; alignment stays horizontal

**Options considered**

1. Gate on altitude: civilians fly high, hostiles do not.
2. Switch alignment and closing to 3D as well.
3. Require a descent (`vz`) before a hostile call.
4. Keep horizontal alignment/closing, but compute closest-approach miss in 3D.

**Chosen:** 4.

**Why:** Drone 2 on x1-a called civilian 28 at t=5.57 (`miss=5.2 first=10.7 align=1.00`) and rammed it. The ground track is a 5.2 m chord — inside D2's sure-hit/shrink gate. The craft was level at 30 m (`vy = 0`), the same height as the ring, so a "civilians stay high" cut (option 1) would have let it through. 3D alignment (option 2) is what `belief.h` already rejected: a dash from 40 m up reads 0.97 and dilutes the signal. A descent bit (option 3) would have worked on this recording — every hostile dives at ~2.9 m/s, every civilian is level — but it is a posture, not a miss, and a level hostile that still reaches the cylinder would go unnamed.

3D miss is the same geometry AimedAtAsset already wanted. A level overflight's miss is ~altitude and does not shrink (noisy 2D 10.7→5.2 becomes 32→30.5, below `kShrink`). A hostile diving at the origin has miss ~0. Horizontal alignment still fires on both, which is correct: the plane that scores is the ground plane; the discriminant is whether the trajectory actually goes there.

**Cost accepted:**

- A hostile that approaches *level* at an altitude greater than `asset_radius` will not be called. Not observed on x1-a (they arrive at ~10 m as they enter the cylinder). If a later scenario flies level attacks, this is the first thing to revisit.
- `miss=` in the log is now the 3D figure. Old reads of "passes 5 m from the asset" on a 30 m overflight were the bug, not a format to preserve.

---

## D8 — Heartbeat identity, then kinematic keep-out for mates

**The bug:** pickets ram their own interceptors. An interceptor on the way in looks like a dash at the asset — same alignment, same closing, same 3D miss as a hostile — so `Classify` calls it `Hostile`, `ShouldCommit` spends a picket on it, and `EnforceSeparation` **exempts the target**. Two drones, −80 each (`pair_friendly`). Heartbeats already named every live mate (`NotePeer`); they were never bound to a sensor track, so the exemption had no idea it was a mate.

G1b: `pair_friendly` on 3 of 8 (s2: 4, x2-b: 6, x1-a: 2). The 4 m `separation_margin` cannot arrest a 16 m/s closure with 6.7 m/s² of lateral authority — that needs ~19 m — but 19 m around every track would collapse a 16-drone ring of radius 75 m, where neighbours sit 29 m apart. The margin and the ring have to be chosen together, and the 4 m blend stays for *unknown* traffic.

**Options considered**

1. Raise `separation_margin` to 19 m for everyone. Arrests a cruise-on-cruise close, and also permanently repels every neighbour on the ring. Formation dies.
2. Trust `Heartbeat.origin` as a class without associating it to a track. `track_id` is observer-local; origin is a drone_id. No join, no keep-out.
3. Bind heartbeat claimed position + measured RF range to the nearest *local* sensor track, latch `Friendly`, and give mates a keep-out the kinematics can actually fly. Unknown traffic keeps the 4 m blend.

**Chosen:** 3.

- `HeartbeatPlausible`: `|claimed_range − f.range| ≤ 3σ + 2 m`. Range is our measurement. A replay from the wrong side of the arena fails it (the tier-3 hook, used early because it is free).
- `MarkFriendly`: nearest local track within 8 m of the payload position extrapolated by `now − sent_time` (heartbeat carries velocity). Hold 2.5 s (2 Hz, a few losses). Zeros `closing_score` so a prior dash-shaped score cannot re-fire the moment the hold expires.
- `Classify` will not demote `Friendly` while `now ≤ friendly_until`. Ballistic wreckage still wins: it is checked first. A dead mate stops transmitting and becomes wreckage; a live interceptor diving under ProNav has lateral accel and does not.
- `MergePeerReport` drops a report whose nearest track is already `Friendly`. Creating a ghost hostile at the same place was how a picket could commit to a mate it had already identified.
- `EnforceSeparation`: a `Friendly` is **never exempt**. Floor `friendly_margin = v²/(2a) + 4·kill` with `v = 14` m/s (cruise) and `a = lateral_limit` (~19 m on s1). When a pair is closing faster than cruise — two interceptors on the same bearing — the bubble is `v_close²/(2a)+4·kill` instead, so we start in time rather than at the cruise floor. Closing component of the command is cancelled against a mate. Inside 3 kill-radii the intercept is abandoned and we accelerate away. Unknown traffic still gets the 4 m blend — a tendency, not a guarantee.

Tick order is load-bearing: `Update` (Classify) → `ConsumeFrames` (`MarkFriendly`) → `Decide` → `Fly`. The first Classify of a tick can still call a mate hostile; MarkFriendly overwrites before policy commits. `AbortReason` already drops a commit that is no longer `Hostile`.

**Why the 19 m / 75 m ring is jointly chosen, not independently tuned.** Arresting cruise with the lateral bound is `14² / (2 · 6.7) ≈ 14.6 m`, plus four kill radii → ~19 m. Neighbour chord on the s1 ring is `2 · 75 · sin(π/16) ≈ 29 m`. 19 < 29, so a stationary picket does not sit inside a neighbour's bubble. 19 m around *unknown* traffic would, because civilians cross the ring; that is why they keep 4 m and the identity step has to exist. Raising the unknown margin to 19 m without growing the ring (or thinning the fleet) is the option this decision refuses.

**Cost accepted:**

- A hostile that transmits a plausible heartbeat (tier 3 replay that also matches range, or a tier-5 insider) is marked Friendly and not rammed. The range check is the start of that defence, not the end of it. s1 hostiles do not transmit.
- 19 m vs 29 m is the *picket* trade. Two interceptors can close at ~30 m/s, which needs ~70 m to arrest; that is larger than `sense_radius`, so a perfectly head-on pair that first sees each other at 60 m can still hit. The dynamic bubble starts as soon as they are tracks; it cannot invent range. Yielding one interceptor (B3 claims) is the remaining way out.
- Identity only binds to a *local* sensor track. A mate we can hear but not see is noted as a peer and not yet kept out of — at 1 m they are inside `sense_radius`. The association gate stays 8 m: opening it to 14 m on s1 bound heartbeats to the nearer of two close aircraft and added ~70 wrong declarations.
- Unknown / civilian traffic still uses 4 m. Failing to ID a mate still rams. Two aircraft inside the association gate bind the heartbeat to the nearer one.
- `fsep=` on the params line. Viewer `LogPhrase` and the Separation cue text follow; the cue still draws `sep=`, not the mate keep-out.

**Measured:** 8-scenario sweep after this: no `pair_friendly` on s1, s2, x1-a, x2-b. s1 total −947 (bar −1170). `civilians_lost == 0` on s0, s1, x1-b, x1-c. x1-a still reports 2, both at t=1.1 with no `friendly_lost` — the same two events as the pre-D8 baseline; the third civilian on that baseline was our `pair_neutral`, now gone. Leftover `pair_friendly` on x1-c (one pair at t=43.81) and on 1 of 6 fresh tier-1 draws. x1-b's pair is gone.

---

## D9 — Declare only local 2:1 calls (G2)

**The term:** `score.awareness` is `declare_track` only, sampled once a second, most recent declaration per aircraft. Correct +1, wrong −2, unknown or undeclared 0, averaged and clamped to `[0, 60]`. The clamp is why a 1:6 brain scores the same 0 as a silent one. Friendlies are 16 of 24 entities on s1.

**Options considered**

1. Keep publishing every row in the track store, including hearsay, via `ToSwClass`. D8 already latched mates `Friendly`, so s1 went from 0 to ~53 as a side effect — but a peer-reported hostile sits at `track_id = 0` and is scored against whoever our sensors labelled 0.
2. Declare nothing. Zero forever. Same as today's clamp, no upside.
3. Publish only local tracks, and only classes we would bet at 2:1. After D8 that is heartbeat-matched `Friendly`. `AimedAtAsset` `Hostile` still drives intercept, but on x1-b it was 549 wrong ENEMY calls against the scoring hook — below 2:1. Everything else `UNKNOWN` (wreckage included — naming it anything is −2, measured).

**Chosen:** 3, and Hostile stays off the hook until the discriminant is that sure.

Intercept and scoring are separate on purpose. `Classify` / `ShouldCommit` still spend a drone on a Hostile the moment the evidence bar is met. `Declare` skips `!has_local_id`, publishes `FRIENDLY` for a heartbeat-matched mate, and `UNKNOWN` otherwise so a stale ENEMY does not linger into the sample. Calling a compromised friendly "friendly" is incomplete rather than wrong, so the heartbeat latch is not a trap.

This is G2's two moves in order: stay quiet unless confident, then spend the free block we already have from D8.

**Cost accepted:**

- True hostiles are undeclared, so we leave +1 on the table for every sample they are in view. On s1 that is small next to 16 friendlies held for the whole run.
- The inspector's "calls on this craft" is this hook, so hostiles and hearsay no longer colour it. Internal `call` log lines still fire.
- Putting ENEMY back on the hook is the first thing to do if a later discriminant actually clears 2:1.

**Measured**

G2 asked: awareness > 0 on s1, and more than twice as many correct declarations as wrong ones. Target 20–40 points. Test on s1, then x1-a, then x1-b.

How: `scripts\iterate.ps1` on those three ids, same brain, read `score.awareness` and `correct_declarations` / `wrong_declarations` out of the report JSON. Those counts are what the sim scored, not something we inferred.

What we compared against:

- The G2 brief (and our own earlier runs, before D8 named mates): awareness **0** everywhere. On x1-b the raw mix was 172 correct / 1016 wrong — worse than silence, then clamped to 0.
- After D8 we already declared friendlies *and* hostiles. That flipped the sign (s1 ~53). x1-b still had **549 wrong** ENEMY calls — the discriminant is not 2:1 on generated layouts.
- This patch (D9): declare **friends only**, and only on local sensor tracks. Intercept is unchanged (`score.mission` on all three runs was identical to the friends+enemies declare, which is how we know `declare_track` does not steer).

| scenario | awareness now | correct : wrong | vs the brief |
|---|---|---|---|
| s1 | **51.9** | 4146 : 3 | was 0 |
| x1-a | **38.1** | 3910 : 3 | — |
| x1-b | **47.3** | 4409 : 0 | was 172 : 1016 |

So: the gate is met. s1 sits above the 20–40 band because 16 mates are in view for the whole run; that is the free block, not a new intercept trick.

The three remaining wrongs (s1 and x1-a) are the 8 m heartbeat gate picking the nearer of two close aircraft — a mate's ping stuck on a neighbour. x1-b's 549 wrongs were ENEMY labels on civilians; they went to zero when we stopped publishing ENEMY. s1 lost about 1 awareness point because we also stopped taking +1 on true hostiles; x1-a and x1-b went *up* because dropping those −2s was worth more than the lost +1s.

**Revisit:** friends-only made the overlay lie. `declare_track` is the only channel the viewer has for "this drone called enemy," so hostiles went grey even while we still committed to them. ENEMY is back on the hook for local Hostile tracks. Hearsay still skipped. The 549 x1-b wrongs are a classification problem (D2/D7), not a reason to hide the call.

---

## D10 — The asset is a vertical cylinder, not a sphere

**The question:** `AimedAtAsset` said `miss < asset_radius` meant "on course to enter the asset sphere." CHALLENGE.md §2 only says an asset sits at the centre and a hostile that *reaches* it is a loss. `SwBootInfo` and `--dump-params` publish `asset.position` and `asset.radius` — no height, no shape word. FORMAT.md and the viewer prefab called it a sphere; the prefab's live child is a Unity cylinder planted as a 2 m disc at y = 1.

**Measured, s1, five breaches** (viewer y-up, last 10 Hz sample while alive, then the `breach` event):

| hostile slot | last pose horiz | alt | 3D range | event − last sample |
|---|---|---|---|---|
| 26–30 | 30.75–30.82 m | **6.75 m** | 31.5 m | **0.06 s** |

Closing ~15 m/s horizontally. 0.06 s more puts ground range on 30 m while 3D range is still ~30.7 m. A sphere of radius 30 at the origin would be crossed ~0.10 s later, at ~6.4 m altitude. The event matches the cylinder, not the sphere. Slot 24 is the intercept, still 66 m out.

The y = 1 m "hit" was the viewer's marker: EnvironmentView scaled the 2 m-tall cylinder to height 2 m and sat it at y = 1. Hostiles vanished at 7 m on the r = 30 wall, above the hat.

**Chosen:** treat `asset_radius` as the **horizontal** radius of a vertical cylinder from the ground to the arena ceiling. Classification (D7) still uses 3D miss so a level civilian chord is not a dash. That gate is *stricter* than the breach test: a level hostile above `asset_radius` would still score a breach and would not be called. Not seen on s1.

**Cost accepted:** the viewer cylinder is now floor-to-ceiling, which is honest and can occlude whatever flies inside the radius (civilians). The inscribed-sphere comment is gone; `AimedAtAsset` is not the physics.

---

## D11 — Commit only a catchable local intercept (G3)

**The term:** `W_kill` 100, scaled by how early; `P_breach` −200. Converting a breach into an early kill is up to **300 points of swing**. After G1/G2, s1 was 1 kill / 5 breaches, total −947. The overlay was marking hostiles; the drones were not stopping them.

**Step 1 — ProNav is not the leak.** `flight.cpp` ProNav is standard PN (N = 3.5) plus LOS thrust when closing < 12 m/s. `flight.h` already says a stern chase against the shared 6.7 m/s² bound does not converge. The first s1 intercept (hostile_0, t≈16.5) finished, so the guidance can kill when the geometry is a closing intercept. Watching a commit that never meets is a commit-rule failure, not a PN failure.

**Step 2 — the commit rule was spending drones on things they cannot catch.** Three stacked bugs, not a tuning constant:

1. **`|| ttg < 12`.** `ShouldCommit` was `closing > -2 || ttg < 12`. Anything already inside 12 s of the asset was a commit, including stern chases (`close=-14.5` in the s1 log). That is exactly the geometry `flight.h:31` refuses.
2. **Hearsay `trk=0`.** `MostUrgentHostile` returned peer-reported Hostiles (`has_local_id` false, `track_id` 0). `Decide` then skipped re-resolve because `target_id_ != 0`. Stance stayed `Committed`, `target_` was null every later tick, `Fly` cruised the ring. Timeout 25 s, spawn interval 14 s: one ghost commit parked a picket through the next arrival.
3. **Closing used only their velocity.** `RangeRate(hostile, hostile_vel, us)` ignores our motion. A picket that has started inward reads as closing on an outbound. ProNav flies *relative* velocity; commit must too.

**Options considered**

1. Leave the `||` and raise `kEvidenceForCall` so we "commit earlier" by calling earlier. Calls the same uncatchable geometry, just sooner, and re-opens G1 civilian FPs.
2. Drop the `||`, require relative closing, only local tracks, and arrive before the cylinder — still let every drone that can see it go. Fixes stern/hearsay; re-introduces three-on-one (pair_friendly / two interceptors panicking at 3 kill-radii and both missing).
3. Same geometry gate as (2), plus a radio-free allocation: the facing ring slot (same angle as `RingSlot`) is the only drone allowed to spend itself.

**Chosen:** 3.

- Local Hostile only, and only if the call is younger than 6 s. Older latches were wreckage or a mate we failed to ID; neighbours of a spent owner chased them for the 12 s abort timeout and missed s1 hostile_4, which they had already called. A track we just aborted is not re-chased for 2 s (stops the Hostile/Unknown flicker loop).
- Evidence bar is the call. `kEvidenceForCall` moved 1.2 → 0.6 after the commit rule was spending the facing drone: 0.6 s of aimed geometry, 3 wrong declarations on s1 (same as 1.2), `civilians_lost == 0`.
- `ClosingSpeed` (both velocities, horizontal) ≥ 1 m/s. Evasion can flip the sign for a beat; receding abort waits 6 s, not immediate.
- Catchable: fly at cruise (14 m/s, same as `Fly`) along the line of sight, arrive 0.5 s before `TimeToCylinder`. Stationary `range/closing` is the picket waiting; a neighbour of a dead owner has to fly.
- Owner is `round(bearing / 2π · n) % n` while that drone's heartbeat is live. If the owner has been silent 1.5 s (three missed 2 Hz beats), the two neighbouring slots may go. Heartbeats must actually be on the wire: unread Claims used to win the one-frame-per-tick, interceptors went silent, neighbours stacked, MarkFriendly expired. Claims are not composed until something reads them; heartbeat is the highest priority. Never-heard is assumed alive so we do not steal sectors at boot.
- Abort timeout 12 s, derived from `enemy_spawn_interval` 14 s.

**Step 3 — classifier sensitivity is the remaining tension, not the first knob.** G1 made `AimedAtAsset` conservative so civilians stay unknown. Every extra 0.1 s of evidence is 1.6 m of hostile dash and a later intercept, which is how `W_kill` decays and how `P_breach` appears. Lowering the call bar recovers intercepts and re-opens civilian rams. That trade *is* tier 1: the same airframe is the weapon and the liability, and the discriminant is a 3D miss on craft that look identical in the horizontal plane. The commit rule moved first (local, relative closing, catchable, one owner, no ghost latches). Then `kEvidenceForCall` 1.2 → 0.6. On s1 that did not cost a civilian; the sixth kill was the fresh-Hostile gate, not the 0.6 s.

**Cost accepted:**

- A dead facing picket still leaves a hole until neighbours notice 1.5 s of silence. Two neighbours may both go; D8 keep-out is what stops them ramming.
- Catchable is not a full PN intercept solve. A beam shot with tiny closing is rejected even if a lead pursuit would work.
- A Hostile call that is genuinely 6 s old and still the right target is refused. Not seen: real intercepts finish in ~4 s of Hostile.
- `kEvidenceForCall` 0.6 is still a guess. Generated layouts with tighter civilian chords are where it will show.

**Measured, s1:** 6/6 hostiles, 0 breaches, `civilians_lost == 0`, wasted 0, 3 wrong declarations, mission +40.6, total **+130.9**, `asset_survived`. Was 1/6 and −947. Each converted breach is the 300-point swing the brief named; the extra was not chasing ghosts in spent sectors.

**Measured, `-Tier 1 -Count 6`:** 6/6 completed. 0 civilians, 0 wasted, 0 `pair_friendly` on all six. Four layouts 3/3 or 4/4 and positive (best +215). Two leaked one (2/3 and 3/4, worst −110). Mean +76. The over-fit failure mode would have been s1-perfect and generated-zero; this is not that.

---

## D12 — Unknown traffic gets the arrest distance, not a 4 m floor

**The guess:** `separation_margin = kill_radius · 4 = 4 m` around unknown/civilian. Arresting 16 m/s with 6.7 m/s² needs `v²/(2a) ≈ 19 m`. At 4 m the drone has 0.25 s to shed 16 m/s, which needs 64 m/s² — about 10× too late.

**Options considered**

1. Raise the static floor to 19 m for everyone. D8 already refused this: 19 m around every track on a 75 m / 16-drone ring (neighbours 29 m apart) is a permanent repulsion. Formation dies.
2. Leave 4 m. Honest about the ring, and after G1/D8 the leftover s1/x1-a civilian losses are not our rams (x1-a two `civilian_lost` at t=1.1, no `friendly_lost`, no `pair_neutral`). The kinematics are still wrong the next time we *do* close on a chord.
3. Keep 4 m as the *non-closing* floor, and grow unknown/civilian/wreckage to `v_close²/(2a)+4·kill` when the pair is actually closing — the same arrest D8 already uses for mates. Do not cancel the closing command (the intercept target must still be rammable) and do not panic.

**Chosen:** 3.

Neighbours on the ring are not closing, so they stay on the 4 m floor and the ring holds. A civilian or wreckage closing at 16 m/s gets 19 m, in time for the lateral bound. The intercept `exempt` is unchanged. Mates still cancel closing and panic inside 3 kill-radii.

**Cost accepted:** an interceptor can be shoved off a dash by a civilian that enters the arrest bubble. That is the G1 trade written as a manoeuvre rather than as a class. s1 did not pay it.

**Measured:** s1 6/6, civ 0, total **+130.9** — identical to D11, so the new bubble is not the thing that was catching intercepts. x1-a 4/4, civ 2 at t=1.1 with no `pair_neutral` (unchanged; those two are not our collisions).

---

## D13 — Local tracks die with the sensor picture

**The guess:** `kDropAfter = 3.0 s`. `--dump-params` on s1 says `sense.track_drop_time=2.0`. Holding a second past the simulator is a frozen Hostile at the last pose — D11's neighbours chased those for 12 s and missed the next inbound.

**Options considered**

1. Set 3.0 → 2.0 to match s1's dump-params. CHALLENGE.md §3 says drop time is unpublished and *varies between missions*. Fitting 2.0 is the same class of mistake as fitting `|| ttg < 12`.
2. Keep 3.0 to outlive a short dropout. Costs the ghost latch D11 had to paper over.
3. Drop a *local* track the moment it is absent from `obs.tracks()`. The simulator already made the decision (destroyed vanish next tick; out-of-range after its private hold). Hearsay is not in that list and keeps a 2 s age-out (four missed 0.5 s reports).

**Chosen:** 3. The unpublished number is measured by absence, not guessed.

**Cost accepted:** a one-tick sensor dropout now forgets the track and the next look is a new `track_id` with evidence from zero. s1 `dropout_prob=0`, so this did not fire. A mission with dropouts will re-learn class; that is the published ABI.

**Measured, s1:** 6/6, civ 0, 3 wrong, total **+131.3** (was +130.9). Awareness 54.4 vs 53.9 — fewer stale declarations, not a new intercept trick.

---

## D14 — Extrapolate peer reports, then associate at 8 m

**The guess:** `kGate = 12 m` to fuse a peer's track report onto a local track. The comment said to size it from `fix_sigma` and latency; the arithmetic on dump-params is ~3–4 m, not 12. 12 m merges two aircraft. 4 m (sigmas only, after extrapolating age) *duplicated* the same aircraft: 2089 `call` transitions on s1, comms 26 vs 36.

**Options considered**

1. Shrink 12 → 3 because the comment's own formula says so. Ignores outbox delay and unmodelled turn. Measured: duplicates, log flood, comms collapse.
2. Leave 12. Still fuses two craft 10 m apart. Latency motion is still inside the gate, so extrapolation would be free lunch left on the table.
3. Extrapolate the payload by measured `now − sent_time` (same as heartbeat; latency is unpublished, this is the measurement), drop reports older than 2 s, and associate at 8 m: `2·1.2 + 3·√2·0.35 + 0.2 s · 16 m/s ≈ 7 m`, rounded. 4 m was the sigma-only number and failed in flight.

**Chosen:** 3.

**Cost accepted:** two aircraft inside 8 m still merge. A report whose velocity is a lie (tier 3) is extrapolated the wrong way and may miss the real track — it then sits as hearsay, which D11 will not intercept. Multi-hop (s2, 220 m) is still the missing piece; this only makes the one-hop fuse honest.

**Measured:** s1 6/6, civ 0, total **+130.0**, comms 35.5, 694 log lines (4 m gate was 2245 lines / comms 25.9). s2 1/6 → **2/6**, −902 → **−700**, civ 0. One extra fused intercept, not a new guidance trick.

---

## D15 — One drone per hostile, and the others get off the line (G4)

**The leak:** two drones still go for one inbound. D11's backup was "if the facing slot is silent, *both* neighbours may go." That is the brief's named failure mode (§9.2): three-on-one intercepts well and then the spare two kill each other and the debris. Even without a ram, a picket on the line of sight **cancels the interceptor's ProNav** (D8 mate-avoidance zeroes the closing component). The second drone is not extra firepower; it is traffic.

Claims were the half-built answer (`ClaimMsg` is on the wire format). D11 already measured why not to finish them: unread Claims won the one frame per tick, interceptors went silent, neighbours treated the owner as dead and stacked.

**Options considered**

1. Honour `ClaimMsg`. Stickiness across a sector-boundary weave, but it is a radio fact. A dropped claim is two owners; composing it is how D11's heartbeats died. Not until identity has a channel that cannot starve.
2. Keep both neighbours as the spent-sector backup. Covers hostile_4/5 when the facing drone is gone; pays two interceptors and a spoiled ProNav whenever the facing drone is merely quiet.
3. Radio-free unique owner: facing slot, then the **first live drone clockwise** — one successor, not both. Do not start (and abort if already committed) when a Friendly is already flying at this hostile and is closer. Pickets whose ring slot sits inside `friendly_margin` of the owner's corridor step off it. The interceptor keeps the D8 arrest blend and the 3-kill-radius panic, but does **not** cancel ProNav toward a mate; the picket is the one that yields.

**Chosen:** 3.

**Cost accepted:** a dead facing picket leaves a hole until 1.5 s of silence, then only the clockwise neighbour covers — the counter-clockwise one stays. A radio dropout that looks like death still hands the intercept to +1, not to both. Two drones with disagreeing `heard_[]` (packet loss) can still both think they are UniqueOwner; the closer-chaser abort is the backstop, and it needs the other to be visibly flying at the target (~5 m/s along LOS). Claims remain defined and unread.

**Measured, s1:** 6/6, civ 0, wasted 0, total **+130.0** (identical to D14). Six `commit` lines, six different drones, **zero** `abort`. The previous both-neighbours backup was not firing on this layout; the unique rule did not give a kill back.

**Measured, 8 named scenarios:** `pair_friendly` **0** on all eight. s2 2/6 → **3/6** (−700 → −502). x1-a still 4/4 with the same t≈1.1 civilian losses plus one `pair_neutral` and one wasted — not a stacked intercept.

**Measured, `-Tier 1 -Count 6`:** 6/6 completed. `pair_friendly` **0**, wasted 0. Three layouts full clear (best +163.5). Three leaked one. Mean −26 on this draw (D11's +76 was six different tokens; kills did not drop on the fixtures we can compare).

---

## D16 — Yield and log pings as viewer cues, not new brain verbs

**Options considered**

1. Log `yield` whenever a picket's slot is pushed off an intercept corridor. Honest about the decision, costs the log budget on every ring neighbour that steps (D3: transitions only, overflow is `drone == -1`).
2. Reconstruct the yield in the viewer from intercept spans + `params fsep=`, and draw log-named relations (call / drop / wreck / near / ram / duplicate abort) as 1.4 s fading lines associated the same way Intercept already associates `trk=`.

**Chosen:** 2.

**Why:** the intercept cue is a line because the recording already has commit/abort. Yield is the same geometry with no extra verb — the picket is inside `friendly_margin` of an intercept you can already see. A yield log would fire often on a tight ring and buy a confirmation the overlay can replay from numbers it already quotes. Pings are the logs themselves; the only extra is the same observer-local → world-entity join Intercept already makes, held for 1.4 s so a scrub lands on them.

**Cost accepted:** yield is drawn against interceptor→predicted ram (D17), not the owner's empty ring slot the brain uses before a chaser is visible. Those agree while the interceptor is leaving the slot. Yield rows in Logs are the same reconstruction, merged in at load as `yield` / `yield clear` so the chip can filter them — Raw is not a brain line; the tooltip says so. Hearsay (`peer`) pings have no world entity and stay off. A `drop` ping needs a declaration that disappeared in the last 0.15 s; a drop without a prior call has nothing to aim at.

---

## D17 — Yield only on the remaining flight, not the chord to the hostile

**The leak:** D15 stepped a picket off anyone else's intercept if the picket's ring slot sat inside `friendly_margin` of the **full** segment owner-slot → hostile. A far inbound draws a chord across the ring. The interceptor rams in ~3 s / ~50 m; pickets sitting near the hostile's *current* pose — or anywhere else on that long chord the interceptor will never fly — still yield. That is traffic-avoidance of empty air.

**Options considered**

1. Yield only after a `commit` log (the viewer already has a span). Leaves the owner's lane blocked until they leave the slot. Does not fix the far-chord: once committed, interceptor→hostile is still the long line.
2. Per-picket: yield iff time-to-meet the picket is less than time-to-meet the hostile. Same information as clipping the corridor, more branches.
3. Clip the keep-out to a first-order horizon. Assume cruise (14 m/s) along remaining LOS. `t_meet = range / closing`; along-track length is `cruise × min(t_meet + 0.5 s, 12 s)`. If assumed cruise is not closing, there is no intercept and no corridor. Origin is the chasing Friendly if we can see one (~5 m/s along LOS, same test as D15), else the owner's slot. Lateral keep-out stays `friendly_margin`. No new numbers: cruise, catch slack, abort timeout, and min-closing are already the commit rule.

**Chosen:** 3.

**Why:** remaining flight is speed × time-to-collision. A picket past the predicted ram is not the traffic that cancels ProNav. The 0.5 s slack and the ~19 m bubble around the horizon endpoint cover a slightly late kill; they do not cover a stern chase we already refuse to fly.

**Cost accepted:** a weave that delays the ram past the horizon can clip a picket that sat still because we thought the meet was earlier. The viewer Collect uses the same clip so the amber cue matches the brain; D16's reconstructed `yield` rows will fire less often.

**Measured, s1:** 6/6, civ 0, wasted 0, total **+130.0** (identical to D15). Six `commit` lines, `pair_friendly` 0.

**Measured, 8 named scenarios:** worst **−524.6** (x2-b), mean **−136.4**, best **+158.4**. Same ladder as D15. This is a picket-goal clip, not a commit change.

---

## D18 — hop-limited TrackReport forward, commit on fused hearsay

**Options considered**

1. Do nothing. s2 `propagation_p95_s` stays `null`; UniqueOwner on a gap never hears the 220 m inbound. Baseline `hostiles_reached_asset` 5, last measured 3.
2. Re-originate a new TrackReport from every drone that fused a peer Hostile (new origin/seq). Information travels semantically. The simulator's hop metric keys on a rebroadcast of the *same* frame (the example's flood of heartbeats is how fixture `p95=0.09` / `max_hops=7` happened). Budget: every drone that hears a hostile reports it at 0.5 Hz as if it were the seer.
3. Naive flood of every frame, hop 4, like `hover_relay_brain.cpp`. Proves the radio. Exhausts s2's 3072 B/s if heartbeats join the flood.
4. Hop-limited forward of **TrackReport only**. Same origin/seq/sent_time, `hops++`, `SeenSet` once, cap `kMaxHops=4`, outbox priority 2 (below original reports at 3 and heartbeats at 5). `Pump` already refuses the last 64 B. Compose reports **local** Hostiles only — hearsay rides the author's frame. Association stays D14 (geometry after `now−sent_time`, 8 m). UniqueOwner may commit on that fused Hostile: store_id, not track_id 0; catchable vs the cylinder; no `sense_radius` gate (hearsay is often 100 m out). Heartbeats stay one hop (never-heard is already assumed alive).

**Chosen:** 4.

**Why:** s2 is "the arrival happens 220 m out where only one drone can see it and the fleet has to be told." The facing slot is often not the seer (largest gap on the ring). A report that stops at one hop never reaches the drone that can still intercept. Forwarding the author's frame is what the hop metric measures, and it is cheaper than re-originating. Committing on hearsay is what turns a shared picture into a kill; D11's "hearsay never intercepts" was the bug that left Fly with a null target, not a principle. store_id plus absorb-into-local when the interceptor gets a sensor track is the fix. The association machinery (pose, velocity, time, class — no `track_id`) is the same piece insider detection will need.

**Cost accepted:** from s3, an unverified peer can send us chasing a ghost or a civilian. Range-vs-claim on the first hop still applies to heartbeats; TrackReport hops are currently trusted. We will have to refuse or discount a report whose velocity extrapolates the wrong way before naming this a defence. Flooding TrackReports still costs bytes; s2 budget is 3072, headroom 64. Hop 4 on a 16-drone 75 m ring is more than the diameter (~2 hops) and will duplicate around the ring — SeenSet stops a second copy of the same (origin, seq).

**Viewer:** Hops cue is a BFS tree on `links[]` from the selection (reachability, not a transcript). Disagree window lists declared calls plus fused `call … peer origin= hops= n= e=` lines. Pings no longer drop hearsay.

**Measured, s2:** `propagation_p95_s` **0.09** s (was `null`), `max_hops_observed` 1, `hostiles_reached_asset` **3** (baseline 5). Same 3/6 as the last local-only ladder; the three late breaches are spent facing slots (drones 0–2 already used) whose clockwise successors sit off-axis and fail catchable until ~55 m. Peer `call` lines show hops 0–3 and n/e association. No `commit … peer` on this layout: UniqueOwner of the first three inbounds is already inside 60 m when the seer reports.

**Measured, s1:** 6/6, civ 0, wasted 0, total **+134.3** (was +130.0). `p95` 0.05 s, hops 2.

**Measured, 8 named scenarios:** worst **−511.1** (x2-b, was −524.6), mean **−108.2** (was −136.4), best **+160.3** (x1-b). x1-a 4/4 with 2 civilians (−51.0). x2-a still 2/4 (−297.9).








