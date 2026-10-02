#!/usr/bin/env python3
"""Learn and hold out a structural selector for bounded exact search.

This learns reusable feature buckets, never individual endpoint pairs.  The
objective is the contest total-score delta per selected row, including time.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from pathlib import Path
from statistics import fmean


def point_score(golden: float, predicted: float) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def direction(dx: int, dy: int) -> int:
    return (1 if dx > 0 else 0) + (2 if dx < 0 else 0) + (3 if dy > 0 else 0) + (6 if dy < 0 else 0)


def distance_band(distance: int) -> int:
    for index, boundary in enumerate((2, 4, 8, 16)):
        if distance <= boundary:
            return index
    return 4


def uncertainty_band(value: float) -> int:
    for index, boundary in enumerate((0.002, 0.005, 0.01, 0.02, 0.05)):
        if value < boundary:
            return index
    return 5


def key_for(row: dict[str, float], scheme: str) -> tuple[int, ...]:
    values = {
        "sc": int(row["source_class"]),
        "tc": int(row["target_class"]),
        "sr": int(row["source_route"]),
        "tr": int(row["target_route"]),
        "dir": direction(int(row["dx"]), int(row["dy"])),
        "db": distance_band(int(row["cheb"])),
        "ub": uncertainty_band(row["uncertainty"]),
        "dx": int(row["dx"]),
        "dy": int(row["dy"]),
    }
    return tuple(values[name] for name in scheme.split("+"))


def load_rows(trace_path: Path, golden_path: Path) -> list[dict[str, float]]:
    with golden_path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        delay_key = next(name for name in reader.fieldnames or [] if name.lower() == "delay")
        golden = [float(row[delay_key]) for row in reader]
    rows: list[dict[str, float]] = []
    with trace_path.open("r", encoding="utf-8-sig", newline="") as stream:
        for index, raw in enumerate(csv.DictReader(stream)):
            row = {name: float(value) for name, value in raw.items()}
            gain = 0.0
            if row["exact_completed"] > 0:
                gain = point_score(golden[index], row["output_delay"]) - point_score(
                    golden[index], row["v8_delay"]
                )
            # Total score = .8 * accuracy_percent - seconds/120.  At 100M rows,
            # one mean microsecond per input row costs 100/120 total-score points.
            row["gain"] = gain
            row["utility"] = 80.0 * gain - (100.0 / 120.0) * row["exact_us"]
            rows.append(row)
    if len(rows) != len(golden):
        raise ValueError("trace/Golden length mismatch")
    return rows


def evaluate(
    rows: list[dict[str, float]], scheme: str, minimum_count: int, margin: float
) -> dict[str, object]:
    train_groups: dict[tuple[int, ...], list[dict[str, float]]] = defaultdict(list)
    for index, row in enumerate(rows):
        if index % 2 == 0:
            train_groups[key_for(row, scheme)].append(row)
    accepted = {
        key
        for key, group in train_groups.items()
        if len(group) >= minimum_count and fmean(item["utility"] for item in group) > margin
    }

    test = [row for index, row in enumerate(rows) if index % 2 == 1]
    selected = [row for row in test if key_for(row, scheme) in accepted]
    exact = [row for row in selected if row["exact_completed"] > 0]
    mean_utility_all_short = sum(row["utility"] for row in selected) / max(len(test), 1)
    return {
        "scheme": scheme,
        "minimum_count": minimum_count,
        "margin": margin,
        "accepted_buckets": len(accepted),
        "test_rows": len(test),
        "selected_rows": len(selected),
        "selected_rate_short": len(selected) / max(len(test), 1),
        "completion_rate_selected": len(exact) / max(len(selected), 1),
        "mean_gain_selected": fmean((row["gain"] for row in selected)) if selected else 0.0,
        "mean_us_selected": fmean((row["exact_us"] for row in selected)) if selected else 0.0,
        "mean_utility_all_short": mean_utility_all_short,
        # Short<=16 has measured prevalence 15,611 / 1,000,000.
        "projected_total_score_delta": mean_utility_all_short * 0.015611,
        "accepted": [list(key) for key in sorted(accepted)],
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = load_rows(args.trace, args.golden)
    schemes = (
        "sc+tc",
        "sc+tc+db",
        "sc+tc+dir",
        "sc+tc+dir+db",
        "sc+tc+ub",
        "sc+tc+dir+ub",
        "sr+tc+db",
        "sc+tr+db",
        "sr+tc+dx+dy",
        "sc+tc+dx+dy",
    )
    results = [
        evaluate(rows, scheme, minimum_count, margin)
        for scheme in schemes
        for minimum_count in (2, 3, 5, 10, 20, 40)
        for margin in (0.0, 2.0, 5.0)
    ]
    results.sort(key=lambda row: float(row["projected_total_score_delta"]), reverse=True)
    payload = {"best": results[0], "top": results[:20]}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
