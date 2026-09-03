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

---

## D19 — Survivors re-space on a nearby death, not on radio loss

**The leak:** a spent facing slot stays empty. The next hostile on that bearing meets the clockwise neighbour still sitting 29 m off-axis, who fails catchable until ~55 m. D18 named this on s2: drones 0–2 used, late breaches. The brief's own fleet note says the late arrivals are where fleets leak, and `policy.cpp` had a TODO to re-space.

**Options considered**

1. Do nothing. UniqueOwner already walks clockwise; the successor intercepts from their original station. Cheap. Leaves the hole.
2. Re-space on any 1.5 s heartbeat silence, and assign ownership from the live ranks (`FacingSlot(n_live)`). Even coverage, one owner. Opposite-side drones leave comm range as the ring spreads — that silence looks like death, the live set collapses to whoever we can still hear, and everyone chases a moving 8–9 station ring. Measured: s1 4/6, **−269**, first inbound unowned while Forming never logged `picket`.
3. Re-space stations on *nearby* death only. A mate is off the live ring iff they went silent *and* their last pose was inside `comm_radius − cruise·1.5 s − 10 m` (they could not have left radio). Never-heard stays alive. **Allocation stays UniqueOwner on the original id ring** (D15) so a ghost far-side silence cannot hand the inbound to nobody. Radius does not shrink.

**Chosen:** 3.

**Why:** the user request is redistributing picket coords when a friendly disappears. The hole is local. Neighbours can still hear the victim; opposite-side drones cannot, and must not re-plan the whole circle. Keeping UniqueOwner means the clockwise successor is the same drone who slides into the hole, not a remapped rank that can disagree across the radio horizon.

**Cost accepted:** two drones with disagreeing `heard_[]` still compute different live rings (same class as UniqueOwner). A wrap-around hole is still owned by id 0 while id n−1 sits on it. Clustered deaths two-or-more slots away can look like radio loss to the next facing picket, who then does not slide far enough. x2-b is unchanged (never commits).

**Measured, s1:** 6/6, civ 0, wasted 0, total **+147.2** (was +134.3).

**Measured, s2:** 4/6, **−278.3** (was 3/6, −495.2). Two late breaches remain.

**Measured, 8 named scenarios:** worst **−511.1** (x2-b, unchanged), mean **−30.5** (was −108.2), best **+151.9** (x1-b). x2-a 4/4 **+112.8** (was 2/4, −297.9). x1-a 4/4, 2 civilians (−61.1).









---

## D21 — Stations bisect the local gap; standoff and gap close as one decision

**The leak, measured rather than guessed.** s2 was 4/6 with two breaches and no
mechanism I trusted. Tracing it (`--trace`, log records) settled it in one run:

| hostile | window | outcome | nearest friendly at the end |
|---|---|---|---|
| 27 | 10–24 s | killed | 2.0 m — **and a drone dies** (16→15) |
| 29 | 26–40 s | killed | 1.1 m — **drone dies** (15→14) |
| 31 | 42–58 s | killed | 1.6 m — **drone dies** (14→13) |
| **32** | 58–75 s | **breach** | 57.7 m |
| **34** | 74–91 s | **breach** | 25.3 m |
| 35 | 90–103 s | killed | 1.5 m |

Hostiles arrive one at a time on bearings −10°, 0°, 10°, **20°, 20°**, 30°, and
`s2.json` says the spawner picks the bearing furthest from any defender. Every
kill is a mutual ram (`losses_by_cause: {pair_hostile: 4}`) that costs the drone
*in that sector*. Three kills emptied a contiguous arc and the next two hostiles
walked through it. **Only 5 commits in the whole run, and none at all between
t=58.1 and t=87.1** — a 29 s window covering both breaches. At t=70 and t=85 the
arc from d15@335° to d3@68° — **93°** — was empty, with the survivors still on
their original bearings.

**Why D19 did not close it.** D19 re-spaces by *global* rank: index among the
live, spread over `CountLive` slots. That needs a liveness vector no drone has.
At ring 67.5 m the slot chords are 27 m, 54 m, 80 m, so only ±2 neighbours are
inside `comm_radius` 75, and `comms.max_hops_observed` is **1** — heartbeats are
not relayed. Each drone re-indexes against a different, mostly-stale roster, so
the ring rotates a couple of degrees instead of closing.

**Options measured** (8 fixed scenarios × radius fraction F, plus 20 fresh
`--new-token` ids). Sweep mean, baseline **−30.5**:

1. **Station-occupancy liveness** (treat a station-distant silence as a death, so
   far-side deaths register). Mechanically worked, but worse at *every* radius:
   F0.5 mean **−55.1**, and it made s2 itself worse (4/6 → 3/6). More drones
   counted dead → more station churn than hole closed.
2. **Radius contraction on loss** (hold the full-fleet chord: `R(n) = R_full ·
   sin(π/N)/sin(π/n)`, so the ring pulls in as it thins). Also worse: F0.75 mean
   **−55.7** vs **−30.9** without it, and it broke x2-a (4/4 → 3/4). A single
   death translates the whole ring inward and everyone re-stations at once.
3. **Radius alone.** F0.75 mean **−30.9** — a wash. It splits the tiers rather
   than helping: big gains on wide-radio layouts, matching losses on tight ones.
4. **Local gap bisection.** Walk out from our own slot both ways to the first
   drone we still believe is flying; stand at the midpoint. **Chosen.**

**Chosen:** 4, at F = 0.625.

**Why:** bisection uses nothing beyond the neighbours we can actually hear, so it
needs no consensus and no relay. It is a pure function of the liveness bitmap, so
it cannot oscillate, and it is a fixed point at full strength — nobody abandons a
sector while the ring is whole. The hole closes by diffusion: its two lips slide
in, their neighbours follow.

**Why the radius moved in the same commit.** They are one decision. Meeting a
hostile further out is paid straight into the score (`W_kill·(1 − t_engage/t_free)`),
but every metre of radius also widens the hole a death leaves, since slot spacing
is `2R·sin(π/n)`. Pushed out on its own, F0.75 was a wash. With the gap closing
behind each loss, the same push becomes reward. Past ~0.75 the ring outruns its
own recovery — a leaker at that range cannot be run down, because a hostile has
our lateral limit — and tier-2 layouts collapse (F1.0 mean **−233**).

**Rejected as scenario-fitting:** a radius scaled by `comm/sense`. It looked
principled and scored well, but `comm/sense` is 1.0–1.33 across *every* generated
scenario; only s1 (1.5) and s2 (1.25) are outliers, so the rule was keying on the
two named scenarios and did nothing on fresh ids.

**Cost accepted:** the death signal is shorter than the radio. `RingAlive` counts
silence as death only inside `comm_radius − cruise·1.5 s − 10 m` = 44 m, and the
slot chords are 27/54/80 m, so only the *immediate* neighbour registers. Each lip
of the hole therefore slides half a slot, not a full one — the 93° hole closes by
22.5°, not 45°. Widening that radius is the obvious follow-up and is untested.
The two-slot shift cap is unreachable at this ring size.

**Measured, 8 fixed scenarios:** improves **every one**. min **−511.1 → −303.8**,
mean **−30.5 → +16.3**, max **151.9 → 191.9**. s1 147.2 → 190.3 (6/6), x2-b
0/3 → 1/3, x1-b 151.9 → 191.9. Civilians 2 and wasted 0, both unchanged.

**Measured, 20 fresh `--new-token` ids** (never used while tuning): mean
**−52.5 → −20.8**, kills 51/65 → 52/65, breaches 14 → 13. tier-1 mean
**31.4 → 85.7** (29/31 → 30/31 kills, 2 → 1 breaches); tier-2 mean −136.4 →
−127.3. Floor −307.6 → −315.8, i.e. 8 points worse — the one regression.

**Not achieved:** s2 stays 4/6. Bisection at F0.5 does reach **5/6 (−82.0)**, but
costs x1-c and x2-a and drops the sweep mean to −55.2. The 5/6 is real; it is not
worth the mean.

**Determinism:** `--replay` clean on s1, s2, x1-a, x2-b. All three test suites
pass, including a new `TestStationBisectsTheGap` pinning the geometry above.
---

## D22 — Lead intercept for midcourse, proportional navigation for terminal

**Where the score actually was.** The report carries a per-intercept breakdown I
had not read. On s1 we scored 6/6 kills, zero breaches — and **99.1 mission
points out of a possible 600**:

```
reward:   14.4  17.5  21.3  21.4   5.7  18.8     (W_kill = 100 each)
urgency:  .856  .825  .787  .786  .943  .812     (reward = 100*(1 - urgency))
t_free_s = 9.04       ← the whole budget from spawn to the asset
```

The arithmetic confirms the weights from data rather than the spec: s2 is
`41.5 − 2×200 = −358.4` mission, so P_breach = 200 and W_kill = 100. We were
banking **16%** of the available kill reward. On s1 there are no breaches left
to prevent, so that 500 was the only thing on the table.

**Why.** Tracing one s1 intercept, the committed drone's own speed:

| t | hostile range to asset | separation | closing | **our speed** |
|---|---|---|---|---|
| 8.5 | 169.6 | 86.1 | 1.8 | **0.4** |
| 10.5 | 155.0 | 71.7 | 12.7 | **0.3** |
| 12.5 | 126.0 | 43.6 | 14.0 | **0.2** |
| 14.0 | 103.7 | 23.2 | 13.3 | 6.4 |
| 15.5 | 81.4 | 3.9 | 12.2 | 9.1 |

**The interceptor never flew at the hostile.** It held 0.2–0.4 m/s for four
seconds and was rammed at 81 m — our own ring radius. Every metre of "closing"
was the hostile's own speed. Two compounding causes, both in `ProNav`:

1. Pure PN commands acceleration only *across* the line of sight, to null its
   rotation rate. A picket already standing on the hostile's inbound bearing
   sees almost no rotation, so PN commands almost nothing. PN is a terminal
   homing law; it was being used as an intercept law.
2. The one term that would have fixed it — `if (closing < 12.0f) accel += ...`
   — keys on **closing speed**, which the hostile supplies for free at 15 m/s.
   The condition is false from t = 10.5 onward, so the drone never accelerated.

**Options measured** (8 fixed scenarios; sweep mean, D21 baseline **+16.3**):

1. **Lead intercept all the way in**, flown at `max_speed`. Reward capture
   jumps — max 191.9 → 280.9, x1-b +89, x2-b 1/3 → 2/3 — but kills collapse:
   mean **−199.7**. Measuring closest approach explains it: misses of
   **1.0–3.3 m against a 1.0 m kill radius**. The lead point assumes constant
   target velocity, s1 hostiles evade, and at 35 m/s of closing there is no
   range left to correct.
2. **Lead intercept at 0.85·max_speed.** Better (−31.4), same disease.
3. **Blend lead → PN by range**, handing over while there is still time to null
   the error. 20–50 m: −85.6. 30–80 m: +14.8. 12–35 m: −252.6.
4. **Raise the terminal PN gain** with the handover. The lead intercept arrives
   with far more closing speed than N = 3.5 was tuned for. N = 5: +39.1.
   **N = 7: +63.9.** N = 9: +38.3. N = 12: +37.5.
5. **Handover on time-to-go instead of range** — `range/closing`, which is the
   scale-free way to say "enough time to correct". Sounds more principled and
   is measurably worse: best variant **+39.2** against +99.1 for plain range.
   `closing` swings wildly during the approach, so the threshold jitters while
   range is monotone. Rejected on the numbers.

**Chosen:** blend by range 25 → 70 m, terminal gain N = 7.

**Why:** it is the standard midcourse/terminal split. The lead solution is
closed form — `|d + w·t| = s·t` is a quadratic in t — so the midcourse leg has
no tuning in it at all, and it returns −1 when no intercept exists, which is
the honest answer for an equal-speed stern chase. The two tuned numbers are the
handover band and the terminal gain, both measured.

**Cost accepted:** the handover band is in metres, and the time-to-go
reformulation that would make it scale-free is empirically worse. N = 6 and
N = 7 score the same, so the gain sits on a plateau; the handover does not —
25–70 m gives +99.1 and 25–60 m gives +76.4, so it is a ridge.

**Measured, 8 fixed scenarios:** improves or matches **every one**. min
**−303.8 → −246.8**, mean **+16.3 → +99.1**, max **191.9 → 276.7**. Civilians
2 and wasted 0, both unchanged. x2-b **1/3 → 3/3** (−303.8 → +124.8).

**Measured, s1 reward capture:** mission 99.1 → 140.3; per-kill rewards
14.4/17.5/21.3/21.4/5.7/18.8 → 16.7/25.4/29.4/29.5/12.5/26.8, urgency 0.79–0.94
→ 0.71–0.88.

**Measured, 20 fresh `--new-token` ids:** **17 of 20 improve.** mean
**−20.8 → +30.9**, kills 52 → 54/65, breaches 13 → 11. tier-1 mean 85.7 →
**145.9** with **31/31 kills and zero breaches**; tier-2 mean −127.3 → −84.1.

