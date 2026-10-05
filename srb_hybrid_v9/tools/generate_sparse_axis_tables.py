#!/usr/bin/env python3
"""Generate an exact, full-device, sparse H/V periodic-transfer artifact.

Distances below ``cutoff`` keep a dense table because reachability is still
changing.  At and beyond the cutoff, each source state stores only the target
states that remain reachable.  On this architecture the stable set is 40
targets for 120 direction-compatible sources and empty for the other 40.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from array import array
from pathlib import Path

from analyze_axis_oracle import INF, STATE_COUNT, build_axis_table


MAGIC = b"AXS9"
VERSION = 1


def table_slice(table: array, radius: int, source: int, delta: int) -> array:
    begin = (source * (2 * radius + 1) + delta + radius) * STATE_COUNT
    return table[begin:begin + STATE_COUNT]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transitions", type=Path, required=True)
    parser.add_argument("--horizontal-max", type=int, default=119)
    parser.add_argument("--vertical-max", type=int, default=549)
    parser.add_argument("--search-margin", type=int, default=64)
    parser.add_argument("--cutoff", type=int, default=32)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.cutoff <= min(args.horizontal_max, args.vertical_max):
        parser.error("cutoff must be covered by both axes")

    maxima = (args.horizontal_max, args.vertical_max)
    tables = [
        build_axis_table(
            args.transitions,
            axis,
            maximum,
            maximum + args.search_margin,
        )
        for axis, maximum in zip(("H", "V"), maxima, strict=True)
    ]

    dense = array("H")
    for table, radius in zip(tables, maxima, strict=True):
        for source in range(STATE_COUNT):
            for delta in range(-args.cutoff + 1, args.cutoff):
                dense.extend(table_slice(table, radius, source, delta))

    sections: list[tuple[array, array, array]] = []
    section_reports: list[dict[str, object]] = []
    for axis_index, (table, radius) in enumerate(zip(tables, maxima, strict=True)):
        long_count = radius - args.cutoff + 1
        for direction in (-1, 1):
            offsets = array("I", [0])
            target_ids = array("B")
            costs = array("H")
            per_source_counts: list[int] = []
            for source in range(STATE_COUNT):
                stable = tuple(
                    target
                    for target, value in enumerate(
                        table_slice(table, radius, source, direction * args.cutoff)
                    )
                    if value != INF
                )
                for distance in range(args.cutoff + 1, radius + 1):
                    current = tuple(
                        target
                        for target, value in enumerate(
                            table_slice(table, radius, source, direction * distance)
                        )
                        if value != INF
                    )
                    if current != stable:
                        raise ValueError(
                            f"reachability is not stable: axis={axis_index} "
                            f"direction={direction} source={source} distance={distance}"
                        )
                target_ids.extend(stable)
                per_source_counts.append(len(stable))
                offsets.append(len(target_ids))
                for target in stable:
                    for distance in range(args.cutoff, radius + 1):
                        value = table[
                            (
                                source * (2 * radius + 1)
                                + direction * distance
                                + radius
                            ) * STATE_COUNT
                            + target
                        ]
                        if value == INF:
                            raise AssertionError("stable target unexpectedly unreachable")
                        costs.append(value)
            if len(costs) != len(target_ids) * long_count:
                raise AssertionError("sparse cost shape mismatch")
            sections.append((offsets, target_ids, costs))
            section_reports.append({
                "axis": "H" if axis_index == 0 else "V",
                "direction": direction,
                "max_distance": radius,
                "distance_count": long_count,
                "target_entries": len(target_ids),
                "cost_entries": len(costs),
                "source_target_count_histogram": {
                    str(count): per_source_counts.count(count)
                    for count in sorted(set(per_source_counts))
                },
            })

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as stream:
        stream.write(struct.pack(
            "<4sHHHHH",
            MAGIC,
            VERSION,
            STATE_COUNT,
            args.cutoff,
            args.horizontal_max,
            args.vertical_max,
        ))
        stream.write(struct.pack("<Q", len(dense)))
        dense.tofile(stream)
        for offsets, target_ids, costs in sections:
            stream.write(struct.pack("<QQQ", len(offsets), len(target_ids), len(costs)))
            offsets.tofile(stream)
            target_ids.tofile(stream)
            costs.tofile(stream)

    digest = hashlib.sha256(args.output.read_bytes()).hexdigest().upper()
    full_dense_bytes = sum(
        STATE_COUNT * (2 * radius + 1) * STATE_COUNT * 2 for radius in maxima
    )
    report = {
        "format": "AXS9 sparse exact axis transfer v1",
        "states": STATE_COUNT,
        "cutoff": args.cutoff,
        "horizontal_max": args.horizontal_max,
        "vertical_max": args.vertical_max,
        "search_margin": args.search_margin,
        "dense_prefix_entries": len(dense),
        "sections": section_reports,
        "full_dense_bytes": full_dense_bytes,
        "sparse_bytes": args.output.stat().st_size,
        "compression_ratio": args.output.stat().st_size / full_dense_bytes,
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
