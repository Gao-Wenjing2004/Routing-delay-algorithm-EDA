#!/usr/bin/env python3
"""Extract exact-Teacher requests that must use a periodic Block side corridor."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/")
PERIODIC_BLOCKS = tuple((76, 89, lower, lower + 49) for lower in range(0, 501, 100))


def coordinate(value: str) -> tuple[int, int]:
    match = ENDPOINT.match(value)
    if match is None:
        raise ValueError(f"bad endpoint: {value}")
    return int(match.group(1)), int(match.group(2))


def band(distance: int) -> str:
    if distance <= 128:
        return "65-128"
    if distance <= 256:
        return "129-256"
    return "257+"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    selected: list[dict[str, str]] = []
    total = reachable = 0
    by_band: Counter[str] = Counter()
    exact_path_traits: Counter[str] = Counter()
    skeletons: Counter[str] = Counter()
    vertical_corridor_x: Counter[int] = Counter()
    corridor_rows: Counter[str] = Counter()
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            total += 1
            if row["Reachable"] != "1":
                continue
            reachable += 1
            sx, sy = coordinate(row["From"])
            tx, ty = coordinate(row["To"])
            low_y, high_y = sorted((sy, ty))
            intersects = any(
                left <= sx <= right and left <= tx <= right and
                high_y >= lower and low_y <= upper
                for left, right, lower, upper in PERIODIC_BLOCKS
            )
            if not intersects:
                continue
            distance = max(abs(tx - sx), abs(ty - sy))
            by_band[band(distance)] += 1
            portal = row.get("PortalSignature", "")
            if "R" in portal:
                exact_path_traits["rim_event"] += 1
            if "B" in portal:
                exact_path_traits["crossable_block_event"] += 1
            if int(row.get("BlockSegments", "0")):
                exact_path_traits["block_segment"] += 1
            skeletons[row.get("TurnSequence", "") or "identity"] += 1
            x, y = sx, sy
            row_corridors: set[int] = set()
            for move in row.get("MoveSignature", "").split("|"):
                if not move:
                    continue
                axis, dx_text, dy_text, _ = move.split(":", 3)
                dx, dy = int(dx_text), int(dy_text)
                nx, ny = x + dx, y + dy
                if axis == "V":
                    move_low, move_high = sorted((y, ny))
                    if any(
                        move_high >= lower and move_low <= upper
                        for _, _, lower, upper in PERIODIC_BLOCKS
                    ) and not 76 <= x <= 89:
                        vertical_corridor_x[x] += 1
                        row_corridors.add(x)
                x, y = nx, ny
            if row_corridors:
                corridor_rows["has_vertical_block_corridor"] += 1
                if row_corridors <= {75, 90}:
                    corridor_rows["only_nearest_side_x75_or_x90"] += 1
                if all(64 <= value <= 101 for value in row_corridors):
                    corridor_rows["inside_side_band_plus_minus_12"] += 1
            selected.append(row)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("From", "To", "Golden", "Candidates"))
        for row in selected:
            writer.writerow((row["From"], row["To"], row["Delay"], ""))

    report = {
        "definition": (
            "Both endpoint x coordinates lie in the periodic Block columns 76..89, "
            "and the endpoint y interval intersects at least one non-crossable Block."
        ),
        "input_rows": total,
        "reachable_rows": reachable,
        "selected_rows": len(selected),
        "selected_rate": len(selected) / max(reachable, 1),
        "by_distance_band": dict(sorted(by_band.items())),
        "exact_path_traits": dict(sorted(exact_path_traits.items())),
        "turn_skeletons": dict(skeletons.most_common()),
        "vertical_block_corridor_x": {
            str(key): value for key, value in sorted(vertical_corridor_x.items())
        },
        "corridor_row_coverage": dict(sorted(corridor_rows.items())),
        "generated_candidates": (
            "No learned candidates. The C++ --block-portals branch deterministically "
            "prices H-V-H through x=75 and x=90."
        ),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
