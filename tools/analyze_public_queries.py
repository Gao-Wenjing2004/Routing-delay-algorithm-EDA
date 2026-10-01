#!/usr/bin/env python3
"""Analyze public SRB delay queries without changing the production solver.

The authoritative endpoint identity is the pair of names resolved through
SRB_Inst.json and SRB_Port.json, matching the baseline C++ solver.  Geometry,
Gap, Block, Atlas, and cache statistics are derived from that canonical form.

This tool intentionally distinguishes:
  * exact/reversible identities (safe for deterministic reuse on one arch),
  * geometry/environment candidates (descriptive, not proved delay-invariant),
  * actual V3 branches (only when --v3-branch-labels is supplied).
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import random
import statistics
import struct
import sys
import threading
import time
from array import array
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Iterator, Sequence

import numpy as np


DISTANCE_BINS = (
    (0, 8, "0-8"),
    (9, 16, "9-16"),
    (17, 32, "17-32"),
    (33, 64, "33-64"),
    (65, 128, "65-128"),
    (129, 256, "129-256"),
    (257, None, "257+"),
)

BRANCH_NAMES = {
    0: "parse_error",
    1: "atlas",
    2: "fallback_distance",
    3: "fallback_block",
    4: "fallback_other",
}


class PeakMemoryMonitor:
    """Best-effort process RSS sampler; peak_wset is used on Windows if exposed."""

    def __init__(self, interval_seconds: float = 0.05) -> None:
        self.interval_seconds = interval_seconds
        self.peak_rss = 0
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self._process = None
        try:
            import psutil  # type: ignore

            self._process = psutil.Process(os.getpid())
        except Exception:
            self._process = None

    def start(self) -> None:
        if self._process is None:
            return

        def sample() -> None:
            while not self._stop.is_set():
                try:
                    info = self._process.memory_info()
                    self.peak_rss = max(
                        self.peak_rss,
                        int(getattr(info, "peak_wset", 0)),
                        int(getattr(info, "rss", 0)),
                    )
                except Exception:
                    pass
                self._stop.wait(self.interval_seconds)

        self._thread = threading.Thread(target=sample, name="peak-memory", daemon=True)
        self._thread.start()

    def stop(self) -> int:
        self._stop.set()
        if self._thread is not None:
            self._thread.join(timeout=1.0)
        if self._process is not None:
            try:
                info = self._process.memory_info()
                self.peak_rss = max(
                    self.peak_rss,
                    int(getattr(info, "peak_wset", 0)),
                    int(getattr(info, "rss", 0)),
                )
            except Exception:
                pass
        return self.peak_rss


@dataclass(frozen=True)
class Block:
    block_id: int
    lower: int
    upper: int
    left: int
    right: int
    vertical_crossable: bool
    vertical_cross_delay: int
    horizontal_crossable: bool
    horizontal_cross_delay: int

    @property
    def category(self) -> str:
        if self.horizontal_crossable and self.vertical_crossable:
            return "both_crossable"
        if self.horizontal_crossable:
            return "horizontal_only"
        if self.vertical_crossable:
            return "vertical_only"
        return "neither_crossable"


@dataclass(frozen=True)
class GapLine:
    gap_id: int
    direction: str
    site: int
    delay: int


@dataclass
class Architecture:
    arch_dir: Path
    source_files: dict[str, Path]
    fingerprint_sha256: str
    width: int
    height: int
    cell_names: list[str]
    cell_x: np.ndarray
    cell_y: np.ndarray
    instance_to_cell: dict[str, int]
    coord_to_cell: dict[tuple[int, int], int]
    port_names: list[str]
    port_to_id: dict[str, int]
    port_direction: list[str]
    port_is_routing_input: np.ndarray
    gaps: list[GapLine]
    blocks: list[Block]
    nets: list[dict[str, Any]]
    arcs: list[dict[str, Any]]
    input_port_count: int
    output_port_count: int
    routing_input_count: int
    max_net_span: int
    missing_cell_count: int
    block_cell_count: int
    block_holes_match_instances: bool


@dataclass
class ParsedRows:
    source_rows_total: int
    analyzed_rows: int
    success_rows: int
    failure_rows: int
    failure_reasons: Counter[str]
    line_no: np.ndarray
    source_code: np.ndarray
    target_code: np.ndarray
    sx: np.ndarray
    sy: np.ndarray
    tx: np.ndarray
    ty: np.ndarray
    sp: np.ndarray
    tp: np.ndarray
    delay: np.ndarray
    validation_rows: list[dict[str, Any]]
    input_columns: list[str]
    delay_column: str | None


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Streaming/reproducible structural analysis of public SRB queries"
    )
    parser.add_argument("--input", required=True, type=Path, help="request or public Golden CSV")
    parser.add_argument(
        "--arch-dir",
        required=True,
        type=Path,
        help="directory containing SRB_Inst/Port/Arc/Net/Gap.json",
    )
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument(
        "--sample-size",
        type=int,
        default=None,
        help="reservoir sample size; the whole CSV is scanned but only the sample is analyzed",
    )
    parser.add_argument("--seed", type=int, default=20260827)
    parser.add_argument(
        "--detailed-block-gap",
        action=argparse.BooleanOptionalAction,
        default=True,
        help="enable request-level Block/Gap signatures and detailed distributions",
    )
    parser.add_argument(
        "--v3-branch-labels",
        type=Path,
        default=None,
        help="optional CSV containing line_number,branch from an external unchanged-V3 probe",
    )
    parser.add_argument(
        "--atlas-file",
        type=Path,
        default=None,
        help="optional local_atlas.bin; enables exact unchanged V3 branch classification in Python",
    )
    parser.add_argument(
        "--top-n", type=int, default=100, help="number of top rows written to top-*.csv"
    )
    parser.add_argument(
        "--no-charts", action="store_true", help="skip PNG generation (all tables still written)"
    )
    args = parser.parse_args(argv)
    if args.sample_size is not None and args.sample_size <= 0:
        parser.error("--sample-size must be positive")
    if args.top_n <= 0:
        parser.error("--top-n must be positive")
    if args.v3_branch_labels is not None and args.atlas_file is not None:
        parser.error("use only one of --v3-branch-labels and --atlas-file")
    return args


def _json_array(root: dict[str, Any], *names: str) -> list[dict[str, Any]]:
    for name in names:
        value = root.get(name)
        if isinstance(value, list):
            return value
    raise ValueError(f"none of JSON arrays {names!r} is present")


def _json_object(root: dict[str, Any], *names: str) -> dict[str, Any]:
    for name in names:
        value = root.get(name)
        if isinstance(value, dict):
            return value
    raise ValueError(f"none of JSON objects {names!r} is present")


def _sha256_files(paths: Sequence[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda p: p.name):
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        with path.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1 << 20), b""):
                digest.update(chunk)
        digest.update(b"\0")
    return digest.hexdigest()


def load_architecture(arch_dir: Path) -> Architecture:
    arch_dir = arch_dir.resolve()
    names = {
        "inst": "SRB_Inst.json",
        "port": "SRB_Port.json",
        "arc": "SRB_Arc.json",
        "net": "SRB_Net.json",
        "gap": "SRB_Gap.json",
    }
    paths = {key: arch_dir / value for key, value in names.items()}
    missing = [str(path) for path in paths.values() if not path.is_file()]
    if missing:
        raise FileNotFoundError("missing architecture file(s): " + ", ".join(missing))

    roots: dict[str, dict[str, Any]] = {}
    for key, path in paths.items():
        with path.open("r", encoding="utf-8-sig") as stream:
            roots[key] = json.load(stream)

    instances = _json_array(roots["inst"], "Inst", "Insts")
    ports = _json_array(roots["port"], "Port", "Ports")
    arcs = _json_array(roots["arc"], "Arcs", "Arc")
    nets = _json_array(roots["net"], "Nets", "Net")
    gap_root = _json_object(roots["gap"], "Gap", "Gaps")
    line_rows = gap_root.get("Line", gap_root.get("Lines", []))
    block_rows = gap_root.get("Block", gap_root.get("Blocks", []))
    if not isinstance(line_rows, list) or not isinstance(block_rows, list):
        raise ValueError("Gap Line/Block members must be arrays")

    instance_to_cell: dict[str, int] = {}
    coord_to_cell: dict[tuple[int, int], int] = {}
    cell_names: list[str] = []
    xs: list[int] = []
    ys: list[int] = []
    for row in instances:
        name = str(row["name"])
        x, y = int(row["x"]), int(row["y"])
        if name in instance_to_cell:
            raise ValueError(f"duplicate instance name: {name}")
        if (x, y) in coord_to_cell:
            raise ValueError(f"duplicate instance coordinate: {(x, y)}")
        cid = len(cell_names)
        instance_to_cell[name] = cid
        coord_to_cell[(x, y)] = cid
        cell_names.append(name)
        xs.append(x)
        ys.append(y)
    if not cell_names:
        raise ValueError("architecture has no instances")
    width, height = max(xs) + 1, max(ys) + 1

    port_names: list[str] = []
    port_to_id: dict[str, int] = {}
    directions: list[str] = []
    for row in ports:
        name = row.get("Name", row.get("name"))
        direction = row.get("Direction", row.get("direction"))
        if not isinstance(name, str) or not isinstance(direction, str):
            raise ValueError("Port rows require Name and Direction strings")
        normalized_direction = direction.strip().lower()
        if normalized_direction not in {"input", "output"}:
            raise ValueError(f"unknown port direction for {name}: {direction}")
        if name in port_to_id:
            raise ValueError(f"duplicate port name: {name}")
        port_to_id[name] = len(port_names)
        port_names.append(name)
        directions.append(normalized_direction)

    net_target_names: set[str] = set()
    max_net_span = 0
    for row in nets:
        source, target = str(row["from"]), str(row["to"])
        if source not in port_to_id or target not in port_to_id:
            raise ValueError(f"Net references unknown port: {source} -> {target}")
        if directions[port_to_id[source]] != "output" or directions[port_to_id[target]] != "input":
            raise ValueError(f"Net is not Output -> Input: {source} -> {target}")
        dx, dy = int(row["delta x"]), int(row["delta y"])
        if (dx == 0) == (dy == 0):
            raise ValueError(f"Net must move on exactly one axis: {source}")
        max_net_span = max(max_net_span, abs(dx), abs(dy))
        net_target_names.add(target)

    routing = np.asarray([name in net_target_names for name in port_names], dtype=np.bool_)
    gaps = [
        GapLine(
            gap_id=int(row["id"]),
            direction=str(row["direction"]).strip().lower(),
            site=int(row["site"]),
            delay=int(row["delay"]),
        )
        for row in line_rows
    ]
    for gap in gaps:
        if gap.direction not in {"horizontal", "vertical"}:
            raise ValueError(f"unknown Gap Line direction: {gap.direction}")
    blocks = [
        Block(
            block_id=int(row["id"]),
            lower=int(row["lower"]),
            upper=int(row["upper"]),
            left=int(row["left"]),
            right=int(row["right"]),
            vertical_crossable=bool(row["vertical crossable"]),
            vertical_cross_delay=int(row["vertical cross delay"]),
            horizontal_crossable=bool(row["horizontal crossable"]),
            horizontal_cross_delay=int(row["horizontal cross delay"]),
        )
        for row in block_rows
    ]

    block_coords: set[tuple[int, int]] = set()
    for block in blocks:
        for y in range(max(0, block.lower), min(height - 1, block.upper) + 1):
            for x in range(max(0, block.left), min(width - 1, block.right) + 1):
                block_coords.add((x, y))
    all_coords = {(x, y) for x in range(width) for y in range(height)}
    missing_coords = all_coords - set(coord_to_cell)

    return Architecture(
        arch_dir=arch_dir,
        source_files=paths,
        fingerprint_sha256=_sha256_files(list(paths.values())),
        width=width,
        height=height,
        cell_names=cell_names,
        cell_x=np.asarray(xs, dtype=np.int16),
        cell_y=np.asarray(ys, dtype=np.int16),
        instance_to_cell=instance_to_cell,
        coord_to_cell=coord_to_cell,
        port_names=port_names,
        port_to_id=port_to_id,
        port_direction=directions,
        port_is_routing_input=routing,
        gaps=gaps,
        blocks=blocks,
        nets=nets,
        arcs=arcs,
        input_port_count=sum(direction == "input" for direction in directions),
        output_port_count=sum(direction == "output" for direction in directions),
        routing_input_count=int(routing.sum()),
        max_net_span=max_net_span,
        missing_cell_count=len(missing_coords),
        block_cell_count=len(block_coords),
        block_holes_match_instances=missing_coords == block_coords,
    )


def parse_endpoint(spec: str, arch: Architecture) -> tuple[int, int, int, int, str]:
    """Match baseline split_spec + instance/port map validation."""
    value = spec.strip()
    slash = value.find("/")
    if slash <= 0 or slash + 1 >= len(value):
        raise ValueError("bad_endpoint_syntax")
    instance_name, port_name = value[:slash], value[slash + 1 :]
    cell_id = arch.instance_to_cell.get(instance_name)
    if cell_id is None:
        raise ValueError("unknown_instance")
    port_id = arch.port_to_id.get(port_name)
    if port_id is None:
        raise ValueError("unknown_port")
    return (
        cell_id,
        int(arch.cell_x[cell_id]),
        int(arch.cell_y[cell_id]),
        port_id,
        f"{instance_name}/{port_name}",
    )


def _normalize_fieldnames(fieldnames: Sequence[str] | None) -> tuple[dict[str, str], str | None]:
    if not fieldnames:
        raise ValueError("CSV has no header")
    normalized = {name.strip().lower(): name for name in fieldnames if name is not None}
    if "from" not in normalized or "to" not in normalized:
        raise ValueError("CSV must contain From and To columns")
    delay_column = next(
        (normalized[key] for key in ("delay", "min delay", "predict_delay") if key in normalized),
        None,
    )
    return normalized, delay_column


def _iter_or_reservoir(
    reader: csv.DictReader,
    sample_size: int | None,
    seed: int,
) -> tuple[int, Iterable[tuple[int, dict[str, str]]]]:
    if sample_size is None:
        # Caller consumes the reader immediately while its stream remains open.
        return -1, ((reader.line_num, row) for row in reader if row)
    rng = random.Random(seed)
    reservoir: list[tuple[int, dict[str, str]]] = []
    total = 0
    for row in reader:
        if not row:
            continue
        total += 1
        item = (reader.line_num, dict(row))
        if len(reservoir) < sample_size:
            reservoir.append(item)
        else:
            replace = rng.randrange(total)
            if replace < sample_size:
                reservoir[replace] = item
    reservoir.sort(key=lambda item: item[0])
    return total, reservoir


def read_queries(
    input_path: Path,
    arch: Architecture,
    output_dir: Path,
    sample_size: int | None,
    seed: int,
) -> ParsedRows:
    line_no_values = array("I")
    source_code_values = array("I")
    target_code_values = array("I")
    sx_values, sy_values, tx_values, ty_values = (array("h") for _ in range(4))
    sp_values, tp_values = array("H"), array("H")
    delay_values = array("d")
    failure_reasons: Counter[str] = Counter()
    validation: list[dict[str, Any]] = []
    validation_seen = 0
    validation_rng = random.Random(seed ^ 0x5EED5EED)
    analyzed_rows = 0

    failures_path = output_dir / "parse_failures.csv"
    with input_path.open("r", encoding="utf-8-sig", newline="") as stream, failures_path.open(
        "w", encoding="utf-8", newline=""
    ) as failure_stream:
        reader = csv.DictReader(stream)
        normalized, delay_column = _normalize_fieldnames(reader.fieldnames)
        input_columns = list(reader.fieldnames or [])
        from_column, to_column = normalized["from"], normalized["to"]
        failure_writer = csv.writer(failure_stream)
        failure_writer.writerow(["line_number", "reason", "From", "To", "raw_row_json"])

        scanned_total, rows = _iter_or_reservoir(reader, sample_size, seed)
        for line_number, row in rows:
            analyzed_rows += 1
            raw_from = row.get(from_column, "") or ""
            raw_to = row.get(to_column, "") or ""
            try:
                scell, sx, sy, sp, canonical_from = parse_endpoint(raw_from, arch)
                tcell, tx, ty, tp, canonical_to = parse_endpoint(raw_to, arch)
                parsed_delay = math.nan
                if delay_column is not None:
                    raw_delay = (row.get(delay_column, "") or "").strip()
                    if not raw_delay:
                        raise ValueError("missing_delay")
                    try:
                        parsed_delay = float(raw_delay)
                    except ValueError as exc:
                        raise ValueError("invalid_delay") from exc
                    if not math.isfinite(parsed_delay):
                        raise ValueError("nonfinite_delay")
            except ValueError as exc:
                reason = str(exc)
                failure_reasons[reason] += 1
                failure_writer.writerow(
                    [line_number, reason, raw_from, raw_to, json.dumps(row, ensure_ascii=False)]
                )
                continue

            port_count = len(arch.port_names)
            source_code = scell * port_count + sp
            target_code = tcell * port_count + tp
            line_no_values.append(line_number)
            source_code_values.append(source_code)
            target_code_values.append(target_code)
            sx_values.append(sx)
            sy_values.append(sy)
            tx_values.append(tx)
            ty_values.append(ty)
            sp_values.append(sp)
            tp_values.append(tp)
            delay_values.append(parsed_delay)

            validation_seen += 1
            check = {
                "line_number": line_number,
                "raw_from": raw_from,
                "raw_to": raw_to,
                "canonical_from": canonical_from,
                "canonical_to": canonical_to,
                "from_x": sx,
                "from_y": sy,
                "from_port": arch.port_names[sp],
                "to_x": tx,
                "to_y": ty,
                "to_port": arch.port_names[tp],
                "roundtrip_ok": raw_from.strip() == canonical_from and raw_to.strip() == canonical_to,
            }
            if len(validation) < 100:
                validation.append(check)
            else:
                replace = validation_rng.randrange(validation_seen)
                if replace < 100:
                    validation[replace] = check

        if sample_size is None:
            scanned_total = analyzed_rows

    return ParsedRows(
        source_rows_total=scanned_total,
        analyzed_rows=analyzed_rows,
        success_rows=len(source_code_values),
        failure_rows=sum(failure_reasons.values()),
        failure_reasons=failure_reasons,
        line_no=np.frombuffer(line_no_values, dtype=np.uint32).copy(),
        source_code=np.frombuffer(source_code_values, dtype=np.uint32).copy(),
        target_code=np.frombuffer(target_code_values, dtype=np.uint32).copy(),
        sx=np.frombuffer(sx_values, dtype=np.int16).copy(),
        sy=np.frombuffer(sy_values, dtype=np.int16).copy(),
        tx=np.frombuffer(tx_values, dtype=np.int16).copy(),
        ty=np.frombuffer(ty_values, dtype=np.int16).copy(),
        sp=np.frombuffer(sp_values, dtype=np.uint16).copy(),
        tp=np.frombuffer(tp_values, dtype=np.uint16).copy(),
        delay=np.frombuffer(delay_values, dtype=np.float64).copy(),
        validation_rows=sorted(validation, key=lambda row: row["line_number"]),
        input_columns=input_columns,
        delay_column=delay_column,
    )


def endpoint_text(code: int, arch: Architecture) -> str:
    port_count = len(arch.port_names)
    cell_id, port_id = divmod(int(code), port_count)
    return f"{arch.cell_names[cell_id]}/{arch.port_names[port_id]}"


def endpoint_parts(code: int, arch: Architecture) -> tuple[int, int, int, str]:
    port_count = len(arch.port_names)
    cell_id, port_id = divmod(int(code), port_count)
    return (
        int(arch.cell_x[cell_id]),
        int(arch.cell_y[cell_id]),
        port_id,
        arch.port_names[port_id],
    )


def packed_exact(source_code: np.ndarray, target_code: np.ndarray) -> np.ndarray:
    return (source_code.astype(np.uint64) << np.uint64(32)) | target_code.astype(np.uint64)


def packed_template(
    sp: np.ndarray,
    tp: np.ndarray,
    dx: np.ndarray,
    dy: np.ndarray,
    width: int,
    height: int,
) -> np.ndarray:
    dxu = dx.astype(np.int64) + (width - 1)
    dyu = dy.astype(np.int64) + (height - 1)
    if (
        np.any(dxu < 0)
        or np.any(dxu > 0xFFFF)
        or np.any(dyu < 0)
        or np.any(dyu > 0xFFFF)
        or np.any(sp.astype(np.uint64) > 0xFFFF)
        or np.any(tp.astype(np.uint64) > 0xFFFF)
    ):
        raise ValueError("template fields do not fit reversible 16-bit packing")
    return (
        (sp.astype(np.uint64) << np.uint64(48))
        | (tp.astype(np.uint64) << np.uint64(32))
        | (dxu.astype(np.uint64) << np.uint64(16))
        | dyu.astype(np.uint64)
    )


def unpack_template(key: int, arch: Architecture) -> tuple[int, int, int, int]:
    sp = (int(key) >> 48) & 0xFFFF
    tp = (int(key) >> 32) & 0xFFFF
    dx = ((int(key) >> 16) & 0xFFFF) - (arch.width - 1)
    dy = (int(key) & 0xFFFF) - (arch.height - 1)
    return sp, tp, dx, dy


def quantiles(values: np.ndarray, points: Sequence[int] = (50, 75, 90, 95, 99)) -> dict[str, float]:
    if values.size == 0:
        return {f"p{point}": math.nan for point in points} | {"max": math.nan}
    result = {f"p{point}": float(np.percentile(values, point)) for point in points}
    result["max"] = float(np.max(values))
    return result


def ratio(numerator: int | float, denominator: int | float) -> float:
    return float(numerator) / float(denominator) if denominator else math.nan


def frequency_distribution(counts: np.ndarray) -> dict[str, Any]:
    if counts.size == 0:
        return {}
    exact_frequency = np.bincount(counts.astype(np.int64))
    exact_rows = [
        {
            "frequency": int(freq),
            "unique_keys": int(key_count),
            "requests": int(freq * key_count),
        }
        for freq, key_count in enumerate(exact_frequency)
        if freq > 0 and key_count
    ]
    bins = (
        (1, 1, "1"),
        (2, 5, "2-5"),
        (6, 10, "6-10"),
        (11, 100, "11-100"),
        (101, None, "101+"),
    )
    binned = []
    for low, high, label in bins:
        mask = counts >= low
        if high is not None:
            mask &= counts <= high
        binned.append(
            {
                "bin": label,
                "unique_keys": int(mask.sum()),
                "requests": int(counts[mask].sum()),
            }
        )
    return {"exact_frequency": exact_rows, "bins": binned}


def reuse_stats(counts: np.ndarray, total: int, top_ks: Sequence[int] = (10, 50, 100, 500, 1000)) -> dict[str, Any]:
    sorted_counts = np.sort(counts)[::-1]
    unique_count = int(counts.size)
    repeated_request_count = int(counts[counts >= 2].sum())
    return {
        "unique": unique_count,
        "mean_frequency": float(np.mean(counts)) if counts.size else math.nan,
        "median_frequency": float(np.median(counts)) if counts.size else math.nan,
        "p90_frequency": float(np.percentile(counts, 90)) if counts.size else math.nan,
        "p95_frequency": float(np.percentile(counts, 95)) if counts.size else math.nan,
        "p99_frequency": float(np.percentile(counts, 99)) if counts.size else math.nan,
        "max_frequency": int(np.max(counts)) if counts.size else 0,
        "one_minus_unique_over_total": ratio(total - unique_count, total),
        "requests_from_keys_seen_at_least_twice": repeated_request_count,
        "requests_from_keys_seen_at_least_twice_ratio": ratio(repeated_request_count, total),
        "frequency_distribution": frequency_distribution(counts),
        "top_k_coverage": {
            str(k): {
                "requests": int(sorted_counts[:k].sum()),
                "ratio": ratio(int(sorted_counts[:k].sum()), total),
            }
            for k in top_ks
        },
    }


def classify_environment(
    rows: ParsedRows,
    arch: Architecture,
) -> dict[str, np.ndarray]:
    sx, sy, tx, ty = (a.astype(np.int32) for a in (rows.sx, rows.sy, rows.tx, rows.ty))
    min_x, max_x = np.minimum(sx, tx), np.maximum(sx, tx)
    min_y, max_y = np.minimum(sy, ty), np.maximum(sy, ty)
    n = rows.success_rows

    if len(arch.blocks) > 64 or len(arch.gaps) > 64:
        raise ValueError("current exact environment bitmask implementation supports up to 64 Blocks/Gap Lines")
    block_mask = np.zeros(n, dtype=np.uint64)
    block_count = np.zeros(n, dtype=np.uint8)
    for bi, block in enumerate(arch.blocks):
        hit = (
            (min_x <= block.right)
            & (max_x >= block.left)
            & (min_y <= block.upper)
            & (max_y >= block.lower)
        )
        block_mask[hit] |= np.uint64(1) << np.uint64(bi)
        block_count += hit.astype(np.uint8)

    gap_mask = np.zeros(n, dtype=np.uint64)
    horizontal_gap_count = np.zeros(n, dtype=np.uint8)
    vertical_gap_count = np.zeros(n, dtype=np.uint8)
    gap_delay = np.zeros(n, dtype=np.int32)
    for gi, gap in enumerate(arch.gaps):
        if gap.direction == "vertical":
            crossed = (min_x <= gap.site) & (gap.site < max_x)
            vertical_gap_count += crossed.astype(np.uint8)
        else:
            crossed = (min_y <= gap.site) & (gap.site < max_y)
            horizontal_gap_count += crossed.astype(np.uint8)
        gap_mask[crossed] |= np.uint64(1) << np.uint64(gi)
        gap_delay[crossed] += gap.delay

    boundary_clearance = np.minimum.reduce(
        [sx, sy, (arch.width - 1) - sx, (arch.height - 1) - sy]
    ).astype(np.int16)
    return {
        "block_mask": block_mask,
        "block_count": block_count,
        "block_related": block_mask != 0,
        "gap_mask": gap_mask,
        "gap_related": gap_mask != 0,
        "horizontal_gap_count": horizontal_gap_count,
        "vertical_gap_count": vertical_gap_count,
        "gap_count": horizontal_gap_count.astype(np.uint16) + vertical_gap_count.astype(np.uint16),
        "gap_delay": gap_delay,
        "boundary_clearance": boundary_clearance,
    }


def load_branch_labels(path: Path, row_line_numbers: np.ndarray) -> tuple[np.ndarray, dict[str, Any]]:
    max_line = int(max(int(row_line_numbers.max(initial=0)), 2))
    dense = np.full(max_line + 1, 255, dtype=np.uint8)
    raw_counts: Counter[str] = Counter()
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.DictReader(stream)
        required = {"line_number", "branch"}
        if not reader.fieldnames or not required.issubset(reader.fieldnames):
            raise ValueError("V3 branch label CSV must contain line_number,branch")
        reverse = {value: key for key, value in BRANCH_NAMES.items()}
        for row in reader:
            line_number = int(row["line_number"])
            branch_name = row["branch"].strip()
            if branch_name not in reverse:
                raise ValueError(f"unknown V3 branch label: {branch_name}")
            if line_number >= dense.size:
                dense.resize(line_number + 1, refcheck=False)
                dense[max_line + 1 :] = 255
                max_line = line_number
            dense[line_number] = reverse[branch_name]
            raw_counts[branch_name] += 1
    labels = dense[row_line_numbers.astype(np.int64)]
    missing = int((labels == 255).sum())
    if missing:
        raise ValueError(f"V3 branch labels missing for {missing} analyzed successful rows")
    return labels, {"file_counts": dict(raw_counts), "matched_success_rows": int(labels.size)}


def build_local_atlas_port_model(arch: Architecture) -> dict[str, Any]:
    """Reproduce prepare_local_arch.py's port/route tables in memory."""
    port_count = len(arch.port_names)
    input_ports = [pid for pid, direction in enumerate(arch.port_direction) if direction == "input"]
    port_to_input = np.full(port_count, -1, dtype=np.int16)
    for iid, pid in enumerate(input_ports):
        port_to_input[pid] = iid

    input_to_route = np.full(len(input_ports), -1, dtype=np.int16)
    route_to_input: list[int] = []
    output_net: list[tuple[int, int, int]] = [(-1, 0, 0) for _ in range(port_count)]
    for net in arch.nets:
        output_pid = arch.port_to_id[str(net["from"])]
        input_pid = arch.port_to_id[str(net["to"])]
        iid = int(port_to_input[input_pid])
        if input_to_route[iid] < 0:
            input_to_route[iid] = len(route_to_input)
            route_to_input.append(iid)
        output_net[output_pid] = (
            int(input_to_route[iid]),
            int(net["delta x"]),
            int(net["delta y"]),
        )

    arc_map: dict[tuple[int, int], int] = {}
    for arc in arch.arcs:
        from_pid = arch.port_to_id[str(arc["from"])]
        to_pid = arch.port_to_id[str(arc["to"])]
        key = (int(port_to_input[from_pid]), to_pid)
        arc_map[key] = min(arc_map.get(key, 1 << 30), int(arc["delay"]))

    transitions: list[list[tuple[int, int, int, int]]] = [[] for _ in input_ports]
    direct: list[dict[int, int]] = [dict() for _ in input_ports]
    target_arcs: list[list[tuple[int, int]]] = [[] for _ in range(port_count)]
    for (iid, output_pid), cost in arc_map.items():
        direct[iid][output_pid] = cost
        route, dx, dy = output_net[output_pid]
        if route >= 0:
            transitions[iid].append((route, dx, dy, cost))
        source_route = int(input_to_route[iid])
        if source_route >= 0:
            target_arcs[output_pid].append((source_route, cost))
    for iid, edges in enumerate(transitions):
        best: dict[tuple[int, int, int], int] = {}
        for route, dx, dy, cost in edges:
            key = (route, dx, dy)
            best[key] = min(best.get(key, 1 << 30), cost)
        transitions[iid] = [
            (route, dx, dy, cost) for (route, dx, dy), cost in sorted(best.items())
        ]
    for edges in target_arcs:
        edges.sort()
    return {
        "port_to_input": port_to_input,
        "input_to_route": input_to_route,
        "output_net": output_net,
        "transitions": transitions,
        "direct": direct,
        "target_arcs": target_arcs,
        "route_count": len(route_to_input),
    }


