#!/usr/bin/env python3
"""Extract intermediate boundary events from exact periodic-Portal paths."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path


INSTANCE = re.compile(r"^SRB_(\d+)_(\d+)/")
MOVE = re.compile(r"^[HV]:(-?\d+):(-?\d+):[^>]+>(.+)$")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--direction", choices=("up", "down"), required=True)
    parser.add_argument("--source-boundary", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    sign = 1 if args.direction == "up" else -1

    extracted: list[dict[str, object]] = []
    x_counts: Counter[int] = Counter()
    state_counts: Counter[str] = Counter()
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            match = INSTANCE.match(row["From"])
            if not match:
                raise ValueError(f"bad source endpoint: {row['From']}")
            x, y = map(int, match.groups())
            target_match = INSTANCE.match(row["To"])
            if not target_match:
                raise ValueError(f"bad target endpoint: {row['To']}")
            target_y = int(target_match.group(2))
            possible_periods = [
                periods for periods in range(1, 6)
                if 1 <= sign * (
                    target_y - (args.source_boundary + sign * 100 * periods)
                ) <= 12
            ]
            if len(possible_periods) != 1:
                raise ValueError(
                    f"cannot infer canonical period count for target y={target_y}"
                )
            periods = possible_periods[0]
            boundaries = {
                args.source_boundary + sign * 100 * index
                for index in range(1, periods)
            }
            row_events = []
            for encoded in filter(None, row["MoveSignature"].split("|")):
                move = MOVE.match(encoded)
                if not move:
                    raise ValueError(f"bad move signature: {encoded}")
                dx, dy = int(move.group(1)), int(move.group(2))
                route = move.group(3)
                next_x, next_y = x + dx, y + dy
                for boundary in sorted(boundaries):
                    crossed = (
                        y <= boundary < next_y if sign > 0
                        else next_y < boundary <= y
                    )
                    if not crossed:
                        continue
                    phase = sign * (next_y - boundary)
                    event = {
                        "boundary": boundary,
                        "x": next_x,
                        "y": next_y,
                        "phase": phase,
                        "route": route,
                    }
                    row_events.append(event)
                    x_counts[next_x] += 1
                    state_counts[f"x={next_x}|phase={phase}|{route}"] += 1
                x, y = next_x, next_y
            extracted.append({
                "row": int(row["row"]),
                "from": row["From"],
                "to": row["To"],
                "delay": int(row["Delay"]),
                "events": row_events,
            })

    report = {
        "direction": args.direction,
        "paths": len(extracted),
        "intermediate_events": sum(len(item["events"]) for item in extracted),
        "x_counts": {str(key): value for key, value in sorted(x_counts.items())},
        "unique_event_states": len(state_counts),
        "event_state_counts": dict(state_counts.most_common()),
        "paths_detail": extracted,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({
        key: report[key] for key in (
            "direction", "paths", "intermediate_events", "x_counts",
            "unique_event_states",
        )
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
