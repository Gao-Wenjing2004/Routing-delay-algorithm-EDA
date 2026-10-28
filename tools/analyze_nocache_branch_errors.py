#!/usr/bin/env python3
"""Score K48 decision branches on exact, source/region-isolated Golden data."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import defaultdict
from dataclasses import dataclass
from itertools import zip_longest
from pathlib import Path


@dataclass
class Metric:
    rows: int = 0
    score: float = 0.0
    absolute: int = 0
    exact: int = 0

    def add(self, golden: int, predicted: int) -> None:
        self.rows += 1
        error = abs(predicted - golden)
        self.absolute += error
        self.exact += predicted == golden
        self.score += 1.0 if golden == 0 and predicted == 0 else (
            0.0 if golden == 0 else 1.0 - math.tanh(4.0 * error / golden)
        )

    def result(self) -> dict[str, float | int]:
        return {
            "rows": self.rows,
            "accuracy": 100.0 * self.score / self.rows if self.rows else math.nan,
            "mae_ps": self.absolute / self.rows if self.rows else math.nan,
            "exact_share_pct": 100.0 * self.exact / self.rows if self.rows else math.nan,
        }


def delay_column(fields: list[str] | None) -> str:
    for name in fields or ():
        if name.lower() in {"delay", "min delay", "predict_delay", "golden_delay_ps"}:
            return name
    raise ValueError(f"no delay column in {fields}")


def load_categories(path: Path | None) -> dict[tuple[str, str], str]:
    if path is None:
        return {}
    with path.open(encoding="utf-8-sig", newline="") as handle:
        return {(row["From"], row["To"]): row["Category"] for row in csv.DictReader(handle)}


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--metadata", type=Path)
    parser.add_argument("--split", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    categories = load_categories(args.metadata)
    overall = Metric()
    grouped: dict[str, dict[str, Metric]] = {
        "branch": defaultdict(Metric),
        "category": defaultdict(Metric),
        "distance": defaultdict(Metric),
    }
    with args.golden.open(encoding="utf-8-sig", newline="") as gf, args.trace.open(
        encoding="utf-8-sig", newline=""
    ) as tf:
        golden_reader, trace_reader = csv.DictReader(gf), csv.DictReader(tf)
        gd, td = delay_column(golden_reader.fieldnames), delay_column(trace_reader.fieldnames)
        for line, pair in enumerate(zip_longest(golden_reader, trace_reader), 2):
            golden, trace = pair
            if golden is None or trace is None:
                raise ValueError(f"row count mismatch at line {line}")
            key = golden["From"], golden["To"]
            if key != (trace["From"], trace["To"]):
                raise ValueError(f"alignment mismatch at line {line}")
            gv, pv = int(golden[gd]), int(trace[td])
            overall.add(gv, pv)
            grouped["branch"][trace["Branch"]].add(gv, pv)
            grouped["category"][categories.get(key, "unclassified")].add(gv, pv)
            sx, sy = (int(value) for value in key[0].split("/", 1)[0][4:].split("_"))
            tx, ty = (int(value) for value in key[1].split("/", 1)[0][4:].split("_"))
            distance = max(abs(tx - sx), abs(ty - sy))
            bucket = "0-16" if distance <= 16 else "17-32" if distance <= 32 else "33-64" if distance <= 64 else "65-128" if distance <= 128 else "129-256" if distance <= 256 else "257+"
            grouped["distance"][bucket].add(gv, pv)
    report = {
        "schema": 1,
        "split": args.split,
        "overall": overall.result(),
        "segments": {
            dimension: {name: metric.result() for name, metric in sorted(values.items())}
            for dimension, values in grouped.items()
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
