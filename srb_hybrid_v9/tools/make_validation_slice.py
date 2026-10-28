#!/usr/bin/env python3
"""Create an order-preserving deterministic public validation slice."""

from __future__ import annotations

import argparse
import csv
import random
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--count", type=int, default=20_000)
    parser.add_argument("--seed", type=int, default=20261002)
    parser.add_argument("--out-requests", type=Path, required=True)
    parser.add_argument("--out-golden", type=Path, required=True)
    args = parser.parse_args()

    with args.requests.open("r", encoding="utf-8-sig", newline="") as stream:
        total = sum(1 for _ in stream) - 1
    if not 0 < args.count <= total:
        raise ValueError(f"count must be in [1, {total}]")
    chosen = set(random.Random(args.seed).sample(range(total), args.count))

    args.out_requests.parent.mkdir(parents=True, exist_ok=True)
    args.out_golden.parent.mkdir(parents=True, exist_ok=True)
    with (
        args.requests.open("r", encoding="utf-8-sig", newline="") as req_in,
        args.golden.open("r", encoding="utf-8-sig", newline="") as gold_in,
        args.out_requests.open("w", encoding="utf-8", newline="") as req_out,
        args.out_golden.open("w", encoding="utf-8", newline="") as gold_out,
    ):
        req_reader = csv.DictReader(req_in)
        gold_reader = csv.DictReader(gold_in)
        req_writer = csv.DictWriter(req_out, fieldnames=["From", "To"])
        gold_writer = csv.DictWriter(gold_out, fieldnames=["From", "To", "Delay"])
        req_writer.writeheader()
        gold_writer.writeheader()
        written = 0
        for row_index, (request, golden) in enumerate(zip(req_reader, gold_reader)):
            if request["From"] != golden["From"] or request["To"] != golden["To"]:
                raise ValueError(f"request/Golden mismatch at zero-based row {row_index}")
            if row_index not in chosen:
                continue
            req_writer.writerow({"From": request["From"], "To": request["To"]})
            delay_key = next(key for key in golden if key.lower() == "delay")
            gold_writer.writerow(
                {"From": golden["From"], "To": golden["To"], "Delay": golden[delay_key]}
            )
            written += 1
    if written != args.count:
        raise RuntimeError(f"expected {args.count} rows, wrote {written}")
    print(f"wrote {written} deterministic validation rows from {total}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
