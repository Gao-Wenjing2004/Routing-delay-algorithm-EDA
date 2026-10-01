#!/usr/bin/env python3
"""Train compact V8 P2 residual students without shipping an Atlas.

The runtime baseline is the no-Atlas V6/P0 prediction.  Golden labels are used
only on the training side of the fixed split.  An optional radius-56 prediction
file is treated as an architecture/Dijkstra teacher: its correction is fitted
without reading Golden labels and then evaluated on the same untouched split.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict
import json
import math
from pathlib import Path
import sys
import time

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze_public_queries as public_analysis  # noqa: E402
import srb_v4_research as research  # noqa: E402
from srb_score import accuracy_metrics, load_aligned_csv  # noqa: E402


def parse_args() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", required=True, type=Path)
    parser.add_argument("--arch-dir", required=True, type=Path)
    parser.add_argument("--base", required=True, type=Path,
                        help="no-Atlas prediction CSV used by the runtime student")
    parser.add_argument("--teacher", type=Path,
                        help="optional radius-56/Dijkstra-backed prediction CSV")
    parser.add_argument("--p1", type=Path,
                        help="optional architecture-only PRP P1 prediction feature")
    parser.add_argument("--aux-p0", type=Path,
                        help="optional original no-Atlas P0 prediction feature")
    parser.add_argument("--aux-structure", type=Path,
                        help="optional V5 architecture/Dijkstra-geometry prediction feature")
    parser.add_argument("--output-dir", type=Path,
                        default=root / "analysis" / "p2")
    parser.add_argument("--seed", type=int, default=20260827)
    parser.add_argument("--iterations", type=int, default=4)
    parser.add_argument("--trees", type=int, default=64,
                        help="number of shallow histogram-boosted Student trees; 0 disables")
    parser.add_argument("--lgb-learning-rate", type=float, default=0.12)
    parser.add_argument(
        "--candidates",
        default=("global_calibration,single_residual,port_family_residual,"
                 "port_family_macro10_family_phase,port_pair_residual"),
    )
    return parser.parse_args()


def compact_metrics(golden: np.ndarray, predicted: np.ndarray, mask: np.ndarray) -> dict[str, object]:
    m = accuracy_metrics(golden[mask], predicted[mask])
    return {
        "rows": m["rows"],
        "acc_score": m["acc_score"],
        "mae": m["mae"],
        "rmse": m["rmse"],
        "mean_relative_error": m["mean_relative_error"],
        "median_relative_error": m["median_relative_error"],
        "p50_absolute_error": m["p50_absolute_error"],
        "p90_absolute_error": m["p90_absolute_error"],
        "p95_absolute_error": m["p95_absolute_error"],
        "p99_absolute_error": m["p99_absolute_error"],
        "max_absolute_error": m["max_absolute_error"],
        "within_5pct": m["within_5pct"],
        "within_10pct": m["within_10pct"],
    }


def build_features(rows, arch):
    sx, sy, tx, ty = (a.astype(np.int32) for a in (rows.sx, rows.sy, rows.tx, rows.ty))
    sp, tp = rows.sp.astype(np.int32), rows.tp.astype(np.int32)
    dx, dy = tx - sx, ty - sy
    ax, ay = np.abs(dx), np.abs(dy)
    cheb = np.maximum(ax, ay)
    quad = ((dx > 0).astype(np.int32) + 2 * (dx < 0).astype(np.int32)
            + 3 * (dy > 0).astype(np.int32) + 6 * (dy < 0).astype(np.int32))

    import re
    family_keys = [re.sub(r"\[\d+\]$", "[]", name) for name in arch.port_names]
    family_to_id = {name: i for i, name in enumerate(dict.fromkeys(family_keys))}
    port_to_family = np.asarray([family_to_id[name] for name in family_keys], dtype=np.int32)
    source_family, target_family = port_to_family[sp], port_to_family[tp]
    family_count = len(family_to_id)
    static = {
        "sx": sx, "sy": sy, "tx": tx, "ty": ty,
        "sp": sp, "tp": tp, "dx": dx, "dy": dy,
        "ax": ax, "ay": ay, "cheb": cheb, "quad": quad,
        "port_pair": sp * len(arch.port_names) + tp,
        "source_family": source_family,
        "target_family": target_family,
        "port_family_pair": source_family * family_count + target_family,
        "port_family_count": family_count,
    }
    prpr_path = REPO_ROOT / "srb_prpr_v8" / "analysis" / "prpr" / "prpr_model.json"
    if prpr_path.is_file():
        prpr = json.loads(prpr_path.read_text(encoding="utf-8"))
        if list(prpr["port_names"]) != list(arch.port_names):
            raise ValueError("PRP port order differs from architecture")
        state_classes = [int(value) for value in prpr["state_classes"]]
        source_class, target_class = [], []
        source_states, target_states = [], []
        for pid in range(len(arch.port_names)):
            iid = int(prpr["port_to_input"][pid])
            if iid >= 0:
                state = int(prpr["input_to_state"][iid])
                source_state = target_state = state
            else:
                source_state = int(prpr["output_nets"][pid][0])
                targets = prpr["target_arcs"][pid]
                target_state = int(min(targets, key=lambda row: (int(row[1]), int(row[0])))[0]) if targets else -1
            source_class.append(state_classes[source_state] if source_state >= 0 else 16)
            target_class.append(state_classes[target_state] if target_state >= 0 else 16)
            source_states.append(source_state if source_state >= 0 else 160)
            target_states.append(target_state if target_state >= 0 else 160)
        source_class = np.asarray(source_class, dtype=np.int32)
        target_class = np.asarray(target_class, dtype=np.int32)
        static["source_state_class"] = source_class[sp]
        static["target_state_class"] = target_class[tp]
        static["source_route_state"] = np.asarray(source_states, dtype=np.int32)[sp]
        static["target_route_state"] = np.asarray(target_states, dtype=np.int32)[tp]
    env = public_analysis.classify_environment(rows, arch)
    env["endpoint_boundary_clearance"] = np.minimum.reduce([
        sx, sy, tx, ty,
        arch.width - 1 - sx, arch.height - 1 - sy,
        arch.width - 1 - tx, arch.height - 1 - ty,
    ]).astype(np.int16)
    return static, env


def fixed_split(rows, static, env, port_count: int, seed: int):
    pair_key = rows.sp.astype(np.int64) * port_count + rows.tp.astype(np.int64)
    _, inverse, counts = np.unique(pair_key, return_inverse=True, return_counts=True)
    pair_frequency = counts[inverse]
    splits, manifest, rare = research.build_fixed_splits(
        rows, static["cheb"], env["block_related"], env["gap_count"],
        env["endpoint_boundary_clearance"], pair_frequency, seed,
    )
    train, valid = splits["random_stratified"]
    return train, valid, rare, manifest


def fit_relative_target(
    name: str,
    relative_target: np.ndarray,
    selected: np.ndarray,
    groups: list[research.Group],
    iterations: int,
) -> research.ResidualFit:
    current = np.zeros(relative_target.size, dtype=np.float64)
    effects = [np.zeros(group.size, dtype=np.float64) for group in groups]
    for _ in range(iterations):
        for index, group in enumerate(groups):
            current -= effects[index][group.code]
            residual = relative_target - current
            code = group.code[selected]
            selected_residual = residual[selected]
            robust = 1.0 / np.maximum(np.abs(selected_residual), 0.0025)
            robust /= robust.mean()
            robust = np.minimum(robust, 20.0)
            sums = np.bincount(code, weights=robust * selected_residual,
                               minlength=group.size)
            counts = np.bincount(code, weights=robust, minlength=group.size)
            effects[index] = np.clip(sums / (counts + group.shrink), -0.25, 0.25)
            current += effects[index][group.code]
    return research.ResidualFit(name, 1.0, groups, effects)


def teacher_groups(static: dict[str, np.ndarray], family_count: int) -> list[research.Group]:
    # The exact displacement table is small (129x129) and independent of ports;
    # unlike an Atlas it does not contain a port-pair dimension.
    dx = np.clip(static["dx"], -64, 64) + 64
    dy = np.clip(static["dy"], -64, 64) + 64
    displacement = dx * 129 + dy
    direction = static["quad"]
    port_count = int(max(static["sp"].max(), static["tp"].max())) + 1
    remainder8 = (static["dx"] % 8) * 8 + static["dy"] % 8
    distance_bin = np.minimum(8, static["cheb"] // 8)
    source_direction = static["sp"] * 9 + direction
    target_direction = static["tp"] * 9 + direction
    source_family_distance = (static["source_family"] * 9 + direction) * 9 + distance_bin
    target_family_distance = (static["target_family"] * 9 + direction) * 9 + distance_bin
    return [
        # Teacher labels come from the architecture, not Golden, so these can
        # use much lighter shrinkage than a fitted Golden residual.
        research.Group("teacher_displacement", displacement, 129 * 129, 3.0),
        research.Group("teacher_direction_remainder8", direction * 64 + remainder8,
                       9 * 64, 5.0),
        research.Group("teacher_source_port_direction", source_direction, port_count * 9, 8.0),
        research.Group("teacher_target_port_direction", target_direction, port_count * 9, 8.0),
        research.Group("teacher_source_family_distance", source_family_distance,
                       family_count * 9 * 9, 5.0),
        research.Group("teacher_target_family_distance", target_family_distance,
                       family_count * 9 * 9, 5.0),
        research.Group("teacher_source_family", static["source_family"], family_count, 5.0),
        research.Group("teacher_target_family", static["target_family"], family_count, 5.0),
        research.Group("teacher_family_pair", static["port_family_pair"],
                       family_count * family_count, 3.0),
    ]


def fit_teacher_student(
    base: np.ndarray,
    teacher: np.ndarray,
    static: dict[str, np.ndarray],
    env: dict[str, np.ndarray],
    iterations: int,
) -> tuple[np.ndarray, research.ResidualFit, np.ndarray]:
    gate = ((base > 0) & (teacher > 0)
            & (static["cheb"] <= 72) & ~env["block_related"])
    active = gate & (teacher != base)
    relative = np.zeros(base.size, dtype=np.float64)
    relative[gate] = np.clip((teacher[gate] - base[gate]) / base[gate], -0.25, 0.25)
    groups = teacher_groups(static, int(static["port_family_count"]))
    fit = fit_relative_target("dijkstra_teacher_student", relative, gate, groups, iterations)
    correction = np.zeros(base.size, dtype=np.float64)
    for group, effect in zip(fit.groups, fit.effects):
        correction += effect[group.code]
    correction[~gate] = 0.0
    predicted = np.floor(np.maximum(0.0, base * (1.0 + correction)) + 0.5)
    return predicted, fit, active


def tree_features(static: dict[str, np.ndarray], env: dict[str, np.ndarray], base: np.ndarray):
    """Return compact runtime features and a categorical-column mask."""
    cheb = static["cheb"]
    minimum = np.minimum(static["ax"], static["ay"])
    ratio = minimum / np.maximum(cheb, 1)
    source_phase10 = (static["sx"] % 10) * 10 + static["sy"] % 10
    target_phase10 = (static["tx"] % 10) * 10 + static["ty"] % 10
    remainder8 = (static["dx"] % 8) * 8 + static["dy"] % 8
    remainder10 = (static["dx"] % 10) * 10 + static["dy"] % 10
    distance_bin = np.minimum(17, cheb // 32)
    source_bus = static["sp"] % 8
    target_bus = static["tp"] % 8
    p1 = static.get("p1_prediction", base)
    aux_p0 = static.get("aux_p0_prediction", base)
    aux_structure = static.get("aux_structure_prediction", base)
    columns = [
        base / 4096.0,
        static["dx"] / 600.0,
        static["dy"] / 600.0,
        static["ax"] / 600.0,
        static["ay"] / 600.0,
        cheb / 600.0,
        ratio,
        env["endpoint_boundary_clearance"] / 128.0,
        env["gap_delay"] / 512.0,
        p1 / 4096.0,
        (p1 - base) / 4096.0,
        p1 / np.maximum(base, 1.0),
        aux_p0 / 4096.0,
        (aux_p0 - base) / 4096.0,
        aux_p0 / np.maximum(base, 1.0),
        aux_structure / 4096.0,
        (aux_structure - base) / 4096.0,
        aux_structure / np.maximum(base, 1.0),
        static["source_family"],
        static["target_family"],
        static["quad"],
        remainder8,
        remainder10,
        source_phase10,
        target_phase10,
        env["block_mask"].astype(np.int32),
        env["block_count"],
        env["horizontal_gap_count"],
        env["vertical_gap_count"],
        distance_bin,
        source_bus,
        target_bus,
        static["sp"] / 496.0,
        static["tp"] / 496.0,
        (static["sp"] * 503 + static["tp"]) % 251,
        (static["source_family"] * 223 + static["target_family"]) % 251,
        static.get("source_state_class", np.full(base.size, 16, dtype=np.int32)),
        static.get("target_state_class", np.full(base.size, 16, dtype=np.int32)),
        (static.get("source_state_class", np.full(base.size, 16, dtype=np.int32)) * 17
         + static.get("target_state_class", np.full(base.size, 16, dtype=np.int32))) % 251,
        static.get("source_route_state", np.full(base.size, 160, dtype=np.int32)),
        static.get("target_route_state", np.full(base.size, 160, dtype=np.int32)),
        (static.get("source_route_state", np.full(base.size, 160, dtype=np.int32)) * 163
         + static.get("target_route_state", np.full(base.size, 160, dtype=np.int32))) % 251,
    ]
    names = [
        "base", "dx", "dy", "abs_dx", "abs_dy", "cheb", "axis_ratio",
        "boundary", "gap_delay", "p1_delay", "p1_minus_base", "p1_over_base",
        "aux_p0_delay", "aux_p0_minus_base", "aux_p0_over_base",
        "aux_structure_delay", "aux_structure_minus_base", "aux_structure_over_base",
        "source_family", "target_family", "direction",
        "remainder8", "remainder10", "source_phase10", "target_phase10",
        "block_mask", "block_count", "horizontal_gap_count", "vertical_gap_count",
        "distance_bin", "source_bus_index", "target_bus_index",
        "source_port_order", "target_port_order", "port_pair_hash251",
        "family_pair_hash251", "source_state_class", "target_state_class",
        "state_pair_hash251",
        "source_route_state", "target_route_state", "route_pair_hash251",
    ]
    # The first 18 columns are continuous; port/family/state identities and
    # periodic cells are categorical.
    categorical = np.asarray([False] * 18 + [True] * 14
                             + [False, False, True, True, True, True, True,
                                True, True, True],
                             dtype=np.bool_)
    return np.column_stack(columns).astype(np.float32), names, categorical


def fit_tree_student(
    name: str,
    base: np.ndarray,
    target_delay: np.ndarray,
    fit_mask: np.ndarray,
    apply_mask: np.ndarray,
    static: dict[str, np.ndarray],
    env: dict[str, np.ndarray],
    trees: int,
    seed: int,
):
    from sklearn.ensemble import HistGradientBoostingRegressor

    x, names, categorical = tree_features(static, env, base)
    y = np.zeros(base.size, dtype=np.float64)
    usable = fit_mask & (base > 0) & (target_delay > 0)
    y[usable] = np.clip((target_delay[usable] - base[usable]) / base[usable], -0.25, 0.25)
    model = HistGradientBoostingRegressor(
        loss="absolute_error",
        learning_rate=0.06,
        max_iter=trees,
        max_leaf_nodes=63,
        max_depth=8,
        min_samples_leaf=40,
        l2_regularization=3.0,
        max_bins=255,
        categorical_features=categorical,
        early_stopping=False,
        random_state=seed,
    )
    model.fit(x[usable], y[usable])
    correction = model.predict(x)
    correction[~apply_mask] = 0.0
    predicted = np.floor(np.maximum(0.0, base * (1.0 + correction)) + 0.5)
    metadata = {
        "name": name,
        "feature_names": names,
        "categorical_features": categorical.tolist(),
        "trees": trees,
        "learning_rate": model.learning_rate,
        "max_leaf_nodes": model.max_leaf_nodes,
        "max_depth": model.max_depth,
        "fit_rows": int(usable.sum()),
        "apply_rows": int(apply_mask.sum()),
        "model": model,
    }
    return predicted, metadata


def fit_lightgbm_student(
    name: str,
    base: np.ndarray,
    target_delay: np.ndarray,
    fit_mask: np.ndarray,
    apply_mask: np.ndarray,
    static: dict[str, np.ndarray],
    env: dict[str, np.ndarray],
    trees: int,
    seed: int,
    learning_rate: float = 0.12,
):
    import lightgbm as lgb

    x, names, categorical = tree_features(static, env, base)
    usable = fit_mask & (base > 0) & (target_delay > 0)
    y = np.clip((target_delay[usable] - base[usable]) / base[usable], -0.25, 0.25)
    model = lgb.LGBMRegressor(
        objective="regression_l1",
        n_estimators=trees,
        learning_rate=learning_rate,
        num_leaves=63,
        max_depth=8,
        min_child_samples=60,
        subsample=1.0,
        colsample_bytree=0.9,
        reg_alpha=0.0005,
        reg_lambda=2.0,
        max_bin=255,
        random_state=seed,
        deterministic=True,
        force_col_wise=True,
        n_jobs=1,
        verbosity=-1,
    )
    categorical_indices = np.flatnonzero(categorical).tolist()
    model.fit(x[usable], y, categorical_feature=categorical_indices)
    correction = model.predict(x)
    correction[~apply_mask] = 0.0
    predicted = np.floor(np.maximum(0.0, base * (1.0 + correction)) + 0.5)
    metadata = {
        "name": name,
        "feature_names": names,
        "categorical_features": categorical.tolist(),
        "trees": trees,
        "fit_rows": int(usable.sum()),
        "apply_rows": int(apply_mask.sum()),
        "model": model,
    }
    return predicted, metadata


def serialise_fit(fit: research.ResidualFit) -> dict[str, object]:
    return {
        "name": fit.name,
        "alpha": fit.alpha,
        "parameter_count": fit.parameter_count,
        "parameter_bytes_float32": fit.parameter_bytes,
        "groups": [
            {"name": group.name, "size": group.size, "shrink": group.shrink,
             "values": effect.astype(np.float32).tolist()}
            for group, effect in zip(fit.groups, fit.effects)
        ],
    }


def main() -> int:
    opt = parse_args()
    started = time.perf_counter()
    out = opt.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    arch = public_analysis.load_architecture(opt.arch_dir)
    rows = public_analysis.read_queries(opt.golden, arch, out, None, opt.seed)
    if rows.failure_rows:
        raise ValueError("Golden parsing failed; inspect parse_failures.csv")
    golden_csv, base = load_aligned_csv(opt.golden, opt.base)
    golden = rows.delay.astype(np.float64)
    if not np.array_equal(golden_csv, golden):
        raise ValueError("Golden parser mismatch")
    static, env = build_features(rows, arch)
    if opt.p1:
        _, p1_prediction = load_aligned_csv(opt.golden, opt.p1)
        static["p1_prediction"] = p1_prediction
    if opt.aux_p0:
        _, aux_p0_prediction = load_aligned_csv(opt.golden, opt.aux_p0)
        static["aux_p0_prediction"] = aux_p0_prediction
    if opt.aux_structure:
        _, aux_structure_prediction = load_aligned_csv(opt.golden, opt.aux_structure)
        static["aux_structure_prediction"] = aux_structure_prediction
    train, valid, rare, split_manifest = fixed_split(
        rows, static, env, len(arch.port_names), opt.seed
    )

    results: dict[str, object] = {
        "data": {
            "rows": rows.success_rows,
            "training_rows": int(train.sum()),
            "validation_rows": int(valid.sum()),
            "architecture_sha256": arch.fingerprint_sha256,
            "base": str(opt.base),
            "teacher": str(opt.teacher) if opt.teacher else None,
        },
        "split": split_manifest,
        "models": {},
    }
    models: dict[str, research.ResidualFit] = {}
    tree_models: dict[str, dict[str, object]] = {}
    predictions: dict[str, np.ndarray] = {"base_no_atlas": base}

    teacher_fit = None
    teacher_active = None
    dijkstra_lgb_prediction = None
    if opt.teacher:
        _, teacher = load_aligned_csv(opt.golden, opt.teacher)
        teacher_prediction, teacher_fit, teacher_active = fit_teacher_student(
            base, teacher, static, env, opt.iterations
        )
        predictions["teacher_student"] = teacher_prediction
        models["teacher_student"] = teacher_fit
        if opt.trees:
            teacher_gate = ((base > 0) & (teacher > 0)
                            & (static["cheb"] <= 72) & ~env["block_related"])
            tree_prediction, tree_meta = fit_tree_student(
                "dijkstra_tree_student", base, teacher, teacher_gate, teacher_gate,
                static, env, opt.trees, opt.seed ^ 0xD1A57A,
            )
            predictions["dijkstra_tree_student"] = tree_prediction
            tree_models["dijkstra_tree_student"] = tree_meta
            lgb_prediction, lgb_meta = fit_lightgbm_student(
                "dijkstra_lgbm_student", base, teacher, teacher_gate, teacher_gate,
                static, env, opt.trees, opt.seed ^ 0xD1A57A, opt.lgb_learning_rate,
            )
            predictions["dijkstra_lgbm_student"] = lgb_prediction
            tree_models["dijkstra_lgbm_student"] = lgb_meta
            dijkstra_lgb_prediction = lgb_prediction

    requested = [name.strip() for name in opt.candidates.split(",") if name.strip()]
    allowed = {
        "global_calibration", "single_residual", "port_family_residual",
        "port_family_macro10_family_phase", "port_pair_residual",
    }
    if not set(requested).issubset(allowed):
        raise ValueError(f"unknown candidates: {set(requested) - allowed}")

    for name in requested:
        groups = research.make_residual_groups(
            name, base, static, env, rare, len(arch.port_names)
        )
        fit = research.fit_residual(
            name, base, golden, train, np.ones(base.size, dtype=np.bool_),
            groups, opt.iterations,
        )
        models[name] = fit
        predictions[name] = research.apply_correction(base, fit)

    if opt.trees:
        tree_prediction, tree_meta = fit_tree_student(
            "golden_tree_student", base, golden, train,
            np.ones(base.size, dtype=np.bool_), static, env, opt.trees, opt.seed,
        )
        predictions["golden_tree_student"] = tree_prediction
        tree_models["golden_tree_student"] = tree_meta
        lgb_prediction, lgb_meta = fit_lightgbm_student(
            "golden_lgbm_student", base, golden, train,
            np.ones(base.size, dtype=np.bool_), static, env, opt.trees, opt.seed,
            opt.lgb_learning_rate,
        )
        predictions["golden_lgbm_student"] = lgb_prediction
        tree_models["golden_lgbm_student"] = lgb_meta
        if dijkstra_lgb_prediction is not None:
            composed_prediction, composed_meta = fit_lightgbm_student(
                "dijkstra_plus_golden_lgbm", dijkstra_lgb_prediction, golden,
                train, np.ones(base.size, dtype=np.bool_), static, env,
                opt.trees, opt.seed ^ 0xC0A905E, opt.lgb_learning_rate,
            )
            composed_meta["prefix_model"] = "dijkstra_lgbm_student"
            predictions["dijkstra_plus_golden_lgbm"] = composed_prediction
            tree_models["dijkstra_plus_golden_lgbm"] = composed_meta

        short_gate = static["cheb"] <= 80
        short_prediction, short_meta = fit_tree_student(
            "golden_short_tree_student", base, golden, train & short_gate,
            short_gate, static, env, opt.trees, opt.seed ^ 0x5A0A7,
        )
        predictions["golden_short_tree_student"] = short_prediction
        tree_models["golden_short_tree_student"] = short_meta

    # Golden-calibrate the architecture-only teacher student on the training
    # side.  This tests whether teacher structure and Golden semantics compose.
    if "teacher_student" in predictions:
        teacher_base = predictions["teacher_student"]
        name = "teacher_plus_macro10"
        groups = research.make_residual_groups(
            "port_family_macro10_family_phase", teacher_base, static, env,
            rare, len(arch.port_names),
        )
        fit = research.fit_residual(
            name, teacher_base, golden, train,
            np.ones(base.size, dtype=np.bool_), groups, opt.iterations,
        )
        models[name] = fit
        predictions[name] = research.apply_correction(teacher_base, fit)

    metric_rows = []
    for name, predicted in predictions.items():
        full_m = compact_metrics(golden, predicted, np.ones(base.size, dtype=np.bool_))
        valid_m = compact_metrics(golden, predicted, valid)
        short_m = compact_metrics(golden, predicted, valid & (static["cheb"] <= 64))
        long_m = compact_metrics(golden, predicted, valid & (static["cheb"] > 64))
        model = models.get(name)
        tree_model = tree_models.get(name)
        # A 63-leaf tree has at most 125 nodes.  Store a conservative upper
        # bound here; the exporter records the exact count.
        tree_parameter_count = int(tree_model["trees"]) * 125 if tree_model else 0
        if tree_model and tree_model.get("prefix_model"):
            tree_parameter_count *= 2
        results["models"][name] = {
            "full_public": full_m,
            "fixed_validation": valid_m,
            "validation_short_le_64": short_m,
            "validation_long_gt_64": long_m,
            "parameter_count": model.parameter_count if model else tree_parameter_count,
            "parameter_bytes_float32": model.parameter_bytes if model else tree_parameter_count * 16,
            "alpha": model.alpha if model else 0.0,
        }
        metric_rows.append((float(valid_m["acc_score"]), name))
        print(f"{name:38s} valid={valid_m['acc_score']:.6f} "
              f"full={full_m['acc_score']:.6f} params="
              f"{model.parameter_count if model else tree_parameter_count:,}",
              flush=True)

    best_name = max(metric_rows)[1]
    results["selection"] = {
        "criterion": "highest fixed random_stratified validation acc_score",
        "selected": best_name,
        "base_validation_acc": results["models"]["base_no_atlas"]["fixed_validation"]["acc_score"],
        "selected_validation_acc": results["models"][best_name]["fixed_validation"]["acc_score"],
        "teacher_active_rows": int(teacher_active.sum()) if teacher_active is not None else 0,
    }
    results["elapsed_seconds"] = time.perf_counter() - started
    (out / "p2_ablation.json").write_text(
        json.dumps(results, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    if best_name in models:
        (out / "p2_student_model.json").write_text(
            json.dumps(serialise_fit(models[best_name]), ensure_ascii=False,
                       separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
    elif best_name in tree_models and "lgbm" in best_name:
        # LightGBM's Windows C API cannot open this workspace's non-ASCII
        # path.  Materialise the model string through Python's Unicode-safe IO.
        (out / "p2_lightgbm_model.txt").write_text(
            tree_models[best_name]["model"].booster_.model_to_string(),
            encoding="utf-8",
        )
        (out / "p2_lightgbm_model.json").write_text(
            json.dumps(tree_models[best_name]["model"].booster_.dump_model(),
                       ensure_ascii=False, separators=(",", ":")) + "\n",
            encoding="utf-8",
        )
        prefix_name = tree_models[best_name].get("prefix_model")
        if prefix_name:
            (out / "p2_teacher_lightgbm_model.txt").write_text(
                tree_models[prefix_name]["model"].booster_.model_to_string(),
                encoding="utf-8",
            )
            (out / "p2_teacher_lightgbm_model.json").write_text(
                json.dumps(tree_models[prefix_name]["model"].booster_.dump_model(),
                           ensure_ascii=False, separators=(",", ":")) + "\n",
                encoding="utf-8",
            )
    print(f"selected={best_name} elapsed={results['elapsed_seconds']:.2f}s")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
