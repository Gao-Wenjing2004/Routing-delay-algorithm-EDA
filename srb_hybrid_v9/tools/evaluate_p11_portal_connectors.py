#!/usr/bin/env python3
"""Generate and score label-free P11 local Portal connector candidates.

The structured pricer sees only the query endpoints and each legal Portal event.
EntryIndex/ExitIndex are used after pricing solely for rank diagnostics, never to
choose a prediction.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import struct
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import BinaryIO, Iterable


BEAMS = (1, 2, 4, 8, 16, 32)
BIG = 1_000_000_000


def endpoint_y(specification: str) -> int:
    instance = specification.split("/", 1)[0]
    return int(instance.rsplit("_", 1)[1])


def event_endpoint(event: dict, boundary: int, direction: int) -> str:
    y = boundary + direction * int(event["phase"])
    return f'SRB_{event["x"]}_{y}/{event["route"]}'


def boundaries(row: dict[str, str]) -> tuple[int, int]:
    direction = int(row["Direction"])
    source_y = endpoint_y(row["From"])
    target_y = endpoint_y(row["To"])
    if direction > 0:
        source = ((source_y - 49 + 99) // 100) * 100 + 49
        target = ((target_y - 50) // 100) * 100 + 49
    else:
        source = (source_y // 100) * 100
        target = (target_y // 100 + 1) * 100
    if abs(target - source) // 100 != int(row["Periods"]):
        raise ValueError(f"inferred boundaries disagree with periods: {row}")
    return source, target


def load_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def load_events(up_manifest: Path, down_manifest: Path) -> dict[int, list[dict]]:
    with up_manifest.open(encoding="utf-8") as stream:
        up = json.load(stream)["sources"]
    with down_manifest.open(encoding="utf-8") as stream:
        down = json.load(stream)["sources"]
    if len(up) != 1920 or len(down) != 1920:
        raise ValueError("P11 requires the band=6, dimension=1920 manifests")
    return {1: up, -1: down}


def generate(args: argparse.Namespace) -> None:
    rows = load_rows(args.decomposition)
    events = load_events(args.up_manifest, args.down_manifest)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    count = 0
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("From", "To", "Golden", "Candidates"))
        for row_index, row in enumerate(rows):
            direction = int(row["Direction"])
            source_boundary, target_boundary = boundaries(row)
            row_events = events[direction]
            for event in row_events:
                writer.writerow((row["From"], event_endpoint(event, source_boundary, direction), 0, ""))
                count += 1
            for event in row_events:
                writer.writerow((event_endpoint(event, target_boundary, direction), row["To"], 0, ""))
                count += 1
            if (row_index + 1) % 10 == 0 or row_index + 1 == len(rows):
                print(f"generated={row_index + 1}/{len(rows)} requests={count}")
    print(json.dumps({"teacher_rows": len(rows), "request_rows": count,
                      "output": str(args.output)}, ensure_ascii=False, indent=2))


class PackedClosure:
    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        if len(self.data) < 20 or self.data[:8] != b"PPC10B01":
            raise ValueError(f"bad packed closure: {path}")
        version, self.dimension, self.band, self.periods = struct.unpack_from("<4H", self.data, 8)
        self.direction = struct.unpack_from("<b", self.data, 16)[0]
        if version != 1:
            raise ValueError("unsupported packed closure")
        self.packed_bytes = (self.dimension * self.dimension * 10 + 7) // 8 + 2
        self.period_bytes = self.dimension * 4 + self.packed_bytes
        if len(self.data) != 20 + self.period_bytes * self.periods:
            raise ValueError("packed closure size mismatch")

    def lookup(self, period: int, source: int, target: int) -> int:
        base = 20 + (period - 1) * self.period_bytes
        row = struct.unpack_from("<H", self.data, base + source * 2)[0]
        column = struct.unpack_from("<H", self.data, base + (self.dimension + target) * 2)[0]
        packed = base + self.dimension * 4
        bit = (source * self.dimension + target) * 10
        byte, shift = divmod(bit, 8)
        window = int.from_bytes(self.data[packed + byte:packed + byte + 3], "little")
        return row + column + ((window >> shift) & 1023)


@dataclass
class Metric:
    reachable: int = 0
    exact: int = 0
    under: int = 0
    score: float = 0.0
    absolute_error: float = 0.0


def priced_rows(path: Path) -> Iterable[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        yield from csv.DictReader(stream)


def read_costs(iterator: Iterable[dict[str, str]], count: int) -> tuple[list[tuple[int, int]], float, int]:
    result: list[tuple[int, int]] = []
    elapsed = 0.0
    transitions = 0
    for event_index in range(count):
        try:
            row = next(iterator)  # type: ignore[arg-type]
        except StopIteration as error:
            raise ValueError("priced connector CSV ended early") from error
        cost = int(row["Predicted"])
        if cost >= 0:
            result.append((cost, event_index))
        elapsed += float(row["ElapsedUs"])
        transitions += int(row["Transitions"])
    result.sort()
    return result, elapsed, transitions


def rank_of(ranked: list[tuple[int, int]], event: int) -> int:
    return next((index for index, (_, candidate) in enumerate(ranked, 1)
                 if candidate == event), 0)


def predicted_at(ranked: list[tuple[int, int]], event: int) -> int:
    return next((cost for cost, candidate in ranked if candidate == event), -1)


def analyze(args: argparse.Namespace) -> None:
    rows = load_rows(args.decomposition)
    closures = {1: PackedClosure(args.up_closure), -1: PackedClosure(args.down_closure)}
    if closures[1].direction != 1 or closures[-1].direction != -1:
        raise ValueError("closure directions are reversed")
    dimension = closures[1].dimension
    if dimension != 1920 or closures[-1].dimension != dimension:
        raise ValueError("P11 requires matching band=6 closures")
    iterator = iter(priced_rows(args.priced))
    metrics = {beam: Metric() for beam in BEAMS}
    source_rank_bands: Counter[str] = Counter()
    target_rank_bands: Counter[str] = Counter()
    total_elapsed = 0.0
    total_transitions = 0
    output_rows: list[dict[str, object]] = []

    def rank_band(rank: int) -> str:
        if rank == 0:
            return "unreachable"
        for beam in BEAMS:
            if rank <= beam:
                return f"top{beam}"
        return "33+"

    for row_index, row in enumerate(rows):
        source, source_us, source_transitions = read_costs(iterator, dimension)
        target, target_us, target_transitions = read_costs(iterator, dimension)
        total_elapsed += source_us + target_us
        total_transitions += source_transitions + target_transitions
        direction = int(row["Direction"])
        period = int(row["Periods"])
        golden = int(row["Golden"])
        true_source = int(row["EntryIndex"])
        true_target = int(row["ExitIndex"])
        source_rank = rank_of(source, true_source)
        target_rank = rank_of(target, true_target)
        source_rank_bands[rank_band(source_rank)] += 1
        target_rank_bands[rank_band(target_rank)] += 1
        predictions: dict[int, int] = {}
        closure = closures[direction]
        for beam in BEAMS:
            best = BIG
            for source_cost, source_event in source[:beam]:
                for target_cost, target_event in target[:beam]:
                    best = min(best, source_cost + closure.lookup(
                        period, source_event, target_event) + target_cost)
            prediction = -1 if best == BIG else best
            predictions[beam] = prediction
            if prediction >= 0:
                metric = metrics[beam]
                metric.reachable += 1
                metric.exact += prediction == golden
                metric.under += prediction < golden
                error = abs(prediction - golden)
                metric.absolute_error += error
                metric.score += 1.0 - math.tanh(4.0 * error / golden) if golden else float(prediction == 0)
        output = {
            "From": row["From"], "To": row["To"], "Golden": golden,
            "Direction": direction, "Periods": period,
            "SourceReachable": len(source), "TargetReachable": len(target),
            "TrueSourceRank": source_rank, "TrueTargetRank": target_rank,
            "OracleSourceLocal": int(row["SourceLocal"]),
            "PredictedTrueSourceLocal": predicted_at(source, true_source),
            "OracleTargetLocal": int(row["TargetLocal"]),
            "PredictedTrueTargetLocal": predicted_at(target, true_target),
            **{f"Pred{beam}": predictions[beam] for beam in BEAMS},
            "Transitions": source_transitions + target_transitions,
            "ConnectorUs": source_us + target_us,
        }
        output_rows.append(output)
        if (row_index + 1) % 10 == 0 or row_index + 1 == len(rows):
            print(f"analyzed={row_index + 1}/{len(rows)}")
    try:
        next(iterator)
    except StopIteration:
        pass
    else:
        raise ValueError("priced connector CSV has extra rows")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(output_rows[0]))
        writer.writeheader()
        writer.writerows(output_rows)
    report = {
        "teacher_rows": len(rows),
        "dimension_per_side": dimension,
        "label_free_selection": True,
        "true_indices_used_for_prediction": False,
        "source_true_event_rank_bands": dict(source_rank_bands),
        "target_true_event_rank_bands": dict(target_rank_bands),
        "mean_exhaustive_connector_us": total_elapsed / len(rows),
        "mean_transitions": total_transitions / len(rows),
        "beams": {
            str(beam): {
                "reachable": metrics[beam].reachable,
                "exact": metrics[beam].exact,
                "under": metrics[beam].under,
                "accuracy": 100.0 * metrics[beam].score / len(rows),
                "mae": metrics[beam].absolute_error / len(rows),
            } for beam in BEAMS
        },
    }
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser()
    subparsers = result.add_subparsers(dest="command", required=True)
    generate_parser = subparsers.add_parser("generate")
    generate_parser.add_argument("--decomposition", type=Path, required=True)
    generate_parser.add_argument("--up-manifest", type=Path, required=True)
    generate_parser.add_argument("--down-manifest", type=Path, required=True)
    generate_parser.add_argument("--output", type=Path, required=True)
    generate_parser.set_defaults(function=generate)
    analyze_parser = subparsers.add_parser("analyze")
    analyze_parser.add_argument("--decomposition", type=Path, required=True)
    analyze_parser.add_argument("--priced", type=Path, required=True)
    analyze_parser.add_argument("--up-closure", type=Path, required=True)
    analyze_parser.add_argument("--down-closure", type=Path, required=True)
    analyze_parser.add_argument("--output", type=Path, required=True)
    analyze_parser.add_argument("--report", type=Path, required=True)
    analyze_parser.set_defaults(function=analyze)
    return result


def main() -> None:
    args = parser().parse_args()
    args.function(args)


if __name__ == "__main__":
    main()
