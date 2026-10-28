#!/usr/bin/env python3
"""Fail closed before retiring K48 accuracy assets.

The gate requires independently validated local connectors.  A good residual
table or an exact P10 core alone is intentionally insufficient.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path


PROTECTED = (
    "augment48_full110.bin",  # Short48
    "complete_hull.bin",      # Hull
    "adaptive_a.bin",         # residual/selector support
    "pair30.delta.bin",       # learned residual assignment
)


def load(path: Path | None) -> dict:
    if path is None or not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def positive_three_way(report: dict) -> bool:
    gains = (report.get("selected") or {}).get("gains", {})
    return all(float(gains.get(name, 0.0)) > 0.0 for name in ("train", "source", "region"))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--residual-report", type=Path)
    parser.add_argument("--connector-report", type=Path)
    parser.add_argument("--p10-up-report", type=Path)
    parser.add_argument("--p10-down-report", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    residual = load(args.residual_report)
    connector = load(args.connector_report)
    up, down = load(args.p10_up_report), load(args.p10_down_report)
    checks = {
        "residual_three_way_positive": bool(residual.get("accepted")) and positive_three_way(residual),
        "connector_is_fixed_lookup": connector.get("lookup_kind") == "fixed-count-direct",
        "connector_has_no_request_identity": connector.get("request_identity_stored") is False,
        "connector_three_way_positive": positive_three_way(connector),
        "connector_exact_core_coverage_pct": float(connector.get("exact_core_coverage_pct", 0.0)) >= 99.0,
        "p10_up_lossless": bool(up.get("lossless", up.get("verified_entries", 0) > 0)),
        "p10_down_lossless": bool(down.get("lossless", down.get("verified_entries", 0) > 0)),
    }
    authorized = all(checks.values())
    inventory = {}
    reclaimable = 0
    for name in PROTECTED:
        path = args.runtime / "assets" / name
        size = path.stat().st_size if path.exists() else 0
        inventory[name] = size
        reclaimable += size
    report = {
        "schema": 1,
        "authorized": authorized,
        "policy": "fail closed; no Short48/Hull/residual removal before exact connector replacement",
        "checks": checks,
        "protected_assets": inventory,
        "potential_reclaim_bytes": reclaimable,
        "action": "assets may be retired in a new package" if authorized else "keep frozen K48 assets",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0 if authorized else 3


if __name__ == "__main__":
    raise SystemExit(main())
