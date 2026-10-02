#!/usr/bin/env python3
"""Ablate a fixed post-tree residual memory without storing 1M predictions."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[2]
TOOLS = REPO_ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze_public_queries as public_analysis  # noqa: E402
import srb_v4_research as research  # noqa: E402
from srb_score import accuracy_metrics, load_aligned_csv, point_scores  # noqa: E402

import train_p2_student as p2  # noqa: E402


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", required=True, type=Path)
    parser.add_argument("--arch-dir", required=True, type=Path)
    parser.add_argument("--base", required=True, type=Path)
    parser.add_argument("--model-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--seed", type=int, default=20260827)
    return parser.parse_args()


def score(golden: np.ndarray, predicted: np.ndarray) -> float:
    return float(accuracy_metrics(golden, predicted)["acc_score"])


def apply(base: np.ndarray, correction: np.ndarray, alpha: float) -> np.ndarray:
    return np.floor(np.maximum(0.0, base * (1.0 + alpha * correction)) + 0.5)


def predict_dump(path: Path, features: np.ndarray) -> np.ndarray:
    """Evaluate a dumped LightGBM model using its exact exported tree semantics."""
    model = json.loads(path.read_text(encoding="utf-8"))
    result = np.zeros(features.shape[0], dtype=np.float64)
    all_rows = np.arange(features.shape[0], dtype=np.int32)
    for tree in model["tree_info"]:
        stack = [(tree["tree_structure"], all_rows)]
        while stack:
            node, rows = stack.pop()
            if rows.size == 0:
                continue
            if "leaf_value" in node:
                result[rows] += float(node["leaf_value"])
                continue
            values = features[rows, int(node["split_feature"])]
            if str(node.get("decision_type", "<=")) == "==":
                categories = np.asarray(
                    [int(value) for value in str(node.get("threshold", "")).split("||") if value],
                    dtype=np.int32,
                )
                left = np.isin(values.astype(np.int32), categories)
            else:
                left = values <= float(node["threshold"])
            stack.append((node["right_child"], rows[~left]))
            stack.append((node["left_child"], rows[left]))
    return result


def main() -> int:
    opt = parse_args()
    arch = public_analysis.load_architecture(opt.arch_dir)
    parse_dir = opt.output.parent / ".post_memory_parse"
    parse_dir.mkdir(parents=True, exist_ok=True)
    rows = public_analysis.read_queries(opt.golden, arch, parse_dir, None, opt.seed)
    if rows.failure_rows:
        raise ValueError("Golden parsing failed")
    golden_csv, base = load_aligned_csv(opt.golden, opt.base)
    golden = rows.delay.astype(np.float64)
    if not np.array_equal(golden_csv, golden):
        raise ValueError("Golden parser mismatch")
    static, env = p2.build_features(rows, arch)
    train, valid, rare, split_manifest = p2.fixed_split(
        rows, static, env, len(arch.port_names), opt.seed
    )

    teacher_x, _, _ = p2.tree_features(static, env, base)
    teacher_correction = predict_dump(
        opt.model_dir / "p2_teacher_lightgbm_model.json", teacher_x
    )
    teacher_gate = (static["cheb"] <= 72) & ~env["block_related"]
    teacher_correction[~teacher_gate] = 0.0
    teacher_prediction = apply(base, teacher_correction, 1.0)
    student_x, _, _ = p2.tree_features(static, env, teacher_prediction)
    student_correction = predict_dump(
        opt.model_dir / "p2_lightgbm_model.json", student_x
    )
    composite = apply(teacher_prediction, student_correction, 1.0)

    memory = json.loads(
        (opt.model_dir / "p2_post_residual_model.json").read_text(encoding="utf-8")
    )
    candidate = memory["name"].removeprefix("dijkstra_plus_golden_lgbm_plus_")
    all_groups = research.make_residual_groups(
        candidate, composite, static, env, rare, len(arch.port_names)
    )
    by_name = {group.name: group for group in all_groups}
    memory_names = [str(row["name"]) for row in memory["groups"]]
    if not set(memory_names).issubset(by_name):
        raise ValueError("residual group name mismatch")
    groups = [by_name[name] for name in memory_names]
    alpha = float(memory["alpha"])
    effects = [np.asarray(row["values"], dtype=np.float64) for row in memory["groups"]]
    contributions = [effect[group.code] for effect, group in zip(effects, groups)]
    total = np.sum(contributions, axis=0)

    result: dict[str, object] = {
        "model": memory["name"],
        "alpha": alpha,
        "rows": int(golden.size),
        "training_rows": int(train.sum()),
        "validation_rows": int(valid.sum()),
        "baseline": {
            "fixed_validation_acc": score(golden[valid], composite[valid]),
            "full_public_acc": score(golden, composite),
        },
        "all_groups": {
            "fixed_validation_acc": score(golden[valid], apply(composite, total, alpha)[valid]),
            "full_public_acc": score(golden, apply(composite, total, alpha)),
            "parameter_count": int(sum(group.size for group in groups)),
        },
        "leave_one_out": [],
        "single_group": [],
        "split_manifest": split_manifest,
    }
    for index, (group, contribution, effect) in enumerate(zip(groups, contributions, effects)):
        without = total - contribution
        only_prediction = apply(composite, contribution, alpha)
        without_prediction = apply(composite, without, alpha)
        result["single_group"].append({
            "name": group.name,
            "parameter_count": int(group.size),
            "nonzero_parameters": int(np.count_nonzero(effect)),
            "fixed_validation_acc": score(golden[valid], only_prediction[valid]),
            "full_public_acc": score(golden, only_prediction),
        })
        result["leave_one_out"].append({
            "removed": group.name,
            "parameter_count_after_removal": int(sum(g.size for g in groups) - group.size),
            "fixed_validation_acc": score(golden[valid], without_prediction[valid]),
            "full_public_acc": score(golden, without_prediction),
        })

    selected: list[int] = []
    remaining = set(range(len(groups)))
    current = np.zeros(base.size, dtype=np.float64)
    current_score = result["baseline"]["fixed_validation_acc"]
    greedy_steps = []
    while remaining:
        options = []
        for index in remaining:
            predicted = apply(composite, current + contributions[index], alpha)
            options.append((score(golden[valid], predicted[valid]), index))
        best_score, best_index = max(options)
        if best_score <= current_score + 1e-12:
            break
        selected.append(best_index)
        remaining.remove(best_index)
        current += contributions[best_index]
        current_score = best_score
        predicted = apply(composite, current, alpha)
        greedy_steps.append({
            "added": groups[best_index].name,
            "fixed_validation_acc": current_score,
            "full_public_acc": score(golden, predicted),
            "parameter_count": int(sum(groups[index].size for index in selected)),
        })
    result["greedy_forward"] = greedy_steps
    result["selected_groups"] = [groups[index].name for index in selected]

    memory_prediction = apply(composite, total, alpha)
    delta = point_scores(golden, memory_prediction) - point_scores(golden, composite)
    source_family = static["source_family"]
    family_count = int(static["port_family_count"])
    family_rows = []
    for family in range(family_count):
        train_family = train & (source_family == family)
        valid_family = valid & (source_family == family)
        family_rows.append({
            "family": family,
            "training_rows": int(train_family.sum()),
            "training_total_point_gain": float(delta[train_family].sum()),
            "training_mean_point_gain": float(delta[train_family].mean()) if train_family.any() else 0.0,
            "validation_rows": int(valid_family.sum()),
            "validation_total_point_gain": float(delta[valid_family].sum()),
            "validation_mean_point_gain": float(delta[valid_family].mean()) if valid_family.any() else 0.0,
        })
    ranked = [
        row for row in sorted(
            family_rows,
            key=lambda row: (row["training_mean_point_gain"], row["training_rows"]),
            reverse=True,
        )
        if row["training_rows"] and row["training_mean_point_gain"] > 0.0
    ]
    gates = []
    for top in (1, 2, 4, 8, 12, 16, 24, 32, 36, 40, 44, 46, 48, 50):
        selected_families = [int(row["family"]) for row in ranked[:top]]
        gate = np.isin(source_family, selected_families)
        gated_prediction = composite.copy()
        gated_prediction[gate] = memory_prediction[gate]
        gates.append({
            "top": top,
            "source_families": selected_families,
            "apply_rows": int(gate.sum()),
            "apply_share": float(gate.mean()),
            "fixed_validation_acc": score(golden[valid], gated_prediction[valid]),
            "full_public_acc": score(golden, gated_prediction),
        })
    result["source_family_training_rank"] = family_rows
    result["training_selected_gates"] = gates

    opt.output.parent.mkdir(parents=True, exist_ok=True)
    opt.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({
        "baseline": result["baseline"],
        "all_groups": result["all_groups"],
        "greedy_forward": greedy_steps,
    }, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
