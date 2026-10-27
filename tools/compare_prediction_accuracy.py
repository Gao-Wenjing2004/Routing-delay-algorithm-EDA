#!/usr/bin/env python3
"""Compare two delay predictions against one Golden CSV in a single pass."""

from __future__ import annotations

import argparse
import csv
import json
import math
from itertools import zip_longest
from pathlib import Path


def delay(row: dict[str, str]) -> int:
    for name in ("Delay", "delay"):
        value = row.get(name)
        if value is not None:
            return int(value)
    raise KeyError("CSV has neither Delay nor delay column")


def score(prediction: int, golden: int) -> float:
    return 1.0 - math.tanh(4.0 * abs(prediction - golden) / golden)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    rows = changed = improved = worsened = equal_score = 0
    control_sum = candidate_sum = 0.0
    with args.golden.open(encoding="utf-8-sig", newline="") as gf, \
         args.control.open(encoding="utf-8-sig", newline="") as cf, \
         args.candidate.open(encoding="utf-8-sig", newline="") as nf:
        readers = (csv.DictReader(gf), csv.DictReader(cf), csv.DictReader(nf))
        for triple in zip_longest(*readers):
            if any(row is None for row in triple):
                raise SystemExit("CSV row-count mismatch")
            golden_row, control_row, candidate_row = triple
            golden = delay(golden_row)
            control = delay(control_row)
            candidate = delay(candidate_row)
            control_score = score(control, golden)
            candidate_score = score(candidate, golden)
            rows += 1
            control_sum += control_score
            candidate_sum += candidate_score
            if control == candidate:
                continue
            changed += 1
            if candidate_score > control_score:
                improved += 1
            elif candidate_score < control_score:
                worsened += 1
            else:
                equal_score += 1

    if rows == 0:
        raise SystemExit("empty CSV")
    payload = {
        "rows": rows,
        "changed_rows": changed,
        "improved_rows": improved,
        "worsened_rows": worsened,
        "equal_score_changed_rows": equal_score,
        "control_accuracy": 100.0 * control_sum / rows,
        "candidate_accuracy": 100.0 * candidate_sum / rows,
        "accuracy_delta_points": 100.0 * (candidate_sum - control_sum) / rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
