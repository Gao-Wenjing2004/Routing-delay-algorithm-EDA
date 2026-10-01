#!/usr/bin/env python3
"""Evaluate architecture-only PRP-R P1 against P0 and V3 on fixed scopes.

The P1 estimator reads only the generated architecture model.  Golden values
are consumed after prediction for scoring and are never used to fit P1.
"""

from __future__ import annotations

import argparse
from array import array
import csv
from dataclasses import dataclass, field
import json
import math
from pathlib import Path
import re
import time
from typing import Iterable


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    root = here.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=root / "analysis" / "prpr" / "prpr_model.json")
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--p0", type=Path, required=True)
    parser.add_argument("--v3", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=root / "analysis" / "prpr")
    parser.add_argument("--limit", type=int)
    return parser.parse_args()


def endpoint(text: str, port_id: dict[str, int]) -> tuple[int, int, int]:
    cell, port = text.strip().split("/", 1)
    prefix, xs, ys = cell.split("_")
    if prefix != "SRB":
        raise ValueError(f"invalid endpoint: {text}")
    return int(xs), int(ys), port_id[port]


def fnv_scope(source: str, target: str) -> bool:
    value = 2166136261
    for byte in (source + "|" + target).encode("utf-8"):
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value % 5 == 0


def vector_bin(dx: int, dy: int) -> int:
    if dx == 0 and dy == 0:
        return 0
    return int(math.floor((math.atan2(dy, dx) + math.pi) * 16 / (2 * math.pi) + 0.5)) % 16


def direction8(dx: int, dy: int) -> str:
    if dx == 0 and dy == 0:
        return "same"
    ax, ay = abs(dx), abs(dy)
    if ax * 2 < ay:
        return "N" if dy > 0 else "S"
    if ay * 2 < ax:
        return "E" if dx > 0 else "W"
    return ("N" if dy > 0 else "S") + ("E" if dx > 0 else "W")


@dataclass
class Candidate:
    delay: float
    remainder: int
    kind: str
    primitive_count: int


