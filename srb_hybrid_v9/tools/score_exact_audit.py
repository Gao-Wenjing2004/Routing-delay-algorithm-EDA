#!/usr/bin/env python3
"""Score completed compact-exact audit rows against official Golden labels."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


BANDS = ((0, 8, "0-8"), (9, 16, "9-16"), (17, 32, "17-32"),
         (33, 64, "33-64"), (65, 10**9, "65+"))


@dataclass
class Stats:
    rows: int = 0
    completed: int = 0
    exact_matches: int = 0
    score_sum: float = 0.0
    absolute_sum: int = 0
    expanded_sum: int = 0
    elapsed_us_sum: float = 0.0

    def add(self, golden: int, trace: dict[str, str]) -> None:
        self.rows += 1
        self.expanded_sum += int(trace["expanded"])
        self.elapsed_us_sum += float(trace["elapsed_us"])
        if trace["completed"] != "1":
            return
        exact = int(trace["exact_delay"])
        error = abs(exact - golden)
        score = float(exact == 0) if golden == 0 else 1.0 - math.tanh(4.0 * error / golden)
        self.completed += 1
        self.exact_matches += exact == golden
        self.score_sum += score
        self.absolute_sum += error

    def record(self) -> dict[str, object]:
        return {
            "rows": self.rows,
            "completed": self.completed,
            "completion_rate": self.completed / self.rows if self.rows else 0.0,
            "golden_exact_matches": self.exact_matches,
            "golden_exact_rate_completed": self.exact_matches / self.completed if self.completed else 0.0,
            "accuracy_completed": 100.0 * self.score_sum / self.completed if self.completed else None,
            "mae_completed": self.absolute_sum / self.completed if self.completed else None,
            "mean_expanded": self.expanded_sum / self.rows if self.rows else 0.0,
            "mean_elapsed_us": self.elapsed_us_sum / self.rows if self.rows else 0.0,
        }


def delay_column(fieldnames: list[str] | None) -> str:
    return next(name for name in fieldnames or [] if name.strip().lower() in {"delay", "min delay"})


def band(cheb: int) -> str:
    return next(name for low, high, name in BANDS if low <= cheb <= high)


def obstacle(trace: dict[str, str]) -> str:
    has_block = int(trace["block_count"]) > 0
    has_gap = int(trace["horizontal_gaps"]) + int(trace["vertical_gaps"]) > 0
    if has_block and has_gap:
        return "block_and_gap"
    if has_block:
        return "block_only"
    if has_gap:
        return "gap_only"
    return "clear"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--mismatches", type=Path, required=True)
    args = parser.parse_args()

    overall = Stats()
    groups: dict[tuple[str, str], Stats] = defaultdict(Stats)
    mismatch_rows: list[dict[str, object]] = []
    with (
        args.golden.open("r", encoding="utf-8-sig", newline="") as golden_stream,
        args.trace.open("r", encoding="utf-8-sig", newline="") as trace_stream,
    ):
        golden_rows = csv.DictReader(golden_stream)
        trace_rows = csv.DictReader(trace_stream)
        golden_delay = delay_column(golden_rows.fieldnames)
        for index, (golden, trace) in enumerate(zip(golden_rows, trace_rows), 1):
            if (golden["From"], golden["To"]) != (trace["From"], trace["To"]):
                raise ValueError(f"endpoint mismatch at data row {index}")
            value = int(golden[golden_delay])
            key = (band(int(trace["cheb"])), obstacle(trace))
            overall.add(value, trace)
            groups[key].add(value, trace)
            if trace["completed"] == "1" and int(trace["exact_delay"]) != value:
                mismatch_rows.append({
                    "row": index - 1,
                    "From": trace["From"],
                    "To": trace["To"],
                    "golden": value,
                    "exact": int(trace["exact_delay"]),
                    "difference": int(trace["exact_delay"]) - value,
                    "cheb": int(trace["cheb"]),
                    "obstacle_proxy": key[1],
                    "expanded": int(trace["expanded"]),
                })
        if next(golden_rows, None) is not None or next(trace_rows, None) is not None:
            raise ValueError("row count mismatch")

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.mismatches.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "warning": "Obstacle labels are endpoint-rectangle proxies, not actual path traversals.",
        "overall": overall.record(),
        "by_stratum": {
            f"{distance}|{kind}": stats.record()
            for (distance, kind), stats in sorted(groups.items())
        },
        "mismatch_count": len(mismatch_rows),
    }
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    with args.mismatches.open("w", encoding="utf-8", newline="") as stream:
        fields = ["row", "From", "To", "golden", "exact", "difference",
                  "cheb", "obstacle_proxy", "expanded"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(mismatch_rows)
    print(json.dumps(report["overall"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
