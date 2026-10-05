#!/usr/bin/env python3
"""Measure how compactly exact long Dijkstra paths map to elastic templates."""

from __future__ import annotations

import argparse
import csv
import json
from collections import Counter
from pathlib import Path

from generate_block_portal_oof_candidates import exact_template


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_ids = {name: index for index, name in enumerate(model["port_names"])}
    templates: Counter[str] = Counter()
    run_counts: Counter[int] = Counter()
    rows = unreachable = 0
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for raw in csv.DictReader(stream):
            if raw["Reachable"] != "1" or not raw["PrimitiveSequence"]:
                unreachable += 1
                continue
            label = exact_template(raw, model, port_ids)
            templates[label] += 1
            run_counts[label.split("@", 1)[0].count(">") + 1] += 1
            rows += 1

    ordered = templates.most_common()
    limits = (1, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 2048, 4096)
    prefix = 0
    cursor = 0
    coverage: dict[str, float] = {}
    for limit in limits:
        while cursor < min(limit, len(ordered)):
            prefix += ordered[cursor][1]
            cursor += 1
        coverage[f"top_{limit}_coverage"] = prefix / max(rows, 1)
    dictionary_bytes = sum(len(label.encode("utf-8")) + 2 for label in templates)
    report = {
        "definition": (
            "Exact primitive paths are merged into H/V runs; the final run on each axis "
            "is an elastic residual and is stored as zero."
        ),
        "rows": rows,
        "unreachable_or_empty": unreachable,
        "unique_elastic_templates": len(templates),
        "raw_template_dictionary_bytes": dictionary_bytes,
        "mean_rows_per_template": rows / max(len(templates), 1),
        "run_count_distribution": {
            str(key): value for key, value in sorted(run_counts.items())
        },
        **coverage,
        "top_templates": [
            {"template": label, "count": count, "rate": count / max(rows, 1)}
            for label, count in ordered[:24]
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
