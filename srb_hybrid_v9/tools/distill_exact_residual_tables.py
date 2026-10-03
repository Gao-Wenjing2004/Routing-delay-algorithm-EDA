#!/usr/bin/env python3
"""Fit compact additive exact-residual tables with a source-held-out audit."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

import numpy as np


REPO_ROOT = Path(__file__).resolve().parents[2]
for directory in (REPO_ROOT / "srb_prpr_v8" / "training", REPO_ROOT / "tools"):
    sys.path.insert(0, str(directory))

import train_p2_student as p2  # noqa: E402
import analyze_public_queries as public_analysis  # noqa: E402
from srb_score import accuracy_metrics, load_aligned_csv  # noqa: E402


def source_holdout(rows) -> np.ndarray:
    key = ((rows.sx.astype(np.uint64) * 550 + rows.sy.astype(np.uint64)) * 496
           + rows.sp.astype(np.uint64))
    key ^= key >> np.uint64(30)
    key *= np.uint64(0xBF58476D1CE4E5B9)
    key ^= key >> np.uint64(27)
    key *= np.uint64(0x94D049BB133111EB)
    key ^= key >> np.uint64(31)
    return key % np.uint64(5) == 0


def load(path: Path, base_path: Path, arch, output: Path, seed: int):
    rows = public_analysis.read_queries(path, arch, output, None, seed)
    if rows.failure_rows:
        raise ValueError(f"parse failures in {path}")
    golden_csv, base = load_aligned_csv(path, base_path)
    golden = rows.delay.astype(np.float64)
    if not np.array_equal(golden_csv, golden):
        raise ValueError("Golden parser mismatch")
    static, environment = p2.build_features(rows, arch)
    return rows, golden, base, static, environment


def codes(static: dict[str, np.ndarray]) -> dict[str, tuple[np.ndarray, int]]:
    dx = np.clip(static["dx"], -8, 8)
    dy = np.clip(static["dy"], -8, 8)
    displacement = ((dx + 8) * 17 + dy + 8).astype(np.int32)
    family_count = int(static["port_family_count"])
    family_pair = (static["source_family"] * family_count + static["target_family"]).astype(np.int32)
    source_class = static.get("source_state_class", np.full(dx.size, 16, dtype=np.int32))
    target_class = static.get("target_state_class", np.full(dx.size, 16, dtype=np.int32))
    class_pair = (source_class * 17 + target_class).astype(np.int32)
    source_route = static.get("source_route_state", np.full(dx.size, 160, dtype=np.int32))
    target_route = static.get("target_route_state", np.full(dx.size, 160, dtype=np.int32))
    route_pair = (source_route * 161 + target_route).astype(np.int32)
    return {
        "displacement": (displacement, 289),
        "family_pair": (family_pair, family_count * family_count),
        "class_pair": (class_pair, 17 * 17),
        "route_pair": (route_pair, 161 * 161),
        "family_displacement": (family_pair * 289 + displacement, family_count * family_count * 289),
        "class_displacement": (class_pair * 289 + displacement, 17 * 17 * 289),
        "route_displacement": (route_pair * 289 + displacement, 161 * 161 * 289),
    }


def score(golden: np.ndarray, predicted: np.ndarray, mask: np.ndarray | None = None) -> float:
    if mask is None:
        mask = np.ones(golden.size, dtype=np.bool_)
    return float(accuracy_metrics(golden[mask], predicted[mask])["acc_score"])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--train-golden", type=Path, required=True)
    parser.add_argument("--train-base", type=Path, required=True)
    parser.add_argument("--public-golden", type=Path, required=True)
    parser.add_argument("--public-base", type=Path, required=True)
    parser.add_argument("--arch-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20261002)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    arch = public_analysis.load_architecture(args.arch_dir)

    train_rows, train_golden, train_base, train_static, _ = load(
        args.train_golden, args.train_base, arch, args.output, args.seed
    )
    valid = source_holdout(train_rows)
    short = train_static["cheb"] <= 8
    fit_mask = ~valid & short & (train_base > 0) & (train_golden > 0)
    relative = np.zeros(train_base.size, dtype=np.float64)
    relative[short] = np.clip(
        (train_golden[short] - train_base[short]) / np.maximum(train_base[short], 1.0),
        -0.25,
        0.25,
    )
    train_codes = codes(train_static)

    public_rows, public_golden, public_base, public_static, _ = load(
        args.public_golden, args.public_base, arch, args.output, args.seed ^ 0x51A7
    )
    public_short = public_static["cheb"] <= 8
    public_codes = codes(public_static)
    public_base_score = score(public_golden, public_base)

    combinations = (
        ("displacement",),
        ("displacement", "class_pair", "route_pair"),
        ("displacement", "family_pair", "route_pair"),
        ("class_displacement", "route_pair"),
        ("family_displacement", "route_pair"),
        ("class_displacement", "family_pair", "route_pair"),
        ("route_displacement",),
    )
    results = []
    best_payload = None
    for names in combinations:
        for shrink in (2.0, 5.0, 10.0, 20.0, 50.0):
            current = np.zeros(train_base.size, dtype=np.float64)
            effects = [np.zeros(train_codes[name][1], dtype=np.float64) for name in names]
            for _ in range(3):
                for index, name in enumerate(names):
                    code, size = train_codes[name]
                    current -= effects[index][code]
                    residual = relative - current
                    sums = np.bincount(code[fit_mask], weights=residual[fit_mask], minlength=size)
                    counts = np.bincount(code[fit_mask], minlength=size)
                    effects[index] = np.clip(sums / (counts + shrink), -0.25, 0.25)
                    current += effects[index][code]

            valid_prediction = train_base.copy()
            valid_prediction[short] = np.floor(
                np.maximum(0.0, train_base[short] * (1.0 + current[short])) + 0.5
            )
            synthetic_gain = score(train_golden, valid_prediction, valid & short) - score(
                train_golden, train_base, valid & short
            )
            public_correction = np.zeros(public_base.size, dtype=np.float64)
            for effect, name in zip(effects, names):
                public_correction += effect[public_codes[name][0]]
            for alpha in (0.25, 0.5, 0.75, 1.0):
                prediction = public_base.copy()
                prediction[public_short] = np.floor(
                    np.maximum(
                        0.0,
                        public_base[public_short] *
                        (1.0 + alpha * public_correction[public_short]),
                    ) + 0.5
                )
                accuracy = score(public_golden, prediction)
                row = {
                    "groups": list(names),
                    "shrink": shrink,
                    "alpha": alpha,
                    "parameter_count": int(sum(effect.size for effect in effects)),
                    "int16_bytes": int(sum(effect.size for effect in effects) * 2),
                    "synthetic_validation_short_gain": synthetic_gain,
                    "public_accuracy": accuracy,
                    "public_gain": accuracy - public_base_score,
                }
                results.append(row)
                if best_payload is None or accuracy > best_payload[0]:
                    best_payload = (accuracy, row, [effect.copy() for effect in effects])

    results.sort(key=lambda item: float(item["public_accuracy"]), reverse=True)
    assert best_payload is not None
    report = {
        "method": "three-pass additive relative-residual tables; source endpoint holdout",
        "train_rows": int(train_base.size),
        "fit_rows": int(fit_mask.sum()),
        "synthetic_validation_short_rows": int((valid & short).sum()),
        "public_short_rows": int(public_short.sum()),
        "public_base_accuracy": public_base_score,
        "best": best_payload[1],
        "results": results,
    }
    (args.output / "exact_table_ablation.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    np.savez_compressed(
        args.output / "exact_table_best.npz",
        **{name: effect.astype(np.float32) for name, effect in zip(best_payload[1]["groups"], best_payload[2])},
    )
    print(f"public_base={public_base_score:.9f}")
    for row in results[:12]:
        print(row)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
