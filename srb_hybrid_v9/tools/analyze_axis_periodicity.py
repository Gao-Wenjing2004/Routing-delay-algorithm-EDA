#!/usr/bin/env python3
"""Find exact eventual periods in the obstacle-free H/V transfer tables.

For a period ``p`` and a starting distance ``s``, a state pair is counted as
periodic when reachability is identical and ``T(d+p)-T(d)`` is one constant
for every ``d`` in ``[s, radius-p]``.  Such a pair can be stored as a finite
prefix plus ``(period, increment)`` instead of one value per distance.
"""

from __future__ import annotations

import argparse
import json
from array import array
from pathlib import Path


INF = 65535
STATES = 160


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--axis", type=Path, required=True)
    parser.add_argument("--radius", type=int, required=True)
    parser.add_argument("--starts", default="16,32,48,64")
    parser.add_argument("--periods", default="1,2,3,4,5,6,10,12,15,20,30,60")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    starts = sorted({int(value) for value in args.starts.split(",") if value})
    periods = sorted({int(value) for value in args.periods.split(",") if value})
    values = array("H")
    with args.axis.open("rb") as stream:
        values.fromfile(stream, args.axis.stat().st_size // values.itemsize)
    width = 2 * args.radius + 1
    expected = 2 * STATES * width * STATES
    if len(values) != expected:
        raise ValueError(f"axis table has {len(values)} values, expected {expected}")

    def matrix_value(axis: int, direction: int, distance: int, pair: int) -> int:
        source, target = divmod(pair, STATES)
        delta_index = args.radius + direction * distance
        return values[((axis * STATES + source) * width + delta_index) * STATES + target]

    tests: list[dict[str, object]] = []
    for axis in range(2):
        for direction in (-1, 1):
            for start in starts:
                for period in periods:
                    if start < 0 or start + period > args.radius:
                        continue
                    periodic_pairs = periodic_reachable_pairs = reachable_pairs = 0
                    increments: dict[int, int] = {}
                    for pair in range(STATES * STATES):
                        increment: int | None = None
                        pair_reachable = False
                        valid = True
                        for distance in range(start, args.radius - period + 1):
                            left = matrix_value(axis, direction, distance, pair)
                            right = matrix_value(axis, direction, distance + period, pair)
                            if (left == INF) != (right == INF):
                                valid = False
                                break
                            if left == INF:
                                continue
                            pair_reachable = True
                            delta = right - left
                            if increment is None:
                                increment = delta
                            elif increment != delta:
                                valid = False
                                break
                        if pair_reachable:
                            reachable_pairs += 1
                        if valid:
                            periodic_pairs += 1
                            if increment is not None:
                                periodic_reachable_pairs += 1
                                increments[increment] = increments.get(increment, 0) + 1
                    tests.append({
                        "axis": "H" if axis == 0 else "V",
                        "direction": direction,
                        "start": start,
                        "period": period,
                        "periodic_pairs": periodic_pairs,
                        "all_pairs": STATES * STATES,
                        "periodic_pair_rate": periodic_pairs / (STATES * STATES),
                        "reachable_pairs": reachable_pairs,
                        "periodic_reachable_pairs": periodic_reachable_pairs,
                        "periodic_reachable_rate": (
                            periodic_reachable_pairs / reachable_pairs
                            if reachable_pairs else 0.0
                        ),
                        "increment_histogram": {
                            str(key): value for key, value in sorted(increments.items())
                        },
                    })

    perfect = [row for row in tests if row["periodic_pairs"] == STATES * STATES]
    report = {
        "definition": (
            "For every state pair, reachability repeats and T(d+p)-T(d) is "
            "constant for all tested d at or beyond start."
        ),
        "radius": args.radius,
        "states": STATES,
        "perfect_tests": perfect,
        "tests": tests,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({
        "radius": args.radius,
        "test_count": len(tests),
        "perfect_test_count": len(perfect),
        "perfect_tests": perfect,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