class PRPREstimator:
    def __init__(self, model: dict[str, object]):
        self.model = model
        self.port_names = list(model["port_names"])
        self.port_id = {name: index for index, name in enumerate(self.port_names)}
        self.port_directions = list(model["port_directions"])
        self.port_to_input = list(model["port_to_input"])
        self.input_to_state = list(model["input_to_state"])
        self.state_classes = list(model["state_classes"])
        self.output_nets = [tuple(row) for row in model["output_nets"]]
        self.direct_arcs = [
            [tuple(item) for item in rows] for rows in model["direct_arcs"]
        ]
        self.target_arcs = [
            [tuple(item) for item in rows] for rows in model["target_arcs"]
        ]
        self.connectors = model["class_connectors"]
        self.basis_by_bin = {int(row["direction_bin"]): row for row in model["basis"]}
        if len(self.basis_by_bin) != 16:
            raise ValueError("P1 requires one architecture-derived basis primitive per direction bin")
        self.runtime_by_bin: dict[int, list[dict[str, object]]] = {
            direction: [] for direction in range(16)
        }
        for row in model["runtime_primitives"]:
            self.runtime_by_bin[int(row["direction_bin"])].append(row)
        if any(not rows for rows in self.runtime_by_bin.values()):
            raise ValueError("P1 requires a multi-scale runtime primitive in every direction bin")
        for rows in self.runtime_by_bin.values():
            rows.sort(key=lambda row: math.hypot(int(row["dx"]), int(row["dy"])))
        self._coverage_cache: dict[tuple[int, int], tuple[bool, bool, int]] = {}
        self.axis_rate = {
            "east": self._axis_rate(lambda p: p["dx"] > 0 and p["dy"] == 0),
            "west": self._axis_rate(lambda p: p["dx"] < 0 and p["dy"] == 0),
            "north": self._axis_rate(lambda p: p["dy"] > 0 and p["dx"] == 0),
            "south": self._axis_rate(lambda p: p["dy"] < 0 and p["dx"] == 0),
        }
        width, height = int(model["width"]), int(model["height"])
        self.x_gap_prefix = [0] * width
        self.y_gap_prefix = [0] * height
        vertical = {int(row["site"]): int(row["delay"]) for row in model["gaps"] if row["direction"] == "vertical"}
        horizontal = {int(row["site"]): int(row["delay"]) for row in model["gaps"] if row["direction"] == "horizontal"}
        running = 0
        for x in range(width):
            self.x_gap_prefix[x] = running
            running += vertical.get(x, 0)
        running = 0
        for y in range(height):
            self.y_gap_prefix[y] = running
            running += horizontal.get(y, 0)
        self.blocks = list(model["blocks"])
        self._source_by_bin = self._precompute_source_candidates()
        self._target_candidates = self._precompute_target_candidates()

    def _axis_rate(self, predicate) -> float:
        rows = [
            float(row["total_delay"]) / max(abs(int(row["dx"])) + abs(int(row["dy"])), 1)
            for bin_rows in self.runtime_by_bin.values()
            for row in bin_rows
            if predicate(row)
        ]
        if not rows:
            raise ValueError("missing axis primitive")
        return min(rows)

    def _alignment_penalty(self, dx: int, dy: int, wanted_bin: int) -> float:
        if dx == 0 and dy == 0:
            return 50.0
        delta = abs(vector_bin(dx, dy) - wanted_bin)
        delta = min(delta, 16 - delta)
        return 12.0 * delta

    def _precompute_source_candidates(self) -> list[list[list[tuple[int, int, int, int]]]]:
        result: list[list[list[tuple[int, int, int, int]]]] = []
        for pid in range(len(self.port_names)):
            per_bin: list[list[tuple[int, int, int, int]]] = []
            for wanted in range(16):
                candidates: list[tuple[int, int, int, int]] = []
                iid = int(self.port_to_input[pid])
                if iid >= 0:
                    state = int(self.input_to_state[iid])
                    if state >= 0:
                        candidates.append((state, 0, 0, 0))
                    else:
                        for output_pid, cost in self.direct_arcs[iid]:
                            next_state, dx, dy = self.output_nets[int(output_pid)]
                            if int(next_state) >= 0:
                                candidates.append((int(next_state), int(dx), int(dy), int(cost)))
                else:
                    next_state, dx, dy = self.output_nets[pid]
                    if int(next_state) >= 0:
                        candidates.append((int(next_state), int(dx), int(dy), 0))
                candidates.sort(
                    key=lambda row: (
                        row[3] + self._alignment_penalty(row[1], row[2], wanted),
                        row,
                    )
                )
                # P1 evaluates one deterministic architecture-derived entry.
                # Keeping two entries and two exits multiplies the seven
                # decomposition candidates to 28 without a learned selector.
                per_bin.append(candidates[:1])
            result.append(per_bin)
        return result

    def _precompute_target_candidates(self) -> list[list[tuple[int, int]]]:
        result: list[list[tuple[int, int]]] = []
        for pid in range(len(self.port_names)):
            iid = int(self.port_to_input[pid])
            if iid >= 0:
                state = int(self.input_to_state[iid])
                result.append([(state, 0)] if state >= 0 else [])
            else:
                rows = sorted((int(state), int(cost)) for state, cost in self.target_arcs[pid])
                rows.sort(key=lambda row: (row[1], row[0]))
                result.append(rows[:1])
        return result

    def _residual_cost(self, dx: int, dy: int) -> float:
        x_rate = self.axis_rate["east" if dx >= 0 else "west"]
        y_rate = self.axis_rate["north" if dy >= 0 else "south"]
        return abs(dx) * x_rate + abs(dy) * y_rate

    def _connector(self, source_class: int, target_class: int) -> tuple[int, int, int]:
        row = self.connectors[source_class][target_class]
        return int(row["dx"]), int(row["dy"]), int(row["cost"])

    @staticmethod
    def _count_options(value: float) -> list[int]:
        values = {0, max(0, int(math.floor(value))), max(0, int(math.ceil(value))), max(0, int(round(value)))}
        return sorted(values)

    def _runtime_primitive(self, direction: int, dx: int, dy: int) -> dict[str, object]:
        wanted_scale = min(120.0, max(12.0, math.hypot(dx, dy)))
        return min(
            self.runtime_by_bin[direction],
            key=lambda row: (
                abs(math.hypot(int(row["dx"]), int(row["dy"])) - wanted_scale),
                float(row["total_delay"]) / max(math.hypot(int(row["dx"]), int(row["dy"])), 1.0),
                int(row["primitive_id"]),
            ),
        )

    def _single_candidate(self, source_class: int, target_class: int, dx: int, dy: int, primitive: dict[str, object]) -> Candidate:
        pclass = int(primitive["entry_class"])
        pre_dx, pre_dy, pre_cost = self._connector(source_class, pclass)
        post_dx, post_dy, post_cost = self._connector(pclass, target_class)
        vx, vy = dx - pre_dx - post_dx, dy - pre_dy - post_dy
        px, py = int(primitive["dx"]), int(primitive["dy"])
        projection = (vx * px + vy * py) / max(px * px + py * py, 1)
        best: Candidate | None = None
        for count in self._count_options(projection):
            rx, ry = vx - count * px, vy - count * py
            candidate = Candidate(
                pre_cost + post_cost + count * int(primitive["total_delay"]) + self._residual_cost(rx, ry),
                max(abs(rx), abs(ry)),
                "single_primitive",
                1 if count else 0,
            )
            if best is None or (candidate.delay, candidate.remainder) < (best.delay, best.remainder):
                best = candidate
        assert best is not None
        return best

    def _pair_candidate(self, source_class: int, target_class: int, dx: int, dy: int, first: dict[str, object], second: dict[str, object]) -> Candidate | None:
        c1, c2 = int(first["entry_class"]), int(second["entry_class"])
        pre_dx, pre_dy, pre_cost = self._connector(source_class, c1)
        mid_dx, mid_dy, mid_cost = self._connector(c1, c2)
        post_dx, post_dy, post_cost = self._connector(c2, target_class)
        vx = dx - pre_dx - mid_dx - post_dx
        vy = dy - pre_dy - mid_dy - post_dy
        x1, y1 = int(first["dx"]), int(first["dy"])
        x2, y2 = int(second["dx"]), int(second["dy"])
        determinant = x1 * y2 - y1 * x2
        if determinant == 0:
            return None
        real1 = (vx * y2 - vy * x2) / determinant
        real2 = (x1 * vy - y1 * vx) / determinant
        if real1 < -0.75 or real2 < -0.75:
            return None
        best: Candidate | None = None
        for n1 in self._count_options(real1):
            for n2 in self._count_options(real2):
                rx, ry = vx - n1 * x1 - n2 * x2, vy - n1 * y1 - n2 * y2
                candidate = Candidate(
                    pre_cost
                    + mid_cost
                    + post_cost
                    + n1 * int(first["total_delay"])
                    + n2 * int(second["total_delay"])
                    + self._residual_cost(rx, ry),
                    max(abs(rx), abs(ry)),
                    "dual_primitive",
                    int(n1 > 0) + int(n2 > 0),
                )
                if best is None or (candidate.delay, candidate.remainder) < (best.delay, best.remainder):
                    best = candidate
        return best

    def _travel(self, source_state: int, target_state: int, dx: int, dy: int) -> Candidate:
        source_class = int(self.state_classes[source_state])
        target_class = int(self.state_classes[target_state])
        direct_dx, direct_dy, direct_cost = self._connector(source_class, target_class)
        best = Candidate(
            direct_cost + self._residual_cost(dx - direct_dx, dy - direct_dy),
            max(abs(dx - direct_dx), abs(dy - direct_dy)),
            "connector_remainder",
            0,
        )
        wanted = vector_bin(dx, dy)
        for offset in (-1, 0, 1):
            primitive = self._runtime_primitive((wanted + offset) % 16, dx, dy)
            candidate = self._single_candidate(
                source_class, target_class, dx, dy, primitive
            )
            if (candidate.delay, candidate.remainder) < (best.delay, best.remainder):
                best = candidate
        pair_bins = [
            ((wanted - 1) % 16, wanted),
            (wanted, (wanted + 1) % 16),
        ]
        horizontal = 8 if dx >= 0 else 0
        vertical = 12 if dy >= 0 else 4
        pair_bins.append((horizontal, vertical))
        seen = set()
        for first_bin, second_bin in pair_bins:
            key = (first_bin, second_bin)
            if key in seen:
                continue
            seen.add(key)
            candidate = self._pair_candidate(
                source_class,
                target_class,
                dx,
                dy,
                self._runtime_primitive(first_bin, dx, dy),
                self._runtime_primitive(second_bin, dx, dy),
            )
            if candidate is not None and (candidate.delay, candidate.remainder) < (best.delay, best.remainder):
                best = candidate
        return best

    @staticmethod
    def _raw_single_remainder(dx: int, dy: int, primitive: dict[str, object]) -> int:
        px, py = int(primitive["dx"]), int(primitive["dy"])
        projection = (dx * px + dy * py) / max(px * px + py * py, 1)
        return min(
            max(abs(dx - count * px), abs(dy - count * py))
            for count in PRPREstimator._count_options(projection)
        )

    @staticmethod
    def _raw_pair_remainder(dx: int, dy: int, first: dict[str, object], second: dict[str, object]) -> int | None:
        x1, y1 = int(first["dx"]), int(first["dy"])
        x2, y2 = int(second["dx"]), int(second["dy"])
        determinant = x1 * y2 - y1 * x2
        if determinant == 0:
            return None
        real1 = (dx * y2 - dy * x2) / determinant
        real2 = (x1 * dy - y1 * dx) / determinant
        if real1 < -0.75 or real2 < -0.75:
            return None
        return min(
            max(abs(dx - n1 * x1 - n2 * x2), abs(dy - n1 * y1 - n2 * y2))
            for n1 in PRPREstimator._count_options(real1)
            for n2 in PRPREstimator._count_options(real2)
        )

    def displacement_coverage(self, dx: int, dy: int) -> tuple[bool, bool, int]:
        """Report expressibility independently of the minimum-delay choice."""

        key = (dx, dy)
        cached = self._coverage_cache.get(key)
        if cached is not None:
            return cached
        if dx == 0 and dy == 0:
            result = (False, True, 0)
            self._coverage_cache[key] = result
            return result
        wanted = vector_bin(dx, dy)
        single_bins = tuple((wanted + offset) % 16 for offset in (-1, 0, 1))
        best_any = max(abs(dx), abs(dy))
        for direction in single_bins:
            for primitive in self.runtime_by_bin[direction]:
                best_any = min(best_any, self._raw_single_remainder(dx, dy, primitive))
        horizontal = 8 if dx >= 0 else 0
        vertical = 12 if dy >= 0 else 4
        # For coverage (not delay selection), the shortest horizontal and
        # vertical cycles form the finest lattice.  Longer and diagonal
        # cycles can improve cost but cannot improve this lattice remainder.
        first = min(
            self.runtime_by_bin[horizontal],
            key=lambda row: math.hypot(int(row["dx"]), int(row["dy"])),
        )
        second = min(
            self.runtime_by_bin[vertical],
            key=lambda row: math.hypot(int(row["dx"]), int(row["dy"])),
        )
        best_dual = max(abs(dx), abs(dy))
        remainder = self._raw_pair_remainder(dx, dy, first, second)
        if remainder is not None:
            best_dual = min(best_dual, remainder)
            best_any = min(best_any, remainder)
        result = (best_dual <= 12, best_any <= 12, best_any)
        self._coverage_cache[key] = result
        return result

    def endpoint_kind(self, pid: int) -> str:
        iid = int(self.port_to_input[pid])
        if iid >= 0:
            return "routing_input" if int(self.input_to_state[iid]) >= 0 else "query_input"
        return "reachable_output" if self.target_arcs[pid] else "logic_output"

    def port_family(self, pid: int) -> str:
        name = re.sub(r"\[\d+\]$", "", self.port_names[pid])
        return re.sub(r"\d+$", "", name)

    def gap_cost(self, sx: int, sy: int, tx: int, ty: int) -> int:
        return abs(self.x_gap_prefix[tx] - self.x_gap_prefix[sx]) + abs(self.y_gap_prefix[ty] - self.y_gap_prefix[sy])

    def overlap_features(self, sx: int, sy: int, tx: int, ty: int) -> tuple[int, int, int]:
        min_x, max_x = sorted((sx, tx))
        min_y, max_y = sorted((sy, ty))
        blocks = sum(
            min_x <= int(block["right"])
            and max_x >= int(block["left"])
            and min_y <= int(block["upper"])
            and max_y >= int(block["lower"])
            for block in self.blocks
        )
        gap_count = 0
        for row in self.model["gaps"]:
            site = int(row["site"])
            if row["direction"] == "vertical":
                gap_count += min_x < site <= max_x
            else:
                gap_count += min_y < site <= max_y
        boundary = min(sx, tx, 119 - sx, 119 - tx, sy, ty, 549 - sy, 549 - ty)
        return int(blocks), int(gap_count), int(boundary)

    def predict(self, source: tuple[int, int, int], target: tuple[int, int, int]) -> tuple[int, Candidate]:
        sx, sy, source_pid = source
        tx, ty, target_pid = target
        if source == target:
            return 0, Candidate(0.0, 0, "identity", 0)
        dx, dy = tx - sx, ty - sy
        source_iid = int(self.port_to_input[source_pid])
        if dx == 0 and dy == 0 and source_iid >= 0 and self.port_directions[target_pid] == "output":
            direct = [cost for output_pid, cost in self.direct_arcs[source_iid] if int(output_pid) == target_pid]
            if direct:
                value = min(direct)
                return value, Candidate(float(value), 0, "direct_arc", 0)
        wanted = vector_bin(dx, dy)
        source_candidates = self._source_by_bin[source_pid][wanted]
        target_candidates = self._target_candidates[target_pid]
        best: Candidate | None = None
        for source_state, seed_dx, seed_dy, seed_cost in source_candidates:
            for target_state, exit_cost in target_candidates:
                travel = self._travel(
                    source_state,
                    target_state,
                    dx - seed_dx,
                    dy - seed_dy,
                )
                candidate = Candidate(
                    seed_cost + exit_cost + travel.delay,
                    travel.remainder,
                    travel.kind,
                    travel.primitive_count,
                )
                if best is None or (candidate.delay, candidate.remainder) < (best.delay, best.remainder):
                    best = candidate
        if best is None:
            # Invalid structural endpoint combination: deterministic analytic fallback.
            best = Candidate(self._residual_cost(dx, dy), max(abs(dx), abs(dy)), "unreachable_fallback", 0)
        delay = best.delay + self.gap_cost(sx, sy, tx, ty)
        return max(0, int(math.floor(delay + 0.5))), best