**Open, and the one regression:** the floor on fresh ids gets worse,
−315.8 → −556.2, on two tier-2 layouts with fast (18.5 m/s) hostiles. Measured
closest approach there: the old law misses by 1.9–2.7 m and the new one by
2.0–8.0 m. Both miss; the lead intercept misses wider. This is terminal
accuracy against a fast evader and it is unsolved — a lead point that estimated
target *acceleration*, rather than assuming constant velocity, is the obvious
next thing and is untested.

**Determinism:** `--replay` clean on s1, s2, x1-a, x2-b. All three suites pass,
including `TestLeadIntercept`, which pins the closed form (including that it
correctly refuses an equal-speed perpendicular crossing) and the standstill case
that was the original bug.
---

## D23 — Rotating and counter-rotating pickets: measured, rejected

Tested on the D22 baseline (sweep mean **+99.1**) because "rotating ring webs"
is a recurring suggestion for this problem: drones fly interlocking,
counter-rotating circular paths so that when a hostile pierces one spot,
another drone rotating into that sector closes the gap.

**Option 1 — uniform rotation.** Every station sweeps the ring at a constant
rate; radius unchanged, so it composes with D21's bisection instead of
replacing it. Ownership has to rotate with the stations or an inbound is
handed to a drone no longer facing it, so `FacingSlot` de-spins the bearing
before quantising.

| rate | tangential speed at R≈86 | sweep mean | min |
|---|---|---|---|
| baseline | 0 | **+99.1** | −246.8 |
| 0.02 rad/s | 1.7 m/s | −5.3 | −677.2 |
| 0.05 rad/s | 4.3 m/s | +51.9 | −222.2 |
| 0.10 rad/s | 8.6 m/s | −0.1 | −293.3 |

Worse at every rate, and x2-b falls 3/3 → 2/3 at all three.

**Option 2 — counter-rotating interlocking rings.** Even ids on an inner ring
turning one way, odd ids on an outer ring turning the other, radii separated by
24 m so the two never fight `EnforceSeparation`. Mean **−412.9**, min −908.7,
every single scenario worse.

**Rejected, both.**

**Why, and it is not a tuning failure.** Three measured reasons:

1. **Coverage was never the bottleneck.** Awareness scores 52–58 of 60 — we
   already detect essentially everything. Rotation buys sensing we do not need
   and pays for it in velocity.
2. **A rotating picket always carries tangential velocity across the inbound
   corridor**, which the interceptor's velocity servo must null before it can
   close. That is the same budget D22 just bought back.
3. **Splitting into two rings halves each ring's density**, and D21 already
   measured density loss as the thing that breaks tier-2 layouts. The dual-ring
   collapse is that result again, larger.

**The deeper reason, worth saying out loud:** s2's spawner enters "on the
bearing furthest from any defender". Against an adversary that attacks your
weakest bearing, **uniform spacing is the maximin strategy** — any
concentration is exploitable, and we measured that directly in D21 when
redistribution schemes moved the gap instead of closing it. Rotation preserves
uniformity, so it cannot improve the distribution; it can only add cost. The
useful half of the "rotating web" intuition is that a hole must close, and D21
closes it locally and for free, without anyone having to fly a circle.

**Nothing committed but this entry.** The experiment scripts are scratch; the
numbers above are the deliverable.
---

## D24 — Best-interceptor assignment: prototyped, measured, not adopted

Today's rule answers **"who is pointed at it"**, not "who can catch it":

```cpp
const uint32_t facing = FacingSlot(t.position, cfg_.asset, cfg_.fleet_size);
return UniqueOwner(facing, ...) == cfg_.drone_id;   // first live CLOCKWISE
```

Two things are wrong with it on paper. It scores by bearing rather than by
reachability, and when the facing drone is dead it walks clockwise **only**, so
a better-placed drone counter-clockwise never gets the track. Ownership is
exclusive, so if the drone it picks cannot catch the hostile, nobody else may
try — which is exactly the s2 29 s dead window in D21.

D22 gave us the right metric for free: `TimeToIntercept` returns the lead
solution's time, and −1 when a candidate cannot catch the target at all.

**Prototypes, all against the D22 baseline (sweep mean +99.1):**

| assignment rule | mean | min |
|---|---|---|
| current (bearing, first live clockwise) | **+99.1** | −246.8 |
| min time-to-intercept, scored from stations | +22.0 | −236.6 |
| …plus "must catch before the cylinder" | +22.1 | −236.6 |
| …plus hysteresis (a committed drone keeps its track) | +22.0 | −236.6 |
| min time-to-intercept, scored from heard poses | +21.0 | −457.1 |
| top **2** by time-to-intercept (drop exclusivity) | −30.8 | −450.7 |

**Not adopted.** Every variant is worse, and doubling up is worst of all — it
breaks s1 from 6/6 to 4/6.

**Is the current rule actually choosing badly?** Instrumented it to log, at
every commit, the committing drone's own time-to-intercept next to the best
available. Over s2, x2-b and x1-c: **optimal 9 times out of 15**, and most of
the misses are near-optimal (2.0 vs 1.7, 3.1 vs 2.4). Two are not:
`d2 tau=2.9 best=0.2` and `d3 tau=2.6 best=0.6`.

So the observation that a badly-placed drone sometimes takes the intercept is
**correct**. It is also mostly harmless, for a reason worth writing down: for a
hostile flying *radially* at the asset, the drone on the facing bearing is
usually the one nearest its corridor — bearing and catchability coincide, and
the clockwise walk lands a slot or two away, still near-optimal.

**Why fixing it does not pay.** Traced x2-b under both rules: each logs three
rams, yet the baseline scores 3/3 and the reassigned build 2/3 — the difference
is one `ram ... rng=2.6` followed by `abort not-closing`. Measured closest
approach across scenarios sits at **1–3 m against a 1.0 m kill radius**.
Intercepts turn on terminal miss distance, not on who flies them; changing the
assignment reshuffles geometry and flips marginal intercepts in both directions.
The ±1 kill per scenario in these rows is that noise, not a signal about
allocation quality.

**The confound, and what a correct version would need.** The two large
mis-assignments above are partly an artefact: the "better" drone was already
committed to another hostile, so it was never actually available. Neither the
diagnostic nor a naive min-tau rule knows who is busy — and a drone cannot know,
because D11 took `Claim` off the wire for bandwidth. **A real weapon-target
assignment needs claims broadcast again.** That is the honest prerequisite, and
it is untested.

