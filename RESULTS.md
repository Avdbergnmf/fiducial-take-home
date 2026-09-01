# Results

Scores come from the report JSON of each run. Runs are deterministic from their
scenario id, and scores are bit-identical across machines and thread counts, so
every number here is directly comparable.

Generated ids were drawn with `--new-token`, not chosen by hand.

**Sweep set (8 scenarios, used for every row below):**
`s0, s1, s2, x1-a, x1-b, x1-c, x2-a, x2-b`

---

## Current brain

| scenario | total | hostiles stopped | breaches | friendlies lost (wasted) | awareness | detection | comms | notes |
| --- | ---: | :---: | :---: | :---: | ---: | :---: | ---: | --- |
| x1-b | **158.4** | 4/4 | 0 | 0 | 55.8 | n/a | 37.8 | best run |
| s1 | **130.0** | 6/6 | 0 | 0 | 54.4 | n/a | 35.5 | all six stopped, nothing wasted |
| s0 | 99.3 | 0/0 | 0 | 0 | 59.7 | n/a | 39.6 | no threats; awareness near max |
| x1-c | 94.2 | 3/3 | 0 | 0 | 50.6 | n/a | 38.0 | |
| x1-a | −246.4 | 4/4 | 0 | 1 | 35.3 | n/a | 34.7 | stopped all, but rammed 3 civilians |
| x2-a | −300.5 | 2/4 | 2 | 0 | 53.3 | n/a | 36.6 | tier 2: info does not cross the fleet |
| s2 | −501.9 | 3/6 | 3 | 0 | 58.0 | n/a | 32.9 | tier 2 |
| x2-b | **−524.6** | 0/3 | 3 | 0 | 52.6 | n/a | 22.8 | **worst case** — never commits |

**min −524.6 · mean −136.4 · max +158.4**

Score split (§9 defaults: `W_kill` 100 urgency-scaled, `P_breach` 200,
`P_waste` 40, `P_civilian` 150, `W_aware` 60, `W_comms` 40, `W_detect` 80).
Kill is the sum of intercept rewards, not 100 per stop — a late kill can be
worth 0. Detection is `n/a` / 0 everywhere: `declare_identity` is never
called, so the tier-5 term is zero by construction. That is deliberate — see
DESIGN.md.

| scenario | kill | breach | wasted | civilian | awareness | comms | detect | total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | :---: | ---: |
| x1-b | +64.9 | 0 | 0 | 0 | +55.8 | +37.8 | n/a | **158.4** |
| s1 | +40.2 | 0 | 0 | 0 | +54.4 | +35.5 | n/a | **130.0** |
| s0 | 0 | 0 | 0 | 0 | +59.7 | +39.6 | n/a | 99.3 |
| x1-c | +5.5 | 0 | 0 | 0 | +50.6 | +38.0 | n/a | 94.2 |
| x1-a | +173.6 | 0 | −40 | −450 | +35.3 | +34.7 | n/a | −246.4 |
| x2-a | +9.6 | −400 | 0 | 0 | +53.3 | +36.6 | n/a | −300.5 |
| s2 | +7.3 | −600 | 0 | 0 | +58.0 | +32.9 | n/a | −501.9 |
| x2-b | 0 | −600 | 0 | 0 | +52.6 | +22.8 | n/a | **−524.6** |

All friendly losses on the good runs are `pair_hostile`, which §9.2 does not
charge: one friendly per hostile destroyed is free. The only *wasted* loss in
the whole sweep is 1 drone on x1-a.

---

## How it got here — version ladder

Each row is a real commit, rebuilt from git history and swept over the same 8
scenarios. Deltas are on the **minimum**, because §11.2 says the floor is what
gets graded.

