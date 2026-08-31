# The loop

The process, in order, with the exact commands. Follow it literally when tired.

Everything runs from the repo root in PowerShell. Every script takes
`-ExecutionPolicy Bypass -File` because unsigned local scripts are blocked by
default, and every path is quoted because the repo path has spaces in it.

---

## 0. Before you change anything: know your starting numbers

You cannot tell whether a change helped if you never wrote down where you were.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build.ps1
powershell -ExecutionPolicy Bypass -File scripts\determinism.ps1
powershell -ExecutionPolicy Bypass -File scripts\sweep.ps1
Copy-Item -Recurse -Force runs\sweep runs\sweep_baseline
```

**Why determinism first.** A non-deterministic brain makes every later
measurement meaningless — you would be reading noise and calling it signal, and
you would not find out until much later. It takes 30 seconds now. (It passes
today: 4/4 as of the last check.)

**Why a baseline copy.** `compare.ps1` needs a "before". Copying the whole
directory means you can diff a whole sweep later, not just one run.

---

## 1. The tight loop — change ONE thing, run ONE scenario, look

```powershell
powershell -ExecutionPolicy Bypass -File scripts\iterate.ps1 -Scenario s1 -Note "what I changed"
```

That single command builds, runs s1 with `--trace` and `--report`, converts the
trace for the viewer, copies it into StreamingAssets, appends a row to
`runs\history.csv`, and prints the score.

Add `-Fast` to skip the viewer half when you only want the number — it is
noticeably quicker, and most iterations only need the number.

**Why one thing at a time.** Two changes in one run and you have learned nothing
about either. The runs are deterministic and take a few seconds, so there is no
reason to batch.

**Why the `-Note`.** It lands in `history.csv`. In six hours you will not
remember which run was which, and the note is what turns a list of numbers into
a story you can put in DESIGN.md.

**What to read, in this order:**

1. `RUN FAILED` / crashes / ABI violations — nothing else matters if this fires.
2. `losses by cause` — see the surprise list below.
3. `kills` and `breaches` — the mission term, which dwarfs everything else.
4. `total` — last, because it is a sum and sums hide their causes.

---

## 2. When to widen to a sweep

Widen when the tight loop says a change helped **and you are about to keep it**.
Not before — a sweep is 10× the wall time and you do not need it to reject an
idea that already failed on s1.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\sweep.ps1
powershell -ExecutionPolicy Bypass -File scripts\compare.ps1 runs\sweep_baseline runs\sweep
```

**What to look at first, in order:**

1. **The completion count.** `sweep.ps1` prints it before any score and shouts
   in red if a report is missing. A run that crashed or hung matters more than
   any number — CHALLENGE.md §11.2 says to read it first, and the exit status
   reflects it.
2. **The worst row.** The table is sorted worst-first for exactly this reason.
   §11.2: "look at your worst run rather than the average, because a mean hides
   the one layout your brain breaks on." You are graded on the floor.
3. **Whether the worst row got worse** even though the mean improved. That is
   the classic bad trade and `compare.ps1` prints `worst run` as its own line.

Once a day, draw fresh ids rather than reusing the same ones:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\sweep.ps1 -Tier 1 -Count 6
```

**Why.** §11.1: "a token you picked is a layout you will quietly fit to, and the
run we grade you on is one you have never seen." `-Tier`/`-Count` draws its
tokens with `--new-token`, so you cannot cheat by accident.

---

## 3. Viewer, or report?

**The report is enough when** you are asking *how much* — did the score move, did
the losses drop, did kills go up. That is most of the day. Use `-Fast`.

**Open the viewer when you are asking *why*, and specifically:**

- Losses you cannot explain from the cause breakdown. `pair_friendly` tells you
  two of yours collided; only the geometry tells you they were converging on the
  same target from opposite sides.
- A hostile that got through while drones were nearby — a picket problem is a
  shape, and shapes are not visible in JSON.
- Anything involving *who could hear whom*. The links layer is the fastest way
  to see a message that never crossed the fleet.
- Before you write a DESIGN.md paragraph about behaviour. Do not describe
  behaviour you have only inferred from a score.

```powershell
powershell -ExecutionPolicy Bypass -File scripts\iterate.ps1 -Scenario s1   # no -Fast
```

Then open the Unity viewer; `iterate.ps1` has already synced the data.

---

## 4. Where what you learn gets written down

The design document is worth as much as the code and is much harder to
reconstruct afterwards (§14). Do not leave it to Monday night.

- **`notes/decisions.md`** — one entry per decision you could have made
  differently: options, what you chose, why, what it costs. Write it *when you
  make the decision*, in two minutes, while the reasoning is live.
- **`RESULTS.md`** — the scores table. Fill it from
  `powershell -ExecutionPolicy Bypass -File scripts\history.ps1`, which prints
  best-so-far per scenario. Include the generated ids and your worst cases.
- **`DESIGN.md`** — assembled at the end from `decisions.md`. If an entry is in
  decisions.md you already have the paragraph.
- **`notes/provenance.md`** — when you tune a constant from a guess to something
  derived or measured, move its row. That table is the direct answer to "why is
  this number 0.8?", which you *will* be asked.

Check the trend rather than a feeling:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\history.ps1 -Scenario s1
```

