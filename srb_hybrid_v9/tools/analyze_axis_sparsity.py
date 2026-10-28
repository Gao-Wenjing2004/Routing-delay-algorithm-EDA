#!/usr/bin/env python3
"""Measure whether exact axis transfers admit a sparse target-state layout."""

from __future__ import annotations

import argparse
import json
from array import array
from collections import Counter
from pathlib import Path


INF = 65535
STATES = 160


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--axis", type=Path, required=True)
    parser.add_argument("--radius", type=int, required=True)
    parser.add_argument("--stable-from", type=int, default=16)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    values = array("H")
    with args.axis.open("rb") as stream:
        values.fromfile(stream, args.axis.stat().st_size // values.itemsize)
    width = 2 * args.radius + 1
    expected = 2 * STATES * width * STATES
    if len(values) != expected:
        raise ValueError(f"axis table has {len(values)} values, expected {expected}")

    def targets(axis: int, source: int, delta: int) -> tuple[int, ...]:
        begin = ((axis * STATES + source) * width + delta + args.radius) * STATES
        return tuple(
            target for target in range(STATES) if values[begin + target] != INF
        )

    directions: list[dict[str, object]] = []
    for axis in range(2):
        for direction in (-1, 1):
            set_counts: Counter[int] = Counter()
            stable_sources = 0
            reachable_histogram: Counter[int] = Counter()
            target_union_total = 0
            for source in range(STATES):
                sets = [
                    targets(axis, source, direction * distance)
                    for distance in range(args.stable_from, args.radius + 1)
                ]
                unique = len(set(sets))
                set_counts[unique] += 1
                if unique == 1:
                    stable_sources += 1
                union = set().union(*map(set, sets))
                target_union_total += len(union)
                for item in sets:
                    reachable_histogram[len(item)] += 1
            directions.append({
                "axis": "H" if axis == 0 else "V",
                "direction": direction,
                "stable_from": args.stable_from,
                "sources_with_constant_target_set": stable_sources,
                "sources": STATES,
                "unique_target_sets_per_source_histogram": {
                    str(key): value for key, value in sorted(set_counts.items())
                },
                "reachable_targets_per_source_distance_histogram": {
                    str(key): value for key, value in sorted(reachable_histogram.items())
                },
                "target_union_entries": target_union_total,
            })

    report = {
        "definition": (
            "Reachable target states for each axis/source/direction across all "
            "distances at or beyond stable_from."
        ),
        "radius": args.radius,
        "stable_from": args.stable_from,
        "directions": directions,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
