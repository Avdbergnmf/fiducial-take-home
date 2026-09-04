#!/usr/bin/env python3
"""plot_ablation.py -- figures for the git version ladder.

    python tools/plot_ablation.py
    python tools/plot_ablation.py --update-csv

Reads runs/ablation/<ver>/{meta.json,summary.csv} written by
scripts/ablation.ps1. Writes notes/ablation-*.png and
notes/ablation-summary.md. Optional --update-csv fills the expected
min/mean/max/worst columns in scripts/versions.csv from the live metas.

history.ps1 is the iterate log, not this ladder.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from decimal import Decimal, ROUND_HALF_UP
from pathlib import Path

import matplotlib.dates as mdates

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

REPO = Path(__file__).resolve().parents[1]
ABLATION = REPO / "runs" / "ablation"
NOTES = REPO / "notes"
MANIFEST = REPO / "scripts" / "versions.csv"
IDS = ["s0", "s1", "s2", "x1-a", "x1-b", "x1-c", "x2-a", "x2-b"]
# tab10, stable per id so the delta stack and the trajectories match.
ID_COLORS = {
    "s0": "#4e79a7",
    "s1": "#f28e2b",
    "s2": "#e15759",
    "x1-a": "#76b7b2",
    "x1-b": "#59a14f",
    "x1-c": "#edc948",
    "x2-a": "#b07aa1",
    "x2-b": "#ff9da7",
}

COL_FLOOR = "#b45309"
COL_MEAN = "#1d4ed8"
COL_MAX = "#64748b"
COL_BREACH = "#dc2626"
COL_OK = "#15803d"
# CEST. Windows Python here has no tzdata; do not use ZoneInfo.
LOCAL = timezone(timedelta(hours=2))


def version_key(name: str) -> tuple:
    m = re.fullmatch(r"V(\d+)", name, flags=re.IGNORECASE)
    if m:
        return (0, int(m.group(1)))
    return (1, name)


def load_versions() -> list[dict]:
    rows = []
    if not ABLATION.is_dir():
        return rows
    for child in ABLATION.iterdir():
        if not child.is_dir() or child.name.startswith("_"):
            continue
        meta_path = child / "meta.json"
        summary_path = child / "summary.csv"
        if not meta_path.is_file() or not summary_path.is_file():
            continue
        meta = json.loads(meta_path.read_text(encoding="utf-8-sig"))
        per = {}
        with summary_path.open(encoding="utf-8-sig", newline="") as fh:
            for rec in csv.DictReader(fh):
                per[rec["Id"]] = rec
        meta["_dir"] = str(child)
        meta["_per"] = per
        meta["version"] = meta.get("version") or child.name
        rows.append(meta)
    rows.sort(key=lambda r: version_key(str(r["version"])))
    return rows


def fget(rec: dict, key: str, default: float = 0.0) -> float:
    try:
        return float(rec.get(key, default) or default)
    except (TypeError, ValueError):
        return default


def round1(x) -> float:
    """Half-up to 0.1. Python/banker's round(152.25, 1) is 152.2 — a fake dip."""
    return float(Decimal(str(x)).quantize(Decimal("0.1"), rounding=ROUND_HALF_UP))


def iget(rec: dict, key: str, default: int = 0) -> int:
    try:
        return int(float(rec.get(key, default) or default))
    except (TypeError, ValueError):
        return default


def style():
    plt.rcParams.update(
        {
            "figure.facecolor": "white",
            "axes.facecolor": "white",
            "axes.grid": True,
            "grid.alpha": 0.25,
            "font.size": 10,
            "axes.titlesize": 12,
            "axes.titleweight": "semibold",
        }
    )


def git_commit_datetime(spec: str):
    spec = (spec or "").strip()
    if spec in ("", "SNAPSHOT", "WORKING", "."):
        return None
    try:
        raw = subprocess.check_output(
            ["git", "-C", str(REPO), "log", "-1", "--format=%ci", spec],
            text=True,
            stderr=subprocess.DEVNULL,
        ).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None
    if not raw:
        return None
    dt = datetime.strptime(raw, "%Y-%m-%d %H:%M:%S %z")
    return dt.astimezone(LOCAL).replace(tzinfo=None)


