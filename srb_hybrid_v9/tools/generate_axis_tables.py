#!/usr/bin/env python3
"""Generate exact translation-invariant H/V state-transfer tables."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from analyze_axis_oracle import build_axis_table


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--transitions", type=Path, required=True)
    parser.add_argument("--radius", type=int, required=True)
    parser.add_argument("--search-radius", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    if args.radius <= 0 or args.search_radius < args.radius:
        parser.error("search-radius must cover a positive radius")

    horizontal = build_axis_table(
        args.transitions, "H", args.radius, args.search_radius
    )
    vertical = build_axis_table(
        args.transitions, "V", args.radius, args.search_radius
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as stream:
        horizontal.tofile(stream)
        vertical.tofile(stream)
    digest = hashlib.sha256(args.output.read_bytes()).hexdigest().upper()
    report = {
        "radius": args.radius,
        "search_radius": args.search_radius,
        "states": 160,
        "bytes": args.output.stat().st_size,
        "sha256": digest,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
