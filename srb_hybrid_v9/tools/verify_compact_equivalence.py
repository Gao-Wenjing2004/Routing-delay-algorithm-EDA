#!/usr/bin/env python3
"""Compare full-graph and compact-kernel bounded-search traces."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("full", type=Path)
    parser.add_argument("compact", type=Path)
    args = parser.parse_args()
    checked = 0
    with (
        args.full.open("r", encoding="utf-8-sig", newline="") as full_stream,
        args.compact.open("r", encoding="utf-8-sig", newline="") as compact_stream,
    ):
        full_reader = csv.DictReader(full_stream)
        compact_reader = csv.DictReader(compact_stream)
        for line, (full, compact) in enumerate(zip(full_reader, compact_reader), 2):
            fields = ("exact_completed", "budget_exhausted", "expanded")
            if any(full[field] != compact[field] for field in fields):
                raise ValueError(f"search-state mismatch at line {line}")
            if full["exact_completed"] == "1" and full["output_delay"] != compact["output_delay"]:
                raise ValueError(f"exact-delay mismatch at line {line}")
            checked += 1
        if next(full_reader, None) is not None or next(compact_reader, None) is not None:
            raise ValueError("trace row-count mismatch")
    print(f"compact/full bounded-search equivalence: {checked}/{checked}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
