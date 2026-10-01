#!/usr/bin/env python3
"""Benchmark the formal exact SRB solver on a bounded CSV sample.

The exact solver is intentionally kept out of the V4 executable.  This helper
records a small-sample throughput/RSS reference without implying that a linear
projection is a substitute for a full 100M benchmark.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import time
from pathlib import Path

import psutil


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Benchmark the exact SRB baseline")
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--graph", required=True, type=Path)
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--json-out", required=True, type=Path)
    parser.add_argument("--rss-poll-ms", type=float, default=10.0)
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def required_float(pattern: str, text: str, name: str) -> float:
    match = re.search(pattern, text)
    if not match:
        raise ValueError(f"solver stderr missing {name}")
    return float(match.group(1))


def required_int(pattern: str, text: str, name: str) -> int:
    match = re.search(pattern, text)
    if not match:
        raise ValueError(f"solver stderr missing {name}")
    return int(match.group(1))


def main() -> int:
    args = parse_args()
    if args.rss_poll_ms <= 0:
        raise ValueError("--rss-poll-ms must be positive")
    executable = args.executable.resolve()
    graph = args.graph.resolve()
    input_path = args.input.resolve()
    output_path = args.output.resolve()
    json_path = args.json_out.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    json_path.parent.mkdir(parents=True, exist_ok=True)
    command = [str(executable), str(graph), "--csv", str(input_path), str(output_path)]
    started = time.perf_counter()
    process = subprocess.Popen(
        command,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    peak = 0
    ps_process = psutil.Process(process.pid)
    while process.poll() is None:
        try:
            memory = ps_process.memory_info()
            peak = max(peak, int(getattr(memory, "peak_wset", 0)), int(memory.rss))
        except (psutil.AccessDenied, psutil.NoSuchProcess):
            pass
        time.sleep(args.rss_poll_ms / 1000.0)
    stdout, stderr = process.communicate()
    wall = time.perf_counter() - started
    if process.returncode:
        raise RuntimeError(f"baseline failed ({process.returncode})\n{stdout}\n{stderr}")
    rows = required_int(r"processed=(\d+)", stderr, "processed")
    query_seconds = required_float(r"elapsed=([0-9.]+) s", stderr, "elapsed")
    load_seconds = required_float(r"binary_load=([0-9.]+) s", stderr, "binary_load")
    throughput = required_float(r"throughput=([0-9.]+) q/s", stderr, "throughput")
    projected_query = query_seconds * 100_000_000 / rows
    result = {
        "scope": "bounded exact-solver sample; projection is diagnostic only",
        "command": command,
        "executable_sha256": sha256_file(executable),
        "graph_sha256": sha256_file(graph),
        "rows": rows,
        "binary_load_seconds": load_seconds,
        "query_seconds": query_seconds,
        "wall_seconds": wall,
        "throughput_qps": throughput,
        "peak_rss_mib": peak / (1024.0 * 1024.0),
        "output_sha256": sha256_file(output_path),
        "projected_100m_seconds_linear": load_seconds + projected_query,
        "projected_100m_within_1800_seconds": load_seconds + projected_query <= 1800.0,
        "projection_caveat": "31-row query mix and cache state need not represent 100M rows",
    }
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    json_path.write_text(rendered + "\n", encoding="utf-8")
    print(rendered)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
