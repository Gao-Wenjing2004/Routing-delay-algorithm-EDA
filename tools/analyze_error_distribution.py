#!/usr/bin/env python3
"""Quantify where SRB prediction-score loss is concentrated on aligned public data.

The report uses the contest's per-row loss, tanh(4 * |prediction-golden| / golden),
so each segment's contribution adds up exactly to the total accuracy-score loss.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path

import numpy as np

import analyze_public_queries as public_analysis
from srb_score import load_aligned_csv, point_scores


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", required=True, type=Path)
    parser.add_argument(
        "--prediction",
        action="append",
        required=True,
        metavar="NAME=CSV",
        help="aligned prediction CSV; repeat for multiple models",
    )
    parser.add_argument("--primary", required=True, help="primary model name")
    parser.add_argument("--arch-dir", required=True, type=Path)
    parser.add_argument("--atlas", required=True, type=Path)
    parser.add_argument("--query-radius", type=int, default=48)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--top-errors", type=int, default=1000)
    return parser.parse_args()


def parse_predictions(values: list[str]) -> dict[str, Path]:
    result: dict[str, Path] = {}
    for value in values:
        if "=" not in value:
            raise ValueError(f"prediction must be NAME=CSV, got {value!r}")
        name, raw_path = value.split("=", 1)
        name = name.strip()
        if not name or name in result:
            raise ValueError(f"empty or duplicate prediction name: {name!r}")
        result[name] = Path(raw_path)
    return result


def json_value(value: object) -> object:
    if isinstance(value, (np.integer,)):
        return int(value)
    if isinstance(value, (np.floating,)):
        return float(value)
    if isinstance(value, np.ndarray):
        return value.tolist()
    if isinstance(value, dict):
        return {str(key): json_value(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_value(item) for item in value]
    return value


def segment_metrics(
    golden: np.ndarray,
    predicted: np.ndarray,
    point_loss: np.ndarray,
    mask: np.ndarray,
    total_loss: float,
) -> dict[str, float | int]:
    count = int(mask.sum())
    if count == 0:
        return {
            "rows": 0,
            "row_share_pct": 0.0,
            "acc_score": math.nan,
            "loss_contribution_points": 0.0,
            "loss_share_pct": 0.0,
            "loss_index_vs_average": math.nan,
            "mean_relative_error_pct": math.nan,
            "mae_ps": math.nan,
            "p95_absolute_error_ps": math.nan,
            "exact_share_pct": math.nan,
            "under_share_pct": math.nan,
            "over_share_pct": math.nan,
        }
    selected_loss = point_loss[mask]
    selected_golden = golden[mask]
    selected_prediction = predicted[mask]
    signed = selected_prediction - selected_golden
    absolute = np.abs(signed)
    relative = np.divide(
        absolute,
        selected_golden,
        out=np.zeros_like(absolute),
        where=selected_golden != 0,
    )
    loss_sum = float(selected_loss.sum())
    return {
        "rows": count,
        "row_share_pct": 100.0 * count / golden.size,
        "acc_score": 100.0 * (1.0 - float(selected_loss.mean())),
        "loss_contribution_points": 100.0 * loss_sum / golden.size,
        "loss_share_pct": 100.0 * loss_sum / total_loss if total_loss else 0.0,
        "loss_index_vs_average": float(selected_loss.mean()) / float(point_loss.mean()),
        "mean_relative_error_pct": 100.0 * float(relative.mean()),
        "mae_ps": float(absolute.mean()),
        "p95_absolute_error_ps": float(np.percentile(absolute, 95)),
        "exact_share_pct": 100.0 * float(np.mean(signed == 0)),
        "under_share_pct": 100.0 * float(np.mean(signed < 0)),
        "over_share_pct": 100.0 * float(np.mean(signed > 0)),
    }


def interval_mask(values: np.ndarray, low: int | None, high: int | None) -> np.ndarray:
    mask = np.ones(values.size, dtype=np.bool_)
    if low is not None:
        mask &= values >= low
    if high is not None:
        mask &= values <= high
    return mask


def make_categories(
    rows: public_analysis.ParsedRows,
    arch: public_analysis.Architecture,
    env: dict[str, np.ndarray],
    golden: np.ndarray,
    cheb: np.ndarray,
    dx: np.ndarray,
    dy: np.ndarray,
    labels: np.ndarray,
) -> dict[str, list[tuple[str, np.ndarray]]]:
    n = rows.success_rows
    all_rows = np.ones(n, dtype=np.bool_)
    endpoint_boundary = np.minimum.reduce(
        [
            rows.sx.astype(np.int32),
            rows.sy.astype(np.int32),
            (arch.width - 1) - rows.sx.astype(np.int32),
            (arch.height - 1) - rows.sy.astype(np.int32),
            rows.tx.astype(np.int32),
            rows.ty.astype(np.int32),
            (arch.width - 1) - rows.tx.astype(np.int32),
            (arch.height - 1) - rows.ty.astype(np.int32),
        ]
    )
    block_count = env["block_count"]
    gap_count = env["gap_count"]
    hgap = env["horizontal_gap_count"] > 0
    vgap = env["vertical_gap_count"] > 0
    block = env["block_related"]

    categories: dict[str, list[tuple[str, np.ndarray]]] = {
        "all": [("all", all_rows)],
        "distance": [
            ("0-16", interval_mask(cheb, 0, 16)),
            ("17-32", interval_mask(cheb, 17, 32)),
            ("33-48", interval_mask(cheb, 33, 48)),
            ("49-56", interval_mask(cheb, 49, 56)),
            ("57-72", interval_mask(cheb, 57, 72)),
            ("73-128", interval_mask(cheb, 73, 128)),
            ("129-256", interval_mask(cheb, 129, 256)),
            ("257+", interval_mask(cheb, 257, None)),
        ],
        "block_bbox": [("none", ~block), ("intersects", block)],
        "block_count": [
            ("0", block_count == 0),
            ("1", block_count == 1),
            ("2", block_count == 2),
            ("3+", block_count >= 3),
        ],
        "gap_orientation": [
            ("none", ~hgap & ~vgap),
            ("horizontal_only", hgap & ~vgap),
            ("vertical_only", ~hgap & vgap),
            ("both", hgap & vgap),
        ],
        "gap_count": [
            ("0", gap_count == 0),
            ("1-2", interval_mask(gap_count, 1, 2)),
            ("3-4", interval_mask(gap_count, 3, 4)),
            ("5+", interval_mask(gap_count, 5, None)),
        ],
        "v3_branch": [
            (name, labels == code)
            for code, name in public_analysis.BRANCH_NAMES.items()
            if np.any(labels == code)
        ],
        "endpoint_boundary_clearance": [
            ("0", endpoint_boundary == 0),
            ("1-8", interval_mask(endpoint_boundary, 1, 8)),
            ("9-16", interval_mask(endpoint_boundary, 9, 16)),
            ("17-32", interval_mask(endpoint_boundary, 17, 32)),
            ("33+", interval_mask(endpoint_boundary, 33, None)),
        ],
        "orientation": [
            ("same_cell", (dx == 0) & (dy == 0)),
            ("horizontal", (dx != 0) & (dy == 0)),
            ("vertical", (dx == 0) & (dy != 0)),
            ("diagonal", (dx != 0) & (dy != 0)),
        ],
        "golden_delay_ps": [
            ("0-999", interval_mask(golden, 0, 999)),
            ("1000-1499", interval_mask(golden, 1000, 1499)),
            ("1500-1999", interval_mask(golden, 1500, 1999)),
            ("2000-2999", interval_mask(golden, 2000, 2999)),
            ("3000-4999", interval_mask(golden, 3000, 4999)),
            ("5000+", interval_mask(golden, 5000, None)),
        ],
        "distance_x_block": [],
    }
    distance_groups = [
        ("0-48", interval_mask(cheb, 0, 48)),
        ("49-128", interval_mask(cheb, 49, 128)),
        ("129-256", interval_mask(cheb, 129, 256)),
        ("257+", interval_mask(cheb, 257, None)),
    ]
    for distance_name, distance_mask in distance_groups:
        categories["distance_x_block"].append((f"{distance_name}|no_block", distance_mask & ~block))
        categories["distance_x_block"].append((f"{distance_name}|block", distance_mask & block))
    return categories


def grouped_port_rows(
    arch: public_analysis.Architecture,
    rows: public_analysis.ParsedRows,
    point_loss: np.ndarray,
    predicted: np.ndarray,
    golden: np.ndarray,
    limit: int = 30,
) -> list[dict[str, object]]:
    port_count = len(arch.port_names)
    codes = rows.sp.astype(np.int64) * port_count + rows.tp.astype(np.int64)
    group_count = port_count * port_count
    counts = np.bincount(codes, minlength=group_count)
    losses = np.bincount(codes, weights=point_loss, minlength=group_count)
    absolute = np.abs(predicted - golden)
    abs_sum = np.bincount(codes, weights=absolute, minlength=group_count)
    order = np.argsort(losses)[::-1]
    total_loss = float(point_loss.sum())
    result: list[dict[str, object]] = []
    for code in order:
        if counts[code] == 0:
            continue
        sp, tp = divmod(int(code), port_count)
        count = int(counts[code])
        result.append(
            {
                "source_port": arch.port_names[sp],
                "target_port": arch.port_names[tp],
                "rows": count,
                "row_share_pct": 100.0 * count / golden.size,
                "acc_score": 100.0 * (1.0 - float(losses[code]) / count),
                "loss_contribution_points": 100.0 * float(losses[code]) / golden.size,
                "loss_share_pct": 100.0 * float(losses[code]) / total_loss,
                "mae_ps": float(abs_sum[code]) / count,
            }
        )
        if len(result) >= limit:
            break
    return result


def overlapping_feature_rows(
    items: list[object],
    bitmask: np.ndarray,
    point_loss: np.ndarray,
    golden: np.ndarray,
    predicted: np.ndarray,
    total_loss: float,
    kind: str,
) -> list[dict[str, object]]:
    result: list[dict[str, object]] = []
    for index, item in enumerate(items):
        mask = (bitmask & (np.uint64(1) << np.uint64(index))) != 0
        if not mask.any():
            continue
        metrics = segment_metrics(golden, predicted, point_loss, mask, total_loss)
        if kind == "block":
            label = f"Block_{getattr(item, 'block_id')}"
            metadata = {
                "category": getattr(item, "category"),
                "left": getattr(item, "left"),
                "right": getattr(item, "right"),
                "lower": getattr(item, "lower"),
                "upper": getattr(item, "upper"),
            }
        else:
            label = f"Gap_{getattr(item, 'gap_id')}"
            metadata = {
                "direction": getattr(item, "direction"),
                "site": getattr(item, "site"),
                "delay": getattr(item, "delay"),
            }
        result.append({"feature": label, **metadata, **metrics})
    result.sort(key=lambda row: float(row["loss_contribution_points"]), reverse=True)
    return result


def write_dict_csv(path: Path, rows: list[dict[str, object]]) -> None:
    if not rows:
        return
    columns: list[str] = []
    for row in rows:
        for key in row:
            if key not in columns:
                columns.append(key)
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=columns, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def write_top_errors(
    path: Path,
    limit: int,
    arch: public_analysis.Architecture,
    rows: public_analysis.ParsedRows,
    golden: np.ndarray,
    predicted: np.ndarray,
    point_loss: np.ndarray,
    cheb: np.ndarray,
    env: dict[str, np.ndarray],
    labels: np.ndarray,
) -> None:
    limit = min(limit, golden.size)
    selected = np.argpartition(point_loss, -limit)[-limit:]
    selected = selected[np.argsort(point_loss[selected])[::-1]]
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "rank",
                "csv_line_number",
                "From",
                "To",
                "golden_ps",
                "predicted_ps",
                "signed_error_ps",
                "absolute_error_ps",
                "relative_error_pct",
                "contest_loss",
                "chebyshev_distance",
                "block_count",
                "gap_count",
                "v3_branch",
            ]
        )
        for rank, index in enumerate(selected, 1):
            signed = float(predicted[index] - golden[index])
            relative = abs(signed) / float(golden[index]) if golden[index] else math.inf
            writer.writerow(
                [
                    rank,
                    int(rows.line_no[index]),
                    public_analysis.endpoint_text(int(rows.source_code[index]), arch),
                    public_analysis.endpoint_text(int(rows.target_code[index]), arch),
                    int(golden[index]),
                    int(predicted[index]),
                    int(signed),
                    int(abs(signed)),
                    100.0 * relative,
                    float(point_loss[index]),
                    int(cheb[index]),
                    int(env["block_count"][index]),
                    int(env["gap_count"][index]),
                    public_analysis.BRANCH_NAMES[int(labels[index])],
                ]
            )


def main() -> int:
    opt = parse_args()
    prediction_paths = parse_predictions(opt.prediction)
    if opt.primary not in prediction_paths:
        raise ValueError(f"primary model {opt.primary!r} is not in --prediction")
    opt.output_dir.mkdir(parents=True, exist_ok=True)
    parse_dir = opt.output_dir / ".parse"
    parse_dir.mkdir(exist_ok=True)

    arch = public_analysis.load_architecture(opt.arch_dir)
    rows = public_analysis.read_queries(opt.golden, arch, parse_dir, None, 20260920)
    if rows.failure_rows or rows.success_rows != rows.analyzed_rows:
        raise ValueError(f"parsed={rows.success_rows}, failures={rows.failure_rows}")

    predictions: dict[str, np.ndarray] = {}
    golden: np.ndarray | None = None
    for name, path in prediction_paths.items():
        aligned_golden, predicted = load_aligned_csv(opt.golden, path)
        if golden is None:
            golden = aligned_golden
        elif not np.array_equal(golden, aligned_golden):
            raise ValueError(f"Golden mismatch for {name}")
        predictions[name] = predicted
    assert golden is not None
    if golden.size != rows.success_rows:
        raise ValueError(f"row count mismatch: Golden={golden.size}, parsed={rows.success_rows}")

    sx, sy, tx, ty = (
        values.astype(np.int32) for values in (rows.sx, rows.sy, rows.tx, rows.ty)
    )
    dx, dy = tx - sx, ty - sy
    cheb = np.maximum(np.abs(dx), np.abs(dy))
    env = public_analysis.classify_environment(rows, arch)
    labels, atlas_meta = public_analysis.classify_v3_from_atlas(
        opt.atlas,
        rows,
        arch,
        cheb,
        env["block_related"],
        query_radius=opt.query_radius,
    )
    categories = make_categories(rows, arch, env, golden, cheb, dx, dy, labels)

    report: dict[str, object] = {
        "scope": "public 1M rows; official contest accuracy loss formula",
        "architecture_fingerprint_sha256": arch.fingerprint_sha256,
        "atlas": atlas_meta,
        "models": {},
    }
    long_rows: list[dict[str, object]] = []
    for model_name, predicted in predictions.items():
        scores = point_scores(golden, predicted)
        point_loss = 1.0 - scores
        total_loss = float(point_loss.sum())
        model_report: dict[str, object] = {
            "rows": int(golden.size),
            "accuracy_score": 100.0 * float(scores.mean()),
            "score_loss_points": 100.0 * float(point_loss.mean()),
            "segments": {},
        }
        for category, segment_masks in categories.items():
            segment_report: dict[str, object] = {}
            for segment, mask in segment_masks:
                metrics = segment_metrics(golden, predicted, point_loss, mask, total_loss)
                segment_report[segment] = metrics
                long_rows.append(
                    {
                        "model": model_name,
                        "category": category,
                        "segment": segment,
                        **metrics,
                    }
                )
            model_report["segments"][category] = segment_report

        order = np.argsort(point_loss)[::-1]
        cumulative = np.cumsum(point_loss[order])
        concentration: dict[str, object] = {}
        for pct in (0.1, 0.5, 1, 2, 5, 10, 20):
            count = max(1, int(round(golden.size * pct / 100.0)))
            concentration[f"top_{pct:g}_pct_rows"] = {
                "rows": count,
                "loss_share_pct": 100.0 * float(cumulative[count - 1]) / total_loss,
                "min_contest_loss": float(point_loss[order[count - 1]]),
            }
        model_report["loss_concentration"] = concentration

        signed = predicted - golden
        direction_rows = []
        for name, mask in (
            ("under", signed < 0),
            ("exact", signed == 0),
            ("over", signed > 0),
        ):
            direction_rows.append(
                {"direction": name, **segment_metrics(golden, predicted, point_loss, mask, total_loss)}
            )
        model_report["error_direction"] = direction_rows
        model_report["top_port_pairs"] = grouped_port_rows(
            arch, rows, point_loss, predicted, golden
        )
        model_report["blocks_overlapping"] = overlapping_feature_rows(
            arch.blocks,
            env["block_mask"],
            point_loss,
            golden,
            predicted,
            total_loss,
            "block",
        )
        model_report["gaps_overlapping"] = overlapping_feature_rows(
            arch.gaps,
            env["gap_mask"],
            point_loss,
            golden,
            predicted,
            total_loss,
            "gap",
        )
        report["models"][model_name] = model_report

        if model_name == opt.primary:
            write_top_errors(
                opt.output_dir / "top_error_rows.csv",
                opt.top_errors,
                arch,
                rows,
                golden,
                predicted,
                point_loss,
                cheb,
                env,
                labels,
            )
            write_dict_csv(opt.output_dir / "top_port_pairs.csv", model_report["top_port_pairs"])
            write_dict_csv(opt.output_dir / "block_overlap_stats.csv", model_report["blocks_overlapping"])
            write_dict_csv(opt.output_dir / "gap_overlap_stats.csv", model_report["gaps_overlapping"])

    write_dict_csv(opt.output_dir / "segment_loss_stats.csv", long_rows)
    (opt.output_dir / "error_distribution.json").write_text(
        json.dumps(json_value(report), ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(
        json.dumps(
            {
                name: {
                    "accuracy_score": data["accuracy_score"],
                    "score_loss_points": data["score_loss_points"],
                }
                for name, data in report["models"].items()
            },
            ensure_ascii=False,
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
