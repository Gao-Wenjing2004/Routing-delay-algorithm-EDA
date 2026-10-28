#!/usr/bin/env python3
"""Filter aligned request/Golden CSVs by geometric distance."""

from __future__ import annotations

import argparse
import csv
import re
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/")


def coord(value: str) -> tuple[int, int]:
    match = ENDPOINT.match(value)
    if not match:
        raise ValueError(f"bad endpoint: {value}")
    return int(match.group(1)), int(match.group(2))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--max-cheb", type=int, required=True)
    parser.add_argument("--out-requests", type=Path, required=True)
    parser.add_argument("--out-golden", type=Path, required=True)
    args = parser.parse_args()

    args.out_requests.parent.mkdir(parents=True, exist_ok=True)
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
        count = 0
        for index, (request, golden) in enumerate(zip(req_reader, gold_reader)):
            if request["From"] != golden["From"] or request["To"] != golden["To"]:
                raise ValueError(f"unaligned row {index}")
            sx, sy = coord(request["From"])
            tx, ty = coord(request["To"])
            if max(abs(tx - sx), abs(ty - sy)) > args.max_cheb:
                continue
            req_writer.writerow({"From": request["From"], "To": request["To"]})
            delay_key = next(key for key in golden if key.lower() == "delay")
            gold_writer.writerow({"From": golden["From"], "To": golden["To"], "Delay": golden[delay_key]})
            count += 1
    print(f"wrote {count} rows with Chebyshev distance <= {args.max_cheb}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
