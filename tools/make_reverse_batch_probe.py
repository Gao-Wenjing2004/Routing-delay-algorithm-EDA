#!/usr/bin/env python3
"""Create one-target requests for forward/reverse exact-Dijkstra parity."""

import argparse
import csv
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pool", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=256)
    args = parser.parse_args()
    sources: list[str] = []
    used: set[str] = set()
    target = ""
    with args.pool.open(encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            if not target:
                target = row["To"]
            if row["From"] != target and row["From"] not in used:
                sources.append(row["From"])
                used.add(row["From"])
            if len(sources) == args.rows:
                break
    if not target or len(sources) != args.rows:
        raise ValueError(f"only found {len(sources)} unique sources")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["From", "To"])
        writer.writerows((source, target) for source in sources)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
