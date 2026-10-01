#!/usr/bin/env python3
"""Evaluate a held-out selector between a V4 base and an architecture-only predictor.

The selector sees only values available at inference time.  Every outer split
uses an inner fit/calibration partition; the outer validation rows never fit
tables or select a threshold.
"""

from __future__ import annotations

import argparse
import csv
import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
if str(HERE) not in sys.path:
    sys.path.insert(0, str(HERE))

import analyze_public_queries as public_analysis  # noqa: E402
from srb_score import accuracy_metrics, load_aligned_csv, point_scores  # noqa: E402
from srb_v4_research import bin_by_edges, splitmix64  # noqa: E402


@dataclass(frozen=True)
class Group:
    name: str
    code: np.ndarray
    size: int
    shrink: float


def options() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--golden", required=True, type=Path)
    p.add_argument("--base", required=True, type=Path)
    p.add_argument("--structural", required=True, type=Path)
    p.add_argument("--arch-dir", required=True, type=Path)
    p.add_argument("--fixed-splits", required=True, type=Path)
    p.add_argument("--output-dir", required=True, type=Path)
    p.add_argument("--seed", type=int, default=20260828)
    return p.parse_args()


def fit_additive_gain(groups: list[Group], gain: np.ndarray, mask: np.ndarray) -> list[np.ndarray]:
    current = np.zeros(gain.size, dtype=np.float64)
    effects = [np.zeros(group.size, dtype=np.float64) for group in groups]
    for _ in range(3):
        for index, group in enumerate(groups):
            current -= effects[index][group.code]
            code = group.code[mask]
            residual = gain[mask] - current[mask]
            sums = np.bincount(code, weights=residual, minlength=group.size)
            counts = np.bincount(code, minlength=group.size)
            effects[index] = np.clip(sums / (counts + group.shrink), -0.25, 0.25)
            current += effects[index][group.code]
    return effects


def predicted_gain(groups: list[Group], effects: list[np.ndarray]) -> np.ndarray:
    result = np.zeros(groups[0].code.size, dtype=np.float64)
    for group, effect in zip(groups, effects):
        result += effect[group.code]
    return result


