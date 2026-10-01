#!/usr/bin/env python3
"""Fit the experimental architecture-family V4 residual on the fixed train side."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
import time
import zlib
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[1]
TOOLS = REPO_ROOT / "tools"
V4_DIR = REPO_ROOT / "srb_fast_v4"
for location in (str(TOOLS), str(V4_DIR)):
    if location not in sys.path:
        sys.path.insert(0, location)

import analyze_public_queries as public_analysis  # noqa: E402
import srb_v4_research as research  # noqa: E402
import train_v4_residual as compact_train  # noqa: E402
from srb_score import accuracy_metrics, load_aligned_csv  # noqa: E402


ALPHA = 1.25
ITERATIONS = 3
SEED = 20260827
PRODUCTION_SPLIT = "random_stratified"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--v3-predictions", type=Path, required=True)
    parser.add_argument("--arch-dir", type=Path, required=True)
    parser.add_argument("--atlas-file", type=Path, required=True)
    parser.add_argument("--fixed-splits", type=Path, required=True)
    parser.add_argument("--out-header", type=Path, required=True)
    parser.add_argument("--out-manifest", type=Path, required=True)
    parser.add_argument("--out-expected", type=Path, required=True)
    parser.add_argument("--atlas-query-radius", type=int, default=48)
    parser.add_argument(
        "--candidate",
        choices=(
            "port_family_residual",
            "port_family_compact",
            "port_family_macro8",
            "port_family_macro8_endpoint",
            "port_family_macro10_family_phase",
        ),
        default="port_family_residual",
    )
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


def main() -> int:
    args = parse_args()
    started = time.perf_counter()
    args.out_header.parent.mkdir(parents=True, exist_ok=True)
    args.out_manifest.parent.mkdir(parents=True, exist_ok=True)
    args.out_expected.parent.mkdir(parents=True, exist_ok=True)

    arch = public_analysis.load_architecture(args.arch_dir)
    parse_dir = args.out_manifest.parent / ".family_train_parse"
    parse_dir.mkdir(parents=True, exist_ok=True)
    rows = public_analysis.read_queries(args.golden, arch, parse_dir, None, SEED)
    if rows.failure_rows or rows.success_rows != 1_000_000:
        raise ValueError("family training requires 1,000,000 successfully parsed rows")
    golden_csv, base = load_aligned_csv(args.golden, args.v3_predictions)
    golden = rows.delay.astype(np.float64)
    if not np.array_equal(golden_csv, golden):
        raise ValueError("Golden alignment mismatch")

    env = public_analysis.classify_environment(rows, arch)
    sx, sy, tx, ty = (v.astype(np.int32) for v in (rows.sx, rows.sy, rows.tx, rows.ty))
    sp, tp = rows.sp.astype(np.int32), rows.tp.astype(np.int32)
    env["endpoint_boundary_clearance"] = np.minimum.reduce(
        [sx, sy, tx, ty, arch.width - 1 - sx, arch.height - 1 - sy,
         arch.width - 1 - tx, arch.height - 1 - ty]
    ).astype(np.int16)
    dx, dy = tx - sx, ty - sy
    ax, ay = np.abs(dx), np.abs(dy)
    cheb = np.maximum(ax, ay)
    quad = (
        (dx > 0).astype(np.int32)
        + 2 * (dx < 0).astype(np.int32)
        + 3 * (dy > 0).astype(np.int32)
        + 6 * (dy < 0).astype(np.int32)
    )
    labels, atlas_metadata = public_analysis.classify_v3_from_atlas(
        args.atlas_file,
        rows,
        arch,
        cheb,
        env["block_related"],
        query_radius=args.atlas_query_radius,
    )
    fallback_mask = labels != 1

    family_keys = [re.sub(r"\[\d+\]$", "[]", name) for name in arch.port_names]
    family_to_id = {name: index for index, name in enumerate(dict.fromkeys(family_keys))}
    port_to_family = np.asarray([family_to_id[name] for name in family_keys], dtype=np.uint16)
    source_family = port_to_family[sp].astype(np.int32)
    target_family = port_to_family[tp].astype(np.int32)
    family_count = len(family_to_id)
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
        "port_pair": sp * len(arch.port_names) + tp,
        "source_family": source_family,
        "target_family": target_family,
        "port_family_pair": source_family * family_count + target_family,
        "port_family_count": family_count,
    }
    candidate = args.candidate
    feature_versions = {
        "port_family_residual": "v4-port-family-residual-irls-v1",
        "port_family_compact": "v4-port-family-compact-irls-v1",
        "port_family_macro8": "v6-port-family-macro8-irls-v1",
        "port_family_macro8_endpoint": "v6-port-family-macro8-endpoint-irls-v1",
        "port_family_macro10_family_phase": "v6-port-family-macro10-family-phase-irls-v1",
    }
    feature_version = feature_versions[candidate]
    groups = research.make_residual_groups(
        candidate,
        base,
        static,
        env,
        np.zeros(rows.success_rows, dtype=np.bool_),
        len(arch.port_names),
    )
    with np.load(args.fixed_splits) as split_data:
        if not np.array_equal(
            np.asarray(split_data["line_number"], dtype=np.uint32), rows.line_no
        ):
            raise ValueError("fixed split line numbers do not match")
        packed = np.asarray(split_data[f"valid_{PRODUCTION_SPLIT}"], dtype=np.uint8)
    heldout = np.unpackbits(packed)[: rows.success_rows].astype(np.bool_)
    train = ~heldout
    fit = research.fit_residual(
        candidate, base, golden, train, fallback_mask, groups, ITERATIONS
    )
    fit.alpha = ALPHA
    corrected = compact_train.simulate_float32(base, fit)
    predicted = base.copy()
    predicted[fallback_mask] = corrected[fallback_mask]

    effect_names = {
        "prediction_bin": "kPredictionBin",
        "distance_quad": "kDistanceQuad",
        "boundary": "kBoundary",
        "gap_shape": "kGapShape",
        "block_distance": "kBlockDistance",
        "port_family_pair": "kPortFamilyPair",
        "source_family_distance": "kSourceFamilyDistance",
        "target_family_distance": "kTargetFamilyDistance",
        "source_family": "kSourceFamily",
        "target_family": "kTargetFamily",
        "macro8_direction": "kMacro8Direction",
        "source_family_macro8": "kSourceFamilyMacro8",
        "target_family_macro8": "kTargetFamilyMacro8",
        "source_phase": "kSourcePhase",
        "target_phase": "kTargetPhase",
        "source_family_phase": "kSourceFamilyPhase",
        "target_family_phase": "kTargetFamilyPhase",
    }
    if any(group.name not in effect_names for group in fit.groups):
        raise ValueError("unknown family residual group")
    x_gap_count, y_gap_count = compact_train.make_gap_count_prefix(arch)
    atlas_sha256 = sha256_file(args.atlas_file)
    atlas_crc32 = crc32_file(args.atlas_file)
    with args.out_header.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("// Generated by experiments/train_v4_family_residual.py.\n")
        stream.write("#pragma once\n\n#include <cstdint>\n\n")
        stream.write("namespace srb_v4_family_data {\n\n")
        stream.write(f'static const char kFeatureVersion[] = "{feature_version}";\n')
        stream.write(f'static const char kArchitectureSha256[] = "{arch.fingerprint_sha256}";\n')
        stream.write(f'static const char kAtlasSha256[] = "{atlas_sha256}";\n')
        stream.write(f"static const uint32_t kAtlasCrc32 = UINT32_C(0x{atlas_crc32:08X});\n")
        stream.write(f"static constexpr float kAlpha = {compact_train.cpp_float(ALPHA)};\n")
        stream.write(f"static constexpr uint32_t kParameterCount = {fit.parameter_count};\n")
        stream.write(f"static constexpr uint32_t kFamilyCount = {family_count};\n\n")
        for group, effect in zip(fit.groups, fit.effects):
            compact_train.write_float_array(stream, effect_names[group.name], effect)
        compact_train.write_integer_array(stream, "uint16_t", "kPortToFamily", port_to_family)
        compact_train.write_integer_array(stream, "uint8_t", "kXGapCountPrefix", x_gap_count)
        compact_train.write_integer_array(stream, "uint8_t", "kYGapCountPrefix", y_gap_count)
        stream.write("}  // namespace srb_v4_family_data\n")

    metrics_base = accuracy_metrics(golden, base)
    metrics_all = accuracy_metrics(golden, predicted)
    metrics_heldout = accuracy_metrics(golden[heldout], predicted[heldout])
    metrics_heldout_base = accuracy_metrics(golden[heldout], base[heldout])
    manifest = {
        "feature_version": feature_version,
        "candidate": candidate,
        "status": "experimental; fixed heldout not used for fit",
        "alpha": ALPHA,
        "iterations": ITERATIONS,
        "seed": SEED,
        "production_split": PRODUCTION_SPLIT,
        "training_rows": int(train.sum()),
        "heldout_validation_rows": int(heldout.sum()),
        "residual_training_rows": int((train & fallback_mask).sum()),
        "validation_rows_used_for_final_fit": 0,
        "atlas_query_radius_for_residual_fit": args.atlas_query_radius,
        "port_count": len(arch.port_names),
        "family_count": family_count,
        "family_rule": "remove terminal [decimal-index]; otherwise preserve full port name",
        "parameter_count": fit.parameter_count,
        "parameter_bytes_float32": fit.parameter_bytes,
        "groups": [
            {"name": group.name, "size": group.size, "shrink": group.shrink}
            for group in fit.groups
        ],
        "architecture_fingerprint_sha256": arch.fingerprint_sha256,
        "atlas_sha256": atlas_sha256,
        "atlas_crc32": f"{atlas_crc32:08X}",
        "atlas_metadata": atlas_metadata,
        "training_data_sha256": sha256_file(args.golden),
        "fixed_splits_sha256": sha256_file(args.fixed_splits),
        "base_v3_predictions_sha256": sha256_file(args.v3_predictions),
        "header_sha256": sha256_file(args.out_header),
        "metrics": {
            "v3": metrics_base,
            "family_public_all_mixed_train_and_holdout": metrics_all,
            "public_all_delta_acc_score": metrics_all["acc_score"] - metrics_base["acc_score"],
            "family_heldout": metrics_heldout,
            "heldout_delta_acc_score": (
                metrics_heldout["acc_score"] - metrics_heldout_base["acc_score"]
            ),
        },
        "elapsed_seconds": time.perf_counter() - started,
    }
    args.out_manifest.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    np.save(args.out_expected, predicted.astype(np.uint32))
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
