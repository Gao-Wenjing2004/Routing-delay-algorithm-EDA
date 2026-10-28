#!/usr/bin/env python3
"""Measure exact forward/reverse local-connector vectors and compression limits."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import struct
from array import array
from collections import Counter
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--rows", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    raw = args.binary.read_bytes()
    if len(raw) < 16 or raw[:8] != b"LCONV101":
        raise ValueError("bad local connector Teacher")
    version, dimension, row_count = struct.unpack_from("<HHI", raw, 8)
    if version != 1:
        raise ValueError("unsupported connector version")
    row_bytes = 4 + dimension * 4
    if len(raw) != 16 + row_count * row_bytes:
        raise ValueError("connector file length mismatch")
    with args.rows.open(encoding="utf-8-sig", newline="") as stream:
        metadata = list(csv.DictReader(stream))
    if len(metadata) != row_count:
        raise ValueError("connector report row mismatch")

    sides = {"source": Counter(), "target": Counter()}
    raw_patterns = {"source": set(), "target": set()}
    normalized_patterns = {"source": set(), "target": set()}
    top_patterns = {name: {beam: set() for beam in (1, 2, 4, 8, 16, 32)} for name in sides}
    for row in range(row_count):
        offset = 16 + row * row_bytes + 4
        for side_index, name in enumerate(("source", "target")):
            values = array("H")
            begin = offset + side_index * dimension * 2
            values.frombytes(raw[begin:begin + dimension * 2])
            reachable = [(value, index) for index, value in enumerate(values) if value != 65535]
            reachable.sort()
            sides[name]["reachable"] += len(reachable)
            sides[name]["rows_with_reachable"] += bool(reachable)
            encoded = values.tobytes()
            raw_patterns[name].add(hashlib.sha256(encoded).digest())
            if reachable:
                base = reachable[0][0]
                normalized = array("H", (65535 if value == 65535 else value - base for value in values))
                normalized_patterns[name].add(hashlib.sha256(normalized.tobytes()).digest())
            for beam in top_patterns[name]:
                top_patterns[name][beam].add(tuple(index for _, index in reachable[:beam]))
    report = {
        "schema": 1,
        "kind": "exact-forward-reverse-local-connector-teacher-audit",
        "rows": row_count,
        "dimension": dimension,
        "connector_complete_rows": sum(
            int(row["SourceReachable"]) > 0 and int(row["TargetReachable"]) > 0 for row in metadata
        ),
        "lookup_kind": "offline-query-specific-vector; not deployable",
        "request_identity_stored": True,
        "raw_uint16_bytes": row_count * dimension * 4,
        "sides": {
            name: {
                "mean_reachable": values["reachable"] / max(row_count, 1),
                "rows_with_reachable": values["rows_with_reachable"],
                "unique_raw_vectors": len(raw_patterns[name]),
                "unique_min_normalized_vectors": len(normalized_patterns[name]),
                "unique_top_event_patterns": {
                    str(beam): len(patterns) for beam, patterns in top_patterns[name].items()
                },
            } for name, values in sides.items()
        },
        "decision": (
            "Teacher proves both endpoint directions can be priced with one forward and one reverse Dijkstra. "
            "It is not an online table and cannot authorize K48 asset retirement."
        ),
        "exact_core_coverage_pct": 0.0,
        "selected": {"gains": {"train": 0.0, "source": 0.0, "region": 0.0}},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
