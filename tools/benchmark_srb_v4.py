#!/usr/bin/env python3
"""Benchmark V3/V4 modes with identical executable, inputs, and monitoring."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import statistics
import subprocess
import time
from pathlib import Path
from typing import Any

import psutil


METRIC_PATTERNS = {
    "processed": r"processed=(\d+)",
    "legacy_atlas_queries": r"legacy_atlas_queries=(\d+)",
    "v3_fallback_queries": r"v3_fallback_queries=(\d+)",
    "v4_residual_queries": r"v4_residual_queries=(\d+)",
    "atlas_load_seconds": r"atlas_load=([0-9.]+) s",
    "query_and_io_seconds": r"query_and_io=([0-9.]+) s",
    "buffered_write_seconds": r"buffered_write=([0-9.]+) s",
    "reported_total_seconds": r"total=([0-9.]+) s",
    "throughput_qps": r"throughput=([0-9.]+) q/s",
    "atlas_memory_mib": r"atlas_memory=([0-9.]+) MiB",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Fair SRB V3/V4 process benchmark")
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--atlas", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--sizes", default="100000,1000000")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--modes", default="v3,v4")
    parser.add_argument("--rss-poll-ms", type=float, default=10.0)
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def count_rows(path: Path) -> int:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        reader = csv.reader(stream)
        try:
            next(reader)
        except StopIteration:
            return 0
        return sum(1 for row in reader if row)


def create_sized_input(source: Path, target: Path, rows: int) -> None:
    """Create an exact prefix, cycling the source only when rows exceeds it."""
    target.parent.mkdir(parents=True, exist_ok=True)
    with target.open("w", encoding="utf-8", newline="") as output_stream:
        writer = csv.writer(output_stream, lineterminator="\n")
        written = 0
        header: list[str] | None = None
        while written < rows:
            before_cycle = written
            with source.open("r", encoding="utf-8-sig", newline="") as input_stream:
                reader = csv.reader(input_stream)
                try:
                    current_header = next(reader)
                except StopIteration as exc:
                    raise ValueError("input CSV is empty") from exc
                if header is None:
                    header = current_header
                    writer.writerow(header)
                elif current_header != header:
                    raise ValueError("source CSV header changed while creating benchmark input")
                for row in reader:
                    if not row:
                        continue
                    writer.writerow(row)
                    written += 1
                    if written == rows:
                        break
            if written == before_cycle:
                raise ValueError("input CSV contains no data rows")
    if written != rows:
        raise ValueError(f"requested prefix {rows:,}, input only supplied {written:,} rows")


def parse_stderr(stderr: str) -> dict[str, int | float]:
    result: dict[str, int | float] = {}
    integer_metrics = {
        "processed",
        "legacy_atlas_queries",
        "v3_fallback_queries",
        "v4_residual_queries",
    }
    for name, pattern in METRIC_PATTERNS.items():
        match = re.search(pattern, stderr)
        if not match:
            raise ValueError(f"benchmark stderr missing {name}: {stderr}")
        value: int | float = int(match.group(1)) if name in integer_metrics else float(match.group(1))
        result[name] = value
    return result


def run_once(
    executable: Path,
    input_path: Path,
    output_path: Path,
    atlas_path: Path,
    mode: str,
    poll_seconds: float,
) -> dict[str, Any]:
    solver_mode = "v4" if mode in {"v4-reference", "v4-optimized", "v4-family"} else mode
    command = [
        str(executable),
        "-in",
        str(input_path),
        "-out",
        str(output_path),
        "-atlas",
        str(atlas_path),
        "--mode",
        solver_mode,
    ]
    if mode == "v4-reference":
        command.extend(["--residual-kernel", "reference"])
    elif mode == "v4-optimized":
        command.extend(["--residual-kernel", "optimized"])
    elif mode == "v4-family":
        command.extend(["--residual-kernel", "family"])
    started = time.perf_counter()
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    observed_peak = 0
    ps_process = psutil.Process(process.pid)
    while process.poll() is None:
        try:
            info = ps_process.memory_info()
            observed_peak = max(
                observed_peak,
                int(getattr(info, "peak_wset", 0)),
                int(getattr(info, "rss", 0)),
            )
        except (psutil.NoSuchProcess, psutil.AccessDenied):
            pass
        time.sleep(poll_seconds)
    stdout, stderr = process.communicate()
    wall = time.perf_counter() - started
    if process.returncode != 0:
        raise RuntimeError(
            f"benchmark command failed ({process.returncode}): {command}\n{stdout}\n{stderr}"
        )
    result: dict[str, Any] = parse_stderr(stderr)
    result.update(
        {
            "wall_seconds": wall,
            "peak_rss_mib": observed_peak / (1024.0 * 1024.0),
            "output_bytes": output_path.stat().st_size,
            "output_sha256": sha256_file(output_path),
            "stderr": stderr.strip(),
        }
    )
    return result


def aggregate(runs: list[dict[str, Any]], formal_rows: int = 100_000_000) -> dict[str, Any]:
    numeric = [
        "wall_seconds",
        "peak_rss_mib",
        "atlas_load_seconds",
        "query_and_io_seconds",
        "buffered_write_seconds",
        "reported_total_seconds",
        "throughput_qps",
    ]
    summary: dict[str, Any] = {"runs": len(runs)}
    for name in numeric:
        values = [float(run[name]) for run in runs]
        summary[name] = {
            "mean": statistics.mean(values),
            "median": statistics.median(values),
            "min": min(values),
            "max": max(values),
        }
    hashes = [str(run["output_sha256"]) for run in runs]
    summary["consistent"] = len(set(hashes)) == 1
    summary["output_sha256"] = hashes[0] if summary["consistent"] else hashes
    processed = int(runs[0]["processed"])
    mean_query = float(summary["query_and_io_seconds"]["mean"])
    mean_load = float(summary["atlas_load_seconds"]["mean"])
    summary["projected_100m_seconds_linear"] = mean_load + mean_query * formal_rows / processed
    summary["projected_100m_within_1800_seconds"] = (
        summary["projected_100m_seconds_linear"] <= 1800.0
    )
    summary["processed"] = processed
    summary["legacy_atlas_queries"] = int(runs[0]["legacy_atlas_queries"])
    summary["v3_fallback_queries"] = int(runs[0]["v3_fallback_queries"])
    summary["v4_residual_queries"] = int(runs[0]["v4_residual_queries"])
    return summary


def main() -> int:
    args = parse_args()
    if args.runs <= 0 or args.rss_poll_ms <= 0:
        raise ValueError("runs and RSS poll interval must be positive")
    executable = args.executable.resolve()
    atlas = args.atlas.resolve()
    source_input = args.input.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    available_rows = count_rows(source_input)
    sizes = [int(value) for value in args.sizes.split(",") if value.strip()]
    modes = [value.strip() for value in args.modes.split(",") if value.strip()]
    if any(size <= 0 for size in sizes):
        raise ValueError("sizes must be positive")
    allowed_modes = {"v3", "v4", "v4-base", "v4-reference", "v4-optimized", "v4-family", "v6"}
    if not set(modes).issubset(allowed_modes):
        raise ValueError(
            "modes must be v3, v4, v4-base, v4-reference, v4-optimized, v4-family, and/or v6"
        )

    inputs: dict[int, Path] = {}
    for size in sizes:
        if size == available_rows:
            inputs[size] = source_input
        else:
            suffix = "repeated" if size > available_rows else "prefix"
            prefix = output_dir / f"benchmark_input_{suffix}_{size}.csv"
            if not prefix.is_file() or count_rows(prefix) != size:
                create_sized_input(source_input, prefix, size)
            inputs[size] = prefix

    raw: dict[str, Any] = {
        "executable": str(executable),
        "executable_sha256": sha256_file(executable),
        "atlas": str(atlas),
        "atlas_bytes": atlas.stat().st_size,
        "input": str(source_input),
        "source_input_rows": available_rows,
        "runs_per_case": args.runs,
        "cases": {},
    }
    csv_rows: list[dict[str, Any]] = []
    for size in sizes:
        runs_by_mode: dict[str, list[dict[str, Any]]] = {mode: [] for mode in modes}
        # Alternate the within-pair order.  Running every V3 repetition before
        # every V4 repetition biases a sub-second query benchmark through
        # thermal/load drift even though Atlas loading dominates wall time.
        for run_index in range(args.runs):
            iteration_modes = modes if run_index % 2 == 0 else list(reversed(modes))
            for mode in iteration_modes:
                case_name = f"{mode}_{size}"
                output_path = output_dir / f"{case_name}_predictions.csv"
                result = run_once(
                    executable,
                    inputs[size],
                    output_path,
                    atlas,
                    mode,
                    args.rss_poll_ms / 1000.0,
                )
                result["run"] = run_index + 1
                runs_by_mode[mode].append(result)
                print(
                    f"case={case_name} run={run_index + 1}/{args.runs} "
                    f"wall={result['wall_seconds']:.6f}s "
                    f"query={result['query_and_io_seconds']:.6f}s "
                    f"peak={result['peak_rss_mib']:.3f}MiB",
                    flush=True,
                )
                csv_rows.append(
                    {
                        "case": case_name,
                        "mode": mode,
                        "rows": size,
                        "run": run_index + 1,
                        "wall_seconds": result["wall_seconds"],
                        "atlas_load_seconds": result["atlas_load_seconds"],
                        "query_and_io_seconds": result["query_and_io_seconds"],
                        "buffered_write_seconds": result["buffered_write_seconds"],
                        "throughput_qps": result["throughput_qps"],
                        "peak_rss_mib": result["peak_rss_mib"],
                        "output_sha256": result["output_sha256"],
                    }
                )
        for mode in modes:
            case_name = f"{mode}_{size}"
            output_path = output_dir / f"{case_name}_predictions.csv"
            case_runs = runs_by_mode[mode]
            raw["cases"][case_name] = {
                "mode": mode,
                "rows": size,
                "input": str(inputs[size]),
                "output": str(output_path),
                "raw_runs": case_runs,
                "summary": aggregate(case_runs),
            }

    (output_dir / "benchmark_results.json").write_text(
        json.dumps(raw, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    with (output_dir / "benchmark_runs.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(csv_rows[0]))
        writer.writeheader()
        writer.writerows(csv_rows)
    print(f"results={output_dir / 'benchmark_results.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
