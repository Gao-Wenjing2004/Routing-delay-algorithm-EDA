#!/usr/bin/env python3
"""Score sparse structured predictions with fallback to a full baseline CSV."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def score(golden: int, predicted: int) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--selected", type=Path, required=True)
    parser.add_argument(
        "--sparse", action="store_true",
        help="selected CSV contains only chosen rows rather than one row per baseline request",
    )
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    totals = {
        "rows": 0, "selected": 0, "completed": 0,
        "baseline_score": 0.0, "hybrid_score": 0.0,
        "elapsed_us": 0.0, "selected_elapsed_us": 0.0,
        "unselected_elapsed_us": 0.0, "generate_us": 0.0,
    }
    sparse_rows = None
    if args.sparse:
        with args.selected.open("r", encoding="utf-8-sig", newline="") as ss:
            sparse_rows = {
                (row["From"], row["To"]): row for row in csv.DictReader(ss)
            }
    with args.golden.open("r", encoding="utf-8-sig", newline="") as gs, \
            args.baseline.open("r", encoding="utf-8-sig", newline="") as bs:
        golden_rows = csv.DictReader(gs)
        baseline_rows = csv.DictReader(bs)
        selected_stream = None if args.sparse else args.selected.open(
            "r", encoding="utf-8-sig", newline="")
        selected_rows = None if selected_stream is None else csv.DictReader(selected_stream)
        matched_sparse = 0
        for golden, baseline in zip(golden_rows, baseline_rows, strict=True):
            key = (golden["From"], golden["To"])
            if key != (baseline["From"], baseline["To"]):
                raise ValueError(f"row alignment mismatch at {key}")
            if sparse_rows is not None:
                selected = sparse_rows.get(key)
                if selected is None:
                    selected = {
                        "Predicted": "-1", "Candidates": "0", "ElapsedUs": "0",
                        "GenerateUs": "0",
                    }
                else:
                    matched_sparse += 1
            else:
                assert selected_rows is not None
                selected = next(selected_rows)
                if key != (selected["From"], selected["To"]):
                    raise ValueError(f"selected row alignment mismatch at {key}")
            golden_delay = int(
                golden.get("delay", golden.get("Delay", golden.get("Golden", "")))
            )
            baseline_delay = int(baseline["Delay"])
            structured_delay = int(selected["Predicted"])
            chosen = structured_delay if structured_delay >= 0 else baseline_delay
            is_selected = int(selected["Candidates"]) > 0
            elapsed = float(selected["ElapsedUs"])
            totals["rows"] += 1
            totals["selected"] += is_selected
            totals["completed"] += structured_delay >= 0
            totals["baseline_score"] += score(golden_delay, baseline_delay)
            totals["hybrid_score"] += score(golden_delay, chosen)
            totals["elapsed_us"] += elapsed
            totals["selected_elapsed_us" if is_selected else "unselected_elapsed_us"] += elapsed
            totals["generate_us"] += float(selected.get("GenerateUs", 0.0))
        if selected_stream is not None:
            if next(selected_rows, None) is not None:
                raise ValueError("selected row count mismatch")
            selected_stream.close()
        if sparse_rows is not None and matched_sparse != len(sparse_rows):
            raise ValueError("one or more sparse selected rows were not found in the baseline")

    rows = int(totals["rows"])
    selected_count = int(totals["selected"])
    baseline_accuracy = 100.0 * totals["baseline_score"] / rows
    hybrid_accuracy = 100.0 * totals["hybrid_score"] / rows
    added_seconds_100m = totals["elapsed_us"] / rows * 100.0
    report = {
        "rows": rows,
        "selected": selected_count,
        "selected_rate": selected_count / rows,
        "completed": int(totals["completed"]),
        "completion_rate_selected": totals["completed"] / max(selected_count, 1),
        "baseline_accuracy": baseline_accuracy,
        "hybrid_accuracy": hybrid_accuracy,
        "accuracy_gain_points": hybrid_accuracy - baseline_accuracy,
        "mean_elapsed_us_all": totals["elapsed_us"] / rows,
        "mean_elapsed_us_selected": totals["selected_elapsed_us"] / max(selected_count, 1),
        "mean_elapsed_us_unselected": totals["unselected_elapsed_us"] / max(rows - selected_count, 1),
        "mean_generate_us_selected": totals["generate_us"] / max(selected_count, 1),
        "projected_added_seconds_100m": added_seconds_100m,
        "projected_total_score_delta": (
            0.8 * (hybrid_accuracy - baseline_accuracy) - added_seconds_100m / 120.0),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
