# Viewer data format

```
swarm_sim --trace run.jsonl  ->  tools/build_viewer_data.py  ->  run.bin + run.meta.json  ->  viewer
```

Python owns every data job: parse the JSONL, expand deltas, convert NED to a
left-handed y-up frame, derive events. The viewer (Unity, three.js, whatever)
loads two files and draws. It never parses the raw trace, never sees NED, and
never expands a delta.

Generate:

```
python tools/build_viewer_data.py runs/fixture.jsonl runs/fixture
python tools/plot_fixture.py
```

That writes `runs/fixture.bin`, `runs/fixture.meta.json`, and
`runs/fixture_check.png`. The PNG is a sanity check, not a viewer input.
`fixture.bin` is gitignored (binary, regenerable); `fixture.meta.json` is
committed so the contract has a real example.

---

## `run.bin`

Little-endian float32, C-order, frame-major, fixed stride.

```
stride      = 11          # floats per entity per frame
offset(f,s) = ((f * slot_count) + s) * 11
nbytes      = frame_count * slot_count * 11 * 4
```

| index | contents |
|---|---|
| 0..2 | position xyz |
| 3..5 | velocity xyz |
| 6..9 | rotation xyzw |
| 10   | alive (`1.0` or `0.0`) |

One slot per entity for the whole run. Outside `[first_frame, last_frame]` the
slot is zeros with `alive = 0`. One `File.ReadAllBytes` (or `fetch` +
`Float32Array`) on the viewer side; no per-frame parsing.

`slot_count`, `frame_count` and `stride` are in the sidecar meta, not in the
binary. The binary is a blob of floats and nothing else.

---

## `run.meta.json`

Everything sparse or non-uniform. Field order is the order below.

| field | meaning |
|---|---|
| `format_version` | `1` |
| `source` | basename of the `.jsonl` |
| `scenario` | scenario id (`s1`, `x1-…`) |
| `dt` | simulator step, seconds (0.01) |
| `trace_hz` | recorded frames per second (10) |
| `frame_count` | number of `frame` records |
| `slot_count` | number of entity slots |
| `stride` | `11` |
| `duration` | time of the last recorded frame, seconds |
| `arena.min/max` | axis-aligned box, **already in viewer coordinates** |
| `asset.position/radius` | same |
| `fleet_size` | friendly count at boot (`header.fleet_size`) |
| `entities[]` | see below |
| `events[]` | derived; sorted by `t` |
| `links[]` | radio-link **intervals**, not deltas |
| `beliefs[]` | one row per declaration change, sorted by `t` |
| `logs[]` | `t`, `drone`, `text` |
| `telemetry[]` | `t`, `drone`, `bytes_sent`, `budget_remaining` |
| `report` | the trace's final `report` record, verbatim |

### `entities[]`

| field | meaning |
|---|---|
| `slot` | index into the binary, `0 .. slot_count-1` |
| `trace_id` | id from the recording |
| `kind` | `friendly` \| `hostile` \| `civilian` \| `wreckage` \| `unknown` |
| `drone_id` | friendly's `drone_id`; `-1` otherwise |
| `first_frame` / `last_frame` | inclusive indices into the binary |
| `compromised_from` | first frame the ground-truth flag is set, or `-1` |

The recording calls hostiles `enemy` and civilians `neutral`. Those names are
mapped here so the viewer has one vocabulary. The mapping is the only place
those recording names appear.

### `events[]`

Nothing in the trace is tagged "event". The sidecar derives them.

| field | meaning |
|---|---|
| `t` | seconds |
| `frame` | index of the last recorded frame at or before `t` |
| `kind` | see below |
| `severity` | `0` info · `1` expected loss · `2` bad · `3` worst |
| `slots` | entities involved (may be empty) |
| `text` | one line for a log / tooltip |

| `kind` | source | severity |
|---|---|---|
| `spawn` | entity appears between frames | 0 |
| `death` | entity disappears, not paired as a collision | 0 (wreckage) or 1 |
| `intercept` | two deaths same frame, close enough, friendly × hostile | 0 |
| `friendly collision` | same, friendly × friendly | 2 |
| `civilian collision` | same, anyone × civilian | 3 |
| `collision` | same, any other pair | 1 |
| `report_breach` | `report.events[]` | 3 |
| `report_*` | any other `report.events[].type` | see code |

A breach **cannot** be derived from geometry: the hostile is removed on reaching
the asset, which looks like any other disappearance. The report is authoritative
for kills and breaches. Where geometry and the report disagree, the report wins
and the sidecar prints the gap. Geometry still supplies spawn/death timing and
attaches report events to slots (the report names drones and hostiles, never
entity ids).

Collision pairing uses `header.kill_radius` plus one recorded frame of closing
speed. The recording is 10 Hz; a lot happens between frames.

### `links[]`

| field | meaning |
|---|---|
| `a`, `b` | **drone ids**, not slots (`a < b`) |
| `t_start`, `t_end` | interval during which the pair can hear each other |

The recording sends `add` / `remove` deltas. The sidecar holds the last state
(the initial "no links" state is never sent) and writes closed intervals.
Still-open links close at `duration`.

