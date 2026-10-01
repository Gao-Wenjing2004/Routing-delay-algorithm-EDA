#!/usr/bin/env python3
"""Strictly compare two aligned SRB prediction CSV files.

The byte hash is reported separately from semantic delay equality so compiler or
line-ending differences cannot be confused with an algorithm change.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from collections import Counter
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare aligned SRB prediction CSV files")
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--json-out", type=Path)
    parser.add_argument("--examples", type=int, default=20)
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def delay_column(reader: csv.DictReader) -> str:
    for name in reader.fieldnames or ():
        if name.strip().lower() in {"delay", "min delay", "predict_delay"}:
            return name
    raise ValueError("CSV does not contain a delay column")


def next_nonempty(reader: csv.DictReader) -> dict[str, str] | None:
    for row in reader:
        if row and any(value for value in row.values()):
            return row
    return None


def compare(reference: Path, candidate: Path, example_limit: int) -> dict[str, object]:
    delta_counts: Counter[int] = Counter()
    examples: list[dict[str, object]] = []
    rows = 0
    mismatches = 0
    max_absolute_delta = 0
    with reference.open("r", encoding="utf-8-sig", newline="") as rf, candidate.open(
        "r", encoding="utf-8-sig", newline=""
    ) as cf:
        rr = csv.DictReader(rf)
        cr = csv.DictReader(cf)
        rd = delay_column(rr)
        cd = delay_column(cr)
        while True:
            reference_row = next_nonempty(rr)
            candidate_row = next_nonempty(cr)
            if reference_row is None or candidate_row is None:
                if reference_row is not candidate_row:
                    raise ValueError("row-count mismatch")
                break
            rows += 1
            reference_endpoint = (reference_row.get("From"), reference_row.get("To"))
            candidate_endpoint = (candidate_row.get("From"), candidate_row.get("To"))
            if reference_endpoint != candidate_endpoint:
                raise ValueError(f"endpoint/order mismatch at data row {rows}")
            reference_delay = int(reference_row[rd])
            candidate_delay = int(candidate_row[cd])
            delta = candidate_delay - reference_delay
            if delta:
                mismatches += 1
                delta_counts[delta] += 1
                max_absolute_delta = max(max_absolute_delta, abs(delta))
                if len(examples) < example_limit:
                    examples.append(
                        {
                            "data_row": rows,
                            "from": reference_endpoint[0],
                            "to": reference_endpoint[1],
                            "reference_delay": reference_delay,
                            "candidate_delay": candidate_delay,
                            "delta": delta,
                        }
                    )
    reference_sha256 = sha256_file(reference)
    candidate_sha256 = sha256_file(candidate)
    return {
        "reference": str(reference.resolve()),
        "candidate": str(candidate.resolve()),
        "reference_sha256": reference_sha256,
        "candidate_sha256": candidate_sha256,
        "byte_identical": reference_sha256 == candidate_sha256,
        "rows": rows,
        "prediction_identical": mismatches == 0,
        "mismatches": mismatches,
        "mismatch_rate": mismatches / rows if rows else 0.0,
        "max_absolute_delta": max_absolute_delta,
        "signed_delta_counts": {str(key): delta_counts[key] for key in sorted(delta_counts)},
        "examples": examples,
    }


def main() -> int:
    args = parse_args()
    if args.examples < 0:
        raise ValueError("--examples must be non-negative")
    result = compare(args.reference, args.candidate, args.examples)
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
