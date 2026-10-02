#!/usr/bin/env python3
"""Simulate bounded-search routing policies from one max-budget trace."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path


def point_score(golden: float, predicted: float) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def load_golden(path: Path) -> list[float]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        delay = next(name for name in reader.fieldnames or [] if name.lower() == "delay")
        return [float(row[delay]) for row in reader]


def load_trace(path: Path) -> dict[str, list[float]]:
    columns: dict[str, list[float]] = {}
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            for key, value in row.items():
                columns.setdefault(key, []).append(float(value))
    return columns


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--population", type=int, default=100_000_000)
    args = parser.parse_args()

    golden = load_golden(args.golden)
    trace = load_trace(args.trace)
    if len(golden) != len(trace["row"]):
        raise ValueError("Golden/trace row count mismatch")

    v8 = trace["v8_delay"]
    exact_value = trace["output_delay"]
    expanded = trace["expanded"]
    complete = [value > 0 for value in trace["exact_completed"]]
    cheb = trace["cheb"]
    block = [value > 0 for value in trace["block_count"]]
    uncertainty = trace["uncertainty"]
    exact_us = trace["exact_us"]
    us_per_expand = [cost / max(count, 1) for cost, count in zip(exact_us, expanded)]
    base_accuracy = sum(point_score(g, p) for g, p in zip(golden, v8)) / len(golden) * 100.0

    policies: list[tuple[str, list[bool]]] = [("all", [True] * len(golden))]
    for limit in (2, 4, 8, 16, 24, 32, 48, 64, 96):
        policies.append((f"distance<={limit}", [value <= limit for value in cheb]))
    policies.append(("block", block))
    for threshold in (0.002, 0.005, 0.010, 0.020, 0.050):
        policies.append((f"uncertainty>={threshold:g}", [value >= threshold for value in uncertainty]))
    for limit in (8, 16, 24, 32, 48, 64):
        policies.append((f"distance<={limit}|block", [d <= limit or b for d, b in zip(cheb, block)]))
        policies.append(
            (f"distance<={limit}|uncertainty>=0.01", [d <= limit or u >= 0.01 for d, u in zip(cheb, uncertainty)])
        )

    rows: list[dict[str, object]] = []
    for budget in (16, 64, 256, 1024, 4096):
        finished_in_budget = [done and count <= budget for done, count in zip(complete, expanded)]
        estimated_row_us = [min(count, budget) * rate for count, rate in zip(expanded, us_per_expand)]
        for name, selected in policies:
            used = [pick and done for pick, done in zip(selected, finished_in_budget)]
            predicted = [exact if use else fast for use, exact, fast in zip(used, exact_value, v8)]
            accuracy = sum(point_score(g, p) for g, p in zip(golden, predicted)) / len(golden) * 100.0
            extra_us_per_row = sum(cost if pick else 0.0 for pick, cost in zip(selected, estimated_row_us)) / len(golden)
            projected_seconds = extra_us_per_row * args.population / 1_000_000.0
            accuracy_gain = accuracy - base_accuracy
            total_delta = 0.8 * accuracy_gain - projected_seconds / 120.0
            selected_count = sum(selected)
            used_count = sum(used)
            rows.append(
                {
                    "policy": name,
                    "max_expanded": budget,
                    "rows": len(golden),
                    "selected_rows": selected_count,
                    "selected_rate": selected_count / len(golden),
                    "exact_rows": used_count,
                    "completion_rate_selected": used_count / max(selected_count, 1),
                    "v8_accuracy": base_accuracy,
                    "hybrid_accuracy": accuracy,
                    "accuracy_gain": accuracy_gain,
                    "extra_us_per_input_row": extra_us_per_row,
                    "projected_extra_seconds_100m": projected_seconds,
                    "estimated_total_score_delta": total_delta,
                    "accuracy_gain_per_added_us": accuracy_gain / extra_us_per_row
                    if extra_us_per_row > 0
                    else math.nan,
                }
            )

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    best_accuracy = sorted(rows, key=lambda row: float(row["hybrid_accuracy"]), reverse=True)[:8]
    best_total = sorted(rows, key=lambda row: float(row["estimated_total_score_delta"]), reverse=True)[:8]
    print(f"V8 validation accuracy: {base_accuracy:.6f}")
    print("best accuracy candidates:")
    for row in best_accuracy:
        print(row)
    print("best estimated total-score candidates:")
    for row in best_total:
        print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