def classify_v3_from_atlas(
    atlas_path: Path,
    rows: ParsedRows,
    arch: Architecture,
    chebyshev: np.ndarray,
    block_related: np.ndarray,
    query_radius: int = 48,
) -> tuple[np.ndarray, dict[str, Any]]:
    """Classify the unchanged V3 Atlas/fallback branch using its binary Atlas."""
    source_label = str(atlas_path)
    atlas_path = atlas_path.resolve()
    header_format = "<8s6IQ"
    header_size = struct.calcsize(header_format)
    with atlas_path.open("rb") as stream:
        header_bytes = stream.read(header_size)
    if len(header_bytes) != header_size:
        raise ValueError(f"truncated Atlas header: {atlas_path}")
    magic, version, radius, routes, cells, sources, declared_header_bytes, values = struct.unpack(
        header_format, header_bytes
    )
    model = build_local_atlas_port_model(arch)
    expected_cells = (2 * radius + 1) ** 2
    expected_values = routes * cells * routes
    expected_bytes = header_size + expected_values * 2
    if (
        not magic.startswith(b"SRBLAT2")
        or version != 1
        or radius < 56
        or cells != expected_cells
        or sources != routes
        or routes != model["route_count"]
        or declared_header_bytes != header_size
        or values != expected_values
        or atlas_path.stat().st_size != expected_bytes
    ):
        raise ValueError("Atlas header/size is incompatible with architecture or V3")

    atlas = np.memmap(
        atlas_path,
        mode="r",
        dtype="<u2",
        offset=header_size,
        shape=(routes, cells, routes),
    )
    labels = np.full(rows.success_rows, 4, dtype=np.uint8)
    if query_radius < 0 or query_radius > radius:
        raise ValueError(f"query_radius must be in [0,{radius}], got {query_radius}")
    labels[chebyshev > query_radius] = 2
    labels[(chebyshev <= query_radius) & block_related] = 3
    candidates = np.flatnonzero((chebyshev <= query_radius) & ~block_related)
    width = 2 * radius + 1
    port_to_input = model["port_to_input"]
    input_to_route = model["input_to_route"]
    output_net = model["output_net"]
    transitions = model["transitions"]
    target_arcs = model["target_arcs"]
    direct = model["direct"]

    for row_index in candidates:
        source_pid = int(rows.sp[row_index])
        target_pid = int(rows.tp[row_index])
        dx = int(rows.tx[row_index]) - int(rows.sx[row_index])
        dy = int(rows.ty[row_index]) - int(rows.sy[row_index])
        source_input = int(port_to_input[source_pid])
        target_input = int(port_to_input[target_pid])

        seeds: list[tuple[int, int, int]] = []
        if source_input >= 0:
            source_route = int(input_to_route[source_input])
            if source_route >= 0:
                seeds.append((source_route, 0, 0))
            else:
                seeds.extend(
                    (route, seed_dx, seed_dy)
                    for route, seed_dx, seed_dy, _ in transitions[source_input]
                )
        else:
            route, seed_dx, seed_dy = output_net[source_pid]
            if route >= 0:
                seeds.append((route, seed_dx, seed_dy))
        if not seeds:
            continue

        terminals: list[int] = []
        if target_input >= 0:
            target_route = int(input_to_route[target_input])
            if target_route < 0:
                continue
            terminals.append(target_route)
        else:
            terminals.extend(route for route, _ in target_arcs[target_pid])

        hit = False
        if dx == 0 and dy == 0 and source_input >= 0 and target_input < 0:
            hit = target_pid in direct[source_input]
        if not hit and terminals:
            for source_route, seed_dx, seed_dy in seeds:
                residual_dx = dx - seed_dx
                residual_dy = dy - seed_dy
                if not (-radius <= residual_dx <= radius and -radius <= residual_dy <= radius):
                    continue
                cell = (residual_dy + radius) * width + (residual_dx + radius)
                for target_route in terminals:
                    if int(atlas[source_route, cell, target_route]) != 0xFFFF:
                        hit = True
                        break
                if hit:
                    break
        if hit:
            labels[row_index] = 1
    del atlas
    return labels, {
        "source": source_label,
        "header": {
            "version": version,
            "radius": radius,
            "routes": routes,
            "cells": cells,
            "values": values,
            "bytes": expected_bytes,
        },
        "classification": (
            "Python reproduction of HybridEstimator::predict_local reachability; "
            f"query_radius={query_radius}"
        ),
    }


