#!/usr/bin/env python3
"""Decompose exact Teacher paths into local connectors and a Portal core."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
MOVE = re.compile(r"^[HV]:(-?\d+):(-?\d+):([^>]+)>(.+)$")


def endpoint(value: str) -> tuple[int, int, str]:
    match = ENDPOINT.match(value)
    if not match:
        raise ValueError(f"bad endpoint: {value}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def load_architecture(arc_path: Path, net_path: Path, gap_path: Path):
    arcs_json = json.loads(arc_path.read_text(encoding="utf-8-sig"))["Arcs"]
    arcs: dict[tuple[str, str], int] = {}
    for item in arcs_json:
        key = (item["from"], item["to"])
        arcs[key] = min(arcs.get(key, 1 << 30), int(item["delay"]))
    nets_json = json.loads(net_path.read_text(encoding="utf-8-sig"))["Nets"]
    nets = {
        item["from"]: (item["to"], int(item["delta x"]), int(item["delta y"]))
        for item in nets_json
    }
    gap = json.loads(gap_path.read_text(encoding="utf-8-sig"))["Gap"]
    return arcs, nets, gap["Line"], gap["Block"]


def segment_extra(
    x: int,
    y: int,
    next_x: int,
    next_y: int,
    lines: list[dict[str, object]],
    blocks: list[dict[str, object]],
) -> int:
    horizontal = y == next_y
    if horizontal == (x == next_x):
        raise ValueError("move is not on exactly one axis")
    extra = 0
    lo = min(x, next_x) if horizontal else min(y, next_y)
    hi = max(x, next_x) if horizontal else max(y, next_y)
    for line in lines:
        if horizontal and line["direction"] == "vertical":
            if lo <= int(line["site"]) < hi:
                extra += int(line["delay"])
        elif not horizontal and line["direction"] == "horizontal":
            if lo <= int(line["site"]) < hi:
                extra += int(line["delay"])
    for block in blocks:
        crossable_key = "horizontal crossable" if horizontal else "vertical crossable"
        if not bool(block[crossable_key]):
            continue
        if horizontal:
            same_lane = int(block["lower"]) <= y <= int(block["upper"])
            intersects = max(lo + 1, int(block["left"])) <= min(hi, int(block["right"]))
            delay_key = "horizontal cross delay"
        else:
            same_lane = int(block["left"]) <= x <= int(block["right"])
            intersects = max(lo + 1, int(block["lower"])) <= min(hi, int(block["upper"]))
            delay_key = "vertical cross delay"
        if same_lane and intersects:
            extra += int(block[delay_key])
    return extra


def event_index(manifest: dict[str, object]) -> dict[tuple[int, int, str], int]:
    return {
        (int(item["x"]), int(item["phase"]), str(item["route"])): index
        for index, item in enumerate(manifest["sources"])  # type: ignore[arg-type]
    }


def collapsed_skeleton(moves: list[str]) -> str:
    result = []
    for move in moves:
        axis = move[0]
        if not result or result[-1] != axis:
            result.append(axis)
    return ">".join(result)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--up-manifest", type=Path, required=True)
    parser.add_argument("--down-manifest", type=Path, required=True)
    parser.add_argument("--arc", type=Path, required=True)
    parser.add_argument("--net", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    with args.requests.open("r", encoding="utf-8-sig", newline="") as stream:
        selected = {(row["From"], row["To"]) for row in csv.DictReader(stream)}
    manifests = {
        1: event_index(json.loads(args.up_manifest.read_text(encoding="utf-8-sig"))),
        -1: event_index(json.loads(args.down_manifest.read_text(encoding="utf-8-sig"))),
    }
    arcs, nets, lines, blocks = load_architecture(args.arc, args.net, args.gap)

    output_rows = []
    pricing_mismatches = []
    missing_events = 0
    selected_seen = 0
    by_period: Counter[int] = Counter()
    source_skeletons: Counter[str] = Counter()
    target_skeletons: Counter[str] = Counter()
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if (row["From"], row["To"]) not in selected:
                continue
            selected_seen += 1
            source_x, source_y, source_port = endpoint(row["From"])
            target_x, target_y, target_port = endpoint(row["To"])
            direction = 1 if target_y > source_y else -1 if target_y < source_y else 0
            if direction == 0:
                continue
            boundaries = (
                [49 + 100 * index for index in range(6)]
                if direction > 0 else [100 * index for index in range(1, 6)]
            )
            x, y, current_port = source_x, source_y, source_port
            cumulative = 0
            events = []
            encoded_moves = []
            for move_index, encoded in enumerate(filter(None, row["MoveSignature"].split("|"))):
                encoded_moves.append(encoded)
                match = MOVE.match(encoded)
                if not match:
                    raise ValueError(f"bad move signature: {encoded}")
                dx, dy = int(match.group(1)), int(match.group(2))
                output_port, next_input = match.group(3), match.group(4)
                next_x, next_y = x + dx, y + dy
                move_cost = 0
                if current_port != output_port:
                    try:
                        move_cost += arcs[(current_port, output_port)]
                    except KeyError as error:
                        raise ValueError(
                            f"missing Arc {current_port}->{output_port}"
                        ) from error
                if output_port not in nets or nets[output_port][0] != next_input:
                    raise ValueError(f"bad Net {output_port}->{next_input}")
                move_cost += segment_extra(x, y, next_x, next_y, lines, blocks)
                cumulative += move_cost
                for boundary in boundaries:
                    crossed = (
                        y <= boundary < next_y if direction > 0
                        else next_y < boundary <= y
                    )
                    if not crossed:
                        continue
                    phase = direction * (next_y - boundary)
                    key = (next_x, phase, next_input)
                    index = manifests[direction].get(key)
                    if index is None:
                        missing_events += 1
                        continue
                    events.append({
                        "boundary": boundary,
                        "endpoint": f"SRB_{next_x}_{next_y}/{next_input}",
                        "index": index,
                        "cumulative": cumulative,
                        "move_index": move_index,
                    })
                x, y, current_port = next_x, next_y, next_input

            priced = cumulative
            if (x, y, current_port) != (target_x, target_y, target_port):
                if (x, y) != (target_x, target_y):
                    raise ValueError(f"path did not reach target cell: {row['From']}->{row['To']}")
                try:
                    priced += arcs[(current_port, target_port)]
                except KeyError as error:
                    raise ValueError(
                        f"missing final Arc {current_port}->{target_port}"
                    ) from error
            golden = int(row["Delay"])
            if priced != golden:
                pricing_mismatches.append({
                    "from": row["From"], "to": row["To"],
                    "priced": priced, "golden": golden,
                })

            # Keep the first forward crossing of each boundary and preserve the
            # route order.  Positive-weight shortest paths should not need a
            # recrossing, but this makes the extraction deterministic.
            unique_events = []
            seen_boundaries = set()
            for event in events:
                if event["boundary"] in seen_boundaries:
                    continue
                seen_boundaries.add(event["boundary"])
                unique_events.append(event)
            if len(unique_events) < 2:
                continue
            entry, exit_event = unique_events[0], unique_events[-1]
            periods = abs(int(exit_event["boundary"]) - int(entry["boundary"])) // 100
            if not 1 <= periods <= 4:
                continue
            prefix = int(entry["cumulative"])
            core = int(exit_event["cumulative"]) - prefix
            suffix = golden - int(exit_event["cumulative"])
            entry_move = int(entry["move_index"])
            exit_move = int(exit_event["move_index"])
            source_moves = encoded_moves[:entry_move + 1]
            target_moves = encoded_moves[exit_move + 1:]
            source_skeleton = collapsed_skeleton(source_moves)
            target_skeleton = collapsed_skeleton(target_moves)
            source_skeletons[source_skeleton] += 1
            target_skeletons[target_skeleton] += 1
            by_period[periods] += 1
            output_rows.append({
                "From": row["From"],
                "To": row["To"],
                "Golden": golden,
                "Direction": direction,
                "Periods": periods,
                "EntryEndpoint": entry["endpoint"],
                "ExitEndpoint": exit_event["endpoint"],
                "EntryIndex": entry["index"],
                "ExitIndex": exit_event["index"],
                "SourceLocal": prefix,
                "ExactCore": core,
                "TargetLocal": suffix,
                "SourceLocalSkeleton": source_skeleton,
                "TargetLocalSkeleton": target_skeleton,
                "SourceLocalMoves": "|".join(source_moves),
                "TargetLocalMoves": "|".join(target_moves),
            })

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fieldnames = [
        "From", "To", "Golden", "Direction", "Periods", "EntryEndpoint",
        "ExitEndpoint", "EntryIndex", "ExitIndex", "SourceLocal", "ExactCore",
        "TargetLocal", "SourceLocalSkeleton", "TargetLocalSkeleton",
        "SourceLocalMoves", "TargetLocalMoves",
    ]
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(output_rows)
    report = {
        "selected_requests": len(selected),
        "selected_summaries_seen": selected_seen,
        "decomposed_rows": len(output_rows),
        "period_counts": {str(key): value for key, value in sorted(by_period.items())},
        "missing_manifest_events": missing_events,
        "path_pricing_mismatches": len(pricing_mismatches),
        "path_pricing_mismatch_examples": pricing_mismatches[:20],
        "source_local_skeletons": dict(source_skeletons.most_common()),
        "target_local_skeletons": dict(target_skeletons.most_common()),
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if not pricing_mismatches else 2


if __name__ == "__main__":
    raise SystemExit(main())
