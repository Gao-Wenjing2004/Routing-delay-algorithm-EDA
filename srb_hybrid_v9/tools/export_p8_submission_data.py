#!/usr/bin/env python3
"""Export Teacher-selected P8 macro tables for static submission embedding."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import struct
from collections import defaultdict
from pathlib import Path
from statistics import fmean

from analyze_global_template_beam import (
    cap_rows_per_source,
    delta_model,
    propose,
    template_model,
)
from analyze_structured_generator import load_summary_rows, selector
from analyze_structured_pricer_ablation import load_audit_rows, load_candidates, load_v8
from analyze_path_skeletons import stem


SELECTOR_LEVELS = (
    ("source_port", "target_port", "direction", "macro_band"),
    ("source_stem", "target_stem", "direction", "macro_band"),
    ("source_port", "direction", "macro_band"),
    ("target_port", "direction", "macro_band"),
    ("source_stem", "direction", "macro_band"),
    ("target_stem", "direction", "macro_band"),
    ("direction", "macro_band"),
    ("direction",),
)
TEMPLATE_LEVELS = tuple(names + ("skeleton",) for names in SELECTOR_LEVELS)
DIRECTION_ID = {
    "00": 0, "E0": 1, "W0": 2, "0N": 3, "EN": 4,
    "WN": 5, "0S": 6, "ES": 7, "WS": 8,
}


def export_top_edges(axis_path: Path, output_path: Path, radius: int = 64) -> None:
    states, edge_beam = 160, 32
    width = 2 * radius + 1
    raw = axis_path.read_bytes()
    expected_values = 2 * states * width * states
    if len(raw) != expected_values * 2:
        raise ValueError(f"bad P8 axis table size: {axis_path}")
    costs = memoryview(raw).cast("H")
    result = bytearray(2 * states * width * edge_beam)
    cursor = 0
    for axis in range(2):
        for source in range(states):
            for delta_index in range(width):
                base = ((axis * states + source) * width + delta_index) * states
                chosen = sorted(
                    range(states), key=lambda target: (costs[base + target], target)
                )[:edge_beam]
                result[cursor:cursor + edge_beam] = bytes(chosen)
                cursor += edge_beam
    output_path.write_bytes(result)


def encode_template(pattern: str, template: tuple[int, ...]) -> tuple[int, int, int, int, list[int]]:
    axes = [] if pattern == "identity" else pattern.split(">")
    if len(template) != len(axes) + 2 or len(axes) > 16:
        raise ValueError(f"P8 template is wider than 16 runs: {pattern}@{template}")
    local = [int(value) for value in template[2:]]
    if any(not -128 <= value <= 127 for value in local):
        raise ValueError(f"P8 local displacement exceeds int8: {pattern}@{template}")
    axes_bits = sum((axis == "V") << index for index, axis in enumerate(axes))
    return len(axes), int(template[0]), int(template[1]), axes_bits, local + [0] * (16 - len(local))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--training-summaries", type=Path, required=True)
    parser.add_argument("--teacher-v8", type=Path, required=True)
    parser.add_argument("--teacher-candidates", type=Path, required=True)
    parser.add_argument("--teacher-priced", type=Path, required=True)
    parser.add_argument("--axis", type=Path, required=True)
    parser.add_argument("--data-dir", type=Path, required=True)
    parser.add_argument("--minimum-count", type=int, default=4)
    parser.add_argument("--margin", type=float, default=5.0)
    parser.add_argument("--max-rows-per-source", type=int, default=64)
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    ports = [str(value) for value in model["port_names"]]
    port_id = {name: index for index, name in enumerate(ports)}
    training = cap_rows_per_source(
        load_summary_rows(args.training_summaries, model, port_id),
        args.max_rows_per_source,
    )
    skeleton_libraries = selector(training, SELECTOR_LEVELS)
    template_support = template_model(training, TEMPLATE_LEVELS)
    delta_support = delta_model(training, "macro_band")

    teacher = load_audit_rows(
        args.teacher_priced,
        load_v8(args.teacher_v8),
        load_candidates(args.teacher_candidates),
    )
    grouped: dict[tuple[str, int, str], list[float]] = defaultdict(list)
    for row in teacher:
        key = (
            str(row["source_stem"]).replace("[]", "[*]"),
            DIRECTION_ID[str(row["direction"])],
            str(row["macro_band"]),
        )
        grouped[key].append(float(row["utility"]))
    accepted = {
        key for key, values in grouped.items()
        if len(values) >= args.minimum_count and fmean(values) > args.margin
    }

    group_map = [-1] * (len(ports) * 9 * 2)
    groups: list[tuple[int, int, int]] = []
    bands = ("17-32", "33-64")
    for source_port, name in enumerate(ports):
        for direction in range(9):
            for band_id, macro in enumerate(bands):
                if (stem(name), direction, macro) not in accepted:
                    continue
                index = (source_port * 9 + direction) * 2 + band_id
                group_map[index] = len(groups)
                groups.append((source_port, direction, band_id))

    template_ids: dict[tuple[str, tuple[int, ...]], int] = {}
    templates: list[tuple[int, int, int, int, list[int]]] = []
    beams: list[int] = []
    for source_port, direction, band_id in groups:
        source_name = ports[source_port]
        for target_name in ports:
            row = {
                "source_port": source_name,
                "target_port": target_name,
                "source_stem": stem(source_name),
                "target_stem": stem(target_name),
                "direction": direction,
                "macro_band": bands[band_id],
                "band": "17+",
                "dx": 0,
                "dy": 0,
            }
            proposed = propose(
                row, skeleton_libraries, template_support, delta_support,
                32, 8, 16, 2, 4, 12, 32.0, 2, 64,
            )[:2]
            if len(proposed) != 2:
                raise ValueError("P8 precomputed Beam has fewer than two candidates")
            for pattern, template, _, _ in proposed:
                key = (pattern, tuple(template))
                template_id = template_ids.get(key)
                if template_id is None:
                    template_id = len(templates)
                    if template_id >= 65535:
                        raise ValueError("P8 template ID exceeds uint16")
                    template_ids[key] = template_id
                    templates.append(encode_template(pattern, tuple(template)))
                beams.append(template_id)

    args.data_dir.mkdir(parents=True, exist_ok=True)
    axis_output = args.data_dir / "p8_axis_tables.bin"
    top_output = args.data_dir / "p8_top_edges.bin"
    beam_output = args.data_dir / "p8_macro_beams.bin"
    shutil.copyfile(args.axis, axis_output)
    export_top_edges(axis_output, top_output)

    output = bytearray(b"P8SBM001")
    output += struct.pack("<H", len(templates))
    for runs, h_trunk, v_trunk, axes_bits, local in templates:
        output += struct.pack("<BbbH16b", runs, h_trunk, v_trunk, axes_bits, *local)
    output += struct.pack("<HH", len(ports), len(groups))
    output += struct.pack(f"<{len(group_map)}h", *group_map)
    output += struct.pack(f"<{len(beams)}H", *beams)
    beam_output.write_bytes(output)

    digest = hashlib.sha256()
    for path in (axis_output, top_output, beam_output):
        digest.update(path.read_bytes())
    (args.data_dir / "p8_data_stamp.inc").write_text(
        f'#define P8_DATA_STAMP "{digest.hexdigest()}"\n',
        encoding="ascii", newline="\n",
    )
    print(
        f"accepted_buckets={len(accepted)} groups={len(groups)} "
        f"templates={len(templates)} beam_entries={len(beams)} "
        f"axis_bytes={axis_output.stat().st_size} top_bytes={top_output.stat().st_size} "
        f"beam_bytes={beam_output.stat().st_size}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
