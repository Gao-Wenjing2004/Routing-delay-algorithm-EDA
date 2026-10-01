#!/usr/bin/env python3
"""Compare V6 with aligned V3/V4 predictions on structural segments."""

from __future__ import annotations

import argparse
import csv
import json
from pathlib import Path

import numpy as np

import analyze_public_queries as public_analysis
from srb_score import accuracy_metrics, load_aligned_csv


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--golden", required=True, type=Path)
    p.add_argument("--v3", required=True, type=Path)
    p.add_argument("--v4", required=True, type=Path)
    p.add_argument("--v6", required=True, type=Path)
    p.add_argument("--arch-dir", required=True, type=Path)
    p.add_argument("--atlas", required=True, type=Path)
    p.add_argument("--fixed-splits", required=True, type=Path)
    p.add_argument("--output-dir", required=True, type=Path)
    return p.parse_args()


def compact(golden: np.ndarray, predicted: np.ndarray, mask: np.ndarray) -> dict:
    result = accuracy_metrics(golden[mask], predicted[mask])
    return {
        key: result[key]
        for key in (
            "rows",
            "acc_score",
            "mae",
            "rmse",
            "mean_relative_error",
            "p95_absolute_error",
            "p99_absolute_error",
            "max_absolute_error",
        )
    }


def main() -> int:
    opt = parse_args()
    opt.output_dir.mkdir(parents=True, exist_ok=True)
    arch = public_analysis.load_architecture(opt.arch_dir)
    (opt.output_dir / ".parse").mkdir(parents=True, exist_ok=True)
    rows = public_analysis.read_queries(
        opt.golden, arch, opt.output_dir / ".parse", None, 20260827
    )
    if rows.failure_rows:
        raise ValueError(f"failed to parse {rows.failure_rows} rows")
    golden, v3 = load_aligned_csv(opt.golden, opt.v3)
    for path, name in ((opt.v4, "v4"), (opt.v6, "v6")):
        aligned_golden, prediction = load_aligned_csv(opt.golden, path)
        if not np.array_equal(golden, aligned_golden):
            raise ValueError(f"Golden mismatch for {name}")
        if name == "v4":
            v4 = prediction
        else:
            v6 = prediction

    sx, sy, tx, ty = (
        values.astype(np.int32) for values in (rows.sx, rows.sy, rows.tx, rows.ty)
    )
    dx, dy = tx - sx, ty - sy
    cheb = np.maximum(np.abs(dx), np.abs(dy))
    env = public_analysis.classify_environment(rows, arch)
    labels56, atlas_meta = public_analysis.classify_v3_from_atlas(
        opt.atlas, rows, arch, cheb, env["block_related"], query_radius=56
    )
    atlas56 = labels56 == 1
    all_rows = np.ones(rows.success_rows, dtype=np.bool_)
    segments: dict[str, np.ndarray] = {
        "all": all_rows,
        "atlas_radius56": atlas56,
        "fallback_radius56": ~atlas56,
        "block_bbox": env["block_related"],
        "no_block_bbox": ~env["block_related"],
        "gap_span": env["gap_related"],
        "no_gap_span": ~env["gap_related"],
        "distance_0_48": cheb <= 48,
        "distance_49_56": (cheb >= 49) & (cheb <= 56),
        "distance_57_128": (cheb >= 57) & (cheb <= 128),
        "distance_129_256": (cheb >= 129) & (cheb <= 256),
        "distance_257_plus": cheb >= 257,
    }
    with np.load(opt.fixed_splits) as split_data:
        for key in split_data.files:
            if key.startswith("valid_"):
                segments[f"split_{key[6:]}"] = np.unpackbits(split_data[key])[
                    : rows.success_rows
                ].astype(np.bool_)

    report = {
        "scope": "public 1M diagnostic; not hidden official score",
        "architecture_fingerprint_sha256": arch.fingerprint_sha256,
        "atlas": atlas_meta,
        "segments": {},
    }
    csv_rows = []
    for name, mask in segments.items():
        if not mask.any():
            continue
        metrics = {
            "v3": compact(golden, v3, mask),
            "v4": compact(golden, v4, mask),
            "v6": compact(golden, v6, mask),
        }
        metrics["v6_delta_acc_vs_v3"] = (
            metrics["v6"]["acc_score"] - metrics["v3"]["acc_score"]
        )
        metrics["v6_delta_acc_vs_v4"] = (
            metrics["v6"]["acc_score"] - metrics["v4"]["acc_score"]
        )
        report["segments"][name] = metrics
        csv_rows.append(
            [
                name,
                int(mask.sum()),
                metrics["v3"]["acc_score"],
                metrics["v4"]["acc_score"],
                metrics["v6"]["acc_score"],
                metrics["v6_delta_acc_vs_v3"],
                metrics["v6_delta_acc_vs_v4"],
                metrics["v3"]["mae"],
                metrics["v4"]["mae"],
                metrics["v6"]["mae"],
            ]
        )

    (opt.output_dir / "v6_segment_analysis.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    with (opt.output_dir / "v6_segment_summary.csv").open(
        "w", encoding="utf-8-sig", newline=""
    ) as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "segment",
                "rows",
                "v3_acc",
                "v4_acc",
                "v6_acc",
                "v6_delta_vs_v3",
                "v6_delta_vs_v4",
                "v3_mae",
                "v4_mae",
                "v6_mae",
            ]
        )
        writer.writerows(csv_rows)
    print(json.dumps(report["segments"]["all"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
