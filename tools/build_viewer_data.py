#!/usr/bin/env python3
"""build_viewer_data.py -- swarm_sim trace -> run.bin + run.meta.json

    python tools/build_viewer_data.py runs/fixture.jsonl runs/fixture

Python owns all data work: parsing, delta expansion, coordinate conversion and
event derivation. The viewer reads two files and renders. It never parses the
raw trace, never sees NED, and never expands a delta.

Written against the header in the JSONL itself. Field names are taken from
that record (entity_columns in particular), and there are no fallbacks for
names this format does not use.

Output is engine-agnostic: run.bin is a flat float32 array and run.meta.json is
plain JSON. Nothing here is Unity-specific except the coordinate convention,
which is documented in FORMAT.md.
"""

import argparse
import json
import os
import sys
from datetime import datetime, timezone

import numpy as np

FORMAT_VERSION = 1
# floats per entity per frame, Unity axes: px py pz, vx vy vz, qx qy qz qw, alive
STRIDE = 11

# Trace class name -> the kind names the viewer uses.
KIND_OF_CLASS = {
    "friendly": "friendly",
    "enemy": "hostile",
    "neutral": "civilian",
    "wreckage": "wreckage",
}

# Severity is "how bad is this", 0..3. See FORMAT.md.
SEV_INFO, SEV_LOSS_EXPECTED, SEV_LOSS_BAD, SEV_LOSS_WORST = 0, 1, 2, 3


# ---------------------------------------------------------------------------
# Coordinate conversion. THE ONLY TWO PLACES THIS ARITHMETIC APPEARS.
# Sim is NED, right-handed, z down. Unity is left-handed, y up.
# ---------------------------------------------------------------------------

def ned_to_unity_vec3(x, y, z):
    """Position or velocity. NED (north, east, down) -> Unity (right, up, fwd)."""
    return (y, -z, x)


def ned_quat_to_unity(qw, qx, qy, qz):
    """Attitude. NED (w,x,y,z) -> Unity (x,y,z,w).

    A basis permutation plus a handedness flip. This is SILENT when wrong: the
    aircraft still move correctly and only their banking looks off. Unverified
    until checked visually in Unity -- see FORMAT.md.
    """
    return (-qy, qz, -qx, qw)


# ---------------------------------------------------------------------------
# Parsing
# ---------------------------------------------------------------------------

def read_trace(path):
    """Stream the JSONL once. Returns a dict of the record types we understand."""
    out = {"header": None, "frames": [], "links": [], "beliefs": [],
           "logs": [], "telemetry": [], "report": None}
    skipped = 0
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            line = line.strip()
            if not line:
                continue  # trailing blank line is normal
            try:
                rec = json.loads(line)
            except ValueError as exc:
                skipped += 1
                warn("line %d is not valid JSON, skipping (%s)" % (lineno, exc))
                continue
            kind = rec.get("type")
            if kind == "header":
                out["header"] = rec
            elif kind == "frame":
                out["frames"].append(rec)
            elif kind == "links":
                out["links"].append(rec)
            elif kind == "beliefs":
                out["beliefs"].append(rec)
            elif kind == "log":
                out["logs"].append(rec)
            elif kind == "telemetry":
                out["telemetry"].append(rec)
            elif kind == "report":
                out["report"] = rec
            # else: unknown record type, skipped on purpose (CHALLENGE.md 11.3)
    if skipped:
        warn("skipped %d unparseable line(s)" % skipped)
    if out["header"] is None:
        die("no header record in %s" % path)
    if not out["frames"]:
        die("no frame records in %s (the trace is written at the end of a run, "
            "so an empty file means the run did not finish)" % path)
    return out


def warn(msg):
    print("warning: %s" % msg, file=sys.stderr)


def die(msg):
    print("error: %s" % msg, file=sys.stderr)
    raise SystemExit(1)


# ---------------------------------------------------------------------------
# Slots
# ---------------------------------------------------------------------------

