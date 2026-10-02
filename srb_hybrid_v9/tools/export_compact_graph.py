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


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
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
        _cells = vector(stream, "hh")
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
    print(
        f"ports={len(ports)} inputs={input_count} routes={route_count} nets={len(nets)} "
        f"transitions={len(transitions)} arc_cells={len(arc_to_output)} output={args.output}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