**Where the points actually are:** terminal accuracy. Same conclusion as D22's
open item — a lead point that estimates target *acceleration* instead of
assuming constant velocity.

**Nothing committed but this entry.**
---

## D26 — Terminal guidance: zero-effort miss instead of proportional navigation

**First, a correction to D22 and D24.** Both said intercepts turn on "1-3 m of
terminal miss against a 1 m kill radius". That reading was wrong, and it came
from a broken instrument: a ram removes **both** entities, so a *successful*
kill also leaves a truncated 1-3 m closest approach in the trace. I was
measuring kills and calling them misses.

**The instrument, rebuilt** (`/tmp` scratch, not shipped): classify each hostile
against the report's `intercepts` list, measure closest approach only for the
ones that got away, and solve the CPA of each 10 Hz segment analytically rather
than taking the sample minimum — at 35 m/s of closing a sample is 3.5 m of
travel, larger than the kill radius. That gives a gradient the score does not:
the score moves in whole kills, "how close did we get to the ones we lost" moves
smoothly.

**What it says.** On the fixed set, only **2 hostiles escape at all**, both on
s2, at 9.3 m and 15.9 m — never engaged, not missed. There is nothing left to
win there. On 20 fresh ids: 11 escapes, of which **6 are near misses**
(1.67, 1.99, 2.25, 2.67, 3.92, 8.02 m; mean 3.42) and 5 were never engaged
(18-53 m). All six near misses are tier-2, which fits — 8% loss and latency
make a staler track and a worse lead point.

**Options measured** (fixed-set sweep mean as the guard, near-miss count on the
six fresh ids that lose hostiles as the gradient; baseline **+99.1** / 11
escapes / 6 near):

1. **Give the terminal law full authority** (fade only the midcourse leg, since
   `LimitAccel` clips the sum). 8 escapes, 3 near — but sweep mean **+67.5**.
2. **Reserve authority: cap midcourse at 0.7 / 0.5 of the lateral bound.**
   **6 escapes and zero near misses** — the accuracy problem simply goes away —
   but the sweep mean collapses to **+6.8 / +2.2**. Arriving gentler makes the
   hit easy and the interception late. A trade, not a win. Rejected.
3. **Zero-effort miss**: `a = N·ZEM/t_go²`, where ZEM is where the target would
   pass us if neither accelerated again. N=3 → 14 escapes; N=4 → 13; N=6 → 10;
   N=8 → 10; **N=10 → 9**; N=14 → 8 escapes but the sweep mean falls to +70.7.
   Escapes fall monotonically to N=10 with the score flat.

**Chosen:** 3, at N = 10.

**Why:** classic PN nulls the line-of-sight *rotation rate*, which is equivalent
to nulling the miss only while the closing rate is steady. After D22 ours is
not — the midcourse leg hands over still accelerating hard — so PN was solving a
slightly wrong problem at exactly the moment it mattered. ZEM steers at the
predicted miss directly and stays correct through the handover. It is the same
closed-form family, no extra state, no extra tuning beyond the one gain.

**Cost accepted:** tier-1 mean falls 145.9 → 127.2 on fresh ids, at **identical
31/31 kills and zero breaches** — so it is reward, not kills: the stronger
terminal steering pulls slightly off the lead point and engages a little later
where nothing was going wrong anyway.

**Measured, 8 fixed scenarios:** flat. mean 99.1 → 98.2, min −246.8 → −247.9,
max 276.7 → 276.4, same kills everywhere, civilians 2, wasted 0.

**Measured, 20 fresh ids:** mean **30.9 → 44.4**, min **−556.2 → −505.4**,
kills **54 → 56/65**, breaches **11 → 9**, tier-2 mean **−84.1 → −38.5**.

**Still open:** 5 of the 9 remaining escapes were never engaged at all (18-53 m
closest approach). Those are allocation or coverage, not accuracy, and D24 says
a real weapon-target assignment needs `Claim` back on the wire first.

**Determinism:** `--replay` clean on s1, s2, x1-a, x2-b. All three suites pass,
including `TestZeroEffortMissSteersAtTheMiss`.
---

## D27 — Allocation is not the constraint; the actionable window is

D24 left "a real weapon-target assignment needs Claim back on the wire" as the
next step, and D26 left five escapes that were never engaged at all. This is the
same instrument-first treatment applied to those.

**The instrument** (`/tmp` scratch, not shipped). For every hostile that got
away, walk the trace with **ground truth** — every drone's real position, which
no single drone has — and separate three questions:

* `seen_s` — how long was it inside somebody's sense radius?
* `feas_s` — how long did some live drone have a lead solution landing before
  the cylinder, using the same closed form the brain flies?
* `BOTH_s` — how long were those true *at the same time*? That is the only
  window in which any allocation rule could have acted.

**What it says**, over s2 and the six fresh tier-2 ids that lose hostiles:

| | escaped | seen_s | feas_s | **BOTH_s** |
|---|---|---|---|---|
| range across 11 escapes | 11 | 4.5 – 11.5 | 7.0 – 15.1 | **1.5 – 7.3** |

Every escape was actionable — **zero were physically uncatchable**. But the
window is 1.5–7.3 s, and `feas_s` exceeds `seen_s` almost everywhere: the
hostile is catchable well before anybody can see it.

**Options measured** (fixed-set sweep mean guard = **+98.2**, escapes on the
allocation bench = **11**):

1. **Fallback ownership.** Keep the bearing rule as primary (D24 showed
   replacing it costs 78 points) and let the best feasible drone take the track
   only when the bearing owner has no lead solution. **No change whatsoever** —
   the fallback never fires, because `TimeToIntercept` returns a solution for
   almost any ring drone against an inbound. "Has a solution" is not "can catch
   it in time".
2. **Fallback on the deadline** instead: the owner must land before the cylinder,
   at margins 1.0, 0.8, 0.6 of the time-to-go. **No change at any margin.**
3. **Fix the commit gate.** `ShouldCommit` still tested `t_meet = range/closing`
   — how long until the hostile arrives at *us*. That was the right model when a
   picket waited on station and was rammed, which is literally what it did
   before D22; since D22 we fly a lead point at max_speed, so it understates
   what we can reach. Replaced it with the lead solution, with and without the
   old `closing >= kMinClosing` precondition. **Bit-identical scores on all
   eight scenarios**, verified against the per-scenario totals and the loaded
   brain path, not just the summary.
