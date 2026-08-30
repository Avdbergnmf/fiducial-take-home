#!/usr/bin/env bash
# check_determinism.sh -- prove the brain produces bit-identical runs.
#
#   tools/check_determinism.sh [brain.so] [id ...]
#
# Run from the package root, so the simulator finds scenarios/. Runs are
# reproducible from their id alone, down to the packet loss pattern, so any
# difference between two runs of the same id is the brain.
#
# Three separate checks, because they catch different bugs:
#   1. thread-order       --threads 1 --record, then --threads 8 --replay
#   2. repeatability      the same command twice, reports compared byte for byte
#   3. breadth            several ids, because some bugs only fire on some layouts
set -uo pipefail

SIM=${SIM:-bin/swarm_sim}
BRAIN=${1:-brain/build/brain.so}
shift || true
IDS=("${@:-x1-a x1-b x2-a s1}")
# shellcheck disable=SC2206
IDS=(${IDS[*]})

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail=0
pass=0

note() { printf '  %s\n' "$*"; }

for id in "${IDS[@]}"; do
    printf '\n=== %s ===\n' "$id"

    # -- 1. thread order ----------------------------------------------------
    # The strongest check: worker threads interleave differently at --threads 8,
    # so anything whose result depends on evaluation order diverges here.
    if ! "$SIM" --scenario "$id" --brain "$BRAIN" --threads 1 \
                --record "$WORK/$id.hash" --report "$WORK/$id.a.json" --quiet; then
        note "RUN FAILED at --threads 1 (crash or hang matters more than any score)"
        fail=$((fail + 1)); continue
    fi

    if "$SIM" --scenario "$id" --brain "$BRAIN" --threads 8 \
              --replay "$WORK/$id.hash" --quiet; then
        note "thread-order: ok"
        pass=$((pass + 1))
    else
        note "thread-order: DIVERGED"
        note "  usual causes: wall-clock read; unseeded randomness; uninitialised"
        note "  memory; iterating a container whose order depends on allocation"
        note "  addresses; an unstable sort over equal keys."
        fail=$((fail + 1)); continue
    fi

    # -- 2. repeatability ---------------------------------------------------
    # Same command twice. Catches anything that varies run to run on one thread,
    # which --replay would not, since it compares against a hash of run one.
    "$SIM" --scenario "$id" --brain "$BRAIN" --threads 1 \
           --report "$WORK/$id.b.json" --quiet >/dev/null 2>&1
    if cmp -s "$WORK/$id.a.json" "$WORK/$id.b.json"; then
        note "repeatability: ok"
        pass=$((pass + 1))
    else
        note "repeatability: reports DIFFER between two identical runs"
        diff <(head -40 "$WORK/$id.a.json") <(head -40 "$WORK/$id.b.json") | head -20
        fail=$((fail + 1))
    fi
done

printf '\n%d check(s) passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ] || exit 1
