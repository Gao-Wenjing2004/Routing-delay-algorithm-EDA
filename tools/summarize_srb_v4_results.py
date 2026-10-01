#!/usr/bin/env python3
"""Create the compact, score-aware V4 validation and selection artifacts."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path
from typing import Any

from srb_score import total_score


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Summarize final SRB V4 evidence")
    parser.add_argument("--validation", required=True, type=Path)
    parser.add_argument("--compact-final-benchmark", required=True, type=Path)
    parser.add_argument("--compact-5m-benchmark", required=True, type=Path)
    parser.add_argument("--port-pair-final-benchmark", required=True, type=Path)
    parser.add_argument("--port-pair-5m-benchmark", required=True, type=Path)
    parser.add_argument("--v3-score", required=True, type=Path)
    parser.add_argument("--v4-score", required=True, type=Path)
    parser.add_argument("--out-csv", required=True, type=Path)
    parser.add_argument("--out-json", required=True, type=Path)
    return parser.parse_args()


def load_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def projection(benchmark: dict[str, Any], case: str) -> float:
    return float(benchmark["cases"][case]["summary"]["projected_100m_seconds_linear"])


def candidate_accuracy_summary(validation: dict[str, Any], candidate: str) -> dict[str, float]:
    deltas = [
        float(split["candidates"][candidate]["delta_acc_score"])
        for split in validation["splits"].values()
    ]
    return {"mean": sum(deltas) / len(deltas), "min": min(deltas), "max": max(deltas)}


def candidate_timing_evidence(
    final_benchmark: dict[str, Any],
    benchmark_5m: dict[str, Any],
) -> dict[str, Any]:
    differences = {
        "100k_projection_delta_seconds": projection(final_benchmark, "v4_100000")
        - projection(final_benchmark, "v3_100000"),
        "1m_projection_delta_seconds": projection(final_benchmark, "v4_1000000")
        - projection(final_benchmark, "v3_1000000"),
        "5m_repeated_projection_delta_seconds": projection(benchmark_5m, "v4_5000000")
        - projection(benchmark_5m, "v3_5000000"),
    }
    differences["conservative_observed_delta_seconds"] = max(differences.values())
    return differences


def main() -> int:
    args = parse_args()
    validation = load_json(args.validation)
    compact_final = load_json(args.compact_final_benchmark)
    compact_5m = load_json(args.compact_5m_benchmark)
    port_pair_final = load_json(args.port_pair_final_benchmark)
    port_pair_5m = load_json(args.port_pair_5m_benchmark)
    v3_score = load_json(args.v3_score)
    v4_score = load_json(args.v4_score)

    # The 5M repeated-input run has a longer measurement interval and is used
    # for the central 100M estimate.  It is still a projection, not an official run.
    v3_seconds = projection(compact_5m, "v3_5000000")
    v4_seconds = projection(compact_5m, "v4_5000000")
    rows: list[dict[str, Any]] = []
    candidate = "single_residual"
    for split_name, split in validation["splits"].items():
        base = split["v3"]["overall"]
        selected = split["candidates"][candidate]["metrics"]["overall"]
        base_total = total_score(float(base["acc_score"]), v3_seconds, True)
        selected_total = total_score(float(selected["acc_score"]), v4_seconds, True)
        rows.append(
            {
                "split": split_name,
                "validation_rows": split["validation_rows"],
                "v3_acc_score": base["acc_score"],
                "v4_acc_score": selected["acc_score"],
                "delta_acc_score": float(selected["acc_score"]) - float(base["acc_score"]),
                "v3_estimated_total_score": base_total["estimated_total_score"],
                "v4_estimated_total_score": selected_total["estimated_total_score"],
                "delta_estimated_total_score": float(selected_total["estimated_total_score"])
                - float(base_total["estimated_total_score"]),
                "v3_mae": base["mae"],
                "v4_mae": selected["mae"],
                "v3_rmse": base["rmse"],
                "v4_rmse": selected["rmse"],
                "v4_mean_relative_error": selected["mean_relative_error"],
                "v4_p50_absolute_error": selected["p50_absolute_error"],
                "v4_p90_absolute_error": selected["p90_absolute_error"],
                "v4_p95_absolute_error": selected["p95_absolute_error"],
                "v4_p99_absolute_error": selected["p99_absolute_error"],
                "v4_max_absolute_error": selected["max_absolute_error"],
            }
        )

    compact_accuracy = candidate_accuracy_summary(validation, "single_residual")
    port_pair_accuracy = candidate_accuracy_summary(validation, "port_pair_residual")
    compact_timing = candidate_timing_evidence(compact_final, compact_5m)
    port_pair_timing = candidate_timing_evidence(port_pair_final, port_pair_5m)

    def conservative_total_delta(accuracy: dict[str, float], timing: dict[str, Any]) -> float:
        # For a difference only, the consistency terms cancel and the official
        # time term changes by -0.15 * 100 * delta_seconds / 1800.
        return 0.8 * accuracy["min"] - float(timing["conservative_observed_delta_seconds"]) / 120.0

    selection = {
        "selected": "single_residual",
        "selection_basis": (
            "positive estimated total-score delta on all seven held-out splits under the "
            "largest observed 100M timing overhead; smaller model wins over unstable richer model"
        ),
        "central_timing_source": "5x 5M repeated public queries; diagnostic linear projection",
        "central_projected_100m_seconds": {"v3": v3_seconds, "v4": v4_seconds},
        "public_cpp_estimated_score": {
            "v3": v3_score["score"],
            "v4": v4_score["score"],
            "delta_total_score": float(v4_score["score"]["estimated_total_score"])
            - float(v3_score["score"]["estimated_total_score"]),
            "accuracy_scope": (
                "public all rows; new residual fit uses 800,304 fixed training rows and "
                "holds out 199,696 rows; inherited V3 base was previously fit on all public rows"
            ),
        },
        "candidates": {
            "single_residual": {
                "parameter_count": 2784,
                "accuracy_delta_across_splits": compact_accuracy,
                "timing": compact_timing,
                "worst_split_conservative_estimated_total_delta": conservative_total_delta(
                    compact_accuracy, compact_timing
                ),
                "decision": "production default",
            },
            "port_pair_residual": {
                "parameter_count": 408512,
                "accuracy_delta_across_splits": port_pair_accuracy,
                "timing": port_pair_timing,
                "worst_split_conservative_estimated_total_delta": conservative_total_delta(
                    port_pair_accuracy, port_pair_timing
                ),
                "decision": "research-only; timing evidence is not stably score-positive",
            },
        },
        "validation_estimated_total": {
            "minimum_delta": min(float(row["delta_estimated_total_score"]) for row in rows),
            "maximum_delta": max(float(row["delta_estimated_total_score"]) for row in rows),
            "all_positive": all(float(row["delta_estimated_total_score"]) > 0 for row in rows),
            "timing_assumption": {
                "v3_projected_100m_seconds": v3_seconds,
                "v4_projected_100m_seconds": v4_seconds,
            },
        },
    }

    args.out_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.out_csv.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    args.out_json.parent.mkdir(parents=True, exist_ok=True)
    args.out_json.write_text(
        json.dumps(selection, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(selection, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
