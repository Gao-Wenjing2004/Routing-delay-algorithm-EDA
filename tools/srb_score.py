#!/usr/bin/env python3
"""Parameterized SRB contest scoring and strict CSV comparison.

The positive-Golden formula and score weights come from the V0.2 contest PDF.
The PDF does not define zero-Golden behavior, invalid-row handling, or time-score
clipping, so those choices are explicit parameters instead of hidden policy.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence

import numpy as np


@dataclass(frozen=True)
class ScoreContract:
    accuracy_weight: float = 0.80
    time_weight: float = 0.15
    consistency_weight: float = 0.05
    tanh_coefficient: float = 4.0
    limit_time_seconds: float = 1800.0
    run_count: int = 5
    zero_golden_policy: str = "exact_zero"
    clip_time_score: bool = False
    invalid_run_threshold: int = 2

    def validate(self) -> None:
        weights = self.accuracy_weight + self.time_weight + self.consistency_weight
        if not math.isclose(weights, 1.0, rel_tol=0.0, abs_tol=1e-12):
            raise ValueError(f"score weights must sum to 1, got {weights}")
        if self.tanh_coefficient <= 0 or self.limit_time_seconds <= 0:
            raise ValueError("tanh coefficient and time limit must be positive")
        if self.run_count <= 0 or not (1 <= self.invalid_run_threshold <= self.run_count):
            raise ValueError("invalid run-count contract")
        if self.zero_golden_policy not in {"exact_zero", "error"}:
            raise ValueError(f"unknown zero-Golden policy: {self.zero_golden_policy}")


DEFAULT_CONTRACT = ScoreContract()


def point_scores(
    golden: np.ndarray,
    predicted: np.ndarray,
    contract: ScoreContract = DEFAULT_CONTRACT,
) -> np.ndarray:
    contract.validate()
    g = np.asarray(golden, dtype=np.float64)
    p = np.asarray(predicted, dtype=np.float64)
    if g.shape != p.shape:
        raise ValueError(f"shape mismatch: golden={g.shape}, predicted={p.shape}")
    if np.any(~np.isfinite(g)) or np.any(~np.isfinite(p)):
        raise ValueError("non-finite delay in scoring input")
    if np.any(g < 0):
        raise ValueError("negative Golden delay is unsupported")

    result = np.empty(g.shape, dtype=np.float64)
    positive = g > 0
    relative = np.zeros(g.shape, dtype=np.float64)
    np.divide(np.abs(p - g), g, out=relative, where=positive)
    result[positive] = 1.0 - np.tanh(contract.tanh_coefficient * relative[positive])
    if np.any(~positive):
        if contract.zero_golden_policy == "error":
            raise ValueError("official formula is undefined for zero Golden delay")
        result[~positive] = (p[~positive] == 0).astype(np.float64)
    return result


def accuracy_metrics(
    golden: np.ndarray,
    predicted: np.ndarray,
    contract: ScoreContract = DEFAULT_CONTRACT,
) -> dict[str, float | int]:
    g = np.asarray(golden, dtype=np.float64)
    p = np.asarray(predicted, dtype=np.float64)
    scores = point_scores(g, p, contract)
    error = np.abs(p - g)
    positive = g > 0
    relative = np.divide(
        error[positive],
        g[positive],
        out=np.zeros(int(positive.sum()), dtype=np.float64),
        where=g[positive] != 0,
    )
    q = np.percentile(error, [50, 90, 95, 99]) if error.size else np.full(4, np.nan)
    return {
        "rows": int(g.size),
        "acc_score": float(scores.mean() * 100.0) if scores.size else math.nan,
        "mae": float(error.mean()) if error.size else math.nan,
        "rmse": float(np.sqrt(np.mean(np.square(error)))) if error.size else math.nan,
        "mean_relative_error": float(relative.mean()) if relative.size else math.nan,
        "median_relative_error": float(np.median(relative)) if relative.size else math.nan,
        "p50_absolute_error": float(q[0]),
        "p90_absolute_error": float(q[1]),
        "p95_absolute_error": float(q[2]),
        "p99_absolute_error": float(q[3]),
        "max_absolute_error": float(error.max()) if error.size else math.nan,
        "exact_matches": int(np.count_nonzero(p == g)),
        "within_5pct": float(np.mean(relative <= 0.05)) if relative.size else math.nan,
        "within_10pct": float(np.mean(relative <= 0.10)) if relative.size else math.nan,
    }


def time_score(
    average_seconds: float,
    contract: ScoreContract = DEFAULT_CONTRACT,
) -> float:
    contract.validate()
    if not math.isfinite(average_seconds) or average_seconds < 0:
        raise ValueError("average runtime must be finite and non-negative")
    value = 100.0 * (1.0 - average_seconds / contract.limit_time_seconds)
    if contract.clip_time_score:
        value = min(100.0, max(0.0, value))
    return value


def total_score(
    accuracy_score: float,
    average_seconds: float,
    consistent: bool,
    invalid_runs: int = 0,
    contract: ScoreContract = DEFAULT_CONTRACT,
) -> dict[str, float | int | bool]:
    contract.validate()
    if invalid_runs < 0 or invalid_runs > contract.run_count:
        raise ValueError("invalid_runs is outside the configured run count")
    disqualified = invalid_runs >= contract.invalid_run_threshold
    tscore = time_score(average_seconds, contract)
    cscore = 100.0 if consistent else 0.0
    combined = (
        contract.accuracy_weight * accuracy_score
        + contract.time_weight * tscore
        + contract.consistency_weight * cscore
    )
    if disqualified:
        combined = 0.0
    return {
        "acc_score": float(accuracy_score),
        "time_score": float(tscore),
        "consistency_score": float(cscore),
        "estimated_total_score": float(combined),
        "average_seconds": float(average_seconds),
        "consistent": bool(consistent),
        "invalid_runs": int(invalid_runs),
        "disqualified": bool(disqualified),
    }


def _delay_column(fieldnames: Sequence[str] | None) -> str:
    for name in fieldnames or ():
        if name.strip().lower() in {"delay", "min delay", "predict_delay"}:
            return name
    raise ValueError("CSV does not contain a delay column")


def load_aligned_csv(golden_path: Path, prediction_path: Path) -> tuple[np.ndarray, np.ndarray]:
    golden_values: list[int] = []
    prediction_values: list[int] = []
    with golden_path.open("r", encoding="utf-8-sig", newline="") as gf, prediction_path.open(
        "r", encoding="utf-8-sig", newline=""
    ) as pf:
        golden_reader = csv.DictReader(gf)
        prediction_reader = csv.DictReader(pf)
        golden_delay = _delay_column(golden_reader.fieldnames)
        prediction_delay = _delay_column(prediction_reader.fieldnames)
        if not golden_reader.fieldnames or not prediction_reader.fieldnames:
            raise ValueError("CSV header is missing")
        for line_number, pair in enumerate(zip(golden_reader, prediction_reader), 2):
            golden_row, prediction_row = pair
            if golden_row.get("From") != prediction_row.get("From") or golden_row.get(
                "To"
            ) != prediction_row.get("To"):
                raise ValueError(f"endpoint/order mismatch at line {line_number}")
            golden_values.append(int(golden_row[golden_delay]))
            prediction_values.append(int(prediction_row[prediction_delay]))
        try:
            next(golden_reader)
        except StopIteration:
            pass
        else:
            raise ValueError("prediction CSV has fewer rows than Golden CSV")
        try:
            next(prediction_reader)
        except StopIteration:
            pass
        else:
            raise ValueError("prediction CSV has more rows than Golden CSV")
    return np.asarray(golden_values, dtype=np.float64), np.asarray(
        prediction_values, dtype=np.float64
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Strict SRB prediction scoring")
    parser.add_argument("golden", type=Path)
    parser.add_argument("prediction", type=Path)
    parser.add_argument("--average-seconds", type=float)
    parser.add_argument("--inconsistent", action="store_true")
    parser.add_argument("--invalid-runs", type=int, default=0)
    parser.add_argument("--zero-golden-policy", choices=("exact_zero", "error"), default="exact_zero")
    parser.add_argument("--clip-time-score", action="store_true")
    parser.add_argument("--json-out", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    contract = ScoreContract(
        zero_golden_policy=args.zero_golden_policy,
        clip_time_score=args.clip_time_score,
    )
    golden, predicted = load_aligned_csv(args.golden, args.prediction)
    result: dict[str, object] = {
        "contract": asdict(contract),
        "accuracy": accuracy_metrics(golden, predicted, contract),
    }
    if args.average_seconds is not None:
        result["score"] = total_score(
            float(result["accuracy"]["acc_score"]),  # type: ignore[index]
            args.average_seconds,
            not args.inconsistent,
            args.invalid_runs,
            contract,
        )
    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)
    if args.json_out is not None:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(rendered + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