---

## 5. How to know when to stop tuning something

Stop when any of these is true:

- **Two changes in a row moved the number by less than ~5%.** You are in the
  noise of your own judgement. Take the better one and move on.
- **The gain is smaller than the next goal on the ladder.** Tuning a threshold
  for +20 while an unimplemented feature is worth +300 is procrastination with
  extra steps. Check `notes/brain-goals.md`.
- **You cannot say why the change helped.** A number that improved for reasons
  you cannot explain will not survive a new layout, and you cannot defend it on
  Tuesday. Revert it or understand it.
- **The worst-case run did not move.** You are fitting to the layout you are
  staring at.

Write down where you stopped and why, in `decisions.md`. "I stopped tuning the
miss gate at 18 m because the next two steps were worth under 10 points each" is
a *good* interview answer. It shows you were spending attention deliberately.

---

## If the score surprises you, look here first

In order. Grounded in CHALLENGE.md §9.2, which says this is where a naive swarm
bleeds and to look there first when a score surprises you.

1. **`losses by cause`** — printed by `iterate.ps1` and `sweep.ps1`, and the
   real field is `mission.losses_by_cause` (the brief calls it
   `friendlies_lost_wasted`, but that field is a scalar; the per-cause breakdown
   is the separate `losses_by_cause` object). Read the key:
   - `pair_neutral` — you rammed a civilian. **−150 plus −40, the most expensive
     single mistake in the game.** It means your classifier called a civilian
     hostile *and* your commit rule believed it. Currently happening on 5 of 8
     scenarios.
   - `pair_friendly` — two of yours collided. −40 each, so −80 a time, and it is
     usually two interceptors converging on one target from opposite sides.
     "Three drones per hostile intercepts well and then loses the spare two to
     each other" (§9.2).
   - `pair_hostile` — this one is **free** if it was the collision that killed
     the hostile: one friendly per hostile destroyed is not charged (§9.2).
     Check `friendlies_lost_credited` before you treat it as a problem.
   - `wreckage` — you flew into debris. Every collision leaves lethal wreckage
     (§2.1), so a successful intercept makes that airspace *more* dangerous.
   - `ground` / `arena` — a flight bug, not a tactics bug. Look at
     `EnforceArena` and the altitude handling, and remember NED has z down.
2. **`hostiles_reached_asset`** — each breach is −200, twice what stopping it
   early would have earned. Six breaches is −1200 before anything else happens.
3. **`asset_survival_time_s`** — when the *first* breach happened. If it is
   early, your picket never formed; if late, you nearly held.
4. **`correct_declarations` vs `wrong_declarations`** — the awareness term
   averages +1/−2/0 per sample and clamps at 0, so wrong ≫ correct scores
   exactly the same as saying nothing. But the same wrong beliefs feed the
   commit rule, which is how a bad classifier turns into a dead civilian.
5. **`integrity.brain_crashes` and `abi_violations`** — should be 0. A crash is
   reported as the loss of that drone and will cost you.
6. **`comms.propagation_p95_s`** — `null` means nothing your fleet says crosses
   more than one hop. On s2 that is the difference between an intercept and a
   breach (§9.3).
