#!/usr/bin/env python3
"""Build a deterministic distance/Block-stratified public Golden audit slice."""

from __future__ import annotations

import argparse
import csv
import json
import random
import re
from collections import Counter, defaultdict
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/")
BANDS = ((0, 16, "0-16"), (17, 32, "17-32"), (33, 64, "33-64"),
         (65, 128, "65-128"), (129, 256, "129-256"), (257, 10**9, "257+"))


def coordinates(specification: str) -> tuple[int, int]:
    match = ENDPOINT.match(specification)
    if match is None:
        raise ValueError(f"bad endpoint: {specification}")
    return int(match.group(1)), int(match.group(2))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--per-stratum", type=int, default=10)
    parser.add_argument("--seed", type=int, default=20261002)
    parser.add_argument("--out-requests", type=Path, required=True)
    parser.add_argument("--out-golden", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    args = parser.parse_args()
    if args.per_stratum <= 0:
        raise ValueError("per-stratum must be positive")

    gap_root = json.loads(args.gap.read_text(encoding="utf-8-sig"))["Gap"]
    blocks = gap_root["Block"]
    rng = random.Random(args.seed)
    seen: Counter[str] = Counter()
    reservoirs: dict[str, list[tuple[int, dict[str, str], dict[str, str]]]] = defaultdict(list)

    with (
        args.requests.open("r", encoding="utf-8-sig", newline="") as request_stream,
        args.golden.open("r", encoding="utf-8-sig", newline="") as golden_stream,
    ):
        requests = csv.DictReader(request_stream)
        golden = csv.DictReader(golden_stream)
        for row_index, (request, answer) in enumerate(zip(requests, golden)):
            if request["From"] != answer["From"] or request["To"] != answer["To"]:
                raise ValueError(f"request/Golden mismatch at row {row_index}")
            sx, sy = coordinates(request["From"])
            tx, ty = coordinates(request["To"])
            distance = max(abs(tx - sx), abs(ty - sy))
            band = next(name for low, high, name in BANDS if low <= distance <= high)
            min_x, max_x = sorted((sx, tx))
            min_y, max_y = sorted((sy, ty))
            block_related = any(
                min_x <= int(block["right"]) and max_x >= int(block["left"])
                and min_y <= int(block["upper"]) and max_y >= int(block["lower"])
                for block in blocks
            )
            delay_key = next(key for key in answer if key.lower() == "delay")
            delay = int(answer[delay_key])
            delay_class = "low" if delay < 1500 else "normal"
            stratum = f"{band}|{'block' if block_related else 'no_block'}|{delay_class}"
            seen[stratum] += 1
            item = (row_index, dict(request), {
                "From": answer["From"], "To": answer["To"], "Delay": str(delay)})
            bucket = reservoirs[stratum]
            if len(bucket) < args.per_stratum:
                bucket.append(item)
            else:
                replacement = rng.randrange(seen[stratum])
                if replacement < args.per_stratum:
                    bucket[replacement] = item

    chosen = sorted((item for bucket in reservoirs.values() for item in bucket), key=lambda item: item[0])
    args.out_requests.parent.mkdir(parents=True, exist_ok=True)
    args.out_golden.parent.mkdir(parents=True, exist_ok=True)
    args.manifest.parent.mkdir(parents=True, exist_ok=True)
    with (
        args.out_requests.open("w", encoding="utf-8", newline="") as request_stream,
        args.out_golden.open("w", encoding="utf-8", newline="") as golden_stream,
    ):
        request_writer = csv.DictWriter(request_stream, fieldnames=["From", "To"])
        golden_writer = csv.DictWriter(golden_stream, fieldnames=["From", "To", "Delay"])
        request_writer.writeheader()
        golden_writer.writeheader()
        for _, request, answer in chosen:
            request_writer.writerow({"From": request["From"], "To": request["To"]})
            golden_writer.writerow(answer)

    selected_counts = Counter()
    for _, request, answer in chosen:
        sx, sy = coordinates(request["From"])
        tx, ty = coordinates(request["To"])
        distance = max(abs(tx - sx), abs(ty - sy))
        band = next(name for low, high, name in BANDS if low <= distance <= high)
        min_x, max_x = sorted((sx, tx))
        min_y, max_y = sorted((sy, ty))
        block_related = any(
            min_x <= int(block["right"]) and max_x >= int(block["left"])
            and min_y <= int(block["upper"]) and max_y >= int(block["lower"])
            for block in blocks
        )
        selected_counts[f"{band}|{'block' if block_related else 'no_block'}|{'low' if int(answer['Delay']) < 1500 else 'normal'}"] += 1

    manifest = {
        "seed": args.seed,
        "per_stratum": args.per_stratum,
        "rows": len(chosen),
        "available_counts": dict(sorted(seen.items())),
        "selected_counts": dict(sorted(selected_counts.items())),
    }
    args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(chosen)} stratified exact-audit rows across {len(selected_counts)} strata")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
