#!/usr/bin/env python3
"""Build a small runtime-oriented elastic-template Beam.

Unlike the full P6 research generator, this model deliberately uses only six
coarse, enumerable feature levels.  Its tables can therefore be embedded in a
submission and queried before pricing without an endpoint-pair Atlas.
"""

from __future__ import annotations

import argparse
import csv
import json
import struct
from pathlib import Path

from analyze_global_template_beam import build_support_model, support_rank
from analyze_path_skeletons import fnv1a
from analyze_structured_generator import load_summary_rows, normalized_template


LEVELS = (
    ("source_stem", "target_stem", "direction", "band"),
    ("source_stem", "target_stem", "direction"),
    ("source_stem", "direction", "band"),
    ("target_stem", "direction", "band"),
    ("direction", "band"),
    ("direction",),
)


def label(row: dict[str, object]):
    pattern = str(row["skeleton"])
    template = normalized_template(pattern, tuple(row["continuation_deltas"]))
    return None if template is None else (pattern, template)


def encode(value) -> str:
    pattern, template = value
    return pattern + "@" + ":".join(str(item) for item in template) + "S"


def key_hash(values: tuple[object, ...]) -> int:
    result = 14695981039346656037
    for value in values:
        for byte in str(value).encode("utf-8"):
            result ^= byte
            result = (result * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        result ^= 0xFF
        result = (result * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return result


def export_runtime_table(path: Path, support, per_level: int) -> None:
    labels = sorted(support.global_counts["all"], key=encode)
    if len(labels) >= 65535:
        raise ValueError("runtime table needs wider template IDs")
    label_id = {label: index for index, label in enumerate(labels)}
    global_counts = support.global_counts["all"]
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        stream.write(b"P6RTB001")
        stream.write(struct.pack("<I", len(labels)))
        for item in labels:
            encoded = encode(item).encode("utf-8")
            stream.write(struct.pack("<H", len(encoded)))
            stream.write(encoded)
        stream.write(struct.pack("<Q", sum(global_counts.values())))
        stream.write(struct.pack("<" + "I" * len(labels), *(
            global_counts[item] for item in labels)))
        global_top = [label_id[item] for item, _ in global_counts.most_common(per_level)]
        stream.write(struct.pack("<H", len(global_top)))
        stream.write(struct.pack("<" + "H" * len(global_top), *global_top))
        stream.write(struct.pack("<H", len(support.levels)))
        for level in support.levels:
            stream.write(struct.pack("<I", len(level.counts)))
            seen_hashes: set[int] = set()
            for key, counts in level.counts.items():
                hashed = key_hash(key)
                if hashed in seen_hashes:
                    raise ValueError(f"feature-key hash collision in {level.names}")
                seen_hashes.add(hashed)
                top = [label_id[item] for item, _ in counts.most_common(per_level)]
                entries = sorted((label_id[item], count) for item, count in counts.items())
                stream.write(struct.pack("<QIHH", hashed, sum(counts.values()), len(top), len(entries)))
                stream.write(struct.pack("<" + "H" * len(top), *top))
                for item, count in entries:
                    stream.write(struct.pack("<HI", item, count))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--training-summaries", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--candidates-output", type=Path, required=True)
    parser.add_argument("--table-output", type=Path)
    parser.add_argument("--beam", type=int, default=8)
    parser.add_argument("--alpha", type=float, default=32.0)
    parser.add_argument("--min-count", type=int, default=2)
    parser.add_argument("--per-level", type=int, default=32)
    parser.add_argument("--run-penalty", type=float, default=0.0)
    parser.add_argument("--validation-modulus", type=int, default=5)
    parser.add_argument("--validation-fold", type=int, default=0)
    parser.add_argument("--exclude-validation-fold-from-training", action="store_true")
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    training = load_summary_rows(args.training_summaries, model, port_id)
    if args.exclude_validation_fold_from_training:
        training = [
            row for row in training
            if fnv1a(str(row["source_endpoint"])) % args.validation_modulus !=
            args.validation_fold
        ]
    validation = [
        row for row in load_summary_rows(args.summaries, model, port_id)
        if fnv1a(str(row["source_endpoint"])) % args.validation_modulus ==
        args.validation_fold and not row["block"]
    ]
    support = build_support_model(training, LEVELS, label, lambda row: "all")
    if args.table_output is not None:
        export_runtime_table(args.table_output, support, args.per_level)
    level_key_counts = [len(level.counts) for level in support.levels]
    level_label_entries = [
        sum(len(counts) for counts in level.counts.values())
        for level in support.levels
    ]
    labels = set(support.global_counts.get("all", {}))
    estimated_binary_bytes = sum(len(encode(item)) + 1 for item in labels)
    estimated_binary_bytes += 6 * len(labels)
    for level, key_count, entry_count in zip(
            support.levels, level_key_counts, level_label_entries):
        # uint16 feature IDs, uint32 support, uint16 entry count, then
        # (uint16 template ID, uint32 count) for every exact score entry.
        estimated_binary_bytes += key_count * (2 * len(level.names) + 6)
        estimated_binary_bytes += entry_count * 6

    hits = 0
    exported = []
    for row in validation:
        ranked = support_rank(
            row, support, "all", max(args.beam, args.per_level * 2),
            args.alpha, args.min_count, args.per_level)
        ranked.sort(key=lambda item: (
            -(item[1] - args.run_penalty * (
                0 if item[0][0] == "identity" else item[0][0].count(">") + 1)),
            str(item[0])))
        ranked = ranked[:args.beam]
        candidates = [item[0] for item in ranked]
        hits += label(row) in candidates
        exported.append((
            row["from"], row["to"], row["golden"],
            ";".join(encode(item) for item in candidates)))

    report = {
        "definition": "coarse enumerable support-aware elastic-template Beam",
        "levels": LEVELS,
        "training_rows": len(training),
        "validation_rows_no_block": len(validation),
        "beam": args.beam,
        "alpha": args.alpha,
        "min_count": args.min_count,
        "per_level": args.per_level,
        "run_penalty": args.run_penalty,
        "unique_templates": len(labels),
        "level_key_counts": level_key_counts,
        "level_label_entries": level_label_entries,
        "estimated_exact_binary_table_bytes": estimated_binary_bytes,
        "exact_teacher_template_recall": hits / max(len(validation), 1),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    args.candidates_output.parent.mkdir(parents=True, exist_ok=True)
    with args.candidates_output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("From", "To", "Golden", "Candidates"))
        writer.writerows(exported)
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
