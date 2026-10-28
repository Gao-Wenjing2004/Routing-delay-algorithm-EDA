#!/usr/bin/env python3
"""Reprice the known Dijkstra H/V skeleton with exact 160-state axis tables.

Unlike analyze_axis_oracle.py, this experiment is given the correct sequence of
axis runs extracted from the Dijkstra path.  A mismatch therefore points to the
periodic primitive pricing/state model (or omitted absolute constraints), not to
the skeleton selector.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from pathlib import Path

from analyze_axis_oracle import (
    INF,
    STATE_COUNT,
    build_axis_table,
    endpoint,
    source_candidates,
    target_candidates,
)


GAP_EVENT = re.compile(r"G(\d+)@")


def primitive_vectors(sequence: str) -> list[tuple[str, int]]:
    result: list[tuple[str, int]] = []
    for item in sequence.split("|") if sequence else []:
        head = item.split("@", 1)[0]
        axis, delta = head.split(":", 1)
        result.append((axis, int(delta)))
    return result


def runs(vectors: list[tuple[str, int]]) -> list[tuple[str, int]]:
    result: list[tuple[str, int]] = []
    for axis, delta in vectors:
        if result and result[-1][0] == axis:
            result[-1] = (axis, result[-1][1] + delta)
        else:
            result.append((axis, delta))
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--transitions", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--output-radius", type=int, default=32)
    parser.add_argument("--search-radius", type=int, default=64)
    parser.add_argument("--beam", type=int, default=160)
    args = parser.parse_args()

    try:
        import numpy as np
    except ModuleNotFoundError:
        parser.error(
            "this Oracle experiment requires NumPy; install it in the Python "
            "environment used to run the script"
        )

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    gap_root = json.loads(args.gap.read_text(encoding="utf-8-sig"))["Gap"]
    gap_delays = [int(line["delay"]) for line in gap_root["Line"]]
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    input_name_by_state = [
        model["port_names"][model["input_ports"][input_id]]
        for input_id in model["state_to_input"]
    ]
    horizontal_raw = build_axis_table(
        args.transitions, "H", args.output_radius, args.search_radius)
    vertical_raw = build_axis_table(
        args.transitions, "V", args.output_radius, args.search_radius)
    width = 2 * args.output_radius + 1
    horizontal = np.frombuffer(horizontal_raw, dtype=np.uint16).reshape(
        STATE_COUNT, width, STATE_COUNT)
    vertical = np.frombuffer(vertical_raw, dtype=np.uint16).reshape(
        STATE_COUNT, width, STATE_COUNT)

    total = actual_no_block = candidate_rows = exact_matches = 0
    score_sum = 0.0
    absolute_sum = 0
    under = over = 0
    no_source = no_target = out_of_radius = 0
    candidate_gap_rows = candidate_clear_rows = 0
    by_turns: dict[int, dict[str, int]] = {}
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if row["Reachable"] != "1":
                continue
            total += 1
            if int(row["BlockSegments"]):
                continue
            actual_no_block += 1
            _, _, source_name = endpoint(row["From"])
            _, _, target_name = endpoint(row["To"])
            sources = source_candidates(model, port_id[source_name])
            targets = target_candidates(model, port_id[target_name])
            if not sources:
                no_source += 1
                continue
            if not targets:
                no_target += 1
                continue
            vectors = primitive_vectors(row["PrimitiveSequence"])
            route_sequence = row["RoutingStateSequence"].split(">") \
                if row["RoutingStateSequence"] else []
            best = 10**9
            had_radius_candidate = False
            for source_state, entry_dx, entry_dy, entry_cost in sources:
                remaining = vectors
                if entry_dx or entry_dy:
                    expected = ("H", entry_dx) if entry_dx else ("V", entry_dy)
                    if not remaining or remaining[0] != expected:
                        continue
                    if route_sequence and input_name_by_state[source_state] != route_sequence[0]:
                        continue
                    remaining = remaining[1:]
                path_runs = runs(remaining)
                if any(abs(delta) > args.output_radius for _, delta in path_runs):
                    continue
                had_radius_candidate = True
                dp = np.full(STATE_COUNT, 10**9, dtype=np.uint32)
                dp[source_state] = entry_cost
                for run_index, (axis, delta) in enumerate(path_runs):
                    matrix = horizontal[:, delta + args.output_radius, :] \
                        if axis == "H" else vertical[:, delta + args.output_radius, :]
                    candidate = dp[:, None] + matrix.astype(np.uint32)
                    candidate[matrix == INF] = 10**9
                    dp = candidate.min(axis=0)
                    if args.beam < STATE_COUNT and run_index + 1 < len(path_runs):
                        keep = np.argpartition(dp, args.beam)[:args.beam]
                        pruned = np.full(STATE_COUNT, 10**9, dtype=np.uint32)
                        pruned[keep] = dp[keep]
                        dp = pruned
                for target_state, exit_cost in targets:
                    value = int(dp[target_state]) + exit_cost
                    if value < best:
                        best = value
            if not had_radius_candidate:
                out_of_radius += 1
                continue
            if best >= 10**9:
                continue
            # The axis tables intentionally contain only periodic Arc costs.
            # Charge each Gap event on the known path separately; repeated
            # crossings remain repeated, matching the graph semantics.
            gap_extra = sum(
                gap_delays[int(index)] for index in GAP_EVENT.findall(row["PortalSignature"])
            )
            best += gap_extra
            candidate_rows += 1
            if int(row["GapCrossings"]):
                candidate_gap_rows += 1
            else:
                candidate_clear_rows += 1
            golden = int(row["Delay"])
            error = abs(best - golden)
            exact_matches += best == golden
            under += best < golden
            over += best > golden
            absolute_sum += error
            score_sum += 1.0 - math.tanh(4.0 * error / golden) if golden else float(best == 0)
            turn = int(row["Turns"])
            group = by_turns.setdefault(turn, {"rows": 0, "matches": 0})
            group["rows"] += 1
            group["matches"] += best == golden

    report = {
        "definition": "known Dijkstra turn skeleton repriced by exact 160-state axis connectors",
        "route_state_beam": args.beam,
        "all_rows": total,
        "actual_no_block_rows": actual_no_block,
        "candidate_rows": candidate_rows,
        "candidate_coverage_actual_no_block": candidate_rows / max(actual_no_block, 1),
        "candidate_clear_rows": candidate_clear_rows,
        "candidate_gap_rows": candidate_gap_rows,
        "exact_matches": exact_matches,
        "exact_match_rate": exact_matches / max(candidate_rows, 1),
        "accuracy": 100.0 * score_sum / max(candidate_rows, 1),
        "mae": absolute_sum / max(candidate_rows, 1),
        "underestimates": under,
        "overestimates": over,
        "no_source_candidates": no_source,
        "no_target_candidates": no_target,
        "out_of_axis_radius": out_of_radius,
        "by_turns": {
            str(turn): {**values, "exact_rate": values["matches"] / values["rows"]}
            for turn, values in sorted(by_turns.items())
        },
        "table_bytes_uint16": 2 * (len(horizontal_raw) + len(vertical_raw)),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