def build_slots(header, frames):
    """One slot per trace entity id, in order of first appearance.

    Frame rows are arrays; their meaning comes from header["entity_columns"],
    so nothing here hardcodes a column position.
    """
    cols = {name: i for i, name in enumerate(header["entity_columns"])}
    classes = header["entity_classes"]

    slots = {}       # trace id -> slot index
    entities = []    # slot-indexed metadata
    for f_idx, frame in enumerate(frames):
        for row in frame["entities"]:
            tid = int(row[cols["id"]])
            cls = classes[int(row[cols["cls"]])]
            if tid not in slots:
                slots[tid] = len(entities)
                entities.append({
                    "slot": len(entities),
                    "trace_id": tid,
                    "kind": KIND_OF_CLASS.get(cls, "unknown"),
                    "drone_id": int(row[cols["drone"]]),
                    "first_frame": f_idx,
                    "last_frame": f_idx,
                    "compromised_from": -1,
                })
            ent = entities[slots[tid]]
            ent["last_frame"] = f_idx
            if ent["compromised_from"] < 0 and int(row[cols["compromised"]]) != 0:
                ent["compromised_from"] = f_idx
            if KIND_OF_CLASS.get(cls, "unknown") != ent["kind"]:
                warn("entity %d changed class %s -> %s at frame %d; keeping the first"
                     % (tid, ent["kind"], cls, f_idx))
    return cols, slots, entities


def fill_array(frames, cols, slots, entity_count):
    """(frame_count, slot_count, STRIDE) float32, converted to Unity axes."""
    arr = np.zeros((len(frames), entity_count, STRIDE), dtype=np.float32)
    for f_idx, frame in enumerate(frames):
        for row in frame["entities"]:
            s = slots[int(row[cols["id"]])]
            px, py, pz = ned_to_unity_vec3(row[cols["px"]], row[cols["py"]], row[cols["pz"]])
            vx, vy, vz = ned_to_unity_vec3(row[cols["vx"]], row[cols["vy"]], row[cols["vz"]])
            rx, ry, rz, rw = ned_quat_to_unity(row[cols["qw"]], row[cols["qx"]],
                                               row[cols["qy"]], row[cols["qz"]])
            arr[f_idx, s] = (px, py, pz, vx, vy, vz, rx, ry, rz, rw, 1.0)
    return arr


# ---------------------------------------------------------------------------
# Delta expansion
# ---------------------------------------------------------------------------

def expand_links(link_records, end_t):
    """links are deltas; the viewer wants intervals. Open links close at end_t."""
    open_at = {}   # (a,b) -> t_start
    out = []
    for rec in link_records:
        t = rec["t"]
        for a, b in rec.get("remove", []):
            key = (min(a, b), max(a, b))
            if key in open_at:
                out.append({"a": key[0], "b": key[1],
                            "t_start": open_at.pop(key), "t_end": t})
        for a, b in rec.get("add", []):
            key = (min(a, b), max(a, b))
            if key not in open_at:
                open_at[key] = t
    for key, t0 in open_at.items():
        out.append({"a": key[0], "b": key[1], "t_start": t0, "t_end": end_t})
    out.sort(key=lambda e: (e["t_start"], e["a"], e["b"]))
    return out


def expand_beliefs(belief_records, header, slots):
    """beliefs are deltas: one entry per change. Rows are [observer, entity, class].

    The initial "nothing declared" state is never sent, so a consumer holds the
    last value it saw per (observer, slot).
    """
    names = header["belief_classes"]
    out = []
    for rec in belief_records:
        for observer, tid, cls in rec.get("set", []):
            if tid not in slots:
                warn("belief about unknown entity id %d at t=%s" % (tid, rec["t"]))
                continue
            out.append({"t": rec["t"], "observer": int(observer),
                        "slot": slots[tid],
                        "class": names[int(cls)] if 0 <= cls < len(names) else "unknown"})
    out.sort(key=lambda e: (e["t"], e["observer"], e["slot"]))
    return out


def expand_telemetry(telemetry_records):
    """The trace stores parallel arrays indexed by drone id; the viewer wants rows."""
    out = []
    for rec in telemetry_records:
        sent = rec.get("tx_bytes", [])
        budget = rec.get("tx_budget", [])
        for drone in range(len(sent)):
            out.append({"t": rec["t"], "drone": drone,
                        "bytes_sent": sent[drone],
                        "budget_remaining": budget[drone] if drone < len(budget) else 0})
    return out


# ---------------------------------------------------------------------------
# Events
# ---------------------------------------------------------------------------

