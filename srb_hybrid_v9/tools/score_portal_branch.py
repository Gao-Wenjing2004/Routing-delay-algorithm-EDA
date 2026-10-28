#!/usr/bin/env python3
"""Score a legal Block-Portal branch against a fallback estimator."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def point(golden: int, predicted: int) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--portal", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    totals = {name: 0.0 for name in ("baseline", "replace", "minimum", "oracle")}
    rows = reachable = exact = legal_violations = 0
    with args.baseline.open("r", encoding="utf-8-sig", newline="") as bs, \
            args.portal.open("r", encoding="utf-8-sig", newline="") as ps:
        for baseline, portal in zip(csv.DictReader(bs), csv.DictReader(ps), strict=True):
            if (baseline["From"], baseline["To"]) != (portal["From"], portal["To"]):
                raise ValueError("row alignment mismatch")
            golden = int(portal["Golden"])
            fallback = int(baseline["Delay"])
            structured = int(portal["Predicted"])
            fallback_score = point(golden, fallback)
            totals["baseline"] += fallback_score
            if structured < 0:
                structured = fallback
            else:
                reachable += 1
                exact += structured == golden
                legal_violations += structured < golden
            structured_score = point(golden, structured)
            totals["replace"] += structured_score
            totals["minimum"] += point(golden, min(fallback, structured))
            totals["oracle"] += max(fallback_score, structured_score)
            rows += 1

    report = {
        "rows": rows,
        "portal_reachable": reachable,
        "portal_exact": exact,
        "portal_exact_rate_all": exact / max(rows, 1),
        "legal_path_below_golden_violations": legal_violations,
        **{f"{name}_accuracy": 100.0 * value / rows for name, value in totals.items()},
        "replace_gain": 100.0 * (totals["replace"] - totals["baseline"]) / rows,
        "minimum_gain": 100.0 * (totals["minimum"] - totals["baseline"]) / rows,
        "oracle_gain": 100.0 * (totals["oracle"] - totals["baseline"]) / rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
