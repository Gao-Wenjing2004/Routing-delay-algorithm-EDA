#!/usr/bin/env python3
"""Compare one or more unbounded-Dijkstra label shards with official Golden."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path

from make_stratified_exact_audit import BANDS, coordinates, obstacle_proxy


@dataclass
class Stats:
    rows: int = 0
    completed: int = 0
    exact: int = 0
    score_sum: float = 0.0
    absolute_sum: int = 0

    def add(self, golden: int, predicted: int | None) -> None:
        self.rows += 1
        if predicted is None:
            return
        self.completed += 1
        error = abs(predicted - golden)
        self.exact += predicted == golden
        self.absolute_sum += error
        self.score_sum += float(predicted == 0) if golden == 0 else \
            1.0 - math.tanh(4.0 * error / golden)

    def record(self) -> dict[str, object]:
        return {
            "rows": self.rows,
            "completed": self.completed,
            "completion_rate": self.completed / self.rows if self.rows else 0.0,
            "golden_exact_matches": self.exact,
            "golden_exact_rate_completed": self.exact / self.completed if self.completed else 0.0,
            "accuracy_completed": 100.0 * self.score_sum / self.completed if self.completed else None,
            "mae_completed": self.absolute_sum / self.completed if self.completed else None,
        }


def delay_key(fields: list[str] | None) -> str:
    return next(name for name in fields or () if name.strip().lower() in {"delay", "min delay"})


def band(distance: int) -> str:
    return next(name for low, high, name in BANDS if low <= distance <= high)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--labels", type=Path, action="append", required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--mismatches", type=Path, required=True)
    args = parser.parse_args()

    exact: dict[tuple[str, str], int] = {}
    for path in args.labels:
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            key = delay_key(reader.fieldnames)
            for row in reader:
                endpoints = (row["From"], row["To"])
                if endpoints in exact:
                    raise ValueError(f"duplicate exact label: {endpoints}")
                exact[endpoints] = int(row[key])

    gap = json.loads(args.gap.read_text(encoding="utf-8-sig"))["Gap"]
    blocks = list(gap["Block"])
    lines = list(gap["Line"])
    overall = Stats()
    grouped: dict[tuple[str, str], Stats] = defaultdict(Stats)
    mismatches: list[dict[str, object]] = []
    matched_labels: set[tuple[str, str]] = set()
    with args.golden.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        key = delay_key(reader.fieldnames)
        for index, row in enumerate(reader):
            endpoints = (row["From"], row["To"])
            predicted = exact.get(endpoints)
            if predicted is not None:
                matched_labels.add(endpoints)
            golden = int(row[key])
            sx, sy = coordinates(row["From"])
            tx, ty = coordinates(row["To"])
            distance = max(abs(tx - sx), abs(ty - sy))
            stratum = (
                band(distance), obstacle_proxy(sx, sy, tx, ty, blocks, lines)
            )
            overall.add(golden, predicted)
            grouped[stratum].add(golden, predicted)
            if predicted is not None and predicted != golden:
                mismatches.append({
                    "row": index,
                    "From": row["From"],
                    "To": row["To"],
                    "golden": golden,
                    "dijkstra": predicted,
                    "difference": predicted - golden,
                    "cheb": distance,
                    "obstacle_proxy": stratum[1],
                })

    unused = set(exact) - matched_labels
    if unused:
        raise ValueError(f"{len(unused)} exact labels do not occur in Golden")
    report = {
        "definition": "unbounded reconstructed-graph Dijkstra vs official Golden",
        "labels": [str(path) for path in args.labels],
        "overall": overall.record(),
        "by_stratum": {
            f"{distance}|{obstacle}": stats.record()
            for (distance, obstacle), stats in sorted(grouped.items())
        },
        "mismatch_count": len(mismatches),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.mismatches.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    with args.mismatches.open("w", encoding="utf-8", newline="") as stream:
        fields = ["row", "From", "To", "golden", "dijkstra", "difference",
                  "cheb", "obstacle_proxy"]
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(mismatches)
    print(json.dumps(report["overall"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
