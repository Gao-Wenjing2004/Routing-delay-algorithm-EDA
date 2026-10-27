#!/usr/bin/env python3
"""Audit where a route-period candidate beats the current estimator.

This is diagnostic only: it never emits a production gate.  Keys are purely
structural (distance, axis magnitude, quadrant and connector cardinality), so
the report exposes whether a future fixed O(1) gate has any generalizable
signal before fitting one.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import defaultdict
from itertools import zip_longest
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")


def score(golden: int, predicted: int) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def delay_column(fields: list[str] | None) -> str:
    for name in fields or []:
        if name.lower() in {"delay", "min delay", "predict_delay", "golden_delay_ps"}:
            return name
    raise ValueError(f"delay column missing: {fields}")


def endpoint(text: str) -> tuple[int, int, str]:
    match = ENDPOINT.match(text)
    if not match:
        raise ValueError(f"bad endpoint: {text}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def magnitude_bin(value: int) -> str:
    for edge in (0, 16, 32, 64, 94, 104, 128, 192, 256):
        if value <= edge:
            return f"le{edge}"
    return "257+"


def connector_counts(model: dict[str, object]) -> tuple[dict[str, int], list[int], list[int]]:
    names = {name: index for index, name in enumerate(model["port_names"])}
    sources: list[int] = []
    targets: list[int] = []
    for port in range(len(names)):
        input_id = int(model["port_to_input"][port])
        if input_id >= 0:
            route = int(model["input_to_state"][input_id])
            sources.append(1 if route >= 0 else len({
                int(model["output_nets"][int(output)][0])
                for output, _cost in model["direct_arcs"][input_id]
                if int(model["output_nets"][int(output)][0]) >= 0
            }))
            targets.append(1 if route >= 0 else 0)
        else:
            sources.append(int(int(model["output_nets"][port][0]) >= 0))
            targets.append(len(model["target_arcs"][port]))
    return names, sources, targets


def add(stats: dict[str, list[float]], key: str, baseline: float, candidate: float) -> None:
    row = stats[key]
    row[0] += 1
    row[1] += baseline
    row[2] += candidate
    row[3] += max(baseline, candidate)
    row[4] += candidate > baseline
    row[5] += candidate < baseline


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    names, source_counts, target_counts = connector_counts(model)
    stats: dict[str, list[float]] = defaultdict(lambda: [0.0] * 6)
    totals = [0.0] * 6
    with (
        args.golden.open("r", encoding="utf-8-sig", newline="") as gf,
        args.baseline.open("r", encoding="utf-8-sig", newline="") as bf,
        args.candidate.open("r", encoding="utf-8-sig", newline="") as cf,
    ):
        readers = (csv.DictReader(gf), csv.DictReader(bf), csv.DictReader(cf))
        columns = [delay_column(reader.fieldnames) for reader in readers]
        for line, rows in enumerate(zip_longest(*readers), 2):
            if any(row is None for row in rows):
                raise ValueError(f"row count mismatch at line {line}")
            golden, baseline, candidate = rows
            assert golden is not None and baseline is not None and candidate is not None
            pair = (golden["From"], golden["To"])
            if (baseline["From"], baseline["To"]) != pair or (candidate["From"], candidate["To"]) != pair:
                raise ValueError(f"endpoint mismatch at line {line}")
            sx, sy, source_name = endpoint(pair[0])
            tx, ty, target_name = endpoint(pair[1])
            dx, dy = tx - sx, ty - sy
            source_port, target_port = names[source_name], names[target_name]
            base_score = score(int(golden[columns[0]]), int(baseline[columns[1]]))
            candidate_score = score(int(golden[columns[0]]), int(candidate[columns[2]]))
            add(stats, "all", base_score, candidate_score)
            features = {
                "d": magnitude_bin(max(abs(dx), abs(dy))),
                "x": magnitude_bin(abs(dx)),
                "y": magnitude_bin(abs(dy)),
                "q": f"{int(dx > 0)}{int(dy > 0)}",
                "c": f"{source_counts[source_port]}x{target_counts[target_port]}",
            }
            for name, value in features.items():
                add(stats, f"{name}={value}", base_score, candidate_score)
            add(stats, "dxy=" + "/".join((features["d"], features["x"], features["y"])), base_score, candidate_score)
            add(stats, "dc=" + "/".join((features["d"], features["c"])), base_score, candidate_score)
            ax, ay = abs(dx), abs(dy)
            for upper in (104, 128, 160, 192, 256, 549):
                if 95 <= ax <= 119 and 93 <= ay <= upper:
                    add(stats, f"gate=x95_119_y93_{upper}", base_score, candidate_score)
                    add(stats, f"gate=x95_119_y93_{upper}/c={features['c']}", base_score, candidate_score)
            for lower in (0, 16, 32, 64, 93):
                if 95 <= ax <= 119 and lower <= ay <= 192:
                    add(stats, f"gate=x95_119_y{lower}_192", base_score, candidate_score)

    report_rows = []
    for key, values in stats.items():
        count, baseline_sum, candidate_sum, oracle_sum, wins, losses = values
        report_rows.append({
            "key": key,
            "rows": int(count),
            "baseline_accuracy": 100.0 * baseline_sum / count,
            "candidate_accuracy": 100.0 * candidate_sum / count,
            "candidate_gain": 100.0 * (candidate_sum - baseline_sum) / count,
            "oracle_gain": 100.0 * (oracle_sum - baseline_sum) / count,
            "candidate_better_rows": int(wins),
            "candidate_worse_rows": int(losses),
        })
    report_rows.sort(key=lambda row: (row["candidate_gain"], row["rows"]), reverse=True)
    report = {"schema": 1, "groups": report_rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report_rows[:30], ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
