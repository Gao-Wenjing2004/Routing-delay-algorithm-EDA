#!/usr/bin/env python3
"""Audit a support-aware global Beam for P5 elastic path templates.

This stage does not price paths.  It asks the cheaper prerequisite question:
does one global Beam contain the held-out Dijkstra skeleton/template at all?
Sparse one-sample keys are shrunk toward a skeleton-specific global prior.
Half of the Beam may be reserved for single-run mutations of strong seeds.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Hashable

from analyze_path_skeletons import fnv1a
from analyze_structured_generator import (
    SELECTOR_LEVELS,
    TEMPLATE_LEVELS,
    load_summary_rows,
    normalized_template,
    selected_skeletons,
    selector,
)
from analyze_axis_oracle import build_axis_table


Label = Hashable


@dataclass
class CountLevel:
    names: tuple[str, ...]
    counts: dict[tuple[object, ...], Counter]


@dataclass
class SupportModel:
    levels: list[CountLevel]
    global_counts: dict[str, Counter]


def build_support_model(
    rows: list[dict[str, object]], levels: tuple[tuple[str, ...], ...],
    label_fn: Callable[[dict[str, object]], Label | None],
    context_fn: Callable[[dict[str, object]], str],
) -> SupportModel:
    count_levels = [defaultdict(Counter) for _ in levels]
    global_counts: dict[str, Counter] = defaultdict(Counter)
    for row in rows:
        label = label_fn(row)
        if label is None:
            continue
        context = context_fn(row)
        global_counts[context][label] += 1
        for index, names in enumerate(levels):
            count_levels[index][tuple(row[name] for name in names)][label] += 1
    return SupportModel(
        [CountLevel(names, dict(counts)) for names, counts in zip(levels, count_levels)],
        dict(global_counts),
    )


def support_rank(
    row: dict[str, object], model: SupportModel, context: str, limit: int,
    alpha: float, min_count: int, per_level: int,
) -> list[tuple[Label, float]]:
    prior_counts = model.global_counts.get(context, Counter())
    if not prior_counts:
        return []
    candidate_labels: set[Label] = {
        label for label, _ in prior_counts.most_common(per_level)
    }
    matched: list[Counter] = []
    for level in model.levels:
        counts = level.counts.get(tuple(row[name] for name in level.names))
        if not counts:
            continue
        matched.append(counts)
        candidate_labels.update(label for label, _ in counts.most_common(per_level))

    prior_total = sum(prior_counts.values())
    prior_width = max(len(prior_counts), 1)
    ranked = []
    for label in candidate_labels:
        base = (prior_counts[label] + 0.5) / (prior_total + 0.5 * prior_width)
        best_gain = 0.0
        for counts in matched:
            count = counts[label]
            if count < min_count:
                continue
            support = sum(counts.values())
            posterior = (count + alpha * base) / (support + alpha)
            # Evidence from a tiny key is deliberately weak even when pure.
            confidence = (count / (count + 2.0)) * (support / (support + 8.0))
            best_gain = max(best_gain, confidence * math.log(max(posterior / base, 1e-12)))
        ranked.append((label, math.log(base) + best_gain))
    ranked.sort(key=lambda item: (-item[1], str(item[0])))
    return ranked[:limit]


def skeleton_model(rows: list[dict[str, object]]) -> SupportModel:
    return build_support_model(
        rows,
        # Skeleton is the label, so its feature keys do not contain skeleton.
        SELECTOR_LEVELS,
        lambda row: str(row["skeleton"]),
        lambda row: "all",
    )


def template_model(
    rows: list[dict[str, object]],
    levels: tuple[tuple[str, ...], ...] = TEMPLATE_LEVELS,
) -> SupportModel:
    return build_support_model(
        rows,
        levels,
        lambda row: normalized_template(
            str(row["skeleton"]), tuple(row["continuation_deltas"])),
        lambda row: str(row["skeleton"]),
    )


def delta_model(rows: list[dict[str, object]], band_name: str = "band") -> SupportModel:
    base_levels = (
        ("source_port", "target_port", "direction", band_name),
        ("source_stem", "target_stem", "direction", band_name),
        ("source_port", "direction", band_name),
        ("target_port", "direction", band_name),
        ("direction", band_name),
        ("direction",),
    )
    levels = tuple(names + ("skeleton", "run_index") for names in base_levels)
    count_levels = [defaultdict(Counter) for _ in levels]
    global_counts: dict[str, Counter] = defaultdict(Counter)
    for row in rows:
        for run_index, delta in enumerate(row["continuation_deltas"]):
            context = f"{row['skeleton']}:{run_index}"
            label = int(delta)
            global_counts[context][label] += 1
            for index, names in enumerate(levels):
                key = tuple(
                    run_index if name == "run_index" else row[name]
                    for name in names
                )
                count_levels[index][key][label] += 1
    return SupportModel(
        [CountLevel(names, dict(counts)) for names, counts in zip(levels, count_levels)],
        dict(global_counts),
    )


def mutation_candidates(
    row: dict[str, object], pattern: str, template: tuple[int, ...],
    delta_support: SupportModel, alternatives: int, alpha: float,
    min_count: int, per_level: int,
) -> list[tuple[tuple[int, ...], float]]:
    axes = [] if pattern == "identity" else pattern.split(">")
    if len(template) != len(axes) + 2:
        return []
    trunks = {int(template[0]), int(template[1])}
    values = list(template[2:])
    result = []
    for run_index in range(len(axes)):
        if run_index in trunks:
            continue
        query = dict(row)
        query["skeleton"] = pattern
        query["run_index"] = run_index
        ranked = support_rank(
            query, delta_support, f"{pattern}:{run_index}", alternatives + 1,
            alpha, min_count, per_level)
        for rank, (delta, delta_score) in enumerate(ranked):
            delta = int(delta)
            if delta == values[run_index]:
                continue
            mutated = list(values)
            mutated[run_index] = delta
            result.append((tuple(template[:2]) + tuple(mutated), delta_score - 0.08 * rank))
            if len(result) >= alternatives * max(len(axes), 1):
                break
    return result


def propose(
    row: dict[str, object], skeleton_libraries,
    template_support: SupportModel, delta_support: SupportModel,
    beam: int, skeleton_limit: int, seeds_per_skeleton: int,
    mutation_seed_count: int, mutation_alternatives: int,
    mutation_slots: int, alpha: float, min_count: int, per_level: int,
) -> list[tuple[str, tuple[int, ...], float, str]]:
    skeletons = [
        (pattern, -0.35 * rank)
        for rank, pattern in enumerate(
            selected_skeletons(row, skeleton_libraries, skeleton_limit))
    ]
    seeds: dict[tuple[str, tuple[int, ...]], tuple[float, str]] = {}
    mandatory: dict[tuple[str, tuple[int, ...]], tuple[float, str]] = {}
    mutations: dict[tuple[str, tuple[int, ...]], tuple[float, str]] = {}
    for pattern_label, pattern_score in skeletons:
        pattern = str(pattern_label)
        query = dict(row)
        query["skeleton"] = pattern
        templates = support_rank(
            query, template_support, pattern, seeds_per_skeleton,
            alpha, min_count, per_level)
        for seed_rank, (template_label, template_score) in enumerate(templates):
            template = tuple(int(value) for value in template_label)
            joint = pattern_score + template_score - 0.02 * seed_rank
            key = (pattern, template)
            if key not in seeds or joint > seeds[key][0]:
                seeds[key] = (joint, "seed")
            if seed_rank == 0:
                mandatory[key] = (joint, "seed")
            if seed_rank >= mutation_seed_count:
                continue
            for mutated, delta_score in mutation_candidates(
                    row, pattern, template, delta_support,
                    mutation_alternatives, alpha, min_count, per_level):
                mutation_score = joint + 0.10 * delta_score - 0.35
                mutation_key = (pattern, mutated)
                if mutation_key not in mutations or mutation_score > mutations[mutation_key][0]:
                    mutations[mutation_key] = (mutation_score, "mutation")

    def ordered(values):
        return sorted(
            ((pattern, template, score, origin)
             for (pattern, template), (score, origin) in values.items()),
            key=lambda item: (-item[2], item[0], item[1]),
        )

    selected_mandatory = ordered(mandatory)[:beam]
    selected_keys = {(pattern, template) for pattern, template, _, _ in selected_mandatory}
    selected_mutations = [
        item for item in ordered(mutations)
        if (item[0], item[1]) not in selected_keys
    ][:min(mutation_slots, beam - len(selected_mandatory))]
    selected_keys.update((pattern, template) for pattern, template, _, _ in selected_mutations)
    selected_seeds = [
        item for item in ordered(seeds)
        if (item[0], item[1]) not in selected_keys
    ][:beam - len(selected_mandatory) - len(selected_mutations)]
    result = selected_mandatory + selected_seeds + selected_mutations
    result.sort(key=lambda item: (-item[2], item[0], item[1]))
    return result


def cap_rows_per_source(
    rows: list[dict[str, object]], limit: int,
) -> list[dict[str, object]]:
    if limit <= 0:
        return rows
    groups: dict[str, list[dict[str, object]]] = defaultdict(list)
    for row in rows:
        groups[str(row["source_endpoint"])].append(row)
    result = []
    for source in sorted(groups):
        ranked = sorted(
            groups[source],
            key=lambda row: (
                fnv1a(str(row["from"]) + "\0" + str(row["to"])),
                str(row["to"]),
            ),
        )
        result.extend(ranked[:limit])
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--summaries", type=Path, required=True)
    parser.add_argument("--training-summaries", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--candidates-output", type=Path, default=None)
    parser.add_argument("--requests-output", type=Path, default=None)
    parser.add_argument("--transitions", type=Path, default=None)
    parser.add_argument("--axis-output", type=Path, default=None)
    parser.add_argument("--axis-radius", type=int, default=32)
    parser.add_argument("--axis-search-radius", type=int, default=64)
    parser.add_argument("--pricer-model-output", type=Path, default=None)
    parser.add_argument("--beam", type=int, default=32)
    parser.add_argument("--skeleton-limit", type=int, default=8)
    parser.add_argument("--seeds-per-skeleton", type=int, default=16)
    parser.add_argument("--mutation-seed-count", type=int, default=2)
    parser.add_argument("--mutation-alternatives", type=int, default=4)
    parser.add_argument("--mutation-slots", type=int, default=12)
    parser.add_argument("--alpha", type=float, default=32.0)
    parser.add_argument("--min-count", type=int, default=2)
    parser.add_argument("--per-level", type=int, default=64)
    parser.add_argument("--validation-modulus", type=int, default=5)
    parser.add_argument("--validation-fold", type=int, default=0)
    parser.add_argument("--exclude-validation-fold-from-training", action="store_true")
    parser.add_argument(
        "--coarse-only", action="store_true",
        help="drop exact dx/dy lookup levels and use macro distance bands",
    )
    parser.add_argument(
        "--max-rows-per-source", type=int, default=0,
        help="deterministically cap each complete source endpoint for fast ablations",
    )
    args = parser.parse_args()

    if args.validation_modulus <= 1:
        parser.error("--validation-modulus must be greater than one")
    if not 0 <= args.validation_fold < args.validation_modulus:
        parser.error("--validation-fold must be in [0, validation-modulus)")
    if args.axis_radius <= 0 or args.axis_search_radius < args.axis_radius:
        parser.error("--axis-search-radius must cover a positive --axis-radius")

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    if args.pricer_model_output is not None:
        compact_keys = (
            "width", "height", "port_names", "port_to_input", "input_to_state",
            "output_nets", "direct_arcs", "target_arcs", "gaps", "blocks",
        )
        args.pricer_model_output.parent.mkdir(parents=True, exist_ok=True)
        args.pricer_model_output.write_text(
            json.dumps({key: model[key] for key in compact_keys}, ensure_ascii=False) + "\n",
            encoding="utf-8")
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    training = load_summary_rows(args.training_summaries, model, port_id)
    all_rows = load_summary_rows(args.summaries, model, port_id)
    training = cap_rows_per_source(training, args.max_rows_per_source)
    all_rows = cap_rows_per_source(all_rows, args.max_rows_per_source)
    if args.exclude_validation_fold_from_training:
        training = [
            row for row in training
            if fnv1a(str(row["source_endpoint"])) % args.validation_modulus !=
            args.validation_fold
        ]
    validation = [
        row for row in all_rows
        if fnv1a(str(row["source_endpoint"])) % args.validation_modulus ==
        args.validation_fold and not row["block"]
    ]
    if args.coarse_only:
        selector_levels = (
            ("source_port", "target_port", "direction", "macro_band"),
            ("source_stem", "target_stem", "direction", "macro_band"),
            ("source_port", "direction", "macro_band"),
            ("target_port", "direction", "macro_band"),
            ("source_stem", "direction", "macro_band"),
            ("target_stem", "direction", "macro_band"),
            ("direction", "macro_band"),
            ("direction",),
        )
        template_levels = tuple(names + ("skeleton",) for names in selector_levels)
    else:
        selector_levels = SELECTOR_LEVELS
        template_levels = TEMPLATE_LEVELS
    skeleton_libraries = selector(training, selector_levels)
    template_support = template_model(training, template_levels)
    delta_support = delta_model(training, "macro_band" if args.coarse_only else "band")

    if (args.transitions is None) != (args.axis_output is None):
        parser.error("--transitions and --axis-output must be supplied together")
    if args.axis_output is not None:
        horizontal = build_axis_table(
            args.transitions, "H", args.axis_radius, args.axis_search_radius)
        vertical = build_axis_table(
            args.transitions, "V", args.axis_radius, args.axis_search_radius)
        args.axis_output.parent.mkdir(parents=True, exist_ok=True)
        with args.axis_output.open("wb") as stream:
            horizontal.tofile(stream)
            vertical.tofile(stream)

    skeleton_hits = template_hits = mutation_hits = 0
    candidate_total = mutation_total = 0
    misses = []
    exported = []
    for row in validation:
        candidates = propose(
            row, skeleton_libraries, template_support, delta_support,
            args.beam, args.skeleton_limit, args.seeds_per_skeleton,
            args.mutation_seed_count, args.mutation_alternatives,
            args.mutation_slots, args.alpha, args.min_count, args.per_level)
        actual_pattern = str(row["skeleton"])
        actual_template = normalized_template(
            actual_pattern, tuple(row["continuation_deltas"]))
        skeleton_hits += any(pattern == actual_pattern for pattern, _, _, _ in candidates)
        matching = [
            item for item in candidates
            if item[0] == actual_pattern and item[1] == actual_template
        ]
        template_hits += bool(matching)
        mutation_hits += bool(matching and matching[0][3] == "mutation")
        candidate_total += len(candidates)
        mutation_total += sum(item[3] == "mutation" for item in candidates)
        if args.candidates_output is not None:
            encoded = []
            for pattern, template, _, origin in candidates:
                encoded.append(
                    pattern + "@" + ":".join(str(value) for value in template) +
                    ("M" if origin == "mutation" else "S")
                )
            exported.append((row["from"], row["to"], row["golden"], ";".join(encoded)))
        if not matching and len(misses) < 20:
            misses.append({
                "from": row["from"], "to": row["to"],
                "skeleton": actual_pattern, "template": actual_template,
                "candidate_skeletons": list(dict.fromkeys(item[0] for item in candidates)),
            })

    denominator = max(len(validation), 1)
    report = {
        "definition": "support-aware joint skeleton/template global Beam with single-run mutations",
        "training_rows": len(training),
        "validation_rows_no_block": len(validation),
        "beam": args.beam,
        "skeleton_limit": args.skeleton_limit,
        "seeds_per_skeleton": args.seeds_per_skeleton,
        "mutation_seed_count": args.mutation_seed_count,
        "mutation_alternatives": args.mutation_alternatives,
        "mutation_slots": args.mutation_slots,
        "alpha": args.alpha,
        "min_count": args.min_count,
        "validation_modulus": args.validation_modulus,
        "validation_fold": args.validation_fold,
        "excluded_validation_fold_from_training": args.exclude_validation_fold_from_training,
        "coarse_only": args.coarse_only,
        "max_rows_per_source": args.max_rows_per_source,
        "mean_candidates": candidate_total / denominator,
        "mean_mutations": mutation_total / denominator,
        "skeleton_covered_by_global_beam": skeleton_hits / denominator,
        "exact_teacher_template_recall": template_hits / denominator,
        "recalled_by_mutation": mutation_hits / denominator,
        "miss_examples": misses,
        "warning": "Template identity recall is conservative; exact pricing may find an equal-delay alternative.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    if args.candidates_output is not None:
        args.candidates_output.parent.mkdir(parents=True, exist_ok=True)
        with args.candidates_output.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("From", "To", "Golden", "Candidates"))
            writer.writerows(exported)
    if args.requests_output is not None:
        args.requests_output.parent.mkdir(parents=True, exist_ok=True)
        with args.requests_output.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("From", "To"))
            writer.writerows((source, target) for source, target, _, _ in exported)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
