#!/usr/bin/env python3
"""Price 71/72 and single-Block requests with exact axis/Portal structures.

This is an offline oracle and ablation, not an online search.  It uses only
architecture-derived state tables, endpoint-local connectors and concrete
candidate segments.  Gap and crossable-Block costs are charged on those
segments; a segment through a non-crossable Block is rejected.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import sys
from array import array
from collections import defaultdict
from dataclasses import dataclass
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from srb_hybrid_v9.tools.analyze_axis_oracle import (
    INF,
    STATE_COUNT,
    build_axis_table,
    source_candidates,
    target_candidates,
)


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")


def endpoint(text: str) -> tuple[int, int, str]:
    match = ENDPOINT.fullmatch(text)
    if not match:
        raise ValueError(f"bad endpoint: {text}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def point_score(golden: int, predicted: int) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


@dataclass
class Metric:
    rows: int = 0
    candidate_rows: int = 0
    baseline_score: float = 0.0
    candidate_score: float = 0.0
    hybrid_score: float = 0.0
    baseline_abs: int = 0
    candidate_abs: int = 0
    exact: int = 0
    under: int = 0

    def add(self, golden: int, baseline: int, candidate: int | None) -> None:
        self.rows += 1
        self.baseline_score += point_score(golden, baseline)
        self.hybrid_score += point_score(golden, baseline)
        self.baseline_abs += abs(baseline - golden)
        if candidate is None:
            return
        self.candidate_rows += 1
        self.candidate_score += point_score(golden, candidate)
        self.hybrid_score += point_score(golden, candidate) - point_score(golden, baseline)
        self.candidate_abs += abs(candidate - golden)
        self.exact += candidate == golden
        self.under += candidate < golden

    def result(self) -> dict[str, float | int]:
        baseline_accuracy = 100.0 * self.baseline_score / max(self.rows, 1)
        hybrid_accuracy = 100.0 * self.hybrid_score / max(self.rows, 1)
        return {
            "rows": self.rows,
            "candidate_rows": self.candidate_rows,
            "coverage_pct": 100.0 * self.candidate_rows / max(self.rows, 1),
            "baseline_accuracy": baseline_accuracy,
            "candidate_accuracy_on_covered": 100.0 * self.candidate_score / max(self.candidate_rows, 1),
            "hybrid_accuracy": hybrid_accuracy,
            "hybrid_gain": hybrid_accuracy - baseline_accuracy,
            "baseline_mae": self.baseline_abs / max(self.rows, 1),
            "candidate_mae_on_covered": self.candidate_abs / max(self.candidate_rows, 1),
            "candidate_exact_pct": 100.0 * self.exact / max(self.candidate_rows, 1),
            "candidate_under_rows": self.under,
        }


class Architecture:
    def __init__(self, path: Path) -> None:
        root = json.loads(path.read_text(encoding="utf-8-sig"))["Gap"]
        self.lines = root["Line"]
        self.blocks = root["Block"]

    @staticmethod
    def between(site: int, first: int, second: int) -> bool:
        return min(first, second) <= site < max(first, second)

    def segment_extra(
        self,
        first: tuple[int, int],
        second: tuple[int, int],
        reject_crossable_block: bool = False,
    ) -> int | None:
        x, y = first
        nx, ny = second
        if (x == nx) == (y == ny):
            return 0 if first == second else None
        horizontal = y == ny
        lo, hi = sorted((x, nx) if horizontal else (y, ny))
        extra = 0
        for line in self.lines:
            if horizontal and line["direction"] == "vertical" and lo <= int(line["site"]) < hi:
                extra += int(line["delay"])
            elif not horizontal and line["direction"] == "horizontal" and lo <= int(line["site"]) < hi:
                extra += int(line["delay"])
        for block in self.blocks:
            if horizontal:
                same_lane = int(block["lower"]) <= y <= int(block["upper"])
                intersects = max(lo + 1, int(block["left"])) <= min(hi, int(block["right"]))
                if not same_lane or not intersects:
                    continue
                if not bool(block["horizontal crossable"]):
                    return None
                if reject_crossable_block:
                    # In the official graph a crossable Block's missing SRBs do
                    # not consume Net displacement.  The obstacle-free AXS9
                    # displacement is therefore not a valid core for this
                    # segment; leave it to an exact Portal/P10 transfer.
                    return None
                extra += int(block["horizontal cross delay"])
            else:
                same_lane = int(block["left"]) <= x <= int(block["right"])
                intersects = max(lo + 1, int(block["lower"])) <= min(hi, int(block["upper"]))
                if not same_lane or not intersects:
                    continue
                if not bool(block["vertical crossable"]):
                    return None
                if reject_crossable_block:
                    return None
                extra += int(block["vertical cross delay"])
        return extra

    def path_extra(
        self, points: list[tuple[int, int]], reject_crossable_block: bool = False,
    ) -> int | None:
        total = 0
        for first, second in zip(points, points[1:]):
            value = self.segment_extra(first, second, reject_crossable_block)
            if value is None:
                return None
            total += value
        return total

    def bbox_periodic_blocks(self, source: tuple[int, int], target: tuple[int, int]) -> list[dict]:
        sx, sy = source
        tx, ty = target
        min_x, max_x = sorted((sx, tx))
        min_y, max_y = sorted((sy, ty))
        return [
            block for index, block in enumerate(self.blocks) if index > 0
            and min_x <= int(block["right"]) and max_x >= int(block["left"])
            and min_y <= int(block["upper"]) and max_y >= int(block["lower"])
        ]


class AxisPricer:
    def __init__(self, transitions: Path, radius: int, margin: int) -> None:
        self.radius = radius
        self.horizontal = build_axis_table(transitions, "H", radius, radius + margin)
        self.vertical = build_axis_table(transitions, "V", radius, radius + margin)

    @lru_cache(maxsize=None)
    def reachable(self, axis: str, source: int, delta: int) -> tuple[tuple[int, int], ...]:
        if abs(delta) > self.radius:
            return ()
        table = self.horizontal if axis == "H" else self.vertical
        begin = (source * (2 * self.radius + 1) + delta + self.radius) * STATE_COUNT
        return tuple(
            (target, int(table[begin + target]))
            for target in range(STATE_COUNT) if table[begin + target] != INF
        )

    def chain(self, source: int, target: int, runs: tuple[tuple[str, int], ...]) -> int:
        return self.chain_to_targets(source, {target: 0}, runs)

    def chain_to_targets(
        self, source: int, targets: dict[int, int], runs: tuple[tuple[str, int], ...],
    ) -> int:
        costs = {source: 0}
        for axis, delta in runs:
            if delta == 0:
                continue
            following: dict[int, int] = {}
            for state, cost in costs.items():
                for next_state, edge in self.reachable(axis, state, delta):
                    value = cost + edge
                    if value < following.get(next_state, INF):
                        following[next_state] = value
            costs = following
            if not costs:
                return INF
        return min((costs[state] + exit_cost for state, exit_cost in targets.items()
                    if state in costs), default=INF)


def load_trace(path: Path) -> dict[tuple[str, str], dict[str, str]]:
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return {(row["From"], row["To"]): row for row in csv.DictReader(stream)}


def parse_panel(value: str) -> tuple[str, Path, Path]:
    fields = value.split(":", 2)
    if len(fields) != 3:
        raise argparse.ArgumentTypeError("panel must be name:summaries.csv:trace.csv")
    return fields[0], Path(fields[1]), Path(fields[2])


def geometry_candidates(
    start: tuple[int, int], target: tuple[int, int], blocks: list[dict], portal_band: int,
) -> list[tuple[str, list[tuple[int, int]], tuple[tuple[str, int], ...]]]:
    sx, sy = start
    tx, ty = target
    result = [
        ("HV", [start, (tx, sy), target], (("H", tx - sx), ("V", ty - sy))),
        ("VH", [start, (sx, ty), target], (("V", ty - sy), ("H", tx - sx))),
    ]
    if sy == ty:
        result.append(("H", [start, target], (("H", tx - sx),)))
    if sx == tx:
        result.append(("V", [start, target], (("V", ty - sy),)))
    for block in blocks:
        portal_xs = list(range(int(block["left"]) - portal_band, int(block["left"])))
        portal_xs += list(range(int(block["right"]) + 1, int(block["right"]) + portal_band + 1))
        for portal_x in portal_xs:
            result.append((
                "HVH",
                [start, (portal_x, sy), (portal_x, ty), target],
                (("H", portal_x - sx), ("V", ty - sy), ("H", tx - portal_x)),
            ))
    return result


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--transitions", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--panel", type=parse_panel, action="append", required=True)
    parser.add_argument("--radius", type=int, default=128)
    parser.add_argument("--search-margin", type=int, default=64)
    parser.add_argument("--portal-band", type=int, default=6)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    architecture = Architecture(args.gap)
    pricer = AxisPricer(args.transitions, args.radius, args.search_margin)
    metrics: dict[str, dict[str, Metric]] = defaultdict(lambda: defaultdict(Metric))
    skeletons: dict[str, dict[str, int]] = defaultdict(lambda: defaultdict(int))

    for split, summaries_path, trace_path in args.panel:
        trace = load_trace(trace_path)
        with summaries_path.open(encoding="utf-8-sig", newline="") as stream:
            for row in csv.DictReader(stream):
                if row["Reachable"] != "1":
                    continue
                key = row["From"], row["To"]
                baseline_row = trace.get(key)
                if baseline_row is None:
                    continue
                sx, sy, source_name = endpoint(row["From"])
                tx, ty, target_name = endpoint(row["To"])
                distance = max(abs(tx - sx), abs(ty - sy))
                blocks = architecture.bbox_periodic_blocks((sx, sy), (tx, ty))
                groups = []
                if distance in (71, 72):
                    groups += ["boundary_71_72", f"distance_{distance}"]
                    if not blocks:
                        groups.append("boundary_no_block")
                if len(blocks) == 1:
                    groups.append("single_block_bbox")
                if int(row["BlockSegments"]) > 0:
                    groups.append("actual_block_segment")
                if distance in (71, 72) or len(blocks) == 1:
                    groups.append("replacement_target")
                if not groups or distance > args.radius:
                    continue

                best = INF
                selected = "none"
                target_costs: dict[int, int] = {}
                for target_state, exit_cost in target_candidates(model, port_id[target_name]):
                    target_costs[target_state] = min(
                        target_costs.get(target_state, INF), exit_cost
                    )
                for source_state, entry_dx, entry_dy, entry_cost in source_candidates(
                    model, port_id[source_name]
                ):
                    start = sx + entry_dx, sy + entry_dy
                    entry_extra = architecture.segment_extra(
                        (sx, sy), start, reject_crossable_block=True
                    )
                    if entry_extra is None:
                        continue
                    for name, points, runs in geometry_candidates(
                        start, (tx, ty), blocks, args.portal_band
                    ):
                        extra = architecture.path_extra(points, reject_crossable_block=True)
                        if extra is None:
                            continue
                        core = pricer.chain_to_targets(source_state, target_costs, runs)
                        if core == INF:
                            continue
                        value = entry_cost + entry_extra + core + extra
                        if value < best:
                            best, selected = value, name
                candidate = None if best == INF else best
                golden = int(row["Delay"])
                baseline = int(baseline_row["Delay"])
                for group in groups:
                    metrics[split][group].add(golden, baseline, candidate)
                    skeletons[split][selected] += candidate is not None

    report = {
        "schema": 1,
        "kind": "exact-axis-plus-single-block-portal-offline-oracle",
        "request_identity_stored": False,
        "online_search": False,
        "radius": args.radius,
        "search_margin": args.search_margin,
        "portal_band": args.portal_band,
        "candidate_policy": (
            "H/V/HV/VH plus H-V-H at the configured columns on each side of one periodic Block"
        ),
        "safety_policy": (
            "reject an AXS9 segment that intersects a crossable Block because official Net displacement "
            "skips missing Block sites; such segments require an exact Portal/P10 transfer"
        ),
        "panels": {
            split: {group: value.result() for group, value in sorted(groups.items())}
            for split, groups in sorted(metrics.items())
        },
        "selected_skeletons": {split: dict(values) for split, values in skeletons.items()},
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if not any(
        metric.under for groups in metrics.values() for metric in groups.values()
    ) else 2


if __name__ == "__main__":
    raise SystemExit(main())
