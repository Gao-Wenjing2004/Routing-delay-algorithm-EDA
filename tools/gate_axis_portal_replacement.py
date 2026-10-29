#!/usr/bin/env python3
"""Fail closed before implementing or shipping the axis/Portal replacement."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


PANELS = ("train", "source_holdout", "region_holdout")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--oracle", type=Path, required=True)
    parser.add_argument("--templates", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    oracle = json.loads(args.oracle.read_text(encoding="utf-8"))
    templates = json.loads(args.templates.read_text(encoding="utf-8"))
    checks: dict[str, bool] = {}
    measurements: dict[str, dict[str, float | int]] = {}
    for panel in PANELS:
        row = oracle.get("panels", {}).get(panel, {}).get("replacement_target", {})
        measurements[panel] = row
        checks[f"{panel}_has_256_rows"] = int(row.get("rows", 0)) >= 256
        checks[f"{panel}_coverage_99pct"] = float(row.get("coverage_pct", 0.0)) >= 99.0
        checks[f"{panel}_never_under_golden"] = int(row.get("candidate_under_rows", 1)) == 0
        checks[f"{panel}_accuracy_positive"] = float(row.get("hybrid_gain", 0.0)) > 0.0
    validation = templates.get("validation", {})
    checks["source_template_report_present"] = "source_holdout" in validation
    checks["region_template_report_present"] = "region_holdout" in validation
    structural_pass = all(checks.values())
    report = {
        "schema": 1,
        "structural_pass": structural_pass,
        "retirement_authorized": False,
        "checks": checks,
        "measurements": measurements,
        "policy": (
            "A structural pass only authorizes a C++ fixed-lookup implementation. "
            "Short48/Hull retirement still requires byte-exact runtime, three-panel "
            "accuracy and end-to-end speed gates."
        ),
        "next_action": (
            "implement fixed-count C++ AXS9/P10/Portal lookup"
            if structural_pass else "keep K48 assets and revise connector/Portal geometry"
        ),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
