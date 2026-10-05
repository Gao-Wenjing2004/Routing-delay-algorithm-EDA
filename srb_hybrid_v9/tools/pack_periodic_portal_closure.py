#!/usr/bin/env python3
"""Losslessly pack Portal closure costs as row+column+10-bit residuals."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from array import array
from pathlib import Path


OUTPUT_MAGIC = b"PPC10B01"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--embed-periods", type=int)
    args = parser.parse_args()

    raw = args.input.read_bytes()
    if len(raw) < 20:
        raise ValueError("truncated closure")
    magic, version, dimension, band, periods, direction = struct.unpack(
        "<8sHHHHb3x", raw[:20]
    )
    if magic != b"PPCLOS01" or version != 1:
        raise ValueError("unsupported closure")
    embed_periods = args.embed_periods or periods
    if not 1 <= embed_periods <= periods:
        raise ValueError("embed-periods is outside the closure range")
    values = array("H")
    values.frombytes(raw[20:])
    if sys.byteorder != "little":
        values.byteswap()
    entries = dimension * dimension
    if len(values) != periods * entries:
        raise ValueError("bad closure body length")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    residual_maxima = []
    with args.output.open("wb") as output:
        output.write(struct.pack(
            "<8sHHHHb3x", OUTPUT_MAGIC, 1, dimension, band,
            embed_periods, direction,
        ))
        for period in range(embed_periods):
            offset = period * entries
            row_bases = array("H")
            residual = array("H", [0]) * entries
            for source in range(dimension):
                begin = offset + source * dimension
                row = values[begin:begin + dimension]
                base = min(row)
                row_bases.append(base)
                for target, value in enumerate(row):
                    residual[source * dimension + target] = value - base
            column_bases = array("H")
            for target in range(dimension):
                base = min(residual[target::dimension])
                column_bases.append(base)
                for source in range(dimension):
                    index = source * dimension + target
                    residual[index] -= base
            maximum = max(residual)
            residual_maxima.append(maximum)
            if maximum > 1023:
                raise ValueError(
                    f"period {period + 1} residual {maximum} exceeds 10 bits"
                )
            if sys.byteorder != "little":
                row_bases.byteswap()
                column_bases.byteswap()
            row_bases.tofile(output)
            column_bases.tofile(output)
            accumulator = 0
            bits = 0
            packed = bytearray()
            for value in residual:
                accumulator |= int(value) << bits
                bits += 10
                while bits >= 8:
                    packed.append(accumulator & 0xff)
                    accumulator >>= 8
                    bits -= 8
            if bits:
                packed.append(accumulator & 0xff)
            expected = (entries * 10 + 7) // 8
            if len(packed) != expected:
                raise AssertionError("10-bit packing length mismatch")
            packed.extend(b"\0\0")  # permits one branch-free 24-bit random read
            output.write(packed)

    digest = hashlib.sha256(args.output.read_bytes()).hexdigest().upper()
    report = {
        "format": "PPC10B01 row+column+10-bit Portal closure",
        "dimension": dimension,
        "band": band,
        "source_periods": periods,
        "embedded_periods": embed_periods,
        "direction": direction,
        "residual_maxima": residual_maxima,
        "raw_source_bytes": len(raw),
        "packed_bytes": args.output.stat().st_size,
        "compression_ratio": args.output.stat().st_size / len(raw),
        "sha256": digest,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
