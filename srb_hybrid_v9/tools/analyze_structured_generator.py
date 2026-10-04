#!/usr/bin/env python3
"""Audit a compact PRP-R path-structure generator on exact Dijkstra paths.

The generator never receives the Dijkstra segment displacements.  It predicts
one of the 17 alternating H/V turn skeletons, expands only architecture-legal
axis residuals, prices every segment with the exact 160-state periodic tables,
keeps a small state/structure beam, and charges Gap lines on the concrete
candidate polyline.  Candidates touching a Block are rejected in this P5
stage; Block detours are deliberately left to the next stage.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import time
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

from analyze_axis_oracle import (
    INF,
    STATE_COUNT,
    build_axis_table,
    endpoint,
    source_candidates,
    target_candidates,
)
from analyze_path_skeletons import fnv1a, stem


BIG = 1_000_000_000
ATOMIC = {
    "H": (-10, -4, -2, -1, 0, 1, 2, 4, 10),
    "V": (-12, -4, -2, -1, 0, 1, 2, 4, 12),
}
SKELETONS = (
    "identity", "H", "V", "H>V", "V>H", "H>V>H", "V>H>V",
    "H>V>H>V", "V>H>V>H", "H>V>H>V>H", "V>H>V>H>V",
    "H>V>H>V>H>V", "V>H>V>H>V>H",
    "H>V>H>V>H>V>H", "V>H>V>H>V>H>V",
    "H>V>H>V>H>V>H>V", "V>H>V>H>V>H>V>H",
)
BUS = re.compile(r"\[\d+\]")


def direction(dx: int, dy: int) -> int:
    return (1 if dx > 0 else 0) + (2 if dx < 0 else 0) + \
           (3 if dy > 0 else 0) + (6 if dy < 0 else 0)


def band(cheb: int) -> str:
    return "0-8" if cheb <= 8 else "9-16" if cheb <= 16 else "17+"


def point_score(golden: int, predicted: int) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def ranked_library(
    rows: list[dict[str, object]], names: tuple[str, ...]
) -> dict[tuple[object, ...], list[str]]:
    counts: dict[tuple[object, ...], Counter[str]] = defaultdict(Counter)
    for row in rows:
        counts[tuple(row[name] for name in names)][str(row["skeleton"])] += 1
    return {key: [value for value, _ in values.most_common()] for key, values in counts.items()}


def primitive_runs(sequence: str) -> list[tuple[str, int]]:
    result: list[tuple[str, int]] = []
    for item in sequence.split("|") if sequence else []:
        axis, delta = item.split("@", 1)[0].split(":", 1)
        if result and result[-1][0] == axis:
            result[-1] = (axis, result[-1][1] + int(delta))
        else:
            result.append((axis, int(delta)))
    return result


SELECTOR_LEVELS = (
    ("source_port", "target_port", "dx", "dy"),
    ("source_port", "target_stem", "dx", "dy"),
    ("source_stem", "target_port", "dx", "dy"),
    ("source_stem", "target_stem", "dx", "dy"),
    ("target_port", "dx", "dy"),
    ("source_port", "dx", "dy"),
    ("target_stem", "dx", "dy"),
    ("source_stem", "dx", "dy"),
    ("source_port", "target_port", "direction", "band"),
    ("source_port", "target_stem", "direction", "band"),
    ("source_stem", "target_port", "direction", "band"),
    ("source_stem", "target_stem", "direction", "band"),
    ("source_port", "target_port", "direction"),
    ("source_stem", "target_stem", "direction"),
    ("source_port", "direction", "band"),
    ("target_port", "direction", "band"),
    ("source_stem", "direction", "band"),
    ("target_stem", "direction", "band"),
    ("direction", "band"),
    ("direction",),
)

DELTA_LEVELS = tuple(
    names + ("skeleton", "run_index") for names in SELECTOR_LEVELS
)

TEMPLATE_LEVELS = tuple(
    names + ("skeleton",) for names in SELECTOR_LEVELS
)


def selector(
    rows: list[dict[str, object]],
    levels: tuple[tuple[str, ...], ...] = SELECTOR_LEVELS,
):
    return [(names, ranked_library(rows, names)) for names in levels]


def selected_skeletons(
    row: dict[str, object], libraries, limit: int
) -> list[str]:
    if limit <= 0:
        return list(SKELETONS)
    result: list[str] = []
    seen: set[str] = set()
    for names, library in libraries:
        key = tuple(row[name] for name in names)
        for value in library.get(key, ()):
            if value in seen:
                continue
            seen.add(value)
            result.append(value)
            if len(result) == limit:
                return result
    for value in SKELETONS:
        if value not in seen:
            result.append(value)
            if len(result) == limit:
                break
    return result


def delta_selector(rows: list[dict[str, object]]):
    records = []
    for row in rows:
        for run_index, delta in enumerate(row["continuation_deltas"]):
            record = dict(row)
            record["run_index"] = run_index
            record["delta"] = delta
            records.append(record)
    libraries = []
    for names in DELTA_LEVELS:
        counts: dict[tuple[object, ...], Counter[int]] = defaultdict(Counter)
        for row in records:
            counts[tuple(row[name] for name in names)][int(row["delta"])] += 1
        libraries.append((names, {
            key: [value for value, _ in values.most_common()]
            for key, values in counts.items()
        }))
    return libraries


def selected_delta_hints(
    row: dict[str, object], pattern: str, run_index: int, libraries, limit: int,
) -> list[int]:
    if limit <= 0:
        return []
    query = dict(row)
    query["skeleton"] = pattern
    query["run_index"] = run_index
    result: list[int] = []
    seen: set[int] = set()
    for names, library in libraries:
        key = tuple(query[name] for name in names)
        for value in library.get(key, ()):
            value = int(value)
            if value in seen:
                continue
            seen.add(value)
            result.append(value)
            if len(result) == limit:
                return result
    return result


def normalized_template(pattern: str, deltas: tuple[int, ...]) -> tuple[int, ...] | None:
    axes = [] if pattern == "identity" else pattern.split(">")
    if len(axes) != len(deltas):
        return None
    values = list(deltas)
    trunks = []
    for axis in ("H", "V"):
        indices = [index for index, value in enumerate(axes) if value == axis]
        if not indices:
            trunks.append(-1)
            continue
        # A shortest path normally consists of small state-changing local
        # moves plus one long, elastic run.  Preserve the local moves and let
        # the largest run absorb the query's exact endpoint displacement.
        trunk = max(indices, key=lambda index: (abs(values[index]), index))
        trunks.append(trunk)
        values[trunk] = 0
    return tuple(trunks + values)


def template_selector(rows: list[dict[str, object]]):
    counts = [defaultdict(Counter) for _ in TEMPLATE_LEVELS]
    for row in rows:
        pattern = str(row["skeleton"])
        template = normalized_template(pattern, tuple(row["continuation_deltas"]))
        if template is None:
            continue
        for level_index, names in enumerate(TEMPLATE_LEVELS):
            counts[level_index][tuple(row[name] for name in names)][template] += 1
    return [
        (names, {
            key: [value for value, _ in values.most_common()]
            for key, values in level.items()
        })
        for names, level in zip(TEMPLATE_LEVELS, counts)
    ]


def selected_templates(
    row: dict[str, object], pattern: str, libraries, limit: int,
) -> list[tuple[int, ...]]:
    query = dict(row)
    query["skeleton"] = pattern
    result: list[tuple[int, ...]] = []
    seen: set[tuple[int, ...]] = set()
    for names, library in libraries:
        key = tuple(query[name] for name in names)
        for value in library.get(key, ()):
            value = tuple(int(item) for item in value)
            if value in seen:
                continue
            seen.add(value)
            result.append(value)
            if len(result) == limit:
                return result
    return result


@dataclass
class Partial:
    costs: object
    x: int
    y: int
    segments: tuple[tuple[str, int], ...]
    score: int


class Geometry:
    def __init__(self, model: dict[str, object]):
        self.width = int(model["width"])
        self.height = int(model["height"])
        self.gaps = list(model["gaps"])
        self.blocks = list(model["blocks"])

    def segment_extra(self, x: int, y: int, axis: str, delta: int) -> tuple[bool, int]:
        nx, ny = (x + delta, y) if axis == "H" else (x, y + delta)
        if not (0 <= nx < self.width and 0 <= ny < self.height):
            return False, 0
        min_x, max_x = sorted((x, nx))
        min_y, max_y = sorted((y, ny))
        for block in self.blocks:
            if (min_x <= int(block["right"]) and max_x >= int(block["left"])
                    and min_y <= int(block["upper"]) and max_y >= int(block["lower"])):
                return False, 0
        extra = 0
        for gap in self.gaps:
            site = int(gap["site"])
            if axis == "H" and str(gap["direction"]).lower() == "vertical":
                if min_x <= site < max_x:
                    extra += int(gap["delay"])
            elif axis == "V" and str(gap["direction"]).lower() != "vertical":
                if min_y <= site < max_y:
                    extra += int(gap["delay"])
        return True, extra


def delta_options(
    axis: str, remaining: int, remaining_runs: int, radius: int, full: bool,
    hints: list[int],
) -> list[int]:
    if remaining_runs == 1:
        return [remaining] if -radius <= remaining <= radius else []
    if full:
        return list(range(-radius, radius + 1))
    values = set(ATOMIC[axis])
    values.update(hints)
    quotient = remaining / remaining_runs
    values.update((math.floor(quotient), math.ceil(quotient), round(quotient), remaining))
    return sorted(value for value in values if -radius <= value <= radius)


def prune_states(np, costs, ranking, beam: int):
    if beam >= STATE_COUNT:
        return costs
    keep = np.argpartition(ranking, beam)[:beam]
    result = np.full(STATE_COUNT, BIG, dtype=np.uint32)
    result[keep] = costs[keep]
    return result


def terminal_lower_bounds(np, axes: list[str], targets, horizontal_any, vertical_any):
    final = np.full(STATE_COUNT, BIG, dtype=np.uint32)
    for state, exit_cost in targets:
        final[state] = min(int(final[state]), int(exit_cost))
    result = [final]
    lower = final
    for axis in reversed(axes):
        matrix = horizontal_any if axis == "H" else vertical_any
        values = matrix.astype(np.uint32) + lower[None, :]
        values[matrix == INF] = BIG
        lower = values.min(axis=1)
        result.append(lower)
    result.reverse()
    return result


def generate_for_skeleton(
    np, axes: list[str], source, targets, total_dx: int, total_dy: int,
    sx: int, sy: int, horizontal, vertical, radius: int,
    horizontal_any, vertical_any, state_beam: int, structure_beam: int,
    full_deltas: bool, delta_hints: list[list[int]], geometry: Geometry,
) -> tuple[int, tuple[tuple[str, int], ...]] | None:
    source_state, entry_dx, entry_dy, entry_cost = source
    entry_axis = "H" if entry_dx else "V" if entry_dy else ""
    if entry_axis and (not axes or axes[0] != entry_axis):
        return None
    entry_valid, entry_extra = (True, 0)
    if entry_axis:
        entry_valid, entry_extra = geometry.segment_extra(sx, sy, entry_axis, entry_dx or entry_dy)
    if not entry_valid:
        return None
    costs = np.full(STATE_COUNT, BIG, dtype=np.uint32)
    costs[source_state] = entry_cost + entry_extra
    initial = Partial(costs, entry_dx, entry_dy, (), entry_cost + entry_extra)
    partials = [initial]
    lower_bounds = terminal_lower_bounds(
        np, axes, targets, horizontal_any, vertical_any)
    for run_index, axis in enumerate(axes):
        remaining_runs = sum(value == axis for value in axes[run_index:])
        expanded: list[Partial] = []
        table = horizontal if axis == "H" else vertical
        for partial in partials:
            remaining = (total_dx - partial.x) if axis == "H" else (total_dy - partial.y)
            deltas = delta_options(
                axis, remaining, remaining_runs, radius, full_deltas,
                delta_hints[run_index] if run_index < len(delta_hints) else [])
            if not deltas:
                continue
            active = np.flatnonzero(partial.costs < BIG)
            if not len(active):
                continue
            for delta_index, delta in enumerate(deltas):
                x, y = sx + partial.x, sy + partial.y
                valid, extra = geometry.segment_extra(x, y, axis, delta)
                if not valid:
                    continue
                # A moving source Arc belongs to the first compressed run.
                # If it already accounts for the entire run, its continuation
                # is a true no-op: keep the routing state and do not price a
                # second, artificial zero-displacement graph walk.
                if run_index == 0 and entry_axis == axis and delta == 0:
                    next_costs = partial.costs.copy()
                else:
                    matrix = table[active, delta + radius, :]
                    values = partial.costs[active, None] + matrix.astype(np.uint32)
                    values[matrix == INF] = BIG
                    next_costs = values.min(axis=0)
                finite = next_costs < BIG
                next_costs = next_costs.copy()
                next_costs[finite] += extra
                ranking = next_costs + lower_bounds[run_index + 1]
                ranking[(next_costs >= BIG) | (lower_bounds[run_index + 1] >= BIG)] = BIG
                next_costs = prune_states(np, next_costs, ranking, state_beam)
                if not np.any(next_costs < BIG):
                    continue
                nx = partial.x + (delta if axis == "H" else 0)
                ny = partial.y + (delta if axis == "V" else 0)
                residual = abs(total_dx - nx) + abs(total_dy - ny)
                ranked = next_costs + lower_bounds[run_index + 1]
                ranked[(next_costs >= BIG) | (lower_bounds[run_index + 1] >= BIG)] = BIG
                score = int(ranked.min()) + 40 * residual
                expanded.append(Partial(
                    next_costs, nx, ny, partial.segments + ((axis, delta),), score))
        expanded.sort(key=lambda item: (item.score, item.x, item.y, item.segments))
        # A single globally cheapest structure can dominate many route states
        # and evict the only viable structure for a particular terminal Arc.
        # Retain the global beam plus the best structure for every surviving
        # Routing Input state.  This is the key state-aware part of P5.
        selected = set(range(min(structure_beam, len(expanded))))
        if expanded:
            state_scores = np.stack([item.costs for item in expanded])
            state_scores = state_scores + lower_bounds[run_index + 1][None, :]
            selected.update(int(index) for index in state_scores.argmin(axis=0))
        partials = [expanded[index] for index in sorted(
            selected, key=lambda index: (
                expanded[index].score, expanded[index].x,
                expanded[index].y, expanded[index].segments))]
        if not partials:
            return None
    best: tuple[int, tuple[tuple[str, int], ...]] | None = None
    for partial in partials:
        if partial.x != total_dx or partial.y != total_dy:
            continue
        for target_state, exit_cost in targets:
            value = int(partial.costs[target_state]) + exit_cost
            if value >= BIG:
                continue
            candidate = (value, partial.segments)
            if best is None or candidate < best:
                best = candidate
    return best


def instantiate_template(
    pattern: str, template: tuple[int, ...], required_dx: int, required_dy: int,
) -> tuple[int, ...] | None:
    axes = [] if pattern == "identity" else pattern.split(">")
    if len(template) != len(axes) + 2:
        return None
    trunk_by_axis = {"H": int(template[0]), "V": int(template[1])}
    values = [int(value) for value in template[2:]]
    for axis, required in (("H", required_dx), ("V", required_dy)):
        indices = [index for index, value in enumerate(axes) if value == axis]
        if not indices:
            if required:
                return None
            continue
        trunk = trunk_by_axis[axis]
        if trunk not in indices:
            return None
        values[trunk] = required - sum(
            values[index] for index in indices if index != trunk)
    return tuple(values)


def price_template(
    np, axes: list[str], deltas: tuple[int, ...], source, targets,
    total_dx: int, total_dy: int, sx: int, sy: int, horizontal, vertical,
    radius: int, horizontal_any, vertical_any, state_beam: int, geometry: Geometry,
) -> int | None:
    source_state, entry_dx, entry_dy, entry_cost = source
    entry_axis = "H" if entry_dx else "V" if entry_dy else ""
    if entry_axis and (not axes or axes[0] != entry_axis):
        return None
    if len(axes) != len(deltas):
        return None
    if any(abs(value) > radius for value in deltas):
        return None
    if entry_axis:
        valid, entry_extra = geometry.segment_extra(
            sx, sy, entry_axis, entry_dx or entry_dy)
        if not valid:
            return None
    else:
        entry_extra = 0
    costs = np.full(STATE_COUNT, BIG, dtype=np.uint32)
    costs[source_state] = entry_cost + entry_extra
    x, y = entry_dx, entry_dy
    lower_bounds = terminal_lower_bounds(
        np, axes, targets, horizontal_any, vertical_any)
    for run_index, (axis, delta) in enumerate(zip(axes, deltas)):
        # The source connector has already performed this first run when the
        # learned local template leaves no continuation displacement.
        source_noop = run_index == 0 and entry_axis == axis and delta == 0
        if not source_noop:
            valid, extra = geometry.segment_extra(sx + x, sy + y, axis, delta)
            if not valid:
                return None
            active = np.flatnonzero(costs < BIG)
            if not len(active):
                return None
            table = horizontal if axis == "H" else vertical
            matrix = table[active, delta + radius, :]
            values = costs[active, None] + matrix.astype(np.uint32)
            values[matrix == INF] = BIG
            costs = values.min(axis=0)
            finite = costs < BIG
            costs[finite] += extra
        ranking = costs + lower_bounds[run_index + 1]
        ranking[(costs >= BIG) | (lower_bounds[run_index + 1] >= BIG)] = BIG
        costs = prune_states(np, costs, ranking, state_beam)
        x += delta if axis == "H" else 0
        y += delta if axis == "V" else 0
    if x != total_dx or y != total_dy:
        return None
    best = BIG
    for target_state, exit_cost in targets:
        best = min(best, int(costs[target_state]) + int(exit_cost))
    return best if best < BIG else None


def estimate_row_templates(
    np, row: dict[str, object], patterns: list[str], model, port_id,
    horizontal, vertical, horizontal_any, vertical_any, radius: int,
    state_beam: int, geometry: Geometry, template_libraries, template_top_k: int,
) -> tuple[int, str, tuple[tuple[str, int], ...]] | None:
    sx, sy, source_name = endpoint(str(row["from"]))
    tx, ty, target_name = endpoint(str(row["to"]))
    source_pid = port_id[source_name]
    target_pid = port_id[target_name]
    sources = source_candidates(model, source_pid)
    targets = target_candidates(model, target_pid)
    best = (0, "identity", ()) if row["from"] == row["to"] else None
    source_iid = int(model["port_to_input"][source_pid])
    if sx == tx and sy == ty and source_iid >= 0:
        direct = [
            int(cost) for output, cost in model["direct_arcs"][source_iid]
            if int(output) == target_pid
        ]
        if direct:
            best = min(best, (min(direct), "identity", ())) if best else \
                (min(direct), "identity", ())
    for pattern in patterns:
        axes = [] if pattern == "identity" else pattern.split(">")
        templates = selected_templates(
            row, pattern, template_libraries, template_top_k)
        for source in sources:
            required_dx = tx - sx - int(source[1])
            required_dy = ty - sy - int(source[2])
            for template in templates:
                deltas = instantiate_template(
                    pattern, template, required_dx, required_dy)
                if deltas is None:
                    continue
                value = price_template(
                    np, axes, deltas, source, targets, tx - sx, ty - sy,
                    sx, sy, horizontal, vertical, radius, horizontal_any,
                    vertical_any, state_beam, geometry)
                if value is None:
                    continue
                segments = tuple(zip(axes, deltas))
                record = (value, pattern, segments)
                if best is None or record < best:
                    best = record
    return best


def estimate_row(
    np, row: dict[str, object], patterns: list[str], model, port_id,
    horizontal, vertical, horizontal_any, vertical_any, radius: int,
    state_beam: int, structure_beam: int, full_deltas: bool, geometry: Geometry,
    delta_libraries, delta_top_k: int,
) -> tuple[int, str, tuple[tuple[str, int], ...]] | None:
    sx, sy, source_name = endpoint(str(row["from"]))
    tx, ty, target_name = endpoint(str(row["to"]))
    source_pid = port_id[source_name]
    target_pid = port_id[target_name]
    sources = source_candidates(model, source_pid)
    targets = target_candidates(model, target_pid)
    best = (0, "identity", ()) if row["from"] == row["to"] else None
    source_iid = int(model["port_to_input"][source_pid])
    if sx == tx and sy == ty and source_iid >= 0:
        direct = [
            int(cost) for output, cost in model["direct_arcs"][source_iid]
            if int(output) == target_pid
        ]
        if direct:
            candidate = (min(direct), "identity", ())
            if best is None or candidate < best:
                best = candidate
    for pattern in patterns:
        axes = [] if pattern == "identity" else pattern.split(">")
        hints = [
            selected_delta_hints(row, pattern, run_index, delta_libraries, delta_top_k)
            for run_index in range(len(axes))
        ]
        for source in sources:
            candidate = generate_for_skeleton(
                np, axes, source, targets, tx - sx, ty - sy, sx, sy,
                horizontal, vertical, radius, horizontal_any, vertical_any,
                state_beam, structure_beam, full_deltas, hints, geometry)
            if candidate is None:
                continue
            value, segments = candidate
            record = (value, pattern, segments)
            if best is None or record < best:
                best = record
    return best


def load_summary_rows(path: Path, model, port_id) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        for raw in csv.DictReader(stream):
            if raw["Reachable"] != "1":
                continue
            sx, sy, source_port = endpoint(raw["From"])
            tx, ty, target_port = endpoint(raw["To"])
            dx, dy = tx - sx, ty - sy
            cheb = max(abs(dx), abs(dy))
            macro_band = "0-16" if cheb <= 16 else "17-32" if cheb <= 32 else \
                "33-64" if cheb <= 64 else "65-128" if cheb <= 128 else "129+"
            rows.append({
                "from": raw["From"], "to": raw["To"], "golden": int(raw["Delay"]),
                "source_endpoint": raw["From"], "source_port": source_port,
                "target_port": target_port, "source_stem": stem(source_port),
                "target_stem": stem(target_port), "direction": direction(dx, dy),
                "band": band(cheb), "macro_band": macro_band, "dx": dx, "dy": dy,
                "skeleton": raw["TurnSequence"] or "identity",
                "block": int(raw["BlockSegments"]) > 0,
                "gap": int(raw["GapCrossings"]) > 0,
                "primitive_sequence": raw["PrimitiveSequence"],
            })
            run_values = primitive_runs(raw["PrimitiveSequence"])
            continuation = [value for _, value in run_values]
            source_pid = port_id[source_port]
            source_iid = int(model["port_to_input"][source_pid])
            source_route = int(model["input_to_state"][source_iid]) if source_iid >= 0 else -1
            if continuation and source_route < 0:
                first_item = raw["PrimitiveSequence"].split("|", 1)[0]
                _, entry_delta = first_item.split("@", 1)[0].split(":", 1)
                continuation[0] -= int(entry_delta)
            rows[-1]["continuation_deltas"] = tuple(continuation)
    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--transitions", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--training-summaries", type=Path, default=None,
                        help="optional independent Dijkstra teacher summaries")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--output-radius", type=int, default=32)
    parser.add_argument("--search-radius", type=int, default=64)
    parser.add_argument("--state-beam", type=int, default=32)
    parser.add_argument("--structure-beam", type=int, default=32)
    parser.add_argument("--full-deltas", action="store_true",
                        help="enumerate every axis residual in the table radius")
    parser.add_argument("--delta-top-k", type=int, default=8,
                        help="held-out Dijkstra segment hints per skeleton run")
    parser.add_argument("--selector-top-k", type=int, default=8,
                        help="0 tries all 17 patterns")
    parser.add_argument("--template-top-k", type=int, default=0,
                        help="use this many learned elastic full-path templates")
    parser.add_argument("--known-skeleton", action="store_true")
    parser.add_argument("--max-validation", type=int, default=0)
    parser.add_argument("--only-from", default="")
    parser.add_argument("--only-to", default="")
    args = parser.parse_args()
    try:
        import numpy as np
    except ModuleNotFoundError:
        parser.error("this audit requires NumPy")
    if args.search_radius < args.output_radius:
        parser.error("search radius must cover output radius")

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    horizontal_raw = build_axis_table(
        args.transitions, "H", args.output_radius, args.search_radius)
    vertical_raw = build_axis_table(
        args.transitions, "V", args.output_radius, args.search_radius)
    width = 2 * args.output_radius + 1
    horizontal = np.frombuffer(horizontal_raw, dtype=np.uint16).reshape(
        STATE_COUNT, width, STATE_COUNT)
    vertical = np.frombuffer(vertical_raw, dtype=np.uint16).reshape(
        STATE_COUNT, width, STATE_COUNT)
    horizontal_any = horizontal.min(axis=1)
    vertical_any = vertical.min(axis=1)
    geometry = Geometry(model)

    rows = load_summary_rows(args.summaries, model, port_id)
    training = load_summary_rows(args.training_summaries, model, port_id) \
        if args.training_summaries is not None else \
        [row for row in rows if fnv1a(str(row["source_endpoint"])) % 5 != 0]
    validation = [row for row in rows if fnv1a(str(row["source_endpoint"])) % 5 == 0]
    if args.only_from:
        validation = [row for row in validation if row["from"] == args.only_from]
    if args.only_to:
        validation = [row for row in validation if row["to"] == args.only_to]
    if args.max_validation:
        validation = validation[:args.max_validation]
    libraries = selector(training)
    delta_libraries = None if args.template_top_k else delta_selector(training)
    template_libraries = template_selector(training) if args.template_top_k else None

    attempted = candidate_rows = exact = 0
    selector_hits = block_rows = 0
    score_sum = absolute_sum = 0.0
    elapsed_sum = 0.0
    chosen_patterns: Counter[str] = Counter()
    examples = []
    for row in validation:
        if row["block"]:
            block_rows += 1
            continue
        patterns = [str(row["skeleton"])] if args.known_skeleton else \
            selected_skeletons(row, libraries, args.selector_top_k)
        selector_hits += str(row["skeleton"]) in patterns
        attempted += 1
        started = time.perf_counter_ns()
        if args.template_top_k:
            result = estimate_row_templates(
                np, row, patterns, model, port_id, horizontal, vertical,
                horizontal_any, vertical_any, args.output_radius,
                args.state_beam, geometry, template_libraries,
                args.template_top_k)
        else:
            result = estimate_row(
                np, row, patterns, model, port_id, horizontal, vertical,
                horizontal_any, vertical_any, args.output_radius,
                args.state_beam, args.structure_beam, args.full_deltas, geometry,
                delta_libraries, args.delta_top_k)
        elapsed_sum += (time.perf_counter_ns() - started) / 1000.0
        if result is None:
            continue
        predicted, pattern, segments = result
        candidate_rows += 1
        chosen_patterns[pattern] += 1
        golden = int(row["golden"])
        error = abs(predicted - golden)
        exact += predicted == golden
        score_sum += point_score(golden, predicted)
        absolute_sum += error
        if error and len(examples) < 20:
            examples.append({
                "from": row["from"], "to": row["to"], "golden": golden,
                "predicted": predicted, "pattern": pattern, "actual": row["skeleton"],
                "segments": segments,
            })

    report = {
        "definition": "17 H/V skeletons + architecture residuals + exact axis pricing + segment Gap",
        "split": "FNV1a(complete source endpoint) % 5; bucket 0 is validation",
        "training_source": str(args.training_summaries) if args.training_summaries else "same-file bucket 1-4",
        "training_rows": len(training),
        "known_skeleton": args.known_skeleton,
        "selector_top_k": args.selector_top_k,
        "state_beam": args.state_beam,
        "structure_beam": args.structure_beam,
        "full_deltas": args.full_deltas,
        "delta_top_k": args.delta_top_k,
        "template_top_k": args.template_top_k,
        "validation_rows": len(validation),
        "excluded_actual_block_rows": block_rows,
        "attempted_rows": attempted,
        "selector_recall": selector_hits / max(attempted, 1),
        "candidate_rows": candidate_rows,
        "coverage": candidate_rows / max(attempted, 1),
        "exact_matches": exact,
        "exact_match_rate": exact / max(candidate_rows, 1),
        "accuracy": 100.0 * score_sum / max(candidate_rows, 1),
        "mae": absolute_sum / max(candidate_rows, 1),
        "mean_python_us": elapsed_sum / max(attempted, 1),
        "chosen_patterns": dict(chosen_patterns.most_common()),
        "mismatch_examples": examples,
        "warning": "Python timing is for relative ablation only; C++ timing decides submission Go/No-Go.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
