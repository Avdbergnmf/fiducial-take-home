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
import sys
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

REPO = Path(__file__).resolve().parents[1]
ABLATION = REPO / "runs" / "ablation"
NOTES = REPO / "notes"
MANIFEST = REPO / "scripts" / "versions.csv"
IDS = ["s0", "s1", "s2", "x1-a", "x1-b", "x1-c", "x2-a", "x2-b"]

COL_FLOOR = "#b45309"
COL_MEAN = "#1d4ed8"
COL_MAX = "#64748b"
COL_BREACH = "#dc2626"
COL_OK = "#15803d"


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


def write_summary_md(rows: list[dict]) -> None:
    lines = [
        "# Ablation ladder",
        "",
        "Generated by `tools/plot_ablation.py` from `runs/ablation/`.",
        "`scripts/history.ps1` is the iterate CSV, not this table.",
        "",
        "![floor / mean / max](ablation-ladder.png)",
        "",
        "![per-id totals](ablation-heatmap.png)",
        "",
        "![kills / breaches / civilians](ablation-mission.png)",
        "",
        "![worst id per version](ablation-floor.png)",
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
        mn_s = f"{float(mn):.1f}"
        mean_s = f"{float(mean):.1f}"
        mx_s = f"{float(mx):.1f}"
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
        rec["min"] = f"{float(hit['min']):.1f}"
        rec["mean"] = f"{float(hit['mean']):.1f}"
        rec["max"] = f"{float(hit['max']):.1f}"
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
    write_summary_md(rows)
    if args.update_csv:
        update_csv(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
