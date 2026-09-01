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

