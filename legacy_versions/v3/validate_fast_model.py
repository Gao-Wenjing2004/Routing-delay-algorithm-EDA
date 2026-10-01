#!/usr/bin/env python3
"""Compare estimate output with a golden CSV using the contest accuracy formula."""

from __future__ import annotations

import argparse
import csv
import math
from pathlib import Path

import numpy as np


def args():
    parser = argparse.ArgumentParser()
    parser.add_argument("golden", type=Path)
    parser.add_argument("prediction", type=Path)
    return parser.parse_args()


def delay_column(fieldnames):
    for name in fieldnames or []:
        if name.strip().lower() in {"delay", "min delay", "predict_delay"}:
            return name
    raise ValueError("CSV does not contain a delay column")


def main():
    opt = args()
    relative_errors = []
    score_sum = 0.0
    exact = 0
    count = 0

    with opt.golden.open("r", encoding="utf-8-sig", newline="") as gf, opt.prediction.open(
        "r", encoding="utf-8-sig", newline=""
    ) as pf:
        golden_reader = csv.DictReader(gf)
        pred_reader = csv.DictReader(pf)
        gkey = delay_column(golden_reader.fieldnames)
        pkey = delay_column(pred_reader.fieldnames)

        for line_no, (g, p) in enumerate(zip(golden_reader, pred_reader), 2):
            if g["From"] != p["From"] or g["To"] != p["To"]:
                raise ValueError(f"endpoint mismatch at line {line_no}")
            golden = int(g[gkey])
            predicted = int(p[pkey])
            if golden == predicted:
                exact += 1
            if golden == 0:
                point_score = 1.0 if predicted == 0 else 0.0
            else:
                rel = abs(predicted - golden) / golden
                relative_errors.append(rel)
                point_score = 1.0 - math.tanh(4.0 * rel)
            score_sum += point_score
            count += 1

        try:
            next(golden_reader)
            raise ValueError("prediction CSV has fewer rows than golden CSV")
        except StopIteration:
            pass
        try:
            next(pred_reader)
            raise ValueError("prediction CSV has more rows than golden CSV")
        except StopIteration:
            pass

    rel = np.asarray(relative_errors, dtype=np.float64)
    print(f"rows                : {count:,}")
    print(f"acc_score           : {score_sum / count * 100:.6f}")
    print(f"exact matches       : {exact:,} ({exact / count * 100:.4f}%)")
    print(f"mean relative error : {rel.mean() * 100:.4f}%")
    print(f"median rel. error   : {np.median(rel) * 100:.4f}%")
    print(f"p90 relative error  : {np.percentile(rel, 90) * 100:.4f}%")
    print(f"within 5%           : {(rel <= 0.05).mean() * 100:.4f}%")
    print(f"within 10%          : {(rel <= 0.10).mean() * 100:.4f}%")


if __name__ == "__main__":
    main()

