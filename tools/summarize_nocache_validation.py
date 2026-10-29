#!/usr/bin/env python3
"""Emit compact, public-safe summaries for the no-cache validation workflow."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def load(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def metric(value: dict) -> dict:
    return {
        "rows": value.get("rows", 0),
        "accuracy": round(float(value.get("accuracy", 0.0)), 6),
        "mae_ps": round(float(value.get("mae_ps", 0.0)), 3),
        "exact_pct": round(float(value.get("exact_share_pct", 0.0)), 4),
    }


def weakest(values: dict[str, dict], limit: int = 6) -> list[dict]:
    eligible = [(name, value) for name, value in values.items() if int(value.get("rows", 0))]
    eligible.sort(key=lambda item: (float(item[1].get("accuracy", 0.0)), -int(item[1]["rows"])))
    return [{"name": name, **metric(value)} for name, value in eligible[:limit]]


def portal_summary(report: dict) -> dict:
    return {
        "structural_pass": report.get("structural_pass"),
        "retirement_authorized": report.get("retirement_authorized"),
        "checks": report.get("checks"),
        "measurements": report.get("measurements"),
        "next_action": report.get("next_action"),
    }


def speed_summary(report: dict) -> dict:
    variants = {}
    for threads, row in report.get("variants", {}).items():
        variants[threads] = {
            key: row.get(key) for key in (
                "load", "read", "parse", "predict", "format", "write",
                "local_1b_speedup_vs_k48", "official_1b_seconds_relative_projection",
                "score_at_accuracy_97_56", "score_at_accuracy_99_9", "score_at_accuracy_100",
            )
        }
    return {
        "rows": report.get("rows"),
        "official_baseline_seconds": report.get("official_baseline_seconds"),
        "best_threads": report.get("best_threads"),
        "variants": variants,
        "asset_load_ablation": report.get("asset_load_ablation"),
        "projection_warning": report.get("projection_warning"),
    }


def annotation(title: str, value: object) -> None:
    message = json.dumps(value, ensure_ascii=False, separators=(",", ":"))
    message = message.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
    print(f"::notice title={title}::{message}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--reports", type=Path)
    parser.add_argument("--residual", type=Path)
    parser.add_argument("--retirement", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--annotations", action="store_true")
    args = parser.parse_args()
    summary: dict[str, object] = {"schema": 1}

    if args.reports:
        splits: dict[str, object] = {}
        for split in ("train", "source_holdout", "region_holdout"):
            path = args.reports / f"{split}_branch_errors.json"
            if not path.exists():
                continue
            report = load(path)
            segments = report["segments"]
            splits[split] = {
                "overall": metric(report["overall"]),
                "weakest_branches": weakest(segments["branch"]),
                "weakest_categories": weakest(segments["category"]),
                "distance": {name: metric(value) for name, value in segments["distance"].items()},
            }
        summary["splits"] = splits
        for name in ("path_gap_block_audit", "connector_gate", "reverse_batch_parity", "k48_independent_608k"):
            path = args.reports / f"{name}.json"
            if path.exists():
                report = load(path)
                if name == "path_gap_block_audit":
                    summary[name] = report.get("overall", {})
                elif name == "connector_gate":
                    summary[name] = {
                        key: report.get(key) for key in (
                            "rows", "dimension", "connector_complete_rows", "lookup_kind",
                            "request_identity_stored", "exact_core_coverage_pct", "sides", "decision",
                        )
                    }
                else:
                    summary[name] = report
        portal = args.reports / "axis_portal_gate.json"
        if portal.exists():
            summary["axis_portal_gate"] = portal_summary(load(portal))
        speed = args.reports / "soa_mt_speed_summary.json"
        if speed.exists():
            summary["soa_mt_speed"] = speed_summary(load(speed))

    if args.residual and args.residual.exists():
        report = load(args.residual)
        selected = report.get("selected") or {}
        summary["direct_residual"] = {
            "accepted": report.get("accepted"),
            "baseline": report.get("baseline"),
            "selected": {
                key: selected.get(key) for key in (
                    "scale_ps", "coarse_support", "fine_support", "scores", "gains",
                    "corrected_rows", "coarse_cells", "fine_cells",
                )
            },
            "asset_bytes": report.get("asset_bytes"),
        }

    if args.retirement and args.retirement.exists():
        summary["asset_retirement"] = load(args.retirement)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(summary, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if args.annotations:
        for name, value in summary.items():
            if name != "schema":
                annotation(f"no-cache V10 {name}", value)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
