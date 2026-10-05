#!/usr/bin/env python3
"""Pack ordered exact labels into a compact uint16 periodic Portal matrix."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import struct
from array import array
from pathlib import Path


INF = 65535
MAGIC = b"PPORT001"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--labels", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    dimension = int(manifest["dimension"])
    costs = array("H")
    unreachable = 0
    minimum = INF
    maximum = 0
    with args.labels.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            value = int(row["Delay"])
            if value < 0:
                costs.append(INF)
                unreachable += 1
            else:
                if value >= INF:
                    raise ValueError(f"Portal delay exceeds uint16: {value}")
                costs.append(value)
                minimum = min(minimum, value)
                maximum = max(maximum, value)
    expected = dimension * dimension
    if len(costs) != expected:
        raise ValueError(f"matrix has {len(costs)} labels, expected {expected}")

    sign = 1 if manifest["direction"] == "up" else -1
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as stream:
        stream.write(struct.pack(
            "<8sHHHHb3x", MAGIC, 1, dimension, int(manifest["side_band"]),
            int(manifest["periods"]), sign,
        ))
        costs.tofile(stream)
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest().upper()
    report = {
        "format": "PPORT001 exact periodic Portal matrix",
        "direction": manifest["direction"],
        "dimension": dimension,
        "side_band": int(manifest["side_band"]),
        "periods": int(manifest["periods"]),
        "entries": len(costs),
        "reachable": len(costs) - unreachable,
        "unreachable": unreachable,
        "density": (len(costs) - unreachable) / max(len(costs), 1),
        "minimum_delay": None if minimum == INF else minimum,
        "maximum_delay": maximum,
        "bytes": args.output.stat().st_size,
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
