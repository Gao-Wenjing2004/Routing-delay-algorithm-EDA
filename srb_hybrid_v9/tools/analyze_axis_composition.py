#!/usr/bin/env python3
"""Audit whether long one-axis transfers equal compositions of shorter tables."""

from __future__ import annotations

import argparse
import json
import random
from array import array
from pathlib import Path


INF = 65535
STATES = 160


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--axis", type=Path, required=True)
    parser.add_argument("--radius", type=int, required=True)
    parser.add_argument("--chunk", type=int, default=64)
    parser.add_argument(
        "--overlap", type=int, default=0,
        help="allow the composition boundary to move by this many cells",
    )
    parser.add_argument("--pairs", type=int, default=1024)
    parser.add_argument("--seed", type=int, default=20261005)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 0 < args.chunk < args.radius:
        parser.error("chunk must be positive and smaller than radius")

    values = array("H")
    with args.axis.open("rb") as stream:
        values.fromfile(stream, args.axis.stat().st_size // values.itemsize)
    width = 2 * args.radius + 1
    expected = 2 * STATES * width * STATES
    if len(values) != expected:
        raise ValueError(f"axis table has {len(values)} values, expected {expected}")

    def cost(axis: int, source: int, delta: int, target: int) -> int:
        return values[
            ((axis * STATES + source) * width + delta + args.radius) * STATES + target
        ]

    all_pairs = [(source, target) for source in range(STATES) for target in range(STATES)]
    random.Random(args.seed).shuffle(all_pairs)
    pairs = all_pairs[: min(args.pairs, len(all_pairs))]
    compared = exact = direct_unreachable = composed_unreachable = composed_better = 0
    over_sum = over_max = 0
    by_axis_direction: dict[str, dict[str, int | float]] = {}
    for axis in range(2):
        for sign in (-1, 1):
            local_compared = local_exact = local_over_sum = local_over_max = 0
            for distance in range(args.chunk + 1, args.radius + 1):
                delta = sign * distance
                for source, target in pairs:
                    direct = cost(axis, source, delta, target)
                    composed = INF
                    for split in range(
                        max(1, args.chunk - args.overlap),
                        min(args.radius, args.chunk + args.overlap) + 1,
                    ):
                        first_delta = sign * split
                        second_delta = sign * (distance - split)
                        for middle in range(STATES):
                            left = cost(axis, source, first_delta, middle)
                            right = cost(axis, middle, second_delta, target)
                            if left != INF and right != INF:
                                composed = min(composed, left + right)
                                if composed == direct:
                                    break
                        if composed == direct:
                            break
                    if direct == INF:
                        direct_unreachable += 1
                        if composed != INF:
                            composed_better += 1
                        continue
                    if composed == INF:
                        composed_unreachable += 1
                        continue
                    compared += 1
                    local_compared += 1
                    if composed == direct:
                        exact += 1
                        local_exact += 1
                    elif composed < direct:
                        composed_better += 1
                    else:
                        over = composed - direct
                        over_sum += over
                        over_max = max(over_max, over)
                        local_over_sum += over
                        local_over_max = max(local_over_max, over)
            by_axis_direction[f"{'H' if axis == 0 else 'V'}{'+' if sign > 0 else '-'}"] = {
                "compared": local_compared,
                "exact": local_exact,
                "exact_rate": local_exact / local_compared if local_compared else 0.0,
                "mean_over_all_compared": local_over_sum / local_compared if local_compared else 0.0,
                "max_over": local_over_max,
            }

    report = {
        "definition": "direct exact axis transfer vs min-plus composition through a fixed chunk boundary",
        "radius": args.radius,
        "chunk": args.chunk,
        "boundary_overlap": args.overlap,
        "sampled_state_pairs": len(pairs),
        "compared": compared,
        "exact": exact,
        "exact_rate": exact / compared if compared else 0.0,
        "mean_over_all_compared": over_sum / compared if compared else 0.0,
        "max_over": over_max,
        "direct_unreachable": direct_unreachable,
        "composed_unreachable": composed_unreachable,
        "composed_better_than_direct": composed_better,
        "by_axis_direction": by_axis_direction,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