Map `a`/`b` to slots via `entities[].drone_id`.

### `beliefs[]`

| field | meaning |
|---|---|
| `t` | seconds |
| `observer` | **drone id** of the brain that declared |
| `slot` | entity the declaration is about |
| `class` | `unknown` \| `friendly` \| `enemy` \| `neutral` \| `compromised` |

Deltas: one row per change. Hold the last value per `(observer, slot)`; the
initial "nothing declared" state is never sent. Class names are the recording's
`belief_classes`, not the viewer's `kind` names — `enemy`/`neutral` here, not
`hostile`/`civilian`.

**Unverified on the fixture.** The example brain never calls `declare_track`,
so this fixture has zero `beliefs` records. The path is untested until a brain
that actually declares is recorded.

### `logs[]` / `telemetry[]`

Logs are whatever the brains wrote through `host->log`, full rate, plus the
simulator itself at `drone == -1` (log-budget overflow). Telemetry is 1 Hz,
one row per drone, `bytes_sent` cumulative and `budget_remaining` instantaneous.

---

## Coordinate conversion

The sim is NED, right-handed, z down. The viewer frame is left-handed, y up
(Unity / three.js default). Conversion happens in **exactly two functions** in
`tools/build_viewer_data.py` and nowhere else:

```
position/velocity:  viewer = (ned.y, -ned.z, ned.x)
attitude:           ned (w,x,y,z) -> viewer (x,y,z,w) = (-qy, qz, -qx, qw)
```

The quaternion conversion is a basis permutation plus a handedness flip. It is
**silent when wrong**: aircraft still move correctly and only their banking
looks off. Treat it as unverified until a visual check in the viewer (a drone
commanded to yaw should yaw, a drone banking into a turn should roll the way
the trail curves). If it is wrong, fix those two functions and rebuild the
`.bin` — do not compensate in the viewer.

Arena `min`/`max` are converted corner-wise and then re-sorted, because NED
`min.z` is the ceiling (`-120`) and becomes viewer `max.y` (`+120`).

---

## How to load this in Unity

No Unity code lives in this repo yet. The viewer should do this and nothing
more:

1. Drop `run.bin` and `run.meta.json` in `Assets/StreamingAssets/` (or load
   from an absolute path in the editor). `StreamingAssets` is the one folder
   `File.ReadAllBytes` can see in a build.
2. Parse the meta with Newtonsoft.Json (or `System.Text.Json`). Unity's
   `JsonUtility` cannot handle this schema — nested lists, a verbatim
   `report` object, no wrapper class.
3. `File.ReadAllBytes(binPath)` once. `Buffer.BlockCopy` into a `float[]`
   of length `frame_count * slot_count * 11`.
4. At scrub time `t`, `frame = clamp(floor(t * trace_hz), 0, frame_count-1)`.
   Optionally lerp to `frame+1` with the fractional part.
5. For each slot with `alive > 0.5`:

   ```
   i = (frame * slot_count + slot) * 11
   position = (data[i], data[i+1], data[i+2])
   rotation = (data[i+6], data[i+7], data[i+8], data[i+9])   // Quaternion(x,y,z,w)
   ```

6. Draw the rest from the meta: arena box, asset sphere, `events` on a
   timeline, `links` as line segments between the two drones' current
   positions while `t_start <= t <= t_end`, `logs`/`telemetry` on the
   selected drone, `beliefs` as a per-observer overlay, `compromised_from`
   as ground truth.

Playback rate is `trace_hz` (10), not `dt` (the sim's 100 Hz). Default view:
arena + asset + aircraft coloured by `kind`, trails on, everything else
toggleable. Colour means one thing: friendly / hostile / civilian / wreckage.

three.js is the same two files: `fetch` the bin as `ArrayBuffer`, wrap in
`Float32Array`, same index arithmetic. Positions are already y-up.

---

## What the real schema did that the prompt did not

Written against `notes/schema.md` / `runs/fixture.jsonl`. No fallbacks for
field names that are not in this recording.

| Specified / guessed | Actual |
|---|---|
| frame rows as objects | arrays, columns named by `header.entity_columns` |
| class names `hostile` / `civilian` | `entity_classes`: `friendly`, `enemy`, `neutral`, `wreckage` |
| `kill_radius` from `--dump-params` | also on the `header` (`1.0`) |
| telemetry as per-drone objects | parallel arrays `tx_bytes` / `tx_budget`, indexed by drone id |
| links as current pairs | `add` / `remove` of `[drone_a, drone_b]` |
| beliefs as current map | `set`: `[observer_drone, entity_id, belief_class]` |
| ~1800 frames / 180 s on s1 | **890 frames / 88.9 s** on this fixture — the example brain lets every hostile through and the sim ends after the last breach. `s1.json` `time_limit` is 180; that is the cap, not the length of a lost run. |
| `beliefs` records present | **absent**. Example brain never declares. |
| `report.mission.intercepts` populated | `[]` on this fixture. Shape of a hit is therefore untested; the whole `report` is copied through so a later run does not need a parser change. |

Header `entity_columns`, in order:

```
id, cls, drone, px, py, pz, vx, vy, vz, qw, qx, qy, qz, compromised
```
