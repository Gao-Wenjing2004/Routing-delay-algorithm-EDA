#!/usr/bin/env python3
"""Architecture-only PRP-R quotient graph and primitive discovery utilities.

This module deliberately never reads Golden delay answers.  It builds the
periodic graph from SRB_Port/Arc/Net and derives a bounded primitive library
from that graph.  Absolute SRB coordinates are not part of a routing state.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass
import csv
import json
import math
from pathlib import Path
from typing import Iterable


DIRECTION_BINS = 16


@dataclass(frozen=True)
class Edge:
    from_state: int
    to_state: int
    dx: int
    dy: int
    cost: int
    from_input_port: int
    via_output_port: int


@dataclass(frozen=True)
class ClassEdge:
    from_class: int
    to_class: int
    dx: int
    dy: int
    cost: int
    representative_from_state: int
    representative_to_state: int


@dataclass(frozen=True)
class Primitive:
    primitive_id: int
    entry_class: int
    exit_class: int
    dx: int
    dy: int
    total_delay: int
    number_of_edges: int
    direction_bin: int
    path_classes: tuple[int, ...]

    @property
    def displacement(self) -> float:
        return math.hypot(self.dx, self.dy)

    @property
    def delay_per_unit(self) -> float:
        return self.total_delay / max(self.displacement, 1.0)


@dataclass
class Architecture:
    port_names: list[str]
    port_directions: list[str]
    port_to_input: list[int]
    input_ports: list[int]
    input_to_state: list[int]
    state_to_input: list[int]
    state_incoming_dx: list[int]
    state_incoming_dy: list[int]
    state_incoming_output: list[int]
    edges: list[Edge]
    direct_arcs: list[list[tuple[int, int]]]
    output_nets: list[tuple[int, int, int]]
    target_arcs: list[list[tuple[int, int]]]
    approximate_classes: list[int]
    exact_classes: list[int]
    scc_ids: list[int]
    gaps: list[dict[str, object]]
    blocks: list[dict[str, object]]
    width: int
    height: int

    @property
    def state_count(self) -> int:
        return len(self.state_to_input)

    @property
    def approximate_class_count(self) -> int:
        return max(self.approximate_classes, default=-1) + 1

    @property
    def exact_class_count(self) -> int:
        return max(self.exact_classes, default=-1) + 1


def load_json(path: Path) -> object:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def direction_name(dx: int, dy: int) -> str:
    if dx == 0 and dy == 0:
        return "stationary"
    vertical = "N" if dy > 0 else "S" if dy < 0 else ""
    horizontal = "E" if dx > 0 else "W" if dx < 0 else ""
    return vertical + horizontal if vertical else horizontal


def direction_bin(dx: int, dy: int, bins: int = DIRECTION_BINS) -> int:
    if dx == 0 and dy == 0:
        return 0
    angle = math.atan2(dy, dx)
    return int(math.floor((angle + math.pi) * bins / (2.0 * math.pi) + 0.5)) % bins


def _tarjan_scc(state_count: int, edges: Iterable[Edge]) -> list[int]:
    adjacency: list[list[int]] = [[] for _ in range(state_count)]
    for edge in edges:
        adjacency[edge.from_state].append(edge.to_state)

    index = 0
    stack: list[int] = []
    on_stack = [False] * state_count
    indices = [-1] * state_count
    low = [0] * state_count
    component = [-1] * state_count
    component_count = 0

    def visit(node: int) -> None:
        nonlocal index, component_count
        indices[node] = low[node] = index
        index += 1
        stack.append(node)
        on_stack[node] = True
        for nxt in adjacency[node]:
            if indices[nxt] < 0:
                visit(nxt)
                low[node] = min(low[node], low[nxt])
            elif on_stack[nxt]:
                low[node] = min(low[node], indices[nxt])
        if low[node] != indices[node]:
            return
        while True:
            member = stack.pop()
            on_stack[member] = False
            component[member] = component_count
            if member == node:
                break
        component_count += 1

    for state in range(state_count):
        if indices[state] < 0:
            visit(state)
    return component


def _refine_equivalence(
    state_count: int,
    edges: list[Edge],
    initial_colors: list[int],
) -> list[int]:
    outgoing: list[list[Edge]] = [[] for _ in range(state_count)]
    for edge in edges:
        outgoing[edge.from_state].append(edge)
    colors = list(initial_colors)
    for _ in range(state_count):
        signatures = []
        for state in range(state_count):
            signature = (
                colors[state],
                tuple(
                    sorted(
                        (edge.dx, edge.dy, edge.cost, colors[edge.to_state])
                        for edge in outgoing[state]
                    )
                ),
            )
            signatures.append(signature)
        unique = {signature: cid for cid, signature in enumerate(sorted(set(signatures)))}
        refined = [unique[signature] for signature in signatures]
        if refined == colors:
            break
        colors = refined
    return colors


def load_architecture(arch_dir: Path) -> Architecture:
    port_root = load_json(arch_dir / "SRB_Port.json")
    arc_root = load_json(arch_dir / "SRB_Arc.json")
    net_root = load_json(arch_dir / "SRB_Net.json")
    gap_root = load_json(arch_dir / "SRB_Gap.json")
    inst_root = load_json(arch_dir / "SRB_Inst.json")
    assert isinstance(port_root, dict) and isinstance(arc_root, dict)
    assert isinstance(net_root, dict) and isinstance(gap_root, dict)
    assert isinstance(inst_root, dict)

    port_rows = port_root.get("Port", port_root.get("Ports"))
    if not isinstance(port_rows, list):
        raise ValueError("SRB_Port.json does not contain Port/Ports")
    port_names = [str(row.get("Name", row.get("name"))) for row in port_rows]
    port_directions = [str(row.get("Direction", row.get("direction"))).lower() for row in port_rows]
    port_id = {name: index for index, name in enumerate(port_names)}
    if len(port_id) != len(port_names):
        raise ValueError("duplicate port names")

    input_ports = [pid for pid, direction in enumerate(port_directions) if direction == "input"]
    port_to_input = [-1] * len(port_names)
    for iid, pid in enumerate(input_ports):
        port_to_input[pid] = iid

    net_rows = net_root.get("Nets")
    if not isinstance(net_rows, list):
        raise ValueError("SRB_Net.json does not contain Nets")
    input_to_state = [-1] * len(input_ports)
    state_to_input: list[int] = []
    state_incoming_dx: list[int] = []
    state_incoming_dy: list[int] = []
    state_incoming_output: list[int] = []
    output_nets = [(-1, 0, 0) for _ in port_names]
    for net in net_rows:
        output_pid = port_id[str(net["from"])]
        input_pid = port_id[str(net["to"])]
        iid = port_to_input[input_pid]
        if iid < 0:
            raise ValueError(f"Net target is not an Input: {net['to']}")
        if input_to_state[iid] < 0:
            state = len(state_to_input)
            input_to_state[iid] = state
            state_to_input.append(iid)
            state_incoming_dx.append(int(net["delta x"]))
            state_incoming_dy.append(int(net["delta y"]))
            state_incoming_output.append(output_pid)
        state = input_to_state[iid]
        if output_nets[output_pid][0] >= 0:
            raise ValueError(f"duplicate Net source: {net['from']}")
        output_nets[output_pid] = (state, int(net["delta x"]), int(net["delta y"]))

    arc_rows = arc_root.get("Arcs")
    if not isinstance(arc_rows, list):
        raise ValueError("SRB_Arc.json does not contain Arcs")
    arc_map: dict[tuple[int, int], int] = {}
    for arc in arc_rows:
        from_pid = port_id[str(arc["from"])]
        to_pid = port_id[str(arc["to"])]
        if port_to_input[from_pid] < 0 or port_directions[to_pid] != "output":
            raise ValueError(f"expected Input -> Output Arc: {arc['from']} -> {arc['to']}")
        key = (from_pid, to_pid)
        delay = int(arc["delay"])
        arc_map[key] = min(arc_map.get(key, 1 << 30), delay)

    direct_arcs: list[list[tuple[int, int]]] = [[] for _ in input_ports]
    target_arcs: list[list[tuple[int, int]]] = [[] for _ in port_names]
    edges: list[Edge] = []
    for (from_pid, output_pid), cost in sorted(arc_map.items()):
        iid = port_to_input[from_pid]
        direct_arcs[iid].append((output_pid, cost))
        next_state, dx, dy = output_nets[output_pid]
        if next_state >= 0:
            from_state = input_to_state[iid]
            if from_state >= 0:
                edges.append(
                    Edge(from_state, next_state, dx, dy, cost, from_pid, output_pid)
                )
            target_arcs[output_pid].append((input_to_state[iid], cost))

    for rows in direct_arcs:
        rows.sort()
    for rows in target_arcs:
        rows[:] = sorted((state, cost) for state, cost in rows if state >= 0)
    edges.sort(
        key=lambda edge: (
            edge.from_state,
            edge.to_state,
            edge.dx,
            edge.dy,
            edge.cost,
            edge.from_input_port,
            edge.via_output_port,
        )
    )

    incoming_vectors = sorted(set(zip(state_incoming_dx, state_incoming_dy)))
    vector_class = {vector: cid for cid, vector in enumerate(incoming_vectors)}
    approximate_classes = [
        vector_class[(dx, dy)] for dx, dy in zip(state_incoming_dx, state_incoming_dy)
    ]
    exact_classes = _refine_equivalence(len(state_to_input), edges, approximate_classes)
    scc_ids = _tarjan_scc(len(state_to_input), edges)

    gap = gap_root.get("Gap", {})
    gaps = list(gap.get("Line", [])) if isinstance(gap, dict) else []
    blocks = list(gap.get("Block", [])) if isinstance(gap, dict) else []
    instances = inst_root.get("Inst", [])
    if not isinstance(instances, list) or not instances:
        raise ValueError("SRB_Inst.json contains no instances")
    width = max(int(row["x"]) for row in instances) + 1
    height = max(int(row["y"]) for row in instances) + 1

    return Architecture(
        port_names=port_names,
        port_directions=port_directions,
        port_to_input=port_to_input,
        input_ports=input_ports,
        input_to_state=input_to_state,
        state_to_input=state_to_input,
        state_incoming_dx=state_incoming_dx,
        state_incoming_dy=state_incoming_dy,
        state_incoming_output=state_incoming_output,
        edges=edges,
        direct_arcs=direct_arcs,
        output_nets=output_nets,
        target_arcs=target_arcs,
        approximate_classes=approximate_classes,
        exact_classes=exact_classes,
        scc_ids=scc_ids,
        gaps=gaps,
        blocks=blocks,
        width=width,
        height=height,
    )


def build_class_edges(arch: Architecture) -> list[ClassEdge]:
    best: dict[tuple[int, int, int, int], Edge] = {}
    for edge in arch.edges:
        from_class = arch.approximate_classes[edge.from_state]
        to_class = arch.approximate_classes[edge.to_state]
        key = (from_class, to_class, edge.dx, edge.dy)
        if key not in best or edge.cost < best[key].cost:
            best[key] = edge
    rows = []
    for (from_class, to_class, dx, dy), edge in sorted(best.items()):
        rows.append(
            ClassEdge(
                from_class,
                to_class,
                dx,
                dy,
                edge.cost,
                edge.from_state,
                edge.to_state,
            )
        )
    return rows


def discover_class_connectors(
    class_edges: list[ClassEdge],
    class_count: int,
    max_depth: int = 4,
) -> list[list[dict[str, int]]]:
    """Find one bounded, low-detour connector for every ordered class pair.

    Connector selection is architecture-only.  The score adds a conservative
    displacement opportunity cost so that a very cheap edge which travels far
    away is not preferred over a compact class change.
    """

    outgoing: list[list[ClassEdge]] = [[] for _ in range(class_count)]
    for edge in class_edges:
        outgoing[edge.from_class].append(edge)
    result: list[list[dict[str, int]]] = [
        [
            {"dx": 0, "dy": 0, "cost": 0, "edges": 0}
            if source == target
            else {"dx": 0, "dy": 0, "cost": 1 << 30, "edges": 0}
            for target in range(class_count)
        ]
        for source in range(class_count)
    ]
    for source in range(class_count):
        frontier: dict[tuple[int, int, int], tuple[int, int]] = {
            (source, 0, 0): (0, 0)
        }
        candidates: list[list[tuple[int, int, int, int]]] = [
            [] for _ in range(class_count)
        ]
        candidates[source].append((0, 0, 0, 0))
        for depth in range(1, max_depth + 1):
            next_frontier: dict[tuple[int, int, int], tuple[int, int]] = {}
            for (current, dx, dy), (cost, _) in frontier.items():
                for edge in outgoing[current]:
                    ndx, ndy = dx + edge.dx, dy + edge.dy
                    ncost = cost + edge.cost
                    key = (edge.to_class, ndx, ndy)
                    old = next_frontier.get(key)
                    if old is None or ncost < old[0]:
                        next_frontier[key] = (ncost, depth)
            frontier = next_frontier
            for (target, dx, dy), (cost, _) in frontier.items():
                candidates[target].append((cost, dx, dy, depth))
        for target in range(class_count):
            if not candidates[target]:
                continue
            cost, dx, dy, depth = min(
                candidates[target],
                key=lambda row: (
                    row[0] + 11.0 * math.hypot(row[1], row[2]),
                    row[3],
                    row[0],
                ),
            )
            result[source][target] = {
                "dx": dx,
                "dy": dy,
                "cost": cost,
                "edges": depth,
            }
    return result


def _prune_frontier(
    frontier: dict[tuple[int, int, int], tuple[int, tuple[int, ...]]],
    layer_cap: int,
) -> dict[tuple[int, int, int], tuple[int, tuple[int, ...]]]:
    if len(frontier) <= layer_cap:
        return frontier
    groups: dict[tuple[int, int, int], list[tuple[tuple[int, int, int], tuple[int, tuple[int, ...]]]]] = {}
    for key, value in frontier.items():
        state, dx, dy = key
        magnitude_bin = min(11, int(math.hypot(dx, dy)) // 8)
        group = (state, direction_bin(dx, dy), magnitude_bin)
        groups.setdefault(group, []).append((key, value))
    selected: dict[tuple[int, int, int], tuple[int, tuple[int, ...]]] = {}
    per_group = max(1, layer_cap // max(1, len(groups)))
    for rows in groups.values():
        rows.sort(
            key=lambda item: (
                item[1][0] / max(math.hypot(item[0][1], item[0][2]), 1.0),
                item[1][0],
                -abs(item[0][1]) - abs(item[0][2]),
            )
        )
        for key, value in rows[:per_group]:
            selected[key] = value
    if len(selected) > layer_cap:
        ranked = sorted(
            selected.items(),
            key=lambda item: (
                item[1][0] / max(math.hypot(item[0][1], item[0][2]), 1.0),
                item[1][0],
            ),
        )
        return dict(ranked[:layer_cap])
    return selected


def discover_primitives(
    arch: Architecture,
    max_depth: int = 16,
    max_displacement: int = 128,
    layer_cap: int = 24000,
    global_limit: int = 512,
    per_direction_limit: int = 32,
) -> tuple[list[Primitive], list[ClassEdge]]:
    class_edges = build_class_edges(arch)
    outgoing: list[list[tuple[int, ClassEdge]]] = [
        [] for _ in range(arch.approximate_class_count)
    ]
    for edge_id, edge in enumerate(class_edges):
        outgoing[edge.from_class].append((edge_id, edge))

    candidates: dict[tuple[int, int, int], tuple[int, int, tuple[int, ...]]] = {}
    for start_class in range(arch.approximate_class_count):
        frontier: dict[tuple[int, int, int], tuple[int, tuple[int, ...]]] = {
            (start_class, 0, 0): (0, (start_class,))
        }
        for depth in range(1, max_depth + 1):
            next_frontier: dict[tuple[int, int, int], tuple[int, tuple[int, ...]]] = {}
            for (current_class, dx, dy), (cost, path) in frontier.items():
                for _, edge in outgoing[current_class]:
                    ndx = dx + edge.dx
                    ndy = dy + edge.dy
                    if abs(ndx) > max_displacement or abs(ndy) > max_displacement:
                        continue
                    ncost = cost + edge.cost
                    key = (edge.to_class, ndx, ndy)
                    previous = next_frontier.get(key)
                    npath = path + (edge.to_class,)
                    if previous is None or (ncost, len(npath)) < (previous[0], len(previous[1])):
                        next_frontier[key] = (ncost, npath)
            frontier = _prune_frontier(next_frontier, layer_cap)
            for (end_class, dx, dy), (cost, path) in frontier.items():
                if end_class != start_class or (dx == 0 and dy == 0):
                    continue
                key = (start_class, dx, dy)
                previous = candidates.get(key)
                value = (cost, depth, path)
                if previous is None or (cost, depth) < (previous[0], previous[1]):
                    candidates[key] = value

    by_direction: dict[int, list[tuple[tuple[int, int, int], tuple[int, int, tuple[int, ...]]]]] = {
        direction: [] for direction in range(DIRECTION_BINS)
    }
    for key, value in candidates.items():
        by_direction[direction_bin(key[1], key[2])].append((key, value))

    chosen: list[tuple[tuple[int, int, int], tuple[int, int, tuple[int, ...]]]] = []
    chosen_keys: set[tuple[int, int, int]] = set()
    for direction in range(DIRECTION_BINS):
        rows = by_direction[direction]
        # Unit-cost-only ranking strongly favours very long cycles.  Keep a
        # Pareto-like mix of short, medium and long cycles so that the runtime
        # solver can also represent short displacements without an Atlas.
        scale_limits = (16.0, 32.0, 64.0, 96.0, math.inf)
        scale_rows: list[list[tuple[tuple[int, int, int], tuple[int, int, tuple[int, ...]]]]] = [
            [] for _ in scale_limits
        ]
        for item in rows:
            magnitude = math.hypot(item[0][1], item[0][2])
            bucket = next(i for i, limit in enumerate(scale_limits) if magnitude <= limit)
            scale_rows[bucket].append(item)
        for bucket_rows in scale_rows:
            bucket_rows.sort(
                key=lambda item: (
                    item[1][0] / max(math.hypot(item[0][1], item[0][2]), 1.0),
                    item[1][0],
                    item[1][1],
                    item[0],
                )
            )
        vector_seen: set[tuple[int, int]] = set()
        class_counts: dict[int, int] = {}
        direction_selected = 0
        per_scale_limit = max(1, per_direction_limit // len(scale_limits))

        def take(bucket_rows, limit):
            nonlocal direction_selected
            taken = 0
            for key, value in bucket_rows:
                if direction_selected >= per_direction_limit or taken >= limit:
                    break
                vector = (key[1], key[2])
                if key in chosen_keys:
                    continue
                if vector in vector_seen and class_counts.get(key[0], 0) >= 2:
                    continue
                chosen.append((key, value))
                chosen_keys.add(key)
                vector_seen.add(vector)
                class_counts[key[0]] = class_counts.get(key[0], 0) + 1
                direction_selected += 1
                taken += 1

        for bucket_rows in scale_rows:
            take(bucket_rows, per_scale_limit)
        ranked = sorted(
            rows,
            key=lambda item: (
                item[1][0] / max(math.hypot(item[0][1], item[0][2]), 1.0),
                item[1][0],
                item[1][1],
            ),
        )
        take(ranked, per_direction_limit - direction_selected)

    if len(chosen) < global_limit:
        remaining = [
            (key, value)
            for key, value in candidates.items()
            if key not in chosen_keys
        ]
        remaining.sort(
            key=lambda item: (
                item[1][0] / max(math.hypot(item[0][1], item[0][2]), 1.0),
                -math.hypot(item[0][1], item[0][2]),
            )
        )
        chosen.extend(remaining[: max(0, global_limit - len(chosen))])
    chosen = chosen[:global_limit]

    primitives = []
    for primitive_id, (key, value) in enumerate(
        sorted(
            chosen,
            key=lambda item: (
                direction_bin(item[0][1], item[0][2]),
                item[1][0] / max(math.hypot(item[0][1], item[0][2]), 1.0),
                item[0],
            ),
        )
    ):
        start_class, dx, dy = key
        cost, depth, path = value
        primitives.append(
            Primitive(
                primitive_id,
                start_class,
                start_class,
                dx,
                dy,
                cost,
                depth,
                direction_bin(dx, dy),
                path,
            )
        )
    return primitives, class_edges


def select_basis(primitives: list[Primitive]) -> list[Primitive]:
    basis: list[Primitive] = []
    for wanted in range(DIRECTION_BINS):
        rows = [primitive for primitive in primitives if primitive.direction_bin == wanted]
        if not rows:
            continue
        rows.sort(
            key=lambda primitive: (
                primitive.delay_per_unit,
                -primitive.displacement,
                primitive.number_of_edges,
            )
        )
        basis.append(rows[0])
    return basis


def select_runtime_primitives(
    primitives: list[Primitive], target_scales: tuple[int, ...] = (12, 24, 48, 80, 120)
) -> list[Primitive]:
    """Select a deterministic multi-scale runtime library.

    One primitive per direction and target scale keeps the online candidate
    count fixed while preserving the scale diversity found offline.
    """

    selected: list[Primitive] = []
    selected_ids: set[int] = set()
    for wanted in range(DIRECTION_BINS):
        rows = [primitive for primitive in primitives if primitive.direction_bin == wanted]
        for scale in target_scales:
            candidates = [row for row in rows if row.primitive_id not in selected_ids]
            if not candidates:
                break
            best = min(
                candidates,
                key=lambda primitive: (
                    abs(primitive.displacement - scale),
                    primitive.delay_per_unit,
                    primitive.number_of_edges,
                    primitive.primitive_id,
                ),
            )
            selected.append(best)
            selected_ids.add(best.primitive_id)
    return sorted(selected, key=lambda primitive: (primitive.direction_bin, primitive.displacement))


def write_csv(path: Path, rows: Iterable[dict[str, object]], fieldnames: list[str]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def primitive_rows(primitives: list[Primitive]) -> list[dict[str, object]]:
    rows = []
    for primitive in primitives:
        rows.append(
            {
                "primitive_id": primitive.primitive_id,
                "entry_state_class": primitive.entry_class,
                "exit_state_class": primitive.exit_class,
                "net_displacement_x": primitive.dx,
                "net_displacement_y": primitive.dy,
                "total_delay": primitive.total_delay,
                "number_of_edges": primitive.number_of_edges,
                "direction": direction_name(primitive.dx, primitive.dy),
                "direction_bin": primitive.direction_bin,
                "delay_per_euclidean_unit": f"{primitive.delay_per_unit:.8f}",
                "path_classes": ";".join(map(str, primitive.path_classes)),
            }
        )
    return rows