def direct_one_net_candidates(arch: Architecture) -> dict[tuple[int, int], list[tuple[int, int, int]]]:
    """Map (source port,target port) to possible one-Net (dx,dy,output pid).

    The candidate includes zero or one source Arc, exactly one Net, and zero or
    one target Arc.  It is not a statement about the globally shortest path.
    """
    port_count = len(arch.port_names)
    input_index = [-1] * port_count
    input_ports = [pid for pid, direction in enumerate(arch.port_direction) if direction == "input"]
    for iid, pid in enumerate(input_ports):
        input_index[pid] = iid
    arc_map: dict[tuple[int, int], int] = {}
    outgoing_outputs: dict[int, list[int]] = defaultdict(list)
    target_outputs: dict[int, list[int]] = defaultdict(list)
    for row in arch.arcs:
        fp, tp = arch.port_to_id[str(row["from"])], arch.port_to_id[str(row["to"])]
        delay = int(row["delay"])
        key = (fp, tp)
        if delay < arc_map.get(key, 1 << 30):
            arc_map[key] = delay
    for fp, tp in arc_map:
        outgoing_outputs[fp].append(tp)
        target_outputs[fp].append(tp)

    net_by_output: dict[int, tuple[int, int, int]] = {}
    for row in arch.nets:
        op = arch.port_to_id[str(row["from"])]
        ip = arch.port_to_id[str(row["to"])]
        net_by_output[op] = (ip, int(row["delta x"]), int(row["delta y"]))

    candidates: dict[tuple[int, int], set[tuple[int, int, int]]] = defaultdict(set)
    for source_pid in range(port_count):
        if arch.port_direction[source_pid] == "output":
            possible_outputs = [source_pid]
        else:
            possible_outputs = outgoing_outputs.get(source_pid, [])
        for output_pid in possible_outputs:
            net = net_by_output.get(output_pid)
            if net is None:
                continue
            target_input, dx, dy = net
            terminal_ports = [target_input] + target_outputs.get(target_input, [])
            for target_pid in terminal_ports:
                candidates[(source_pid, target_pid)].add((dx, dy, output_pid))
    return {key: sorted(value) for key, value in candidates.items()}


def simulate_net_move(
    x: int,
    y: int,
    dx: int,
    dy: int,
    arch: Architecture,
) -> tuple[int, int, bool, bool]:
    """Reproduce baseline Block counting semantics for one Net.

    Returns final x/y, whether a Block rectangle was touched, and connectivity.
    """
    horizontal = dx != 0
    step = 1 if (dx if horizontal else dy) > 0 else -1
    needed = abs(dx if horizontal else dy)
    counted = 0
    touched = False
    guard = 0
    guard_limit = (arch.width + arch.height) * 4 + needed + 16
    while counted < needed:
        guard += 1
        if guard > guard_limit:
            return x, y, touched, False
        if horizontal:
            x += step
        else:
            y += step
        if x < 0 or x >= arch.width or y < 0 or y >= arch.height:
            return x, y, touched, False
        if (x, y) in arch.coord_to_cell:
            counted += 1
            continue
        block = next(
            (
                candidate
                for candidate in arch.blocks
                if candidate.left <= x <= candidate.right and candidate.lower <= y <= candidate.upper
            ),
            None,
        )
        if block is None:
            counted += 1
            continue
        touched = True
        crossable = block.horizontal_crossable if horizontal else block.vertical_crossable
        if not crossable:
            counted += 1
    connected = (x, y) in arch.coord_to_cell
    return x, y, touched, connected


def classify_direct_one_net(rows: ParsedRows, arch: Architecture) -> tuple[np.ndarray, np.ndarray]:
    candidates = direct_one_net_candidates(arch)
    n = rows.success_rows
    direct = np.zeros(n, dtype=np.bool_)
    block_intersection = np.zeros(n, dtype=np.bool_)
    max_cross_width = max((b.right - b.left + 1 for b in arch.blocks if b.horizontal_crossable), default=0)
    max_cross_height = max((b.upper - b.lower + 1 for b in arch.blocks if b.vertical_crossable), default=0)
    for i in range(n):
        key = (int(rows.sp[i]), int(rows.tp[i]))
        options = candidates.get(key)
        if not options:
            continue
        observed_dx = int(rows.tx[i]) - int(rows.sx[i])
        observed_dy = int(rows.ty[i]) - int(rows.sy[i])
        for dx, dy, _ in options:
            if dx != 0:
                if observed_dy != 0 or np.sign(observed_dx) != np.sign(dx):
                    continue
                if abs(observed_dx) < abs(dx) or abs(observed_dx) > abs(dx) + max_cross_width:
                    continue
            else:
                if observed_dx != 0 or np.sign(observed_dy) != np.sign(dy):
                    continue
                if abs(observed_dy) < abs(dy) or abs(observed_dy) > abs(dy) + max_cross_height:
                    continue
            fx, fy, touched, connected = simulate_net_move(
                int(rows.sx[i]), int(rows.sy[i]), dx, dy, arch
            )
            if connected and fx == int(rows.tx[i]) and fy == int(rows.ty[i]):
                direct[i] = True
                block_intersection[i] |= touched
    return direct, block_intersection


