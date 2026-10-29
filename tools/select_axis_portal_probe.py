#!/usr/bin/env python3
"""Select a deterministic 71/72 and Block-focused exact-path probe."""

from __future__ import annotations

import argparse
import csv
import hashlib
from collections import defaultdict
from pathlib import Path


TARGETS = {"distance_71", "distance_72", "block_cross", "block_near"}


def rank(row: dict[str, str]) -> bytes:
    return hashlib.blake2b(
        (row["Category"] + "\0" + row["From"] + "\0" + row["To"]).encode(),
        digest_size=16,
        person=b"axis-portal",
    ).digest()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--summaries", type=Path)
    parser.add_argument("--trace", type=Path)
    parser.add_argument("--summary-output", type=Path)
    parser.add_argument("--trace-output", type=Path)
    parser.add_argument("--rows", type=int, default=4096)
    args = parser.parse_args()
    groups: dict[str, list[dict[str, str]]] = defaultdict(list)
    with args.metadata.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if row["Category"] in TARGETS:
                groups[row["Category"]].append(row)
    if not groups:
        raise ValueError("no targeted rows")
    chosen: dict[tuple[str, str], dict[str, str]] = {}
    quota = max(1, args.rows // len(groups))
    for name, rows in sorted(groups.items()):
        # First retain one row per complete source to avoid a probe dominated
        # by a few source endpoints, then fill by a stable pair hash.
        per_source: dict[str, list[dict[str, str]]] = defaultdict(list)
        for row in rows:
            per_source[row["From"]].append(row)
        selected = []
        for source_rows in per_source.values():
            selected.append(min(source_rows, key=rank))
        seen = {(row["From"], row["To"]) for row in selected}
        selected.extend(
            row for row in sorted(rows, key=rank)
            if (row["From"], row["To"]) not in seen
        )
        for row in selected[:quota]:
            chosen[(row["From"], row["To"])] = row
    if len(chosen) < args.rows:
        remaining = [
            row for rows in groups.values() for row in rows
            if (row["From"], row["To"]) not in chosen
        ]
        for row in sorted(remaining, key=rank)[: args.rows - len(chosen)]:
            chosen[(row["From"], row["To"])] = row
    selected = sorted(chosen.values(), key=lambda row: (row["From"], row["To"]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream, lineterminator="\n")
        writer.writerow(("From", "To"))
        writer.writerows((row["From"], row["To"]) for row in selected)
    pairs = {(row["From"], row["To"]) for row in selected}

    def filter_csv(source: Path | None, destination: Path | None) -> None:
        if source is None and destination is None:
            return
        if source is None or destination is None:
            parser.error("summary/trace input and output must be supplied together")
        destination.parent.mkdir(parents=True, exist_ok=True)
        with source.open(encoding="utf-8-sig", newline="") as input_stream, destination.open(
            "w", encoding="utf-8", newline=""
        ) as output_stream:
            reader = csv.DictReader(input_stream)
            if not reader.fieldnames or "From" not in reader.fieldnames or "To" not in reader.fieldnames:
                raise ValueError(f"missing From/To columns: {source}")
            writer = csv.DictWriter(output_stream, fieldnames=reader.fieldnames, lineterminator="\n")
            writer.writeheader()
            writer.writerows(row for row in reader if (row["From"], row["To"]) in pairs)

    filter_csv(args.summaries, args.summary_output)
    filter_csv(args.trace, args.trace_output)
    counts = {name: sum(row["Category"] == name for row in selected) for name in sorted(groups)}
    print(f"selected={len(selected)} categories={counts}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