@dataclass
class Metrics:
    keep_quantiles: bool = False
    rows: int = 0
    loss_sum: float = 0.0
    relative_sum: float = 0.0
    absolute_sum: float = 0.0
    squared_sum: float = 0.0
    max_error: float = 0.0
    within_1: int = 0
    within_5: int = 0
    within_10: int = 0
    relatives: array = field(default_factory=lambda: array("d"))

    def add(self, golden: int, predicted: int) -> None:
        absolute = abs(predicted - golden)
        relative = absolute / golden if golden else (0.0 if predicted == 0 else math.inf)
        loss = math.tanh(4.0 * relative) if math.isfinite(relative) else 1.0
        self.rows += 1
        self.loss_sum += loss
        self.relative_sum += relative
        self.absolute_sum += absolute
        self.squared_sum += absolute * absolute
        self.max_error = max(self.max_error, absolute)
        self.within_1 += relative <= 0.01
        self.within_5 += relative <= 0.05
        self.within_10 += relative <= 0.10
        if self.keep_quantiles:
            self.relatives.append(relative)

    def render(self) -> dict[str, object]:
        if not self.rows:
            return {"rows": 0}
        result: dict[str, object] = {
            "rows": self.rows,
            "acc_score": 100.0 * (1.0 - self.loss_sum / self.rows),
            "mean_relative_error": self.relative_sum / self.rows,
            "mae": self.absolute_sum / self.rows,
            "rmse": math.sqrt(self.squared_sum / self.rows),
            "max_error": self.max_error,
            "within_1pct": self.within_1 / self.rows,
            "within_5pct": self.within_5 / self.rows,
            "within_10pct": self.within_10 / self.rows,
        }
        if self.keep_quantiles:
            values = sorted(self.relatives)
            for label, fraction in (("p50", 0.50), ("p90", 0.90), ("p95", 0.95), ("p99", 0.99)):
                index = min(len(values) - 1, int(math.ceil(fraction * len(values))) - 1)
                result[f"{label}_relative_error"] = values[index]
        return result