4. **Push the ring out again.** D21 rejected a wider ring because a leaker past
   it could not be run down — which is exactly what D22's lead intercept fixed,
   so the trade deserved re-testing. It did not survive: fixed mean 98.2 → 49.0
   → −116.5 → −146.2 at F = 0.75 / 0.875 / 1.0, fresh tier-2 kills 25 → 20 → 18
   → 15. Tier-1 improves throughout (fresh mean 127.2 → 175.9), which is the
   same tier split as D21, now confirmed on top of D22 and D26.

**Nothing adopted. No code change.**

**Why four ownership variants and two commit-gate variants changed nothing:**
execution never reaches them. Logged the calls on a fresh id that loses 2 of 3:

```
hostile id21  spawns 29.2 -> gone 45.1    first "call hostile" 41.23   (3.9 s left)
hostile id22  spawns 47.8 -> gone 63.6    first "call hostile" 59.75   (3.9 s left)
```

Classification is **not** slow — first sensor contact to `call` is about 0.6 s.
The hostile is simply only inside sense range for ~4.5 s, having flown 220 m to
get there. There is barely a window to allocate *in*, so who owns it and what
the gate permits are both nearly irrelevant.

**The real constraint is sensor reach against a spawner that picks the bearing
furthest from any defender**, and the one geometric lever on it — a wider ring —
is the one tier-2's radio cannot carry. That is not a rule that needs fixing; it
is the shape of the problem.

**What I am NOT claiming:** that relaying would help. It propagates a detection
that already exists, and the hearsay `call trk=0` lines land within ~0.5 s of the
local call, so propagation is not the lag. First detection is geometry.
---

## D28 — The ring must sit well inside the circle hostiles enter on

**Found by inspection of a bad run, then confirmed by measurement.** On
`x1-72362d4ff5f79a18684e1706d8687322` the nearest picket sat at radius 99 and
the first hostile appeared at 127 — **27 m outside the ring**. It was inside the
picket before it could be called, and the run scored −519 at 2/5 with three
breaches.

`ring_radius_` was `asset_radius + comm_radius·0.625`, which is a *radio*
property and says nothing about how far out the threat starts. When a scenario
pairs a generous radio with a small arena the two are unrelated and the ring
lands almost on the spawn circle.

**The spawn radius is derivable.** `spawn.enemy_radius` is not in `SwBootInfo`,
but `arena_min`/`arena_max` are, and the ratio is stable:

| scenario | arena½ | spawn | ratio | ring | **ring/spawn** | score |
|---|---|---|---|---|---|---|
| x1-b | 232 | 185.6 | 0.80 | 92.8 | 0.50 | +276 |
| x2-b | 239 | 191.5 | 0.80 | 94.0 | 0.49 | +124 |
| x2-a | 184 | 147.5 | 0.80 | 72.6 | 0.49 | +179 |
| s1 | 200 | 170.0 | 0.85 | 86.2 | 0.51 | +230 |
| s2 | 250 | 220.0 | 0.88 | 76.9 | 0.35 | −248 |
| x1-a | 187 | 149.3 | 0.80 | 112.1 | **0.75** | −18 |
| x1-ae01dd… | 171 | 136.8 | 0.80 | 89.7 | **0.66** | **−395** |
| x1-72362d… | 158 | 126.6 | 0.80 | 99.1 | **0.78** | **−519** |

Every generated scenario spawns hostiles at **0.80 of the arena half-extent**;
the two hand-written ones are 0.85 and 0.88, so 0.80 is the conservative read
and the cap binds slightly early there rather than late. Every healthy run sits
near ring/spawn = 0.50; everything at 0.66 and above is a disaster.

**Cap swept** (hard = the two ids above; guard = the fixed sweep mean, +98.2):

| cap | x1-72362d… | x1-ae01dd… | sweep mean |
|---|---|---|---|
| none | −519.4 (2/5) | −395.4 (2/3) | **98.2** |
| 0.45 | +191.3 (5/5) | −139.8 (3/3) | 84.7 |
| 0.55 | +212.4 (5/5) | −147.5 (3/3) | 95.5 |
| **0.65** | **+226.3 (5/5)** | **−159.6 (3/3)** | **97.3** |
| 0.70 | +20.5 (4/5) | −395.4 (2/3) | 97.3 |

**Chosen:** 0.65. It is the largest cap that still bites on x1-ae01dd (whose
ratio is 0.66) — 0.70 misses it entirely and the run stays broken — while giving
away the least standoff elsewhere.

**Measured:** both bad runs go to **zero breaches and a full kill count**;
−519.4 → +226.3 and −395.4 → −159.6. The fixed sweep is untouched apart from
x1-a (−18.1 → −25.2, still 4/4), mean 98.2 → 97.3. On 20 fresh ids it is
**inert** — mean 44.4 → 44.6, identical kills and breaches — because the cap
only binds where the ring was already too far out.

**Cost accepted:** the 0.80 constant is measured from the generator, not read
from the scenario. If a future generator moved the spawn circle in without
moving the arena, the cap would be too loose. A `spawn.enemy_radius` field in
`SwBootInfo` would remove the guess entirely; the cap is a proxy for a number
the brain is not given.

**Determinism:** `--replay` clean on s1, s2, x1-a, x2-b and x1-72362d. All three
suites pass, including `TestRingStaysInsideTheSpawnCircle`, which pins both that
the cap bites on a small arena and that it does *not* bite on a large one.
---

## D29 — Score attribution in the viewer, and what the civilian penalty really is

**The question that started it:** several runs score badly with nothing visibly
wrong — full kill count, no breaches, no waste. `x1-fba99bede1412c0b8cb25f2b9b68cfe1`
is 4/4 hostiles destroyed, zero breaches, zero wasted, and scores **−456.3**.

**The scoring formula, verified rather than quoted.** Across nine runs spanning
both tiers it reproduces the reported mission score exactly:

```
mission = sum(intercept rewards) - 150*civilians - 200*breaches - 40*wasted
```

`build_scoring` re-checks this per run and publishes `verified`, so a future
rules change surfaces as `verified: false` rather than a silently skewed ledger.

**Where the points went.** `kill_radius` applies to **every pair of physical
objects with no exceptions** — the ABI header says so — civilians included. Two
civilians that drift together destroy each other, and we are charged 150 each.
Cross-referencing every civilian death against the frames:

| scenario | civilians lost | nearest friendly at death |
|---|---|---|
| x1-a | 2 | 65 m, 65 m |
| x1-fba99bede… | 4 | **t=0**, **t=0**, 138 m, 138 m |
| x1-19941165… | 2 | **t=0**, **t=0** |
| x1-8e0cbbb… | 1 | 2 m ← genuinely ours |

