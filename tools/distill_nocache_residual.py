#!/usr/bin/env python3
"""Distil K48 residuals into two fixed-index int8 tables.

This is deliberately not a request cache: neither endpoint identity nor a
request hash is stored.  A query performs one dense fine-table read and, when
that cell is unsupported, one dense coarse-table read.  Training, model
selection and the final gate use disjoint source/region panels.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import struct
from dataclasses import dataclass
from pathlib import Path


MAGIC = b"RSDV1001"
BRANCHES = (
    "identity", "unreachable", "local_r24", "safe_hull", "long_hull",
    "short48_or_parent", "short_teacher", "long_dual", "short_dual",
    "normalized_fallback",
)
BRANCH_ID = {name: index for index, name in enumerate(BRANCHES)}
DISTANCES = 10
QUADRANTS = 9
ENVIRONMENTS = 4
X_PHASES = 8
Y_PHASES = 12
SENTINEL = -128


@dataclass(frozen=True)
class Row:
    source: str
    target: str
    golden: int
    base: int
    branch: int
    dx: int
    dy: int
    gap: int
    block_mask: int


def delay_column(fields: list[str] | None, prefer_base: bool = False) -> str:
    names = fields or []
    if prefer_base and "BaseDelay" in names:
        return "BaseDelay"
    for name in names:
        if name.lower() in {"delay", "min delay", "predict_delay", "golden_delay_ps"}:
            return name
    raise ValueError(f"no delay column in {fields}")


def read_panel(golden_path: Path, trace_path: Path) -> list[Row]:
    result: list[Row] = []
    with golden_path.open(encoding="utf-8-sig", newline="") as golden_file, trace_path.open(
        encoding="utf-8-sig", newline=""
    ) as trace_file:
        golden_reader, trace_reader = csv.DictReader(golden_file), csv.DictReader(trace_file)
        gd = delay_column(golden_reader.fieldnames)
        td = delay_column(trace_reader.fieldnames, prefer_base=True)
        while True:
            golden, trace = next(golden_reader, None), next(trace_reader, None)
            if golden is None or trace is None:
                if golden is not None or trace is not None:
                    raise ValueError("Golden/trace row count mismatch")
                break
            pair = golden["From"], golden["To"]
            if pair != (trace["From"], trace["To"]):
                raise ValueError(f"Golden/trace alignment mismatch: {pair}")
            result.append(Row(
                pair[0], pair[1], int(golden[gd]), int(trace[td]),
                BRANCH_ID.get(trace["Branch"], len(BRANCHES) - 1),
                int(trace["Dx"]), int(trace["Dy"]), int(trace["Gap"]),
                int(trace.get("BlockMask", "0") or 0),
            ))
    return result


def distance_bucket(dx: int, dy: int) -> int:
    distance = max(abs(dx), abs(dy))
    if distance <= 16: return 0
    if distance <= 23: return 1
    if distance == 24: return 2
    if distance <= 26: return 3
    if distance <= 48: return 4
    if distance <= 71: return 5
    if distance == 72: return 6
    if distance <= 128: return 7
    if distance <= 256: return 8
    return 9


def quadrant(dx: int, dy: int) -> int:
    return (1 if dx > 0 else 2 if dx < 0 else 0) + (3 if dy > 0 else 6 if dy < 0 else 0)


def environment(row: Row) -> int:
    return (1 if row.gap else 0) | (2 if row.block_mask else 0)


def coarse_index(row: Row) -> int:
    return (((row.branch * DISTANCES + distance_bucket(row.dx, row.dy)) * QUADRANTS +
             quadrant(row.dx, row.dy)) * ENVIRONMENTS + environment(row))


def fine_index(row: Row) -> int:
    return ((coarse_index(row) * X_PHASES + abs(row.dx) % X_PHASES) * Y_PHASES +
            abs(row.dy) % Y_PHASES)


COARSE_SIZE = len(BRANCHES) * DISTANCES * QUADRANTS * ENVIRONMENTS
FINE_SIZE = COARSE_SIZE * X_PHASES * Y_PHASES


def accuracy(rows: list[Row], corrections: list[int] | None = None) -> float:
    score = 0.0
    for index, row in enumerate(rows):
        predicted = row.base if corrections is None else max(0, row.base + corrections[index])
        if row.golden == 0:
            score += float(predicted == 0)
        else:
            score += 1.0 - math.tanh(4.0 * abs(predicted - row.golden) / row.golden)
    return 100.0 * score / len(rows) if rows else math.nan


def fit_table(rows: list[Row], key, size: int, support: int, scale: int) -> list[int]:
    buckets: dict[int, list[int]] = {}
    for row in rows:
        if row.golden > 0 and row.base > 0:
            buckets.setdefault(key(row), []).append(row.golden - row.base)
    table = [SENTINEL] * size
    for index, values in buckets.items():
        if len(values) < support:
            continue
        correction = round(statistics.median(values) / scale)
        table[index] = max(-127, min(127, correction))
    return table


def apply(rows: list[Row], coarse: list[int], fine: list[int], scale: int) -> list[int]:
    result: list[int] = []
    for row in rows:
        value = fine[fine_index(row)]
        if value == SENTINEL:
            value = coarse[coarse_index(row)]
        result.append(0 if value == SENTINEL else value * scale)
    return result


def write_predictions(path: Path, rows: list[Row], corrections: list[int]) -> None:
    with path.open("w", encoding="utf-8", newline="") as output:
        writer = csv.writer(output, lineterminator="\n")
        writer.writerow(("From", "To", "Delay"))
        for row, correction in zip(rows, corrections):
            writer.writerow((row.source, row.target, max(0, row.base + correction)))


def write_asset(path: Path, scale: int, coarse: list[int], fine: list[int]) -> None:
    header = struct.pack(
        "<8s10I", MAGIC, 1, scale, len(BRANCHES), DISTANCES, QUADRANTS,
        ENVIRONMENTS, X_PHASES, Y_PHASES, len(coarse), len(fine),
    )
    path.write_bytes(header + bytes(value & 255 for value in coarse + fine))


def main() -> int:
    parser = argparse.ArgumentParser()
    for split in ("train", "source", "region"):
        parser.add_argument(f"--{split}-golden", type=Path, required=True)
        parser.add_argument(f"--{split}-trace", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    panels = {
        name: read_panel(getattr(args, f"{name}_golden"), getattr(args, f"{name}_trace"))
        for name in ("train", "source", "region")
    }
    baseline = {name: accuracy(rows) for name, rows in panels.items()}
    candidates: list[dict[str, object]] = []
    payloads: dict[tuple[int, int, int], tuple[list[int], list[int]]] = {}
    for scale in (1, 2, 4, 8):
        for coarse_support in (8, 16, 32, 64):
            coarse = fit_table(panels["train"], coarse_index, COARSE_SIZE, coarse_support, scale)
            for fine_support in (4, 8, 16, 32):
                fine = fit_table(panels["train"], fine_index, FINE_SIZE, fine_support, scale)
                scores: dict[str, float] = {}
                gains: dict[str, float] = {}
                nonzero: dict[str, int] = {}
                for name, rows in panels.items():
                    correction = apply(rows, coarse, fine, scale)
                    scores[name] = accuracy(rows, correction)
                    gains[name] = scores[name] - baseline[name]
                    nonzero[name] = sum(value != 0 for value in correction)
                candidate = {
                    "scale_ps": scale, "coarse_support": coarse_support,
                    "fine_support": fine_support, "scores": scores, "gains": gains,
                    "corrected_rows": nonzero,
                    "coarse_cells": sum(value != SENTINEL for value in coarse),
                    "fine_cells": sum(value != SENTINEL for value in fine),
                }
                candidates.append(candidate)
                payloads[(scale, coarse_support, fine_support)] = coarse, fine

    # Candidate choice is made without looking at the region score.  The region
    # score is used only as the final accept/reject gate.
    eligible = [row for row in candidates if row["gains"]["train"] > 0 and row["gains"]["source"] > 0]  # type: ignore[index]
    selected = max(eligible, key=lambda row: row["gains"]["source"], default=None)  # type: ignore[index]
    accepted = bool(selected and selected["gains"]["region"] > 0)  # type: ignore[index]
    if accepted:
        key = (selected["scale_ps"], selected["coarse_support"], selected["fine_support"])  # type: ignore[index]
        coarse, fine = payloads[key]
        write_asset(args.output_dir / "direct_residual.bin", key[0], coarse, fine)
        for name, rows in panels.items():
            write_predictions(args.output_dir / f"{name}_candidate.csv", rows, apply(rows, coarse, fine, key[0]))
    else:
        # An explicit header-only empty model is auditable and cannot change a
        # prediction.  This is preferable to smuggling a failed table forward.
        coarse, fine = [SENTINEL] * COARSE_SIZE, [SENTINEL] * FINE_SIZE
        write_asset(args.output_dir / "direct_residual.bin", 1, coarse, fine)

    report = {
        "schema": 1,
        "kind": "cache-free-fixed-index-int8-residual",
        "request_identity_stored": False,
        "lookup_count": "one fine read plus at most one coarse fallback read",
        "dimensions": {"coarse": COARSE_SIZE, "fine": FINE_SIZE},
        "baseline": baseline,
        "selection_uses_region": False,
        "accepted": accepted,
        "selected": selected,
        "asset_bytes": (args.output_dir / "direct_residual.bin").stat().st_size,
        "candidates": sorted(candidates, key=lambda row: row["gains"]["source"], reverse=True),  # type: ignore[index]
    }
    (args.output_dir / "distillation_report.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({key: report[key] for key in ("accepted", "baseline", "selected", "asset_bytes")},
                     ensure_ascii=False, indent=2))
    return 0 if accepted else 2


if __name__ == "__main__":
    raise SystemExit(main())
