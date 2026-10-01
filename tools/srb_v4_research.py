#!/usr/bin/env python3
"""Reproducible V4 split, audit, and lightweight residual experiments.

This is an offline research tool.  It does not modify V3 artifacts and does not
put a candidate into the V4 production path.  A candidate is selected only when
its held-out official accuracy score satisfies the configured stability rule.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import importlib.util
import json
import math
import re
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np

THIS_DIR = Path(__file__).resolve().parent
REPO_ROOT = THIS_DIR.parent
if str(THIS_DIR) not in sys.path:
    sys.path.insert(0, str(THIS_DIR))

import analyze_public_queries as public_analysis  # noqa: E402
from srb_score import DEFAULT_CONTRACT, accuracy_metrics, load_aligned_csv  # noqa: E402


DISTANCE_REPORT_BINS = (
    (0, 48, "0-48"),
    (49, 128, "49-128"),
    (129, 256, "129-256"),
    (257, None, "257+"),
)


@dataclass(frozen=True)
class Group:
    name: str
    code: np.ndarray
    size: int
    shrink: float


@dataclass
class ResidualFit:
    name: str
    alpha: float
    groups: list[Group]
    effects: list[np.ndarray]

    @property
    def parameter_count(self) -> int:
        return int(sum(effect.size for effect in self.effects))

    @property
    def parameter_bytes(self) -> int:
        return self.parameter_count * 4


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="SRB V4 fixed-split residual research")
    parser.add_argument("--golden", required=True, type=Path)
    parser.add_argument("--v3-predictions", required=True, type=Path)
    parser.add_argument("--arch-dir", required=True, type=Path)
    parser.add_argument("--atlas-file", required=True, type=Path)
    parser.add_argument("--v3-train-script", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=20260827)
    parser.add_argument("--v3-iterations", type=int, default=8)
    parser.add_argument("--residual-iterations", type=int, default=3)
    parser.add_argument(
        "--candidates",
        default="global_calibration,single_residual,two_expert,three_expert,port_pair_residual",
        help="comma-separated residual candidates",
    )
    parser.add_argument(
        "--splits",
        default=(
            "random_stratified,spatial_region,long_distance,block_holdout,"
            "gap_stratified,rare_port_pair,boundary_holdout"
        ),
        help="comma-separated fixed validation splits",
    )
    parser.add_argument(
        "--skip-v3-retrain",
        action="store_true",
        help="use the fixed full-public V3 predictions for quick probing only",
    )
    parser.add_argument(
        "--stability-tolerance",
        type=float,
        default=0.005,
        help="maximum allowed held-out acc_score regression on any split",
    )
    parser.add_argument(
        "--minimum-mean-gain",
        type=float,
        default=0.005,
        help="minimum mean acc_score gain needed for selection",
    )
    return parser.parse_args()


def load_module(path: Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path.resolve())
    if spec is None or spec.loader is None:
        raise ImportError(f"cannot import module from {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def splitmix64(values: np.ndarray, seed: int) -> np.ndarray:
    x = values.astype(np.uint64, copy=True) + np.uint64(seed & 0xFFFFFFFFFFFFFFFF)
    x += np.uint64(0x9E3779B97F4A7C15)
    x = (x ^ (x >> np.uint64(30))) * np.uint64(0xBF58476D1CE4E5B9)
    x = (x ^ (x >> np.uint64(27))) * np.uint64(0x94D049BB133111EB)
    return x ^ (x >> np.uint64(31))


def bin_by_edges(values: np.ndarray, edges: Sequence[int]) -> np.ndarray:
    return np.searchsorted(np.asarray(edges, dtype=np.int64), values, side="right").astype(
        np.int32
    )


def build_fixed_splits(
    rows: public_analysis.ParsedRows,
    cheb: np.ndarray,
    block_related: np.ndarray,
    gap_count: np.ndarray,
    boundary_clearance: np.ndarray,
    pair_frequency: np.ndarray,
    seed: int,
) -> tuple[dict[str, tuple[np.ndarray, np.ndarray]], dict[str, Any], np.ndarray]:
    n = rows.success_rows
    line_id = rows.line_no.astype(np.uint64)
    distance_stratum = bin_by_edges(cheb, (8, 16, 32, 64, 128, 256, 368, 488))
    gap_stratum = np.minimum(gap_count.astype(np.int32), 15)
    base_stratum = distance_stratum + 16 * block_related.astype(np.int32) + 32 * gap_stratum
    random_hash = splitmix64(line_id ^ (base_stratum.astype(np.uint64) << np.uint64(32)), seed)
    random_valid = random_hash % np.uint64(5) == 0

    source_region = (rows.sx.astype(np.int32) // 16) * 18 + rows.sy.astype(np.int32) // 32
    target_region = (rows.tx.astype(np.int32) // 16) * 18 + rows.ty.astype(np.int32) // 32
    region_ids = np.arange(8 * 18, dtype=np.uint64)
    held_regions = splitmix64(region_ids, seed ^ 0x5350415449414C) % np.uint64(5) == 0
    spatial_valid = held_regions[source_region] | held_regions[target_region]

    long_threshold = int(np.percentile(cheb, 90))
    long_valid = cheb >= long_threshold

    gap_hash = splitmix64(
        line_id ^ (gap_stratum.astype(np.uint64) << np.uint64(40)), seed ^ 0x474150
    )
    gap_valid = gap_hash % np.uint64(5) == 0

    unique_frequencies, request_counts = np.unique(pair_frequency, return_counts=True)
    cumulative = np.cumsum(request_counts)
    target_rows = max(10_000, int(math.ceil(n * 0.10)))
    rare_index = int(np.searchsorted(cumulative, target_rows, side="left"))
    rare_threshold = int(unique_frequencies[min(rare_index, unique_frequencies.size - 1)])
    rare_valid = pair_frequency <= rare_threshold

    boundary_threshold = 8
    boundary_valid = boundary_clearance <= boundary_threshold

    dx = rows.tx.astype(np.int32) - rows.sx.astype(np.int32)
    dy = rows.ty.astype(np.int32) - rows.sy.astype(np.int32)
    remainder8 = (dx % 8) * 8 + (dy % 8)
    remainder_ids = np.arange(64, dtype=np.uint64)
    held_remainders = (
        splitmix64(remainder_ids, seed ^ 0x4D4143524F38) % np.uint64(5) == 0
    )
    macro8_remainder_valid = held_remainders[remainder8]

    source_phase8 = (rows.sx.astype(np.int32) % 8) * 8 + rows.sy.astype(np.int32) % 8
    target_phase8 = (rows.tx.astype(np.int32) % 8) * 8 + rows.ty.astype(np.int32) % 8
    held_phases = splitmix64(remainder_ids, seed ^ 0x504841534538) % np.uint64(5) == 0
    macro8_phase_valid = held_phases[source_phase8] | held_phases[target_phase8]

    remainder10 = (dx % 10) * 10 + (dy % 10)
    remainder10_ids = np.arange(100, dtype=np.uint64)
    held_remainders10 = (
        splitmix64(remainder10_ids, seed ^ 0x4D4143524F3130) % np.uint64(5) == 0
    )
    macro10_remainder_valid = held_remainders10[remainder10]
    source_phase10 = (rows.sx.astype(np.int32) % 10) * 10 + rows.sy.astype(np.int32) % 10
    target_phase10 = (rows.tx.astype(np.int32) % 10) * 10 + rows.ty.astype(np.int32) % 10
    held_phases10 = (
        splitmix64(remainder10_ids, seed ^ 0x50484153453130) % np.uint64(5) == 0
    )
    macro10_phase_valid = held_phases10[source_phase10] | held_phases10[target_phase10]

    masks = {
        "random_stratified": random_valid,
        "spatial_region": spatial_valid,
        "long_distance": long_valid,
        "block_holdout": block_related,
        "gap_stratified": gap_valid,
        "rare_port_pair": rare_valid,
        "boundary_holdout": boundary_valid,
        "macro8_remainder_holdout": macro8_remainder_valid,
        "macro8_phase_holdout": macro8_phase_valid,
        "macro10_remainder_holdout": macro10_remainder_valid,
        "macro10_phase_holdout": macro10_phase_valid,
    }
    splits: dict[str, tuple[np.ndarray, np.ndarray]] = {}
    manifest: dict[str, Any] = {
        "seed": seed,
        "rows": n,
        "rules": {
            "random_stratified": "splitmix64(line,stratum)%5==0; strata=distance,Block,Gap-count",
            "spatial_region": "validation if Source or Target 16x32 region is in deterministic held-region set",
            "long_distance": f"Chebyshev >= public P90 threshold {long_threshold}",
            "block_holdout": "endpoint closed bounding box intersects any configured Block",
            "gap_stratified": "splitmix64(line,clipped endpoint Gap count)%5==0",
            "rare_port_pair": f"full-port-pair public frequency <= {rare_threshold}, targeting at least 10% of rows",
            "boundary_holdout": f"minimum Source/Target distance to any chip side <= {boundary_threshold}",
            "macro8_remainder_holdout": "validation contains deterministic held-out (dx mod 8,dy mod 8) cells",
            "macro8_phase_holdout": "validation if Source or Target absolute 8x8 phase is held out",
            "macro10_remainder_holdout": "validation contains deterministic held-out (dx mod 10,dy mod 10) cells",
            "macro10_phase_holdout": "validation if Source or Target absolute 10x10 phase is held out",
        },
        "splits": {},
    }
    for name, valid in masks.items():
        valid = np.asarray(valid, dtype=np.bool_)
        train = ~valid
        if not valid.any() or not train.any():
            raise ValueError(f"split {name} has an empty train or validation side")
        splits[name] = (train, valid)
        manifest["splits"][name] = {
            "train_rows": int(train.sum()),
            "validation_rows": int(valid.sum()),
            "validation_ratio": float(valid.mean()),
        }
    return splits, manifest, rare_valid


def build_v3_problem(v3, rows: public_analysis.ParsedRows, arch: public_analysis.Architecture):
    port_names, port_to_id, lines, blocks = v3.load_architecture(
        arch.source_files["port"], arch.source_files["gap"]
    )
    if port_names != arch.port_names or port_to_id != arch.port_to_id:
        raise ValueError("V3 training port order differs from canonical architecture parser")
    sx = rows.sx.astype(np.int32)
    sy = rows.sy.astype(np.int32)
    tx = rows.tx.astype(np.int32)
    ty = rows.ty.astype(np.int32)
    sp = rows.sp.astype(np.int32)
    tp = rows.tp.astype(np.int32)
    golden = rows.delay.astype(np.float64)
    dx, dy = tx - sx, ty - sy
    ax, ay = np.abs(dx), np.abs(dy)
    cheb = np.maximum(ax, ay)
    x_prefix, y_prefix = v3.make_line_prefix(lines)
    line_delay = np.abs(x_prefix[tx] - x_prefix[sx]) + np.abs(y_prefix[ty] - y_prefix[sy])
    target = golden - line_delay
    x = v3.make_base_features(dx, dy)
    groups = v3.build_groups(sx, sy, tx, ty, sp, tp, dx, dy, len(port_names), blocks)
    quad = (
        (dx > 0).astype(np.int32)
        + 2 * (dx < 0).astype(np.int32)
        + 3 * (dy > 0).astype(np.int32)
        + 6 * (dy < 0).astype(np.int32)
    )
    gx = np.minimum(14, ax // 8)
    gy = np.minimum(68, ay // 8)
    geom8 = quad * (15 * 69) + gx * 69 + gy
    ratio = np.minimum(16, (17 * np.minimum(ax, ay)) // (np.maximum(ax, ay) + 1))
    angle = quad * 17 + ratio
    port_pair = sp * len(port_names) + tp
    family_keys = [re.sub(r"\[\d+\]$", "[]", name) for name in port_names]
    family_to_id = {name: index for index, name in enumerate(dict.fromkeys(family_keys))}
    port_to_family = np.asarray([family_to_id[name] for name in family_keys], dtype=np.int32)
    source_family = port_to_family[sp]
    target_family = port_to_family[tp]
    family_count = len(family_to_id)
    port_family_pair = source_family * family_count + target_family
    distance_bin = np.minimum(15, cheb // 32)
    src_distance = (sp * 9 + quad) * 16 + distance_bin
    dst_distance = (tp * 9 + quad) * 16 + distance_bin
    src_region = (sx // 8) * 69 + sy // 8
    dst_region = (tx // 8) * 69 + ty // 8
    rx = (np.sign(dx) * (ax % 8) + 8).astype(np.int32)
    ry = (np.sign(dy) * (ay % 8) + 8).astype(np.int32)
    remainder = rx * 17 + ry
    src_remainder = sp * (17 * 17) + remainder
    dst_remainder = tp * (17 * 17) + remainder
    geom4 = quad * (30 * 138) + np.minimum(29, ax // 4) * 138 + np.minimum(137, ay // 4)
    displacement = (dx + 119) * 1099 + (dy + 549)
    groups.extend(
        [
            ("geom8", geom8, v3.GEOM8_COUNT, 20.0),
            ("angle", angle, v3.ANGLE_COUNT, 20.0),
            ("port_pair", port_pair, len(port_names) ** 2, 20.0),
            ("src_distance", src_distance, len(port_names) * 9 * 16, 20.0),
            ("dst_distance", dst_distance, len(port_names) * 9 * 16, 20.0),
            ("src_region", src_region, v3.REGION_COUNT, 50.0),
            ("dst_region", dst_region, v3.REGION_COUNT, 50.0),
            ("src_remainder", src_remainder, len(port_names) * 17 * 17, 30.0),
            ("dst_remainder", dst_remainder, len(port_names) * 17 * 17, 30.0),
            ("geom4", geom4, v3.GEOM4_COUNT, 30.0),
            ("displacement", displacement, v3.DISPLACEMENT_COUNT, 40.0),
        ]
    )
    static = {
        "sx": sx,
        "sy": sy,
        "tx": tx,
        "ty": ty,
        "sp": sp,
        "tp": tp,
        "dx": dx,
        "dy": dy,
        "ax": ax,
        "ay": ay,
        "cheb": cheb,
        "quad": quad,
        "port_pair": port_pair,
        "source_family": source_family,
        "target_family": target_family,
        "port_family_pair": port_family_pair,
        "port_family_count": family_count,
    }
    return x, target, line_delay, groups, static


def fit_v3_split(
    v3,
    x: np.ndarray,
    target: np.ndarray,
    line_delay: np.ndarray,
    groups,
    train_mask: np.ndarray,
    atlas_mask: np.ndarray,
    atlas_prediction: np.ndarray,
    same_endpoint: np.ndarray,
    iterations: int,
) -> np.ndarray:
    fit_mask = train_mask & ((target + line_delay) > 0)
    _, _, predicted = v3.fit_model(x, target, groups, fit_mask, iterations)
    predicted += line_delay
    predicted[same_endpoint] = 0.0
    predicted = np.floor(np.maximum(predicted, 0.0) + 0.5)
    predicted[atlas_mask] = atlas_prediction[atlas_mask]
    return predicted


def make_residual_groups(
    name: str,
    base: np.ndarray,
    static: dict[str, np.ndarray],
    env: dict[str, np.ndarray],
    rare_mask: np.ndarray,
    port_count: int,
) -> list[Group]:
    cheb = static["cheb"]
    quad = static["quad"]
    block = env["block_related"].astype(np.int32)
    boundary = np.asarray(env["endpoint_boundary_clearance"], dtype=np.int32)
    boundary_bin = bin_by_edges(boundary, (0, 1, 2, 4, 8, 16, 32, 64, 128))
    distance_bin = np.minimum(17, cheb // 32).astype(np.int32)
    prediction_bin = np.minimum(67, base.astype(np.int64) // 128).astype(np.int32)
    gap_h = np.minimum(15, env["horizontal_gap_count"].astype(np.int32))
    gap_v = np.minimum(15, env["vertical_gap_count"].astype(np.int32))
    gap_shape = gap_h * 16 + gap_v
    block_count = np.minimum(7, env["block_count"].astype(np.int32))
    distance_quad = quad * 18 + distance_bin
    block_distance = (block_count * 9 + quad) * 18 + distance_bin

    if name == "global_calibration":
        return [Group("prediction_bin", prediction_bin, 68, 500.0)]
    if name == "single_residual":
        return [
            Group("prediction_bin", prediction_bin, 68, 500.0),
            Group("distance_quad", distance_quad, 9 * 18, 500.0),
            Group("boundary", boundary_bin, 10, 500.0),
            Group("gap_shape", gap_shape, 16 * 16, 500.0),
            Group("block_distance", block_distance, 8 * 9 * 18, 500.0),
            Group("source_port", static["sp"], port_count, 200.0),
            Group("target_port", static["tp"], port_count, 200.0),
        ]
    if name == "two_expert":
        return [
            Group("prediction_expert", prediction_bin * 2 + block, 68 * 2, 500.0),
            Group("distance_expert", distance_quad * 2 + block, 9 * 18 * 2, 500.0),
            Group("boundary_expert", boundary_bin * 2 + block, 10 * 2, 500.0),
            Group("gap_shape", gap_shape, 16 * 16, 500.0),
            Group("block_distance", block_distance, 8 * 9 * 18, 500.0),
            Group("source_port", static["sp"], port_count, 200.0),
            Group("target_port", static["tp"], port_count, 200.0),
        ]
    if name == "three_expert":
        expert = np.where((boundary <= 8) | rare_mask, 2, np.where(block != 0, 1, 0)).astype(
            np.int32
        )
        return [
            Group("prediction_expert", prediction_bin * 3 + expert, 68 * 3, 500.0),
            Group("distance_expert", distance_quad * 3 + expert, 9 * 18 * 3, 500.0),
            Group("boundary_expert", boundary_bin * 3 + expert, 10 * 3, 500.0),
            Group("gap_expert", gap_shape * 3 + expert, 16 * 16 * 3, 500.0),
            Group("block_distance", block_distance, 8 * 9 * 18, 500.0),
            Group("source_port", static["sp"], port_count, 200.0),
            Group("target_port", static["tp"], port_count, 200.0),
        ]
    if name == "port_pair_residual":
        source_distance = (static["sp"] * 9 + quad) * 18 + distance_bin
        target_distance = (static["tp"] * 9 + quad) * 18 + distance_bin
        return [
            Group("prediction_bin", prediction_bin, 68, 500.0),
            Group("distance_quad", distance_quad, 9 * 18, 500.0),
            Group("boundary", boundary_bin, 10, 500.0),
            Group("gap_shape", gap_shape, 16 * 16, 500.0),
            Group("block_distance", block_distance, 8 * 9 * 18, 500.0),
            Group("port_pair", static["port_pair"], port_count * port_count, 30.0),
            Group("source_distance", source_distance, port_count * 9 * 18, 30.0),
            Group("target_distance", target_distance, port_count * 9 * 18, 30.0),
        ]
    if name == "port_family_residual":
        family_count = int(static["port_family_count"])
        source_distance = (static["source_family"] * 9 + quad) * 18 + distance_bin
        target_distance = (static["target_family"] * 9 + quad) * 18 + distance_bin
        return [
            Group("prediction_bin", prediction_bin, 68, 500.0),
            Group("distance_quad", distance_quad, 9 * 18, 500.0),
            Group("boundary", boundary_bin, 10, 500.0),
            Group("gap_shape", gap_shape, 16 * 16, 500.0),
            Group("block_distance", block_distance, 8 * 9 * 18, 500.0),
            Group(
                "port_family_pair",
                static["port_family_pair"],
                family_count * family_count,
                30.0,
            ),
            Group("source_family_distance", source_distance, family_count * 9 * 18, 30.0),
            Group("target_family_distance", target_distance, family_count * 9 * 18, 30.0),
        ]
    macro_periods = {
        "port_family_macro4_endpoint": 4,
        "port_family_macro6_endpoint": 6,
        "port_family_macro8": 8,
        "port_family_macro8_endpoint": 8,
        "port_family_macro9_endpoint": 9,
        "port_family_macro10_endpoint": 10,
        "port_family_macro10_phase": 10,
        "port_family_macro10_family_phase": 10,
        "port_family_macro11_endpoint": 11,
        "port_family_macro12_endpoint": 12,
        "port_family_macro14_endpoint": 14,
        "port_family_macro16_endpoint": 16,
    }
    if name in macro_periods:
        family_count = int(static["port_family_count"])
        period = macro_periods[name]
        cells = period * period
        source_distance = (static["source_family"] * 9 + quad) * 18 + distance_bin
        target_distance = (static["target_family"] * 9 + quad) * 18 + distance_bin
        remainder = (static["dx"] % period) * period + (static["dy"] % period)
        groups = [
            Group("prediction_bin", prediction_bin, 68, 500.0),
            Group("distance_quad", distance_quad, 9 * 18, 500.0),
            Group("boundary", boundary_bin, 10, 500.0),
            Group("gap_shape", gap_shape, 16 * 16, 500.0),
            Group("block_distance", block_distance, 8 * 9 * 18, 500.0),
            Group(
                "port_family_pair",
                static["port_family_pair"],
                family_count * family_count,
                30.0,
            ),
            Group("source_family_distance", source_distance, family_count * 9 * 18, 30.0),
            Group("target_family_distance", target_distance, family_count * 9 * 18, 30.0),
            Group("macro8_direction", quad * cells + remainder, 9 * cells, 100.0),
        ]
        if name != "port_family_macro8":
            groups.extend(
                [
                    Group(
                        "source_family_macro8",
                        static["source_family"] * cells + remainder,
                        family_count * cells,
                        50.0,
                    ),
                    Group(
                        "target_family_macro8",
                        static["target_family"] * cells + remainder,
                        family_count * cells,
                        50.0,
                    ),
                ]
            )
        if name in {"port_family_macro10_phase", "port_family_macro10_family_phase"}:
            source_phase = (static["sx"] % period) * period + static["sy"] % period
            target_phase = (static["tx"] % period) * period + static["ty"] % period
            groups.extend(
                [
                    Group("source_phase", source_phase, cells, 100.0),
                    Group("target_phase", target_phase, cells, 100.0),
                ]
            )
            if name == "port_family_macro10_family_phase":
                groups.extend(
                    [
                        Group(
                            "source_family_phase",
                            static["source_family"] * cells + source_phase,
                            family_count * cells,
                            50.0,
                        ),
                        Group(
                            "target_family_phase",
                            static["target_family"] * cells + target_phase,
                            family_count * cells,
                            50.0,
                        ),
                    ]
                )
        return groups
    if name in {"port_family_pair", "port_family_compact"}:
        family_count = int(static["port_family_count"])
        groups = [
            Group("prediction_bin", prediction_bin, 68, 500.0),
            Group("distance_quad", distance_quad, 9 * 18, 500.0),
            Group("boundary", boundary_bin, 10, 500.0),
            Group("gap_shape", gap_shape, 16 * 16, 500.0),
            Group("block_distance", block_distance, 8 * 9 * 18, 500.0),
            Group(
                "port_family_pair",
                static["port_family_pair"],
                family_count * family_count,
                30.0,
            ),
        ]
        if name == "port_family_compact":
            groups.extend(
                [
                    Group("source_family", static["source_family"], family_count, 100.0),
                    Group("target_family", static["target_family"], family_count, 100.0),
                ]
            )
        return groups
    raise ValueError(f"unknown residual candidate: {name}")


def apply_correction(base: np.ndarray, fit: ResidualFit) -> np.ndarray:
    correction = np.zeros(base.size, dtype=np.float64)
    for group, effect in zip(fit.groups, fit.effects):
        correction += effect[group.code]
    correction *= fit.alpha
    return np.floor(np.maximum(0.0, base * (1.0 + correction)) + 0.5)


def fit_residual(
    name: str,
    base: np.ndarray,
    golden: np.ndarray,
    train_mask: np.ndarray,
    apply_mask: np.ndarray,
    groups: list[Group],
    iterations: int,
) -> ResidualFit:
    selected = train_mask & apply_mask & (base > 0) & (golden > 0)
    if int(selected.sum()) < 100:
        raise ValueError(f"too few residual training rows for {name}")
    target = np.zeros(base.size, dtype=np.float64)
    target[selected] = np.clip((golden[selected] - base[selected]) / base[selected], -0.25, 0.25)
    current = np.zeros(base.size, dtype=np.float64)
    effects = [np.zeros(group.size, dtype=np.float64) for group in groups]
    for _ in range(iterations):
        for index, group in enumerate(groups):
            current -= effects[index][group.code]
            residual = target - current
            code = group.code[selected]
            selected_residual = residual[selected]
            # IRLS approximation to relative L1: the official point loss is
            # nearly linear in small relative error, unlike V3's raw-delay MSE.
            robust_weight = 1.0 / np.maximum(np.abs(selected_residual), 0.0025)
            robust_weight /= robust_weight.mean()
            robust_weight = np.minimum(robust_weight, 20.0)
            sums = np.bincount(
                code,
                weights=robust_weight * selected_residual,
                minlength=group.size,
            )
            counts = np.bincount(code, weights=robust_weight, minlength=group.size)
            effect = sums / (counts + group.shrink)
            effects[index] = np.clip(effect, -0.10, 0.10)
            current += effects[index][group.code]

    best_alpha = 0.0
    best_score = -math.inf
    for alpha in (0.25, 0.50, 0.75, 1.0, 1.25):
        fit = ResidualFit(name, alpha, groups, effects)
        predicted = apply_correction(base, fit)
        score = float(accuracy_metrics(golden[selected], predicted[selected])["acc_score"])
        if score > best_score:
            best_score, best_alpha = score, alpha
    return ResidualFit(name, best_alpha, groups, effects)


def metrics_by_segments(
    golden: np.ndarray,
    predicted: np.ndarray,
    valid: np.ndarray,
    static: dict[str, np.ndarray],
    env: dict[str, np.ndarray],
    atlas_mask: np.ndarray,
    rare_mask: np.ndarray,
) -> dict[str, Any]:
    result: dict[str, Any] = {"overall": accuracy_metrics(golden[valid], predicted[valid])}
    segments: dict[str, Any] = {}

    def add(name: str, mask: np.ndarray) -> None:
        selected = valid & mask
        if selected.any():
            segments[name] = accuracy_metrics(golden[selected], predicted[selected])

    cheb = static["cheb"]
    for low, high, label in DISTANCE_REPORT_BINS:
        mask = cheb >= low
        if high is not None:
            mask &= cheb <= high
        add(f"distance_{label}", mask)
    add("block", env["block_related"])
    add("non_block", ~env["block_related"])
    add("atlas", atlas_mask)
    add("fallback", ~atlas_mask)
    add("rare_port_pair", rare_mask)
    add("common_port_pair", ~rare_mask)
    result["segments"] = segments
    return result


def fallback_other_audit(
    labels: np.ndarray,
    rows: public_analysis.ParsedRows,
    arch: public_analysis.Architecture,
    atlas_radius: int = 56,
) -> dict[str, Any]:
    model = public_analysis.build_local_atlas_port_model(arch)
    reasons: dict[str, int] = {}
    seed_span: dict[str, int] = {}
    indices = np.flatnonzero(labels == 4)
    for row_index in indices:
        sp, tp = int(rows.sp[row_index]), int(rows.tp[row_index])
        dx = int(rows.tx[row_index]) - int(rows.sx[row_index])
        dy = int(rows.ty[row_index]) - int(rows.sy[row_index])
        source_input = int(model["port_to_input"][sp])
        target_input = int(model["port_to_input"][tp])
        seeds: list[tuple[int, int, int]] = []
        if source_input >= 0:
            route = int(model["input_to_route"][source_input])
            if route >= 0:
                seeds.append((route, 0, 0))
            else:
                seeds.extend(
                    (route, sx, sy)
                    for route, sx, sy, _ in model["transitions"][source_input]
                )
        else:
            route, sx, sy = model["output_net"][sp]
            if route >= 0:
                seeds.append((route, sx, sy))
        if not seeds:
            reason = "unsupported_source"
        else:
            direct_possible = dx == 0 and dy == 0 and source_input >= 0 and tp in model["direct"][source_input]
            if target_input >= 0:
                target_route = int(model["input_to_route"][target_input])
                terminal_count = int(target_route >= 0)
            else:
                terminal_count = len(model["target_arcs"][tp])
            if direct_possible:
                reason = "unexpected_direct_miss"
            elif terminal_count == 0:
                reason = "unsupported_target"
            elif not any(
                -atlas_radius <= dx - seed_dx <= atlas_radius
                and -atlas_radius <= dy - seed_dy <= atlas_radius
                for _, seed_dx, seed_dy in seeds
            ):
                reason = "seed_residual_outside_radius"
            else:
                reason = "atlas_unreachable_or_unsupported_combination"
            for _, seed_dx, seed_dy in seeds:
                span = max(abs(seed_dx), abs(seed_dy))
                key = str(span)
                seed_span[key] = seed_span.get(key, 0) + 1
        reasons[reason] = reasons.get(reason, 0) + 1
    return {
        "rows": int(indices.size),
        "reasons": reasons,
        "seed_span_occurrences": seed_span,
        "note": "Reason labels reproduce V3 endpoint/Atlas logic; they do not change predictions.",
    }


def json_default(value):
    if isinstance(value, (np.integer,)):
        return int(value)
    if isinstance(value, (np.floating,)):
        return float(value)
    if isinstance(value, np.ndarray):
        return value.tolist()
    raise TypeError(f"not JSON serializable: {type(value).__name__}")


def write_ablation_csv(path: Path, rows: Iterable[dict[str, Any]]) -> None:
    rows = list(rows)
    fieldnames = [
        "split",
        "candidate",
        "train_rows",
        "validation_rows",
        "v3_acc_score",
        "v4_acc_score",
        "delta_acc_score",
        "v3_mae",
        "v4_mae",
        "v3_rmse",
        "v4_rmse",
        "parameter_count",
        "parameter_bytes",
        "alpha",
        "v3_retrained",
    ]
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def compact_metrics(golden: np.ndarray, predicted: np.ndarray, mask: np.ndarray) -> dict[str, Any]:
    if not mask.any():
        return {"rows": 0}
    metrics = accuracy_metrics(golden[mask], predicted[mask])
    return {
        "rows": metrics["rows"],
        "acc_score": metrics["acc_score"],
        "exact_matches": metrics["exact_matches"],
        "exact_ratio": metrics["exact_matches"] / metrics["rows"],
        "mae": metrics["mae"],
        "p95_absolute_error": metrics["p95_absolute_error"],
    }


def main() -> int:
    args = parse_args()
    if args.v3_iterations <= 0 or args.residual_iterations <= 0:
        raise ValueError("iteration counts must be positive")
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    monitor = public_analysis.PeakMemoryMonitor()
    monitor.start()
    v3 = load_module(args.v3_train_script, "srb_v3_train_for_research")

    arch = public_analysis.load_architecture(args.arch_dir)
    rows = public_analysis.read_queries(args.golden, arch, output_dir, None, args.seed)
    if rows.failure_rows or rows.success_rows != rows.analyzed_rows:
        raise ValueError("Golden parsing failed; see parse_failures.csv")
    golden_from_csv, production_v3 = load_aligned_csv(args.golden, args.v3_predictions)
    golden = rows.delay.astype(np.float64)
    if not np.array_equal(golden_from_csv, golden):
        raise ValueError("canonical parser and strict scorer disagree on Golden delays")

    env = public_analysis.classify_environment(rows, arch)
    sx, sy, tx, ty = (a.astype(np.int32) for a in (rows.sx, rows.sy, rows.tx, rows.ty))
    endpoint_boundary = np.minimum.reduce(
        [sx, sy, tx, ty, arch.width - 1 - sx, arch.height - 1 - sy,
         arch.width - 1 - tx, arch.height - 1 - ty]
    ).astype(np.int16)
    env["endpoint_boundary_clearance"] = endpoint_boundary
    cheb = np.maximum(np.abs(tx - sx), np.abs(ty - sy))
    labels, atlas_metadata = public_analysis.classify_v3_from_atlas(
        args.atlas_file, rows, arch, cheb, env["block_related"]
    )
    atlas_mask = labels == 1
    fallback_mask = ~atlas_mask
    print(
        f"loaded={rows.success_rows:,} atlas={int(atlas_mask.sum()):,} "
        f"fallback={int(fallback_mask.sum()):,}",
        flush=True,
    )

    pair_key = rows.sp.astype(np.int64) * len(arch.port_names) + rows.tp.astype(np.int64)
    _, inverse, counts = np.unique(pair_key, return_inverse=True, return_counts=True)
    pair_frequency = counts[inverse]
    splits, split_manifest, rare_mask = build_fixed_splits(
        rows,
        cheb,
        env["block_related"],
        env["gap_count"],
        endpoint_boundary,
        pair_frequency,
        args.seed,
    )
    requested_splits = [item.strip() for item in args.splits.split(",") if item.strip()]
    missing_splits = [name for name in requested_splits if name not in splits]
    if missing_splits:
        raise ValueError(f"unknown split(s): {missing_splits}")
    requested_candidates = [item.strip() for item in args.candidates.split(",") if item.strip()]
    allowed_candidates = {
        "global_calibration",
        "single_residual",
        "two_expert",
        "three_expert",
        "port_pair_residual",
        "port_family_residual",
        "port_family_pair",
        "port_family_compact",
        "port_family_macro8",
        "port_family_macro8_endpoint",
        "port_family_macro4_endpoint",
        "port_family_macro6_endpoint",
        "port_family_macro9_endpoint",
        "port_family_macro10_endpoint",
        "port_family_macro10_phase",
        "port_family_macro10_family_phase",
        "port_family_macro11_endpoint",
        "port_family_macro12_endpoint",
        "port_family_macro14_endpoint",
        "port_family_macro16_endpoint",
    }
    if not set(requested_candidates).issubset(allowed_candidates):
        raise ValueError(f"unknown candidate(s): {set(requested_candidates) - allowed_candidates}")

    packed_splits = {f"valid_{name}": np.packbits(valid) for name, (_, valid) in splits.items()}
    packed_splits["line_number"] = rows.line_no
    np.savez_compressed(output_dir / "v4_fixed_splits.npz", **packed_splits)
    (output_dir / "v4_split_manifest.json").write_text(
        json.dumps(split_manifest, ensure_ascii=False, indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )

    x, target, line_delay, v3_groups, static = build_v3_problem(v3, rows, arch)
    same_endpoint = (
        (static["sx"] == static["tx"])
        & (static["sy"] == static["ty"])
        & (static["sp"] == static["tp"])
    )

    atlas_metrics = accuracy_metrics(golden[atlas_mask], production_v3[atlas_mask])
    atlas_boundary = {}
    for threshold in (0, 1, 2, 4, 8, 12, 16, 24, 32, 48):
        mask = atlas_mask & (endpoint_boundary >= threshold)
        atlas_boundary[f"clearance_at_least_{threshold}"] = compact_metrics(
            golden, production_v3, mask
        )
    atlas_gap = {
        "no_endpoint_gap": compact_metrics(golden, production_v3, atlas_mask & ~env["gap_related"]),
        "endpoint_gap": compact_metrics(golden, production_v3, atlas_mask & env["gap_related"]),
        "horizontal_only": compact_metrics(
            golden,
            production_v3,
            atlas_mask & (env["horizontal_gap_count"] > 0) & (env["vertical_gap_count"] == 0),
        ),
        "vertical_only": compact_metrics(
            golden,
            production_v3,
            atlas_mask & (env["horizontal_gap_count"] == 0) & (env["vertical_gap_count"] > 0),
        ),
        "both": compact_metrics(
            golden,
            production_v3,
            atlas_mask & (env["horizontal_gap_count"] > 0) & (env["vertical_gap_count"] > 0),
        ),
    }
    v3_audit = {
        "architecture_fingerprint_sha256": arch.fingerprint_sha256,
        "architecture_file_sha256": {
            key: hashlib.sha256(path.read_bytes()).hexdigest()
            for key, path in arch.source_files.items()
        },
        "atlas_file_sha256": hashlib.sha256(args.atlas_file.read_bytes()).hexdigest(),
        "atlas_metadata": atlas_metadata,
        "atlas_public_golden_metrics": atlas_metrics,
        "atlas_public_golden_exact": bool(atlas_metrics["exact_matches"] == atlas_metrics["rows"]),
        "atlas_public_golden_by_endpoint_boundary_clearance": atlas_boundary,
        "atlas_public_golden_by_endpoint_gap_geometry": atlas_gap,
        "production_v3_metrics": accuracy_metrics(golden, production_v3),
        "branch_counts": {
            public_analysis.BRANCH_NAMES[int(code)]: int(np.count_nonzero(labels == code))
            for code in sorted(public_analysis.BRANCH_NAMES)
            if np.any(labels == code)
        },
        "fallback_other": fallback_other_audit(labels, rows, arch),
        "net_span": {
            "configured_maximum": arch.max_net_span,
            "v3_declared_maximum": 8,
            "current_atlas_radius": int(atlas_metadata["header"]["radius"]),
            "current_atlas_bytes": int(atlas_metadata["header"]["bytes"]),
            "radius_60_projected_bytes": int(40 + 160 * 121 * 121 * 160 * 2),
        },
        "v3_model": {
            "formula": "11-term directional linear base plus 18 additive categorical tables and endpoint Gap prefixes",
            "float_parameter_count": 996950,
            "float_parameter_bytes": 996950 * 4,
            "training": "8-iteration residual back-fitting; public all-row final fit; raw-delay squared-error objective",
        },
    }
    (output_dir / "v3_audit.json").write_text(
        json.dumps(v3_audit, ensure_ascii=False, indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )

    ablation_rows: list[dict[str, Any]] = []
    detailed: dict[str, Any] = {
        "contract": asdict(DEFAULT_CONTRACT),
        "methodology": {
            "v3_retrained_per_split": not args.skip_v3_retrain,
            "atlas_predictions": "unchanged production V3 Atlas results; architecture-derived and not fitted",
            "residual_training": "training side only; fallback branch only; validation never fits effects or alpha",
            "selection_rule": {
                "stability_tolerance_acc_points": args.stability_tolerance,
                "minimum_mean_gain_acc_points": args.minimum_mean_gain,
            },
        },
        "splits": {},
    }
    candidate_deltas: dict[str, list[float]] = {name: [] for name in requested_candidates}
    for split_name in requested_splits:
        train, valid = splits[split_name]
        print(
            f"split={split_name} train={int(train.sum()):,} valid={int(valid.sum()):,} "
            f"retrain_v3={not args.skip_v3_retrain}",
            flush=True,
        )
        if args.skip_v3_retrain:
            split_v3 = production_v3.copy()
        else:
            split_v3 = fit_v3_split(
                v3,
                x,
                target,
                line_delay,
                v3_groups,
                train,
                atlas_mask,
                production_v3,
                same_endpoint,
                args.v3_iterations,
            )
        base_report = metrics_by_segments(
            golden, split_v3, valid, static, env, atlas_mask, rare_mask
        )
        split_detail: dict[str, Any] = {
            "train_rows": int(train.sum()),
            "validation_rows": int(valid.sum()),
            "v3": base_report,
            "candidates": {},
        }
        for candidate in requested_candidates:
            groups = make_residual_groups(
                candidate, split_v3, static, env, rare_mask, len(arch.port_names)
            )
            fit = fit_residual(
                candidate,
                split_v3,
                golden,
                train,
                fallback_mask,
                groups,
                args.residual_iterations,
            )
            v4_prediction = split_v3.copy()
            corrected = apply_correction(split_v3, fit)
            v4_prediction[fallback_mask] = corrected[fallback_mask]
            report = metrics_by_segments(
                golden, v4_prediction, valid, static, env, atlas_mask, rare_mask
            )
            base_overall = base_report["overall"]
            v4_overall = report["overall"]
            delta = float(v4_overall["acc_score"] - base_overall["acc_score"])
            candidate_deltas[candidate].append(delta)
            print(
                f"  candidate={candidate} delta_acc={delta:+.6f} "
                f"parameters={fit.parameter_count:,} alpha={fit.alpha:.2f}",
                flush=True,
            )
            split_detail["candidates"][candidate] = {
                "alpha": fit.alpha,
                "parameter_count": fit.parameter_count,
                "parameter_bytes": fit.parameter_bytes,
                "delta_acc_score": delta,
                "metrics": report,
            }
            ablation_rows.append(
                {
                    "split": split_name,
                    "candidate": candidate,
                    "train_rows": int(train.sum()),
                    "validation_rows": int(valid.sum()),
                    "v3_acc_score": base_overall["acc_score"],
                    "v4_acc_score": v4_overall["acc_score"],
                    "delta_acc_score": delta,
                    "v3_mae": base_overall["mae"],
                    "v4_mae": v4_overall["mae"],
                    "v3_rmse": base_overall["rmse"],
                    "v4_rmse": v4_overall["rmse"],
                    "parameter_count": fit.parameter_count,
                    "parameter_bytes": fit.parameter_bytes,
                    "alpha": fit.alpha,
                    "v3_retrained": not args.skip_v3_retrain,
                }
            )
        detailed["splits"][split_name] = split_detail

    selection: dict[str, Any] = {"selected": None, "candidates": {}}
    eligible: list[tuple[float, int, str]] = []
    parameter_counts = {
        name: next(row["parameter_count"] for row in ablation_rows if row["candidate"] == name)
        for name in requested_candidates
    }
    for name, deltas in candidate_deltas.items():
        mean_gain = float(np.mean(deltas))
        minimum_gain = float(np.min(deltas))
        stable = minimum_gain >= -args.stability_tolerance and mean_gain >= args.minimum_mean_gain
        selection["candidates"][name] = {
            "mean_delta_acc_score": mean_gain,
            "minimum_delta_acc_score": minimum_gain,
            "maximum_delta_acc_score": float(np.max(deltas)),
            "stable": stable,
            "parameter_count": parameter_counts[name],
        }
        if stable:
            eligible.append((mean_gain, -parameter_counts[name], name))
    if eligible:
        eligible.sort(reverse=True)
        selection["selected"] = eligible[0][2]
    else:
        selection["decision"] = "no residual candidate enters the production path"
    detailed["selection"] = selection

    write_ablation_csv(output_dir / "v4_ablation.csv", ablation_rows)
    (output_dir / "v4_validation.json").write_text(
        json.dumps(detailed, ensure_ascii=False, indent=2, default=json_default) + "\n",
        encoding="utf-8",
    )
    elapsed = time.perf_counter() - started
    peak = monitor.stop()
    run = {
        "elapsed_seconds": elapsed,
        "peak_rss_mib": peak / (1024.0 * 1024.0),
        "rows": rows.success_rows,
        "requested_splits": requested_splits,
        "requested_candidates": requested_candidates,
        "selected_candidate": selection["selected"],
    }
    (output_dir / "v4_research_run.json").write_text(
        json.dumps(run, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(run, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