def row_datetime(r: dict):
    """Git author time, or ablation `when` for SNAPSHOT / WORKING."""
    for key in ("commit_request", "commit_short", "commit"):
        dt = git_commit_datetime(str(r.get(key) or ""))
        if dt is not None:
            r["_git_time"] = True
            return dt
    when = r.get("when")
    if when:
        dt = datetime.fromisoformat(str(when))
        if dt.tzinfo is not None:
            dt = dt.astimezone(LOCAL).replace(tzinfo=None)
        r["_git_time"] = False
        return dt
    r["_git_time"] = False
    return None


def save(fig, name: str) -> Path:
    NOTES.mkdir(parents=True, exist_ok=True)
    path = NOTES / name
    fig.savefig(path, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {path}")
    return path


def plot_ladder(rows: list[dict]) -> None:
    labels = [r["version"] for r in rows]
    mins = [float(r["min"]) for r in rows]
    means = [float(r["mean"]) for r in rows]
    maxs = [float(r["max"]) for r in rows]
    x = np.arange(len(labels))

    fig, ax = plt.subplots(figsize=(11.5, 4.8))
    ax.fill_between(x, mins, maxs, color="#cbd5e1", alpha=0.55, label="min–max")
    ax.plot(x, maxs, color=COL_MAX, lw=1.4, marker="o", ms=4, label="max")
    ax.plot(x, means, color=COL_MEAN, lw=2.0, marker="o", ms=5, label="mean")
    ax.plot(x, mins, color=COL_FLOOR, lw=2.2, marker="o", ms=5, label="floor (min)")
    ax.axhline(0.0, color="#94a3b8", lw=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("score (8 named ids)")
    ax.set_title("Ablation ladder — floor / mean / max")
    ax.legend(loc="lower right", framealpha=0.95)
    fig.tight_layout()
    save(fig, "ablation-ladder.png")


def plot_timeline(rows: list[dict]) -> None:
    """Same floor / mean / max, x = commit time (not equal version steps)."""
    times = []
    mins = []
    means = []
    maxs = []
    labels = []
    is_git = []
    for r in rows:
        dt = row_datetime(r)
        if dt is None:
            continue
        times.append(dt)
        mins.append(float(r["min"]))
        means.append(float(r["mean"]))
        maxs.append(float(r["max"]))
        labels.append(r["version"])
        is_git.append(bool(r.get("_git_time")))
    if len(times) < 2:
        return

    fig, ax = plt.subplots(figsize=(13.2, 5.2))
    t_step = list(times) + [times[-1] + timedelta(hours=1)]
    n_pad = lambda xs: xs + [xs[-1]]
    ax.fill_between(
        t_step, n_pad(mins), n_pad(maxs), step="post", color="#cbd5e1", alpha=0.55, label="min–max"
    )
    ax.step(t_step, n_pad(maxs), where="post", color=COL_MAX, lw=1.4, label="max")
    ax.step(t_step, n_pad(means), where="post", color=COL_MEAN, lw=2.0, label="mean")
    ax.step(t_step, n_pad(mins), where="post", color=COL_FLOOR, lw=2.2, label="floor (min)")
    git_t = [t for t, g in zip(times, is_git) if g]
    git_y = [y for y, g in zip(means, is_git) if g]
    other_t = [t for t, g in zip(times, is_git) if not g]
    other_y = [y for y, g in zip(means, is_git) if not g]
    ax.plot(git_t, git_y, color=COL_MEAN, marker="o", ms=5.5, ls="none", zorder=4)
    if other_t:
        ax.plot(
            other_t,
            other_y,
            color=COL_MEAN,
            marker="D",
            ms=6,
            ls="none",
            zorder=4,
            markerfacecolor="white",
            markeredgewidth=1.6,
            label="V23/V24 sweep time (not git)",
        )
    ax.axhline(0.0, color="#94a3b8", lw=0.8)
    y_top = max(maxs)
    for i, (t, ver) in enumerate(zip(times, labels)):
        ax.annotate(
            ver,
            (mdates.date2num(t), y_top),
            xytext=(0, 4 + (10 if i % 2 else 0)),
            textcoords="offset points",
            ha="center",
            va="bottom",
            fontsize=7,
            color="#334155",
        )
    ax.set_ylabel("score (8 named ids)")
    ax.set_title("Ablation ladder — score vs commit time")
    ax.xaxis.set_major_locator(mdates.DayLocator())
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%a %b %d"))
    ax.xaxis.set_minor_locator(mdates.HourLocator(byhour=[6, 12, 18]))
    ax.set_xlabel("commit time (local). Equal version steps hide that V1–V14 is one day.")
    ax.legend(loc="lower right", framealpha=0.95)
    y_lo = min(mins)
    ax.set_ylim(y_lo - 0.04 * (y_top - y_lo), y_top + 0.22 * (y_top - y_lo))
    fig.autofmt_xdate(rotation=0, ha="center")
    fig.tight_layout()
    save(fig, "ablation-timeline.png")


def plot_heatmap(rows: list[dict]) -> None:
    mat = np.full((len(IDS), len(rows)), np.nan)
    for j, r in enumerate(rows):
        per = r["_per"]
        for i, sid in enumerate(IDS):
            if sid in per:
                mat[i, j] = fget(per[sid], "Total")

    finite = mat[np.isfinite(mat)]
    vmax = max(abs(finite.min()), abs(finite.max()), 1.0) if finite.size else 1.0
    fig, ax = plt.subplots(figsize=(11.5, 4.2))
    im = ax.imshow(
        mat,
        aspect="auto",
        cmap="RdYlGn",
        vmin=-vmax,
        vmax=vmax,
        interpolation="nearest",
    )
    ax.set_yticks(np.arange(len(IDS)))
    ax.set_yticklabels(IDS)
    ax.set_xticks(np.arange(len(rows)))
    ax.set_xticklabels([r["version"] for r in rows], rotation=45, ha="right")
    ax.set_title("Per-id total")
    ax.grid(False)
    cb = fig.colorbar(im, ax=ax, fraction=0.03, pad=0.02)
    cb.set_label("total")
    fig.tight_layout()
    save(fig, "ablation-heatmap.png")


def plot_breaches(rows: list[dict]) -> None:
    labels = [r["version"] for r in rows]
    breaches = []
    kills = []
    civs = []
    for r in rows:
        b = k = c = 0
        for rec in r["_per"].values():
            b += iget(rec, "Breaches")
            k += iget(rec, "Kills")
            c += iget(rec, "Civilians")
        breaches.append(b)
        kills.append(k)
        civs.append(c)
    x = np.arange(len(labels))
    w = 0.27
    fig, ax = plt.subplots(figsize=(11.5, 4.4))
    ax.bar(x - w, kills, w, color=COL_OK, label="kills (sum)")
    ax.bar(x, breaches, w, color=COL_BREACH, label="breaches (sum)")
    ax.bar(x + w, civs, w, color="#6b7280", label="civilians (sum)")
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("count across 8 ids")
    ax.set_title("Kills / breaches / civilians over the ladder")
    ax.legend(loc="upper left", framealpha=0.95)
    fig.tight_layout()
    save(fig, "ablation-mission.png")


def plot_floor(rows: list[dict]) -> None:
    labels = []
    totals = []
    worst_ids = []
    breaches = []
    civs = []
    for r in rows:
        worst = r.get("worst") or ""
        per = r["_per"]
        rec = per.get(worst)
        if rec is None and per:
            rec = min(per.values(), key=lambda d: fget(d, "Total"))
            worst = rec["Id"]
        if rec is None:
            continue
        labels.append(r["version"])
        totals.append(fget(rec, "Total"))
        worst_ids.append(worst)
        breaches.append(iget(rec, "Breaches"))
        civs.append(iget(rec, "Civilians"))

    x = np.arange(len(labels))
    colors = [COL_BREACH if b > 0 else ("#b45309" if c > 0 else COL_OK) for b, c in zip(breaches, civs)]
    fig, ax = plt.subplots(figsize=(11.5, 4.6))
    ax.bar(x, totals, color=colors, width=0.72)
    ax.axhline(0.0, color="#94a3b8", lw=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(
        [f"{v}\n{w}" for v, w in zip(labels, worst_ids)],
        rotation=45,
        ha="right",
        fontsize=8,
    )
    ax.set_ylabel("worst-id total")
    ax.set_title("Floor scenario per version (red = breach, amber = civ only)")
    fig.tight_layout()
    save(fig, "ablation-floor.png")


def id_totals(rows: list[dict]) -> dict[str, list[float]]:
    out = {sid: [] for sid in IDS}
    for r in rows:
        per = r["_per"]
        for sid in IDS:
            rec = per.get(sid)
            out[sid].append(fget(rec, "Total") if rec else float("nan"))
    return out


def plot_trajectories(rows: list[dict]) -> None:
    """Each named id's total across versions — where the points live."""
    series = id_totals(rows)
    labels = [r["version"] for r in rows]
    x = np.arange(len(labels))
    fig, ax = plt.subplots(figsize=(11.5, 5.4))
    for sid in IDS:
        ax.plot(
            x,
            series[sid],
            color=ID_COLORS[sid],
            lw=1.8,
            marker="o",
            ms=4,
            label=sid,
        )
    ax.axhline(0.0, color="#94a3b8", lw=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("total")
    ax.set_title("Where the points live — each named id across versions")
    ax.legend(loc="lower right", ncol=4, framealpha=0.95, fontsize=8)
    fig.tight_layout()
    save(fig, "ablation-ids.png")


def plot_deltas(rows: list[dict]) -> None:
    """Stacked Δtotal per id at each version step — where points came from."""
    if len(rows) < 2:
        return
    series = id_totals(rows)
    step_labels = [r["version"] for r in rows[1:]]
    n = len(step_labels)
    x = np.arange(n)
    fig, ax = plt.subplots(figsize=(11.5, 5.6))
    pos_bottom = np.zeros(n)
    neg_bottom = np.zeros(n)
    for sid in IDS:
        prev = np.array(series[sid][:-1], dtype=float)
        cur = np.array(series[sid][1:], dtype=float)
        d = (cur - prev) / 8.0
        pos = np.where(d > 0.0, d, 0.0)
        neg = np.where(d < 0.0, d, 0.0)
        ax.bar(
            x,
            pos,
            bottom=pos_bottom,
            color=ID_COLORS[sid],
            width=0.82,
            label=sid,
        )
        ax.bar(x, neg, bottom=neg_bottom, color=ID_COLORS[sid], width=0.82)
        pos_bottom += pos
        neg_bottom += neg
    ax.axhline(0.0, color="#94a3b8", lw=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(step_labels, rotation=45, ha="right")
    ax.set_ylabel("contribution to Δmean (each colour is one id)")
    ax.set_title("Where the mean came from — per-id share of each step")
    ax.legend(loc="upper right", ncol=4, framealpha=0.95, fontsize=8)
    fig.tight_layout()
    save(fig, "ablation-delta.png")


def plot_mean_waterfall(rows: list[dict]) -> None:
    """Running mean, with each step's Δmean as a bar from the previous mean."""
    if len(rows) < 2:
        return
    labels = [r["version"] for r in rows]
    means = np.array([float(r["mean"]) for r in rows], dtype=float)
    x = np.arange(len(labels))
    fig, ax = plt.subplots(figsize=(11.5, 4.8))
    ax.plot(x, means, color=COL_MEAN, lw=2.0, marker="o", ms=5, zorder=3, label="mean")
    for i in range(1, len(means)):
        d = means[i] - means[i - 1]
        color = COL_OK if d >= 0 else COL_BREACH
        ax.plot([i - 1, i], [means[i - 1], means[i - 1]], color="#94a3b8", lw=0.7, zorder=1)
        ax.bar(
            i,
            d,
            bottom=means[i - 1],
            width=0.55,
            color=color,
            alpha=0.85,
            zorder=2,
        )
    ax.axhline(0.0, color="#94a3b8", lw=0.8)
    ax.set_xticks(x)
    ax.set_xticklabels(labels, rotation=45, ha="right")
    ax.set_ylabel("mean of 8 named ids")
    ax.set_title("Mean score — each bar is the move from the previous version")
    ax.legend(loc="lower right", framealpha=0.95)
    fig.tight_layout()
    save(fig, "ablation-waterfall.png")


def write_summary_md(rows: list[dict]) -> None:
    lines = [
        "# Ablation ladder",
        "",
        "Generated by `tools/plot_ablation.py` from `runs/ablation/`.",
        "`scripts/history.ps1` is the iterate CSV, not this table.",
        "",
        "Live brain is **V24** (`WORKING`, D75 connect-first). **V23** is a",
        "frozen sitting-wall snapshot, not live. V23 and V24 match on",
        "min / mean / max at one decimal (mean **152.3** is half-up of 152.25);",
        "per-id cells shuffle by ≤ 0.4. Do not read the last delta column as a",
        "second policy win, and do not flatten V21's real mean dip.",
        "",
        "Attribution: `ablation-ids.png` (where each id sits),",
        "`ablation-delta.png` (which ids moved the mean at each step;",
        "stack height is Δmean), `ablation-waterfall.png` (same Δmean as bars).",
        "`ablation-timeline.png` is the same floor/mean/max on **commit time**",
        "(V23/V24 diamonds are sweep times, not git).",
        "",
        "![floor / mean / max](ablation-ladder.png)",
        "",
        "![per-id totals](ablation-heatmap.png)",
        "",
        "![kills / breaches / civilians](ablation-mission.png)",
        "",
        "![worst id per version](ablation-floor.png)",
        "",
        "![each named id](ablation-ids.png)",
        "",
        "![per-id Δ at each step](ablation-delta.png)",
        "",
        "![mean waterfall](ablation-waterfall.png)",
        "",
        "![score vs commit time](ablation-timeline.png)",
        "",
        "| ver | commit | change | min | mean | max | worst |",
        "| --- | --- | --- | ---: | ---: | ---: | --- |",
    ]
    prev = None
    for r in rows:
        commit = (r.get("commit_short") or r.get("commit_request") or "")[:10]
        label = (r.get("label") or "").replace("|", "/")
        mn = r["min"]
        mean = r["mean"]
        mx = r["max"]
        worst = r.get("worst") or ""
        mn_s = f"{round1(mn):.1f}"
        mean_s = f"{round1(mean):.1f}"
        mx_s = f"{round1(mx):.1f}"
        delta = ""
        if prev is not None:
            d = float(mn) - float(prev)
            delta = f" ({d:+.1f})"
        lines.append(
            f"| {r['version']} | `{commit}` | {label} | {mn_s}{delta} | {mean_s} | {mx_s} | {worst} |"
        )
        prev = mn
    lines.append("")
    path = NOTES / "ablation-summary.md"
    path.write_text("\n".join(lines), encoding="utf-8")
    print(f"wrote {path}")


def update_csv(rows: list[dict]) -> None:
    if not MANIFEST.is_file():
        print("no scripts/versions.csv", file=sys.stderr)
        return
    by_ver = {r["version"]: r for r in rows}
    with MANIFEST.open(encoding="utf-8-sig", newline="") as fh:
        reader = csv.DictReader(fh)
        fieldnames = reader.fieldnames or []
        table = list(reader)
    for rec in table:
        hit = by_ver.get(rec["version"])
        if not hit:
            continue
        rec["min"] = f"{round1(hit['min']):.1f}"
        rec["mean"] = f"{round1(hit['mean']):.1f}"
        rec["max"] = f"{round1(hit['max']):.1f}"
        rec["worst"] = hit.get("worst") or rec.get("worst") or ""
    with MANIFEST.open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(table)
    print(f"updated {MANIFEST}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--update-csv",
        action="store_true",
        help="write measured min/mean/max/worst back into scripts/versions.csv",
    )
    args = parser.parse_args()

    rows = load_versions()
    if not rows:
        print(
            "no ablation summaries under runs/ablation/. "
            "Run scripts/ablation.ps1 -All first.",
            file=sys.stderr,
        )
        return 1

    style()
    plot_ladder(rows)
    plot_heatmap(rows)
    plot_breaches(rows)
    plot_floor(rows)
    plot_trajectories(rows)
    plot_deltas(rows)
    plot_mean_waterfall(rows)
    plot_timeline(rows)
    write_summary_md(rows)
    if args.update_csv:
        update_csv(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
