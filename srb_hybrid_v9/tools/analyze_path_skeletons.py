#!/usr/bin/env python3
"""Measure reusable Top-K path-skeleton recall on exact Dijkstra summaries."""

from __future__ import annotations

import argparse
import csv
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
BUS = re.compile(r"\[\d+\]")


def endpoint(value: str) -> tuple[int, int, str]:
    match = ENDPOINT.match(value)
    if match is None:
        raise ValueError(f"bad endpoint: {value}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def stem(port: str) -> str:
    return BUS.sub("[*]", port)


def direction(dx: int, dy: int) -> int:
    return (1 if dx > 0 else 0) + (2 if dx < 0 else 0) + \
           (3 if dy > 0 else 0) + (6 if dy < 0 else 0)


def fnv1a(value: str) -> int:
    result = 2166136261
    for byte in value.encode("utf-8"):
        result = ((result ^ byte) * 16777619) & 0xffffffff
    return result


def vector_skeleton(primitive_sequence: str) -> str:
    result: list[str] = []
    for primitive in primitive_sequence.split("|") if primitive_sequence else []:
        result.append(primitive.split("@", 1)[0])
    return "|".join(result)


def symbolic_skeleton(primitive_sequence: str) -> str:
    """Keep turn-run choices while making final axis runs displacement-relative.

    For each axis, the last run is determined by total request displacement and
    all previous runs.  Replacing it with R avoids treating every dx/dy as a new
    class while preserving overshoot/backtrack choices such as H:+24,...,H:R.
    """
    primitive_vectors: list[tuple[str, int]] = []
    for primitive in primitive_sequence.split("|") if primitive_sequence else []:
        axis, delta = primitive.split("@", 1)[0].split(":", 1)
        primitive_vectors.append((axis, int(delta)))
    run_values: list[tuple[str, int]] = []
    for axis, delta in primitive_vectors:
        if run_values and run_values[-1][0] == axis:
            run_values[-1] = (axis, run_values[-1][1] + delta)
        else:
            run_values.append((axis, delta))
    last: dict[str, int] = {}
    for index, (axis, _) in enumerate(run_values):
        last[axis] = index
    return "|".join(
        f"{axis}:R" if last[axis] == index else f"{axis}:{delta:+d}"
        for index, (axis, delta) in enumerate(run_values)
    )


def ranked_library(
    training: list[dict[str, object]], key_names: tuple[str, ...], label: str,
) -> dict[tuple[object, ...], list[str]]:
    counts: dict[tuple[object, ...], Counter[str]] = defaultdict(Counter)
    for row in training:
        key = tuple(row[name] for name in key_names)
        counts[key][str(row[label])] += 1
    return {
        key: [value for value, _ in counter.most_common()]
        for key, counter in counts.items()
    }


def recall(
    training: list[dict[str, object]], validation: list[dict[str, object]], label: str,
) -> dict[str, object]:
    levels = (
        ("first_route", "last_route", "direction", "band"),
        ("first_route", "last_route", "direction"),
        ("first_route", "direction", "band"),
        ("last_route", "direction", "band"),
        ("source_port", "target_port", "direction", "band"),
        ("source_port", "target_port", "direction"),
        ("source_port", "target_stem", "direction", "band"),
        ("source_stem", "target_port", "direction", "band"),
        ("source_stem", "target_stem", "direction", "band"),
        ("source_stem", "target_stem", "direction"),
        ("source_port", "direction", "band"),
        ("target_port", "direction", "band"),
        ("source_stem", "direction", "band"),
        ("target_stem", "direction", "band"),
        ("direction", "band"),
        ("direction",),
    )
    libraries = [(names, ranked_library(training, names, label)) for names in levels]
    limits = (1, 4, 8, 16, 32, 64, 128)
    max_candidates = limits[-1]
    hits = {limit: 0 for limit in limits}
    candidate_sum = 0
    for row in validation:
        candidates: list[str] = []
        seen: set[str] = set()
        for names, library in libraries:
            key = tuple(row[name] for name in names)
            for candidate in library.get(key, []):
                if candidate in seen:
                    continue
                seen.add(candidate)
                candidates.append(candidate)
                if len(candidates) >= max_candidates:
                    break
            if len(candidates) >= max_candidates:
                break
        candidate_sum += len(candidates)
        actual = str(row[label])
        for limit in hits:
            hits[limit] += actual in candidates[:limit]
    return {
        "label": label,
        "training_rows": len(training),
        "validation_rows": len(validation),
        "unique_training_labels": len({str(row[label]) for row in training}),
        "mean_candidates": candidate_sum / len(validation) if validation else 0.0,
        **{
            f"top_{limit}_recall": hits[limit] / len(validation) if validation else 0.0
            for limit in hits
        },
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    rows: list[dict[str, object]] = []
    actual_classes: Counter[str] = Counter()
    turns: Counter[int] = Counter()
    net_steps: Counter[int] = Counter()
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for raw in csv.DictReader(stream):
            if raw["Reachable"] != "1":
                continue
            sx, sy, source_port = endpoint(raw["From"])
            tx, ty, target_port = endpoint(raw["To"])
            dx, dy = tx - sx, ty - sy
            cheb = max(abs(dx), abs(dy))
            block = int(raw["BlockSegments"]) > 0
            gap = int(raw["GapCrossings"]) > 0
            actual_class = "block_and_gap" if block and gap else \
                "block_only" if block else "gap_only" if gap else "clear"
            actual_classes[actual_class] += 1
            turns[int(raw["Turns"])] += 1
            net_steps[int(raw["NetSteps"])] += 1
            route_sequence = raw["RoutingStateSequence"].split(">") \
                if raw["RoutingStateSequence"] else []
            rows.append({
                "source_endpoint": raw["From"],
                "source_port": source_port,
                "target_port": target_port,
                "source_stem": stem(source_port),
                "target_stem": stem(target_port),
                "first_route": route_sequence[0] if route_sequence else "none",
                "last_route": route_sequence[-1] if route_sequence else "none",
                "direction": direction(dx, dy),
                "band": "0-8" if cheb <= 8 else "9-16" if cheb <= 16 else "17+",
                "turn_skeleton": raw["TurnSequence"] or "identity",
                "arc_turn_skeleton": f"{raw['FirstArc']}|{raw['TurnSequence']}|{raw['LastArc']}",
                "vector_skeleton": vector_skeleton(raw["PrimitiveSequence"]),
                "symbolic_skeleton": symbolic_skeleton(raw["PrimitiveSequence"]),
            })

    training = [row for row in rows if fnv1a(str(row["source_endpoint"])) % 5 != 0]
    validation = [row for row in rows if fnv1a(str(row["source_endpoint"])) % 5 == 0]
    report = {
        "split": "FNV1a(complete source endpoint) % 5; bucket 0 is validation",
        "rows": len(rows),
        "actual_path_obstacle_classes": dict(sorted(actual_classes.items())),
        "turn_count_distribution": {str(key): value for key, value in sorted(turns.items())},
        "net_step_distribution": {str(key): value for key, value in sorted(net_steps.items())},
        "selector_recall": [
            recall(training, validation, label)
            for label in (
                "turn_skeleton", "symbolic_skeleton", "arc_turn_skeleton", "vector_skeleton"
            )
        ],
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
