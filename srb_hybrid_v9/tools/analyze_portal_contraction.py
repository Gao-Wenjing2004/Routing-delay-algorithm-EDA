#!/usr/bin/env python3
"""Audit safe MacroEdge contraction and construct deterministic Portal candidates."""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from pathlib import Path


def load(path: Path, key: str) -> object:
    return json.loads(path.read_text(encoding="utf-8-sig"))[key]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--portals", type=Path, required=True)
    args = parser.parse_args()

    instances = load(args.arch_dir / "SRB_Inst.json", "Inst")
    ports = load(args.arch_dir / "SRB_Port.json", "Port")
    arcs = load(args.arch_dir / "SRB_Arc.json", "Arcs")
    nets = load(args.arch_dir / "SRB_Net.json", "Nets")
    gap = load(args.arch_dir / "SRB_Gap.json", "Gap")
    assert isinstance(instances, list) and isinstance(ports, list)
    assert isinstance(arcs, list) and isinstance(nets, list) and isinstance(gap, dict)

    coordinates = {(int(row["x"]), int(row["y"])) for row in instances}
    width = max(x for x, _ in coordinates) + 1
    height = max(y for _, y in coordinates) + 1
    net_by_output = {str(row["from"]): row for row in nets}
    routing_inputs = sorted({str(row["to"]) for row in nets})
    routing_set = set(routing_inputs)
    input_net = {str(row["to"]): row for row in nets}

    transitions: dict[str, list[dict[str, object]]] = defaultdict(list)
    incoming: Counter[str] = Counter()
    for arc in arcs:
        source = str(arc["from"])
        net = net_by_output.get(str(arc["to"]))
        if source not in routing_set or net is None:
            continue
        target = str(net["to"])
        transition = {
            "source": source,
            "target": target,
            "output": str(arc["to"]),
            "arc_delay": int(arc["delay"]),
            "dx": int(net["delta x"]),
            "dy": int(net["delta y"]),
        }
        transitions[source].append(transition)
        incoming[target] += 1

    outdegree = {route: len(transitions[route]) for route in routing_inputs}
    indegree = {route: incoming[route] for route in routing_inputs}
    safe_chain_routes = [
        route for route in routing_inputs if outdegree[route] == 1 and indegree[route] == 1
    ]
    duplicate_dominated = 0
    for route in routing_inputs:
        best: dict[tuple[str, int, int], int] = {}
        for edge in transitions[route]:
            key = (str(edge["target"]), int(edge["dx"]), int(edge["dy"]))
            delay = int(edge["arc_delay"])
            if key in best:
                duplicate_dominated += 1
                best[key] = min(best[key], delay)
            else:
                best[key] = delay

    turn_capable = []
    direction_sets: Counter[str] = Counter()
    for route in routing_inputs:
        entering = input_net[route]
        incoming_axis = "H" if int(entering["delta x"]) else "V"
        axes = {"H" if int(edge["dx"]) else "V" for edge in transitions[route]}
        direction_sets["".join(sorted(axes))] += 1
        if any(axis != incoming_axis for axis in axes):
            turn_capable.append(route)

    block_rows = list(gap["Block"])
    block_at: dict[tuple[int, int], int] = {}
    for index, block in enumerate(block_rows):
        for y in range(int(block["lower"]), int(block["upper"]) + 1):
            for x in range(int(block["left"]), int(block["right"]) + 1):
                block_at[(x, y)] = index

    portal_tags: dict[tuple[int, int], set[str]] = defaultdict(set)
    for x, y in coordinates:
        for side, neighbor in (
            ("left", (x + 1, y)), ("right", (x - 1, y)),
            ("bottom", (x, y + 1)), ("top", (x, y - 1)),
        ):
            block_index = block_at.get(neighbor)
            if block_index is not None:
                portal_tags[(x, y)].add(f"block:{block_index}:{side}")
        if x in {0, width - 1} or y in {0, height - 1}:
            portal_tags[(x, y)].add("device-boundary")

    block_signatures: dict[tuple[object, ...], list[int]] = defaultdict(list)
    for index, block in enumerate(block_rows):
        signature = (
            int(block["right"]) - int(block["left"]) + 1,
            int(block["upper"]) - int(block["lower"]) + 1,
            int(block["left"]), int(block["right"]),
            bool(block["vertical crossable"]), int(block["vertical cross delay"]),
            bool(block["horizontal crossable"]), int(block["horizontal cross delay"]),
        )
        block_signatures[signature].append(index)

    repeated_families = []
    for signature, members in block_signatures.items():
        if len(members) < 2:
            continue
        lowers = [int(block_rows[index]["lower"]) for index in members]
        periods = [right - left for left, right in zip(lowers, lowers[1:])]
        repeated_families.append({
            "members": members,
            "signature": list(signature),
            "lower_coordinates": lowers,
            "period_gcd": math.gcd(*periods) if periods else 0,
        })

    line_rows = list(gap["Line"])
    line_families: Counter[tuple[str, int]] = Counter(
        (str(row["direction"]), int(row["delay"])) for row in line_rows
    )

    args.portals.parent.mkdir(parents=True, exist_ok=True)
    with args.portals.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("x", "y", "tags"))
        for (x, y), tags in sorted(portal_tags.items()):
            writer.writerow((x, y, "|".join(sorted(tags))))

    block_rim_cells = sum(any(tag.startswith("block:") for tag in tags) for tags in portal_tags.values())
    boundary_cells = sum("device-boundary" in tags for tags in portal_tags.values())
    report = {
        "architecture": {
            "width": width,
            "height": height,
            "cells": len(coordinates),
            "ports": len(ports),
            "routing_states_per_cell": len(routing_inputs),
            "full_route_states": len(coordinates) * len(routing_inputs),
        },
        "macro_edge_audit": {
            "existing_arc_net_macro_edges": sum(outdegree.values()),
            "outdegree_distribution": dict(sorted(Counter(outdegree.values()).items())),
            "indegree_distribution": dict(sorted(Counter(indegree.values()).items())),
            "safe_degree_1_chain_routes": len(safe_chain_routes),
            "dominated_duplicate_edges": duplicate_dominated,
            "turn_capable_routes": len(turn_capable),
            "outgoing_axis_sets": dict(sorted(direction_sets.items())),
            "finding": (
                "The current solver already contracts Input->Arc->Output->Net. "
                "No persistent route state has both indegree=1 and outdegree=1, so naive lane-chain "
                "contraction is unsafe; the next contraction level must be precomputed periodic/portal transfer edges."
            ),
        },
        "portal_overlay": {
            "block_rim_cells": block_rim_cells,
            "device_boundary_cells": boundary_cells,
            "physical_portal_cells_union": len(portal_tags),
            "physical_portal_route_states_before_equivalence": len(portal_tags) * len(routing_inputs),
            "gap_virtual_lines": len(line_rows),
            "gap_line_families_by_direction_delay": {
                f"{direction}:{delay}": count
                for (direction, delay), count in sorted(line_families.items())
            },
            "repeated_block_families": repeated_families,
            "finding": (
                "Gap Lines are additive crossing costs, not routing choices; represent them as virtual "
                "boundary events/prefix costs instead of materializing every line cell as a Portal. "
                "Block rims and device boundaries are the physical Portal candidates."
            ),
        },
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(
        f"routes={len(routing_inputs)} macro_edges={sum(outdegree.values())} "
        f"safe_chains={len(safe_chain_routes)} block_rim_cells={block_rim_cells} "
        f"portal_union={len(portal_tags)}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
