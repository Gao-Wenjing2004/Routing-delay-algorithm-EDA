#!/usr/bin/env python3
"""Learn reusable elastic Block-Portal templates with source-fold OOF selection."""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
from collections import Counter, defaultdict
from pathlib import Path

from export_exact_path_candidates import ENDPOINT, primitive_runs


BUS = re.compile(r"\[\d+\]")


def fnv1a(value: str) -> int:
    result = 2166136261
    for byte in value.encode("utf-8"):
        result = ((result ^ byte) * 16777619) & 0xFFFFFFFF
    return result


def parse_endpoint(value: str) -> tuple[int, int, str]:
    instance, port = value.split("/", 1)
    _, x, y = instance.split("_")
    return int(x), int(y), port


def stem(port: str) -> str:
    return BUS.sub("[*]", port)


def portal_geometry(sx: int, sy: int, tx: int, ty: int) -> bool:
    low_y, high_y = sorted((sy, ty))
    return 76 <= sx <= 89 and 76 <= tx <= 89 and any(
        high_y >= lower and low_y <= lower + 49 for lower in range(0, 501, 100)
    )


def exact_template(raw: dict[str, str], model: dict[str, object], port_ids: dict[str, int]) -> str:
    runs = primitive_runs(raw["PrimitiveSequence"])
    source_port = ENDPOINT.match(raw["From"])
    if source_port is None:
        raise ValueError(f"bad endpoint: {raw['From']}")
    pid = port_ids[source_port.group(1)]
    iid = int(model["port_to_input"][pid])
    route = int(model["input_to_state"][iid]) if iid >= 0 else -1
    if route < 0:
        first_delta = int(
            raw["PrimitiveSequence"].split("|", 1)[0]
            .split("@", 1)[0].split(":", 1)[1]
        )
        runs[0][1] = int(runs[0][1]) - first_delta
    axes = [str(item[0]) for item in runs]
    values = [int(item[1]) for item in runs]
    h_trunk = max((i for i, axis in enumerate(axes) if axis == "H"), default=-1)
    v_trunk = max((i for i, axis in enumerate(axes) if axis == "V"), default=-1)
    if h_trunk >= 0:
        values[h_trunk] = 0
    if v_trunk >= 0:
        values[v_trunk] = 0
    return (
        ">".join(axes) + f"@{h_trunk}:{v_trunk}:" +
        ":".join(map(str, values)) + "S"
    )


LEVELS = (
    ("source_port", "target_port", "direction", "block_count"),
    ("source_stem", "target_stem", "direction", "block_count"),
    ("source_stem", "direction", "block_count"),
    ("target_stem", "direction", "block_count"),
    ("source_stem", "target_stem", "direction"),
    ("direction", "block_count"),
    ("direction", "band"),
    ("direction",),
)


def libraries(rows: list[dict[str, object]]) -> list[dict[tuple[object, ...], Counter[str]]]:
    result: list[dict[tuple[object, ...], Counter[str]]] = []
    for level in LEVELS:
        values: dict[tuple[object, ...], Counter[str]] = defaultdict(Counter)
        for row in rows:
            values[tuple(row[name] for name in level)][str(row["template"])] += 1
        result.append(values)
    return result


def select(row: dict[str, object], libs, global_counts: Counter[str], limit: int) -> list[str]:
    scores: dict[str, float] = {}
    support: dict[str, int] = {}
    for level_index, (level, library) in enumerate(zip(LEVELS, libs, strict=True)):
        counter = library.get(tuple(row[name] for name in level), Counter())
        for label, count in counter.items():
            # Singleton-specific keys are not allowed to outrank reusable evidence.
            if count < 2:
                continue
            support[label] = max(support.get(label, 0), count)
            score = math.log1p(count) + 0.12 * (len(LEVELS) - level_index)
            scores[label] = max(scores.get(label, float("-inf")), score)
    for label, count in global_counts.most_common(64):
        scores.setdefault(label, 0.35 * math.log1p(count))
        support.setdefault(label, count)
    return sorted(
        scores, key=lambda label: (-scores[label], -support[label], label)
    )[:limit]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--beam", type=int, default=32)
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_ids = {name: index for index, name in enumerate(model["port_names"])}
    rows: list[dict[str, object]] = []
    with args.summaries.open("r", encoding="utf-8-sig", newline="") as stream:
        for raw in csv.DictReader(stream):
            if raw["Reachable"] != "1" or not raw["PrimitiveSequence"]:
                continue
            sx, sy, source_port = parse_endpoint(raw["From"])
            tx, ty, target_port = parse_endpoint(raw["To"])
            if not portal_geometry(sx, sy, tx, ty):
                continue
            crossed = sum(
                max(sy, ty) >= lower and min(sy, ty) <= lower + 49
                for lower in range(0, 501, 100)
            )
            distance = max(abs(tx - sx), abs(ty - sy))
            rows.append({
                "from": raw["From"], "to": raw["To"], "golden": int(raw["Delay"]),
                "source_port": source_port, "target_port": target_port,
                "source_stem": stem(source_port), "target_stem": stem(target_port),
                "direction": ("E" if tx > sx else "W" if tx < sx else "0") +
                             ("N" if ty > sy else "S" if ty < sy else "0"),
                "block_count": crossed,
                "band": "65-128" if distance <= 128 else "129-256" if distance <= 256 else "257+",
                "fold": fnv1a(raw["From"]) % 5,
                "template": exact_template(raw, model, port_ids),
            })

    selected_rows: list[tuple[dict[str, object], list[str]]] = []
    hits = {limit: 0 for limit in (1, 4, 8, 16, 32) if limit <= args.beam}
    fold_sizes: Counter[int] = Counter()
    for fold in range(5):
        training = [row for row in rows if row["fold"] != fold]
        validation = [row for row in rows if row["fold"] == fold]
        libs = libraries(training)
        global_counts = Counter(str(row["template"]) for row in training)
        fold_sizes[fold] = len(validation)
        for row in validation:
            candidates = select(row, libs, global_counts, args.beam)
            actual = str(row["template"])
            for limit in hits:
                hits[limit] += actual in candidates[:limit]
            selected_rows.append((row, candidates))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("From", "To", "Golden", "Candidates"))
        for row, candidates in selected_rows:
            writer.writerow((row["from"], row["to"], row["golden"], ";".join(candidates)))

    report = {
        "definition": "source-fold OOF reusable elastic templates for periodic Block Portal rows",
        "rows": len(rows),
        "unique_templates": len({str(row["template"]) for row in rows}),
        "beam": args.beam,
        "fold_sizes": {str(key): value for key, value in sorted(fold_sizes.items())},
        **{
            f"top_{limit}_exact_template_recall": count / max(len(rows), 1)
            for limit, count in hits.items()
        },
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
