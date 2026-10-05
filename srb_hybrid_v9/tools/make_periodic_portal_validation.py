#!/usr/bin/env python3
"""Create stratified direct-Dijkstra checks for a periodic Portal closure."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def evenly_spaced(size: int, count: int) -> list[int]:
    if count <= 0 or count > size:
        raise ValueError(f"sample count {count} is outside [1,{size}]")
    if count == 1:
        return [size // 2]
    return [round(index * (size - 1) / (count - 1)) for index in range(count)]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--periods", type=int, required=True)
    parser.add_argument("--source-boundary", type=int)
    parser.add_argument("--source-count", type=int, default=32)
    parser.add_argument("--target-count", type=int, default=128)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--mapping", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.periods <= 5:
        parser.error("periods must be in [1,5]")

    manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    dimension = int(manifest["dimension"])
    direction = str(manifest["direction"])
    sign = 1 if direction == "up" else -1
    source_boundary = (
        int(args.source_boundary)
        if args.source_boundary is not None
        else int(manifest["source_boundary"])
    )
    target_boundary = source_boundary + sign * 100 * args.periods
    source_indices = evenly_spaced(dimension, args.source_count)
    target_indices = evenly_spaced(dimension, args.target_count)

    # The event ordering is invariant under a 100-row periodic translation.
    # Rebuild absolute endpoints from the canonical event descriptors rather
    # than copying endpoints from the matrix-generation request.
    events = manifest["sources"]

    def endpoint(event: dict[str, object], boundary: int) -> str:
        phase = int(event["phase"])
        y = boundary + sign * phase
        if not 0 <= y < 550:
            raise ValueError(
                f"validation endpoint outside device: boundary={boundary}, phase={phase}"
            )
        return f"SRB_{int(event['x'])}_{y}/{event['route']}"

    rows: list[tuple[str, str, int, int]] = []
    for source_index in source_indices:
        for target_index in target_indices:
            rows.append((
                endpoint(events[source_index], source_boundary),
                endpoint(events[target_index], target_boundary),
                source_index,
                target_index,
            ))

    args.requests.parent.mkdir(parents=True, exist_ok=True)
    with args.requests.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("From", "To"))
        writer.writerows((source, target) for source, target, _, _ in rows)
    args.mapping.parent.mkdir(parents=True, exist_ok=True)
    with args.mapping.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow((
            "SourceIndex", "TargetIndex", "Periods", "SourceBoundary",
            "TargetBoundary",
        ))
        writer.writerows((
            source_index, target_index, args.periods, source_boundary, target_boundary,
        ) for _, _, source_index, target_index in rows)

    print(json.dumps({
        "direction": direction,
        "periods": args.periods,
        "source_boundary": source_boundary,
        "target_boundary": target_boundary,
        "source_count": len(source_indices),
        "target_count": len(target_indices),
        "rows": len(rows),
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
