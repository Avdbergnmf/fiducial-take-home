#!/usr/bin/env python3
"""plot_orbit_tradeoff.py -- leftover Reach vs orbit rate (D70).

    python tools/plot_orbit_tradeoff.py

Closed-form leftover on the unique-owner Voronoi inbound, same first-sight /
Reach model as CoverCloses. Writes notes/orbit-cover-tradeoff.png.
Numbers are the D70 plant (H = 25 m), not a re-fit at the live 20 m picket.
"""

from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / "notes" / "orbit-cover-tradeoff.png"

OMEGA = [0.0, 0.02, 0.03, 0.04, 0.05, 0.06, 0.08, 0.10, 0.12]
KNIFE_APPROACH = [1.9, 3.32, 3.74, 3.97, 4.0, 3.83, 2.89, 1.93, 1.15]
KNIFE_RECEDE = [1.9, -0.22, -1.53, -2.98, -4.58, -6.29, -10.07, -14.23, -18.73]
FAT_APPROACH = [26.35, 26.5, 26.19, 25.96, 25.78, 25.53, 24.23, 19.91, 15.15]
FAT_RECEDE = [26.35, 24.58, 23.13, 21.33, 19.21, 16.8, 11.23, 4.83, -2.21]
TIGHT_APPROACH = [-0.55, -0.29, -0.49, -0.22, 0.51, 1.46, 2.85, 3.6, 5.72]
TIGHT_RECEDE = [-0.55, -1.77, -2.71, -3.88, -5.24, -6.79, -10.41, -14.64, -19.37]
AC_FRAC_PCT = [0, 0.5, 1.2, 2.1, 3.2, 4.6, 8.2, 12.9, 18.5]

COL_APP = "#15803d"
COL_REC = "#dc2626"
COL_TAX = "#b45309"
COL_SHIP = "#1d4ed8"


def style():
    plt.rcParams.update(
        {
            "figure.facecolor": "white",
            "axes.facecolor": "white",
            "axes.grid": True,
            "grid.alpha": 0.25,
            "font.size": 9,
            "axes.titlesize": 11,
            "axes.titleweight": "semibold",
        }
    )


def leftover_ax(ax, title, approach, recede, ylabel=True):
    ax.axhline(0.0, color="#94a3b8", lw=0.8)
    ax.axvline(0.06, color=COL_SHIP, lw=1.0, ls="--", label="shipped 0.06")
    ax.plot(OMEGA, approach, color=COL_APP, lw=2.0, marker="o", ms=4, label="approaching owner")
    ax.plot(OMEGA, recede, color=COL_REC, lw=2.0, marker="o", ms=4, label="receding neighbour")
    ax.set_title(title)
    ax.set_xlabel("ω (rad/s)")
    if ylabel:
        ax.set_ylabel("leftover Reach (m)")


def main() -> int:
    style()
    fig, axes = plt.subplots(2, 2, figsize=(10.4, 7.2))
    leftover_ax(
        axes[0, 0],
        "Knife-edge · N=6, R=76 m  (D58 just closed)",
        KNIFE_APPROACH,
        KNIFE_RECEDE,
    )
    leftover_ax(
        axes[0, 1],
        "Fat radio ring · N=16, R=86 m",
        FAT_APPROACH,
        FAT_RECEDE,
        ylabel=False,
    )
    leftover_ax(
        axes[1, 0],
        "Tight sense · 35 m, N=16, R=86 m",
        TIGHT_APPROACH,
        TIGHT_RECEDE,
    )

    ax = axes[1, 1]
    ax.axvline(0.06, color=COL_SHIP, lw=1.0, ls="--", label="shipped 0.06")
    ax.plot(OMEGA, AC_FRAC_PCT, color=COL_TAX, lw=2.0, marker="o", ms=4, label="a_c / lat")
    ax.set_title("Station-keeping tax · R=86 m")
    ax.set_xlabel("ω (rad/s)")
    ax.set_ylabel("centripetal share of lat (%)")

    handles, labels = axes[0, 0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="upper center", ncol=3, framealpha=0.95, bbox_to_anchor=(0.5, 1.02))
    fig.suptitle(
        "Orbit tradeoff — leftover Reach vs ω  (D70 plant, H=25 m)",
        y=1.06,
        fontsize=13,
        fontweight="semibold",
    )
    fig.tight_layout()
    OUT.parent.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT, dpi=140, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote {OUT}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
