#!/usr/bin/env python3
"""Audit whether exact P10 entry/exit events compress into small O(1) tables.

The input is produced from full Dijkstra path summaries.  All validation folds
hold out complete source endpoints or complete source regions, so repeated rows
from one source cannot leak its Portal event into training.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
from collections import Counter, defaultdict
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
BUS = re.compile(r"\[\d+\]")
BEAMS = (1, 2, 4, 8, 16, 32, 64)


def endpoint(text: str) -> tuple[int, int, str]:
    match = ENDPOINT.fullmatch(text)
    if not match:
        raise ValueError(f"bad endpoint: {text}")
    return int(match.group(1)), int(match.group(2)), match.group(3)


def stem(port: str) -> str:
    return BUS.sub("[*]", port)


def stable_fold(text: str, folds: int) -> int:
    digest = hashlib.blake2b(text.encode(), digest_size=8, person=b"P10-fold").digest()
    return int.from_bytes(digest, "little") % folds


LEVELS = (
    ("port", "x", "phase", "direction"),
    ("stem", "x", "phase", "direction"),
    ("port", "x8", "phase", "direction"),
    ("stem", "x8", "phase", "direction"),
    ("port", "phase", "direction"),
    ("stem", "phase", "direction"),
    ("port", "x8", "direction"),
    ("stem", "x8", "direction"),
    ("port", "direction"),
    ("stem", "direction"),
    ("direction",),
)

CONDITIONAL_LEVELS = (
    ("direction", "periods", "entry", "port", "x8", "phase"),
    ("direction", "periods", "entry", "stem", "x8", "phase"),
    ("direction", "periods", "entry", "port", "phase"),
    ("direction", "periods", "entry", "stem", "phase"),
    ("direction", "periods", "entry", "port"),
    ("direction", "periods", "entry", "stem"),
    ("direction", "periods", "entry"),
)


def features(row: dict[str, str], role: str) -> dict[str, object]:
    x, y, port = endpoint(row["From"] if role == "source" else row["To"])
    return {
        "port": port,
        "stem": stem(port),
        "x": x,
        "x8": x // 8,
        "phase": y % 100,
        "direction": int(row["Direction"]),
        "periods": int(row["Periods"]),
        "entry": int(row["EntryIndex"]),
    }


class Hierarchy:
    def __init__(self, levels: tuple[tuple[str, ...], ...]) -> None:
        self.levels = levels
        self.tables: list[dict[tuple[object, ...], Counter[int]]] = [
            defaultdict(Counter) for _ in levels
        ]
        self.global_counts: Counter[int] = Counter()

    def add(self, item: dict[str, object], label: int) -> None:
        for level, table in zip(self.levels, self.tables, strict=True):
            table[tuple(item[name] for name in level)][label] += 1
        self.global_counts[label] += 1

    def candidates(self, item: dict[str, object], limit: int = 64) -> tuple[int, ...]:
        scores: dict[int, float] = {}
        support: dict[int, int] = {}
        for index, (level, table) in enumerate(zip(self.levels, self.tables, strict=True)):
            counter = table.get(tuple(item[name] for name in level), Counter())
            for label, count in counter.items():
                if count < 2:
                    continue
                support[label] = max(support.get(label, 0), count)
                value = math.log1p(count) + 0.14 * (len(self.levels) - index)
                scores[label] = max(scores.get(label, float("-inf")), value)
        for label, count in self.global_counts.most_common(limit):
            scores.setdefault(label, 0.25 * math.log1p(count))
            support.setdefault(label, count)
        ordered = sorted(scores, key=lambda label: (-scores[label], -support[label], label))
        return tuple(ordered[:limit])


def evaluate(rows: list[dict[str, str]], fold_kind: str, folds: int) -> dict[str, object]:
    source_hits = Counter()
    target_hits = Counter()
    pair_hits = Counter()
    conditional_hits = Counter()
    evaluated = 0
    fold_rows = Counter()

    def row_fold(row: dict[str, str]) -> int:
        sx, sy, _ = endpoint(row["From"])
        token = row["From"] if fold_kind == "source" else f"{sx // 16}:{sy // 100}"
        return stable_fold(token, folds)

    assigned = [row_fold(row) for row in rows]
    for fold in range(folds):
        source_model = Hierarchy(LEVELS)
        target_model = Hierarchy(LEVELS)
        conditional_model = Hierarchy(CONDITIONAL_LEVELS)
        for row, owner in zip(rows, assigned, strict=True):
            if owner == fold:
                continue
            source_item = features(row, "source")
            target_item = features(row, "target")
            entry = int(row["EntryIndex"])
            exit_event = int(row["ExitIndex"])
            source_model.add(source_item, entry)
            target_model.add(target_item, exit_event)
            target_item["entry"] = entry
            conditional_model.add(target_item, exit_event)
        for row, owner in zip(rows, assigned, strict=True):
            if owner != fold:
                continue
            source_item = features(row, "source")
            target_item = features(row, "target")
            entry = int(row["EntryIndex"])
            exit_event = int(row["ExitIndex"])
            source = source_model.candidates(source_item)
            target = target_model.candidates(target_item)
            conditional_item = dict(target_item)
            conditional_item["entry"] = entry
            conditional = conditional_model.candidates(conditional_item)
            evaluated += 1
            fold_rows[fold] += 1
            for beam in BEAMS:
                source_ok = entry in source[:beam]
                target_ok = exit_event in target[:beam]
                source_hits[beam] += source_ok
                target_hits[beam] += target_ok
                pair_hits[beam] += source_ok and target_ok
                conditional_hits[beam] += source_ok and exit_event in conditional[:beam]

    return {
        "fold_kind": fold_kind,
        "folds": folds,
        "rows": evaluated,
        "fold_rows": {str(key): value for key, value in sorted(fold_rows.items())},
        "source_event_recall": {str(k): source_hits[k] / evaluated for k in BEAMS},
        "target_event_recall": {str(k): target_hits[k] / evaluated for k in BEAMS},
        "independent_pair_recall": {str(k): pair_hits[k] / evaluated for k in BEAMS},
        "entry_conditioned_pair_recall": {
            str(k): conditional_hits[k] / evaluated for k in BEAMS
        },
    }


def empirical_dictionary(rows: list[dict[str, str]], role: str, beam: int) -> dict[str, object]:
    model = Hierarchy(LEVELS)
    items: list[dict[str, object]] = []
    for row in rows:
        item = features(row, role)
        label = int(row["EntryIndex"] if role == "source" else row["ExitIndex"])
        model.add(item, label)
        items.append(item)
    finest_keys: dict[tuple[object, ...], tuple[int, ...]] = {}
    patterns = set()
    for item in items:
        key = tuple(item[name] for name in LEVELS[0])
        values = model.candidates(item, beam)
        finest_keys[key] = values
        patterns.add(values)
    ports = {str(item["port"]) for item in items}
    dense_keys = 2 * len(ports) * 166 * 100
    # Lower-bound accounting: uint16 pattern ID per dense key plus uint16 IDs.
    estimated = dense_keys * 2 + sum(len(pattern) * 2 for pattern in patterns)
    return {
        "role": role,
        "beam": beam,
        "ports": len(ports),
        "observed_finest_keys": len(finest_keys),
        "unique_candidate_patterns": len(patterns),
        "observed_pattern_dedup_ratio": len(patterns) / max(len(finest_keys), 1),
        "dense_key_count": dense_keys,
        "id_only_lower_bound_bytes": estimated,
        "warning": "candidate IDs only; local connector costs require additional encoding",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--decomposition", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--folds", type=int, default=5)
    args = parser.parse_args()
    with args.decomposition.open(encoding="utf-8-sig", newline="") as stream:
        rows = list(csv.DictReader(stream))
    if not rows:
        raise SystemExit("no decomposed P10 rows")
    payload = {
        "schema": 1,
        "kind": "p10-local-connector-compression-audit",
        "rows": len(rows),
        "directions": dict(Counter(row["Direction"] for row in rows)),
        "periods": dict(Counter(row["Periods"] for row in rows)),
        "unique_entries": len({row["EntryIndex"] for row in rows}),
        "unique_exits": len({row["ExitIndex"] for row in rows}),
        "validation": {
            "source_holdout": evaluate(rows, "source", args.folds),
            "region_holdout": evaluate(rows, "region", args.folds),
        },
        "dictionary_lower_bounds": [
            empirical_dictionary(rows, role, beam)
            for role in ("source", "target") for beam in (8, 16, 32)
        ],
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(payload, indent=2))


if __name__ == "__main__":
    main()
