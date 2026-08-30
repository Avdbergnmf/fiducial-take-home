#!/usr/bin/env python3
"""plot_fixture.py -- top-down check that the sidecar parsed the fixture right.

    python tools/plot_fixture.py
    python tools/plot_fixture.py runs/fixture

Reads <stem>.bin + <stem>.meta.json (Unity-frame data, not the raw trace) and
writes <stem>_check.png. Prints a ring / altitude / spawn-radius report and
exits non-zero if the parse looks wrong.

s1.json: 16 friendlies in a 60 m ring at 30 m altitude, 6 hostiles from ~170 m,
8 civilians from ~150 m. The example brain hovers, so the ring must stay a
circle for the whole run. If it does not, the parse is wrong -- say so rather
than proceeding.
"""

import argparse
import json
import math
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Circle, Rectangle
import numpy as np

STRIDE = 11
FRIENDLY_RADIUS = 60.0
FRIENDLY_ALT = 30.0
HOSTILE_SPAWN = 170.0
CIVILIAN_SPAWN = 150.0
# Tight on the hover ring (they should not move). Looser on spawn radii because
# the first recorded frame is one trace step after the entity appears.
RING_TOL = 2.0
ALT_TOL = 2.0
SPAWN_TOL = 8.0

KIND_COLOUR = {
    "friendly": "#2563eb",
    "hostile": "#dc2626",
    "civilian": "#6b7280",
    "wreckage": "#92400e",
    "unknown": "#a855f7",
}


def die(msg):
    print("error: %s" % msg, file=sys.stderr)
    raise SystemExit(1)


def load(stem):
    meta_path = stem + ".meta.json"
    bin_path = stem + ".bin"
    if not os.path.isfile(meta_path) or not os.path.isfile(bin_path):
        die("%s.bin / %s.meta.json missing -- run "
            "python tools/build_viewer_data.py <trace.jsonl> %s first"
            % (stem, stem, stem))
    with open(meta_path, encoding="utf-8") as fh:
        meta = json.load(fh)
    data = np.fromfile(bin_path, dtype="<f4")
    nframes = meta["frame_count"]
    nslots = meta["slot_count"]
    stride = meta["stride"]
    expect = nframes * nslots * stride
    if data.size != expect:
        die("%s is %d floats, expected %d (= %d frames * %d slots * %d)"
            % (bin_path, data.size, expect, nframes, nslots, stride))
    nbytes = os.path.getsize(bin_path)
    if nbytes != expect * 4:
        die("%s is %d bytes, expected %d" % (bin_path, nbytes, expect * 4))
    arr = data.reshape(nframes, nslots, stride)
    return meta, arr, bin_path, nbytes


def horiz_radius(x, z):
    return math.hypot(x, z)


def kind_slots(entities, kind):
    return [e for e in entities if e["kind"] == kind]


def radii_at(arr, ents, frame):
    out = []
    for e in ents:
        x, y, z = arr[frame, e["slot"], 0:3]
        out.append((horiz_radius(float(x), float(z)), float(y)))
    return out


def check(meta, arr):
    """Return (ok, lines). ok is False if the parse looks wrong."""
    ents = meta["entities"]
    friendlies = kind_slots(ents, "friendly")
    hostiles = kind_slots(ents, "hostile")
    civilians = kind_slots(ents, "civilian")
    last = meta["frame_count"] - 1
    lines = []
    ok = True

    def fail(msg):
        nonlocal ok
        ok = False
        lines.append("FAIL  " + msg)

    def note(msg):
        lines.append("      " + msg)

    lines.append("frames %d  slots %d  duration %.2f s  (s1 cap is 180 s)"
                 % (meta["frame_count"], meta["slot_count"], meta["duration"]))
    lines.append("counts  friendly=%d hostile=%d civilian=%d wreckage=%d"
                 % (len(friendlies), len(hostiles), len(civilians),
                    len(kind_slots(ents, "wreckage"))))

    if len(friendlies) != 16:
        fail("expected 16 friendlies, got %d" % len(friendlies))
    if len(hostiles) != 6:
        fail("expected 6 hostiles, got %d" % len(hostiles))
    if len(civilians) != 8:
        fail("expected 8 civilians, got %d" % len(civilians))

    # Altitude sign: Unity y must be +30, not -30. That is the NED conversion.
    for label, frame in (("first", 0), ("last", last)):
        samples = radii_at(arr, friendlies, frame)
        if not samples:
            fail("no friendlies to measure at frame %d" % frame)
            continue
        rs = [r for r, _ in samples]
        ys = [y for _, y in samples]
        r_mean, r_std = float(np.mean(rs)), float(np.std(rs))
        y_mean = float(np.mean(ys))
        lines.append("friendly ring %s: radius %.2f +/- %.2f m  y %.2f m"
                     % (label, r_mean, r_std, y_mean))
        if abs(r_mean - FRIENDLY_RADIUS) > RING_TOL:
            fail("ring radius %.2f is not ~%.0f (parse is wrong)"
                 % (r_mean, FRIENDLY_RADIUS))
        if r_std > RING_TOL:
            fail("ring stddev %.2f m -- not a clean circle (parse is wrong)"
                 % r_std)
        if abs(y_mean - FRIENDLY_ALT) > ALT_TOL:
            fail("altitude y=%.2f is not ~+%.0f -- NED conversion is flipped"
                 % (y_mean, FRIENDLY_ALT))
        if y_mean < 0:
            fail("altitude is negative; Unity y should be up")

    for e in hostiles:
        x, y, z = arr[e["first_frame"], e["slot"], 0:3]
        r = horiz_radius(float(x), float(z))
        if abs(r - HOSTILE_SPAWN) > SPAWN_TOL:
            fail("hostile %d first appears at r=%.1f, expected ~%.0f"
                 % (e["trace_id"], r, HOSTILE_SPAWN))
    if hostiles:
        rs = [horiz_radius(*arr[e["first_frame"], e["slot"], [0, 2]])
              for e in hostiles]
        lines.append("hostile spawn radius: %.1f .. %.1f m (expect ~%.0f)"
                     % (min(rs), max(rs), HOSTILE_SPAWN))

    for e in civilians:
        x, y, z = arr[e["first_frame"], e["slot"], 0:3]
        r = horiz_radius(float(x), float(z))
        if abs(r - CIVILIAN_SPAWN) > SPAWN_TOL:
            fail("civilian %d first appears at r=%.1f, expected ~%.0f"
                 % (e["trace_id"], r, CIVILIAN_SPAWN))
    if civilians:
        rs = [horiz_radius(*arr[e["first_frame"], e["slot"], [0, 2]])
              for e in civilians]
        lines.append("civilian spawn radius: %.1f .. %.1f m (expect ~%.0f)"
                     % (min(rs), max(rs), CIVILIAN_SPAWN))

    if meta["frame_count"] < 500:
        fail("only %d frames; even a short s1 run should be hundreds"
             % meta["frame_count"])
    if meta["duration"] > 170:
        note("full-length run (%.1f s). hover fixture is ~89 s because every "
             "hostile breaches and the sim stops." % meta["duration"])
    else:
        note("duration %.1f s < 180 s cap: expected for the hover brain "
             "(all 6 hostiles breach, sim ends)." % meta["duration"])

    if not meta.get("beliefs"):
        note("no beliefs -- example brain never declares; path untested.")

    return ok, lines


