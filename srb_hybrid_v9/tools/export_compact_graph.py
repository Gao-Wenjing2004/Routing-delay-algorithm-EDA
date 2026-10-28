#!/usr/bin/env python3
"""Export the exact SRB topology needed by tiny bounded searches."""

from __future__ import annotations

import argparse
import heapq
import json
import struct
from pathlib import Path
from typing import BinaryIO


def scalar(stream: BinaryIO, fmt: str) -> int:
    size = struct.calcsize(fmt)
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("truncated graph")
    return int(struct.unpack("<" + fmt, data)[0])


def vector(stream: BinaryIO, fmt: str) -> list[tuple[int, ...]]:
    count = scalar(stream, "Q")
    size = struct.calcsize("<" + fmt)
    data = stream.read(count * size)
    if len(data) != count * size:
        raise ValueError("truncated vector")
    return list(struct.iter_unpack("<" + fmt, data))


def strings(stream: BinaryIO) -> list[str]:
    result = []
    for _ in range(scalar(stream, "Q")):
        size = scalar(stream, "I")
        result.append(stream.read(size).decode("utf-8"))
    return result


def values(rows: list[tuple[int, ...]]) -> list[int]:
    return [row[0] for row in rows]


def format_array(name: str, ctype: str, data: list[int], per_line: int = 24) -> str:
    lines = [f"inline constexpr {ctype} {name}[{len(data)}] = {{"]
    for start in range(0, len(data), per_line):
        lines.append("    " + ", ".join(str(value) for value in data[start : start + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


def shortest_paths(source: int, adjacency: list[list[tuple[int, int]]]) -> list[int]:
    infinity = 1 << 60
    distance = [infinity] * len(adjacency)
    distance[source] = 0
    queue = [(0, source)]
    while queue:
        current, node = heapq.heappop(queue)
        if current != distance[node]:
            continue
        for target, cost in adjacency[node]:
            candidate = current + cost
            if candidate < distance[target]:
                distance[target] = candidate
                heapq.heappush(queue, (candidate, target))
    return distance


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument(
        "--landmark-output",
        type=Path,
        help="optional uint16 ALT/Portal lower-bound header for compact A*",
    )
    args = parser.parse_args()

    with args.graph.open("rb") as stream:
        if stream.read(8) != b"SRBGRPH1":
            raise ValueError("bad graph magic")
        version = scalar(stream, "I")
        endian = scalar(stream, "I")
        if version != 1 or endian != 0x01020304:
            raise ValueError("unsupported graph")
        width = scalar(stream, "i")
        height = scalar(stream, "i")
        input_count = scalar(stream, "H")
        route_count = scalar(stream, "H")
        _arc_count = scalar(stream, "Q")
        cells = vector(stream, "hh")
        ports = strings(stream)
        port_is_input = values(vector(stream, "B"))
        port_to_input = values(vector(stream, "h"))
        input_to_route = values(vector(stream, "h"))
        route_to_input = values(vector(stream, "H"))
        nets = vector(stream, "HHhh")
        net_by_output = values(vector(stream, "h"))
        transition_group_count = scalar(stream, "Q")
        transitions_by_input = [vector(stream, "HHHH") for _ in range(transition_group_count)]
        arc_to_output = values(vector(stream, "H"))
        spatial_next = values(vector(stream, "i"))
        spatial_extra = values(vector(stream, "H"))

    transition_offsets = [0]
    transitions: list[tuple[int, ...]] = []
    for group in transitions_by_input:
        transitions.extend(group)
        transition_offsets.append(len(transitions))

    route_reverse: list[list[tuple[int, int]]] = [[] for _ in range(route_count)]
    for source_route, full_input in enumerate(route_to_input):
        best: dict[int, int] = {}
        for _out, destination_route, arc_delay, _net in transitions_by_input[full_input]:
            best[destination_route] = min(best.get(destination_route, 65535), arc_delay)
        for destination_route, arc_delay in best.items():
            route_reverse[destination_route].append((source_route, arc_delay))
    port_route_lower: list[int] = []
    for port_id in range(len(ports)):
        distances = [1 << 30] * route_count
        queue: list[tuple[int, int]] = []
        if port_is_input[port_id]:
            route = input_to_route[port_to_input[port_id]]
            if route >= 0:
                distances[route] = 0
                heapq.heappush(queue, (0, route))
        else:
            for route, full_input in enumerate(route_to_input):
                delay = arc_to_output[full_input * len(ports) + port_id]
                if delay != 65535:
                    distances[route] = delay
                    heapq.heappush(queue, (delay, route))
        while queue:
            distance, route = heapq.heappop(queue)
            if distance != distances[route]:
                continue
            for previous, weight in route_reverse[route]:
                candidate = distance + weight
                if candidate < distances[previous]:
                    distances[previous] = candidate
                    heapq.heappush(queue, (candidate, previous))
        port_route_lower.extend(65535 if value >= 65535 else value for value in distances)

    gap = json.loads(args.gap.read_text(encoding="utf-8"))["Gap"]
    lines = gap["Line"]
    blocks = gap["Block"]
    body = [
        "#pragma once",
        "#include <cstdint>",
        "namespace v9_compact_data {",
        f"inline constexpr int kWidth = {width};",
        f"inline constexpr int kHeight = {height};",
        f"inline constexpr uint16_t kPortCount = {len(ports)};",
        f"inline constexpr uint16_t kInputCount = {input_count};",
        f"inline constexpr uint16_t kRouteCount = {route_count};",
        f"inline constexpr uint16_t kNoU16 = 65535;",
        "struct Net { uint16_t from_output; uint16_t dst_input; int16_t dx; int16_t dy; };",
        "struct Edge { uint16_t out_port; uint16_t next_input; uint16_t arc_delay; uint16_t net_id; };",
        "struct GapLine { int16_t site; uint16_t delay; uint8_t vertical; };",
        "struct Block { int16_t lower, upper, left, right; uint16_t vertical_delay, horizontal_delay; uint8_t vertical_crossable, horizontal_crossable; };",
        format_array("kPortIsInput", "uint8_t", port_is_input),
        format_array("kPortToInput", "int16_t", port_to_input),
        format_array("kInputToRoute", "int16_t", input_to_route),
        format_array("kRouteToInput", "uint16_t", route_to_input),
        format_array("kNetByOutput", "int16_t", net_by_output),
        format_array("kTransitionOffsets", "uint16_t", transition_offsets),
        "inline constexpr Net kNets[] = {",
    ]
    body.extend(f"    {{{a}, {b}, {dx}, {dy}}}," for a, b, dx, dy in nets)
    body.append("};")
    body.append("inline constexpr Edge kTransitions[] = {")
    body.extend(f"    {{{a}, {b}, {c}, {d}}}," for a, b, c, d in transitions)
    body.append("};")
    body.append(format_array("kArcToOutput", "uint16_t", arc_to_output, per_line=20))
    body.append(format_array("kPortRouteLower", "uint16_t", port_route_lower, per_line=20))
    body.append("inline constexpr GapLine kGapLines[] = {")
    body.extend(
        f"    {{{int(row['site'])}, {int(row['delay'])}, {1 if row['direction'] == 'vertical' else 0}}},"
        for row in lines
    )
    body.append("};")
    body.append("inline constexpr Block kBlocks[] = {")
    body.extend(
        "    {%(lower)d, %(upper)d, %(left)d, %(right)d, %(vertical cross delay)d, "
        "%(horizontal cross delay)d, %(vertical crossable)d, %(horizontal crossable)d},"
        % {
            **{key: int(value) for key, value in row.items() if key != "id"},
        }
        for row in blocks
    )
    body.extend(("};", "} // namespace v9_compact_data", ""))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(body), encoding="utf-8")

    landmark_summary = None
    if args.landmark_output is not None:
        min_arc_by_net = [65535] * len(nets)
        for group in transitions_by_input:
            for _output, _next_input, arc_delay, net_id in group:
                min_arc_by_net[net_id] = min(min_arc_by_net[net_id], arc_delay)
        geometric: dict[tuple[int, int], tuple[int, int]] = {}
        for net_id, (_output, _input, dx, dy) in enumerate(nets):
            if min_arc_by_net[net_id] == 65535:
                continue
            key = (dx, dy)
            if key not in geometric:
                geometric[key] = (net_id, min_arc_by_net[net_id])
            elif min_arc_by_net[net_id] < geometric[key][1]:
                # Spatial behavior is a function of dx/dy, so retain any
                # representative net while updating the relaxed Arc cost.
                geometric[key] = (geometric[key][0], min_arc_by_net[net_id])

        cell_count = len(cells)
        net_count = len(nets)
        forward: list[list[tuple[int, int]]] = [[] for _ in range(cell_count)]
        reverse: list[list[tuple[int, int]]] = [[] for _ in range(cell_count)]
        for cell in range(cell_count):
            base = cell * net_count
            for representative, arc_delay in geometric.values():
                target = spatial_next[base + representative]
                if target < 0:
                    continue
                cost = arc_delay + spatial_extra[base + representative]
                if cost >= 65535:
                    raise ValueError("relaxed edge cost does not fit uint16")
                forward[cell].append((target, cost))
                reverse[target].append((cell, cost))

        cell_at = {(x, y): index for index, (x, y) in enumerate(cells)}

        def nearest(tx: int, ty: int) -> int:
            return min(
                range(cell_count),
                key=lambda index: (abs(cells[index][0] - tx) + abs(cells[index][1] - ty), index),
            )

        landmarks: list[int] = []
        for y in (0, (height - 1) // 2, height - 1):
            for x in (0, (width - 1) // 3, 2 * (width - 1) // 3, width - 1):
                cell = nearest(x, y)
                if cell not in landmarks:
                    landmarks.append(cell)
        portal_candidates = []
        for cell, (x, y) in enumerate(cells):
            rim = False
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
                if nx <= 0 or nx >= width - 1 or ny <= 0 or ny >= height - 1:
                    continue
                if (nx, ny) not in cell_at:
                    rim = True
                    break
            if rim:
                portal_candidates.append(cell)
        for _ in range(14):
            remaining = [cell for cell in portal_candidates if cell not in landmarks]
            if not remaining:
                break
            best = max(
                remaining,
                key=lambda cell: (
                    min(
                        abs(cells[cell][0] - cells[chosen][0]) +
                        abs(cells[cell][1] - cells[chosen][1])
                        for chosen in landmarks
                    ),
                    -cell,
                ),
            )
            landmarks.append(best)

        dense_count = width * height
        landmark_from: list[int] = []
        landmark_to: list[int] = []
        maximum = 0
        infinity = 1 << 60
        for index, landmark in enumerate(landmarks):
            print(f"landmark {index + 1}/{len(landmarks)} cell={landmark}", flush=True)
            from_distance = shortest_paths(landmark, forward)
            to_distance = shortest_paths(landmark, reverse)
            dense_from = [65535] * dense_count
            dense_to = [65535] * dense_count
            for cell, (x, y) in enumerate(cells):
                dense = y * width + x
                if from_distance[cell] < infinity:
                    if from_distance[cell] >= 65535:
                        raise ValueError("landmark-from distance does not fit uint16")
                    dense_from[dense] = from_distance[cell]
                    maximum = max(maximum, from_distance[cell])
                if to_distance[cell] < infinity:
                    if to_distance[cell] >= 65535:
                        raise ValueError("landmark-to distance does not fit uint16")
                    dense_to[dense] = to_distance[cell]
                    maximum = max(maximum, to_distance[cell])
            landmark_from.extend(dense_from)
            landmark_to.extend(dense_to)
        landmark_body = [
            "// Generated by tools/export_compact_graph.py. Do not edit.",
            "#pragma once",
            "#include <cstdint>",
            "namespace v9_compact_landmarks {",
            f"inline constexpr uint16_t kLandmarkCount = {len(landmarks)};",
            f"inline constexpr uint32_t kDenseCellCount = {dense_count};",
            format_array("kFrom", "uint16_t", landmark_from, per_line=24),
            format_array("kTo", "uint16_t", landmark_to, per_line=24),
            "}  // namespace v9_compact_landmarks",
            "",
        ]
        args.landmark_output.parent.mkdir(parents=True, exist_ok=True)
        args.landmark_output.write_text("\n".join(landmark_body), encoding="utf-8")
        landmark_summary = {
            "landmarks": len(landmarks),
            "geometric_moves": len(geometric),
            "dense_cells": dense_count,
            "max_distance": maximum,
            "output": str(args.landmark_output),
            "bytes": args.landmark_output.stat().st_size,
        }
    print(
        f"ports={len(ports)} inputs={input_count} routes={route_count} nets={len(nets)} "
        f"transitions={len(transitions)} arc_cells={len(arc_to_output)} output={args.output}"
    )
    if landmark_summary is not None:
        print(json.dumps(landmark_summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
