#!/usr/bin/env python3
"""Discover what is in a JSON Lines trace. Standard library only.

This is deliberately NOT a parser: it counts records by their `type` field and
prints raw examples verbatim. Nothing here interprets a field's meaning, so it
cannot go stale when the recording format changes.

Usage:
    python tools/inspect_trace.py <trace.jsonl>
"""

import json
import os
import sys


def main(argv):
    if len(argv) != 2:
        print(__doc__.strip(), file=sys.stderr)
        return 2

    path = argv[1]
    if not os.path.exists(path):
        print("no such file: %s" % path, file=sys.stderr)
        return 1

    counts = {}          # type -> number of records
    first = {}           # type -> first record seen, unmodified
    order = []           # types in order of first appearance
    last_type = None
    lines = 0
    parsed = 0
    skipped = 0

    with open(path, "r", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            lines += 1
            stripped = line.strip()
            if not stripped:
                continue
            try:
                record = json.loads(stripped)
            except ValueError as exc:
                skipped += 1
                print("warning: line %d is not valid JSON (%s)" % (lines, exc),
                      file=sys.stderr)
                continue
            parsed += 1
            if not isinstance(record, dict):
                kind = "<not-an-object:%s>" % type(record).__name__
            else:
                kind = record.get("type", "<no-type-field>")
            kind = str(kind)
            if kind not in counts:
                counts[kind] = 0
                first[kind] = record
                order.append(kind)
            counts[kind] += 1
            last_type = kind

    print("file          : %s" % path)
    print("size          : %d bytes" % os.path.getsize(path))
    print("lines         : %d  (parsed %d, skipped %d)" % (lines, parsed, skipped))
    print("last record   : %s" % last_type)
    print()

    print("record types")
    print("-" * 48)
    width = max([len(k) for k in order] + [4])
    for kind in sorted(order, key=lambda k: -counts[k]):
        print("%-*s  %d" % (width, kind, counts[kind]))
    print()

    if "header" in first:
        print("=" * 60)
        print("header record (defines the frame column names)")
        print("=" * 60)
        print(json.dumps(first["header"], indent=2, sort_keys=False))
        print()

    for kind in order:
        print("=" * 60)
        print("first %r record" % kind)
        print("=" * 60)
        print(json.dumps(first[kind], indent=2, sort_keys=False))
        print()

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
