#!/usr/bin/env python3
"""Publish selected validation metrics below GitHub's 4096-character notice limit."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def compact_metric(value: dict) -> dict:
    return {
        "n": value.get("rows", 0),
        "acc": round(float(value.get("accuracy", 0.0)), 6),
        "mae": round(float(value.get("mae_ps", 0.0)), 3),
        "exact": round(float(value.get("exact_share_pct", 0.0)), 4),
    }


def segments(values: dict[str, dict], limit: int | None = None) -> dict[str, dict]:
    rows = sorted(values.items(), key=lambda item: (float(item[1].get("accuracy", 0.0)), item[0]))
    if limit is not None:
        rows = rows[:limit]
    return {name: compact_metric(value) for name, value in rows}


def notice(title: str, value: object) -> None:
    message = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    if len(message) > 4000:
        raise ValueError(f"annotation {title!r} is too long: {len(message)}")
    print(f"::notice title={title}::{message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reports", type=Path, required=True)
    parser.add_argument("--residual", type=Path, required=True)
    parser.add_argument("--retirement", type=Path, required=True)
    args = parser.parse_args()

    for split in ("train", "source_holdout", "region_holdout"):
        report = load(args.reports / f"{split}_branch_errors.json")
        notice(f"V10 split {split}", {
            "overall": compact_metric(report["overall"]),
            "branches": segments(report["segments"]["branch"]),
            "categories_worst8": segments(report["segments"]["category"], 8),
            "distance": segments(report["segments"]["distance"]),
        })

    independent = load(args.reports / "k48_independent_608k.json")["accuracy"]
    selected_segments = {}
    for dimension in ("distance", "block_count", "gap_count"):
        if dimension in independent.get("segments", {}):
            selected_segments[dimension] = segments(independent["segments"][dimension])
    notice("V10 independent 608K", {
        "overall": compact_metric(independent["overall"]),
        "bootstrap_95": independent.get("bootstrap_95"),
        "conservative_accuracy_interval": independent.get("conservative_accuracy_interval"),
        "group_diagnostics": independent.get("group_diagnostics"),
        "segments": selected_segments,
    })

    residual = load(args.residual)
    notice("V10 direct residual", {
        "accepted": residual.get("accepted"),
        "baseline": residual.get("baseline"),
        "selected": residual.get("selected"),
        "asset_bytes": residual.get("asset_bytes"),
    })
    for name in ("path_gap_block_audit", "connector_gate", "reverse_batch_parity"):
        report = load(args.reports / f"{name}.json")
        if name == "path_gap_block_audit":
            report = report.get("overall", {})
        notice(f"V10 {name}", report)
    notice("V10 asset retirement", load(args.retirement))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
