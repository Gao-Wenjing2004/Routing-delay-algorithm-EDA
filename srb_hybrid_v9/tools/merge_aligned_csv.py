#!/usr/bin/env python3
"""Merge CSV shards with identical headers, rejecting duplicate endpoint rows."""

from __future__ import annotations

import argparse
import csv
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    header: list[str] | None = None
    rows: list[dict[str, str]] = []
    seen: set[tuple[str, str]] = set()
    for path in args.input:
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            fields = list(reader.fieldnames or ())
            if header is None:
                header = fields
            elif fields != header:
                raise ValueError(f"header mismatch: {path}")
            for row in reader:
                key = (row.get("From", ""), row.get("To", ""))
                if not all(key):
                    raise ValueError(f"missing From/To in {path}")
                if key in seen:
                    raise ValueError(f"duplicate endpoint row: {key}")
                seen.add(key)
                rows.append(row)

    if header is None:
        raise ValueError("no CSV header")
    rows.sort(key=lambda row: (row["From"], row["To"]))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=header)
        writer.writeheader()
        writer.writerows(rows)
    print(f"merged {len(args.input)} shards and {len(rows)} unique rows")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
