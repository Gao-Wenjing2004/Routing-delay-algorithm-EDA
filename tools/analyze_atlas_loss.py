#!/usr/bin/env python3
"""Decompose V3 Atlas score contribution and test reproducible fallback gates.

This is an offline diagnostic.  It never edits production V3/V4 artifacts.  A
gate selected with public Golden data is reported as learned/experimental, not
as architecture-proved safe reuse.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
from pathlib import Path
from typing import Any

import numpy as np

THIS_DIR = Path(__file__).resolve().parent
if str(THIS_DIR) not in sys.path:
    sys.path.insert(0, str(THIS_DIR))

import analyze_public_queries as public_analysis  # noqa: E402
from srb_score import accuracy_metrics, load_aligned_csv  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--atlas-predictions", type=Path, required=True)
    parser.add_argument("--fallback-predictions", type=Path, required=True)
    parser.add_argument("--arch-dir", type=Path, required=True)
    parser.add_argument("--atlas-file", type=Path, required=True)
    parser.add_argument("--fixed-splits", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20260827)
    parser.add_argument("--pair-min-train", type=int, default=20)
    parser.add_argument("--pair-min-gain", type=float, default=0.0025)
    return parser.parse_args()


def official_points(golden: np.ndarray, predicted: np.ndarray) -> np.ndarray:
    golden = golden.astype(np.float64, copy=False)
    predicted = predicted.astype(np.float64, copy=False)
    relative = np.zeros(golden.size, dtype=np.float64)
    positive = golden > 0
    relative[positive] = np.abs(predicted[positive] - golden[positive]) / golden[positive]
    relative[~positive] = np.where(predicted[~positive] == 0, 0.0, np.inf)
    return 1.0 - np.tanh(4.0 * relative)


def compact_metrics(golden: np.ndarray, predicted: np.ndarray, mask: np.ndarray) -> dict[str, Any]:
    if not np.any(mask):
        return {"rows": 0}
    return accuracy_metrics(golden[mask], predicted[mask])


def gate_metrics(
    name: str,
    golden: np.ndarray,
    atlas_prediction: np.ndarray,
    fallback_prediction: np.ndarray,
    switch_mask: np.ndarray,
    validation_mask: np.ndarray | None = None,
    status: str = "diagnostic",
) -> dict[str, Any]:
    selected = switch_mask if validation_mask is None else switch_mask & validation_mask
    scope = np.ones(golden.size, dtype=np.bool_) if validation_mask is None else validation_mask
    hybrid = atlas_prediction.copy()
    hybrid[switch_mask] = fallback_prediction[switch_mask]
    base_metrics = compact_metrics(golden, atlas_prediction, scope)
    hybrid_metrics = compact_metrics(golden, hybrid, scope)
    return {
        "name": name,
        "status": status,
        "scope_rows": int(np.count_nonzero(scope)),
        "switched_rows": int(np.count_nonzero(selected)),
        "base_acc_score": base_metrics["acc_score"],
        "hybrid_acc_score": hybrid_metrics["acc_score"],
        "delta_acc_score": hybrid_metrics["acc_score"] - base_metrics["acc_score"],
        "base_mae": base_metrics["mae"],
        "hybrid_mae": hybrid_metrics["mae"],
    }


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    arch = public_analysis.load_architecture(args.arch_dir.resolve())
    rows = public_analysis.read_queries(args.golden.resolve(), arch, output_dir, None, args.seed)
    if rows.failure_rows or rows.success_rows != 1_000_000:
        raise ValueError("expected exactly 1,000,000 successfully parsed public rows")

    golden, atlas_prediction = load_aligned_csv(args.golden, args.atlas_predictions)
    fallback_golden, fallback_prediction = load_aligned_csv(args.golden, args.fallback_predictions)
    if not np.array_equal(golden, fallback_golden):
        raise ValueError("Golden alignment differs between prediction files")

    env = public_analysis.classify_environment(rows, arch)
    sx, sy, tx, ty = (a.astype(np.int32) for a in (rows.sx, rows.sy, rows.tx, rows.ty))
    dx, dy = tx - sx, ty - sy
    cheb = np.maximum(np.abs(dx), np.abs(dy))
    boundary = np.minimum.reduce(
        [sx, sy, tx, ty, arch.width - 1 - sx, arch.height - 1 - sy,
         arch.width - 1 - tx, arch.height - 1 - ty]
    )
    labels, atlas_metadata = public_analysis.classify_v3_from_atlas(
        args.atlas_file.resolve(), rows, arch, cheb, env["block_related"]
    )
    atlas_mask = labels == 1

    atlas_points = official_points(golden, atlas_prediction)
    fallback_points = official_points(golden, fallback_prediction)
    existing_loss = 1.0 - atlas_points
    atlas_loss = float(existing_loss[atlas_mask].sum())
    total_loss = float(existing_loss.sum())

    oracle_prediction = atlas_prediction.copy()
    fallback_better = atlas_mask & (fallback_points > atlas_points)
    oracle_prediction[fallback_better] = fallback_prediction[fallback_better]

    fixed = np.load(args.fixed_splits)
    heldout_packed = fixed["valid_random_stratified"]
    heldout = np.unpackbits(heldout_packed, count=rows.success_rows).astype(np.bool_)
    train = ~heldout

    port_count = len(arch.port_names)
    pair_code = rows.sp.astype(np.int64) * port_count + rows.tp.astype(np.int64)
    delta = fallback_points - atlas_points
    pair_train = atlas_mask & train
    pair_counts = np.bincount(pair_code[pair_train], minlength=port_count * port_count)
    pair_delta_sum = np.bincount(
        pair_code[pair_train], weights=delta[pair_train], minlength=port_count * port_count
    )
    pair_mean_delta = np.divide(
        pair_delta_sum,
        pair_counts,
        out=np.zeros_like(pair_delta_sum),
        where=pair_counts > 0,
    )
    selected_pairs = (pair_counts >= args.pair_min_train) & (pair_mean_delta >= args.pair_min_gain)
    learned_pair_gate = atlas_mask & selected_pairs[pair_code]

    no_gap = atlas_mask & ~env["gap_related"]
    horizontal_only = (
        atlas_mask & (env["horizontal_gap_count"] > 0) & (env["vertical_gap_count"] == 0)
    )
    boundary_32 = atlas_mask & (boundary >= 32)
    gates = [
        gate_metrics("disable_all_atlas", golden, atlas_prediction, fallback_prediction, atlas_mask),
        gate_metrics("fallback_no_endpoint_gap", golden, atlas_prediction, fallback_prediction, no_gap),
        gate_metrics("fallback_horizontal_gap_only", golden, atlas_prediction, fallback_prediction, horizontal_only),
        gate_metrics("fallback_boundary_clearance_ge_32", golden, atlas_prediction, fallback_prediction, boundary_32),
        gate_metrics(
            "learned_port_pair_gate_random_heldout",
            golden,
            atlas_prediction,
            fallback_prediction,
            learned_pair_gate,
            heldout,
            "heldout; pair selection uses training rows and public Golden",
        ),
    ]

    segment_rows: list[dict[str, Any]] = []

    def add_segment(kind: str, name: str, mask: np.ndarray) -> None:
        count = int(np.count_nonzero(mask))
        if not count:
            return
        ap = atlas_points[mask]
        fp = fallback_points[mask]
        segment_rows.append(
            {
                "kind": kind,
                "segment": name,
                "rows": count,
                "atlas_acc_score": float(100.0 * ap.mean()),
                "fallback_acc_score": float(100.0 * fp.mean()),
                "fallback_minus_atlas": float(100.0 * (fp - ap).mean()),
                "fallback_better_ratio": float(np.mean(fp > ap)),
                "atlas_exact_ratio": float(np.mean(atlas_prediction[mask] == golden[mask])),
                "atlas_mae": float(np.mean(np.abs(atlas_prediction[mask] - golden[mask]))),
            }
        )

    add_segment("all", "atlas", atlas_mask)
    add_segment("gap", "none", no_gap)
    add_segment("gap", "horizontal_only", horizontal_only)
    add_segment(
        "gap", "vertical_only",
        atlas_mask & (env["horizontal_gap_count"] == 0) & (env["vertical_gap_count"] > 0),
    )
    add_segment(
        "gap", "both",
        atlas_mask & (env["horizontal_gap_count"] > 0) & (env["vertical_gap_count"] > 0),
    )
    for low, high in ((0, 0), (1, 8), (9, 16), (17, 32), (33, 48)):
        add_segment("distance", f"{low}-{high}", atlas_mask & (cheb >= low) & (cheb <= high))
    for low, high in ((0, 0), (1, 1), (2, 3), (4, 7), (8, 15), (16, 31), (32, 999)):
        add_segment("boundary", f"{low}-{high}", atlas_mask & (boundary >= low) & (boundary <= high))
    source_input = np.asarray([d == "input" for d in arch.port_direction], dtype=np.bool_)[rows.sp]
    target_input = np.asarray([d == "input" for d in arch.port_direction], dtype=np.bool_)[rows.tp]
    for source_kind, source_flag in (("input", True), ("output", False)):
        for target_kind, target_flag in (("input", True), ("output", False)):
            add_segment(
                "port_direction",
                f"{source_kind}_to_{target_kind}",
                atlas_mask & (source_input == source_flag) & (target_input == target_flag),
            )

    pair_rows: list[dict[str, Any]] = []
    atlas_pairs = np.unique(pair_code[atlas_mask])
    for code in atlas_pairs:
        mask = atlas_mask & (pair_code == code)
        count = int(mask.sum())
        if count < 10:
            continue
        sp, tp = divmod(int(code), port_count)
        pair_rows.append(
            {
                "from_port": arch.port_names[sp],
                "to_port": arch.port_names[tp],
                "rows": count,
                "train_rows": int(pair_counts[code]),
                "train_fallback_minus_atlas": float(100.0 * pair_mean_delta[code]),
                "all_fallback_minus_atlas": float(100.0 * delta[mask].mean()),
                "atlas_exact_ratio": float(np.mean(atlas_prediction[mask] == golden[mask])),
                "selected_by_train_gate": bool(selected_pairs[code]),
            }
        )
    pair_rows.sort(key=lambda row: row["all_fallback_minus_atlas"], reverse=True)

    result = {
        "scope": "public 1M diagnostic; not hidden official score",
        "architecture_fingerprint_sha256": arch.fingerprint_sha256,
        "atlas": atlas_metadata,
        "rows": rows.success_rows,
        "atlas_rows": int(atlas_mask.sum()),
        "atlas_ratio": float(atlas_mask.mean()),
        "existing_v3": accuracy_metrics(golden, atlas_prediction),
        "disable_atlas": accuracy_metrics(golden, fallback_prediction),
        "atlas_branch_existing": compact_metrics(golden, atlas_prediction, atlas_mask),
        "atlas_branch_fallback": compact_metrics(golden, fallback_prediction, atlas_mask),
        "oracle_choose_atlas_or_fallback": accuracy_metrics(golden, oracle_prediction),
        "oracle_switched_rows": int(fallback_better.sum()),
        "loss_contribution": {
            "total_point_loss_sum": total_loss,
            "atlas_point_loss_sum": atlas_loss,
            "atlas_share_of_total_point_loss": atlas_loss / total_loss,
            "atlas_share_of_rows": float(atlas_mask.mean()),
        },
        "gates": gates,
        "learned_pair_gate": {
            "pair_min_train": args.pair_min_train,
            "pair_min_gain_points": 100.0 * args.pair_min_gain,
            "selected_pair_count": int(selected_pairs.sum()),
            "status": "heldout diagnostic only; not architecture-proved",
        },
    }
    (output_dir / "atlas_loss_analysis.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    with (output_dir / "atlas_loss_segments.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(segment_rows[0]))
        writer.writeheader()
        writer.writerows(segment_rows)
    with (output_dir / "atlas_port_pair_gates.csv").open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(pair_rows[0]))
        writer.writeheader()
        writer.writerows(pair_rows)

    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
