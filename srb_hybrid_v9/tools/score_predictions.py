#!/usr/bin/env python3
"""Dependency-free streaming implementation of the official accuracy term."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path


def delay_key(fieldnames: list[str] | None) -> str:
    return next(name for name in fieldnames or [] if name.strip().lower() in {"delay", "predict_delay", "min delay"})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("golden", type=Path)
    parser.add_argument("prediction", type=Path)
    args = parser.parse_args()
    count = exact = 0
    score_sum = absolute_sum = relative_sum = 0.0
    with (
        args.golden.open("r", encoding="utf-8-sig", newline="") as golden_stream,
        args.prediction.open("r", encoding="utf-8-sig", newline="") as prediction_stream,
    ):
        golden_reader = csv.DictReader(golden_stream)
        prediction_reader = csv.DictReader(prediction_stream)
        golden_delay = delay_key(golden_reader.fieldnames)
        prediction_delay = delay_key(prediction_reader.fieldnames)
        for line, (golden_row, prediction_row) in enumerate(zip(golden_reader, prediction_reader), 2):
            if (golden_row["From"], golden_row["To"]) != (prediction_row["From"], prediction_row["To"]):
                raise ValueError(f"endpoint mismatch at line {line}")
            golden = int(golden_row[golden_delay])
            predicted = int(prediction_row[prediction_delay])
            error = abs(predicted - golden)
            if golden == 0:
                score = float(predicted == 0)
                relative = 0.0
            else:
                relative = error / golden
                score = 1.0 - math.tanh(4.0 * relative)
            count += 1
            exact += predicted == golden
            score_sum += score
            absolute_sum += error
            relative_sum += relative
        if next(golden_reader, None) is not None or next(prediction_reader, None) is not None:
            raise ValueError("row count mismatch")
    print(f"rows={count}")
    print(f"accuracy={score_sum / count * 100.0:.9f}")
    print(f"mae={absolute_sum / count:.9f}")
    print(f"mean_relative_error={relative_sum / count:.9f}")
    print(f"exact_matches={exact}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