def plot(meta, arr, out_path):
    fig, ax = plt.subplots(figsize=(9, 9))
    arena = meta["arena"]
    ax.add_patch(Rectangle(
        (arena["min"][0], arena["min"][2]),
        arena["max"][0] - arena["min"][0],
        arena["max"][2] - arena["min"][2],
        fill=False, edgecolor="#d1d5db", lw=0.8, zorder=0))
    asset = meta["asset"]
    ax.add_patch(Circle(
        (asset["position"][0], asset["position"][2]),
        asset["radius"],
        facecolor="#fef3c7", edgecolor="#d97706", lw=1.2, zorder=1,
        label="asset r=%.0f" % asset["radius"]))
    ax.add_patch(Circle(
        (0, 0), FRIENDLY_RADIUS, fill=False, ls="--",
        edgecolor="#93c5fd", lw=0.9, zorder=1, label="60 m ring"))

    by_kind = {}
    for e in meta["entities"]:
        by_kind.setdefault(e["kind"], []).append(e)

    for kind in ("civilian", "hostile", "friendly", "wreckage", "unknown"):
        ents = by_kind.get(kind, [])
        if not ents:
            continue
        colour = KIND_COLOUR.get(kind, "#111")
        xs, zs = [], []
        for e in ents:
            s = e["slot"]
            alive = arr[:, s, 10] > 0.5
            xs.append(arr[alive, s, 0])
            zs.append(arr[alive, s, 2])
        ax.scatter(np.concatenate(xs), np.concatenate(zs),
                   s=2, c=colour, alpha=0.22, linewidths=0, zorder=2,
                   label="%s (%d)" % (kind, len(ents)))
        # first and last alive positions, so spawn vs rest are readable
        first_x = [float(arr[e["first_frame"], e["slot"], 0]) for e in ents]
        first_z = [float(arr[e["first_frame"], e["slot"], 2]) for e in ents]
        ax.scatter(first_x, first_z, s=28, facecolors="none",
                   edgecolors=colour, linewidths=0.9, zorder=3)

    ax.set_aspect("equal", adjustable="box")
    ax.set_xlabel("Unity X (East)")
    ax.set_ylabel("Unity Z (North)")
    ax.set_title("fixture top-down  ·  %s  ·  %.1f s  ·  %d frames"
                 % (meta["scenario"], meta["duration"], meta["frame_count"]))
    ax.legend(loc="upper right", markerscale=3, framealpha=0.92)
    ax.grid(True, color="#f3f4f6", zorder=0)
    fig.tight_layout()
    fig.savefig(out_path, dpi=140)
    plt.close(fig)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("stem", nargs="?", default=os.path.join("runs", "fixture"),
                    help="output stem (default: runs/fixture)")
    args = ap.parse_args(argv[1:])

    meta, arr, bin_path, nbytes = load(args.stem)
    ok, lines = check(meta, arr)
    png = args.stem + "_check.png"
    plot(meta, arr, png)

    print("read %s (%d bytes) and %s.meta.json" % (bin_path, nbytes, args.stem))
    for line in lines:
        print(line)
    print("wrote %s" % png)
    if ok:
        print("ring check PASSED")
        return 0
    print("ring check FAILED -- the parse is wrong, do not proceed to Unity")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