| ver | commit | change | min | mean | max | Δmin |
| --- | --- | --- | ---: | ---: | ---: | ---: |
| V0 | `5589171` | skeleton (untested draft) | −1546.9 | −895.3 | 39.6 | — |
| V1 | `80c589e` | D2 aimed-at-asset + CPA discriminant | −1093.5 | −687.6 | 39.6 | **+453** |
| V2 | `33f8984` | D3/D4 diagnostic logging | −1093.5 | −687.6 | 39.6 | 0 |
| V3 | `ad2cc9f` | D7 miss distance becomes 3D | −1037.9 | −611.0 | 39.6 | +56 |
| V4 | `dd447fe` | D8 friendly ID from heartbeats | −947.2 | −537.1 | 99.4 | +91 |
| V5 | `8ab1263` | D10 asset is a cylinder | −948.2 | −536.1 | 99.4 | −1 |
| V6 | `917346a` | fix: hostile calling restored | −947.2 | −537.1 | 99.4 | +1 |
| V7 | `2522c14` | D11 commit only a catchable intercept | −521.2 | −134.7 | 158.6 | **+426** |
| V8 | `2d75837` | D13 local tracks die with sensor picture | −700.0 | −211.4 | 158.4 | **−179** |
| V9 | `8664314` | D15 one owner per hostile + yield | −524.6 | −136.4 | 158.4 | +175 |
| V10 | `366fd4d` | current | −524.6 | −136.4 | 158.4 | 0 |

Total: **−1546.9 → −524.6 on the floor, −895.3 → −136.4 on the mean.**

Each version's *worst* report, split the same way. The floor is almost always
a breach bill; awareness and comms never move it by more than ~90 points.

| ver | worst | kill | breach | wasted | civilian | awareness | comms | total |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| V0 | s1 | 0 | −1200 | −80 | −300 | 0 | +33.1 | −1546.9 |
| V1 | x1-a | +42.9 | −600 | −120 | −450 | 0 | +33.6 | −1093.5 |
| V2 | x1-a | +42.9 | −600 | −120 | −450 | 0 | +33.6 | −1093.5 |
| V3 | s2 | +5.4 | −1000 | −80 | 0 | +0.6 | +36.2 | −1037.9 |
| V4 | s1 | +5.4 | −1000 | −40 | 0 | +52.9 | +34.5 | −947.2 |
| V5 | s1 | +5.4 | −1000 | −40 | 0 | +51.9 | +34.5 | −948.2 |
| V6 | s1 | +5.4 | −1000 | −40 | 0 | +52.9 | +34.5 | −947.2 |
| V7 | x2-b | 0 | −600 | 0 | 0 | +52.7 | +26.1 | −521.2 |
| V8 | s2 | +11.5 | −800 | 0 | 0 | +57.8 | +30.8 | −700.0 |
| V9 | x2-b | 0 | −600 | 0 | 0 | +52.6 | +22.8 | −524.6 |
| V10 | x2-b | 0 | −600 | 0 | 0 | +52.6 | +22.8 | −524.6 |

---

## What each step did

**V1 — the discriminant (+453 floor, +547 on s1).** The skeleton called a
civilian "enemy" and rammed it. Replacing the fixed 24 m gate with "sure hit, or
the miss distance has shrunk" removed civilian kills on 4 of 8 scenarios. s1
civilians went 2 → 0. This was the biggest single fix to the floor.

**V2 — logging (0.0, exactly).** Every one of the 8 scenarios scored identically
before and after. D3/D4 claim logging does not touch behaviour; this measures it
rather than asserting it.

**V3 — 3D miss (+56 floor).** Small overall, but +473 on x1-a alone. A chord that
misses by 5 m on the ground can be 30 m away in altitude. Only bites on layouts
where the geometry differs from s1, which is exactly what generated ids are for.

**V4 — friendly ID (+91 floor).** Matching heartbeats to sensor tracks let the
fleet label its own members. s0 jumped 39.6 → 99.4: that is the awareness term
switching on, worth ~60 points, with no intercept involved. It also removed the
last `pair_friendly` collisions on x2-b (2 → 0).

**V5 → V6 — a bug the score could not see (±2).** V5 accidentally stopped
declaring hostiles. Score moved by 1–2 points, so a sweep would never have caught
it. It shows up clearly in `wrong_declarations`, which swung 515 → 0 → 515 on
x1-b. Lesson: the total is not enough on its own.

**V7 — the commit rule (+426 floor, +1078 on s1).** The largest change in the
project. Only commit to a hostile we can actually catch: local track, fresh,
closing, within sensor range. s1 went from 1 kill and 5 breaches to **6 kills and
0 breaches**. Six of eight scenarios improved by 200–1078 points. **But x2-b got
worse by 204** — see worst cases.

**V8 — track drop (−179 floor).** Dropping local tracks the moment they leave the
sensor picture cost 200+ points each on s2, x1-a and x2-a, and gained nothing
anywhere. s1 was unchanged, so the tight loop on s1 could not see it. This is the
clearest example of why the sweep exists.