def delay_column(reader: csv.DictReader) -> str:
    for name in reader.fieldnames or []:
        if name.lower() == "delay":
            return name
    raise ValueError("delay column missing")


def group_labels(
    dx: int,
    dy: int,
    blocks: int,
    gaps: int,
    boundary: int,
    candidate: Candidate,
    source_kind: str,
    target_kind: str,
    source_family: str,
    target_family: str,
) -> Iterable[tuple[str, str]]:
    distance = max(abs(dx), abs(dy))
    if distance <= 16:
        distance_label = "0-16"
    elif distance <= 48:
        distance_label = "17-48"
    elif distance <= 72:
        distance_label = "49-72"
    elif distance <= 128:
        distance_label = "73-128"
    elif distance <= 256:
        distance_label = "129-256"
    else:
        distance_label = "257+"
    remainder = candidate.remainder
    remainder_label = "0-4" if remainder <= 4 else "5-12" if remainder <= 12 else "13-32" if remainder <= 32 else "33+"
    yield "distance", distance_label
    yield "direction", direction8(dx, dy)
    yield "block", "none" if blocks == 0 else "one" if blocks == 1 else "two_plus"
    yield "gap_count", "0" if gaps == 0 else "1-2" if gaps <= 2 else "3-5" if gaps <= 5 else "6+"
    yield "boundary_distance", "0-3" if boundary <= 3 else "4-15" if boundary <= 15 else "16+"
    yield "remainder", remainder_label
    yield "candidate_type", candidate.kind
    yield "primitive_decomposition", "success" if candidate.primitive_count > 0 and remainder <= 12 else "failure"
    yield "source_port_kind", source_kind
    yield "target_port_kind", target_kind
    yield "source_port_family", source_family
    yield "target_port_family", target_family