Over 28 runs: **9 civilian losses, 8 of them unavoidable — 1200 points** charged
for collisions our nearest drone was 65–138 m from, or that happened at t = 0.0
before the brain had issued a single command. On x1-a that penalty is the entire
difference between a −104 mission and a +196 one.

**This is not a bug to fix in the brain.** There is no action available: we do
not control civilian flight paths, and staying further away from them is already
what `separation_margin` does. The one attributable death (2 m) is the only one
worth chasing. What the brain *can* do is not be blamed for the rest, so the
viewer now separates them.

**What shipped:** `tools/build_viewer_data.py` gains a `scoring` block in
`run.meta.json`:

* `formula`, `weights`, and `totals` (with `verified`)
* `attributable` / `unattributable` — count and points either side of the line
* `ledger` — one entry per scoring event: `t`, `frame`, `kind`, `points`,
  `attributable`, `text`, and a `detail` object. Intercepts carry
  `urgency_ratio` and `forgone_by_engaging_late` (kill_max minus what we
  actually banked), which is the reward D22 exists to recover.
* `running` — cumulative mission score over time, for a graph.

Civilian attribution uses a 15 m blame radius against the last frame at or
before the death; a death with no prior frame is reported as such rather than
guessed at.

**Not a brain change.** `brain/src` is untouched, so determinism and the C++
suites are unaffected.
---

## D30 — Respacing: the death signal, not the rule

**The request:** rather than the neighbours of a hole sliding in, have *every*
drone keep equal distance from its neighbours — a local relaxation that should
even the ring out by itself, and needs no shared roster to do it.

**Tried exactly that first.** `EvenSpacingBearing`: take the bearings of the
neighbours we can hear that are actually holding station, find the nearest
either side, move toward the midpoint. Equal gaps is a fixed point, so a healthy
ring does not move. Guards: only on-ring neighbours count (or a drone sortieing
to intercept drags its neighbours off station behind it), and with fewer than
two visible neighbours fall back to the slot rule.

It does work as advertised — gap spread at six survivors improved from 45.0-71.6
to 52.6-75.5 degrees — and it looks good on the fixed sweep: mean **97.3 → 103.5**
with a better floor. Gain 0.5/1.0 and with/without the on-ring filter all landed
in the same place.

**It does not survive unseen data.** Over 20 fresh ids: mean **44.6 → 36.4**,
kills 56 → 55, breaches 9 → 10, tier-2 mean −38.0 → −56.1. And it re-broke
`x1-ae01dd1d7c29b46e2a45774c06baccbc`, one of the two runs D28 had just fixed
(−159.6 → −404.4, 3/3 → 2/3). I also anchored it against the free rotation mode
a Laplacian on a ring has — clamping the deviation from the nominal slot at
0.6 / 1.0 / 1.5 slots — and the hard case was **bit-identical** at every clamp,
so rotation drift was not what was hurting. **Rejected.**

The lesson is about method as much as about spacing: the fixed eight scenarios
are what I have been iterating against all session, so "better on the fixed set,
worse on fresh ids" is exactly the shape overfitting takes.

**What actually fixed it was one constant, and D21 already named it.** D21's
"cost accepted" said the death signal is shorter than the radio: `RingAlive`
counted silence as a death only inside `comm_radius − cruise·1.5 s − 10 m`,
allowing for a mate having flown out of range during the silence. On the ring
they never do — station-keeping is a few m/s, not cruise. That 31 m of slack
meant only the *immediate* neighbour ever registered (slot chords run 27 / 54 /
80 m against a 75 m radio), so each lip of a hole slid **half a slot** and the
gap D21 exists to close only half closed.

Use the radio itself: `d > comm_radius`. A mate whose last known position was
inside our radio has no innocent reason to be quiet.

| threshold | fixed min | fixed mean | fresh mean | fresh kills | fresh breaches |
|---|---|---|---|---|---|
| `comm − cruise·silent − 10` (was) | −247.9 | 97.3 | 44.6 | 56/65 | 9 |
| **`comm_radius`** | **−39.4** | **125.2** | **89.0** | **60/65** | **5** |
| `comm·0.85` | −26.5 | 124.5 | — | — | — |
| `comm − 10` | −75.7 | 99.5 | — | — | — |

**s2 finally reaches 5/6** (−247.9 → −39.4), the target set back in D21 and
missed by every redistribution scheme since. Fresh-id floor −505.4 → −309.5.
Both D28 hard cases hold at 5/5 and 3/3.

**Cost accepted:** tier-1 loses a kill and takes its first breach (31/31 → 30/31,
0 → 1 breach; tier-1 mean 127.2 → 106.3). Treating a silent in-range mate as
dead is occasionally wrong, and when it is, a live drone gets written off and
its neighbours close over a station that was never empty. Tier-2 gains far more
than tier-1 loses — overall fresh mean 44.6 → 89.0 — so it is taken.

**Determinism:** `--replay` clean on s1, s2, x1-a, x2-b and both hard ids. All
three suites pass; `TestStationBisectsTheGap` was updated to pin the new
behaviour (both near neighbours now register as dead, each lip slides a full
slot, and the 90° hole closes by two slots instead of one).
---

## D31 — Classification latency: the dive, and why calling early mostly does not pay

**Two questions first, both answered from the code.**

*Does repositioning block interception?* **No.** `Fly()` branches on
`stance == Committed` before anything else and hands straight to `ProNav`; the
reposition path only runs when not committed.

*Does repositioning corrupt detection or classification?* **No.** Every term in
`Classify` is measured against the **asset**, not against us —
`ApproachAlignment`, `RangeRate` and `ClosestApproachDistance` all take
`cfg_.asset`. Our own motion does not enter the evidence at all.

**What the latency actually is.** `kEvidenceForCall` is only **0.6 s**, so the
integrator is not the delay. The delay is the gate:

```cpp
bool AimedAtAsset(float miss, float miss_at_first, float asset_radius) {
    if (miss < kSureHit) return true;                              // 3D CPA < 5 m
    return miss < asset_radius && miss < miss_at_first - kShrink;  // or shrunk 3 m
}
```

`miss` is **3D**, and hostiles enter at 40 m altitude against an asset on the
ground, so a hostile flying level has a 3D CPA of about its own altitude — above
both branches. It only becomes "aimed" once the **dive** pulls the 3D miss down.
That is the wait, and it is the price of D7: a civilian overflight has a tiny
*ground* miss too, so the ground track alone cannot be the test.

**Measured on x1-ae01dd** (age since spawn):

