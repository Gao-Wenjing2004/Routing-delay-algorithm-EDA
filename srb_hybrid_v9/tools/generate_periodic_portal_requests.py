#!/usr/bin/env python3
"""Generate exact routing-state requests for one periodic Block Portal transfer."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--states", type=Path, required=True)
    parser.add_argument("--direction", choices=("up", "down"), required=True)
    parser.add_argument("--side-band", type=int, default=1)
    parser.add_argument("--periods", type=int, default=1)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.side_band <= 12:
        parser.error("side-band must be in [1,12]")
    if not 1 <= args.periods <= 5:
        parser.error("periods must be in [1,5]")

    with args.states.open("r", encoding="utf-8-sig", newline="") as stream:
        state_rows = list(csv.DictReader(stream))
    sign = 1 if args.direction == "up" else -1
    directional = [
        row for row in state_rows
        if (int(row["incoming_dy"]) > 0) == (sign > 0)
        and int(row["incoming_dx"]) == 0
    ]
    if len(directional) != 40:
        raise ValueError(f"expected 40 direction states, found {len(directional)}")

    xs = list(range(76 - args.side_band, 76)) + \
        list(range(90, 90 + args.side_band))
    # An event is the first routing state after a path crosses a periodic
    # horizontal Block boundary.  The landing phase is constrained by the
    # incoming Net span encoded in that routing state.
    if args.direction == "up":
        source_boundary = 49
        target_boundary = source_boundary + 100 * args.periods
    else:
        source_boundary = 500
        target_boundary = source_boundary - 100 * args.periods

    def events(boundary: int) -> list[dict[str, object]]:
        result = []
        for x in xs:
            for row in directional:
                span = abs(int(row["incoming_dy"]))
                for phase in range(1, span + 1):
                    y = boundary + sign * phase
                    if not 0 <= y < 550:
                        raise ValueError(
                            f"portal event outside device: boundary={boundary} phase={phase}"
                        )
                    result.append({
                        "x": x,
                        "y": y,
                        "relative_x": x - (75 if x < 76 else 90),
                        "phase": phase,
                        "state": int(row["state"]),
                        "route": row["routing_input"],
                        "span": span,
                        "endpoint": f"SRB_{x}_{y}/{row['routing_input']}",
                    })
        return result

    sources = events(source_boundary)
    targets = events(target_boundary)
    if len(sources) != len(targets):
        raise AssertionError("source/target portal dimensions differ")

    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        with args.output.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("From", "To"))
            for source in sources:
                for target in targets:
                    writer.writerow((source["endpoint"], target["endpoint"]))

    manifest = {
        "definition": (
            "first routing state after crossing a periodic horizontal Block boundary; "
            "landing phase is 1..incoming Net span"
        ),
        "direction": args.direction,
        "side_band": args.side_band,
        "periods": args.periods,
        "period": 100,
        "source_boundary": source_boundary,
        "target_boundary": target_boundary,
        "x_coordinates": xs,
        "direction_state_count": len(directional),
        "events_per_x": len(sources) // len(xs),
        "dimension": len(sources),
        "request_rows": len(sources) * len(targets),
        "sources": sources,
        "targets": targets,
    }
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({
        key: manifest[key] for key in (
            "direction", "side_band", "periods", "dimension", "request_rows"
        )
    }, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
