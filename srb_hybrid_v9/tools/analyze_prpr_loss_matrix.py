#!/usr/bin/env python3
"""Attribute current score loss by distance and endpoint obstacle proxy.

The obstacle class deliberately describes the closed endpoint rectangle, not the
unknown routed path.  It is therefore suitable for cheap online gating and for
deciding which exact-path audit to run next, but is never reported as an actual
Block/Gap traversal count.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/")
BANDS = ((0, 8, "0-8"), (9, 16, "9-16"), (17, 32, "17-32"),
         (33, 64, "33-64"), (65, 10**9, "65+"))
OBSTACLES = ("clear", "gap_only", "block_only", "block_and_gap")


@dataclass
class Aggregate:
    rows: int = 0
    score_sum: float = 0.0
    loss_sum: float = 0.0
    absolute_error_sum: int = 0
    exact: int = 0

    def add(self, golden: int, predicted: int) -> None:
        error = abs(predicted - golden)
        if golden == 0:
            score = float(predicted == 0)
        else:
            score = 1.0 - math.tanh(4.0 * error / golden)
        self.rows += 1
        self.score_sum += score
        self.loss_sum += 1.0 - score
        self.absolute_error_sum += error
        self.exact += predicted == golden


def coordinates(specification: str) -> tuple[int, int]:
    match = ENDPOINT.match(specification)
    if match is None:
        raise ValueError(f"bad endpoint: {specification}")
    return int(match.group(1)), int(match.group(2))


def delay_column(fieldnames: list[str] | None) -> str:
    for name in fieldnames or []:
        if name.strip().lower() in {"delay", "predict_delay", "min delay"}:
            return name
    raise ValueError(f"no delay column in {fieldnames}")


def distance_band(distance: int) -> str:
    return next(name for low, high, name in BANDS if low <= distance <= high)


def obstacle_class(
    sx: int, sy: int, tx: int, ty: int,
    blocks: list[dict[str, object]], lines: list[dict[str, object]],
) -> str:
    min_x, max_x = sorted((sx, tx))
    min_y, max_y = sorted((sy, ty))
    block = any(
        min_x <= int(item["right"]) and max_x >= int(item["left"])
        and min_y <= int(item["upper"]) and max_y >= int(item["lower"])
        for item in blocks
    )
    gap = any(
        (str(item["direction"]).lower() == "vertical"
         and min_x <= int(item["site"]) < max_x)
        or (str(item["direction"]).lower() != "vertical"
            and min_y <= int(item["site"]) < max_y)
        for item in lines
    )
    if block and gap:
        return "block_and_gap"
    if block:
        return "block_only"
    if gap:
        return "gap_only"
    return "clear"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--prediction", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--summary", type=Path, required=True)
    args = parser.parse_args()

    gap_root = json.loads(args.gap.read_text(encoding="utf-8-sig"))["Gap"]
    blocks = list(gap_root["Block"])
    lines = list(gap_root["Line"])
    cells: dict[tuple[str, str], Aggregate] = defaultdict(Aggregate)
    band_totals: dict[str, Aggregate] = defaultdict(Aggregate)
    obstacle_totals: dict[str, Aggregate] = defaultdict(Aggregate)
    overall = Aggregate()

    with (
        args.requests.open("r", encoding="utf-8-sig", newline="") as request_stream,
        args.golden.open("r", encoding="utf-8-sig", newline="") as golden_stream,
        args.prediction.open("r", encoding="utf-8-sig", newline="") as prediction_stream,
    ):
        requests = csv.DictReader(request_stream)
        golden_rows = csv.DictReader(golden_stream)
        prediction_rows = csv.DictReader(prediction_stream)
        golden_delay = delay_column(golden_rows.fieldnames)
        prediction_delay = delay_column(prediction_rows.fieldnames)
        for index, rows in enumerate(zip(requests, golden_rows, prediction_rows), 1):
            request, golden_row, prediction_row = rows
            endpoints = (request["From"], request["To"])
            if endpoints != (golden_row["From"], golden_row["To"]):
                raise ValueError(f"request/Golden mismatch at data row {index}")
            if endpoints != (prediction_row["From"], prediction_row["To"]):
                raise ValueError(f"request/prediction mismatch at data row {index}")
            sx, sy = coordinates(endpoints[0])
            tx, ty = coordinates(endpoints[1])
            band = distance_band(max(abs(tx - sx), abs(ty - sy)))
            obstacle = obstacle_class(sx, sy, tx, ty, blocks, lines)
            golden = int(golden_row[golden_delay])
            predicted = int(prediction_row[prediction_delay])
            cells[(band, obstacle)].add(golden, predicted)
            band_totals[band].add(golden, predicted)
            obstacle_totals[obstacle].add(golden, predicted)
            overall.add(golden, predicted)
        if next(requests, None) is not None or next(golden_rows, None) is not None \
                or next(prediction_rows, None) is not None:
            raise ValueError("row count mismatch")

    def record(scope: str, band: str, obstacle: str, value: Aggregate) -> dict[str, object]:
        return {
            "scope": scope,
            "distance_band": band,
            "obstacle_proxy": obstacle,
            "rows": value.rows,
            "request_share": value.rows / overall.rows if overall.rows else 0.0,
            "accuracy": 100.0 * value.score_sum / value.rows if value.rows else 0.0,
            "loss_sum": value.loss_sum,
            "loss_coverage": value.loss_sum / overall.loss_sum if overall.loss_sum else 0.0,
            "mean_loss": value.loss_sum / value.rows if value.rows else 0.0,
            "mae": value.absolute_error_sum / value.rows if value.rows else 0.0,
            "exact_rate": value.exact / value.rows if value.rows else 0.0,
        }

    records: list[dict[str, object]] = []
    for _, _, band in BANDS:
        for obstacle in OBSTACLES:
            records.append(record("matrix", band, obstacle, cells[(band, obstacle)]))
    for _, _, band in BANDS:
        records.append(record("distance_total", band, "all", band_totals[band]))
    for obstacle in OBSTACLES:
        records.append(record("obstacle_total", "all", obstacle, obstacle_totals[obstacle]))
    records.append(record("overall", "all", "all", overall))

    args.matrix.parent.mkdir(parents=True, exist_ok=True)
    args.summary.parent.mkdir(parents=True, exist_ok=True)
    with args.matrix.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(records[0]))
        writer.writeheader()
        writer.writerows(records)

    short = Aggregate()
    for _, high, band in BANDS:
        if high > 64:
            continue
        value = band_totals[band]
        short.rows += value.rows
        short.score_sum += value.score_sum
        short.loss_sum += value.loss_sum
        short.absolute_error_sum += value.absolute_error_sum
        short.exact += value.exact
    summary = {
        "classification_warning": (
            "Block/Gap classes use the closed endpoint rectangle and are gating proxies, "
            "not actual routed-path traversal labels."
        ),
        "overall": record("overall", "all", "all", overall),
        "short_le_64": record("distance_total", "0-64", "all", short),
        "distance_totals": {
            band: record("distance_total", band, "all", band_totals[band])
            for _, _, band in BANDS
        },
        "obstacle_totals": {
            obstacle: record("obstacle_total", "all", obstacle, obstacle_totals[obstacle])
            for obstacle in OBSTACLES
        },
        "highest_loss_cells": sorted(
            (row for row in records if row["scope"] == "matrix"),
            key=lambda row: float(row["loss_coverage"]), reverse=True,
        )[:8],
    }
    args.summary.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