def frame_at(times, t):
    """Index of the last frame at or before t."""
    lo, hi = 0, len(times) - 1
    if t <= times[0]:
        return 0
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if times[mid] <= t:
            lo = mid
        else:
            hi = mid - 1
    return lo


def derive_events(arr, times, entities, header, report):
    """Spawns and deaths come from the frames; everything else from the report.

    The report is authoritative and we prefer it wherever the two disagree:
    a breach cannot be derived from geometry at all (the hostile is removed on
    reaching the asset, which looks exactly like any other disappearance), and
    the report knows *why* a drone was lost, which geometry can only guess at.
    Geometry still earns its place twice: it supplies spawn/death timing the
    report does not carry, and it is what attaches report events to slots, since
    the report names drones and hostiles but never entity ids.
    """
    events = []
    kill_radius = header["kill_radius"]
    frame_dt = (times[-1] - times[0]) / max(1, len(times) - 1)

    def ent_label(ent):
        """Same names as the viewer aircraft list: brain id for friendlies."""
        if ent.get("kind") == "friendly" and int(ent.get("drone_id", -1)) >= 0:
            return "drone %d" % ent["drone_id"]
        return "%s %d" % (ent["kind"], ent["trace_id"])

    # --- spawn / death, from alive transitions -----------------------------
    last_frame = len(times) - 1
    deaths = {}  # frame index -> [slot, ...]
    for ent in entities:
        s, f0, f1 = ent["slot"], ent["first_frame"], ent["last_frame"]
        events.append({"t": times[f0], "frame": f0, "kind": "spawn",
                       "severity": SEV_INFO, "slots": [s],
                       "text": "%s appears" % ent_label(ent)})
        if f1 < last_frame:
            deaths.setdefault(f1 + 1, []).append(s)

    # --- collisions: two deaths on the same frame, close enough to touch ---
    # Threshold is the kill radius plus how far the pair could close in one
    # recorded frame, computed from their own last velocities rather than a
    # constant, because the trace is decimated to 10 Hz and a lot happens
    # between frames.
    paired = set()
    for f_idx, slots_dead in sorted(deaths.items()):
        prev = f_idx - 1
        for i in range(len(slots_dead)):
            for j in range(i + 1, len(slots_dead)):
                a, b = slots_dead[i], slots_dead[j]
                pa, pb = arr[prev, a, 0:3], arr[prev, b, 0:3]
                va, vb = arr[prev, a, 3:6], arr[prev, b, 3:6]
                gap = float(np.linalg.norm(pa - pb))
                closing = float(np.linalg.norm(va - vb)) * frame_dt
                if gap > kill_radius + closing:
                    continue
                ka = entities[a]["kind"]
                kb = entities[b]["kind"]
                kinds = {ka, kb}
                if "civilian" in kinds:
                    sev, label = SEV_LOSS_WORST, "civilian collision"
                elif kinds == {"friendly"}:
                    sev, label = SEV_LOSS_BAD, "friendly collision"
                elif kinds == {"friendly", "hostile"}:
                    sev, label = SEV_INFO, "intercept"
                else:
                    sev, label = SEV_LOSS_EXPECTED, "collision"
                events.append({"t": times[f_idx], "frame": f_idx, "kind": label,
                               "severity": sev, "slots": [a, b],
                               "text": "%s: %s x %s" % (
                                   label, ent_label(entities[a]),
                                   ent_label(entities[b]))})
                paired.add(a)
                paired.add(b)

    for f_idx, slots_dead in sorted(deaths.items()):
        for s in slots_dead:
            if s in paired:
                continue
            ent = entities[s]
            sev = SEV_INFO if ent["kind"] == "wreckage" else SEV_LOSS_EXPECTED
            events.append({"t": times[f_idx], "frame": f_idx, "kind": "death",
                           "severity": sev, "slots": [s],
                           "text": "%s gone" % ent_label(ent)})

    # --- report events: authoritative -------------------------------------
    by_drone = {e["drone_id"]: e["slot"] for e in entities
                if e["kind"] == "friendly" and e["drone_id"] >= 0}
    report_sev = {"breach": SEV_LOSS_WORST, "civilian_lost": SEV_LOSS_WORST,
                  "friendly_lost": SEV_LOSS_BAD, "intercept": SEV_INFO}
    for ev in (report or {}).get("events", []):
        t = ev["t"]
        slots = []
        if "drone" in ev and ev["drone"] in by_drone:
            slots = [by_drone[ev["drone"]]]
        text = ev["type"]
        if ev.get("target"):
            text += " (%s)" % ev["target"]
        if "drone" in ev:
            text += " drone %d" % ev["drone"]
        events.append({"t": t, "frame": frame_at(times, t),
                       "kind": "report_" + ev["type"],
                       "severity": report_sev.get(ev["type"], SEV_LOSS_EXPECTED),
                       "slots": slots, "text": text})

    events.sort(key=lambda e: (e["t"], e["kind"], e["slots"]))
    return events


