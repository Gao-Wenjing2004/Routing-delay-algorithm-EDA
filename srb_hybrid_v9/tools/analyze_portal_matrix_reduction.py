#!/usr/bin/env python3
"""Measure exact min-plus state reduction opportunities in a Portal matrix."""

from __future__ import annotations

import argparse
import json
import struct
import sys
from array import array
from collections import Counter
from pathlib import Path


INF = 65535
BIG = 1 << 29


def load_matrix(path: Path) -> tuple[dict[str, int], array]:
    raw = path.read_bytes()
    if len(raw) < 20:
        raise ValueError("truncated Portal matrix")
    magic, version, dimension, band, periods, direction = struct.unpack(
        "<8sHHHHb3x", raw[:20]
    )
    if magic != b"PPORT001" or version != 1 or periods != 1:
        raise ValueError("unsupported Portal matrix")
    costs = array("H")
    costs.frombytes(raw[20:])
    if sys.byteorder != "little":
        costs.byteswap()
    if len(costs) != dimension * dimension:
        raise ValueError("bad Portal matrix body length")
    return {
        "dimension": dimension,
        "band": band,
        "direction": direction,
    }, costs


def exact_equivalence_groups(matrix: array, dimension: int) -> list[list[int]]:
    groups: dict[bytes, list[int]] = {}
    for state in range(dimension):
        row = matrix[state * dimension:(state + 1) * dimension]
        column = matrix[state::dimension]
        row_min = min(row)
        column_min = min(column)
        normalized = array("H", (value - row_min for value in row))
        normalized.extend(value - column_min for value in column)
        fingerprint = normalized.tobytes()
        groups.setdefault(fingerprint, []).append(state)
    return sorted(groups.values(), key=lambda group: (-len(group), group[0]))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--matrix", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()

    metadata, matrix = load_matrix(args.matrix)
    manifest = json.loads(args.manifest.read_text(encoding="utf-8-sig"))
    dimension = metadata["dimension"]
    if len(manifest["sources"]) != dimension:
        raise ValueError("manifest/matrix dimension mismatch")

    # Compute the exact two-period min-plus product, including tie counts.
    entries = dimension * dimension
    best = array("I", [BIG]) * entries
    tie_counts = array("H", [0]) * entries
    first_winner = array("H", [0]) * entries
    for source in range(dimension):
        source_base = source * dimension
        for target in range(dimension):
            optimum = BIG
            ties = 0
            winner = 0
            for middle in range(dimension):
                candidate = (
                    int(matrix[source_base + middle])
                    + int(matrix[middle * dimension + target])
                )
                if candidate < optimum:
                    optimum, ties, winner = candidate, 1, middle
                elif candidate == optimum:
                    ties += 1
            entry = source_base + target
            best[entry] = optimum
            tie_counts[entry] = ties
            first_winner[entry] = winner

    witness_counts = [0] * dimension
    strict_counts = [0] * dimension
    for source in range(dimension):
        source_base = source * dimension
        for target in range(dimension):
            entry = source_base + target
            for middle in range(dimension):
                if (
                    int(matrix[source_base + middle])
                    + int(matrix[middle * dimension + target])
                    == best[entry]
                ):
                    witness_counts[middle] += 1
                    if tie_counts[entry] == 1:
                        strict_counts[middle] += 1

    # Rank by the number of exact pairs witnessed.  Incrementally measure the
    # exact coverage of that deterministic state prefix.
    selected = sorted(
        range(dimension), key=lambda state: (-witness_counts[state], state)
    )
    restricted = array("I", [BIG]) * entries
    exact_mask = bytearray(entries)
    exact_count = 0
    coverage_curve = []
    for rank, middle in enumerate(selected, 1):
        gain = 0
        for source in range(dimension):
            source_base = source * dimension
            first = int(matrix[source_base + middle])
            middle_base = middle * dimension
            for target in range(dimension):
                entry = source_base + target
                candidate = first + int(matrix[middle_base + target])
                if candidate < restricted[entry]:
                    restricted[entry] = candidate
                if not exact_mask[entry] and restricted[entry] == best[entry]:
                    exact_mask[entry] = 1
                    exact_count += 1
                    gain += 1
        coverage_curve.append({
            "states": rank,
            "covered": exact_count,
            "rate": exact_count / entries,
            "chosen_state": middle,
            "gain": gain,
        })

    events = manifest["sources"]
    selected_detail = []
    for rank, state in enumerate(selected, 1):
        event = events[state]
        selected_detail.append({
            "rank": rank,
            "state_index": state,
            "x": int(event["x"]),
            "phase": int(event["phase"]),
            "route": event["route"],
            "span": int(event["span"]),
            "witness_pairs": witness_counts[state],
            "strict_pairs": strict_counts[state],
        })

    checkpoints = {}
    for count in (1, 2, 4, 8, 16, 24, 32, 40, 64, 80, 128, 160, 240, 320):
        if count <= len(coverage_curve):
            checkpoints[str(count)] = coverage_curve[count - 1]["rate"]
    groups = exact_equivalence_groups(matrix, dimension)
    report = {
        **metadata,
        "entries": entries,
        "states_with_any_witness": int(sum(value > 0 for value in witness_counts)),
        "states_with_strict_witness": int(sum(value > 0 for value in strict_counts)),
        "witness_ranked_exact_cover_states": next(
            (item["states"] for item in coverage_curve if item["covered"] == entries),
            len(selected),
        ),
        "witness_ranked_coverage_checkpoints": checkpoints,
        "winner_x_counts": dict(sorted(Counter(
            int(events[state]["x"]) for state in selected
        ).items())),
        "additive_equivalence_groups": len(groups),
        "largest_additive_equivalence_group": len(groups[0]),
        "nontrivial_additive_equivalence_groups": [
            group for group in groups if len(group) > 1
        ],
        "selected_states": selected_detail,
        "coverage_curve": coverage_curve,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps({
        key: report[key] for key in (
            "dimension", "states_with_any_witness", "states_with_strict_witness",
            "witness_ranked_exact_cover_states", "witness_ranked_coverage_checkpoints",
            "additive_equivalence_groups", "largest_additive_equivalence_group",
        )
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
