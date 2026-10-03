#!/usr/bin/env python3
"""Export the P6 research assets as deterministic submission-time data.

The two large tables stay in their compact binary representation and are linked
directly into the executable by p6_embedded_data.S.  Small architecture arrays
and the hot-path selector are emitted as constexpr C++ arrays.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import shutil
import struct
from pathlib import Path


FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211


def stem(port: str) -> str:
    left = port.find("[")
    right = port.find("]", left + 1)
    if left < 0 or right < left:
        return port
    return f"{port[:left]}[*]{port[right + 1:]}"


def fnv(values: list[str]) -> int:
    value = FNV_OFFSET
    for item in values:
        for byte in item.encode("utf-8"):
            value = ((value ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
        value = ((value ^ 0xFF) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def emit_array(out: list[str], ctype: str, name: str, values: list[int], width: int = 16) -> None:
    out.append(f"inline constexpr {ctype} {name}[{len(values)}] = {{")
    for begin in range(0, len(values), width):
        out.append("    " + ", ".join(str(item) for item in values[begin:begin + width]) + ",")
    out.append("};")
    out.append("")


def flatten_arcs(rows: list[list[list[int]]]) -> tuple[list[int], list[tuple[int, int]]]:
    offsets = [0]
    arcs: list[tuple[int, int]] = []
    for row in rows:
        arcs.extend((int(item[0]), int(item[1])) for item in row)
        offsets.append(len(arcs))
    return offsets, arcs


def read_selector(path: Path) -> set[int]:
    data = path.read_bytes()
    if len(data) < 12 or data[:8] != b"P6SEL001":
        raise ValueError(f"bad P6 selector: {path}")
    count = struct.unpack_from("<I", data, 8)[0]
    if len(data) != 12 + count * 8:
        raise ValueError(f"bad P6 selector size: {path}")
    return set(struct.unpack_from(f"<{count}Q", data, 12))


def export_top_edges(axis_path: Path, output_path: Path) -> None:
    states = 160
    axis_width = 65
    edge_beam = 32
    raw = axis_path.read_bytes()
    expected_values = 2 * states * axis_width * states
    if len(raw) != expected_values * 2:
        raise ValueError(f"bad P6 axis table size: {axis_path}")
    costs = memoryview(raw).cast("H")
    result = bytearray(2 * states * axis_width * edge_beam)
    cursor = 0
    for axis in range(2):
        for source in range(states):
            for delta_index in range(axis_width):
                base = ((axis * states + source) * axis_width + delta_index) * states
                chosen = sorted(range(states), key=lambda target: (costs[base + target], target))[:edge_beam]
                result[cursor:cursor + edge_beam] = bytes(chosen)
                cursor += edge_beam
    output_path.write_bytes(result)


class BinaryReader:
    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def read(self, fmt: str) -> tuple[int, ...]:
        values = struct.unpack_from("<" + fmt, self.data, self.offset)
        self.offset += struct.calcsize("<" + fmt)
        return values

    def take(self, size: int) -> bytes:
        value = self.data[self.offset:self.offset + size]
        if len(value) != size:
            raise ValueError("truncated P6 runtime table")
        self.offset += size
        return value


def decode_template(encoded: str) -> tuple[int, int, int, int, list[int]]:
    pattern, payload = encoded.split("@", 1)
    axes = [] if pattern == "identity" else pattern.split(">")
    values = [int(item) for item in payload[:-1].split(":")]
    if len(values) != len(axes) + 2 or len(axes) > 10:
        raise ValueError(f"bad P6 template: {encoded}")
    axes_bits = sum((axis == "V") << index for index, axis in enumerate(axes))
    local = values[2:] + [0] * (10 - len(axes))
    return len(axes), values[0], values[1], axes_bits, local


def float32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def export_structured_beams(
    runtime_path: Path,
    accepted: set[int],
    unique_stems: list[str],
    output_path: Path,
) -> tuple[int, int]:
    stream = BinaryReader(runtime_path.read_bytes())
    if stream.take(8) != b"P6RTB001":
        raise ValueError(f"bad P6 runtime table: {runtime_path}")
    label_count = stream.read("I")[0]
    labels = []
    templates = []
    for _ in range(label_count):
        size = stream.read("H")[0]
        label = stream.take(size).decode("ascii")
        labels.append(label)
        templates.append(decode_template(label))
    global_total = stream.read("Q")[0]
    global_counts = list(stream.read(f"{label_count}I"))
    denominator = global_total + 0.5 * label_count
    base_log = [math.log((count + 0.5) / denominator) for count in global_counts]
    global_top_count = stream.read("H")[0]
    global_top = list(stream.read(f"{global_top_count}H"))
    level_count = stream.read("H")[0]
    if level_count != 6:
        raise ValueError("P6 runtime level mismatch")
    levels: list[dict[int, tuple[list[int], dict[int, float]]]] = []
    for _ in range(level_count):
        records: dict[int, tuple[list[int], dict[int, float]]] = {}
        record_count = stream.read("I")[0]
        for _ in range(record_count):
            key, support, top_count, entry_count = stream.read("QIHH")
            top = list(stream.read(f"{top_count}H"))
            gains: dict[int, float] = {}
            for _ in range(entry_count):
                template_id, count = stream.read("HI")
                if count < 2:
                    continue
                base = math.exp(base_log[template_id])
                posterior = (count + 32.0 * base) / (support + 32.0)
                confidence = (count / (count + 2.0)) * (support / (support + 8.0))
                gain = confidence * math.log(max(posterior / base, 1e-12))
                if gain > 0.0:
                    gains[template_id] = float32(gain)
            records[key] = (top, gains)
        levels.append(records)
    if stream.offset != len(stream.data):
        raise ValueError("trailing P6 runtime table data")

    direction_names = ["00", "E0", "W0", "0N", "EN", "WN", "0S", "ES", "WS"]
    fine_bands = [["2", "4", "8"], ["16"], ["17+"]]
    coarse_bands = ["0-8", "9-16", "17+"]
    group_map = [-1] * (len(unique_stems) * 9 * 3)
    groups: list[tuple[int, int, int]] = []
    for source_id, source in enumerate(unique_stems):
        for direction, direction_name in enumerate(direction_names):
            for band_id, bands in enumerate(fine_bands):
                if any(fnv([source, direction_name, band]) in accepted for band in bands):
                    index = (source_id * 9 + direction) * 3 + band_id
                    group_map[index] = len(groups)
                    groups.append((source_id, direction, band_id))

    run_counts = [item[0] for item in templates]

    def beam(source: str, target: str, direction: int, band: str) -> list[int]:
        direction_text = str(direction)
        keys = [
            fnv([source, target, direction_text, band]),
            fnv([source, target, direction_text]),
            fnv([source, direction_text, band]),
            fnv([target, direction_text, band]),
            fnv([direction_text, band]),
            fnv([direction_text]),
        ]
        matched = [level.get(key) for level, key in zip(levels, keys)]
        ids = []
        seen = set()
        for template_id in global_top:
            if template_id not in seen:
                seen.add(template_id)
                ids.append(template_id)
        for record in matched:
            if record is None:
                continue
            for template_id in record[0]:
                if template_id not in seen:
                    seen.add(template_id)
                    ids.append(template_id)
        ranked = []
        for template_id in ids:
            gain = max(
                (record[1].get(template_id, 0.0) for record in matched if record is not None),
                default=0.0,
            )
            ranked.append([template_id, base_log[template_id] + gain])
        ranked.sort(key=lambda item: (-item[1], item[0]))
        ranked = ranked[:64]
        for item in ranked:
            item[1] -= 0.35 * run_counts[item[0]]
        ranked.sort(key=lambda item: (-item[1], item[0]))
        result = [int(item[0]) for item in ranked[:5]]
        if len(result) != 5:
            raise ValueError("P6 precomputed beam has fewer than five candidates")
        return result

    beams: list[int] = []
    for source_id, direction, band_id in groups:
        for target in unique_stems:
            beams.extend(beam(
                unique_stems[source_id], target, direction, coarse_bands[band_id]))

    output = bytearray(b"P6SBM001")
    output += struct.pack("<H", label_count)
    for runs, h_trunk, v_trunk, axes_bits, local in templates:
        output += struct.pack("<BbbH10b", runs, h_trunk, v_trunk, axes_bits, *local)
    output += struct.pack("<HH", len(unique_stems), len(groups))
    output += struct.pack(f"<{len(group_map)}h", *group_map)
    output += struct.pack(f"<{len(beams)}H", *beams)
    output_path.write_bytes(output)
    return len(groups), len(beams)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--axis", type=Path, required=True)
    parser.add_argument("--runtime-table", type=Path, required=True)
    parser.add_argument("--selector", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path, required=True)
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8"))
    accepted = read_selector(args.selector)
    ports = [str(item) for item in model["port_names"]]

    unique_stems: list[str] = []
    stem_ids: dict[str, int] = {}
    port_to_stem: list[int] = []
    for port in ports:
        item = stem(port)
        if item not in stem_ids:
            stem_ids[item] = len(unique_stems)
            unique_stems.append(item)
        port_to_stem.append(stem_ids[item])

    direction_names = ["00", "E0", "W0", "0N", "EN", "WN", "0S", "ES", "WS"]
    selector_bands = ["2", "4", "8", "16", "17+"]
    selector_cells = len(ports) * len(direction_names) * len(selector_bands)
    selector_bits = [0] * ((selector_cells + 63) // 64)
    for port_id, port in enumerate(ports):
        port_stem = stem(port)
        for direction, direction_name in enumerate(direction_names):
            for band_id, band in enumerate(selector_bands):
                if fnv([port_stem, direction_name, band]) in accepted:
                    index = (port_id * 9 + direction) * 5 + band_id
                    selector_bits[index >> 6] |= 1 << (index & 63)

    direct_offsets, direct_arcs = flatten_arcs(model["direct_arcs"])
    target_offsets, target_arcs = flatten_arcs(model["target_arcs"])

    out = [
        "// Generated by tools/export_p6_submission_data.py. Do not edit.",
        "#pragma once",
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "",
        "namespace p6_embedded {",
        "",
        "inline constexpr int kWidth = %d;" % int(model["width"]),
        "inline constexpr int kHeight = %d;" % int(model["height"]),
        "inline constexpr int kPortCount = %d;" % len(ports),
        "inline constexpr int kInputCount = %d;" % len(model["input_to_state"]),
        "inline constexpr int kStateCount = 160;",
        "inline constexpr int kAxisRadius = 32;",
        "inline constexpr int kAxisWidth = 65;",
        "",
        "struct OutputNet { int16_t route; int16_t dx; int16_t dy; };",
        "struct Arc { uint16_t id; uint16_t cost; };",
        "struct Gap { uint8_t vertical; int16_t site; uint16_t delay; };",
        "struct Rect { int16_t left; int16_t right; int16_t lower; int16_t upper; };",
        "",
    ]
    emit_array(out, "int16_t", "kPortToInput", [int(x) for x in model["port_to_input"]])
    emit_array(out, "int16_t", "kInputToState", [int(x) for x in model["input_to_state"]])

    out.append(f"inline constexpr OutputNet kOutputNets[{len(model['output_nets'])}] = {{")
    for route, dx, dy in model["output_nets"]:
        out.append(f"    {{{int(route)}, {int(dx)}, {int(dy)}}},")
    out.extend(["};", ""])

    emit_array(out, "uint16_t", "kDirectOffsets", direct_offsets)
    out.append(f"inline constexpr Arc kDirectArcs[{len(direct_arcs)}] = {{")
    for begin in range(0, len(direct_arcs), 8):
        line = ", ".join(f"{{{a}, {b}}}" for a, b in direct_arcs[begin:begin + 8])
        out.append(f"    {line},")
    out.extend(["};", ""])

    emit_array(out, "uint16_t", "kTargetOffsets", target_offsets)
    out.append(f"inline constexpr Arc kTargetArcs[{len(target_arcs)}] = {{")
    for begin in range(0, len(target_arcs), 8):
        line = ", ".join(f"{{{a}, {b}}}" for a, b in target_arcs[begin:begin + 8])
        out.append(f"    {line},")
    out.extend(["};", ""])

    gaps = model["gaps"]
    out.append(f"inline constexpr Gap kGaps[{len(gaps)}] = {{")
    for item in gaps:
        vertical = 1 if item["direction"] == "vertical" else 0
        out.append(f"    {{{vertical}, {int(item['site'])}, {int(item['delay'])}}},")
    out.extend(["};", ""])

    blocks = model["blocks"]
    out.append(f"inline constexpr Rect kBlocks[{len(blocks)}] = {{")
    for item in blocks:
        out.append(
            f"    {{{int(item['left'])}, {int(item['right'])}, "
            f"{int(item['lower'])}, {int(item['upper'])}}},")
    out.extend(["};", ""])

    emit_array(out, "uint16_t", "kPortToStem", port_to_stem)
    out.append(f"inline constexpr const char* kStemNames[{len(unique_stems)}] = {{")
    for item in unique_stems:
        out.append(f"    {cpp_string(item)},")
    out.extend(["};", ""])
    emit_array(out, "uint64_t", "kSelectorBits", selector_bits, width=4)
    out.extend([
        "inline bool selected(uint16_t port, int direction, int band) {",
        "    if (port >= kPortCount || direction < 0 || direction >= 9 || band < 0 || band >= 5)",
        "        return false;",
        "    const std::size_t index = (static_cast<std::size_t>(port) * 9 + direction) * 5 + band;",
        "    return (kSelectorBits[index >> 6] >> (index & 63)) & UINT64_C(1);",
        "}",
        "",
        "} // namespace p6_embedded",
        "",
    ])

    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.header.write_text("\n".join(out), encoding="utf-8", newline="\n")
    args.data_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(args.axis, args.data_dir / "p6_axis_tables.bin")
    export_top_edges(args.axis, args.data_dir / "p6_top_edges.bin")
    group_count, beam_entries = export_structured_beams(
        args.runtime_table, accepted, unique_stems,
        args.data_dir / "p6_structured_beams.bin")
    digest = hashlib.sha256()
    for name in ("p6_axis_tables.bin", "p6_top_edges.bin", "p6_structured_beams.bin"):
        digest.update((args.data_dir / name).read_bytes())
    (args.data_dir / "p6_data_stamp.inc").write_text(
        f'#define P6_DATA_STAMP "{digest.hexdigest()}"\n',
        encoding="ascii", newline="\n")

    true_cells = sum(value.bit_count() for value in selector_bits)
    print(
        f"ports={len(ports)} stems={len(unique_stems)} selector_hashes={len(accepted)} "
        f"selector_cells={true_cells} direct_arcs={len(direct_arcs)} "
        f"target_arcs={len(target_arcs)} beam_groups={group_count} "
        f"beam_entries={beam_entries}")


if __name__ == "__main__":
    main()
