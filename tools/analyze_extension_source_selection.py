#!/usr/bin/env python3
"""Rank Extension source ports on one panel and validate on another.

The V68 prediction represents a full Extension table; V69 represents no
Extension.  Selecting the V68 answer by source port exactly models retaining
that source port's Extension slice while all other ports use the compact V69
path.  Ranking and validation inputs are deliberately separate.
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


def point_score(golden: int, predicted: int) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def load_port_names(path: Path) -> dict[str, int]:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        r"kSortedPortNames\s*\[kPortCount\]\s*=\s*\{(.*?)\n\};",
        text,
        flags=re.S,
    )
    if not match:
        raise ValueError("cannot find kSortedPortNames")
    names = re.findall(r'"([^"]+)"', match.group(1))
    if len(names) != 496:
        raise ValueError(f"expected 496 port names, found {len(names)}")
    return {name: index for index, name in enumerate(names)}


def endpoint_port(endpoint: str, names: dict[str, int]) -> int:
    try:
        return names[endpoint.split("/", 1)[1]]
    except (IndexError, KeyError) as error:
        raise ValueError(f"unknown endpoint port: {endpoint}") from error


def delay_column(fieldnames: list[str] | None) -> str:
    if not fieldnames:
        raise ValueError("missing CSV header")
    for field in fieldnames:
        if field.lower() in {"delay", "min delay", "predict_delay", "golden_delay_ps"}:
            return field
    raise ValueError(f"missing delay column: {fieldnames}")


def rows(golden_path: Path, full_path: Path, compact_path: Path, names: dict[str, int]):
    with (
        golden_path.open("r", encoding="utf-8-sig", newline="") as gf,
        full_path.open("r", encoding="utf-8-sig", newline="") as ff,
        compact_path.open("r", encoding="utf-8-sig", newline="") as cf,
    ):
        golden = csv.DictReader(gf)
        full = csv.DictReader(ff)
        compact = csv.DictReader(cf)
        gd = delay_column(golden.fieldnames)
        fd = delay_column(full.fieldnames)
        cd = delay_column(compact.fieldnames)
        for line, item in enumerate(zip_longest(golden, full, compact), 2):
            gr, fr, cr = item
            if gr is None or fr is None or cr is None:
                raise ValueError(f"row count mismatch at line {line}")
            pair = (gr["From"], gr["To"])
            if (fr["From"], fr["To"]) != pair or (cr["From"], cr["To"]) != pair:
                raise ValueError(f"endpoint mismatch at line {line}")
            yield (
                endpoint_port(pair[0], names),
                int(gr[gd]),
                int(fr[fd]),
                int(cr[cd]),
            )


def train_ranking(golden: Path, full: Path, compact: Path, names: dict[str, int]):
    stats: dict[int, list[float]] = defaultdict(lambda: [0, 0, 0.0])
    total = 0
    changed = 0
    for port, target, full_value, compact_value in rows(golden, full, compact, names):
        total += 1
        if full_value == compact_value:
            continue
        changed += 1
        value = stats[port]
        value[0] += 1
        value[1] += point_score(target, full_value) > point_score(target, compact_value)
        value[2] += point_score(target, full_value) - point_score(target, compact_value)
    ranking = sorted(stats, key=lambda port: (stats[port][2], stats[port][0]), reverse=True)
    return ranking, stats, total, changed


def validate(
    golden: Path,
    full: Path,
    compact: Path,
    names: dict[str, int],
    ranking: list[int],
    counts: list[int],
):
    panels = [(count, set(ranking[:count]), 0.0, 0, 0) for count in counts]
    for port, target, full_value, compact_value in rows(golden, full, compact, names):
        updated = []
        for count, selected, score_sum, row_count, full_rows in panels:
            use_full = port in selected
            predicted = full_value if use_full else compact_value
            updated.append(
                (
                    count,
                    selected,
                    score_sum + point_score(target, predicted),
                    row_count + 1,
                    full_rows + int(use_full and full_value != compact_value),
                )
            )
        panels = updated
    return [
        {
            "retained_source_ports": count,
            "accuracy": 100.0 * score_sum / row_count,
            "changed_rows_restored_to_v68": full_rows,
            "estimated_extension_bytes": round(613_396_114 * count / 94),
            "estimated_extension_build_fraction": count / 94,
        }
        for count, _selected, score_sum, row_count, full_rows in panels
    ]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--port-header", type=Path, required=True)
    parser.add_argument("--train-golden", type=Path, required=True)
    parser.add_argument("--train-full", type=Path, required=True)
    parser.add_argument("--train-compact", type=Path, required=True)
    parser.add_argument("--validation-golden", type=Path, required=True)
    parser.add_argument("--validation-full", type=Path, required=True)
    parser.add_argument("--validation-compact", type=Path, required=True)
    parser.add_argument("--counts", type=int, nargs="+", default=[0, 8, 16, 24, 32, 48, 64, 80, 94])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    names = load_port_names(args.port_header)
    reverse_names = {value: key for key, value in names.items()}
    ranking, stats, train_rows, changed = train_ranking(
        args.train_golden, args.train_full, args.train_compact, names
    )
    counts = sorted(set(max(0, min(value, len(ranking))) for value in args.counts))
    report = {
        "train_rows": train_rows,
        "train_changed_rows": changed,
        "ranked_ports": [
            {
                "rank": rank,
                "port": port,
                "name": reverse_names[port],
                "changed_rows": int(stats[port][0]),
                "v68_better_rows": int(stats[port][1]),
                "train_score_sum_gain": stats[port][2],
            }
            for rank, port in enumerate(ranking, 1)
        ],
        "validation_curve": validate(
            args.validation_golden,
            args.validation_full,
            args.validation_compact,
            names,
            ranking,
            counts,
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report["validation_curve"], indent=2))


if __name__ == "__main__":
    main()
