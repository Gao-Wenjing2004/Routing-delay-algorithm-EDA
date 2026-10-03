#!/usr/bin/env python3
"""Evaluate an exact-state H/V periodic-primitive oracle on Dijkstra paths.

This is a P1 research oracle, not a submission estimator.  It keeps all 160
Routing Input states and computes shortest one-axis connectors from official
Arc+Net transitions.  Online-like evaluation compares four fixed skeletons:
H, V, H->V and V->H.  Rows whose exact path touches a Gap or Block are excluded
because those costs belong to later path-specific obstacle handling.
"""

from __future__ import annotations

import argparse
import csv
import heapq
import json
import math
import re
from array import array
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
INF = 65535
STATE_COUNT = 160


def endpoint(value: str) -> tuple[int, int, str]:
    match = ENDPOINT.match(value)
    if match is None:
        raise ValueError(f"bad endpoint: {value}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def build_axis_table(
    transitions: Path, axis: str, output_radius: int, search_radius: int,
) -> array:
    adjacency: list[list[tuple[int, int, int]]] = [[] for _ in range(STATE_COUNT)]
    with transitions.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            dx, dy = int(row["delta_x"]), int(row["delta_y"])
            if axis == "H" and (dy != 0 or dx == 0):
                continue
            if axis == "V" and (dx != 0 or dy == 0):
                continue
            delta = dx if axis == "H" else dy
            adjacency[int(row["from_state"])].append(
                (int(row["to_state"]), delta, int(row["total_delay"]))
            )

    width = search_radius * 2 + 1
    output_width = output_radius * 2 + 1
    table = array("H", [INF]) * (STATE_COUNT * output_width * STATE_COUNT)
    for source in range(STATE_COUNT):
        distances = [10**9] * (width * STATE_COUNT)
        origin = search_radius * STATE_COUNT + source
        distances[origin] = 0
        heap: list[tuple[int, int, int]] = [(0, 0, source)]
        while heap:
            cost, offset, state = heapq.heappop(heap)
            index = (offset + search_radius) * STATE_COUNT + state
            if cost != distances[index]:
                continue
            for target, delta, edge_cost in adjacency[state]:
                next_offset = offset + delta
                if next_offset < -search_radius or next_offset > search_radius:
                    continue
                next_index = (next_offset + search_radius) * STATE_COUNT + target
                next_cost = cost + edge_cost
                if next_cost >= distances[next_index]:
                    continue
                distances[next_index] = next_cost
                heapq.heappush(heap, (next_cost, next_offset, target))
        for delta in range(-output_radius, output_radius + 1):
            source_base = (delta + search_radius) * STATE_COUNT
            table_base = (source * output_width + delta + output_radius) * STATE_COUNT
            for target in range(STATE_COUNT):
                value = distances[source_base + target]
                if value < INF:
                    table[table_base + target] = value
    return table


def table_cost(table: array, radius: int, source: int, target: int, delta: int) -> int:
    if delta < -radius or delta > radius:
        return INF
    return table[(source * (2 * radius + 1) + delta + radius) * STATE_COUNT + target]


def source_candidates(model: dict[str, object], port: int) -> list[tuple[int, int, int, int]]:
    port_to_input = model["port_to_input"]
    input_to_state = model["input_to_state"]
    iid = int(port_to_input[port])
    if iid >= 0:
        route = int(input_to_state[iid])
        if route >= 0:
            return [(route, 0, 0, 0)]
        result = []
        for output, arc_cost in model["direct_arcs"][iid]:
            route, dx, dy = model["output_nets"][int(output)]
            if int(route) >= 0:
                result.append((int(route), int(dx), int(dy), int(arc_cost)))
        return result
    route, dx, dy = model["output_nets"][port]
    return [(int(route), int(dx), int(dy), 0)] if int(route) >= 0 else []


def target_candidates(model: dict[str, object], port: int) -> list[tuple[int, int]]:
    iid = int(model["port_to_input"][port])
    if iid >= 0:
        route = int(model["input_to_state"][iid])
        return [(route, 0)] if route >= 0 else []
    return [(int(route), int(cost)) for route, cost in model["target_arcs"][port]]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--transitions", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--output-radius", type=int, default=32)
    parser.add_argument("--search-radius", type=int, default=64)
    args = parser.parse_args()
    if args.search_radius < args.output_radius:
        raise ValueError("search radius must cover output radius")

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    horizontal = build_axis_table(
        args.transitions, "H", args.output_radius, args.search_radius)
    vertical = build_axis_table(
        args.transitions, "V", args.output_radius, args.search_radius)

    rows = matched = candidate_rows = 0
    score_sum = 0.0
    absolute_sum = 0
    skeleton_counts = {"H": 0, "V": 0, "HV": 0, "VH": 0}
    excluded_obstacle = 0
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if row["Reachable"] != "1":
                continue
            rows += 1
            if int(row["GapCrossings"]) or int(row["BlockSegments"]):
                excluded_obstacle += 1
                continue
            sx, sy, source_name = endpoint(row["From"])
            tx, ty, target_name = endpoint(row["To"])
            best = INF
            best_skeleton = ""
            for source_state, entry_dx, entry_dy, entry_cost in source_candidates(
                    model, port_id[source_name]):
                dx = tx - sx - entry_dx
                dy = ty - sy - entry_dy
                for target_state, exit_cost in target_candidates(model, port_id[target_name]):
                    base = entry_cost + exit_cost
                    if dy == 0:
                        cost = table_cost(horizontal, args.output_radius,
                                          source_state, target_state, dx)
                        if cost != INF and base + cost < best:
                            best, best_skeleton = base + cost, "H"
                    if dx == 0:
                        cost = table_cost(vertical, args.output_radius,
                                          source_state, target_state, dy)
                        if cost != INF and base + cost < best:
                            best, best_skeleton = base + cost, "V"
                    for middle in range(STATE_COUNT):
                        first = table_cost(horizontal, args.output_radius,
                                           source_state, middle, dx)
                        second = table_cost(vertical, args.output_radius,
                                            middle, target_state, dy)
                        if first != INF and second != INF and base + first + second < best:
                            best, best_skeleton = base + first + second, "HV"
                        first = table_cost(vertical, args.output_radius,
                                           source_state, middle, dy)
                        second = table_cost(horizontal, args.output_radius,
                                            middle, target_state, dx)
                        if first != INF and second != INF and base + first + second < best:
                            best, best_skeleton = base + first + second, "VH"
            if best == INF:
                continue
            candidate_rows += 1
            golden = int(row["Delay"])
            error = abs(best - golden)
            score_sum += 1.0 - math.tanh(4.0 * error / golden) if golden else float(best == 0)
            absolute_sum += error
            if best == golden:
                matched += 1
            skeleton_counts[best_skeleton] += 1

    report = {
        "definition": "exact 160-state axis connector; fixed H,V,HV,VH skeleton oracle",
        "axis_output_radius": args.output_radius,
        "axis_search_radius": args.search_radius,
        "all_summary_rows": rows,
        "excluded_actual_gap_or_block_rows": excluded_obstacle,
        "actual_clear_rows": rows - excluded_obstacle,
        "candidate_rows": candidate_rows,
        "candidate_coverage_actual_clear": candidate_rows / max(rows - excluded_obstacle, 1),
        "exact_matches": matched,
        "exact_match_rate": matched / max(candidate_rows, 1),
        "accuracy": 100.0 * score_sum / max(candidate_rows, 1),
        "mae": absolute_sum / max(candidate_rows, 1),
        "selected_skeletons": skeleton_counts,
        "table_bytes_uint16": 2 * (len(horizontal) + len(vertical)),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