def main() -> int:
    opt = parse_args()
    output_dir = opt.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    model = json.loads(opt.model.read_text(encoding="utf-8"))
    estimator = PRPREstimator(model)

    full = {name: Metrics(True) for name in ("P0_no_atlas", "P1_primitive_base", "V3")}
    validation = {name: Metrics(True) for name in full}
    groups: dict[tuple[str, str], Metrics] = {}
    coverage = CounterLike()
    prediction_path = output_dir / "p1_predictions_1m.csv"
    begin = time.perf_counter()

    with opt.golden.open("r", encoding="utf-8-sig", newline="") as gf, opt.p0.open("r", encoding="utf-8-sig", newline="") as p0f, opt.v3.open("r", encoding="utf-8-sig", newline="") as v3f, prediction_path.open("w", encoding="utf-8", newline="") as out:
        golden_reader = csv.DictReader(gf)
        p0_reader = csv.DictReader(p0f)
        v3_reader = csv.DictReader(v3f)
        golden_delay = delay_column(golden_reader)
        p0_delay = delay_column(p0_reader)
        v3_delay = delay_column(v3_reader)
        writer = csv.writer(out, lineterminator="\n")
        writer.writerow(["From", "To", "delay"])
        for index, rows in enumerate(zip(golden_reader, p0_reader, v3_reader)):
            golden_row, p0_row, v3_row = rows
            if opt.limit is not None and index >= opt.limit:
                break
            source_text, target_text = golden_row["From"], golden_row["To"]
            for other in (p0_row, v3_row):
                if other["From"] != source_text or other["To"] != target_text:
                    raise ValueError(f"alignment mismatch at row {index + 2}")
            source = endpoint(source_text, estimator.port_id)
            target = endpoint(target_text, estimator.port_id)
            p1_value, candidate = estimator.predict(source, target)
            golden = int(golden_row[golden_delay])
            values = {
                "P0_no_atlas": int(p0_row[p0_delay]),
                "P1_primitive_base": p1_value,
                "V3": int(v3_row[v3_delay]),
            }
            writer.writerow([source_text, target_text, p1_value])
            held_out = fnv_scope(source_text, target_text)
            for name, predicted in values.items():
                full[name].add(golden, predicted)
                if held_out:
                    validation[name].add(golden, predicted)
            sx, sy, _ = source
            tx, ty, _ = target
            dx, dy = tx - sx, ty - sy
            blocks, gaps, boundary = estimator.overlap_features(sx, sy, tx, ty)
            source_pid = source[2]
            target_pid = target[2]
            for category, segment in group_labels(
                dx,
                dy,
                blocks,
                gaps,
                boundary,
                candidate,
                estimator.endpoint_kind(source_pid),
                estimator.endpoint_kind(target_pid),
                estimator.port_family(source_pid),
                estimator.port_family(target_pid),
            ):
                groups.setdefault((category, segment), Metrics()).add(golden, p1_value)
            coverage.add(candidate, estimator.displacement_coverage(dx, dy))
            if (index + 1) % 100000 == 0:
                elapsed = time.perf_counter() - begin
                print(f"rows={index + 1} elapsed={elapsed:.1f}s qps={(index + 1) / elapsed:.0f}", flush=True)

    elapsed = time.perf_counter() - begin
    comparison_rows = []
    for scope, metrics_by_name in (("full_public", full), ("fixed_fnv20_validation", validation)):
        for name, metrics in metrics_by_name.items():
            row = {"scope": scope, "model": name, **metrics.render()}
            row["prediction_seconds"] = elapsed if name == "P1_primitive_base" and scope == "full_public" else ""
            comparison_rows.append(row)
    comparison_fields = list(comparison_rows[0])
    for row in comparison_rows:
        for key in row:
            if key not in comparison_fields:
                comparison_fields.append(key)
    with (output_dir / "p0_p1_comparison.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=comparison_fields)
        writer.writeheader()
        writer.writerows(comparison_rows)

    error_rows = []
    for (category, segment), metrics in sorted(groups.items()):
        rendered = metrics.render()
        error_rows.append({"category": category, "segment": segment, **rendered})
    with (output_dir / "error_breakdown.csv").open("w", encoding="utf-8", newline="") as stream:
        fields = list(error_rows[0])
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        writer.writerows(error_rows)

    coverage_result = coverage.render()
    report_path = output_dir / "primitive_coverage_report.md"
    report = report_path.read_text(encoding="utf-8")
    marker = "\n## 公开请求实测覆盖\n"
    report = report.split(marker)[0]
    report += marker + f"""
- 请求数：{coverage_result['rows']}
- 双原语且余数 ≤ 12（与最终择优解耦）：{coverage_result['dual_small_remainder_pct']:.6f}%
- 任意原语且余数 ≤ 12（与最终择优解耦）：{coverage_result['any_small_remainder_pct']:.6f}%
- 位移分解失败或余数 > 12：{coverage_result['failure_pct']:.6f}%
- 最终被选中的候选含原语且余数 ≤ 12：{coverage_result['selected_small_remainder_pct']:.6f}%
- 候选类型：{coverage_result['candidate_types']}
"""
    report_path.write_text(report, encoding="utf-8")

    result = {
        "model_source": model["source"],
        "rows": full["P1_primitive_base"].rows,
        "prediction_seconds_python_reference": elapsed,
        "metrics": {name: metrics.render() for name, metrics in full.items()},
        "validation_metrics": {name: metrics.render() for name, metrics in validation.items()},
        "coverage": coverage_result,
        "prediction_path": str(prediction_path),
    }
    (output_dir / "first_stage_metrics.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


class CounterLike:
    def __init__(self) -> None:
        self.rows = 0
        self.dual_small = 0
        self.any_small = 0
        self.selected_small = 0
        self.kinds: dict[str, int] = {}

    def add(self, candidate: Candidate, coverage: tuple[bool, bool, int]) -> None:
        self.rows += 1
        dual_small, any_small, _ = coverage
        self.dual_small += dual_small
        self.any_small += any_small
        self.selected_small += candidate.remainder <= 12 and candidate.primitive_count > 0
        self.kinds[candidate.kind] = self.kinds.get(candidate.kind, 0) + 1

    def render(self) -> dict[str, object]:
        denominator = max(self.rows, 1)
        return {
            "rows": self.rows,
            "dual_small_remainder_pct": 100.0 * self.dual_small / denominator,
            "any_small_remainder_pct": 100.0 * self.any_small / denominator,
            "failure_pct": 100.0 * (self.rows - self.any_small) / denominator,
            "selected_small_remainder_pct": 100.0 * self.selected_small / denominator,
            "candidate_types": dict(sorted(self.kinds.items())),
        }


if __name__ == "__main__":
    raise SystemExit(main())