# ---------------------------------------------------------------------------
# Score attribution
# ---------------------------------------------------------------------------

# Verified against the reports rather than read off the brief: on nine runs
# spanning both tiers, mission == sum(intercept rewards) - 150*civilians
# - 200*breaches - 40*wasted, exactly. build_scoring re-checks it per run and
# says so in the output, so a future rules change shows up as verified: false
# instead of silently skewing the ledger.
SCORE_WEIGHTS = {
    "kill_max": 100.0,        # scaled by (1 - t_engage/t_free); see reward
    "breach": -200.0,
    "civilian": -150.0,
    "wasted_friendly": -40.0,
}

# A civilian that dies with no drone of ours anywhere near it was not killed by
# us: kill_radius applies to EVERY pair of objects, civilians included, so two
# of them drifting together destroy each other. Measured over 28 runs, 8 of 9
# civilian losses had the nearest friendly 65-138 m away or died before the
# first recorded frame. That is 1200 points of penalty no brain can avoid, and
# it is worth showing as such rather than burying it in the total.
CIVILIAN_BLAME_M = 15.0


def build_scoring(arr, times, entities, report):
    """Per-event ledger of where the score came from, and whether it was ours.

    Everything here is the report's own numbers; the only thing derived from
    the frames is how far the nearest friendly was when a civilian died, which
    is what separates "we rammed a civilian" from "two civilians hit each other
    on the far side of the arena".
    """
    if not report:
        return None

    mission = report.get("mission", {})
    score = report.get("score", {})
    friendly_slots = [e["slot"] for e in entities if e["kind"] == "friendly"]
    civ_slots = [e["slot"] for e in entities if e["kind"] == "civilian"]

    def frame_before(t):
        """Last recorded frame at or before t, or None if t precedes them all."""
        idx = None
        for i, tt in enumerate(times):
            if tt <= t + 1e-9:
                idx = i
            else:
                break
        return idx

    def nearest_friendly_to_any_civilian(t):
        f = frame_before(t)
        if f is None or not friendly_slots or not civ_slots:
            return None
        best = None
        for c in civ_slots:
            if arr[f, c, 10] <= 0.0:      # alive flag
                continue
            for d in friendly_slots:
                if arr[f, d, 10] <= 0.0:
                    continue
                gap = float(np.linalg.norm(arr[f, c, 0:3] - arr[f, d, 0:3]))
                if best is None or gap < best:
                    best = gap
        return best

    by_hostile = {i.get("hostile"): i for i in mission.get("intercepts", [])}
    ledger = []

    for ev in report.get("events", []):
        t = ev["t"]
        kind = ev["type"]
        entry = {"t": t, "frame": frame_at(times, t), "kind": kind,
                 "points": 0.0, "attributable": True, "detail": {}}

        if kind == "intercept":
            hit = by_hostile.get(ev.get("target"), {})
            entry["points"] = float(hit.get("reward", 0.0))
            drones = hit.get("by_drones") or []
            entry["text"] = "%s destroyed%s" % (
                ev.get("target", "hostile"),
                (" by drone %d" % drones[0]) if drones else "")
            entry["detail"] = {k: hit[k] for k in
                               ("urgency_ratio", "t_engage_s", "t_free_s", "t_spawn")
                               if k in hit}
            # Reward is what is left of kill_max after the urgency scaling, so
            # the gap between them is the cost of engaging late. Worth showing.
            entry["detail"]["forgone_by_engaging_late"] = round(
                SCORE_WEIGHTS["kill_max"] - entry["points"], 2)

        elif kind == "breach":
            entry["points"] = SCORE_WEIGHTS["breach"]
            entry["text"] = "%s reached the asset" % ev.get("target", "hostile")

        elif kind == "civilian_lost":
            entry["points"] = SCORE_WEIGHTS["civilian"]
            near = nearest_friendly_to_any_civilian(t)
            if near is None:
                entry["attributable"] = False
                entry["text"] = ("civilian lost before the first recorded frame "
                                 "- no drone had moved yet")
                entry["detail"] = {"nearest_friendly_m": None}
            else:
                entry["attributable"] = near < CIVILIAN_BLAME_M
                entry["text"] = ("civilian lost, nearest drone %.0f m away%s"
                                 % (near, "" if entry["attributable"]
                                    else " - not ours"))
                entry["detail"] = {"nearest_friendly_m": round(near, 1),
                                   "blame_radius_m": CIVILIAN_BLAME_M}

        elif kind == "friendly_lost":
            # A drone spent on a hostile is credited, not wasted, and costs
            # nothing; only an uncredited loss carries the penalty.
            wasted = ev.get("target") != "pair_hostile"
            entry["points"] = SCORE_WEIGHTS["wasted_friendly"] if wasted else 0.0
            entry["text"] = "drone %s lost (%s)" % (ev.get("drone", "?"),
                                                    ev.get("target", "unknown"))
            entry["detail"] = {"credited": not wasted}

        else:
            entry["text"] = kind

        ledger.append(entry)

    ledger.sort(key=lambda e: (e["t"], e["kind"]))

    running, total = [], 0.0
    for e in ledger:
        total += e["points"]
        running.append({"t": e["t"], "mission": round(total, 2)})

    ours = [e for e in ledger if e["attributable"]]
    theirs = [e for e in ledger if not e["attributable"]]
    reported = float(score.get("mission", 0.0))

    return {
        "formula": ("mission = sum(intercept rewards) - 150*civilians "
                    "- 200*breaches - 40*wasted_friendlies"),
        "weights": SCORE_WEIGHTS,
        "totals": {
            "mission": reported,
            "awareness": score.get("awareness"),
            "comms": score.get("comms"),
            "total": score.get("total"),
            "ledger_sum": round(total, 2),
            # If this goes false the scoring rules moved and the ledger is
            # describing a formula the simulator no longer uses.
            "verified": abs(total - reported) < 0.15,
        },
        "attributable": {"count": len(ours),
                         "points": round(sum(e["points"] for e in ours), 2)},
        "unattributable": {"count": len(theirs),
                           "points": round(sum(e["points"] for e in theirs), 2)},
        "ledger": ledger,
        "running": running,
    }


