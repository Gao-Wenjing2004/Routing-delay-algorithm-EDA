#!/usr/bin/env python3
"""Train a compact deterministic delay estimator and emit a C++ header.

The generated model is deliberately small.  It does not memorize endpoint pairs;
it learns reusable source-port, target-port, direction, hop-remainder and Block
relation corrections on top of a geometric baseline.  Gap Line delay is kept as
an exact, architecture-derived term.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import sys
import time
from pathlib import Path

import numpy as np


DIRECTION_COUNT = 9
REMAINDER_COUNT = 17 * 17
DISTANCE_BIN_COUNT = DIRECTION_COUNT * 16
BLOCK_RELATION_COUNT = 81
GEOM8_COUNT = 9 * 15 * 69
ANGLE_COUNT = 9 * 17
GEOM4_COUNT = 9 * 30 * 138
DISPLACEMENT_COUNT = 239 * 1099
REGION_COUNT = 15 * 69


def parse_args() -> argparse.Namespace:
    here = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(
        description="Train the SRB O(1) delay model and generate fast_model_data.hpp"
    )
    parser.add_argument(
        "--golden",
        type=Path,
        default=here / "arch" / "delay_estimate_ans.csv",
        help="public golden CSV containing From,To,delay",
    )
    parser.add_argument(
        "--ports",
        type=Path,
        default=here / "arch" / "SRB_Port.json",
        help="SRB_Port.json",
    )
    parser.add_argument(
        "--gaps",
        type=Path,
        default=here / "arch" / "SRB_Gap.json",
        help="SRB_Gap.json",
    )
    parser.add_argument(
        "--out",
        type=Path,
        default=here / "fast_model_data.hpp",
        help="generated C++ model header",
    )
    parser.add_argument(
        "--iterations",
        type=int,
        default=8,
        help="categorical residual back-fitting iterations",
    )
    parser.add_argument(
        "--skip-validation",
        action="store_true",
        help="fit all data directly without the deterministic 20%% holdout report",
    )
    return parser.parse_args()


def load_architecture(port_path: Path, gap_path: Path):
    with port_path.open("r", encoding="utf-8") as stream:
        root = json.load(stream)
    port_rows = root.get("Port", root.get("Ports"))
    if not isinstance(port_rows, list):
        raise ValueError(f"cannot find Port array in {port_path}")

    port_names = []
    port_to_id = {}
    for row in port_rows:
        name = row.get("Name", row.get("name"))
        if not isinstance(name, str):
            raise ValueError("port without a valid Name")
        if name in port_to_id:
            raise ValueError(f"duplicate port name: {name}")
        port_to_id[name] = len(port_names)
        port_names.append(name)

    with gap_path.open("r", encoding="utf-8") as stream:
        gap_root = json.load(stream)
    gap = gap_root.get("Gap", gap_root.get("Gaps"))
    if not isinstance(gap, dict):
        raise ValueError(f"cannot find Gap object in {gap_path}")
    lines = gap.get("Line", gap.get("Lines", []))
    blocks = gap.get("Block", gap.get("Blocks", []))
    return port_names, port_to_id, lines, blocks


def split_endpoint(spec: str, port_to_id: dict[str, int]):
    try:
        inst, port = spec.strip().split("/", 1)
        prefix, xs, ys = inst.split("_")
        if prefix != "SRB":
            raise ValueError
        return int(xs), int(ys), port_to_id[port]
    except (ValueError, KeyError) as exc:
        raise ValueError(f"bad endpoint: {spec!r}") from exc


def count_rows(path: Path) -> int:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream)
        try:
            next(reader)
        except StopIteration:
            return 0
        return sum(1 for row in reader if row)


def load_golden(path: Path, port_to_id: dict[str, int]):
    n = count_rows(path)
    if n == 0:
        raise ValueError(f"golden CSV is empty: {path}")

    sx = np.empty(n, dtype=np.int16)
    sy = np.empty(n, dtype=np.int16)
    tx = np.empty(n, dtype=np.int16)
    ty = np.empty(n, dtype=np.int16)
    sp = np.empty(n, dtype=np.int16)
    tp = np.empty(n, dtype=np.int16)
    delay = np.empty(n, dtype=np.float64)

    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        if not reader.fieldnames:
            raise ValueError(f"CSV has no header: {path}")
        delay_key = next(
            (name for name in reader.fieldnames if name.strip().lower() in {"delay", "min delay"}),
            None,
        )
        if delay_key is None or "From" not in reader.fieldnames or "To" not in reader.fieldnames:
            raise ValueError("golden CSV must contain From, To and delay columns")

        i = 0
        for row in reader:
            if not row:
                continue
            ax, ay, ap = split_endpoint(row["From"], port_to_id)
            bx, by, bp = split_endpoint(row["To"], port_to_id)
            sx[i], sy[i], sp[i] = ax, ay, ap
            tx[i], ty[i], tp[i] = bx, by, bp
            delay[i] = float(row[delay_key])
            i += 1
    if i != n:
        sx, sy, tx, ty, sp, tp, delay = (
            a[:i] for a in (sx, sy, tx, ty, sp, tp, delay)
        )
    return sx, sy, tx, ty, sp, tp, delay


def make_line_prefix(lines, width: int = 120, height: int = 550):
    x_step = np.zeros(width, dtype=np.int32)
    y_step = np.zeros(height, dtype=np.int32)
    for row in lines:
        direction = str(row["direction"]).lower()
        site = int(row["site"])
        value = int(row["delay"])
        if direction == "vertical" and 0 <= site + 1 < width:
            x_step[site + 1] += value
        elif direction == "horizontal" and 0 <= site + 1 < height:
            y_step[site + 1] += value
    return np.cumsum(x_step), np.cumsum(y_step)


def make_base_features(dx: np.ndarray, dy: np.ndarray) -> np.ndarray:
    xp = np.maximum(dx, 0)
    xn = np.maximum(-dx, 0)
    yp = np.maximum(dy, 0)
    yn = np.maximum(-dy, 0)
    return np.column_stack(
        [
            np.ones(dx.size),
            xp,
            xn,
            yp,
            yn,
            np.sqrt(xp),
            np.sqrt(xn),
            np.sqrt(yp),
            np.sqrt(yn),
            dx == 0,
            dy == 0,
        ]
    ).astype(np.float64, copy=False)


def side_code(values: np.ndarray, low: int, high: int) -> np.ndarray:
    return np.where(values < low, 0, np.where(values > high, 2, 1)).astype(np.int32)


def build_groups(sx, sy, tx, ty, sp, tp, dx, dy, port_count, blocks):
    quad = (
        (dx > 0).astype(np.int32)
        + 2 * (dx < 0).astype(np.int32)
        + 3 * (dy > 0).astype(np.int32)
        + 6 * (dy < 0).astype(np.int32)
    )
    rx = (np.sign(dx) * (np.abs(dx) % 8) + 8).astype(np.int32)
    ry = (np.sign(dy) * (np.abs(dy) % 8) + 8).astype(np.int32)
    distance_bin = np.minimum(15, np.maximum(np.abs(dx), np.abs(dy)) // 32).astype(np.int32)

    groups = [
        ("src_quad", sp.astype(np.int32) * DIRECTION_COUNT + quad, port_count * DIRECTION_COUNT),
        ("dst_quad", tp.astype(np.int32) * DIRECTION_COUNT + quad, port_count * DIRECTION_COUNT),
        ("remainder", rx * 17 + ry, REMAINDER_COUNT),
        ("distance_bin", quad * 16 + distance_bin, DISTANCE_BIN_COUNT),
    ]

    for bi, block in enumerate(blocks):
        left, right = int(block["left"]), int(block["right"])
        lower, upper = int(block["lower"]), int(block["upper"])
        code = (
            side_code(sx, left, right)
            + 3 * side_code(tx, left, right)
            + 9 * side_code(sy, lower, upper)
            + 27 * side_code(ty, lower, upper)
        )
        groups.append((f"block_{bi}", code, BLOCK_RELATION_COUNT))
    return groups


def fit_model(x, y, groups, fit_mask, iterations):
    beta = np.linalg.lstsq(x[fit_mask], y[fit_mask], rcond=None)[0]
    pred = x @ beta
    effects = [np.zeros(group[2], dtype=np.float64) for group in groups]

    for _ in range(iterations):
        for gi, group in enumerate(groups):
            _, code, size = group[:3]
            shrink = float(group[3]) if len(group) > 3 else 0.0
            pred -= effects[gi][code]
            residual = y - pred
            selected_code = code[fit_mask]
            sums = np.bincount(
                selected_code,
                weights=residual[fit_mask],
                minlength=size,
            )
            counts = np.bincount(selected_code, minlength=size)
            denom = counts + shrink
            effects[gi] = np.divide(
                sums,
                denom,
                out=np.zeros_like(sums),
                where=denom != 0,
            )
            pred += effects[gi][code]
    return beta, effects, pred


def report_validation(golden, predicted, mask):
    g = golden[mask]
    p = predicted[mask]
    positive = g > 0
    scores = np.empty(g.size, dtype=np.float64)
    scores[positive] = 1.0 - np.tanh(4.0 * np.abs(p[positive] - g[positive]) / g[positive])
    scores[~positive] = (np.rint(p[~positive]) == 0).astype(np.float64)
    rel = np.abs(p[positive] - g[positive]) / g[positive]
    print(f"holdout rows       : {g.size:,}")
    print(f"holdout acc_score  : {scores.mean() * 100:.6f}")
    print(f"mean relative error: {rel.mean() * 100:.4f}%")
    print(f"median rel. error  : {np.median(rel) * 100:.4f}%")
    print(f"within 5%          : {(rel <= 0.05).mean() * 100:.4f}%")
    print(f"within 10%         : {(rel <= 0.10).mean() * 100:.4f}%")


def cpp_float(value: float) -> str:
    if not math.isfinite(value):
        raise ValueError(f"non-finite model value: {value}")
    text = format(float(value), ".9g")
    if "." not in text and "e" not in text.lower():
        text += ".0"
    return text + "f"


def cpp_string(value: str) -> str:
    return json.dumps(value, ensure_ascii=True)


def write_array(stream, ctype: str, name: str, values, per_line: int = 8):
    flat = np.asarray(values).reshape(-1)
    stream.write(f"static const {ctype} {name}[{flat.size}] = {{\n")
    for i in range(0, flat.size, per_line):
        chunk = flat[i : i + per_line]
        if ctype == "float":
            rendered = ", ".join(cpp_float(float(v)) for v in chunk)
        else:
            rendered = ", ".join(str(int(v)) for v in chunk)
        stream.write(f"    {rendered},\n")
    stream.write("};\n\n")


def write_header(
    out_path: Path,
    port_names,
    blocks,
    x_prefix,
    y_prefix,
    beta,
    effect_by_name,
):
    out_path.parent.mkdir(parents=True, exist_ok=True)
    sorted_ports = sorted((name, pid) for pid, name in enumerate(port_names))
    with out_path.open("w", encoding="utf-8", newline="\n") as stream:
        stream.write("// Generated by train_fast_model.py. Do not edit by hand.\n")
        stream.write("#pragma once\n\n#include <cstdint>\n\n")
        stream.write("namespace srb_fast_data {\n\n")
        stream.write(f"static constexpr int kPortCount = {len(port_names)};\n")
        stream.write(f"static constexpr int kBlockCount = {len(blocks)};\n")
        stream.write("static constexpr int kDirectionCount = 9;\n\n")

        stream.write("static const char* const kSortedPortNames[kPortCount] = {\n")
        for name, _ in sorted_ports:
            stream.write(f"    {cpp_string(name)},\n")
        stream.write("};\n\n")
        write_array(stream, "uint16_t", "kSortedPortIds", [pid for _, pid in sorted_ports], 12)

        write_array(stream, "float", "kBeta", beta)
        write_array(stream, "float", "kSrcQuad", effect_by_name["src_quad"])
        write_array(stream, "float", "kDstQuad", effect_by_name["dst_quad"])
        write_array(stream, "float", "kRemainder", effect_by_name["remainder"])
        write_array(stream, "float", "kDistanceBin", effect_by_name["distance_bin"])
        block_values = np.concatenate(
            [effect_by_name[f"block_{bi}"] for bi in range(len(blocks))]
        ) if blocks else np.zeros(0)
        write_array(stream, "float", "kBlockEffect", block_values)
        write_array(stream, "float", "kGeom8", effect_by_name["geom8"])
        write_array(stream, "float", "kAngle", effect_by_name["angle"])
        write_array(stream, "float", "kPortPair", effect_by_name["port_pair"])
        write_array(stream, "float", "kSrcDistance", effect_by_name["src_distance"])
        write_array(stream, "float", "kDstDistance", effect_by_name["dst_distance"])
        write_array(stream, "float", "kSrcRegion", effect_by_name["src_region"])
        write_array(stream, "float", "kDstRegion", effect_by_name["dst_region"])
        write_array(stream, "float", "kSrcRemainder", effect_by_name["src_remainder"])
        write_array(stream, "float", "kDstRemainder", effect_by_name["dst_remainder"])
        write_array(stream, "float", "kGeom4", effect_by_name["geom4"])
        write_array(stream, "float", "kDisplacement", effect_by_name["displacement"])
        write_array(stream, "uint16_t", "kXGapPrefix", x_prefix)
        write_array(stream, "uint16_t", "kYGapPrefix", y_prefix)
        write_array(stream, "int16_t", "kBlockLeft", [int(b["left"]) for b in blocks])
        write_array(stream, "int16_t", "kBlockRight", [int(b["right"]) for b in blocks])
        write_array(stream, "int16_t", "kBlockLower", [int(b["lower"]) for b in blocks])
        write_array(stream, "int16_t", "kBlockUpper", [int(b["upper"]) for b in blocks])
        stream.write("}  // namespace srb_fast_data\n")


def main() -> int:
    args = parse_args()
    if args.iterations <= 0:
        raise ValueError("--iterations must be positive")

    started = time.perf_counter()
    port_names, port_to_id, lines, blocks = load_architecture(args.ports, args.gaps)
    sx, sy, tx, ty, sp, tp, golden = load_golden(args.golden, port_to_id)
    print(f"loaded rows        : {golden.size:,}")
    print(f"ports / blocks     : {len(port_names)} / {len(blocks)}")

    dx = tx.astype(np.int32) - sx
    dy = ty.astype(np.int32) - sy
    x_prefix, y_prefix = make_line_prefix(lines)
    line_delay = np.abs(x_prefix[tx] - x_prefix[sx]) + np.abs(y_prefix[ty] - y_prefix[sy])
    target = golden - line_delay
    x = make_base_features(dx, dy)
    groups = build_groups(sx, sy, tx, ty, sp, tp, dx, dy, len(port_names), blocks)
    ax = np.abs(dx)
    ay = np.abs(dy)
    cheb = np.maximum(ax, ay)
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
    port_pair = sp.astype(np.int32) * len(port_names) + tp
    distance_bin = np.minimum(15, cheb // 32)
    src_distance = (sp.astype(np.int32) * 9 + quad) * 16 + distance_bin
    dst_distance = (tp.astype(np.int32) * 9 + quad) * 16 + distance_bin
    src_region = (sx.astype(np.int32) // 8) * 69 + sy.astype(np.int32) // 8
    dst_region = (tx.astype(np.int32) // 8) * 69 + ty.astype(np.int32) // 8
    rx = (np.sign(dx) * (ax % 8) + 8).astype(np.int32)
    ry = (np.sign(dy) * (ay % 8) + 8).astype(np.int32)
    remainder = rx * 17 + ry
    src_remainder = sp.astype(np.int32) * (17 * 17) + remainder
    dst_remainder = tp.astype(np.int32) * (17 * 17) + remainder
    geom4 = quad * (30 * 138) + np.minimum(29, ax // 4) * 138 + np.minimum(137, ay // 4)
    displacement = (dx + 119) * 1099 + (dy + 549)
    groups.extend([
        ("geom8", geom8, GEOM8_COUNT, 20.0),
        ("angle", angle, ANGLE_COUNT, 20.0),
        ("port_pair", port_pair, len(port_names) ** 2, 20.0),
        ("src_distance", src_distance, len(port_names) * 9 * 16, 20.0),
        ("dst_distance", dst_distance, len(port_names) * 9 * 16, 20.0),
        ("src_region", src_region, REGION_COUNT, 50.0),
        ("dst_region", dst_region, REGION_COUNT, 50.0),
        ("src_remainder", src_remainder, len(port_names) * 17 * 17, 30.0),
        ("dst_remainder", dst_remainder, len(port_names) * 17 * 17, 30.0),
        ("geom4", geom4, GEOM4_COUNT, 30.0),
        ("displacement", displacement, DISPLACEMENT_COUNT, 40.0),
    ])

    row_id = np.arange(golden.size)
    train_mask = ((row_id % 5) != 0) & (golden > 0)
    holdout_mask = (row_id % 5) == 0

    if not args.skip_validation:
        print("\n[deterministic 80/20 validation]")
        beta, effects, pred = fit_model(x, target, groups, train_mask, args.iterations)
        pred += line_delay
        pred[(sx == tx) & (sy == ty) & (sp == tp)] = 0.0
        report_validation(golden, pred, holdout_mask)

    print("\n[fit final model on all public rows]")
    final_mask = golden > 0
    beta, effects, _ = fit_model(x, target, groups, final_mask, args.iterations)
    effect_by_name = {groups[i][0]: effects[i] for i in range(len(groups))}
    write_header(
        args.out,
        port_names,
        blocks,
        x_prefix,
        y_prefix,
        beta,
        effect_by_name,
    )
    elapsed = time.perf_counter() - started
    print(f"generated          : {args.out.resolve()}")
    print(f"training elapsed   : {elapsed:.3f} s")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise
