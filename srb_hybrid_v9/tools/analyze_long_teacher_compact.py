#!/usr/bin/env python3
"""Memory-bounded long-distance Teacher audit for reusable H/V skeletons."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
BUS = re.compile(r"\[\d+\]")


def parse(value: str) -> tuple[int, int, str]:
    match = ENDPOINT.match(value)
    if match is None:
        raise ValueError(f"bad endpoint: {value}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def stem(value: str) -> str:
    return BUS.sub("[*]", value)


def fnv1a(value: str) -> int:
    result = 2166136261
    for byte in value.encode("utf-8"):
        result = ((result ^ byte) * 16777619) & 0xFFFFFFFF
    return result


LEVELS = (
    ("source_port", "target_port", "direction", "band"),
    ("source_stem", "target_stem", "direction", "band"),
    ("source_stem", "direction", "band"),
    ("target_stem", "direction", "band"),
    ("source_stem", "target_stem", "direction"),
    ("direction", "band"),
    ("direction",),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    obstacles: Counter[str] = Counter()
    turns: Counter[int] = Counter()
    patterns: Counter[str] = Counter()
    unreachable = 0
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for raw in csv.DictReader(stream):
            if raw["Reachable"] != "1":
                unreachable += 1
                continue
            sx, sy, source_port = parse(raw["From"])
            tx, ty, target_port = parse(raw["To"])
            dx, dy = tx - sx, ty - sy
            distance = max(abs(dx), abs(dy))
            block = int(raw["BlockSegments"]) > 0
            gap = int(raw["GapCrossings"]) > 0
            obstacle = "block_and_gap" if block and gap else \
                "block_only" if block else "gap_only" if gap else "clear"
            skeleton = raw["TurnSequence"] or "identity"
            obstacles[obstacle] += 1
            turns[int(raw["Turns"])] += 1
            patterns[skeleton] += 1
            rows.append({
                "source_endpoint": raw["From"],
                "source_port": source_port, "target_port": target_port,
                "source_stem": stem(source_port), "target_stem": stem(target_port),
                "direction": ("E" if dx > 0 else "W" if dx < 0 else "0") +
                             ("N" if dy > 0 else "S" if dy < 0 else "0"),
                "band": "65-128" if distance <= 128 else
                        "129-256" if distance <= 256 else "257+",
                "skeleton": skeleton,
                "fold": fnv1a(raw["From"]) % 5,
            })

    hits = {limit: 0 for limit in (1, 4, 8, 16, 32)}
    candidates_sum = 0
    fold_sizes: Counter[int] = Counter()
    for fold in range(5):
        training = [row for row in rows if row["fold"] != fold]
        validation = [row for row in rows if row["fold"] == fold]
        fold_sizes[fold] = len(validation)
        libraries = []
        for level in LEVELS:
            library: dict[tuple[object, ...], Counter[str]] = defaultdict(Counter)
            for row in training:
                library[tuple(row[name] for name in level)][str(row["skeleton"])] += 1
            libraries.append(library)
        global_counts = Counter(str(row["skeleton"]) for row in training)
        for row in validation:
            ranked: list[str] = []
            seen: set[str] = set()
            for level, library in zip(LEVELS, libraries, strict=True):
                for label, count in library.get(
                    tuple(row[name] for name in level), Counter()
                ).most_common():
                    if count < 2 or label in seen:
                        continue
                    seen.add(label)
                    ranked.append(label)
                    if len(ranked) == 32:
                        break
                if len(ranked) == 32:
                    break
            for label, _ in global_counts.most_common():
                if len(ranked) == 32:
                    break
                if label not in seen:
                    seen.add(label)
                    ranked.append(label)
            candidates_sum += len(ranked)
            actual = str(row["skeleton"])
            for limit in hits:
                hits[limit] += actual in ranked[:limit]

    report = {
        "definition": "source-fold OOF selector recall for long-distance H/V turn skeletons",
        "reachable_rows": len(rows),
        "unreachable_rows": unreachable,
        "obstacle_classes": dict(sorted(obstacles.items())),
        "turn_distribution": {str(key): value for key, value in sorted(turns.items())},
        "unique_turn_skeletons": len(patterns),
        "top_turn_skeletons": dict(patterns.most_common(24)),
        "fold_sizes": {str(key): value for key, value in sorted(fold_sizes.items())},
        "mean_candidates": candidates_sum / max(len(rows), 1),
        **{
            f"top_{limit}_turn_skeleton_recall": count / max(len(rows), 1)
            for limit, count in hits.items()
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
