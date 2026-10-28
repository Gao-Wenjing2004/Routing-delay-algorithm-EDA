#!/usr/bin/env python3
"""Remove unreachable exact queries while preserving aligned V8 predictions."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def delay_column(names: list[str] | None) -> str:
    return next(name for name in names or [] if name.strip().lower() in {"delay", "predict_delay"})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--labels", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--golden-out", type=Path, required=True)
    parser.add_argument("--baseline-out", type=Path, required=True)
    args = parser.parse_args()

    args.golden_out.parent.mkdir(parents=True, exist_ok=True)
    read = written = unreachable = 0
    with (
        args.labels.open("r", encoding="utf-8-sig", newline="") as labels_in,
        args.baseline.open("r", encoding="utf-8-sig", newline="") as baseline_in,
        args.golden_out.open("w", encoding="utf-8", newline="") as golden_out,
        args.baseline_out.open("w", encoding="utf-8", newline="") as baseline_out,
    ):
        labels = csv.DictReader(labels_in)
        baseline = csv.DictReader(baseline_in)
        label_delay = delay_column(labels.fieldnames)
        baseline_delay = delay_column(baseline.fieldnames)
        golden_writer = csv.DictWriter(golden_out, fieldnames=["From", "To", "Delay"])
        baseline_writer = csv.DictWriter(baseline_out, fieldnames=["From", "To", "Delay"])
        golden_writer.writeheader()
        baseline_writer.writeheader()
        for line, (label, base) in enumerate(zip(labels, baseline), 2):
            read += 1
            if (label["From"], label["To"]) != (base["From"], base["To"]):
                raise ValueError(f"endpoint mismatch at line {line}")
            reachable = label.get("Reachable", "1") == "1" and bool(label[label_delay])
            if not reachable:
                unreachable += 1
                continue
            endpoint = {"From": label["From"], "To": label["To"]}
            golden_writer.writerow({**endpoint, "Delay": label[label_delay]})
            baseline_writer.writerow({**endpoint, "Delay": base[baseline_delay]})
            written += 1
        if next(labels, None) is not None or next(baseline, None) is not None:
            raise ValueError("row count mismatch")
    print(f"read={read} reachable_written={written} unreachable_removed={unreachable}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
