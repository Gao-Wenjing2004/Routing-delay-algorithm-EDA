#!/usr/bin/env python3
"""Audit lossless storage choices for a periodic Portal closure."""

from __future__ import annotations

import argparse
import json
import lzma
import struct
import sys
import zlib
from array import array
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--closure", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    raw = args.closure.read_bytes()
    if len(raw) < 20:
        raise ValueError("truncated closure")
    magic, version, dimension, band, periods, direction = struct.unpack(
        "<8sHHHHb3x", raw[:20]
    )
    if magic != b"PPCLOS01" or version != 1:
        raise ValueError("unsupported closure")
    values = array("H")
    values.frombytes(raw[20:])
    if sys.byteorder != "little":
        values.byteswap()
    entries_per_matrix = dimension * dimension
    if len(values) != periods * entries_per_matrix:
        raise ValueError("bad closure body length")

    row_ranges = []
    additive_residuals = []
    row_only_over_8 = 0
    additive_over_8 = 0
    additive_over_10 = 0
    for period in range(periods):
        offset = period * entries_per_matrix
        rows = []
        residual = [0] * entries_per_matrix
        for source in range(dimension):
            begin = offset + source * dimension
            row = values[begin:begin + dimension]
            row_min = min(row)
            rows.append(row_min)
            row_ranges.append(max(row) - row_min)
            for target, value in enumerate(row):
                delta = int(value) - row_min
                residual[source * dimension + target] = delta
                if delta > 255:
                    row_only_over_8 += 1
        columns = []
        for target in range(dimension):
            column_min = min(
                residual[source * dimension + target]
                for source in range(dimension)
            )
            columns.append(column_min)
        for source in range(dimension):
            for target in range(dimension):
                delta = residual[source * dimension + target] - columns[target]
                additive_residuals.append(delta)
                if delta > 255:
                    additive_over_8 += 1
                if delta > 1023:
                    additive_over_10 += 1

    body = raw[20:]
    total_entries = len(values)
    additive_max = max(additive_residuals)
    # The byte+exception estimate stores one uint8 per entry, row and column
    # uint16 potentials per matrix, plus (uint32 index,uint16 residual) escapes.
    additive_escape_bytes = (
        total_entries
        + periods * 2 * dimension * 2
        + additive_over_8 * 6
        + 20
    )
    packed_10_bytes = (
        (total_entries * 10 + 7) // 8
        + periods * 2 * dimension * 2
        + 20
    ) if additive_max <= 1023 else None
    report = {
        "dimension": dimension,
        "band": band,
        "periods": periods,
        "direction": direction,
        "raw_bytes": len(raw),
        "zlib_level9_bytes": len(zlib.compress(raw, 9)),
        "lzma_preset9_bytes": len(lzma.compress(raw, preset=9)),
        "row_range_max": max(row_ranges),
        "row_range_le_255_rate": sum(value <= 255 for value in row_ranges) / len(row_ranges),
        "row_only_over_255_entries": row_only_over_8,
        "additive_residual_max": additive_max,
        "additive_over_255_entries": additive_over_8,
        "additive_over_255_rate": additive_over_8 / total_entries,
        "additive_over_1023_entries": additive_over_10,
        "additive_uint8_escape_estimated_bytes": additive_escape_bytes,
        "additive_uint10_estimated_bytes": packed_10_bytes,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
