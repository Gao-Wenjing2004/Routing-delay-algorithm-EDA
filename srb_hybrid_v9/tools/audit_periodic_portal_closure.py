#!/usr/bin/env python3
"""Compare a Portal closure matrix with independently computed Dijkstra labels."""

from __future__ import annotations

import argparse
import csv
import json
import struct
import sys
from array import array
from pathlib import Path


INF = 65535


def read_closure(path: Path) -> tuple[dict[str, int], list[array]]:
    with path.open("rb") as stream:
        header = stream.read(20)
        if len(header) != 20:
            raise ValueError("truncated closure header")
        magic, version, dimension, band, periods, direction = struct.unpack(
            "<8sHHHHb3x", header
        )
        if magic != b"PPCLOS01" or version != 1:
            raise ValueError("unsupported closure format")
        matrices = []
        entries = dimension * dimension
        for _ in range(periods):
            values = array("H")
            values.fromfile(stream, entries)
            if sys.byteorder != "little":
                values.byteswap()
            matrices.append(values)
        if stream.read(1):
            raise ValueError("trailing bytes in closure file")
    return {
        "dimension": dimension,
        "band": band,
        "periods": periods,
        "direction": direction,
    }, matrices


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--closure", type=Path, required=True)
    parser.add_argument("--mapping", type=Path, required=True)
    parser.add_argument("--labels", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--mismatches", type=Path)
    args = parser.parse_args()

    metadata, matrices = read_closure(args.closure)
    with args.mapping.open("r", encoding="utf-8-sig", newline="") as stream:
        mapping = list(csv.DictReader(stream))
    with args.labels.open("r", encoding="utf-8-sig", newline="") as stream:
        labels = list(csv.DictReader(stream))
    if len(mapping) != len(labels):
        raise ValueError(f"mapping/label mismatch: {len(mapping)} vs {len(labels)}")

    exact = under = over = unreachable = 0
    absolute_error = 0
    maximum_error = 0
    examples: list[dict[str, object]] = []
    mismatch_rows: list[tuple[str, str]] = []
    dimension = metadata["dimension"]
    checked_periods: set[int] = set()
    for map_row, label_row in zip(mapping, labels):
        source = int(map_row["SourceIndex"])
        target = int(map_row["TargetIndex"])
        periods = int(map_row["Periods"])
        checked_periods.add(periods)
        if not 1 <= periods <= len(matrices):
            raise ValueError(f"period {periods} missing from closure")
        predicted = matrices[periods - 1][source * dimension + target]
        golden = int(label_row["Delay"])
        if golden < 0 or predicted == INF:
            unreachable += 1
            continue
        difference = int(predicted) - golden
        absolute_error += abs(difference)
        maximum_error = max(maximum_error, abs(difference))
        if difference == 0:
            exact += 1
        elif difference < 0:
            under += 1
        else:
            over += 1
        if difference and len(examples) < 20:
            examples.append({
                "source_index": source,
                "target_index": target,
                "periods": periods,
                "closure": int(predicted),
                "dijkstra": golden,
                "difference": difference,
            })
        if difference:
            mismatch_rows.append((label_row["From"], label_row["To"]))

    reachable = exact + under + over
    report = {
        **metadata,
        "checked_periods": sorted(checked_periods),
        "rows": len(mapping),
        "reachable_rows": reachable,
        "unreachable_rows": unreachable,
        "exact_rows": exact,
        "exact_rate": exact / max(reachable, 1),
        "under_rows": under,
        "over_rows": over,
        "mean_absolute_error": absolute_error / max(reachable, 1),
        "maximum_absolute_error": maximum_error,
        "mismatch_examples": examples,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    if args.mismatches:
        args.mismatches.parent.mkdir(parents=True, exist_ok=True)
        with args.mismatches.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("From", "To"))
            writer.writerows(mismatch_rows)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if under == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