def cross_check(events, report, entities):
    """Compare what geometry found against what the report says, and report the gap.

    Printed, not silenced: the report wins, but a disagreement is worth seeing
    because it usually means the geometric threshold is off, not the report.
    """
    geo_intercepts = sum(1 for e in events if e["kind"] == "intercept")
    geo_civ = sum(1 for e in events if e["kind"] == "civilian collision")
    geo_ff = sum(1 for e in events if e["kind"] == "friendly collision")
    mission = (report or {}).get("mission", {})
    rep_intercepts = mission.get("hostiles_destroyed", 0)
    rep_civ = mission.get("civilians_lost", 0)
    causes = mission.get("losses_by_cause", {})
    rep_ff = causes.get("pair_friendly", 0)

    lines = []
    if geo_intercepts != rep_intercepts:
        lines.append("  intercepts: geometry %d, report %d (report wins)"
                     % (geo_intercepts, rep_intercepts))
    if geo_civ != rep_civ:
        lines.append("  civilian losses: geometry %d, report %d (report wins)"
                     % (geo_civ, rep_civ))
    if geo_ff * 2 != rep_ff:
        lines.append("  friendly-friendly: geometry %d collisions (=%d drones), "
                     "report %d drones (report wins)" % (geo_ff, geo_ff * 2, rep_ff))
    breaches = sum(1 for e in (report or {}).get("events", []) if e["type"] == "breach")
    if breaches:
        lines.append("  %d breach(es) come from the report only -- a breach is not "
                     "derivable from geometry" % breaches)
    return lines