**V9 — one owner per hostile (+175 floor).** A deterministic owner per hostile,
with the others moving off the intercept line. Recovered almost exactly what V8
lost (+198 s2, +200 x1-c, +200 x2-a). Net effect of V8+V9 together is roughly
flat on the floor, but the fleet stopped converging on the same target.

**V10 — current (0.0).** The last commit is viewer-side only. Measured, not
assumed.

---

## Worst cases

| scenario | kill | breach | wasted | civilian | awareness | comms | detect | total |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | :---: | ---: |
| x2-b | 0 | −600 (3×) | 0 | 0 | +52.6 | +22.8 | n/a | **−524.6** |
| s2 | +7.3 | −600 (3×) | 0 | 0 | +58.0 | +32.9 | n/a | −501.9 |
| x2-a | +9.6 | −400 (2×) | 0 | 0 | +53.3 | +36.6 | n/a | −300.5 |
| x1-a | +173.6 | 0 | −40 | −450 (3×) | +35.3 | +34.7 | n/a | −246.4 |

**x2-b, −524.6 — the floor, and a direct cost of V7.** Every point on the floor
is the three-breach bill. The fleet stops nothing (0 of 3), takes 3 × −200,
and loses no drones at all. Awareness and comms give +75.4 back, which does
not touch the mission hole. That combination means it never commits: the
commit rule added in V7 is strict enough that on this layout nothing ever
clears the bar. Before V7 the same scenario scored −317 with 1 kill. **The
change that won +1078 on s1 cost 204 here.** That trade is the single thing
I would revisit first — the commit rule needs a fallback for when no drone
qualifies, rather than everyone standing down.

**s2 and x2-a, −501.9 and −300.5 — tier 2, not implemented.** Kill rewards are
tiny (+7.3 and +9.6): the intercepts that do happen are late. The score is
the breach bill (−600 and −400). Hostiles arrive far from whoever can see
them. `comms.propagation_p95_s` is `null` for this brain, meaning nothing we
transmit crosses more than one hop. Both scenarios take breaches that the
fleet had the information to prevent but could not move. This is a known gap,
not a tuning problem.

**x1-a, −246.4 — the last civilians.** This is the only worst case that is
*not* a breach story. All 4 hostiles stopped, 0 breaches, and the kill term
is the best in the sweep (+173.6). Then 3 civilians (−450) and one wasted
drone (−40) wipe it out. The discriminant still fires on a chord that passes
very close to the asset. Awareness is also lowest here (35.3), which is the
same root cause showing up in a second metric.

---

## Saved artifacts

`runs/ablation/` is local only (gitignored). Each run is a full recording plus
the viewer sidecar, left in place — nothing is copied into StreamingAssets.

```
scripts/versions.csv                version → commit, expected min/mean/max, worst
runs/ablation/<ver>/                rebuilt reports (summary.csv, log.txt, meta.json, *.json)
runs/ablation/<ver>/<id>.{jsonl,bin,meta.json}   if you passed -Trace / -TraceAll
```

`.json` is the score report, `.jsonl` is `--trace`, `.bin` + `.meta.json` are
what the viewer loads. Rebuild any ladder row with `scripts\ablation.ps1`
(see below). Point the viewer's extra-run folder at `runs\ablation\` or a
version subfolder. Do not copy into StreamingAssets.

`runs/history.csv` holds the 27 live runs recorded during development by
`scripts\iterate.ps1`. It is incomplete — several runs were not committed — which
is why the ladder above was rebuilt from git rather than read out of it.

---

## Reproducing the ladder

The table is `scripts\versions.csv` (version key, commit, expected min/mean/max,
worst id). Rebuild one row without checking out that commit — only `brain/src`
is archived into a scratch tree and compiled against today's SDK:

```
powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -List
powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7
powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -Version V7 -Trace
powershell -ExecutionPolicy Bypass -File scripts\ablation.ps1 -All
```

Output is `runs\ablation\<ver>\`: per-id `*.json` reports, `summary.csv`,
`log.txt`, `meta.json`. `-Trace` adds jsonl + viewer `.bin`/`.meta.json` for
the worst id. That folder is gitignored; point the viewer's extra-run folder
at it if you want to look. A drift warning means the live score is more than
0.15 off the CSV (RESULTS.md's table).

Sanity check: V10 (HEAD-era brain) should still print s1 = 130.0.