| | vz (+ down) | ground miss | 3D miss |
|---|---|---|---|
| hostile, 0.5 s | **+1.67** | 0.1 | 52.6 |
| hostile, 2.0 s | +3.59 | 0.0 | 8.8 |
| hostile, 2.5 s | +3.69 | 0.1 | **2.8** ← called here |
| civilians (all, t=10 s) | **0.00 – 0.14** | — | — |

The dive is unambiguous at 0.5 s and civilians never produce it. The current
test waits until 2.5 s, out of a window about 4 s long.

**Options measured** (mean over the fixed 8, the 20 fresh ids, and the two
reported hard ids — 30 runs, weighted equally):

| | fixed 8 | fresh 20 | hard 2 | **all 30** |
|---|---|---|---|---|
| baseline | 125.2 | 89.0 | 33.0 | **94.9** |
| dive gate, unconditional (vz > 1.0) | **132.5** | 80.8 | **64.6** | **93.5** |
| …vz > 0.6 / vz > 2.0 | 132.8 / 131.2 | — | 66.8 / 55.9 | — |
| **dive gate, only when ttg < 10 s** | 127.4 | **89.2** | 33.0 | **95.7** |
| dive gate, only when ttg < 6 s | 125.2 | 89.0 | 33.0 | 94.9 (inert) |

**Chosen:** the dive path, gated on `TimeToCylinder < 10 s`.

**Why the unconditional version loses, which is the interesting part.** It is
the best variant on the fixed set *and* on both hard ids, and it is **net
negative overall** — the 20 unseen ids drop 89.0 → 80.8, a kill and a breach.
Declarations stay clean (1 wrong, 0 false accusations across the sweep), so it is
not a classification-quality cost. Calling earlier makes the owner *commit*
earlier, leave station earlier and stay away longer, and against a spawner that
enters where nobody is standing that costs more than the early call wins. The
dive is the answer to "I cannot afford to wait", not a replacement for the
patient test.

This is the second time in a row that "better on the fixed set and on
hand-picked bad runs, worse on fresh ids" has shown up (see D30). The fixed
eight are what every session has tuned against and the hard ids were chosen
*because* they were bad; only the fresh draw is an unbiased estimate.

**Measured:** fixed sweep mean 125.2 → **127.4**, floor unchanged at −39.4;
20 fresh ids 89.0 → **89.2** with identical kills (60/65), breaches (5) and
floor; both hard ids unchanged. Small, but it regresses nothing.

**Determinism:** `--replay` clean on s1, s2, x1-a, x2-b and both hard ids. All
three suites pass, including `TestDiveIsAHostileSignatureWhenTimeIsShort`, which
pins that a diving track on the cylinder opens the fast path while a level
civilian on the *same ground track* does not.

---

## D32 — Spawn-inside-sense: the TTG gate could not see the short window

**CHALLENGE.md does not promise a dive.** Hostiles spawn at
`spawn.enemy_altitude` (40 m on s1, 45 m on s2) against a ground-level asset;
nothing in the brief says they descend. The dive is emergent. On recorded
runs it is reliable *so far*: hostiles ramp past +1 m/s down within 0.5 s and
hold ~3.6; civilians sit at 0.00–0.14. D7 already refused to make that the
class, because a level dash would still breach the cylinder. D32 does not
either — it only uses the dive as a *clock* when the existing clock is lying.

**The 2.5–3 s wait on x1-814fd5e is not `kEvidenceForCall`.** That is still
0.6 s. D31 gated the dive on `TimeToCylinder < 10 s`. That function returns
**1e6 until horizontal closing exceeds 0.1 m/s**. Hostiles that spawn
*inside* `sense_radius` of the facing picket (this id: spawn 144 m, ring
~92 m, sense 60 m → 52 m gap) appear while still spooling up, so the gate
never opens and we wait for 3D miss to fall under 5 m — most of a ~7 s
window. Hostiles that enter sense already at dash have a real TTG and were
already calling in ~0.6 s. That is the "starts inside vs zooms in from
outside" split.

`ThreatWindow` uses dash speed (16 m/s) as the floor, so a standing spawn at
144 m is 6.8 s, not infinite. The fast path is then:

```
AimedAtAsset(3D miss)
|| (TimeToCylinder < 10 s && diving)          // D31, already at speed
|| (spooling && ThreatWindow < 8 s && diving) // D32, spawn-inside-sense
```

s1's inbound, once inside sense, is already at dash so it does not take the
new branch. The owner also *stalks* (slides up to 40 m toward the inbound,
still Picketing) only on that same spooling+compact signature, so we buy
acceleration without a full commit.

**What we did not ship.** Two committers on a short TTG: both went, then
`EnforceSeparation` held them 19 m apart and neither reached kill radius.
Unconditional dual also dropped s1 to 5/6. Handover (closer-than-owner) was
the same bet. Stalking every diving inbound with `ThreatWindow < 8 s`
*without* the spooling test looked compact at first sight on s1 (they are
first seen at ~146 m, 7.3 dash-seconds) and emptied sectors for the sixth
hostile. Stalk is spooling-only.

**x1-814fd5e79a980ae1c11a87a76f49336c:** −513.2 (0/3) → **−47.1 (2/3)**, one
breach, 0 waste, 0 civilians, 0 wrong. The remaining miss is not detection:
drones 6 and 7 both called at ~29 s with ttg 6.7–8.7 and still lost it at
34.55. Intercept geometry after a hole in an 8-drone ring, not the 2.5 s wait.

**Fixed 8** (D31 → D32):

| | D31 | D32 |
|---|---|---|
| mean | 125.2 | **139.6** |
| floor | −39.4 (s2, 5/6) | **−39.4 (s2, 5/6)** |
| s1 | ~6/6 | **237.5, 6/6** |
| x2-b | leaking | **147.8, 3/3** |

0 wrong declarations, 0 false accusations on the eight. x1-a still loses two
civilians at t=1.1 (same spawn overlap as before, no friendly death).

**Cost accepted:** the dive is still not in the spec. A later scenario that
flies level at 40 m will not take this path (D7). A later civilian that dives
will. `kDiveRate` 1.0 m/s is 7× the loudest civilian vz measured; that is a
measurement, not a promise. Did not re-run the 20 fresh ids this pass —
x1-ae01dd went 2/3 → 1/3 with 3 wrong, so the "better on the id you stared
at" trap is still live. The remaining breach on 814fd5e is an intercept, not
a call.

**Determinism:** `--replay` clean on s1, s2, x2-b. All three suites pass,
including `TestThreatWindowIsFiniteWhileSpooling`.

---

## D33 — EPN / MPC guidance: already flying the useful core; the rest does not pay