def main() -> int:
    opt = options()
    opt.output_dir.mkdir(parents=True, exist_ok=True)
    arch = public_analysis.load_architecture(opt.arch_dir)
    parse_dir = opt.output_dir / ".parse"
    parse_dir.mkdir(parents=True, exist_ok=True)
    rows = public_analysis.read_queries(opt.golden, arch, parse_dir, None, opt.seed)
    if rows.failure_rows:
        raise ValueError(f"parse failures: {rows.failure_rows}")
    golden_csv, base = load_aligned_csv(opt.golden, opt.base)
    golden_struct, structural = load_aligned_csv(opt.golden, opt.structural)
    if not np.array_equal(golden_csv, golden_struct):
        raise ValueError("Golden inputs are not aligned")
    golden = golden_csv.astype(np.float64)
    base = base.astype(np.float64)
    structural = structural.astype(np.float64)

    env = public_analysis.classify_environment(rows, arch)
    sx, sy, tx, ty = (v.astype(np.int32) for v in (rows.sx, rows.sy, rows.tx, rows.ty))
    dx, dy = tx - sx, ty - sy
    cheb = np.maximum(np.abs(dx), np.abs(dy))
    quad = (
        (dx > 0).astype(np.int32)
        + 2 * (dx < 0).astype(np.int32)
        + 3 * (dy > 0).astype(np.int32)
        + 6 * (dy < 0).astype(np.int32)
    )
    distance_bin = bin_by_edges(cheb, (8, 16, 32, 48, 64, 96, 128, 192, 256, 384, 488))
    boundary = np.minimum.reduce(
        [sx, sy, tx, ty, arch.width - 1 - sx, arch.height - 1 - sy,
         arch.width - 1 - tx, arch.height - 1 - ty]
    )
    boundary_bin = bin_by_edges(boundary, (0, 1, 2, 4, 8, 16, 32, 64, 128))
    gap_h = np.minimum(15, env["horizontal_gap_count"].astype(np.int32))
    gap_v = np.minimum(15, env["vertical_gap_count"].astype(np.int32))
    gap_shape = gap_h * 16 + gap_v
    block_count = np.minimum(7, env["block_count"].astype(np.int32))
    base_bin = np.minimum(67, base.astype(np.int64) // 128).astype(np.int32)
    delta = structural.astype(np.int64) - base.astype(np.int64)
    delta_sign = np.sign(delta).astype(np.int32) + 1
    delta_mag = bin_by_edges(np.abs(delta), (1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024))
    delta_shape = delta_sign * 12 + delta_mag

    family_keys = [re.sub(r"\[\d+\]$", "[]", name) for name in arch.port_names]
    family_to_id = {name: index for index, name in enumerate(dict.fromkeys(family_keys))}
    port_family = np.asarray([family_to_id[name] for name in family_keys], dtype=np.int32)
    sf = port_family[rows.sp.astype(np.int32)]
    tf = port_family[rows.tp.astype(np.int32)]
    family_count = len(family_to_id)

    common = [
        Group("base_bin", base_bin, 68, 200.0),
        Group("architecture_delta", delta_shape, 36, 200.0),
        Group("distance_quad", quad * 12 + distance_bin, 9 * 12, 200.0),
        Group("boundary", boundary_bin, 10, 200.0),
        Group("gap", gap_shape, 256, 200.0),
        Group("block_distance", (block_count * 9 + quad) * 12 + distance_bin, 8 * 9 * 12, 200.0),
    ]
    candidates = {
        "geometry": common,
        "family": common + [
            Group("source_family", sf, family_count, 100.0),
            Group("target_family", tf, family_count, 100.0),
        ],
        "family_pair": common + [
            Group("family_pair", sf * family_count + tf, family_count * family_count, 50.0),
        ],
    }

    base_points = point_scores(golden, base)
    structural_points = point_scores(golden, structural)
    gain = structural_points - base_points
    oracle = np.where(gain > 0.0, structural, base)
    with np.load(opt.fixed_splits) as split_data:
        if not np.array_equal(np.asarray(split_data["line_number"], dtype=np.uint32), rows.line_no):
            raise ValueError("fixed split line numbers do not match")
        split_names = sorted(name[6:] for name in split_data.files if name.startswith("valid_"))
        packed = {name: np.asarray(split_data[f"valid_{name}"], dtype=np.uint8) for name in split_names}

    threshold_grid = np.asarray((0.0, 0.0005, 0.001, 0.002, 0.004, 0.008, 0.016, 0.032))
    details: dict[str, object] = {
        "scope": "public diagnostic with outer heldout selectors; not hidden official score",
        "rows": rows.success_rows,
        "base": accuracy_metrics(golden, base),
        "structural": accuracy_metrics(golden, structural),
        "oracle": accuracy_metrics(golden, oracle),
        "oracle_structural_rows": int(np.count_nonzero(gain > 0.0)),
        "family_count": family_count,
        "splits": {},
    }
    csv_rows: list[dict[str, object]] = []
    for split_name in split_names:
        valid = np.unpackbits(packed[split_name])[: rows.success_rows].astype(np.bool_)
        outer_train = ~valid
        inner_hash = splitmix64(rows.line_no.astype(np.uint64), opt.seed ^ 0x6A09E667)
        calibration = outer_train & (inner_hash % np.uint64(5) == 0)
        fit_mask = outer_train & ~calibration
        split_result: dict[str, object] = {}
        base_valid = accuracy_metrics(golden[valid], base[valid])
        for candidate_name, groups in candidates.items():
            effects = fit_additive_gain(groups, gain, fit_mask)
            estimate = predicted_gain(groups, effects)
            best_threshold = float(threshold_grid[0])
            best_calibration_gain = -np.inf
            for threshold in threshold_grid:
                selected = calibration & (estimate > threshold)
                calibration_gain = float(gain[selected].sum() / max(1, int(calibration.sum())) * 100.0)
                if calibration_gain > best_calibration_gain:
                    best_threshold = float(threshold)
                    best_calibration_gain = calibration_gain
            selected_valid = valid & (estimate > best_threshold)
            hybrid = base.copy()
            hybrid[selected_valid] = structural[selected_valid]
            metrics = accuracy_metrics(golden[valid], hybrid[valid])
            row = {
                "split": split_name,
                "candidate": candidate_name,
                "validation_rows": int(valid.sum()),
                "threshold": best_threshold,
                "selected_rows": int(selected_valid.sum()),
                "selected_ratio": float(selected_valid.sum() / valid.sum()),
                "base_acc": base_valid["acc_score"],
                "hybrid_acc": metrics["acc_score"],
                "delta_acc": metrics["acc_score"] - base_valid["acc_score"],
                "calibration_delta_acc": best_calibration_gain,
            }
            csv_rows.append(row)
            split_result[candidate_name] = row
            print(
                f"split={split_name} candidate={candidate_name} "
                f"selected={row['selected_ratio']:.4%} delta={row['delta_acc']:+.6f}",
                flush=True,
            )
        details["splits"][split_name] = split_result

    summary: dict[str, object] = {}
    for candidate_name in candidates:
        values = [float(row["delta_acc"]) for row in csv_rows if row["candidate"] == candidate_name]
        ratios = [float(row["selected_ratio"]) for row in csv_rows if row["candidate"] == candidate_name]
        summary[candidate_name] = {
            "mean_delta_acc": float(np.mean(values)),
            "min_delta_acc": float(np.min(values)),
            "max_delta_acc": float(np.max(values)),
            "mean_selected_ratio": float(np.mean(ratios)),
            "all_splits_positive": bool(np.min(values) > 0.0),
        }
    details["summary"] = summary
    with (opt.output_dir / "selector_ablation.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(csv_rows[0]))
        writer.writeheader()
        writer.writerows(csv_rows)
    (opt.output_dir / "selector_analysis.json").write_text(
        json.dumps(details, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
