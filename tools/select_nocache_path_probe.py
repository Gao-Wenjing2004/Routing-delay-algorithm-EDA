#!/usr/bin/env python3
"""Select a deterministic structure-balanced path-reconstruction probe."""

from __future__ import annotations

import argparse
import csv
import hashlib
from collections import defaultdict
from pathlib import Path


def rank(row: dict[str, str]) -> bytes:
    return hashlib.blake2b((row["From"] + "\0" + row["To"]).encode(), digest_size=16).digest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rows", type=int, default=4096)
    args = parser.parse_args()
    groups: dict[str, list[dict[str, str]]] = defaultdict(list)
    with args.metadata.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            groups[row["Category"]].append(row)
    chosen: dict[tuple[str, str], dict[str, str]] = {}
    names = sorted(groups)
    quota = max(1, args.rows // max(1, len(names)))
    for name in names:
        for row in sorted(groups[name], key=rank)[:quota]:
            chosen[(row["From"], row["To"])] = row
    if len(chosen) < args.rows:
        remainder = (row for name in names for row in groups[name]
                     if (row["From"], row["To"]) not in chosen)
        for row in sorted(remainder, key=rank)[:args.rows - len(chosen)]:
            chosen[(row["From"], row["To"])] = row
    rows = sorted(chosen.values(), key=lambda row: (row["From"], row["To"]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("From", "To"))
        writer.writerows((row["From"], row["To"]) for row in rows)
    print(f"selected={len(rows)} categories={len(names)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
