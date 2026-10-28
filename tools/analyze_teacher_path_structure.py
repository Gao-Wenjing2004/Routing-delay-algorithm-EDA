#!/usr/bin/env python3
"""Audit actual Dijkstra paths for Gap and Block Portal structure."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter, defaultdict
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with args.trace.open(encoding="utf-8-sig", newline="") as stream:
        trace = {(row["From"], row["To"]): row for row in csv.DictReader(stream)}
    with args.metadata.open(encoding="utf-8-sig", newline="") as stream:
        categories = {(row["From"], row["To"]): row["Category"] for row in csv.DictReader(stream)}
    totals: Counter[str] = Counter()
    by_category: dict[str, Counter[str]] = defaultdict(Counter)
    examples: list[dict[str, object]] = []
    with args.summaries.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            key = row["From"], row["To"]
            if key not in trace:
                continue
            gap_crossings = int(row["GapCrossings"])
            blocks = int(row["BlockSegments"])
            portals = int(row["PortalEvents"])
            endpoint_gap = int(trace[key]["Gap"])
            category = categories.get(key, "unclassified")
            values = {
                "rows": 1,
                "path_gap_crossings": gap_crossings,
                "path_block_segments": blocks,
                "path_portal_events": portals,
                "endpoint_gap_nonzero": int(endpoint_gap != 0),
                "path_gap_without_endpoint_gap": int(gap_crossings > 0 and endpoint_gap == 0),
                "block_path_without_bbox_mask": int(blocks > 0 and int(trace[key].get("BlockMask", "0")) == 0),
            }
            totals.update(values)
            by_category[category].update(values)
            if (gap_crossings or blocks) and len(examples) < 64:
                examples.append({
                    "From": key[0], "To": key[1], "Category": category,
                    "GapCrossings": gap_crossings, "BlockSegments": blocks,
                    "PortalEvents": portals, "PortalSignature": row["PortalSignature"],
                })
    report = {
        "schema": 1,
        "definition": "Gap/Block counts are measured on the reconstructed exact Dijkstra path",
        "overall": dict(totals),
        "by_category": {name: dict(values) for name, values in sorted(by_category.items())},
        "examples": examples,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"overall": report["overall"], "categories": list(report["by_category"])}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
