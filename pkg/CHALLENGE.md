# Swarm Defence Challenge

You are writing the onboard software for a swarm of interceptor drones.

Your code runs on every drone in the fleet, independently. Each drone sees only its own surroundings, talks only to neighbours over a short-range radio, and decides for itself what to do. Together they have to stop hostile drones from reaching a defended asset — without destroying the friendly and civilian aircraft mixed in among them.

This is an architecture problem more than a control problem. Flight control is largely solved for you.

---

## Table of contents

1. [Quick start](#1-quick-start)
2. [The mission](#2-the-mission)
3. [What you can and cannot know](#3-what-you-can-and-cannot-know)
4. [Conventions: frames and units](#4-conventions-frames-and-units)
5. [Interface reference](#5-interface-reference)
6. [The radio](#6-the-radio)
7. [Threats](#7-threats)
8. [Scenarios](#8-scenarios)
9. [Scoring](#9-scoring)
10. [Rules and constraints](#10-rules-and-constraints)
11. [Running the simulator](#11-running-the-simulator)
12. [What to submit](#12-what-to-submit)
13. [How we evaluate](#13-how-we-evaluate)
14. [Suggested approach](#14-suggested-approach)
15. [FAQ](#15-faq)

---

## 1. Quick start

From the root of the package, so the simulator finds `scenarios/` beside it:

```bash
cmake -S examples -B examples/build && cmake --build examples/build --config Release

bin/swarm_sim --scenario s0 --brain examples/build/brain.so
bin/swarm_sim --scenario s1 --brain examples/build/brain.so --report run.json --trace run.jsonl
```

On Windows that is `bin\swarm_sim.exe` and `examples\build\Release\brain.dll`. See `README.md` in the package.

The fleet climbs to 30 m, holds position and chatters. That is `examples/hover_relay_brain.cpp`, your starting point, and it scores about zero.

Your deliverable is one shared library built against one header, `sdk/include/swarm_abi.h`. You do not link against the simulator and you do not get its source.

There is no GUI. The simulator writes a JSON score report and an optional recording. **Building a 3D visualiser for that recording is a graded deliverable** — see [§11.3](#113-build-a-3d-visualiser). Start it early.

---

## 2. The mission

An asset sits at the centre of the arena. Hostile drones approach it. If one reaches it, you have lost the asset.

Your only weapon is the airframe: closing to within `kill_radius` destroys both aircraft, so every interception costs you a drone.

Three kinds of aircraft share the airspace, and they are **physically identical** — same airframe, same limits, same appearance to your sensors:

- **Friendly** — the other drones running your code.
- **Hostile** — trying to reach the asset, and it will evade you.
- **Civilian** — uninvolved.

Nothing in your sensor feed tells you which is which. There is no transponder, no marking, no flag in the data. Working out who is who *is* the challenge.

Four ways to lose, pulling against each other: let a hostile through and the asset is gone; ram a civilian and take a heavy penalty; ram a friendly and you have spent two drones for nothing; never commit and the first case gets you.

### 2.1 Collisions

**The kill radius applies to every pair of aircraft, with no exceptions** — friendly-friendly and friendly-civilian included. No closing-speed threshold, no grace period, no friend-or-foe logic in the physics. Formation separation has to be a hard guarantee rather than a tendency, and two interceptors converging on one hostile are also converging on each other.

**Every collision leaves wreckage:** a tumbling box with the pair's combined mass and momentum, lethal on contact for the same radius, falling until it hits the ground or times out. It appears in your track list with nothing marking it as debris. A successful intercept makes that piece of airspace more dangerous, not less.

---

## 3. What you can and cannot know

**Exactly, with no noise:** the position, velocity and attitude of every aircraft within `sense_radius`, as seen from where you believe you are. Also your own attitude, angular rate and acceleration, and the simulation time.

**With error:** your own position fix, which is off by a little and drifts, slowly and independently on every drone. `SwSelfState::fix_sigma` publishes how wrong it is worth assuming you are. The `range` and `bearing` your receiver reports for an incoming frame are measurements too, with their own sigmas in `SwRxFrame`.

**Not at all:** what class anything is. Whether two aircraft seen at different times were the same one. Anything outside `sense_radius`. Which teammates are alive. What a teammate thinks, unless it tells you.

**Not published, on purpose:** packet loss, radio latency, how long a track survives out of range, and how long the mission runs. All four vary between missions, so anything that needs a number for one of them has to measure it in flight. What *is* published is in `SwBootInfo`, and it is the whole of what you are given.

**Death is silent.** A destroyed drone stops transmitting and vanishes from every track list. Nobody is notified, `fleet_size` never changes, and there is no liveness flag. A peer that goes quiet might be dead, out of range, behind a lossy link, or being interfered with — indistinguishable from where you sit. Logic that waits on an acknowledgement a dead drone will never send waits forever.

**`track_id` is local to you.** Two drones observing the same aircraft assign different ids, so ids are not names you can exchange. An id is retired once the aircraft has been out of range long enough, and a different one issued if it returns.

**Behaviour is your only real evidence,** and reading it usually takes sustained observation from more than one viewpoint. The drone best placed to see something is frequently not the one best placed to act.

---

## 4. Conventions: frames and units

Sign errors here will cost you an afternoon.

- **World frame NED**, right-handed: `x` North, `y` East, `z` **Down**, origin at the arena centre on the ground. Altitude is `-z`, so 30 m up is `z = -30`. Gravity is `+z`.
- **Body frame FRD**: `x` forward, `y` right, `z` down. Thrust acts along body `-z`.
- **Attitude** is a unit quaternion `SwQuat{w,x,y,z}` rotating body FRD into world NED, normalised, `w >= 0`. No Euler angles appear in the interface in either direction.
- **Angular rates** are body frame `(p, q, r)` in rad/s.
- **SI units throughout.** No degrees anywhere.
- **The accelerometer reads specific force**, so a stationary level drone reports about `(0, 0, -9.81)` in body frame, not zero.

---

## 5. Interface reference

Field-by-field documentation is in `swarm_abi.h`. This is the shape of it.

### 5.1 What you implement

```c
void* create (const SwHost* host, const SwBootInfo* boot);
void  tick   (void* self, const SwObservation* obs, SwCommand* cmd);
void  destroy(void* self);

SWARM_EXPORT const SwBrain* swarm_brain_v1(void);
```

`create` runs once per drone before the episode; return your state pointer and it comes back on every later call. `tick` runs once per control step.

The same library is instantiated for every friendly. Instances differ only in `boot->drone_id` and `boot->rng_seed`. There is no leader binary and no central coordinator — if you want one, elect it.

### 5.2 Why nothing needs linking

Calls go through function pointers in both directions, so your library has no unresolved externals and builds against the header alone:

```bash
g++    -shared -fPIC -std=c++17 -I sdk/include brain.cpp -o brain.so
clang++ -shared      -std=c++17 -I sdk/include brain.cpp -o brain.dll
cl /LD /std:c++17 /I sdk\include brain.cpp
```

The boundary is plain C, which is why your compiler need not match ours.

### 5.3 What you observe

`SwObservation`, once per tick: time, `dt`, your own state, the track list, received frames, remaining transmit budget.

Two things in the header repay a close read. `SwSelfState` gives you **two independent sources** of information about your own motion. And `SwRxFrame` carries, alongside the payload, the **measured range and bearing** to whatever transmitted it, each with a published sigma — measurements your receiver made, not claims the sender made.

**All observation pointers are borrowed** and valid only for that `tick`. Copy anything you keep.

### 5.4 What you command

Fill in `SwCommand`. The simulator runs the inner cascade and you may enter it at any level:

| Mode | You supply | When to use it |
|---|---|---|
| `SW_CMD_VEL_NED` | velocity, heading | Simplest. Fine for transit. |
| `SW_CMD_ACCEL_NED` | acceleration, heading | The natural output of a guidance law. Start here. |
| `SW_CMD_ATTITUDE` | quaternion, thrust | If you want your own attitude logic. |
| `SW_CMD_BODY_RATE` | body rates, thrust | Tightest tracking, most work. |
| `SW_CMD_MOTOR_PWM` | four values in [0,1] | Full control, no safety net. |

Every level saturates against the limits in `SwBootInfo`, and there is no actuator delay. **`max_accel` is not what you can pull sideways.** Horizontal acceleration comes from tilting, so it is bounded by `g·tan(max_tilt)` — around 6.7 m/s² where `max_accel` reads 15. Size your intercept geometry on the tilt bound; a hostile has the same one. `VEL_NED` and `ACCEL_NED` compensate gravity for you; in `ATTITUDE` and `BODY_RATE`, thrust is yours. The inner loop is well tuned but has finite bandwidth, which only shows in the last metre or two against something actively dodging. Most candidates should use `ACCEL_NED` and spend their time elsewhere.

### 5.5 Services available to you

From `SwHost`:

- `broadcast(data, len)` — the radio. See [§6](#6-the-radio).
- `declare_track(track_id, class)` and `declare_identity(pubkey, class)` — tell us what you currently believe. **Scoring hooks only:** no effect on the world, invisible to other drones, not readable back. They are scored separately and are not interchangeable: tracks feed the belief term, and **an insider only counts if you name it with `declare_identity`**, by key. Keep them current; awareness is scored even when the intercept fails. They also come back out in the recording, next to the truth.
- `random(out, len)` — seeded. Use instead of `rand()`.
- `log(text)` — diagnostics, timestamped and tagged with `drone_id`. They go into the recording ([§11.3](#113-build-a-3d-visualiser)), and to stderr if you pass `--verbose`.
- `sign`, `verify`, `agree`, `kdf`, `key_import`, `seal`, `unseal`, `hash` — Ed25519, X25519, HKDF, ChaCha20-Poly1305, SHA-256, in scenarios that need them. You design the protocols; we supply the primitives. Unavailable services are `NULL`.

`SwBootInfo` carries your own public key, the mission authority's, and the fleet roster by `drone_id`. It carries **no** group or session key — deriving and rotating those is part of the problem.

---

## 6. The radio

One primitive: `broadcast(bytes, len)`, delivered to every drone within `comm_radius`. No unicast, addressing, routing, acknowledgement, retransmission or connections. If a message has to reach a drone out of range, something in between has to choose to forward it — **multi-hop is what you build.** The payload is opaque bytes; the simulator never looks inside.

| Property | On `s1` | Across missions | Told to you |
|---|---|---|---|
| Range | 90 m nominal, loss ramping to certainty over the last 10 m | 60–120 m | `comm_radius` |
| Maximum payload | 256 bytes per frame | fixed | `mtu` |
| Sends per tick | 1 | fixed | — |
| Transmit budget | 4096 bytes per second, rolling | roughly 1–9 kB/s | `tx_budget_bytes_per_s` |
| Latency | 2 ticks, plus 0 to 1 of jitter | 1–5 ticks, plus 0 to 3 | **no** |
| Packet loss | 2% independently per receiver | 0–20% | **no** |
| Sensor range, for comparison | 60 m | always shorter than the radio | `sense_radius` |

**The `s1` column is one mission, not the specification.** Every row is a per-mission parameter, and the six fixed scenarios already disagree with each other — `s2` onward run at 75 m and 8% loss. Loss and latency are the two you are never told, so a retransmission or flooding scheme has to measure the link it is actually flying rather than size itself on a number from this page.

At `s1`'s budget that is sixteen full frames a second, so rebroadcasting everything you hear exhausts it immediately; `SwObservation::tx_budget_bytes` tells you where you stand. Loss is real and unacknowledged unless you build acknowledgement — and a peer failing to forward looks much like a peer that never received. You can hear further than you can see.

---

## 7. Threats

Expect all of this. None of it is a surprise we are hiding.

**Hostiles are competent.** Same airframe and limits as you, so a stern chase by a single interceptor against an alert evader does not converge. They evade on a pattern drawn from a family whose parameters vary between missions: learnable within a mission, not hardcodable between them.

**Hostiles listen.** They hear everything you send in the clear within range and act on it. Traffic that reveals which drone is going after which target should be expected to be used against you.

**Hostiles lie.** In later scenarios they transmit traffic shaped like yours, including your own earlier traffic replayed back. A frame arriving at your antenna proves only that something transmitted it.

**One of your own drones may be compromised, mid-mission.** This is the centrepiece:

> A compromised drone still runs *your* code, so it speaks your protocol perfectly: it completes your handshake, forwards traffic, joins whatever consensus you designed, and signs with a legitimate key. What has been subverted is the boundary around your software — what its sensors report, and what its actuators actually do. It may sincerely believe things that are not true and act on them in good faith. Its keys are in hostile hands.
>
> Some runs have no compromised drone. Some have two.

A design that lets one drone's report condemn another can be turned against your own fleet by an insider holding a valid key and a false picture of the world; swarms have finished missions having destroyed mostly each other. A design that can never conclude anything about a peer watches an insider work unopposed.

Three things worth knowing before you build a detector. The rest is yours to work out.

- Nothing **local** to the compromised drone gives it away. Its own measurements agree with each other.
- Your own position fix is not exact either, and neither is any peer's — see [§3](#3-what-you-can-and-cannot-know).
- Naming a healthy peer costs you more than naming the real one earns.

Detection is not the hard part on its own. The drone that notices is rarely the one that can reach it, the two are often not in radio contact, and the clock is the hostile's approach. And having named an insider you have to mean it — a fleet that keeps relaying its heartbeats and honouring its claimed targets has changed a line in the report and nothing else.

---

## 8. Scenarios

Each rung adds one thing and they are scored independently, so a subset done well ranks better than everything gestured at. **Do not try to solve it all at once.**

| Scenario | What it adds | Crypto needed |
|---|---|---|
| `s0` | Formation flight and waypoints. No hostiles. Sanity check. | No |
| `s1` | Hostiles that never transmit. Civilians present. Identify by behaviour, allocate, intercept. | No |
| `s2` | Hostiles entering far from whichever drone first sees them. Information has to travel to whoever can act. | No |
| `s3` | Hostiles that impersonate you, including replaying your own captured traffic. | Yes |
| `s4` | Hostiles that exploit what they overhear. | Yes |
| `s5` | A compromised friendly, mid-mission — or none, or two. | Yes |

`s0` through `s2` need no cryptography and already contain the core of the problem. `s5` is the top of the ladder and carries the most weight, because it is the only rung where the fleet has to reason about itself.

These six are fixed layouts. Each has an unlimited supply of randomised variants — [§11.1](#111-scenario-ids) — and you should be testing against those too.

---

## 9. Scoring

`--report` writes the full breakdown. The formulas and their weights are published so you optimise what we actually measure. These are the defaults; a scenario may override any of them, and `--dump-params` prints what a fixed id resolved to.

| | | |
|---|---|---|
| `W_kill` 100 | `P_waste` 40 | `P_civilian` 150 |
| `P_breach` 200 | `W_aware` 60 | `W_detect` 80 |
| `P_false_accuse` 120 | `W_comms` 40 | `detect_deadline` 30 s |

### 9.1 The main term: how fast you stop each hostile

```
reward = W_kill * clamp(1 - t_engage / t_free, 0, 1)
```

`t_free` is how long that hostile would need to reach the asset unobstructed; `t_engage` is spawn to destruction. Kill it early for nearly the full `W_kill`, kill it on the asset's doorstep for almost nothing. Let it through and you pay `P_breach`, twice what stopping it early would have earned.

A breach does not end the run: the hostile is removed, the asset is charged, and remaining arrivals still come, because stopping five of six is not the same as stopping none. `mission.asset_survival_time_s` records the first breach, so how long you held is scored as well as how many got through.

Normalising by `t_free` means an urgent threat and a leisurely one are worth the same, which rewards pushing your engagement envelope outward. A hostile still airborne at the end scores zero — no penalty, no credit.

Don't be surprised if you get negative scores. Depending on the scenario this can make sense, and still be a good score.

### 9.2 The main penalty: drones lost for nothing

One friendly per hostile destroyed is **free**: if the collision that killed a hostile also killed your drone, that loss is not charged. Every other loss costs `P_waste`.

| Cause of loss | Charged? |
|---|---|
| Collided with a hostile | No — this is what the drone is for |
| Collided with another friendly | Yes |
| Collided with a civilian | Yes, plus the much larger civilian penalty |
| Flew into wreckage | Yes |
| Hit the ground | Yes |
| Left the arena | Yes |

This is where a naive swarm bleeds. Three drones per hostile intercepts well and then loses the spare two to each other and to the debris. `friendlies_lost_wasted` breaks it out per cause; look there first when a score surprises you.

### 9.3 Secondary terms

**Situational awareness** comes from `declare_track`. Once a second, for every aircraft you are tracking, we take **only your most recent** declaration: correct `+1`, wrong `−2`, unknown or undeclared `0`. Calling a compromised friendly "friendly" is incomplete rather than wrong and scores nothing either way. The average is clamped into `[0, W_aware]`, so guessing at everything bottoms out at zero rather than in debt — but zero is the whole term, and it is the same score you would get by saying nothing at all.

**Compromise detection** pays `W_detect * clamp(1 - latency / detect_deadline)` to the first drone that names a real insider by key, timed from the moment it was compromised. Naming it again is worth nothing. Naming an innocent costs `P_false_accuse` **once per identity**, however many of your drones say it and however often — and naming the real insider *before* it turned counts as innocent. Unlike the belief term this one is not clamped, so a fleet that turns on itself can run up an unbounded bill.

**Communication efficiency** is frugality on top of having communicated: `W_comms * clamp(1 - bytes_per_drone_per_s / tx_budget)`, and zero for a fleet that never transmits at all. Saying less than you need to is rewarded; saying nothing is not efficiency. How fast information actually crosses the fleet is reported as `comms.propagation_p95_s` but does not enter the score — it is worth watching anyway, because on `s2` it is the difference between an intercept and a breach.

**Compute** is measured against 2000 µs per tick and reported, but **not scored** — it is the one number that would depend on the machine. Being slow still costs you in review, and you will be sitting next to the exam run while it happens.

The secondary terms together are worth a fraction of the mission score. Do not trade an intercept for them.

---

## 10. Rules and constraints

Your `tick` must not:

- **Read the wall clock.** Use `obs->time`.
- **Spawn threads.**
- **Touch files, sockets, environment variables or any other outside channel.**
- **Use `rand()` or any unseeded randomness.** Use `host->random`.
- **Share state between brain instances**, by globals, statics, shared memory or any other route. Graded runs put each instance in its own process, so it will not work — and attempting it is grounds for rejection.
- **Hang.**
- **Allocate memory the simulator must free**, or hold `SwObservation` pointers past the call.

Encouraged: any C++17 inside your library, any data structures, heap allocation in `create`, vendored header-only libraries, as much internal state as you want.

Crashes are contained and reported as the loss of that drone. They will cost you.

---

## 11. Running the simulator

```
swarm_sim [options]

  --brain PATH                      your shared library (required)
  --scenario ID                     which mission to run   (default s1)
  --report FILE                     write the score breakdown as JSON
  --trace FILE                      write a recording of the run as JSON Lines
  --trace-hz N                      frames per second in it (default 10)
  --record FILE                     write a per-tick state hash trace
  --replay FILE                     re-run and verify against a hash trace
  --dump-params [FILE]              print the resolved parameter set and exit
  --set key=value                   override a parameter
  --threads N                       worker threads; 1 forces serial
  --new-token                       print a fresh random scenario token
  --verbose                         print your brains' log output to stderr
  --quiet                           suppress per-event logging
  --help                            the full list
```

A four-minute mission takes a few seconds, so the loop is: run, look, change, run.

`--record` and `--trace` are different despite the similar names. `--record` writes one state hash per tick to prove two runs matched; there is nothing in it to look at. `--trace` writes the run.

`--dump-params` opens up a fixed scenario completely — every radio, sensor and scoring number it resolved. It is **refused for generated ids**, which are the ones you are graded on, so treat it as a way to understand the machine rather than a set of constants to build against.

### 11.1 Scenario ids

**A scenario id is the complete description of a mission.** There is no seed flag: the same id gives the same mission, down to the packet loss pattern.

| Form | What it is |
|---|---|
| `s0` … `s5` | The six fixed scenarios in [§8](#8-scenarios). These never change, so a score difference between two builds is your code and nothing else |
| `x<tier>-<token>` | A **generated** mission. `tier` is 0 to 5 and selects the same mechanisms as the matching fixed scenario; `token` is any string, and it determines the layout |

So `x1-a` and `x1-hello` are two different `s1`-class missions: same rules and difficulty band, different fleet size, spawn geometry and radio conditions.

**Use these heavily.** Something that scores well on all six fixed layouts and falls apart on one it has not seen is a worse result than a lower score that holds everywhere. Sweep a few dozen per tier and read the worst case, not the mean. Draw your tokens with `--new-token` rather than choosing them: a token you picked is a layout you will quietly fit to, and the run we grade you on is one you have never seen.

Results and scores are bit-identical between `--threads 1` and `--threads 8` and across machines, so totals compare directly. A run is fully reproducible from its id, so debugging something at t=94 means rerunning and looking at t=94.

### 11.2 The test loop

Single runs mislead, because the interesting failures are intermittent and layout-dependent. Run batches instead:

```bash
swarm_sim --sweep --brain brain.dll --scenarios s1,x1-a,x1-b,x1-c --report-dir out/ --jobs 4
```

That writes `out/<id>.json` per run and prints how many finished, the mean, and a line for each one that did not. Because runs are deterministic, `--jobs` only changes how long the sweep takes, never what it produces.

Read the completion count before you read any score. A run that crashed or hung matters more than a bad number, and the exit status reflects it. Then look at your **worst** run rather than the average, because a mean hides the one layout your brain breaks on.

Summarising a sweep beyond that is your business. The reports are JSON, one per run, and what you choose to track says something about what you think matters.

**Check your brain is deterministic**, early. Record a run's per-tick state hashes and replay them with a different thread count:

```bash
swarm_sim --scenario x1-a --brain brain.dll --threads 1 --record run.hash
swarm_sim --scenario x1-a --brain brain.dll --threads 8 --replay run.hash
```

The second command fails if anything diverged. A mismatch means non-determinism in your brain — usually a wall-clock read, unseeded randomness, uninitialised memory, or iterating a container whose order depends on allocation addresses. It is much easier to find now than to debug later.

### 11.3 Build a 3D visualiser

**This is a graded deliverable and a significant part of your evaluation.**

A score tells you a run went badly. It will not tell you that your picket left a bearing uncovered, that two drones went after the same hostile while a third sat idle, or that the drone which first called a peer compromised was itself the compromised one. Those are questions about geometry over time, and the only way to answer them is to look.

So `--trace FILE` writes the run, and we expect you to build something that shows it in 3D:

```bash
swarm_sim --scenario x5-8f2c91 --brain brain.dll --trace run.jsonl
```

Two reasons we ask. It is how you will understand your own fleet — everyone who has done this problem well built one early, and the ones who did not spent the week reading logs and guessing. And it is close to the real work here: presenting the behaviour of an autonomous system so a human can judge it is a large part of the job, and what you choose to draw tells us what you were reasoning about.

Use any stack you like — three.js, WebGL, native OpenGL, a game engine, Python with a 3D library. Aim for something you would be happy to drive in front of us: the arena and the asset, aircraft in 3D at their true attitude, and where they have been. The recording carries a full attitude quaternion per aircraft, so you can show an airframe banking into a turn rather than a dot sliding around a plane.

Two views are worth having, and the difference between them is the tier-5 problem: **ground truth**, and **what your fleet believed**. The `compromised` flag in the recording is ground truth your brain never sees, so you can put your detection next to the answer.

Bring it to the interview.

**The format.** JSON Lines — one self-describing object per line, `type` first, so a loop and your language's JSON reader are enough.

| Line | What it carries |
|---|---|
| first, `header` | `dt`, `trace_hz`, arena bounds, the asset, fleet size, and the column names for frame rows |
| `frame` | every entity at one instant: id, class, drone id, position, velocity, attitude quaternion, and whether it is compromised in ground truth |
| `links` | which pairs of your drones can currently hear each other, as `add` and `remove` against the previous state |
| `beliefs` | what each drone has declared about each entity, as changes |
| `log` | whatever your brains wrote through `host->log`, with the drone and the time |
| `telemetry` | bytes sent and budget remaining, per drone, once a second |
| last, `report` | the whole `--report` document, so the file stands alone |

Frames are decimated: 100 Hz simulation, 10 Hz recording by default, raised with `--trace-hz`. Links and beliefs are **deltas**, so hold the last value you saw; a declaration stands until replaced, and the initial "nothing declared" state is not sent. Log records are not decimated — you get every line, in drone order. Nothing is written until the run ends — the file is created at the start so a bad path fails immediately, then stays empty, so do not tail it. Skip record types you do not recognise.

**Everything in the file should be reachable from your viewer.** Every record type in that table is there because it answers a question the geometry alone cannot, and if one of them is in the recording and nothing on screen ever shows it, that is evidence you had and did not use. Concretely, that means your own log lines shown against the drone that wrote them and the moment it wrote them, so you can read what a drone was thinking beside where it was — usually the fastest way to find out why it did something. It means the radio links drawn as who could actually hear whom, which is how a message failing to cross the fleet stops being a mystery. It means beliefs shown **per observer** and not only as the fleet's latest word, because two drones disagreeing about the same aircraft is the interesting case and an average hides it. And it means the per-drone bandwidth, so a drone that has talked itself out of budget is visible as a cause rather than discovered later in the report.

**Make it intuitive.** This matters as much as coverage, and the two pull against each other: a viewer that puts everything on screen at once is a data dump, and a data dump answers no questions. Someone who has not seen your viewer before should be able to follow a run after one sentence of explanation. So give it a clear default picture and let the detail be summoned, select a drone and see its state, its log and its bandwidth; toggle a layer on when you want it. Keep colour meaning one thing throughout, and say somewhere what it means. Make time controllable: scrub, pause, step, and slow down, because everything worth seeing here happens over seconds and involves several aircraft at once. We will ask you to drive it and find something in a run you have not seen, so the test is whether it is quick to answer a new question with, not whether it renders every field.

The log budget is generous but finite. If a recording reaches it the file says so, on a `log` record with `drone` set to `-1`, and takes no more.

---

## 12. What to submit

1. **Source code** for your brain, plus build files. It must build from a clean checkout on Linux or Windows.
2. **A design document**, 2 to 5 pages. This carries real weight: module structure and why, your wire protocol and how it is versioned, how you decide what an aircraft is, how you treat a peer you cannot verify, what you do about a compromised member, what you would do with another week, and what you know is unfinished.
3. **Your 3D visualiser**, plus a recording it shows off well. See [§11.3](#113-build-a-3d-visualiser). Bring it and expect to drive it.
4. **Your tests.** How you tested a distributed system interests us more than coverage numbers.
5. **A short results summary**: which ids you ran and what you scored, including the generated ids you swept and your worst cases.

Tell us what you did not get to. An honest list of gaps reads far better than silence.

---

## 13. How we evaluate

Roughly equal weight on:

- **Architecture** — module boundaries, protocol separated from policy separated from flight code, whether the design survives a new requirement.
- **Scores**, across many ids including ones you have not seen. We care more about the floor than the mean.
- **Insight and presentation** — your visualiser and design document, and whether you can show us what your fleet did and why.
- **Judgement under ambiguity** — this document does not tell you what to do about a peer you cannot verify. We want the reasoning written down and the code matching it.
- **Engineering practice** — protocol versioning, failure handling, testability, readability.

Deliberately open, with no correct answer: how far to trust a plausible but unverifiable peer, how many independent observations justify acting, whether to spend a drone to resolve an ambiguity, how to divide limited bandwidth, and when to break formation for something that is not yet a threat.

---

## 14. Suggested approach

1. Build and fly the example brain. Confirm your toolchain.
2. **Build your visualiser.** You will use it for everything after this.
3. Replace the hover with real guidance: fly a waypoint, then intercept a non-manoeuvring target. Clear `s0`.
4. Classify by behaviour from your own sensors and start emitting `declare_track`. See how far one drone gets alone.
5. Design your wire format and version it from the first byte. Get two drones sharing observations, remembering that `track_id` means nothing to the receiver.
6. Allocate targets between drones that disagree.
7. Move information across the fleet in more than one hop, inside your byte budget. That is `s2`, and it is the heart of the challenge.
8. Only then consider authentication.

**Write the design document as you go.** It is worth as much as the code and much harder to reconstruct afterwards.

---

## 15. FAQ

**Do I get the simulator source?** No. One header, prebuilt binaries and this document. If something looks undocumented or contradictory, ask — the interface is the contract, and gaps in it are our bug.

**Can I write it in something other than C++?** Anything that can export a C symbol and build a shared library. C++17 is what the example uses.

**Can I use third-party libraries?** In your brain: header-only, vendored into the submission, and no unresolved externals beyond the C runtime; anything needing a separate build or system package, ask first. In your visualiser: whatever you like.

**One brain instance per drone, really?** Yes — independent instances of the same code, in separate processes when graded. Any centralised design has to be elected and maintained over the radio.

**How much time should this take?** `s0` to `s2` is a solid few hours for a good engineer. The full ladder is intentionally larger than the time you have; prioritising is part of the exercise, and depth on a subset beats breadth across all of it.

**Are the physics realistic?** Realistic enough. Rigid-body quadrotor, quadratic drag, saturating actuators. Not a flight-dynamics exercise.

**Can I cheat by reading simulator memory or sharing globals between drones?** Graded runs isolate each brain in its own process, so most of it will not work, and attempting it ends the interview.

**Something looks like a simulator bug.** Tell us, with the scenario id and a `--record` trace. Finding one reflects well on you.