# ---------------------------------------------------------------------------

def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("trace", help="the .jsonl recording from --trace")
    ap.add_argument("out_stem", help="output stem; writes <stem>.bin and <stem>.meta.json")
    args = ap.parse_args(argv[1:])

    data = read_trace(args.trace)
    header, frames = data["header"], data["frames"]

    cols, slots, entities = build_slots(header, frames)
    times = [f["t"] for f in frames]
    arr = fill_array(frames, cols, slots, len(entities))

    report = data["report"]["report"] if data["report"] else None
    events = derive_events(arr, times, entities, header, report)

    generated_at = datetime.now(timezone.utc).replace(microsecond=0).strftime(
        "%Y-%m-%dT%H:%M:%SZ")

    meta = {
        "format_version": FORMAT_VERSION,
        "source": os.path.basename(args.trace),
        "scenario": header["scenario"],
        "sim_version": header.get("sim_version"),
        "brain": header.get("brain"),
        "dt": header["dt"],
        "trace_hz": header["trace_hz"],
        "frame_count": len(frames),
        "slot_count": len(entities),
        "stride": STRIDE,
        "duration": times[-1],
        "arena": {
            "min": list(ned_to_unity_vec3(*header["arena"]["min"])),
            "max": list(ned_to_unity_vec3(*header["arena"]["max"])),
        },
        "asset": {
            "position": list(ned_to_unity_vec3(*header["asset"]["position"])),
            "radius": header["asset"]["radius"],
        },
        "kill_radius": header["kill_radius"],
        "fleet_size": header["fleet_size"],
        "provenance": {
            "trace": os.path.abspath(args.trace),
            "scenario": header.get("scenario"),
            "brain": header.get("brain"),
            "sim_version": header.get("sim_version"),
            "header_schema": header.get("schema"),
            "generated_at": generated_at,
        },
        "entities": entities,
        "events": events,
        "links": expand_links(data["links"], times[-1]),
        "beliefs": expand_beliefs(data["beliefs"], header, slots),
        "logs": [{"t": r["t"], "drone": r["drone"], "text": r["text"]}
                 for r in data["logs"]],
        "telemetry": expand_telemetry(data["telemetry"]),
        "report": report,
        "scoring": build_scoring(arr, times, entities, report),
    }

    # The arena box is a min/max pair in NED; converting each corner flips the
    # z and y roles, so re-derive a true min/max rather than trusting the names.
    lo = [min(a, b) for a, b in zip(meta["arena"]["min"], meta["arena"]["max"])]
    hi = [max(a, b) for a, b in zip(meta["arena"]["min"], meta["arena"]["max"])]
    meta["arena"] = {"min": lo, "max": hi}

    bin_path = args.out_stem + ".bin"
    meta_path = args.out_stem + ".meta.json"
    # C-order == frame-major. Force little-endian so the file is portable.
    np.ascontiguousarray(arr, dtype="<f4").tofile(bin_path)
    with open(meta_path, "w", encoding="utf-8", newline="\n") as fh:
        json.dump(meta, fh, indent=1, sort_keys=False)
        fh.write("\n")

    expect = len(frames) * len(entities) * STRIDE * 4
    actual = os.path.getsize(bin_path)
    if actual != expect:
        die("%s is %d bytes, expected %d" % (bin_path, actual, expect))

    kinds = {}
    for e in entities:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    print("wrote %s (%d bytes) and %s" % (bin_path, actual, meta_path))
    print("  provenance: %s" % meta["provenance"]["generated_at"])
    print("  brain: %s" % (meta.get("brain") or "(unknown)"))
    print("  entity_columns: %s" % header["entity_columns"])
    print("  frames %d over %.2f s, slots %d  %s"
          % (len(frames), times[-1], len(entities), kinds))
    print("  events %d, links %d, beliefs %d, logs %d, telemetry %d"
          % (len(meta["events"]), len(meta["links"]), len(meta["beliefs"]),
             len(meta["logs"]), len(meta["telemetry"])))
    notes = cross_check(events, report, entities)
    if notes:
        print("  geometry vs report:")
        for line in notes:
            print(line)
    if not meta["beliefs"]:
        print("  note: no beliefs records -- this brain declared nothing, so the "
              "beliefs path is untested here")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))