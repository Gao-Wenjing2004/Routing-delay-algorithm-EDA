#!/usr/bin/env python3
"""Measure held-out recall of V9 P5 skeletons and elastic path templates."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from analyze_path_skeletons import fnv1a
from analyze_structured_generator import (
    load_summary_rows,
    normalized_template,
    selected_skeletons,
    selected_templates,
    selector,
    template_selector,
)


LIMITS = (1, 4, 8, 16, 32, 64, 128, 256)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--training-summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    training = load_summary_rows(args.training_summaries, model, port_id)
    rows = load_summary_rows(args.summaries, model, port_id)
    validation = [
        row for row in rows
        if fnv1a(str(row["source_endpoint"])) % 5 == 0 and not row["block"]
    ]
    skeleton_libraries = selector(training)
    template_libraries = template_selector(training)
    skeleton_hits = {limit: 0 for limit in LIMITS}
    template_hits = {limit: 0 for limit in LIMITS}
    joint_hits = {limit: 0 for limit in LIMITS}
    missing_template = 0
    for row in validation:
        actual_skeleton = str(row["skeleton"])
        skeleton_candidates = selected_skeletons(row, skeleton_libraries, 16)
        actual_template = normalized_template(
            actual_skeleton, tuple(row["continuation_deltas"]))
        template_candidates = selected_templates(
            row, actual_skeleton, template_libraries, LIMITS[-1])
        if actual_template is None:
            missing_template += 1
            continue
        for limit in LIMITS:
            skeleton_ok = actual_skeleton in skeleton_candidates[:limit]
            template_ok = actual_template in template_candidates[:limit]
            skeleton_hits[limit] += skeleton_ok
            template_hits[limit] += template_ok
            joint_hits[limit] += skeleton_ok and template_ok

    denominator = max(len(validation) - missing_template, 1)
    report = {
        "definition": "held-out observable skeleton and normalized elastic-template recall",
        "training_source": str(args.training_summaries),
        "training_rows": len(training),
        "validation_rows_no_block": len(validation),
        "invalid_actual_templates": missing_template,
        "recall": {
            str(limit): {
                "skeleton": skeleton_hits[limit] / denominator,
                "template_given_actual_skeleton": template_hits[limit] / denominator,
                "joint_same_limit": joint_hits[limit] / denominator,
            }
            for limit in LIMITS
        },
        "note": "Template recall is a conservative path-identity metric; a different template may have the same exact delay.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