def _safe_json(value: Any) -> Any:
    if isinstance(value, dict):
        return {str(key): _safe_json(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [_safe_json(item) for item in value]
    if isinstance(value, np.ndarray):
        return [_safe_json(item) for item in value.tolist()]
    if isinstance(value, np.generic):
        return value.item()
    if isinstance(value, Path):
        return str(value)
    if isinstance(value, float) and not math.isfinite(value):
        return None
    return value


def write_csv(path: Path, header: Sequence[str], rows: Iterable[Sequence[Any]]) -> None:
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(header)
        writer.writerows(rows)


def grouped_delay_stats(codes: np.ndarray, delay: np.ndarray, size: int) -> dict[str, np.ndarray]:
    valid = np.isfinite(delay)
    counts = np.bincount(codes[valid].astype(np.int64), minlength=size).astype(np.int64)
    sums = np.bincount(codes[valid].astype(np.int64), weights=delay[valid], minlength=size)
    sumsq = np.bincount(codes[valid].astype(np.int64), weights=delay[valid] ** 2, minlength=size)
    mean = np.divide(sums, counts, out=np.full(size, np.nan), where=counts > 0)
    variance = np.divide(sumsq, counts, out=np.zeros(size), where=counts > 0) - np.nan_to_num(mean) ** 2
    std = np.sqrt(np.maximum(variance, 0.0))
    return {"count": counts, "mean": mean, "std": std}


def periodic_delay_analysis(
    delay: np.ndarray,
    dx: np.ndarray,
    dy: np.ndarray,
    port_pair: np.ndarray,
    port_pair_size: int,
) -> dict[str, Any]:
    valid = np.isfinite(delay)
    if int(valid.sum()) < 10:
        return {"available": False, "reason": "fewer than 10 valid Golden delays"}
    y = delay[valid].astype(np.float64)
    dxv, dyv = dx[valid].astype(np.float64), dy[valid].astype(np.float64)
    pp = port_pair[valid].astype(np.int64)
    xp, xn = np.maximum(dxv, 0), np.maximum(-dxv, 0)
    yp, yn = np.maximum(dyv, 0), np.maximum(-dyv, 0)
    base_features = np.column_stack([np.ones(y.size), xp, xn, yp, yn])
    beta = np.linalg.lstsq(base_features, y, rcond=None)[0]
    global_pred = base_features @ beta
    global_residual = y - global_pred

    counts = np.bincount(pp, minlength=port_pair_size).astype(np.float64)
    y_sum = np.bincount(pp, weights=y, minlength=port_pair_size)
    y_mean = np.divide(y_sum, counts, out=np.zeros_like(y_sum), where=counts > 0)
    geom = np.column_stack([xp, xn, yp, yn])
    geom_mean = np.column_stack(
        [
            np.divide(
                np.bincount(pp, weights=geom[:, j], minlength=port_pair_size),
                counts,
                out=np.zeros(port_pair_size),
                where=counts > 0,
            )
            for j in range(geom.shape[1])
        ]
    )
    yc = y - y_mean[pp]
    xc = geom - geom_mean[pp]
    controlled_beta = np.linalg.lstsq(xc, yc, rcond=None)[0]
    controlled_pred = y_mean[pp] + xc @ controlled_beta
    residual = y - controlled_pred

    manhattan = np.abs(dxv) + np.abs(dyv)
    hinge = np.column_stack([np.maximum(manhattan - knot, 0) for knot in (8, 16, 32, 64, 128, 256)])
    piece_geom = np.column_stack([geom, hinge])
    piece_mean = np.column_stack(
        [
            np.divide(
                np.bincount(pp, weights=piece_geom[:, j], minlength=port_pair_size),
                counts,
                out=np.zeros(port_pair_size),
                where=counts > 0,
            )
            for j in range(piece_geom.shape[1])
        ]
    )
    piece_xc = piece_geom - piece_mean[pp]
    piece_beta = np.linalg.lstsq(piece_xc, yc, rcond=None)[0]
    piece_residual = y - (y_mean[pp] + piece_xc @ piece_beta)

    abs_rx = (np.abs(dxv).astype(np.int64) % 8)
    abs_ry = (np.abs(dyv).astype(np.int64) % 8)
    remainder = abs_rx * 8 + abs_ry
    rem_count = np.bincount(remainder, minlength=64).astype(np.int64)
    rem_sum = np.bincount(remainder, weights=residual, minlength=64)
    rem_mean = np.divide(rem_sum, rem_count, out=np.zeros(64), where=rem_count > 0)
    remainder_adjusted = residual - rem_mean[remainder]

    pair_remainder = pp * 64 + remainder
    unique_pr, inv_pr, counts_pr = np.unique(pair_remainder, return_inverse=True, return_counts=True)
    sums_pr = np.bincount(inv_pr, weights=y)
    sumsq_pr = np.bincount(inv_pr, weights=y * y)
    means_pr = sums_pr / counts_pr
    std_pr = np.sqrt(np.maximum(sumsq_pr / counts_pr - means_pr * means_pr, 0.0))
    repeated_group = counts_pr >= 2
    within_pair_rem_residual = residual - (
        np.bincount(inv_pr, weights=residual) / counts_pr
    )[inv_pr]

    def metrics(res: np.ndarray, target: np.ndarray = y) -> dict[str, float]:
        sse = float(np.dot(res, res))
        centered = target - float(np.mean(target))
        sst = float(np.dot(centered, centered))
        return {
            "mae": float(np.mean(np.abs(res))),
            "rmse": float(np.sqrt(np.mean(res * res))),
            "residual_std": float(np.std(res)),
            "r2": 1.0 - sse / sst if sst else math.nan,
            **{f"abs_residual_p{p}": float(np.percentile(np.abs(res), p)) for p in (50, 90, 95, 99)},
        }

    residual_by_mod = []
    for rx in range(8):
        for ry in range(8):
            code = rx * 8 + ry
            mask = remainder == code
            residual_by_mod.append(
                {
                    "abs_dx_mod8": rx,
                    "abs_dy_mod8": ry,
                    "count": int(mask.sum()),
                    "share": ratio(int(mask.sum()), y.size),
                    "mean_controlled_linear_residual": float(np.mean(residual[mask])) if np.any(mask) else math.nan,
                    "std_controlled_linear_residual": float(np.std(residual[mask])) if np.any(mask) else math.nan,
                }
            )

    correlations = {}
    for name, values in {
        "dx": dxv,
        "dy": dyv,
        "abs_dx": np.abs(dxv),
        "abs_dy": np.abs(dyv),
        "manhattan": manhattan,
        "chebyshev": np.maximum(np.abs(dxv), np.abs(dyv)),
        "euclidean": np.hypot(dxv, dyv),
    }.items():
        correlations[name] = float(np.corrcoef(y, values)[0, 1]) if np.std(values) else math.nan

    base_metrics = metrics(residual)
    rem_metrics = metrics(remainder_adjusted)
    piece_metrics = metrics(piece_residual)
    pair_rem_metrics = metrics(within_pair_rem_residual)
    return {
        "available": True,
        "golden_rows": int(y.size),
        "delay_quantiles": quantiles(y),
        "delay_correlations": correlations,
        "global_directional_linear": {"coefficients": beta.tolist(), **metrics(global_residual)},
        "port_pair_controlled_directional_linear": {
            "coefficients": controlled_beta.tolist(),
            **base_metrics,
        },
        "port_pair_controlled_piecewise_linear": {
            "hinge_knots_manhattan": [8, 16, 32, 64, 128, 256],
            "coefficients": piece_beta.tolist(),
            **piece_metrics,
            "rmse_reduction_vs_controlled_linear": ratio(
                base_metrics["rmse"] - piece_metrics["rmse"], base_metrics["rmse"]
            ),
        },
        "abs_mod8_residual_adjustment_in_sample": {
            **rem_metrics,
            "rmse_reduction_vs_controlled_linear": ratio(
                base_metrics["rmse"] - rem_metrics["rmse"], base_metrics["rmse"]
            ),
            "between_group_residual_variance_share": ratio(
                float(np.sum(rem_count * rem_mean * rem_mean)), float(np.dot(residual, residual))
            ),
        },
        "port_pair_abs_mod8_groups": {
            "unique_groups": int(unique_pr.size),
            "groups_with_at_least_2_rows": int(repeated_group.sum()),
            "requests_in_groups_with_at_least_2_rows": int(counts_pr[repeated_group].sum()),
            "delay_std_quantiles_for_repeated_groups": quantiles(std_pr[repeated_group]),
            "pooled_within_group_rmse_after_controlled_linear": pair_rem_metrics["rmse"],
            "in_sample_rmse_reduction_vs_controlled_linear": ratio(
                base_metrics["rmse"] - pair_rem_metrics["rmse"], base_metrics["rmse"]
            ),
        },
        "residual_by_abs_mod8": residual_by_mod,
        "warning": (
            "All regressions and group corrections are descriptive in-sample statistics, not a trained "
            "production model or hidden-set generalization evidence."
        ),
    }


def analyze(
    rows: ParsedRows,
    arch: Architecture,
    detailed_block_gap: bool,
    branch_labels_path: Path | None,
    atlas_path: Path | None,
) -> tuple[dict[str, Any], dict[str, Any]]:
    n = rows.success_rows
    if n == 0:
        raise ValueError("no successfully parsed requests")
    dx = rows.tx.astype(np.int32) - rows.sx.astype(np.int32)
    dy = rows.ty.astype(np.int32) - rows.sy.astype(np.int32)
    abs_dx, abs_dy = np.abs(dx), np.abs(dy)
    manhattan = abs_dx + abs_dy
    chebyshev = np.maximum(abs_dx, abs_dy)
    euclidean = np.hypot(dx.astype(np.float64), dy.astype(np.float64))
    orientation_code = np.where(
        (dx == 0) & (dy == 0),
        0,
        np.where(dy == 0, 1, np.where(dx == 0, 2, 3)),
    ).astype(np.uint8)

    exact_key = packed_exact(rows.source_code, rows.target_code)
    exact_unique, exact_counts = np.unique(exact_key, return_counts=True)
    source_unique, source_counts = np.unique(rows.source_code, return_counts=True)
    target_unique, target_counts = np.unique(rows.target_code, return_counts=True)
    port_count = len(arch.port_names)
    port_pair = rows.sp.astype(np.int64) * port_count + rows.tp.astype(np.int64)
    port_pair_size = port_count * port_count
    port_pair_counts = np.bincount(port_pair, minlength=port_pair_size)
    raw_template = packed_template(rows.sp, rows.tp, dx, dy, arch.width, arch.height)
    template_unique, template_counts = np.unique(raw_template, return_counts=True)

    environment = classify_environment(rows, arch)
    block_related = environment["block_related"]
    gap_related = environment["gap_related"]
    ordinary = ~block_related & ~gap_related
    ordinary_template_unique = np.unique(raw_template[ordinary])

    # Geometry + mod-8 is intentionally redundant because the modulo fields are
    # deterministic functions of dx/dy.  Count equality is a required invariant.
    geom_mod8_unique_count = int(template_unique.size)

    env_dtype = np.dtype(
        [
            ("template", "<u8"),
            ("block_mask", "<u8"),
            ("gap_mask", "<u8"),
            ("boundary_near_mask", "u1"),
        ]
    )
    environment_candidate = np.empty(n, dtype=env_dtype)
    environment_candidate["template"] = raw_template
    environment_candidate["block_mask"] = environment["block_mask"]
    environment_candidate["gap_mask"] = environment["gap_mask"]
    boundary_near = (
        (rows.sx.astype(np.int32) < 56).astype(np.uint8)
        | ((rows.sx.astype(np.int32) > arch.width - 1 - 56).astype(np.uint8) << 1)
        | ((rows.sy.astype(np.int32) < 56).astype(np.uint8) << 2)
        | ((rows.sy.astype(np.int32) > arch.height - 1 - 56).astype(np.uint8) << 3)
    )
    environment_candidate["boundary_near_mask"] = boundary_near
    env_candidate_unique, env_candidate_counts = np.unique(environment_candidate, return_counts=True)

    exact_roundtrip = (
        ((exact_key >> np.uint64(32)).astype(np.uint32) == rows.source_code)
        & ((exact_key & np.uint64(0xFFFFFFFF)).astype(np.uint32) == rows.target_code)
    )
    sp_decoded = ((raw_template >> np.uint64(48)) & np.uint64(0xFFFF)).astype(np.uint16)
    tp_decoded = ((raw_template >> np.uint64(32)) & np.uint64(0xFFFF)).astype(np.uint16)
    dx_decoded = (((raw_template >> np.uint64(16)) & np.uint64(0xFFFF)).astype(np.int32) - (arch.width - 1))
    dy_decoded = ((raw_template & np.uint64(0xFFFF)).astype(np.int32) - (arch.height - 1))
    template_roundtrip = (sp_decoded == rows.sp) & (tp_decoded == rows.tp) & (dx_decoded == dx) & (dy_decoded == dy)

    # Conservative safe key includes exact source boundary distances, which fixes
    # absolute source position.  Together with dx/dy it is equivalent to Exact Query
    # on this single architecture.  This deliberately makes no unproved translation claim.
    conservative_safe_unique = int(exact_unique.size)
    conservative_safe_counts = exact_counts

    source_reuse = reuse_stats(source_counts, n)
    target_reuse = reuse_stats(target_counts, n)
    exact_reuse = reuse_stats(exact_counts, n)
    template_reuse = reuse_stats(template_counts, n)
    env_template_reuse = reuse_stats(env_candidate_counts, n)
    safe_template_reuse = reuse_stats(conservative_safe_counts, n)

    # Unique targets per source / unique sources per target use de-duplicated pairs.
    exact_sources = (exact_unique >> np.uint64(32)).astype(np.uint32)
    exact_targets = (exact_unique & np.uint64(0xFFFFFFFF)).astype(np.uint32)
    _, distinct_targets_per_source = np.unique(exact_sources, return_counts=True)
    _, distinct_sources_per_target = np.unique(exact_targets, return_counts=True)

    direct_candidate = np.zeros(n, dtype=np.bool_)
    direct_block_intersection = np.zeros(n, dtype=np.bool_)
    if detailed_block_gap:
        direct_candidate, direct_block_intersection = classify_direct_one_net(rows, arch)

    branch_labels = None
    branch_label_meta: dict[str, Any] | None = None
    if branch_labels_path is not None:
        branch_labels, branch_label_meta = load_branch_labels(branch_labels_path, rows.line_no)
    atlas_precheck = (chebyshev <= 48) & ~block_related
    if atlas_path is not None:
        branch_labels, branch_label_meta = classify_v3_from_atlas(
            atlas_path, rows, arch, chebyshev, block_related
        )

    branch_distribution: dict[str, Any] = {
        "available": branch_labels is not None,
        "precheck_atlas_candidate_count": int(atlas_precheck.sum()),
        "precheck_atlas_candidate_ratio": ratio(int(atlas_precheck.sum()), n),
        "reason_order": ["distance_gt_48", "bbox_intersects_block", "unsupported_or_atlas_unreachable"],
    }
    if branch_labels is not None:
        branch_rows = []
        for code, name in BRANCH_NAMES.items():
            mask = branch_labels == code
            count = int(mask.sum())
            if count == 0:
                continue
            pair_counts_branch = np.bincount(port_pair[mask], minlength=port_pair_size)
            top_pair = int(np.argmax(pair_counts_branch))
            branch_rows.append(
                {
                    "branch": name,
                    "count": count,
                    "ratio": ratio(count, n),
                    "mean_manhattan": float(np.mean(manhattan[mask])),
                    "mean_chebyshev": float(np.mean(chebyshev[mask])),
                    "block_related_ratio": ratio(int(block_related[mask].sum()), count),
                    "gap_related_ratio": ratio(int(gap_related[mask].sum()), count),
                    "top_port_pair": [
                        arch.port_names[top_pair // port_count],
                        arch.port_names[top_pair % port_count],
                    ],
                }
            )
        branch_distribution.update(
            {
                "branches": branch_rows,
                "atlas_count": int((branch_labels == 1).sum()),
                "atlas_ratio": ratio(int((branch_labels == 1).sum()), n),
                "model_fallback_count": int(np.isin(branch_labels, [2, 3, 4]).sum()),
                "model_fallback_ratio": ratio(int(np.isin(branch_labels, [2, 3, 4]).sum()), n),
                "full_search_count": 0,
                "full_search_ratio": 0.0,
                "label_metadata": branch_label_meta,
            }
        )
    else:
        branch_distribution["reason"] = (
            "neither --v3-branch-labels nor --atlas-file supplied; "
            "only exact precheck candidates are reported"
        )

    distance_rows: list[dict[str, Any]] = []
    for low, high, label in DISTANCE_BINS:
        mask = manhattan >= low
        if high is not None:
            mask &= manhattan <= high
        count = int(mask.sum())
        unique_templates = int(np.unique(raw_template[mask]).size) if count else 0
        pair_counts_bin = np.bincount(port_pair[mask], minlength=port_pair_size) if count else np.zeros(port_pair_size, dtype=np.int64)
        top_pair = int(np.argmax(pair_counts_bin)) if count else 0
        row = {
            "distance_metric": "manhattan",
            "bin": label,
            "lower_inclusive": low,
            "upper_inclusive": high,
            "count": count,
            "share": ratio(count, n),
            "unique_port_pairs": int(np.count_nonzero(pair_counts_bin)),
            "top_from_port": arch.port_names[top_pair // port_count] if count else "",
            "top_to_port": arch.port_names[top_pair % port_count] if count else "",
            "block_related_count": int(block_related[mask].sum()),
            "block_related_ratio": ratio(int(block_related[mask].sum()), count),
            "gap_related_count": int(gap_related[mask].sum()),
            "gap_related_ratio": ratio(int(gap_related[mask].sum()), count),
            "unique_geometry_templates": unique_templates,
            "geometry_template_cache_hit_upper_bound": ratio(count - unique_templates, count),
            "atlas_precheck_candidate_count": int(atlas_precheck[mask].sum()),
            "atlas_precheck_candidate_ratio": ratio(int(atlas_precheck[mask].sum()), count),
        }
        if branch_labels is not None:
            row["actual_atlas_count"] = int((branch_labels[mask] == 1).sum())
            row["actual_atlas_ratio"] = ratio(int((branch_labels[mask] == 1).sum()), count)
        distance_rows.append(row)

    dx_counts = Counter(int(value) for value in dx)
    dy_counts = Counter(int(value) for value in dy)
    abs_dx_counts = Counter(int(value) for value in abs_dx)
    abs_dy_counts = Counter(int(value) for value in abs_dy)
    dx_mod = np.mod(dx, 8)
    dy_mod = np.mod(dy, 8)
    abs_mod = (abs_dx % 8) * 8 + (abs_dy % 8)
    mod_pair_counts = np.bincount(abs_mod, minlength=64)

    periodic = periodic_delay_analysis(rows.delay, dx, dy, port_pair, port_pair_size)

    block_involvement = []
    block_category_masks = {
        category: np.zeros(n, dtype=np.bool_)
        for category in ("horizontal_only", "vertical_only", "both_crossable", "neither_crossable")
    }
    for bi, block in enumerate(arch.blocks):
        mask = (environment["block_mask"] & (np.uint64(1) << np.uint64(bi))) != 0
        count = int(mask.sum())
        block_category_masks[block.category] |= mask
        pair_counts_block = np.bincount(port_pair[mask], minlength=port_pair_size)
        top_pair = int(np.argmax(pair_counts_block)) if count else 0

        def relative_positions(x_values: np.ndarray, y_values: np.ndarray) -> list[dict[str, Any]]:
            x_side = np.where(x_values < block.left, 0, np.where(x_values > block.right, 2, 1))
            y_side = np.where(y_values < block.lower, 0, np.where(y_values > block.upper, 2, 1))
            position_code = x_side * 3 + y_side
            position_counts = np.bincount(position_code, minlength=9)
            x_names = ("left", "inside_x", "right")
            y_names = ("below", "inside_y", "above")
            return [
                {
                    "position": f"{x_names[x_code]}_{y_names[y_code]}",
                    "count": int(position_counts[x_code * 3 + y_code]),
                    "ratio": ratio(int(position_counts[x_code * 3 + y_code]), n),
                }
                for x_code in range(3)
                for y_code in range(3)
            ]

        bbox_relations = {
            "intersects": count,
            "block_left_of_bbox": int((block.right < np.minimum(rows.sx, rows.tx)).sum()),
            "block_right_of_bbox": int((block.left > np.maximum(rows.sx, rows.tx)).sum()),
            "block_below_bbox": int((block.upper < np.minimum(rows.sy, rows.ty)).sum()),
            "block_above_bbox": int((block.lower > np.maximum(rows.sy, rows.ty)).sum()),
        }
        block_involvement.append(
            {
                "block_id": block.block_id,
                "category": block.category,
                "count": count,
                "ratio": ratio(count, n),
                "horizontal_crossable": block.horizontal_crossable,
                "vertical_crossable": block.vertical_crossable,
                "mean_manhattan": float(np.mean(manhattan[mask])) if count else math.nan,
                "top_port_pair": [
                    arch.port_names[top_pair // port_count],
                    arch.port_names[top_pair % port_count],
                ] if count else None,
                "geometry_template_cache_hit_upper_bound": (
                    ratio(count - int(np.unique(raw_template[mask]).size), count) if count else math.nan
                ),
                "source_relative_position": relative_positions(rows.sx, rows.sy),
                "target_relative_position": relative_positions(rows.tx, rows.ty),
                "bbox_relative_position_nonexclusive": [
                    {
                        "relation": relation,
                        "count": relation_count,
                        "ratio": ratio(relation_count, n),
                    }
                    for relation, relation_count in bbox_relations.items()
                ],
            }
        )

    block_category_requests = {
        category: {
            "count": int(mask.sum()),
            "ratio_all_requests": ratio(int(mask.sum()), n),
            "ratio_block_related_requests": ratio(int(mask.sum()), int(block_related.sum())),
        }
        for category, mask in block_category_masks.items()
    }

    gap_direction = {
        "none": int((~gap_related).sum()),
        "horizontal_only": int(((environment["horizontal_gap_count"] > 0) & (environment["vertical_gap_count"] == 0)).sum()),
        "vertical_only": int(((environment["vertical_gap_count"] > 0) & (environment["horizontal_gap_count"] == 0)).sum()),
        "both": int(((environment["vertical_gap_count"] > 0) & (environment["horizontal_gap_count"] > 0)).sum()),
    }

    cache_bounds: dict[str, Any] = {
        "exact_query_cache": {
            "status": "safe_on_same_architecture",
            "coverage_after_first_occurrence": exact_reuse["one_minus_unique_over_total"],
            "requests_in_repeated_groups_ratio": exact_reuse["requests_from_keys_seen_at_least_twice_ratio"],
        },
        "top_k_source_tree": {
            "status": "theoretical_query_coverage_not_speedup",
            "coverage": source_reuse["top_k_coverage"],
        },
        "top_k_target_reverse_tree": {
            "status": "theoretical_query_coverage_not_speedup",
            "coverage": target_reuse["top_k_coverage"],
        },
        "raw_geometry_template": {
            "status": "repeatable_geometry_not_proved_safe",
            "coverage_after_first_occurrence": template_reuse["one_minus_unique_over_total"],
        },
        "bbox_environment_candidate_template": {
            "status": "environment_conditioned_candidate_not_proved_safe",
            "coverage_after_first_occurrence": env_template_reuse["one_minus_unique_over_total"],
        },
        "conservative_safe_template": {
            "status": "safe_but_equivalent_to_exact_query_on_this_architecture",
            "coverage_after_first_occurrence": safe_template_reuse["one_minus_unique_over_total"],
        },
        "short_distance_atlas": {
            "status": "actual_when_v3_labels_available_else_precheck_only",
            "chebyshev_le_48_precheck_ratio": ratio(int(atlas_precheck.sum()), n),
            "actual_ratio": branch_distribution.get("atlas_ratio"),
        },
        "macro_candidates": {
            str(size): {
                "definition": f"Chebyshev distance <= {size}",
                "count": int((chebyshev <= size).sum()),
                "ratio": ratio(int((chebyshev <= size).sum()), n),
                "status": "geometric_opportunity_only",
            }
            for size in (8, 16, 32)
        },
        "ordinary_no_block_periodic_candidate": {
            "definition": "endpoint bbox intersects no Block; Gap can be represented separately by endpoint-span signature",
            "count": int((~block_related).sum()),
            "ratio": ratio(int((~block_related).sum()), n),
            "longer_than_atlas_count": int(((~block_related) & (chebyshev > 48)).sum()),
            "longer_than_atlas_ratio": ratio(int(((~block_related) & (chebyshev > 48)).sum()), n),
            "status": "opportunity_only_periodicity_not_proved",
        },
        "block_portal_candidate": {
            "definition": "endpoint bounding box intersects at least one configured Block",
            "count": int(block_related.sum()),
            "ratio": ratio(int(block_related.sum()), n),
            "status": "opportunity_pool_not_validated_coverage",
        },
        "gap_independent_correction_candidate": {
            "definition": "endpoint-span Gap prefix is available; exact path separability is not proved",
            "gap_related_count": int(gap_related.sum()),
            "gap_related_ratio": ratio(int(gap_related.sum()), n),
            "ordinary_no_block_gap_related_count": int((gap_related & ~block_related).sum()),
            "ordinary_no_block_gap_related_ratio": ratio(int((gap_related & ~block_related).sum()), n),
            "status": "candidate_only_except_current_v3_formula",
        },
        "remaining_current_v3_model_or_full_search": {
            "model_fallback_ratio": branch_distribution.get("model_fallback_ratio"),
            "full_search_ratio": branch_distribution.get("full_search_ratio"),
            "status": "actual_current_v3_branch_when_labels_available",
        },
    }

    stats: dict[str, Any] = {
        "schema_version": 1,
        "analysis_scope": {
            "mode": "sample" if rows.source_rows_total != rows.analyzed_rows else "full",
            "source_rows_total": rows.source_rows_total,
            "analyzed_rows": rows.analyzed_rows,
            "successful_rows": rows.success_rows,
            "failed_rows": rows.failure_rows,
            "failure_reasons": dict(rows.failure_reasons),
            "input_columns": rows.input_columns,
            "golden_delay_column": rows.delay_column,
        },
        "architecture": {
            "fingerprint_sha256": arch.fingerprint_sha256,
            "width": arch.width,
            "height": arch.height,
            "instances": len(arch.cell_names),
            "missing_cells": arch.missing_cell_count,
            "block_cells": arch.block_cell_count,
            "block_holes_exactly_match_missing_instances": arch.block_holes_match_instances,
            "ports": port_count,
            "input_ports": arch.input_port_count,
            "output_ports": arch.output_port_count,
            "routing_inputs_net_targets": arch.routing_input_count,
            "arcs": len(arch.arcs),
            "nets": len(arch.nets),
            "gap_lines": len(arch.gaps),
            "blocks": len(arch.blocks),
            "maximum_configured_net_span": arch.max_net_span,
            "v3_declared_maximum_net_span": 8,
            "v3_net_span_mismatch": arch.max_net_span != 8,
            "source_files": {key: str(path) for key, path in arch.source_files.items()},
        },
        "definitions": {
            "exact_query_key": "(architecture SHA-256, canonical From endpoint, canonical To endpoint)",
            "from_key": "canonical absolute endpoint: instance identity + full port name",
            "to_key": "canonical absolute endpoint: instance identity + full port name",
            "geometry_template_key": "(full from_port, full to_port, dx, dy); bus index/direction remain in full port name",
            "geometry_mod8_key": "geometry template plus dx mod 8 and dy mod 8; mathematically redundant",
            "ordinary_region": "endpoint bounding box intersects no Block and endpoint span crosses no Gap Line",
            "bbox_block_related": "closed endpoint axis-aligned bounding box intersects a configured Block rectangle",
            "candidate_direct_net": "zero/one source Arc + exactly one Net + zero/one target Arc connects endpoints under baseline Block counting",
            "gap_related": "endpoint coordinate span crosses at least one configured Gap Line; not necessarily the exact path crossing count",
            "conservative_safe_template": "geometry + exact source boundary distances + Block/Gap signatures; fixes absolute endpoints",
        },
        "base": {
            "total_requests": rows.analyzed_rows,
            "successfully_parsed": rows.success_rows,
            "failed": rows.failure_rows,
            "unique_exact_queries": int(exact_unique.size),
            "exact_cache_hits_after_first": n - int(exact_unique.size),
            "exact_cache_hit_ratio_after_first": ratio(n - int(exact_unique.size), n),
            "requests_in_repeated_exact_groups": int(exact_counts[exact_counts >= 2].sum()),
            "requests_in_repeated_exact_groups_ratio": ratio(int(exact_counts[exact_counts >= 2].sum()), n),
            "exact_frequency_distribution": frequency_distribution(exact_counts),
        },
        "from_reuse": source_reuse,
        "to_reuse": target_reuse,
        "from_to_batching": {
            "mean_distinct_to_per_from": float(np.mean(distinct_targets_per_source)),
            "max_distinct_to_per_from": int(np.max(distinct_targets_per_source)),
            "mean_distinct_from_per_to": float(np.mean(distinct_sources_per_target)),
            "max_distinct_from_per_to": int(np.max(distinct_sources_per_target)),
            "raw_queries_saved_by_one_tree_per_from": n - int(source_unique.size),
            "raw_queries_saved_by_one_tree_per_to": n - int(target_unique.size),
            "deduplicated_queries_saved_by_grouping_from": int(exact_unique.size - source_unique.size),
            "deduplicated_queries_saved_by_grouping_to": int(exact_unique.size - target_unique.size),
            "preferred_by_tree_count_only": "source" if source_unique.size < target_unique.size else ("target" if target_unique.size < source_unique.size else "tie"),
            "caveat": "Tree counts are theoretical reuse only; they do not compare one full Dijkstra with multiple target-directed A* searches.",
        },
        "templates": {
            "port_pair": reuse_stats(port_pair_counts[port_pair_counts > 0], n),
            "geometry": template_reuse,
            "geometry_plus_dxmod8_dymod8": {
                **template_reuse,
                "unique": geom_mod8_unique_count,
                "redundant_with_geometry_key": True,
            },
            "ordinary_no_block_no_gap": {
                "requests": int(ordinary.sum()),
                "request_ratio": ratio(int(ordinary.sum()), n),
                "unique_geometry_templates": int(ordinary_template_unique.size),
                "cache_hit_upper_bound": ratio(int(ordinary.sum()) - int(ordinary_template_unique.size), int(ordinary.sum())),
                "status": "geometry repeatability only; boundary/path invariance not proved",
            },
            "bbox_environment_candidate": env_template_reuse,
            "conservative_safe": {
                **safe_template_reuse,
                "unique": conservative_safe_unique,
                "equivalent_to_exact_query": True,
            },
        },
        "distance": {
            "dx_sign": {
                "negative": int((dx < 0).sum()),
                "zero": int((dx == 0).sum()),
                "positive": int((dx > 0).sum()),
            },
            "dy_sign": {
                "negative": int((dy < 0).sum()),
                "zero": int((dy == 0).sum()),
                "positive": int((dy > 0).sum()),
            },
            "orientation": {
                "zero_displacement": int((orientation_code == 0).sum()),
                "horizontal": int((orientation_code == 1).sum()),
                "vertical": int((orientation_code == 2).sum()),
                "diagonal": int((orientation_code == 3).sum()),
            },
            "orientation_ratio": {
                "zero_displacement": ratio(int((orientation_code == 0).sum()), n),
                "horizontal": ratio(int((orientation_code == 1).sum()), n),
                "vertical": ratio(int((orientation_code == 2).sum()), n),
                "diagonal": ratio(int((orientation_code == 3).sum()), n),
            },
            "manhattan_quantiles": quantiles(manhattan),
            "chebyshev_quantiles": quantiles(chebyshev),
            "euclidean_quantiles": quantiles(euclidean),
            "dx_distribution": [{"value": key, "count": value} for key, value in sorted(dx_counts.items())],
            "dy_distribution": [{"value": key, "count": value} for key, value in sorted(dy_counts.items())],
            "abs_dx_distribution": [{"value": key, "count": value} for key, value in sorted(abs_dx_counts.items())],
            "abs_dy_distribution": [{"value": key, "count": value} for key, value in sorted(abs_dy_counts.items())],
            "manhattan_bins": distance_rows,
        },
        "periodicity": {
            "dx_mod8": [{"remainder": value, "count": int((dx_mod == value).sum()), "ratio": ratio(int((dx_mod == value).sum()), n)} for value in range(8)],
            "dy_mod8": [{"remainder": value, "count": int((dy_mod == value).sum()), "ratio": ratio(int((dy_mod == value).sum()), n)} for value in range(8)],
            "abs_dx_abs_dy_mod8": [
                {
                    "abs_dx_mod8": rx,
                    "abs_dy_mod8": ry,
                    "count": int(mod_pair_counts[rx * 8 + ry]),
                    "ratio": ratio(int(mod_pair_counts[rx * 8 + ry]), n),
                }
                for rx in range(8)
                for ry in range(8)
            ],
            "golden_delay_analysis": periodic,
        },
        "block": {
            "bbox_related_count": int(block_related.sum()),
            "bbox_related_ratio": ratio(int(block_related.sum()), n),
            "bbox_not_related_count": int((~block_related).sum()),
            "bbox_not_related_ratio": ratio(int((~block_related).sum()), n),
            "involved_block_count_distribution": [
                {
                    "blocks": int(value),
                    "count": int((environment["block_count"] == value).sum()),
                    "ratio": ratio(int((environment["block_count"] == value).sum()), n),
                }
                for value in np.unique(environment["block_count"])
            ],
            "related_request_block_categories": block_category_requests,
            "per_block": block_involvement,
            "candidate_direct_one_net_count": int(direct_candidate.sum()),
            "candidate_direct_one_net_ratio": ratio(int(direct_candidate.sum()), n),
            "candidate_direct_one_net_intersects_block_count": int(direct_block_intersection.sum()),
            "candidate_direct_one_net_intersects_block_ratio": ratio(int(direct_block_intersection.sum()), n),
            "actual_solver_path_block_influence": None,
            "actual_solver_path_block_influence_reason": "not observable from public endpoint-only rows or current V3 branch counters",
            "atlas_handled_within_bbox_block_related": int(((branch_labels == 1) & block_related).sum()) if branch_labels is not None else None,
            "model_fallback_within_bbox_block_related": int((np.isin(branch_labels, [2, 3, 4]) & block_related).sum()) if branch_labels is not None else None,
            "full_search_within_bbox_block_related": 0 if branch_labels is not None else None,
        },
        "gap": {
            "endpoint_span_related_count": int(gap_related.sum()),
            "endpoint_span_related_ratio": ratio(int(gap_related.sum()), n),
            "direction_combinations": {
                key: {"count": value, "ratio": ratio(value, n)} for key, value in gap_direction.items()
            },
            "crossed_line_count_distribution": [
                {
                    "gap_lines": int(value),
                    "count": int((environment["gap_count"] == value).sum()),
                    "ratio": ratio(int((environment["gap_count"] == value).sum()), n),
                }
                for value in np.unique(environment["gap_count"])
            ],
            "endpoint_prefix_delay_quantiles_all": quantiles(environment["gap_delay"]),
            "endpoint_prefix_delay_quantiles_gap_related": quantiles(environment["gap_delay"][gap_related]),
            "endpoint_prefix_delay_distribution": [
                {
                    "delay_ps": int(value),
                    "count": int((environment["gap_delay"] == value).sum()),
                    "ratio": ratio(int((environment["gap_delay"] == value).sum()), n),
                }
                for value in np.unique(environment["gap_delay"])
            ],
            "raw_templates_with_multiple_gap_signatures": None,
            "gap_changes_geometry_template_count": None,
            "caveat": "Endpoint-prefix Gap count is exact for the current V3 formula, not proved equal to Gap crossings of every exact shortest path.",
        },
        "existing_v3_branches": branch_distribution,
        "cache_coverage_upper_bounds": cache_bounds,
        "validation": {
            "random_endpoint_roundtrip_samples": len(rows.validation_rows),
            "random_endpoint_roundtrip_all_passed": all(row["roundtrip_ok"] for row in rows.validation_rows),
            "exact_key_reversible_collision_check": bool(np.all(exact_roundtrip)),
            "template_key_reversible_collision_check": bool(np.all(template_roundtrip)),
            "geometry_and_geometry_mod8_unique_counts_equal": int(template_unique.size) == geom_mod8_unique_count,
            "parse_conservation": rows.success_rows + rows.failure_rows == rows.analyzed_rows,
            "orientation_conservation": int(sum((orientation_code == value).sum() for value in range(4))) == n,
            "gap_direction_conservation": sum(gap_direction.values()) == n,
            "distance_bin_conservation": sum(item["count"] for item in distance_rows) == n,
            "block_holes_match_missing_instances": arch.block_holes_match_instances,
        },
    }

    # Quantify how often one raw geometry template occurs under multiple endpoint-span
    # Gap signatures.  This is direct evidence that raw geometry alone is unsafe.
    template_gap_dtype = np.dtype([("template", "<u8"), ("gap", "<u8")])
    template_gap = np.empty(n, dtype=template_gap_dtype)
    template_gap["template"], template_gap["gap"] = raw_template, environment["gap_mask"]
    unique_template_gap = np.unique(template_gap)
    template_of_unique_env = unique_template_gap["template"]
    _, env_counts_by_template = np.unique(template_of_unique_env, return_counts=True)
    raw_templates_multiple_gap = int((env_counts_by_template > 1).sum())
    stats["gap"]["raw_templates_with_multiple_gap_signatures"] = raw_templates_multiple_gap
    stats["gap"]["gap_changes_geometry_template_count"] = raw_templates_multiple_gap
    stats["gap"]["raw_templates_with_multiple_gap_signatures_ratio"] = ratio(
        raw_templates_multiple_gap, int(template_unique.size)
    )

    arrays = {
        "dx": dx,
        "dy": dy,
        "abs_dx": abs_dx,
        "abs_dy": abs_dy,
        "manhattan": manhattan,
        "chebyshev": chebyshev,
        "euclidean": euclidean,
        "orientation_code": orientation_code,
        "exact_key": exact_key,
        "exact_unique": exact_unique,
        "exact_counts": exact_counts,
        "source_unique": source_unique,
        "source_counts": source_counts,
        "target_unique": target_unique,
        "target_counts": target_counts,
        "port_pair": port_pair,
        "port_pair_counts": port_pair_counts,
        "raw_template": raw_template,
        "template_unique": template_unique,
        "template_counts": template_counts,
        "environment": environment,
        "environment_candidate": environment_candidate,
        "env_candidate_unique": env_candidate_unique,
        "env_candidate_counts": env_candidate_counts,
        "ordinary": ordinary,
        "atlas_precheck": atlas_precheck,
        "branch_labels": branch_labels,
        "direct_candidate": direct_candidate,
        "direct_block_intersection": direct_block_intersection,
        "distance_rows": distance_rows,
        "mod_pair_counts": mod_pair_counts,
    }
    return stats, arrays


def top_indices(counts: np.ndarray, top_n: int) -> np.ndarray:
    if counts.size == 0:
        return np.empty(0, dtype=np.int64)
    top_n = min(top_n, counts.size)
    partition = np.argpartition(counts, counts.size - top_n)[-top_n:]
    return partition[np.argsort(counts[partition], kind="stable")[::-1]]


def write_outputs(
    output_dir: Path,
    stats: dict[str, Any],
    arrays: dict[str, Any],
    rows: ParsedRows,
    arch: Architecture,
    top_n: int,
) -> None:
    n = rows.success_rows
    with (output_dir / "query_statistics.json").open("w", encoding="utf-8") as stream:
        json.dump(_safe_json(stats), stream, ensure_ascii=False, indent=2)
        stream.write("\n")

    summary_rows = []

    def add(metric: str, value: Any, unit: str, denominator: str, definition: str) -> None:
        summary_rows.append((metric, value, unit, denominator, definition))

    add("source_rows_total", stats["analysis_scope"]["source_rows_total"], "rows", "input CSV", "Rows found in input after header")
    add("analyzed_rows", stats["analysis_scope"]["analyzed_rows"], "rows", "selected sample/full input", "Rows selected for analysis")
    add("successfully_parsed", n, "requests", "analyzed_rows", "Rows with valid canonical endpoints and delay when present")
    add("parse_failures", rows.failure_rows, "requests", "analyzed_rows", "Rows retained in parse_failures.csv")
    add("unique_exact_queries", stats["base"]["unique_exact_queries"], "keys", "successful requests", stats["definitions"]["exact_query_key"])
    add("exact_cache_hit_ratio_after_first", stats["base"]["exact_cache_hit_ratio_after_first"], "ratio", "successful requests", "(N - unique exact keys) / N")
    add("unique_from", stats["from_reuse"]["unique"], "endpoints", "successful requests", stats["definitions"]["from_key"])
    add("from_one_minus_unique_over_total", stats["from_reuse"]["one_minus_unique_over_total"], "ratio", "successful requests", "1 - unique From / N")
    add("from_repeated_request_ratio", stats["from_reuse"]["requests_from_keys_seen_at_least_twice_ratio"], "ratio", "successful requests", "Requests whose From occurs at least twice / N")
    add("unique_to", stats["to_reuse"]["unique"], "endpoints", "successful requests", stats["definitions"]["to_key"])
    add("to_one_minus_unique_over_total", stats["to_reuse"]["one_minus_unique_over_total"], "ratio", "successful requests", "1 - unique To / N")
    add("to_repeated_request_ratio", stats["to_reuse"]["requests_from_keys_seen_at_least_twice_ratio"], "ratio", "successful requests", "Requests whose To occurs at least twice / N")
    add("unique_geometry_templates", stats["templates"]["geometry"]["unique"], "keys", "successful requests", stats["definitions"]["geometry_template_key"])
    add("geometry_template_repeat_upper_bound", stats["templates"]["geometry"]["one_minus_unique_over_total"], "ratio", "successful requests", "Geometry repeats after first; not safety proof")
    add("conservative_safe_template_hit_ratio", stats["templates"]["conservative_safe"]["one_minus_unique_over_total"], "ratio", "successful requests", "Safe signature fixes absolute origin; equals Exact reuse")
    add("bbox_block_related_ratio", stats["block"]["bbox_related_ratio"], "ratio", "successful requests", stats["definitions"]["bbox_block_related"])
    add("endpoint_span_gap_related_ratio", stats["gap"]["endpoint_span_related_ratio"], "ratio", "successful requests", stats["definitions"]["gap_related"])
    add("atlas_precheck_candidate_ratio", stats["existing_v3_branches"]["precheck_atlas_candidate_ratio"], "ratio", "successful requests", "Chebyshev<=48 and endpoint bbox misses all Blocks")
    add("actual_atlas_ratio", stats["existing_v3_branches"].get("atlas_ratio"), "ratio", "successful requests", "Actual V3 Atlas branch from optional classifier")
    add("current_v3_model_fallback_ratio", stats["existing_v3_branches"].get("model_fallback_ratio"), "ratio", "successful requests", "Actual V3 fallback branch from optional classifier")
    add("max_configured_net_span", arch.max_net_span, "coordinate units", "SRB_Net.json", "Maximum abs(delta x/delta y)")
    write_csv(
        output_dir / "query_statistics_summary.csv",
        ["metric", "value", "unit", "denominator", "definition"],
        summary_rows,
    )

    def top_endpoint_rows(unique: np.ndarray, counts: np.ndarray, label: str) -> Iterator[Sequence[Any]]:
        order = top_indices(counts, top_n)
        cumulative = 0
        for rank, pos in enumerate(order, 1):
            count = int(counts[pos])
            cumulative += count
            code = int(unique[pos])
            x, y, pid, port = endpoint_parts(code, arch)
            yield (
                rank,
                endpoint_text(code, arch),
                x,
                y,
                port,
                arch.port_direction[pid],
                bool(arch.port_is_routing_input[pid]),
                count,
                ratio(count, n),
                ratio(cumulative, n),
                label,
            )

    endpoint_header = [
        "rank",
        "endpoint",
        "x",
        "y",
        "port",
        "port_direction",
        "is_routing_input",
        "count",
        "share",
        "cumulative_share",
        "key_role",
    ]
    write_csv(output_dir / "top_from.csv", endpoint_header, top_endpoint_rows(arrays["source_unique"], arrays["source_counts"], "From"))
    write_csv(output_dir / "top_to.csv", endpoint_header, top_endpoint_rows(arrays["target_unique"], arrays["target_counts"], "To"))

    exact_order = top_indices(arrays["exact_counts"], top_n)
    exact_rows = []
    delay_by_exact: dict[int, list[float]] = defaultdict(list)
    if np.isfinite(rows.delay).any():
        # Only top keys need delay consistency details.
        top_keys_set = {int(arrays["exact_unique"][pos]) for pos in exact_order}
        for key, delay in zip(arrays["exact_key"], rows.delay):
            if int(key) in top_keys_set and math.isfinite(float(delay)):
                delay_by_exact[int(key)].append(float(delay))
    for rank, pos in enumerate(exact_order, 1):
        key = int(arrays["exact_unique"][pos])
        source = key >> 32
        target = key & 0xFFFFFFFF
        values = delay_by_exact.get(key, [])
        exact_rows.append(
            (
                rank,
                endpoint_text(source, arch),
                endpoint_text(target, arch),
                int(arrays["exact_counts"][pos]),
                ratio(int(arrays["exact_counts"][pos]), n),
                min(values) if values else "",
                max(values) if values else "",
                len(set(values)) if values else "",
            )
        )
    write_csv(
        output_dir / "top_exact_queries.csv",
        ["rank", "From", "To", "count", "share", "golden_delay_min", "golden_delay_max", "unique_golden_delays"],
        exact_rows,
    )

    template_rows: list[Sequence[Any]] = []
    template_order = top_indices(arrays["template_counts"], top_n)
    for rank, pos in enumerate(template_order, 1):
        key = int(arrays["template_unique"][pos])
        sp, tp, dx, dy = unpack_template(key, arch)
        template_rows.append(
            (
                "geometry",
                rank,
                arch.port_names[sp],
                arch.port_names[tp],
                dx,
                dy,
                dx % 8,
                dy % 8,
                "",
                "",
                int(arrays["template_counts"][pos]),
                ratio(int(arrays["template_counts"][pos]), n),
                "repeatable_geometry_not_proved_safe",
            )
        )
    env_order = top_indices(arrays["env_candidate_counts"], top_n)
    for rank, pos in enumerate(env_order, 1):
        record = arrays["env_candidate_unique"][pos]
        sp, tp, dx, dy = unpack_template(int(record["template"]), arch)
        template_rows.append(
            (
                "bbox_environment_candidate",
                rank,
                arch.port_names[sp],
                arch.port_names[tp],
                dx,
                dy,
                dx % 8,
                dy % 8,
                int(record["block_mask"]),
                int(record["gap_mask"]),
                int(arrays["env_candidate_counts"][pos]),
                ratio(int(arrays["env_candidate_counts"][pos]), n),
                "conditioned_candidate_not_proved_safe",
            )
        )
    write_csv(
        output_dir / "top_templates.csv",
        [
            "key_type",
            "rank",
            "from_port",
            "to_port",
            "dx",
            "dy",
            "dx_mod8",
            "dy_mod8",
            "block_mask",
            "gap_mask",
            "count",
            "share",
            "reuse_status",
        ],
        template_rows,
    )

    distance_rows = arrays["distance_rows"]
    distance_header = list(distance_rows[0].keys())
    write_csv(
        output_dir / "distance_distribution.csv",
        distance_header,
        ([row.get(column, "") for column in distance_header] for row in distance_rows),
    )

    port_pair_counts = arrays["port_pair_counts"]
    delay_stats = grouped_delay_stats(arrays["port_pair"], rows.delay, len(port_pair_counts))
    manhattan_sum = np.bincount(arrays["port_pair"], weights=arrays["manhattan"], minlength=len(port_pair_counts))
    block_sum = np.bincount(arrays["port_pair"], weights=arrays["environment"]["block_related"], minlength=len(port_pair_counts))
    gap_sum = np.bincount(arrays["port_pair"], weights=arrays["environment"]["gap_related"], minlength=len(port_pair_counts))
    unique_template_source_port = (
        (arrays["template_unique"] >> np.uint64(48)) & np.uint64(0xFFFF)
    ).astype(np.int64)
    unique_template_target_port = (
        (arrays["template_unique"] >> np.uint64(32)) & np.uint64(0xFFFF)
    ).astype(np.int64)
    unique_template_pair = np.bincount(
        unique_template_source_port * len(arch.port_names) + unique_template_target_port,
        minlength=len(port_pair_counts),
    )
    pair_rows = []
    for code in np.flatnonzero(port_pair_counts):
        sp, tp = divmod(int(code), len(arch.port_names))
        count = int(port_pair_counts[code])
        pair_rows.append(
            (
                arch.port_names[sp],
                arch.port_direction[sp],
                bool(arch.port_is_routing_input[sp]),
                arch.port_names[tp],
                arch.port_direction[tp],
                bool(arch.port_is_routing_input[tp]),
                count,
                ratio(count, n),
                int(unique_template_pair[code]),
                ratio(count - int(unique_template_pair[code]), count),
                float(manhattan_sum[code] / count),
                ratio(float(block_sum[code]), count),
                ratio(float(gap_sum[code]), count),
                float(delay_stats["mean"][code]) if delay_stats["count"][code] else "",
                float(delay_stats["std"][code]) if delay_stats["count"][code] else "",
            )
        )
    pair_rows.sort(key=lambda row: (-int(row[6]), str(row[0]), str(row[3])))
    write_csv(
        output_dir / "port_pair_distribution.csv",
        [
            "from_port",
            "from_direction",
            "from_is_routing_input",
            "to_port",
            "to_direction",
            "to_is_routing_input",
            "count",
            "share",
            "unique_geometry_templates",
            "geometry_template_hit_upper_bound",
            "mean_manhattan",
            "bbox_block_related_ratio",
            "endpoint_span_gap_related_ratio",
            "golden_delay_mean",
            "golden_delay_std",
        ],
        pair_rows,
    )

    block_gap_rows: list[Sequence[Any]] = []

    def bg(
        category: str,
        value: str,
        count: int | str,
        denominator: int | str,
        definition: str,
        numeric_value: int | float | str = "",
        unit: str = "requests",
    ) -> None:
        share = ratio(int(count), int(denominator)) if count != "" and denominator != "" else ""
        block_gap_rows.append(
            (category, value, count, share, denominator, numeric_value, unit, definition)
        )

    bg("block_bbox", "related", stats["block"]["bbox_related_count"], n, stats["definitions"]["bbox_block_related"])
    bg("block_bbox", "not_related", stats["block"]["bbox_not_related_count"], n, stats["definitions"]["bbox_block_related"])
    for item in stats["block"]["involved_block_count_distribution"]:
        bg("block_count", str(item["blocks"]), item["count"], n, "Number of Block rectangles intersecting endpoint bbox")
    for item in stats["block"]["per_block"]:
        bg("block_id", str(item["block_id"]), item["count"], n, f"Block category={item['category']}")
        for position in item["source_relative_position"]:
            bg(
                f"block_{item['block_id']}_source_position",
                position["position"],
                position["count"],
                n,
                "Source position relative to Block x/y bounds",
            )
        for position in item["target_relative_position"]:
            bg(
                f"block_{item['block_id']}_target_position",
                position["position"],
                position["count"],
                n,
                "Target position relative to Block x/y bounds",
            )
        for relation in item["bbox_relative_position_nonexclusive"]:
            bg(
                f"block_{item['block_id']}_bbox_relation",
                relation["relation"],
                relation["count"],
                n,
                "Non-intersecting bbox relations can overlap at corners",
            )
    for category, item in stats["block"]["related_request_block_categories"].items():
        bg(
            "block_crossability_category",
            category,
            item["count"],
            n,
            "Request bbox intersects at least one Block of this crossability category",
        )
    bg("candidate_direct_one_net", "exists", stats["block"]["candidate_direct_one_net_count"], n, stats["definitions"]["candidate_direct_net"])
    bg("candidate_direct_one_net", "intersects_block", stats["block"]["candidate_direct_one_net_intersects_block_count"], n, stats["definitions"]["candidate_direct_net"])
    bg("gap_endpoint_span", "related", stats["gap"]["endpoint_span_related_count"], n, stats["definitions"]["gap_related"])
    for key, item in stats["gap"]["direction_combinations"].items():
        bg("gap_direction", key, item["count"], n, "Gap Line directions crossed by endpoint span")
    for item in stats["gap"]["crossed_line_count_distribution"]:
        bg("gap_line_count", str(item["gap_lines"]), item["count"], n, "Configured Gap Lines crossed by endpoint span")
    for item in stats["gap"]["endpoint_prefix_delay_distribution"]:
        bg(
            "gap_endpoint_prefix_delay",
            str(item["delay_ps"]),
            item["count"],
            n,
            "Endpoint-span Gap prefix delay used by current V3",
            numeric_value=item["delay_ps"],
            unit="ps",
        )
    for quantile_name, quantile_value in stats["gap"]["endpoint_prefix_delay_quantiles_gap_related"].items():
        bg(
            "gap_endpoint_prefix_delay_quantile",
            quantile_name,
            "",
            stats["gap"]["endpoint_span_related_count"],
            "Quantile among endpoint-span Gap-related requests",
            numeric_value=quantile_value,
            unit="ps",
        )
    if stats["existing_v3_branches"].get("branches"):
        for item in stats["existing_v3_branches"]["branches"]:
            bg("v3_branch", item["branch"], item["count"], n, "Actual unchanged V3 classifier branch")
    write_csv(
        output_dir / "block_gap_distribution.csv",
        ["category", "value", "count", "share", "denominator", "numeric_value", "unit", "definition"],
        block_gap_rows,
    )

    write_csv(
        output_dir / "parse_validation_samples.csv",
        [
            "line_number",
            "raw_from",
            "raw_to",
            "canonical_from",
            "canonical_to",
            "from_x",
            "from_y",
            "from_port",
            "to_x",
            "to_y",
            "to_port",
            "roundtrip_ok",
        ],
        ([row[column] for column in [
            "line_number", "raw_from", "raw_to", "canonical_from", "canonical_to",
            "from_x", "from_y", "from_port", "to_x", "to_y", "to_port", "roundtrip_ok"
        ]] for row in rows.validation_rows),
    )


def generate_charts(output_dir: Path, stats: dict[str, Any], arrays: dict[str, Any], rows: ParsedRows, arch: Architecture) -> list[str]:
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception as exc:
        return [f"charts unavailable: {exc}"]

    chart_dir = output_dir / "charts"
    chart_dir.mkdir(parents=True, exist_ok=True)
    generated: list[str] = []

    def save(name: str) -> None:
        plt.tight_layout()
        plt.savefig(chart_dir / name, dpi=160, bbox_inches="tight")
        plt.close()
        generated.append(f"charts/{name}")

    plt.figure(figsize=(8, 5))
    for label, counts in (("From", arrays["source_counts"]), ("To", arrays["target_counts"])):
        ranked = np.sort(counts)[::-1]
        plt.loglog(np.arange(1, ranked.size + 1), ranked, label=label, linewidth=1.2)
    plt.xlabel("rank (log)")
    plt.ylabel("frequency (log)")
    plt.title("From/To frequency long tail")
    plt.grid(alpha=0.25)
    plt.legend()
    save("from_to_frequency_long_tail.png")

    plt.figure(figsize=(8, 5))
    labels = [row[2] for row in DISTANCE_BINS]
    counts = [row["count"] for row in arrays["distance_rows"]]
    plt.bar(labels, counts, color="#4C78A8")
    plt.xlabel("Manhattan distance bin")
    plt.ylabel("requests")
    plt.title("Manhattan distance distribution")
    plt.xticks(rotation=25)
    save("distance_distribution.png")

    top_ports = 30
    source_port_counts = np.bincount(rows.sp.astype(np.int64), minlength=len(arch.port_names))
    target_port_counts = np.bincount(rows.tp.astype(np.int64), minlength=len(arch.port_names))
    source_ids = np.argsort(source_port_counts)[::-1][:top_ports]
    target_ids = np.argsort(target_port_counts)[::-1][:top_ports]
    matrix = np.zeros((top_ports, top_ports), dtype=np.int64)
    pair_matrix = arrays["port_pair_counts"].reshape(len(arch.port_names), len(arch.port_names))
    matrix = pair_matrix[np.ix_(source_ids, target_ids)]
    plt.figure(figsize=(11, 9))
    plt.imshow(np.log1p(matrix), aspect="auto", cmap="viridis")
    plt.colorbar(label="log(1 + requests)")
    plt.xticks(range(top_ports), [arch.port_names[i] for i in target_ids], rotation=90, fontsize=6)
    plt.yticks(range(top_ports), [arch.port_names[i] for i in source_ids], fontsize=6)
    plt.xlabel("To port (top 30)")
    plt.ylabel("From port (top 30)")
    plt.title("Port-pair heatmap")
    save("port_pair_heatmap.png")

    plt.figure(figsize=(9, 6))
    hist, xedges, yedges = np.histogram2d(arrays["dx"], arrays["dy"], bins=(60, 80))
    plt.imshow(
        np.log1p(hist.T),
        origin="lower",
        aspect="auto",
        extent=[xedges[0], xedges[-1], yedges[0], yedges[-1]],
        cmap="magma",
    )
    plt.colorbar(label="log(1 + requests)")
    plt.xlabel("dx")
    plt.ylabel("dy")
    plt.title("dx/dy request density")
    save("dx_dy_distribution.png")

    plt.figure(figsize=(7, 6))
    mod_matrix = arrays["mod_pair_counts"].reshape(8, 8)
    plt.imshow(mod_matrix, origin="lower", cmap="Blues")
    plt.colorbar(label="requests")
    plt.xticks(range(8))
    plt.yticks(range(8))
    plt.xlabel("|dy| mod 8")
    plt.ylabel("|dx| mod 8")
    plt.title("Absolute displacement remainder distribution")
    save("mod8_distribution.png")

    plt.figure(figsize=(8, 5))
    reuse = [100.0 * row["geometry_template_cache_hit_upper_bound"] for row in arrays["distance_rows"]]
    plt.plot(labels, reuse, marker="o", color="#F58518")
    upper = max(reuse) * 1.25 if max(reuse, default=0.0) > 0 else 1.0
    plt.ylim(0, upper)
    plt.xlabel("Manhattan distance bin")
    plt.ylabel("repeat upper bound (%)")
    plt.title("Geometry-template repeat upper bound by distance")
    plt.xticks(rotation=25)
    plt.grid(alpha=0.25)
    for index, value in enumerate(reuse):
        plt.annotate(f"{value:.4f}%", (index, value), xytext=(0, 7), textcoords="offset points", ha="center", fontsize=7)
    save("template_reuse_by_distance.png")

    plt.figure(figsize=(7, 5))
    names = ["Block bbox", "Gap span", "Ordinary"]
    values = [
        stats["block"]["bbox_related_ratio"],
        stats["gap"]["endpoint_span_related_ratio"],
        stats["templates"]["ordinary_no_block_no_gap"]["request_ratio"],
    ]
    plt.bar(names, values, color=["#E45756", "#72B7B2", "#54A24B"])
    plt.ylim(0, 1)
    plt.ylabel("request ratio")
    plt.title("Block/Gap geometric classifications (overlap allowed)")
    save("block_gap_request_ratios.png")

    branches = stats["existing_v3_branches"].get("branches", [])
    if branches:
        plt.figure(figsize=(8, 5))
        plt.bar([item["branch"] for item in branches], [item["ratio"] for item in branches], color="#B279A2")
        plt.ylim(0, 1)
        plt.ylabel("request ratio")
        plt.title("Actual unchanged V3 branch coverage")
        plt.xticks(rotation=25)
        save("v3_branch_coverage.png")
    return generated


def pct(value: Any) -> str:
    return "N/A" if value is None or (isinstance(value, float) and not math.isfinite(value)) else f"{100 * float(value):.4f}%"


def write_report(output_dir: Path, stats: dict[str, Any], chart_paths: list[str], command: str) -> None:
    scope = stats["analysis_scope"]
    base = stats["base"]
    from_stats = stats["from_reuse"]
    to_stats = stats["to_reuse"]
    templates = stats["templates"]
    block = stats["block"]
    gap = stats["gap"]
    branch = stats["existing_v3_branches"]
    periodic = stats["periodicity"]["golden_delay_analysis"]
    mode_note = (
        f"Full analysis of all {scope['analyzed_rows']:,} input rows."
        if scope["mode"] == "full"
        else f"Reservoir-sample analysis of {scope['analyzed_rows']:,} rows from {scope['source_rows_total']:,} input rows."
    )
    lines = [
        "# Public SRB Query Structure and Reuse Analysis",
        "",
        f"> Scope: {mode_note}",
        f"> Architecture fingerprint: `{stats['architecture']['fingerprint_sha256']}`",
        "",
        "## 1. Data source and parsing rules",
        "",
        f"- Input columns: `{', '.join(scope['input_columns'])}`.",
        f"- Parsed: {scope['successful_rows']:,}; failed: {scope['failed_rows']:,}. Failed rows and reasons are retained in `parse_failures.csv`.",
        "- Endpoint parsing matches the baseline solver's semantic lookup: split `SRB_instance/PORT`, then require the instance name in `SRB_Inst.json` and the full port name in `SRB_Port.json`.",
        "- Full port names retain direction letters, span family, bus index, Logic port identity, and Routing-Input status (Net target or not).",
        "- Exact Query key is `(architecture fingerprint, canonical From, canonical To)`. Because this run uses one immutable architecture, counts are computed from the two endpoint identities.",
        "",
        "Architecture summary:",
        "",
        "| Item | Value |",
        "|---|---:|",
        f"| Grid extent | {stats['architecture']['width']} x {stats['architecture']['height']} |",
        f"| Existing SRB instances | {stats['architecture']['instances']:,} |",
        f"| Missing cells / Block cells | {stats['architecture']['missing_cells']:,} / {stats['architecture']['block_cells']:,} |",
        f"| Ports (Input / Output / Routing Input) | {stats['architecture']['ports']} ({stats['architecture']['input_ports']} / {stats['architecture']['output_ports']} / {stats['architecture']['routing_inputs_net_targets']}) |",
        f"| Arcs / Nets / Gap Lines / Blocks | {stats['architecture']['arcs']:,} / {stats['architecture']['nets']} / {stats['architecture']['gap_lines']} / {stats['architecture']['blocks']} |",
        f"| Maximum configured Net span | {stats['architecture']['maximum_configured_net_span']} |",
        "",
        "## 2. Core results",
        "",
        "| Metric | Value | Meaning |",
        "|---|---:|---|",
        f"| Successful requests | {scope['successful_rows']:,} | denominator for request ratios |",
        f"| Unique Exact Queries | {base['unique_exact_queries']:,} | absolute endpoint pairs |",
        f"| Exact cache hits after first | {base['exact_cache_hits_after_first']:,} ({pct(base['exact_cache_hit_ratio_after_first'])}) | safe on the same architecture |",
        f"| Unique From | {from_stats['unique']:,} | full absolute From endpoints |",
        f"| `1 - unique_from / total` | {pct(from_stats['one_minus_unique_over_total'])} | search-count reduction ceiling |",
        f"| Requests whose From occurs >=2 | {pct(from_stats['requests_from_keys_seen_at_least_twice_ratio'])} | request coverage, not the prior metric |",
        f"| Unique To | {to_stats['unique']:,} | full absolute To endpoints |",
        f"| `1 - unique_to / total` | {pct(to_stats['one_minus_unique_over_total'])} | reverse search-count reduction ceiling |",
        f"| Requests whose To occurs >=2 | {pct(to_stats['requests_from_keys_seen_at_least_twice_ratio'])} | request coverage |",
        f"| Unique geometry templates | {templates['geometry']['unique']:,} | `(full ports, dx, dy)` |",
        f"| Geometry-template repeat upper bound | {pct(templates['geometry']['one_minus_unique_over_total'])} | not a safe delay-cache claim |",
        f"| Conservative safe template hits | {pct(templates['conservative_safe']['one_minus_unique_over_total'])} | equals Exact Cache because absolute origin is fixed |",
        f"| BBox Block-related | {block['bbox_related_count']:,} ({pct(block['bbox_related_ratio'])}) | geometric classification |",
        f"| Endpoint-span Gap-related | {gap['endpoint_span_related_count']:,} ({pct(gap['endpoint_span_related_ratio'])}) | current V3 prefix geometry, not exact-path count |",
        f"| Actual V3 Atlas | {branch.get('atlas_count', 'N/A')} ({pct(branch.get('atlas_ratio'))}) | exact unchanged branch classifier when available |",
        f"| Actual V3 model fallback | {branch.get('model_fallback_count', 'N/A')} ({pct(branch.get('model_fallback_ratio'))}) | V3 has no full-search fallback |",
        "",
        "## 3. From/To tree and batching potential",
        "",
        f"A full tree per distinct From would replace at most {stats['from_to_batching']['raw_queries_saved_by_one_tree_per_from']:,} independent per-query searches; a reverse tree per distinct To would replace at most {stats['from_to_batching']['raw_queries_saved_by_one_tree_per_to']:,}. After Exact de-duplication, the corresponding grouping reductions are {stats['from_to_batching']['deduplicated_queries_saved_by_grouping_from']:,} and {stats['from_to_batching']['deduplicated_queries_saved_by_grouping_to']:,}.",
        "",
        "| K | Source-tree request coverage | Target reverse-tree request coverage |",
        "|---:|---:|---:|",
    ]
    for k in (10, 50, 100, 500, 1000):
        lines.append(
            f"| {k} | {pct(from_stats['top_k_coverage'][str(k)]['ratio'])} | {pct(to_stats['top_k_coverage'][str(k)]['ratio'])} |"
        )
    lines.extend(
        [
            "",
            "These are theoretical request coverages. They do not prove that one full Dijkstra tree is faster than several target-directed A* searches, nor do they include tree memory cost.",
            "",
            "## 4. Geometry templates, distance, and periodic evidence",
            "",
            f"Adding `(dx mod 8, dy mod 8)` to a key already containing exact `dx,dy` changes the unique count by **0**; the fields are deterministic and therefore redundant. The non-redundant periodic analysis uses `(|dx| mod 8, |dy| mod 8)` groups and Golden residuals.",
            "",
            "Distance quantiles:",
            "",
            "| Metric | P50 | P75 | P90 | P95 | P99 | Max |",
            "|---|---:|---:|---:|---:|---:|---:|",
        ]
    )
    for name, key in (("Manhattan", "manhattan_quantiles"), ("Chebyshev", "chebyshev_quantiles"), ("Euclidean", "euclidean_quantiles")):
        q = stats["distance"][key]
        lines.append(f"| {name} | {q['p50']:.2f} | {q['p75']:.2f} | {q['p90']:.2f} | {q['p95']:.2f} | {q['p99']:.2f} | {q['max']:.2f} |")
    if periodic.get("available"):
        controlled = periodic["port_pair_controlled_directional_linear"]
        piecewise = periodic["port_pair_controlled_piecewise_linear"]
        remainder = periodic["abs_mod8_residual_adjustment_in_sample"]
        lines.extend(
            [
                "",
                f"Golden descriptive fit: the port-pair-controlled directional linear baseline has RMSE {controlled['rmse']:.3f} ps and R2 {controlled['r2']:.6f}. Adding Manhattan hinge terms at 8/16/32/64/128/256 reduces in-sample RMSE by {pct(piecewise['rmse_reduction_vs_controlled_linear'])}. Subtracting the 64 absolute mod-8 group means reduces in-sample RMSE by {pct(remainder['rmse_reduction_vs_controlled_linear'])}.",
                "",
                "This is evidence of association only. The same public rows define and evaluate the residual groups; it is not hidden-set evidence and does not prove an 8x8 graph isomorphism or a safe Macro cache.",
            ]
        )
    else:
        lines.extend(["", f"Golden delay analysis unavailable: {periodic.get('reason', 'unknown reason')}."])

    lines.extend(
        [
            "",
            "## 5. Block and Gap distinctions",
            "",
            f"- Endpoint bbox intersects a Block: {block['bbox_related_count']:,} ({pct(block['bbox_related_ratio'])}). This is exactly the current V3 `safe_regular_region` rejection condition.",
            f"- A valid candidate path with exactly one Net exists: {block['candidate_direct_one_net_count']:,}; among them {block['candidate_direct_one_net_intersects_block_count']:,} touch a Block under baseline Net counting.",
            "- Actual shortest-path Block influence is not available from the endpoint-only million-row Golden file. It is intentionally left `null`, not equated with bbox intersection or one-Net intersection.",
            f"- Endpoint span crosses a Gap Line for {gap['endpoint_span_related_count']:,} requests. {gap['raw_templates_with_multiple_gap_signatures']:,} raw geometry templates occur with multiple Gap signatures, directly showing that raw geometry alone can mix environments.",
            "- The endpoint prefix sum is the exact term used by current V3, but the public endpoint rows do not prove that every exact shortest path crosses each Gap exactly according to endpoint displacement.",
            "",
            "## 6. Existing V3 branch coverage",
            "",
        ]
    )
    if branch.get("available"):
        lines.extend(["| Branch | Requests | Ratio | Mean Manhattan | Block ratio | Gap ratio |", "|---|---:|---:|---:|---:|---:|"])
        for item in branch["branches"]:
            lines.append(
                f"| {item['branch']} | {item['count']:,} | {pct(item['ratio'])} | {item['mean_manhattan']:.2f} | {pct(item['block_related_ratio'])} | {pct(item['gap_related_ratio'])} |"
            )
        lines.extend(
            [
                "",
            "The current V3 uses Atlas or the learned O(1) fallback. It does not invoke the baseline full A*/Dijkstra solver; therefore full-search count is zero in this executable, not a claim that exact search is unnecessary. Branch labels are reproduced by source-equivalent classification without changing solver behavior; per-branch production latency was not instrumented in this phase.",
            ]
        )
    else:
        lines.append(f"Actual branch labels unavailable: {branch.get('reason')}")

    lines.extend(
        [
            "",
            "## 7. Cache/structure coverage interpretation",
            "",
            "- **Semantically safe cache key, but no public-set hits:** Exact Query reuse is safe on the same architecture fingerprint; this public dataset contains zero repeated Exact Queries.",
            "- **Repeatable but not yet safely reusable:** raw `(portA,portB,dx,dy)` geometry and bbox-conditioned templates.",
            "- **Structured opportunity pools:** Chebyshev <= 8/16/32 Macro candidates, no-bbox-Block long requests for a periodic model, and bbox-Block requests for a Portal prototype.",
            "- **Still requires validation or fallback:** every candidate for which translation invariance, boundary behavior, Gap separability, Block topology, and port compatibility have not been proved. Current V3 fallback coverage is reported separately when classifier labels are present.",
            "",
            "The conservative environment signature includes exact source boundary distances; consequently it fixes the absolute source coordinate and collapses to Exact Query reuse. Any higher template reuse reported by weaker signatures is an opportunity upper bound, not a safe cache hit rate.",
            "",
            "## 8. Correctness checks",
            "",
            f"- Random endpoint round-trip checks: {stats['validation']['random_endpoint_roundtrip_samples']} rows; all passed: `{stats['validation']['random_endpoint_roundtrip_all_passed']}`.",
            f"- Exact packed-key reversible check: `{stats['validation']['exact_key_reversible_collision_check']}`.",
            f"- Template packed-key reversible check: `{stats['validation']['template_key_reversible_collision_check']}`.",
            f"- Parse/orientation/Gap/distance conservation checks: `{stats['validation']['parse_conservation']}`, `{stats['validation']['orientation_conservation']}`, `{stats['validation']['gap_direction_conservation']}`, `{stats['validation']['distance_bin_conservation']}`.",
            f"- Configured Block cells exactly equal missing instance coordinates: `{stats['validation']['block_holes_match_missing_instances']}`.",
            f"- V3 metadata discrepancy: configured maximum Net span is {stats['architecture']['maximum_configured_net_span']}, while `srb_hybrid.hpp` declares 8. Because lookup bounds are checked, this currently indicates avoidable fallback risk rather than demonstrated wrong delay.",
            "",
            "## 9. What these data cannot prove",
            "",
            "1. Equal geometry templates have equal exact delay across translations.",
            "2. A shortest path stays inside the endpoint bounding box or crosses Gap Lines only according to endpoint displacement.",
            "3. An 8x8 period is exact merely because some Net families have bounded spans or residual means vary by mod 8.",
            "4. One complete source/target tree is faster or smaller than the current A* workload.",
            "5. A Block Portal graph preserves exact delay without explicit portal/state construction and Golden-path tests.",
            "6. Public-data residual improvements generalize to hidden evaluation data.",
            "",
            "## 10. Next 5 experiments",
            "",
            "1. Construct a controlled replay workload by duplicating public Exact Queries; verify deterministic equality, architecture-version invalidation, and measured cache overhead without treating the synthetic hit rate as public-data evidence.",
            "2. For Top-10/50/100 From and To, compare full-tree build/query time and memory against the same queries under current A*.",
            "3. Select repeated ordinary templates across different origins; run exact solver pairs stratified by boundary clearance, Gap signature, and Block relation to measure true invariance violations.",
            "4. Build an 8x8 vs 16x16 vs 32x32 local Macro prototype on a small port subset, evaluate out-of-origin exact agreement, and inspect periodic residual stability on held-out regions.",
            "5. Instrument a separate exact-solver research binary to label actual path Block/Gap crossings; only then size Portal and Gap-prefix opportunities.",
            "",
            "## 11. Reproduction",
            "",
            "```powershell",
            command,
            "```",
            "",
            f"- Analysis elapsed time: {stats['runtime']['elapsed_seconds']:.3f} seconds.",
            f"- Peak process RSS: {stats['runtime']['peak_process_rss_mib']:.3f} MiB." if stats['runtime']['peak_process_rss_mib'] is not None else "- Peak process RSS: unavailable.",
            "- These values cover parsing, statistics, CSV/JSON generation, and chart generation; they do not include an external exact solver run.",
        ]
    )
    if chart_paths:
        lines.extend(["", "## 12. Charts", ""])
        for chart in chart_paths:
            if chart.startswith("charts unavailable"):
                lines.append(f"- {chart}")
            else:
                title = Path(chart).stem.replace("_", " ").title()
                lines.extend([f"### {title}", "", f"![{title}]({chart.replace(os.sep, '/')})", ""])

    (output_dir / "query_analysis_report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_command(args: argparse.Namespace) -> str:
    parts = [
        f"& '{Path(sys.executable)}'",
        f"'{Path('tools') / Path(__file__).name}'",
        f"--input '{args.input}'",
        f"--arch-dir '{args.arch_dir}'",
        f"--output-dir '{args.output_dir}'",
        f"--seed {args.seed}",
    ]
    if args.sample_size is not None:
        parts.append(f"--sample-size {args.sample_size}")
    if args.detailed_block_gap:
        parts.append("--detailed-block-gap")
    else:
        parts.append("--no-detailed-block-gap")
    if args.v3_branch_labels is not None:
        parts.append(f"--v3-branch-labels '{args.v3_branch_labels}'")
    if args.atlas_file is not None:
        parts.append(f"--atlas-file '{args.atlas_file}'")
    if args.no_charts:
        parts.append("--no-charts")
    return " `\n    ".join(parts)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    started = time.perf_counter()
    monitor = PeakMemoryMonitor()
    monitor.start()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    arch = load_architecture(args.arch_dir)
    rows = read_queries(args.input.resolve(), arch, args.output_dir.resolve(), args.sample_size, args.seed)
    stats, arrays = analyze(
        rows, arch, args.detailed_block_gap, args.v3_branch_labels, args.atlas_file
    )
    write_outputs(args.output_dir.resolve(), stats, arrays, rows, arch, args.top_n)
    chart_paths = [] if args.no_charts else generate_charts(args.output_dir.resolve(), stats, arrays, rows, arch)
    elapsed = time.perf_counter() - started
    peak_bytes = monitor.stop()
    stats["runtime"] = {
        "elapsed_seconds": elapsed,
        "peak_process_rss_bytes": peak_bytes or None,
        "peak_process_rss_mib": peak_bytes / (1024 * 1024) if peak_bytes else None,
        "python": sys.version,
        "platform": platform.platform(),
        "numpy": np.__version__,
        "command": build_command(args),
    }
    stats["outputs"] = {
        "required": [
            "query_statistics.json",
            "query_statistics_summary.csv",
            "top_from.csv",
            "top_to.csv",
            "top_exact_queries.csv",
            "top_templates.csv",
            "distance_distribution.csv",
            "port_pair_distribution.csv",
            "block_gap_distribution.csv",
            "query_analysis_report.md",
        ],
        "additional": ["parse_failures.csv", "parse_validation_samples.csv", *chart_paths],
    }
    with (args.output_dir / "query_statistics_summary.csv").open(
        "a", encoding="utf-8-sig", newline=""
    ) as stream:
        writer = csv.writer(stream)
        writer.writerow(
            [
                "analysis_elapsed_seconds",
                elapsed,
                "seconds",
                "one analysis run",
                "Parsing, statistics, output, and charts",
            ]
        )
        writer.writerow(
            [
                "peak_process_rss_mib",
                stats["runtime"]["peak_process_rss_mib"],
                "MiB",
                "analysis process",
                "Best-effort sampled peak RSS/Windows peak working set",
            ]
        )
    with (args.output_dir / "query_statistics.json").open("w", encoding="utf-8") as stream:
        json.dump(_safe_json(stats), stream, ensure_ascii=False, indent=2)
        stream.write("\n")
    write_report(args.output_dir.resolve(), stats, chart_paths, build_command(args))
    print(json.dumps(_safe_json({
        "mode": stats["analysis_scope"]["mode"],
        "source_rows": rows.source_rows_total,
        "analyzed_rows": rows.analyzed_rows,
        "successful": rows.success_rows,
        "failed": rows.failure_rows,
        "elapsed_seconds": elapsed,
        "peak_rss_mib": stats["runtime"]["peak_process_rss_mib"],
        "output_dir": str(args.output_dir.resolve()),
    }), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise
