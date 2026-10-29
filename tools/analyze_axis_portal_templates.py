#!/usr/bin/env python3
"""Measure no-request-identity skeleton reuse on 71/72 and Block paths."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import Counter, defaultdict
from pathlib import Path

from srb_hybrid_v9.tools.analyze_path_skeletons import stem, symbolic_skeleton


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
BEAMS = (1, 2, 4, 8, 16, 32)
LEVELS = (
    ("source_port", "target_port", "dx", "dy", "source_phase", "target_phase", "kind"),
    ("source_stem", "target_stem", "dx", "dy", "source_phase", "target_phase", "kind"),
    ("source_port", "target_port", "dx8", "dy8", "kind"),
    ("source_stem", "target_stem", "dx8", "dy8", "kind"),
    ("source_port", "target_stem", "direction", "kind"),
    ("source_stem", "target_port", "direction", "kind"),
    ("source_stem", "target_stem", "direction", "kind"),
    ("source_x8", "target_x8", "source_phase", "target_phase", "direction", "kind"),
    ("direction", "kind"),
)


def endpoint(text: str) -> tuple[int, int, str]:
    match = ENDPOINT.fullmatch(text)
    if not match:
        raise ValueError(f"bad endpoint: {text}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def direction(dx: int, dy: int) -> int:
    return (1 if dx > 0 else 0) + (2 if dx < 0 else 0) + (3 if dy > 0 else 0) + (6 if dy < 0 else 0)


def parse_panel(value: str) -> tuple[str, Path]:
    fields = value.split(":", 1)
    if len(fields) != 2:
        raise argparse.ArgumentTypeError("panel must be split:summaries.csv")
    return fields[0], Path(fields[1])


def load(path: Path) -> list[dict[str, object]]:
    result = []
    with path.open(encoding="utf-8-sig", newline="") as stream:
        for row in csv.DictReader(stream):
            if row["Reachable"] != "1":
                continue
            sx, sy, source_port = endpoint(row["From"])
            tx, ty, target_port = endpoint(row["To"])
            dx, dy = tx - sx, ty - sy
            distance = max(abs(dx), abs(dy))
            block = int(row["BlockSegments"]) > 0 or "B" in row["PortalSignature"] or "R" in row["PortalSignature"]
            if distance not in (71, 72) and not block:
                continue
            kind = f"d{distance}" if distance in (71, 72) else "block"
            result.append({
                "source_port": source_port,
                "target_port": target_port,
                "source_stem": stem(source_port),
                "target_stem": stem(target_port),
                "source_x8": sx // 8,
                "target_x8": tx // 8,
                "source_phase": sy % 100,
                "target_phase": ty % 100,
                "dx": dx,
                "dy": dy,
                "dx8": math.floor(dx / 8),
                "dy8": math.floor(dy / 8),
                "direction": direction(dx, dy),
                "kind": kind,
                "turn": row["TurnSequence"] or "identity",
                "symbolic": symbolic_skeleton(row["PrimitiveSequence"]),
            })
    return result


def libraries(rows: list[dict[str, object]], label: str):
    result = []
    for names in LEVELS:
        counts: dict[tuple[object, ...], Counter[str]] = defaultdict(Counter)
        for row in rows:
            counts[tuple(row[name] for name in names)][str(row[label])] += 1
        result.append((names, counts))
    return result


def candidates(row: dict[str, object], model, limit: int = 32) -> tuple[str, ...]:
    scores: dict[str, float] = {}
    support: dict[str, int] = {}
    for index, (names, table) in enumerate(model):
        count = table.get(tuple(row[name] for name in names), Counter())
        for label, occurrences in count.items():
            if occurrences < 2:
                continue
            value = math.log1p(occurrences) + 0.12 * (len(LEVELS) - index)
            scores[label] = max(scores.get(label, float("-inf")), value)
            support[label] = max(support.get(label, 0), occurrences)
    return tuple(sorted(scores, key=lambda label: (-scores[label], -support[label], label))[:limit])


def evaluate(training: list[dict[str, object]], validation: list[dict[str, object]], label: str) -> dict:
    model = libraries(training, label)
    hits = Counter()
    candidate_count = 0
    pattern_keys = set()
    for row in validation:
        values = candidates(row, model)
        candidate_count += len(values)
        pattern_keys.add(values)
        actual = str(row[label])
        for beam in BEAMS:
            hits[beam] += actual in values[:beam]
    return {
        "label": label,
        "training_rows": len(training),
        "validation_rows": len(validation),
        "mean_candidates": candidate_count / max(len(validation), 1),
        "unique_candidate_patterns": len(pattern_keys),
        "recall": {str(beam): hits[beam] / max(len(validation), 1) for beam in BEAMS},
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--panel", type=parse_panel, action="append", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    panels = {name: load(path) for name, path in args.panel}
    training = panels.get("train", [])
    if not training:
        raise ValueError("train panel is empty")
    report = {
        "schema": 1,
        "kind": "no-request-identity-axis-portal-template-recall",
        "rows": {name: len(rows) for name, rows in panels.items()},
        "validation": {
            name: [evaluate(training, rows, label) for label in ("turn", "symbolic")]
            for name, rows in panels.items() if name != "train"
        },
        "rule": "all keys contain only port/stem, displacement, coordinate phase, direction and structural kind",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
