#!/usr/bin/env python3
"""sample_sense.py -- what sense_radius actually is, across ids.

    python tools/sample_sense.py

Fixed scenarios: --dump-params (legal). Generated ids: dump-params is
refused, so we run the mission with --verbose and parse the brain's boot
line `params sense=…` (SwBootInfo, one value per run).

Does not bake a number into the brain. Cover already uses cfg.sense_radius
from boot (D74). This is the fallback distribution if someone asks what
the unpublished draw looks like.
"""

from __future__ import annotations

import json
import math
import os
import re
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
PKG = REPO / "pkg"
OUT = REPO / "notes" / "sense-sample.json"
SENSE_RE = re.compile(r"params sense=([0-9]+(?:\.[0-9]+)?)")
FIXED = ["s0", "s1", "s2", "s3", "s4", "s5"]
NAMED_GENERATED = ["x1-a", "x1-b", "x1-c", "x2-a", "x2-b"]
FRESH_PER_TIER = 8
JOBS = 4


def sim_exe() -> Path:
    win = PKG / "bin" / "swarm_sim.exe"
    nix = PKG / "bin" / "swarm_sim"
    if win.is_file():
        return win
    if nix.is_file():
        return nix
    sys.exit("no swarm_sim in pkg/bin")


def brain_lib() -> Path:
    dll = REPO / "brain" / "build" / "Release" / "brain.dll"
    so = REPO / "brain" / "build" / "brain.so"
    if dll.is_file():
        return dll
    if so.is_file():
        return so
    sys.exit("no brain library; run scripts/build.ps1 first")


def run_sim(args: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(sim_exe()), *args],
        cwd=str(PKG),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def dump_params(scenario: str) -> float | None:
    proc = run_sim(
        ["--brain", str(brain_lib()), "--scenario", scenario, "--dump-params"]
    )
    blob = proc.stdout + "\n" + proc.stderr
    for line in blob.splitlines():
        line = line.strip()
        if line.startswith("sense.radius="):
            return float(line.split("=", 1)[1])
    return None


def new_token() -> str:
    proc = run_sim(["--new-token"])
    if proc.returncode != 0:
        sys.exit(" --new-token failed")
    return proc.stdout.strip().splitlines()[0].strip()


def sense_from_verbose(scenario: str) -> float | None:
    proc = run_sim(
        [
            "--brain",
            str(brain_lib()),
            "--scenario",
            scenario,
            "--verbose",
            "--quiet",
        ]
    )
    blob = proc.stdout + "\n" + proc.stderr
    m = SENSE_RE.search(blob)
    if not m:
        return None
    return float(m.group(1))


def mean_sd(values: list[float]) -> tuple[float, float]:
    n = len(values)
    if n == 0:
        return float("nan"), float("nan")
    mu = sum(values) / n
    if n == 1:
        return mu, 0.0
    var = sum((x - mu) ** 2 for x in values) / (n - 1)
    return mu, math.sqrt(var)


def main() -> int:
    rows: list[dict] = []

    print("fixed (--dump-params)")
    for sid in FIXED:
        sense = dump_params(sid)
        if sense is None:
            print(f"  {sid}: FAILED")
            continue
        rows.append({"id": sid, "sense": sense, "source": "dump-params"})
        print(f"  {sid}: {sense:.1f}")

    generated = list(NAMED_GENERATED)
    for tier in (1, 2):
        for _ in range(FRESH_PER_TIER):
            generated.append(f"x{tier}-{new_token()}")

    print(f"generated (--verbose, n={len(generated)}, jobs={JOBS})")
    with ThreadPoolExecutor(max_workers=JOBS) as pool:
        futs = {pool.submit(sense_from_verbose, sid): sid for sid in generated}
        for fut in as_completed(futs):
            sid = futs[fut]
            try:
                sense = fut.result()
            except Exception as exc:  # noqa: BLE001
                print(f"  {sid}: FAILED {exc}")
                continue
            if sense is None:
                print(f"  {sid}: no params line")
                continue
            rows.append({"id": sid, "sense": sense, "source": "boot-log"})
            print(f"  {sid}: {sense:.1f}")

    gen = [r["sense"] for r in rows if r["source"] == "boot-log"]
    fixed = [r["sense"] for r in rows if r["source"] == "dump-params"]
    all_v = [r["sense"] for r in rows]
    mu_g, sd_g = mean_sd(gen)
    mu_a, sd_a = mean_sd(all_v)

    summary = {
        "fixed_n": len(fixed),
        "fixed_values": sorted(set(round(x, 3) for x in fixed)),
        "generated_n": len(gen),
        "generated_min": min(gen) if gen else None,
        "generated_max": max(gen) if gen else None,
        "generated_mean": mu_g,
        "generated_sd": sd_g,
        "all_mean": mu_a,
        "all_sd": sd_a,
        "rows": rows,
        "note": (
            "Do not compile generated_mean into the brain. "
            "Config::From copies SwBootInfo::sense_radius at create (D74)."
        ),
    }
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(json.dumps(summary, indent=2), encoding="utf-8")

    print()
    print(f"fixed:      n={len(fixed)} unique={summary['fixed_values']}")
    print(
        f"generated:  n={len(gen)}  "
        f"mean={mu_g:.2f}  sd={sd_g:.2f}  "
        f"min={summary['generated_min']}  max={summary['generated_max']}"
    )
    print(f"wrote {OUT}")
    print(summary["note"])
    return 0 if gen or fixed else 1


if __name__ == "__main__":
    raise SystemExit(main())
