#!/usr/bin/env python3
"""Fit the locked V4 residual structure and emit deterministic C++ data.

Model structure and alpha must be selected before this script is run.  This
script performs the final all-public fit only; it does not select features,
regularization, alpha, or a validation threshold.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
import time
import zlib
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent
TOOLS = REPO_ROOT / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze_public_queries as public_analysis  # noqa: E402
import srb_v4_research as research  # noqa: E402
from srb_score import accuracy_metrics, load_aligned_csv  # noqa: E402


FEATURE_VERSION = "v4-compact-residual-irls-v1"
LOCKED_CANDIDATE = "single_residual"
LOCKED_ALPHA = 1.25
LOCKED_ITERATIONS = 3
LOCKED_SEED = 20260827
LOCKED_PRODUCTION_SPLIT = "random_stratified"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Emit locked SRB Fast V4 residual tables")
    parser.add_argument("--golden", required=True, type=Path)
    parser.add_argument("--v3-predictions", required=True, type=Path)
    parser.add_argument("--arch-dir", required=True, type=Path)
    parser.add_argument("--atlas-file", required=True, type=Path)
    parser.add_argument("--fixed-splits", required=True, type=Path)
    parser.add_argument("--out-header", type=Path, default=HERE / "v4_residual_data.hpp")
    parser.add_argument("--out-manifest", type=Path, default=HERE / "v4_model_manifest.json")
    parser.add_argument("--out-expected", type=Path)
    parser.add_argument("--seed", type=int, default=LOCKED_SEED)
    parser.add_argument("--alpha", type=float, default=LOCKED_ALPHA)
    parser.add_argument("--iterations", type=int, default=LOCKED_ITERATIONS)
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def crc32_file(path: Path) -> int:
    value = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            value = zlib.crc32(block, value)
    return value & 0xFFFFFFFF


def cpp_float(value: float) -> str:
    value = float(np.float32(value))
    if not np.isfinite(value):
        raise ValueError("non-finite model value")
    text = format(value, ".9g")
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def write_float_array(stream, name: str, values: np.ndarray, per_line: int = 8) -> None:
    values = np.asarray(values, dtype=np.float32).reshape(-1)
    stream.write(f"static const float {name}[{values.size}] = {{\n")
    for offset in range(0, values.size, per_line):
        rendered = ", ".join(cpp_float(value) for value in values[offset : offset + per_line])
        stream.write(f"    {rendered},\n")
    stream.write("};\n\n")


def write_integer_array(stream, ctype: str, name: str, values: np.ndarray, per_line: int = 16) -> None:
    values = np.asarray(values).reshape(-1)
    stream.write(f"static const {ctype} {name}[{values.size}] = {{\n")
    for offset in range(0, values.size, per_line):
        rendered = ", ".join(str(int(value)) for value in values[offset : offset + per_line])
        stream.write(f"    {rendered},\n")
    stream.write("};\n\n")


def make_gap_count_prefix(arch: public_analysis.Architecture) -> tuple[np.ndarray, np.ndarray]:
    x_step = np.zeros(arch.width, dtype=np.uint8)
    y_step = np.zeros(arch.height, dtype=np.uint8)
    for gap in arch.gaps:
        if gap.direction == "vertical" and 0 <= gap.site + 1 < arch.width:
            x_step[gap.site + 1] += 1
        elif gap.direction == "horizontal" and 0 <= gap.site + 1 < arch.height:
            y_step[gap.site + 1] += 1
    return np.cumsum(x_step, dtype=np.uint8), np.cumsum(y_step, dtype=np.uint8)


def simulate_float32(base: np.ndarray, fit: research.ResidualFit) -> np.ndarray:
    correction = np.zeros(base.size, dtype=np.float32)
    for group, effect in zip(fit.groups, fit.effects):
        correction += np.asarray(effect, dtype=np.float32)[group.code]
    correction *= np.float32(fit.alpha)
    value = base.astype(np.float32) * (np.float32(1.0) + correction)
    return np.floor(np.maximum(value.astype(np.float64), 0.0) + 0.5)


def main() -> int:
    args = parse_args()
    if args.seed != LOCKED_SEED:
        raise ValueError(f"locked model requires seed {LOCKED_SEED}")
    if args.alpha != LOCKED_ALPHA:
        raise ValueError(f"locked model requires alpha {LOCKED_ALPHA}")
    if args.iterations != LOCKED_ITERATIONS:
        raise ValueError(f"locked model requires {LOCKED_ITERATIONS} residual iterations")
    started = time.perf_counter()
    args.out_header.parent.mkdir(parents=True, exist_ok=True)
    args.out_manifest.parent.mkdir(parents=True, exist_ok=True)

    arch = public_analysis.load_architecture(args.arch_dir)
    parse_dir = args.out_manifest.parent / ".v4_train_parse"
    parse_dir.mkdir(parents=True, exist_ok=True)
    rows = public_analysis.read_queries(args.golden, arch, parse_dir, None, args.seed)
    if rows.failure_rows:
        raise ValueError("Golden parse failure during V4 final fit")
    golden_csv, v3_prediction = load_aligned_csv(args.golden, args.v3_predictions)
    golden = rows.delay.astype(np.float64)
    if not np.array_equal(golden_csv, golden):
        raise ValueError("Golden values differ between canonical parser and strict scorer")

    env = public_analysis.classify_environment(rows, arch)
    sx, sy, tx, ty = (value.astype(np.int32) for value in (rows.sx, rows.sy, rows.tx, rows.ty))
    env["endpoint_boundary_clearance"] = np.minimum.reduce(
        [
            sx,
            sy,
            tx,
            ty,
            arch.width - 1 - sx,
            arch.height - 1 - sy,
            arch.width - 1 - tx,
            arch.height - 1 - ty,
        ]
    ).astype(np.int16)
    dx, dy = tx - sx, ty - sy
    cheb = np.maximum(np.abs(dx), np.abs(dy))
    labels, atlas_metadata = public_analysis.classify_v3_from_atlas(
        args.atlas_file, rows, arch, cheb, env["block_related"]
    )
    fallback_mask = labels != 1
    with np.load(args.fixed_splits) as split_data:
        split_line_number = np.asarray(split_data["line_number"], dtype=np.uint32)
        if not np.array_equal(split_line_number, rows.line_no):
            raise ValueError("fixed split line numbers do not match the Golden input")
        packed_validation = np.asarray(
            split_data[f"valid_{LOCKED_PRODUCTION_SPLIT}"], dtype=np.uint8
        )
    production_validation = np.unpackbits(packed_validation)[: rows.success_rows].astype(np.bool_)
    production_train = ~production_validation
    quad = (
        (dx > 0).astype(np.int32)
        + 2 * (dx < 0).astype(np.int32)
        + 3 * (dy > 0).astype(np.int32)
        + 6 * (dy < 0).astype(np.int32)
    )
    static = {
        "sx": sx,
        "sy": sy,
        "tx": tx,
        "ty": ty,
        "sp": rows.sp.astype(np.int32),
        "tp": rows.tp.astype(np.int32),
        "dx": dx,
        "dy": dy,
        "ax": np.abs(dx),
        "ay": np.abs(dy),
        "cheb": cheb,
        "quad": quad,
        "port_pair": rows.sp.astype(np.int32) * len(arch.port_names) + rows.tp.astype(np.int32),
    }
    unused_rare_mask = np.zeros(rows.success_rows, dtype=np.bool_)
    groups = research.make_residual_groups(
        LOCKED_CANDIDATE,
        v3_prediction,
        static,
        env,
        unused_rare_mask,
        len(arch.port_names),
    )
    fit = research.fit_residual(
        LOCKED_CANDIDATE,
        v3_prediction,
        golden,
        production_train,
        fallback_mask,
        groups,
        args.iterations,
    )
    fit.alpha = LOCKED_ALPHA
    corrected = simulate_float32(v3_prediction, fit)
    v4_prediction = v3_prediction.copy()
    v4_prediction[fallback_mask] = corrected[fallback_mask]
    x_gap_count, y_gap_count = make_gap_count_prefix(arch)
    atlas_sha256 = sha256_file(args.atlas_file)
    atlas_crc32 = crc32_file(args.atlas_file)
    effect_names = {
        "prediction_bin": "kPredictionBin",
        "distance_quad": "kDistanceQuad",
        "boundary": "kBoundary",
        "gap_shape": "kGapShape",
        "block_distance": "kBlockDistance",
        "source_port": "kSourcePort",
        "target_port": "kTargetPort",
    }
    if [group.name for group in fit.groups] != list(effect_names):
        raise ValueError("locked residual group order changed")

    with args.out_header.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("// Generated by train_v4_residual.py. Do not edit by hand.\n")
        stream.write("#pragma once\n\n#include <cstdint>\n\n")
        stream.write("namespace srb_v4_data {\n\n")
        stream.write(f'static const char kFeatureVersion[] = "{FEATURE_VERSION}";\n')
        stream.write(f'static const char kArchitectureSha256[] = "{arch.fingerprint_sha256}";\n')
        stream.write(f'static const char kAtlasSha256[] = "{atlas_sha256}";\n')
        stream.write(f"static const uint32_t kAtlasCrc32 = UINT32_C(0x{atlas_crc32:08X});\n")
        stream.write(f"static constexpr float kAlpha = {cpp_float(LOCKED_ALPHA)};\n")
        stream.write(f"static constexpr uint32_t kParameterCount = {fit.parameter_count};\n\n")
        for group, effect in zip(fit.groups, fit.effects):
            write_float_array(stream, effect_names[group.name], effect)
        write_integer_array(stream, "uint8_t", "kXGapCountPrefix", x_gap_count)
        write_integer_array(stream, "uint8_t", "kYGapCountPrefix", y_gap_count)
        stream.write("}  // namespace srb_v4_data\n")

    manifest = {
        "feature_version": FEATURE_VERSION,
        "candidate": LOCKED_CANDIDATE,
        "alpha": LOCKED_ALPHA,
        "iterations": LOCKED_ITERATIONS,
        "seed": LOCKED_SEED,
        "production_split": LOCKED_PRODUCTION_SPLIT,
        "training_rows": int(production_train.sum()),
        "heldout_validation_rows": int(production_validation.sum()),
        "residual_training_rows": int((production_train & fallback_mask).sum()),
        "validation_rows_used_for_final_fit": 0,
        "parameter_count": fit.parameter_count,
        "parameter_bytes_float32": fit.parameter_bytes,
        "groups": [
            {"name": group.name, "size": group.size, "shrink": group.shrink}
            for group in fit.groups
        ],
        "architecture_fingerprint_sha256": arch.fingerprint_sha256,
        "architecture_files_sha256": {
            key: sha256_file(path) for key, path in arch.source_files.items()
        },
        "atlas": {
            "sha256": atlas_sha256,
            "crc32": f"{atlas_crc32:08X}",
            "bytes": args.atlas_file.stat().st_size,
            "metadata": atlas_metadata,
        },
        "training_data_sha256": sha256_file(args.golden),
        "fixed_splits_sha256": sha256_file(args.fixed_splits),
        "base_v3_predictions_sha256": sha256_file(args.v3_predictions),
        "inference_sources_sha256": {
            name: sha256_file(REPO_ROOT / "plusone-srb_fast_v3" / "srb_fast_v3" / name)
            for name in ("srb_fast.hpp", "fast_model_data.hpp", "local_arch_data.hpp")
        },
        "residual_header_sha256": sha256_file(args.out_header),
        "metrics": {
            "v3": accuracy_metrics(golden, v3_prediction),
            "v4_public_all_mixed_train_and_holdout": accuracy_metrics(golden, v4_prediction),
            "public_all_delta_acc_score": float(
                accuracy_metrics(golden, v4_prediction)["acc_score"]
                - accuracy_metrics(golden, v3_prediction)["acc_score"]
            ),
            "v4_training_rows": accuracy_metrics(
                golden[production_train], v4_prediction[production_train]
            ),
            "v4_heldout_rows": accuracy_metrics(
                golden[production_validation], v4_prediction[production_validation]
            ),
            "heldout_delta_acc_score_vs_existing_v3": float(
                accuracy_metrics(
                    golden[production_validation], v4_prediction[production_validation]
                )["acc_score"]
                - accuracy_metrics(
                    golden[production_validation], v3_prediction[production_validation]
                )["acc_score"]
            ),
        },
        "elapsed_seconds": time.perf_counter() - started,
    }
    args.out_manifest.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    if args.out_expected is not None:
        args.out_expected.parent.mkdir(parents=True, exist_ok=True)
        np.save(args.out_expected, v4_prediction.astype(np.uint32))
    print(json.dumps(manifest["metrics"], indent=2))
    print(f"header={args.out_header.resolve()}")
    print(f"manifest={args.out_manifest.resolve()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
