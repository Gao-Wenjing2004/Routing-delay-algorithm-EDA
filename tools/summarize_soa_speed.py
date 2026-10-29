#!/usr/bin/env python3
"""Summarize byte-exact SoA/thread timings and project the official 1B score."""

from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


KEY_VALUE = re.compile(r"([a-z_]+)=([0-9.]+)")
ELAPSED = re.compile(r"Elapsed \(wall clock\) time.*:\s*(\S+)")


def duration(text: str) -> float:
    fields = [float(value) for value in text.split(":")]
    value = 0.0
    for field in fields:
        value = value * 60.0 + field
    return value


def timing(path: Path) -> dict[str, float]:
    text = path.read_text(encoding="utf-8", errors="replace")
    result: dict[str, float] = {}
    for line in text.splitlines():
        match = ELAPSED.search(line)
        if match:
            result["wall"] = duration(match.group(1))
        if "processed=" in line and "total_elapsed=" in line:
            result.update({key: float(value) for key, value in KEY_VALUE.findall(line)})
    if "wall" not in result:
        raise ValueError(f"GNU time elapsed value missing: {path}")
    return result


def total_score(accuracy: float, seconds: float) -> float:
    time_score = 100.0 * (1.0 - seconds / 1800.0)
    return 0.80 * accuracy + 0.15 * time_score + 5.0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--official-baseline-seconds", type=float, default=329.58)
    args = parser.parse_args()
    baseline = timing(args.reports / "k48_independent_timing.txt")
    rows = int(baseline.get("processed", 0))
    if rows <= 0 or "total_elapsed" not in baseline or "stream_elapsed" not in baseline:
        raise ValueError("K48 internal processed/total_elapsed/stream_elapsed metrics missing")
    scale = 1_000_000_000 / rows
    baseline_load = baseline["total_elapsed"] - baseline["stream_elapsed"]
    baseline_local_1b = baseline_load + baseline["stream_elapsed"] * scale
    variants = {}
    for threads in (1, 2, 4, 8):
        row = timing(args.reports / f"soa_mt_{threads}_timing.txt")
        if int(row.get("processed", 0)) != rows or "total_elapsed" not in row or "load" not in row:
            raise ValueError(f"SoA internal timing metrics missing for threads={threads}")
        local_1b = row["load"] + (row["total_elapsed"] - row["load"]) * scale
        projected_speedup = baseline_local_1b / local_1b
        projected = args.official_baseline_seconds / projected_speedup
        variants[str(threads)] = {
            **row,
            "wall_speedup_on_608k_vs_k48": baseline["wall"] / row["wall"],
            "local_1b_seconds_linear_projection": local_1b,
            "local_1b_speedup_vs_k48": projected_speedup,
            "official_1b_seconds_relative_projection": projected,
            "score_at_accuracy_97_56": total_score(97.56, projected),
            "score_at_accuracy_99_9": total_score(99.9, projected),
            "score_at_accuracy_100": total_score(100.0, projected),
        }
    best = min(variants, key=lambda key: variants[key]["official_1b_seconds_relative_projection"])
    report = {
        "schema": 1,
        "kind": "byte-exact-SoA-thread-speed-ablation",
        "official_baseline_seconds": args.official_baseline_seconds,
        "rows": rows,
        "linear_scale_to_1b": scale,
        "local_k48": baseline,
        "local_k48_1b_seconds_linear_projection": baseline_local_1b,
        "variants": variants,
        "best_threads": int(best),
        "projection_warning": (
            "Relative projection assumes local and official phase ratios are stable. "
            "A 10M/50M/100M unique Linux curve remains required before release."
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
