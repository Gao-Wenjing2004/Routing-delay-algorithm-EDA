#!/usr/bin/env python3
"""Generate legal, stratified SRB query pairs for offline exact labeling."""

from __future__ import annotations

import argparse
import csv
import json
import random
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--inst", type=Path, required=True)
    parser.add_argument("--port", type=Path, required=True)
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--max-cheb", type=int, default=32)
    parser.add_argument("--seed", type=int, default=20261002)
    parser.add_argument("--source-direction", choices=("Input", "Output", "any"), default="any")
    parser.add_argument("--target-direction", choices=("Input", "Output", "any"), default="any")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rng = random.Random(args.seed)

    instances = json.loads(args.inst.read_text(encoding="utf-8"))["Inst"]
    coord_to_name = {(int(row["x"]), int(row["y"])): row["name"] for row in instances}
    cells = sorted(coord_to_name)
    ports = json.loads(args.port.read_text(encoding="utf-8"))["Port"]

    def names(direction: str) -> list[str]:
        values = [row["Name"] for row in ports if direction == "any" or row["Direction"] == direction]
        if not values:
            raise ValueError(f"no ports for direction {direction}")
        return values

    source_ports = names(args.source_direction)
    target_ports = names(args.target_direction)

    # Cells beside internal holes are Block-perimeter/portal samples.
    portal_cells = []
    for x, y in cells:
        if any(
            0 < nx < 119 and 0 < ny < 549 and (nx, ny) not in coord_to_name
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
        ):
            portal_cells.append((x, y))

    rows: set[tuple[str, str]] = set()
    attempts = 0
    while len(rows) < args.count:
        attempts += 1
        if attempts > args.count * 200:
            raise RuntimeError("could not generate enough unique legal queries")
        source_pool = portal_cells if portal_cells and rng.random() < 0.35 else cells
        sx, sy = rng.choice(source_pool)
        # Log-like distance mixture deliberately over-samples the very short cases
        # that are rare in a uniform whole-device draw.
        radius = rng.randint(0, args.max_cheb)
        dx = rng.randint(-radius, radius)
        dy = rng.choice((-radius, radius)) if rng.random() < 0.5 else rng.randint(-radius, radius)
        if rng.random() < 0.5:
            dx, dy = dy, dx
        target = (sx + dx, sy + dy)
        if target not in coord_to_name:
            continue
        source = f"{coord_to_name[(sx, sy)]}/{rng.choice(source_ports)}"
        destination = f"{coord_to_name[target]}/{rng.choice(target_ports)}"
        if source != destination:
            rows.add((source, destination))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("From", "To"))
        writer.writerows(sorted(rows))
    print(
        f"wrote {len(rows)} legal queries; max_cheb={args.max_cheb}; "
        f"portal_cells={len(portal_cells)}; seed={args.seed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