Mapped a prompt about Enhanced PN + MPC onto what we fly. We are not facing
supersonic targets; hostiles evade (s1: 3.5 m/s² weave inside 40 m). Fitting
that manoeuvre is tier 7.

**Already flying the useful subset:** midcourse is a one-step CV "MPC"
(`TimeToIntercept`); terminal is ZEM not classic PN (D25); stern chase is
refused at commit. Full receding-horizon MPC needs a weave model.

**Measured against D32** (mean 139.6, floor −39.4, s1 237.5 6/6, hard id
814fd5e −47.1 2/3) and reverted:

1. **Augmented PN** (`ZEM + ½ a_t t_go²` from Δv/dt). Fixed 8 wash (139.3).
   ae01dd 1/3 → 0/3. A weave is not constant accel; N=10 saturates the extra
   term immediately.
2. **Handover 40–70** so terminal is full at weave start. Mean 103.6, x1-c
   3/3 → 2/3, s1 237 → 210. ae01dd went 1/3 → 3/3 — D22's ridge plus the
   stared-at-id trap. 40 m is also a scenario comment.
3. **t_go = time of closest approach.** Mean 139.5, hard id bit-identical.
   Wash.

Remaining misses on the guard set are still not terminal accuracy (D26, D32).

---

## D34 — Stalk is the intercept, leashed, not a slide toward where they are

D32's stalk bought acceleration during the 0.6 s classify wait, but the
goal was `CorridorHorizon` — cruise along the LOS to the inbound's *current*
position, then `Cruise`/`GoTo` into a point 40 m off station. That is
pursuit. The committed law is a lead intercept. On a crossing or
still-spooling inbound the drone lined up on the current bearing, braked
into the leash, and ProNav then had to buy back the lead in the last few
seconds — the same geometry D22 already lost to on a sitting picket.

**Options considered**

1. Leave the slide; only start ProNav after the Hostile latch.
2. Keep `Cruise` into a *lead* point (`TimeToIntercept`), still stop at 40 m.
3. Fly ProNav at the inbound while Picketing, leashed to 40 m off station.
   Not intercepting, not exempt: separation still forbids a ram if it never
   latches. At the cap, hold the lead-leashed point so the airframe can
   reverse onto the slot.

**Chosen:** 3.

**Why:** the user was right that the initial step was flying at where they
are. The cap that was already there (40 m, stopping distance at max_speed
is ~30 m) is the "can still get out of it" bound; the missing piece was the
law. Same spooling+compact+owner gate as D32, so s1's already-at-dash
inbounds still do not stalk.

**Measured, 8 fixed scenarios:** bit-identical to D32. mean **139.6**, floor
**−39.4** (s2 5/6), s1 **237.5 6/6**, x2-b **147.8 3/3**. 0 wrong, 0 waste.

**x1-ae01dd** (the D32 regression): **1/3 −575 → 3/3 −75.6**, 0 wrong (was 3).
Asset survived. Two civilians unchanged (same spawn overlap as before).

**x1-814fd5e:** still 2/3, −47.1 → **−50.5**. Same leftover breach; the
hole-in-the-ring miss is not this wait.

**Cost accepted:** a diving compact spool that is *not* hostile still gets
40 m of ProNav. Civilians have not produced that signature (D7/D32). The
leash plus unknown-track arrest is the ram cap, not a promise they will
never close inside 4·kill.

**Determinism:** `--replay` clean on s1, s2, x2-b. Suites pass, including
`TestStalkAimLeadsNotPursues`.

---

## D35 — Neighbour's call is enough; receding facing yields clockwise

**x1-b40377e53070182f8f25674795f451b5**, hostile_1, breach at 37.11. Drone 0
called at 31.49 (local, 53 m, in sense). Drone 1 is the *facing* owner
(bearing 27° / 10 slots) and is sitting still on station, 67 m out — just
outside sense 60.7, inside comm 67.6. Drone 0 is sliding south at 5.5 m/s
(D19 respace after drone 2 died), receding from the inbound. It never
committed. Drone 1 committed at 32.61 when the hostile entered *its* sense,
1.1 s late, close=11, ttg=4.5, and missed by 14 m at the cylinder.

**What was already true, and what was not.** Who-goes is UniqueOwner
(facing, then clockwise). That already wanted drone 1. Velocity *is* in
`ClosingSpeed`, but `t_meet`'s `extra = cruise − toward` cancelled an
outbound velocity and treated a reverse as free. The closing ≥ 1 gate
uses real relative speed; the time-to-meet did not. Drone 0's motion in
the viewer is the respace, not an intercept.

**Why drone 1 sat.** A peer Hostile added `confidence/255` to
`closing_score`. A fresh call sends score·120 ≈ 72, so +0.28 per packet
against a 0.6 latch. Three 0.5 s reports, and 184 frames dropped to loss
on this run, so the first two never arrived. Drone 1's first Hostile line
was local, not `peer origin=0`.

**Chosen**

1. One Hostile report latches (at least `kEvidenceForCall`, and we now
   send confidence 255). The seer already paid the 0.6 s classify.
2. Burst the first 1.5 s at 0.1 s; the first frame is priority 6, above
   heartbeat, so it is not queued behind identity.
3. If the facing drone is receding from the inbound (toward < −2 m/s) and
   we can see them, UniqueOwner starts at facing+1. One skip only.
4. `t_meet` pays `|toward|/lateral` when toward is negative.

Still one interceptor. Hearsay commit was already legal (D18).

**Measured, x1-b403:** −399.6 2/3 → **−199.4 3/3**, asset survived.
Drone 1 commit **32.61 → 31.52** (`peer origin=0 hops=0`), 0.03 s after
drone 0's call.

**Measured, 8 fixed:** mean **139.6 → 164.6**, floor **−39.4 → 20.6**
(x1-a). **s2 5/6 → 6/6 (+163.4)** — the D18 leak (spent facing, successor
off-axis, catchable only once it is in sense). s1 237.5 → 237.0, still
6/6. x2-b 3/3. 0 wrong, 0 waste. ae01dd still 3/3.

**Cost accepted:** a false Hostile from a neighbour spends the facing
owner. UniqueOwner still only one drone. Tier 3 can replay a Hostile;
that was already the D18 cost, now one packet not three. Burst reports
nudge comms 39.5 → 39.4. Unseen receding facing still owns — missing a
skip is "facing goes" not "two go".

**Determinism:** `--replay` clean on s1, s2, x2-b. Suites pass, including
`TestInboundOwnerSkipsARecedingFacing` and a 72-confidence hearsay latch.
