#!/usr/bin/env python3
"""Generate public/evaluation P8 macro candidates from Teacher-only models.

The accepted selector buckets are learned only from cross-fitted Dijkstra
Teacher predictions.  Golden values supplied here are copied to the audit CSV
and are never used to rank candidates or choose buckets.
"""

from __future__ import annotations

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path
from statistics import fmean

from analyze_global_template_beam import (
    cap_rows_per_source,
    delta_model,
    propose,
    template_model,
)
from analyze_structured_generator import (
    band,
    direction,
    endpoint,
    load_summary_rows,
    selector,
)
from analyze_path_skeletons import stem
from analyze_structured_pricer_ablation import (
    load_audit_rows,
    load_candidates,
    load_v8,
)


SELECTOR_LEVELS = (
    ("source_port", "target_port", "direction", "macro_band"),
    ("source_stem", "target_stem", "direction", "macro_band"),
    ("source_port", "direction", "macro_band"),
    ("target_port", "direction", "macro_band"),
    ("source_stem", "direction", "macro_band"),
    ("target_stem", "direction", "macro_band"),
    ("direction", "macro_band"),
    ("direction",),
)
TEMPLATE_LEVELS = tuple(names + ("skeleton",) for names in SELECTOR_LEVELS)


def macro_band(distance: int) -> str:
    return "0-16" if distance <= 16 else "17-32" if distance <= 32 else \
        "33-64" if distance <= 64 else "65-128" if distance <= 128 else "129+"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--training-summaries", type=Path, required=True)
    parser.add_argument("--teacher-v8", type=Path, required=True)
    parser.add_argument("--teacher-candidates", type=Path, required=True)
    parser.add_argument("--teacher-priced", type=Path, required=True)
    parser.add_argument("--requests", type=Path, required=True)
    parser.add_argument("--golden", type=Path, required=True)
    parser.add_argument("--trace", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--minimum-count", type=int, default=4)
    parser.add_argument("--margin", type=float, default=5.0)
    parser.add_argument("--max-rows-per-source", type=int, default=64)
    parser.add_argument("--beam", type=int, default=32)
    args = parser.parse_args()

    model = json.loads(args.model.read_text(encoding="utf-8-sig"))
    port_id = {name: index for index, name in enumerate(model["port_names"])}
    training = cap_rows_per_source(
        load_summary_rows(args.training_summaries, model, port_id),
        args.max_rows_per_source,
    )
    skeleton_libraries = selector(training, SELECTOR_LEVELS)
    template_support = template_model(training, TEMPLATE_LEVELS)
    delta_support = delta_model(training, "macro_band")

    teacher = load_audit_rows(
        args.teacher_priced,
        load_v8(args.teacher_v8),
        load_candidates(args.teacher_candidates),
    )
    direction_id = {
        "00": 0, "E0": 1, "W0": 2, "0N": 3, "EN": 4,
        "WN": 5, "0S": 6, "ES": 7, "WS": 8,
    }
    grouped: dict[tuple[object, ...], list[float]] = defaultdict(list)
    for row in teacher:
        key = (
            str(row["source_stem"]).replace("[]", "[*]"),
            direction_id[str(row["direction"])],
            row["macro_band"],
        )
        grouped[key].append(float(row["utility"]))
    accepted = {
        key for key, values in grouped.items()
        if len(values) >= args.minimum_count and fmean(values) > args.margin
    }

    selected = eligible = rows = 0
    selected_by_band: dict[str, int] = defaultdict(int)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with (
        args.requests.open("r", encoding="utf-8-sig", newline="") as request_stream,
        args.golden.open("r", encoding="utf-8-sig", newline="") as golden_stream,
        args.trace.open("r", encoding="utf-8-sig", newline="") as trace_stream,
        args.output.open("w", encoding="utf-8", newline="") as output_stream,
    ):
        request_reader = csv.DictReader(request_stream)
        golden_reader = csv.DictReader(golden_stream)
        trace_reader = csv.DictReader(trace_stream)
        writer = csv.writer(output_stream)
        writer.writerow(("From", "To", "Golden", "Candidates"))
        for request, golden, trace in zip(
            request_reader, golden_reader, trace_reader, strict=True
        ):
            rows += 1
            if request["From"] != golden["From"] or request["To"] != golden["To"]:
                raise ValueError(f"request/Golden mismatch at row {rows}")
            sx, sy, source_port = endpoint(request["From"])
            tx, ty, target_port = endpoint(request["To"])
            dx, dy = tx - sx, ty - sy
            cheb = max(abs(dx), abs(dy))
            if not 17 <= cheb <= 64 or int(trace["block_count"]) != 0:
                continue
            eligible += 1
            item = {
                "from": request["From"], "to": request["To"],
                "source_endpoint": request["From"], "source_port": source_port,
                "target_port": target_port, "source_stem": stem(source_port),
                "target_stem": stem(target_port), "direction": direction(dx, dy),
                "band": band(cheb), "macro_band": macro_band(cheb),
                "dx": dx, "dy": dy,
            }
            key = (item["source_stem"], item["direction"], item["macro_band"])
            if key not in accepted:
                continue
            candidates = propose(
                item, skeleton_libraries, template_support, delta_support,
                args.beam, 8, 16, 2, 4, 12, 32.0, 2, 64,
            )
            encoded = [
                pattern + "@" + ":".join(str(value) for value in template) +
                ("M" if origin == "mutation" else "S")
                for pattern, template, _, origin in candidates
            ]
            if not encoded:
                continue
            delay_key = next(key for key in golden if key.lower() == "delay")
            writer.writerow((request["From"], request["To"], golden[delay_key], ";".join(encoded)))
            selected += 1
            selected_by_band[macro_band(cheb)] += 1

    report = {
        "definition": "P8 Teacher-only macro candidate export",
        "rows": rows,
        "eligible_no_block_17_64": eligible,
        "accepted_selector_buckets": len(accepted),
        "selector_scheme": ["source_stem", "direction", "macro_band"],
        "minimum_count": args.minimum_count,
        "margin": args.margin,
        "selected": selected,
        "selected_rate_of_all": selected / max(rows, 1),
        "selected_rate_of_eligible": selected / max(eligible, 1),
        "selected_by_band": dict(sorted(selected_by_band.items())),
        "candidate_beam": args.beam,
        "golden_usage": "copied to audit output only; not used for selection or ranking",
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
