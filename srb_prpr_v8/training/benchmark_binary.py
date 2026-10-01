#!/usr/bin/env python3
"""Run deterministic local timing/size checks and emit reproducible reports."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time


def arguments() -> argparse.Namespace:
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=1_000_000)
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--work-dir", type=Path, default=root / "build" / "benchmark")
    parser.add_argument("--report-dir", type=Path, default=root / "analysis" / "prpr")
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(1 << 20):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    opt = arguments()
    opt.work_dir.mkdir(parents=True, exist_ok=True)
    opt.report_dir.mkdir(parents=True, exist_ok=True)
    exe = opt.exe.resolve()
    requests = opt.requests.resolve()
    runs = []
    for index in range(opt.runs):
        output = opt.work_dir / f"result_{index + 1}.csv"
        started = time.perf_counter()
        completed = subprocess.run(
            [str(exe), "-in", str(requests), "-out", str(output)],
            check=True,
            capture_output=True,
            text=True,
        )
        elapsed = time.perf_counter() - started
        runs.append(
            {
                "run": index + 1,
                "seconds": elapsed,
                "qps": opt.rows / elapsed,
                "sha256": sha256(output),
                "output_bytes": output.stat().st_size,
                "stderr": completed.stderr.strip(),
            }
        )
    mean_seconds = sum(row["seconds"] for row in runs) / len(runs)
    hashes = {row["sha256"] for row in runs}
    report = {
        "scope": "local Windows x86_64, 1M public request stream",
        "rows_per_run": opt.rows,
        "runs": runs,
        "mean_seconds": mean_seconds,
        "mean_qps": opt.rows / mean_seconds,
        "extrapolated_100m_seconds": mean_seconds * (100_000_000 / opt.rows),
        "deterministic": len(hashes) == 1,
        "executable_bytes": exe.stat().st_size,
        "note": "100M is a linear extrapolation, not an official-platform measurement.",
    }
    (opt.report_dir / "benchmark_results.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    (opt.report_dir / "five_run_hashes.txt").write_text(
        "\n".join(f"run_{row['run']} {row['sha256']}" for row in runs) + "\n",
        encoding="utf-8",
    )
    model_header = Path(__file__).resolve().parents[1] / "include" / "prpr_model_data.hpp"
    size_text = (
        f"benchmark_executable_bytes={exe.stat().st_size}\n"
        f"embedded_model_header_bytes={model_header.stat().st_size}\n"
        "external_atlas_bytes=0\n"
        f"under_90mb={'yes' if exe.stat().st_size < 90_000_000 else 'no'}\n"
    )
    (opt.report_dir / "submission_size_report.txt").write_text(size_text, encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if len(hashes) == 1 else 1


if __name__ == "__main__":
    raise SystemExit(main())
