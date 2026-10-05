#!/usr/bin/env python3
"""Audit Teacher local+Portal+local decomposition with packed closures."""

from __future__ import annotations

import argparse
import csv
import json
import struct
from pathlib import Path


class Closure:
    def __init__(self, path: Path) -> None:
        self.raw = path.read_bytes()
        magic, version, self.dimension, self.band, self.periods, self.direction = \
            struct.unpack("<8sHHHHb3x", self.raw[:20])
        if magic != b"PPC10B01" or version != 1:
            raise ValueError(f"unsupported packed closure: {path}")
        self.packed_bytes = (self.dimension * self.dimension * 10 + 7) // 8 + 2
        self.period_bytes = self.dimension * 4 + self.packed_bytes
        if len(self.raw) != 20 + self.periods * self.period_bytes:
            raise ValueError(f"bad packed closure size: {path}")

    def lookup(self, period: int, source: int, target: int) -> int:
        if not 1 <= period <= self.periods:
            raise ValueError(f"period {period} is not embedded")
        base = 20 + (period - 1) * self.period_bytes
        row = struct.unpack_from("<H", self.raw, base + source * 2)[0]
        column_base = base + self.dimension * 2
        column = struct.unpack_from("<H", self.raw, column_base + target * 2)[0]
        packed_base = base + self.dimension * 4
        bit = (source * self.dimension + target) * 10
        byte, shift = divmod(bit, 8)
        window = int.from_bytes(self.raw[packed_base + byte:packed_base + byte + 3], "little")
        return row + column + ((window >> shift) & 1023)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--decomposition", type=Path, required=True)
    parser.add_argument("--up", type=Path, required=True)
    parser.add_argument("--down", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    closures = {1: Closure(args.up), -1: Closure(args.down)}

    rows = exact = under = over = 0
    absolute_error = 0
    maximum_error = 0
    examples = []
    with args.decomposition.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            rows += 1
            direction = int(row["Direction"])
            core = closures[direction].lookup(
                int(row["Periods"]), int(row["EntryIndex"]), int(row["ExitIndex"])
            )
            predicted = int(row["SourceLocal"]) + core + int(row["TargetLocal"])
            golden = int(row["Golden"])
            difference = predicted - golden
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
                    "from": row["From"], "to": row["To"],
                    "periods": int(row["Periods"]),
                    "matrix_core": core, "exact_core": int(row["ExactCore"]),
                    "predicted": predicted, "golden": golden,
                    "difference": difference,
                })
    report = {
        "rows": rows,
        "exact_rows": exact,
        "exact_rate": exact / max(rows, 1),
        "under_rows": under,
        "over_rows": over,
        "mean_absolute_error": absolute_error / max(rows, 1),
        "maximum_absolute_error": maximum_error,
        "mismatch_examples": examples,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if under == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
