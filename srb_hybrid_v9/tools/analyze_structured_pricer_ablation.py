#!/usr/bin/env python3
"""Compare structured-pricer beams with V8 and audit label-free selectors.

The selector uses only endpoint/candidate features available before pricing.
All selector numbers are out-of-fold: four endpoint-hash folds train a bucket
decision and the fifth fold evaluates it.  This avoids reporting an oracle
choice made after seeing Golden or the priced result.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import re
import struct
from collections import defaultdict
from pathlib import Path
from statistics import fmean


ENDPOINT = re.compile(r"SRB_(\d+)_(\d+)/(.*)")
BUS = re.compile(r"\[\d+\]")


def point_score(golden: int, predicted: int) -> float:
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def fnv1a(text: str) -> int:
    value = 2166136261
    for byte in text.encode("utf-8"):
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def fnv1a64_values(values: tuple[object, ...]) -> int:
    result = 14695981039346656037
    for value in values:
        for byte in str(value).encode("utf-8"):
            result ^= byte
            result = (result * 1099511628211) & 0xFFFFFFFFFFFFFFFF
        result ^= 0xFF
        result = (result * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return result


def export_runtime_selector(rows: list[dict[str, object]], path: Path) -> int:
    scheme = ("source_stem", "direction", "band")
    groups: dict[tuple[object, ...], list[float]] = defaultdict(list)
    for row in rows:
        groups[tuple(row[name] for name in scheme)].append(float(row["utility"]))
    accepted = [
        key for key, values in groups.items()
        if len(values) >= 4 and fmean(values) > 0.0
    ]
    hashes = sorted(fnv1a64_values((str(key[0]).replace("[]", "[*]"), key[1], key[2]))
                    for key in accepted)
    if len(hashes) != len(set(hashes)):
        raise ValueError("runtime selector hash collision")
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        stream.write(b"P6SEL001")
        stream.write(struct.pack("<I", len(hashes)))
        stream.write(struct.pack("<" + "Q" * len(hashes), *hashes))
    return len(hashes)


def endpoint(text: str) -> tuple[int, int, str]:
    match = ENDPOINT.fullmatch(text)
    if match is None:
        raise ValueError(f"bad endpoint: {text}")
    return int(match[1]), int(match[2]), match[3]


def displacement_band(distance: int) -> str:
    for boundary in (2, 4, 8, 16):
        if distance <= boundary:
            return str(boundary)
    return "17+"


def delay_band(delay: int) -> str:
    for boundary in (256, 384, 512, 768, 1024):
        if delay <= boundary:
            return str(boundary)
    return "1025+"


def run_count(encoded: str) -> int:
    pattern = encoded.split("@", 1)[0]
    return 0 if pattern == "identity" else pattern.count(">") + 1


def load_v8(path: Path) -> dict[tuple[str, str], int]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        return {
            (row["From"], row["To"]): int(row["Delay"])
            for row in csv.DictReader(stream)
        }


def load_candidates(path: Path) -> dict[tuple[str, str], list[str]]:
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        return {
            (row["From"], row["To"]): row["Candidates"].split(";")
            for row in csv.DictReader(stream)
        }


def enrich(raw: dict[str, str], v8: int, candidates: list[str]) -> dict[str, object]:
    sx, sy, source_port = endpoint(raw["From"])
    tx, ty, target_port = endpoint(raw["To"])
    dx, dy = tx - sx, ty - sy
    patterns = [item.split("@", 1)[0] for item in candidates]
    runs = [run_count(item) for item in candidates]
    direction = ("E" if dx > 0 else "W" if dx < 0 else "0") + \
        ("N" if dy > 0 else "S" if dy < 0 else "0")
    distance = max(abs(dx), abs(dy))
    macro_band = "0-16" if distance <= 16 else "17-32" if distance <= 32 else \
        "33-64" if distance <= 64 else "65-128" if distance <= 128 else "129+"
    return {
        "source_stem": BUS.sub("[]", source_port),
        "target_stem": BUS.sub("[]", target_port),
        "source_port": source_port,
        "target_port": target_port,
        "direction": direction,
        "band": displacement_band(distance),
        "macro_band": macro_band,
        "dx": dx,
        "dy": dy,
        "x10": sx % 10,
        "y12": sy % 12,
        "first_pattern": patterns[0],
        "first_runs": runs[0],
        "min_runs": min(runs),
        "max_runs": max(runs),
        "mean_runs_half": round(2.0 * fmean(runs)) / 2.0,
        "unique_patterns": len(set(patterns)),
        "v8_band": delay_band(v8),
    }


SCHEMES = (
    ("first_pattern", "band"),
    ("first_runs", "mean_runs_half", "band"),
    ("direction", "band", "first_pattern"),
    ("source_stem", "direction", "band"),
    ("source_stem", "direction", "macro_band"),
    ("target_stem", "direction", "band"),
    ("source_stem", "target_stem", "direction"),
    ("source_stem", "target_stem", "direction", "band"),
    ("source_stem", "target_stem", "first_pattern", "band"),
    ("source_stem", "target_stem", "direction", "v8_band"),
    ("direction", "band", "first_pattern", "v8_band"),
    ("direction", "band", "first_pattern", "x10", "y12"),
)


def selector_audits(
    rows: list[dict[str, object]],
    schemes: tuple[tuple[str, ...], ...] = SCHEMES,
) -> list[dict[str, object]]:
    result = []
    for scheme in schemes:
        for minimum_count in (4, 8, 16, 32):
            for margin in (0.0, 2.0, 5.0):
                selected: list[dict[str, object]] = []
                for fold in range(5):
                    groups: dict[tuple[object, ...], list[float]] = defaultdict(list)
                    for row in rows:
                        if row["fold"] == fold:
                            continue
                        groups[tuple(row[name] for name in scheme)].append(float(row["utility"]))
                    accepted = {
                        key for key, values in groups.items()
                        if len(values) >= minimum_count and fmean(values) > margin
                    }
                    selected.extend(
                        row for row in rows
                        if row["fold"] == fold and
                        tuple(row[name] for name in scheme) in accepted
                    )
                total_utility = sum(float(row["utility"]) for row in selected)
                result.append({
                    "scheme": "+".join(scheme),
                    "minimum_count": minimum_count,
                    "margin": margin,
                    "selected_rows": len(selected),
                    "selected_rate": len(selected) / len(rows),
                    "accuracy_gain_selected_points": (
                        100.0 * fmean(float(row["gain"]) for row in selected)
                        if selected else 0.0),
                    "mean_us_selected": (
                        fmean(float(row["elapsed_us"]) for row in selected)
                        if selected else 0.0),
                    "utility_per_eligible_row": total_utility / len(rows),
                })
    result.sort(key=lambda row: (-float(row["utility_per_eligible_row"]), str(row["scheme"])))
    return result


def external_selector_audits(
    training: list[dict[str, object]], evaluation: list[dict[str, object]],
) -> list[dict[str, object]]:
    """Train each bucket selector on independent Teacher rows and test public rows."""
    result = []
    for scheme in SCHEMES:
        for minimum_count in (4, 8, 16, 32):
            for margin in (0.0, 2.0, 5.0):
                groups: dict[tuple[object, ...], list[float]] = defaultdict(list)
                for row in training:
                    groups[tuple(row[name] for name in scheme)].append(float(row["utility"]))
                accepted = {
                    key for key, values in groups.items()
                    if len(values) >= minimum_count and fmean(values) > margin
                }
                selected = [
                    row for row in evaluation
                    if tuple(row[name] for name in scheme) in accepted
                ]
                total_utility = sum(float(row["utility"]) for row in selected)
                result.append({
                    "scheme": "+".join(scheme),
                    "minimum_count": minimum_count,
                    "margin": margin,
                    "accepted_buckets": len(accepted),
                    "selected_rows": len(selected),
                    "selected_rate": len(selected) / len(evaluation),
                    "accuracy_gain_selected_points": (
                        100.0 * fmean(float(row["gain"]) for row in selected)
                        if selected else 0.0),
                    "mean_us_selected": (
                        fmean(float(row["elapsed_us"]) for row in selected)
                        if selected else 0.0),
                    "utility_per_eligible_row": total_utility / len(evaluation),
                })
    result.sort(key=lambda row: (-float(row["utility_per_eligible_row"]), str(row["scheme"])))
    return result


def load_audit_rows(
    path: Path, v8: dict[tuple[str, str], int],
    candidates: dict[tuple[str, str], list[str]],
) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    with path.open("r", encoding="utf-8-sig", newline="") as stream:
        for raw in csv.DictReader(stream):
            key = (raw["From"], raw["To"])
            baseline = v8[key]
            golden = int(raw["Golden"])
            structured = int(raw["Predicted"])
            hybrid = structured if structured >= 0 else baseline
            baseline_score = point_score(golden, baseline)
            hybrid_score = point_score(golden, hybrid)
            gain = hybrid_score - baseline_score
            elapsed_us = float(raw["ElapsedUs"])
            generate_us = float(raw.get("GenerateUs", 0.0))
            row = enrich(raw, baseline, candidates[key][:int(raw["Candidates"])])
            row.update({
                "baseline_score": baseline_score,
                "hybrid_score": hybrid_score,
                "gain": gain,
                "elapsed_us": elapsed_us,
                "generate_us": generate_us,
                "utility": 80.0 * gain - (100.0 / 120.0) * elapsed_us,
                # Keep every query sharing a complete source endpoint in one
                # fold.  This matches the Dijkstra Teacher split and avoids
                # leaking source-local path regularities across selector folds.
                "fold": fnv1a(raw["From"]) % 5,
            })
            rows.append(row)
    return rows


def audit_one(
    label: str, path: Path, v8: dict[tuple[str, str], int],
    candidates: dict[tuple[str, str], list[str]], prevalence: float,
    selector_training: list[dict[str, object]] | None = None,
) -> dict[str, object]:
    rows = load_audit_rows(path, v8, candidates)
    v8_accuracy = 100.0 * fmean(float(row["baseline_score"]) for row in rows)
    mean_gain = fmean(float(row["gain"]) for row in rows)
    mean_us = fmean(float(row["elapsed_us"]) for row in rows)
    mean_generate_us = fmean(float(row["generate_us"]) for row in rows)
    # Keep the public audit on the predeclared selector family as well.  This
    # is both faster for repeated timing passes and avoids mining unrelated
    # feature families on the final public holdout.
    selectors = selector_audits(
        rows, (
            ("source_stem", "direction", "macro_band"),
            ("source_stem", "direction", "band"),
        ))
    best_selector = selectors[0]
    report = {
        "label": label,
        "rows": len(rows),
        "v8_accuracy": v8_accuracy,
        "hybrid_accuracy": v8_accuracy + 100.0 * mean_gain,
        "accuracy_gain_points": 100.0 * mean_gain,
        "mean_us": mean_us,
        "mean_generate_us": mean_generate_us,
        "utility_per_eligible_row": 80.0 * mean_gain - (100.0 / 120.0) * mean_us,
        "projected_total_score_delta_all_eligible": prevalence * (
            80.0 * mean_gain - (100.0 / 120.0) * mean_us),
        "oracle_positive_rows": sum(float(row["utility"]) > 0.0 for row in rows),
        "oracle_utility_per_eligible_row": sum(
            max(float(row["utility"]), 0.0) for row in rows) / len(rows),
        "best_out_of_fold_selector": best_selector,
        "projected_total_score_delta_best_selector": prevalence * float(
            best_selector["utility_per_eligible_row"]),
        "selector_top10": selectors[:10],
    }
    if selector_training is not None:
        # The bucket family is fixed before the public evaluation.  Only its
        # minimum support and safety margin are selected by Teacher folds.
        teacher_oof = selector_audits(
            selector_training, (("source_stem", "direction", "band"),))
        teacher_choice = teacher_oof[0]
        external = external_selector_audits(selector_training, rows)
        chosen_evaluation = next(
            item for item in external
            if item["scheme"] == teacher_choice["scheme"] and
            item["minimum_count"] == teacher_choice["minimum_count"] and
            item["margin"] == teacher_choice["margin"])
        report["teacher_oof_selected_configuration"] = teacher_choice
        report["independent_teacher_selected_public_evaluation"] = chosen_evaluation
        report["projected_total_score_delta_teacher_selected"] = (
            prevalence * float(chosen_evaluation["utility_per_eligible_row"]))
        report["exploratory_best_public_configuration_do_not_select"] = external[0]
        report["teacher_oof_top10"] = teacher_oof[:10]
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--v8", type=Path, required=True)
    parser.add_argument("--candidates", type=Path, required=True)
    parser.add_argument("--priced", action="append", required=True,
                        help="LABEL=CSV; repeat for every candidate limit")
    parser.add_argument("--eligible-prevalence", type=float, default=0.01448)
    parser.add_argument("--selector-training-v8", type=Path)
    parser.add_argument("--selector-training-candidates", type=Path)
    parser.add_argument("--selector-training-priced", type=Path)
    parser.add_argument("--selector-output", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    v8 = load_v8(args.v8)
    candidates = load_candidates(args.candidates)
    selector_options = (
        args.selector_training_v8, args.selector_training_candidates,
        args.selector_training_priced)
    if any(selector_options) and not all(selector_options):
        parser.error("all three --selector-training-* options must be supplied together")
    selector_training = None
    if all(selector_options):
        selector_training = load_audit_rows(
            args.selector_training_priced,
            load_v8(args.selector_training_v8),
            load_candidates(args.selector_training_candidates))
        if args.selector_output is not None:
            print(f"exported_selector_buckets={export_runtime_selector(selector_training, args.selector_output)}")
    elif args.selector_output is not None:
        parser.error("--selector-output requires all --selector-training-* options")
    audits = []
    for value in args.priced:
        label, separator, path = value.partition("=")
        if not separator:
            parser.error("--priced must be LABEL=CSV")
        audits.append(audit_one(
            label, Path(path), v8, candidates, args.eligible_prevalence,
            selector_training if label == "c8" else None))
    report = {
        "definition": "P6 structured-pricer accuracy/microsecond ablation",
        "eligible_prevalence_assumption": args.eligible_prevalence,
        "score_utility": "80 * mean point-score gain - (100/120) * mean added microseconds",
        "timing_note": "ElapsedUs is the measured branch total; GenerateUs is its candidate-generation subset when present.",
        "audits": audits,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
