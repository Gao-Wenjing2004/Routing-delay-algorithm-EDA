#!/usr/bin/env python3
"""Leakage-resistant evaluation for unique SRB routing-delay requests.

The tool deliberately separates four jobs:

* ``generate`` creates deterministic, non-repeating request pairs;
* ``label`` invokes the existing unbounded Dijkstra teacher in source shards;
* ``shuffle`` externally scatters grouped rows for locality-neutral timing;
* ``merge`` combines source-disjoint Teacher/prediction panels;
* ``benchmark`` measures the submitted estimator on unique streams;
* ``evaluate`` scores aligned predictions and reports cluster-bootstrap bounds.

Only Python's standard library is required.  Large request/label files are
streamed; memory use depends on the endpoint pool and the number of groups, not
on the number of scored rows.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import os
import platform
import random
import re
import statistics
import subprocess
import sys
import tempfile
import time
from collections import Counter, defaultdict
from dataclasses import asdict, dataclass
from itertools import zip_longest
from pathlib import Path
from typing import Iterable, Iterator, Sequence


ENDPOINT_RE = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
DISTANCE_BINS = ((0, 16), (17, 32), (33, 64), (65, 128), (129, 256), (257, 10**9))
DISTANCE_NAMES = ("0-16", "17-32", "33-64", "65-128", "129-256", "257+")


@dataclass(frozen=True)
class Endpoint:
    text: str
    x: int
    y: int
    port: str


@dataclass
class Metric:
    rows: int = 0
    score_sum: float = 0.0
    absolute_sum: float = 0.0
    squared_sum: float = 0.0
    relative_sum: float = 0.0
    relative_rows: int = 0
    exact: int = 0
    within_5pct: int = 0
    within_10pct: int = 0

    def add(self, golden: int, predicted: int) -> float:
        point = point_score(golden, predicted)
        error = abs(predicted - golden)
        self.rows += 1
        self.score_sum += point
        self.absolute_sum += error
        self.squared_sum += error * error
        self.exact += int(predicted == golden)
        if golden > 0:
            relative = error / golden
            self.relative_sum += relative
            self.relative_rows += 1
            self.within_5pct += int(relative <= 0.05)
            self.within_10pct += int(relative <= 0.10)
        return point

    def result(self) -> dict[str, float | int]:
        if not self.rows:
            return {"rows": 0, "accuracy": math.nan}
        return {
            "rows": self.rows,
            "accuracy": 100.0 * self.score_sum / self.rows,
            "mae_ps": self.absolute_sum / self.rows,
            "rmse_ps": math.sqrt(self.squared_sum / self.rows),
            "mean_relative_error": (
                self.relative_sum / self.relative_rows if self.relative_rows else math.nan
            ),
            "exact_share_pct": 100.0 * self.exact / self.rows,
            "within_5pct_pct": (
                100.0 * self.within_5pct / self.relative_rows if self.relative_rows else math.nan
            ),
            "within_10pct_pct": (
                100.0 * self.within_10pct / self.relative_rows if self.relative_rows else math.nan
            ),
        }


@dataclass(frozen=True)
class Architecture:
    horizontal_gaps: tuple[int, ...] = ()
    vertical_gaps: tuple[int, ...] = ()
    blocks: tuple[tuple[int, int, int, int], ...] = ()  # left, right, lower, upper


def parse_endpoint(value: str) -> Endpoint:
    match = ENDPOINT_RE.match(value.strip())
    if not match:
        raise ValueError(f"bad endpoint: {value!r}")
    return Endpoint(value.strip(), int(match.group(1)), int(match.group(2)), match.group(3))


def stable_u64(value: str, seed: int) -> int:
    payload = f"{seed}\0{value}".encode("utf-8")
    return int.from_bytes(hashlib.blake2b(payload, digest_size=8).digest(), "little")


def distance_bin(distance: int) -> int:
    for index, (lower, upper) in enumerate(DISTANCE_BINS):
        if lower <= distance <= upper:
            return index
    raise AssertionError(distance)


def load_architecture(path: Path | None) -> Architecture:
    if path is None:
        return Architecture()
    root = json.loads(path.read_text(encoding="utf-8-sig"))["Gap"]
    horizontal: list[int] = []
    vertical: list[int] = []
    for row in root.get("Line", []):
        (vertical if row["direction"] == "vertical" else horizontal).append(int(row["site"]))
    blocks = [
        (int(row["left"]), int(row["right"]), int(row["lower"]), int(row["upper"]))
        for row in root.get("Block", [])
    ]
    return Architecture(tuple(sorted(horizontal)), tuple(sorted(vertical)), tuple(blocks))


def between(value: int, first: int, second: int) -> bool:
    return min(first, second) <= value < max(first, second)


def environment(source: Endpoint, target: Endpoint, architecture: Architecture) -> tuple[int, int]:
    gap_count = sum(between(site, source.y, target.y) for site in architecture.horizontal_gaps)
    gap_count += sum(between(site, source.x, target.x) for site in architecture.vertical_gaps)
    min_x, max_x = sorted((source.x, target.x))
    min_y, max_y = sorted((source.y, target.y))
    block_count = sum(
        min_x <= right and max_x >= left and min_y <= upper and max_y >= lower
        for left, right, lower, upper in architecture.blocks
    )
    return int(block_count), int(gap_count)


def largest_remainder(total: int, weights: Sequence[float]) -> list[int]:
    if total < 0 or not weights or sum(weights) <= 0:
        raise ValueError("invalid quota request")
    scaled = [total * weight / sum(weights) for weight in weights]
    result = [int(math.floor(value)) for value in scaled]
    order = sorted(range(len(weights)), key=lambda index: (scaled[index] - result[index], -index), reverse=True)
    for index in order[: total - sum(result)]:
        result[index] += 1
    return result


def coprime_step(length: int, token: int) -> int:
    if length <= 1:
        return 1
    step = token % length
    if step == 0:
        step = 1
    while math.gcd(step, length) != 1:
        step += 1
        if step == length:
            step = 1
    return step


def choose_sources(candidates: Sequence[Endpoint], count: int, seed: int) -> list[Endpoint]:
    if count > len(candidates):
        raise ValueError(f"requested {count} sources from a pool of {len(candidates)}")
    ordered = sorted(candidates, key=lambda item: stable_u64(item.text, seed))
    chosen: list[Endpoint] = []
    used: set[str] = set()
    covered_ports: set[str] = set()
    covered_regions: set[tuple[int, int]] = set()
    # First pass maximizes structural coverage without an O(N * count) greedy loop.
    for source in ordered:
        region = (source.x // 20, source.y // 100)
        if source.port not in covered_ports or region not in covered_regions:
            chosen.append(source)
            used.add(source.text)
            covered_ports.add(source.port)
            covered_regions.add(region)
            if len(chosen) == count:
                return chosen
    for source in ordered:
        if source.text not in used:
            chosen.append(source)
            if len(chosen) == count:
                break
    return chosen


def pool_profile(path: Path) -> tuple[list[Endpoint], list[Endpoint], list[int], int]:
    sources: dict[str, Endpoint] = {}
    targets: dict[str, Endpoint] = {}
    counts = [0] * len(DISTANCE_BINS)
    rows = 0
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames or "From" not in reader.fieldnames or "To" not in reader.fieldnames:
            raise ValueError("pool CSV must contain From and To columns")
        for row in reader:
            source = parse_endpoint(row["From"])
            target = parse_endpoint(row["To"])
            sources.setdefault(source.text, source)
            targets.setdefault(target.text, target)
            counts[distance_bin(max(abs(target.x - source.x), abs(target.y - source.y)))] += 1
            rows += 1
    if not rows:
        raise ValueError("endpoint pool is empty")
    return list(sources.values()), list(targets.values()), counts, rows


def load_source_names(paths: Sequence[Path]) -> set[str]:
    result: set[str] = set()
    for path in paths:
        with path.open("r", encoding="utf-8-sig", newline="") as handle:
            reader = csv.DictReader(handle)
            if not reader.fieldnames or "From" not in reader.fieldnames:
                raise ValueError(f"source exclusion CSV has no From column: {path}")
            result.update(row["From"].strip() for row in reader)
    return result


def select_targets(
    source: Endpoint,
    targets: Sequence[Endpoint],
    count: int,
    quotas: Sequence[int],
    seed: int,
    profile: str,
    architecture: Architecture,
    collect_environment: bool = True,
) -> tuple[list[Endpoint], Counter[str]]:
    if count >= len(targets):
        raise ValueError("targets-per-source must be smaller than the unique target pool")
    length = len(targets)
    token = stable_u64(source.text, seed)
    offset = token % length
    step = coprime_step(length, token >> 17)
    selected: list[Endpoint] = []
    selected_indices: set[int] = set()
    remaining = list(quotas)
    stats: Counter[str] = Counter()

    def accept(index: int, target: Endpoint, enforce_quota: bool) -> bool:
        if target.text == source.text or index in selected_indices:
            return False
        distance = max(abs(target.x - source.x), abs(target.y - source.y))
        bucket = distance_bin(distance)
        if enforce_quota and remaining[bucket] <= 0:
            return False
        if profile == "stress" and enforce_quota and len(selected) < (3 * count) // 4:
            if distance < 129 or (architecture.blocks and blocks == 0 and gaps < 3):
                return False
        selected.append(target)
        selected_indices.add(index)
        if remaining[bucket] > 0:
            remaining[bucket] -= 1
        stats[f"distance:{DISTANCE_NAMES[bucket]}"] += 1
        if collect_environment:
            blocks, gaps = environment(source, target, architecture)
            stats[f"block_count:{min(blocks, 3)}{'+' if blocks >= 3 else ''}"] += 1
            stats[f"gap_count:{min(gaps, 5)}{'+' if gaps >= 5 else ''}"] += 1
        return True

    for position in range(length):
        index = (offset + position * step) % length
        accept(index, targets[index], True)
        if len(selected) == count:
            return selected, stats
    # Some short-distance quotas are impossible for edge sources. Fill from the
    # same permutation and record the actual distribution in the manifest.
    for position in range(length):
        index = (offset + position * step) % length
        accept(index, targets[index], False)
        if len(selected) == count:
            return selected, stats
    raise ValueError(f"not enough unique targets for {source.text}: {len(selected)}/{count}")


def splitmix64(value: int) -> int:
    """Fast deterministic integer mixer used only to scatter performance rows."""
    mask = (1 << 64) - 1
    value = (value + 0x9E3779B97F4A7C15) & mask
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & mask
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & mask
    return (value ^ (value >> 31)) & mask


def command_generate_perf(args: argparse.Namespace) -> int:
    """Generate unique, hash-scattered timing input without a grouped copy.

    Pair uniqueness follows from two invariants: sources are selected without
    replacement, and each source's affine target walk selects targets without
    replacement. Block/Gap categorization is intentionally omitted because it
    is irrelevant to a black-box throughput benchmark and costly at 100M.
    """
    if args.buckets < 2 or args.buckets > 4096:
        raise ValueError("buckets must be between 2 and 4096")
    started = time.perf_counter()
    sources, targets, reference_counts, reference_rows = pool_profile(args.pool)
    excluded_sources = load_source_names(args.exclude_sources_from)
    if excluded_sources:
        sources = [source for source in sources if source.text not in excluded_sources]
    if args.fold_count <= 0 or not (0 <= args.fold_index < args.fold_count):
        raise ValueError("fold-index must be in [0, fold-count)")
    folded = [
        source for source in sources
        if stable_u64(f"source-fold\0{source.text}", args.seed) % args.fold_count == args.fold_index
    ]
    chosen = choose_sources(folded, args.sources, args.seed)
    if args.profile == "official-like":
        weights = reference_counts
    elif args.profile == "balanced-distance":
        weights = [1] * len(DISTANCE_BINS)
    else:
        weights = [0, 0, 0, 0, 2, 3]
    quotas = largest_remainder(args.targets_per_source, weights)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    publish = args.output.with_suffix(args.output.suffix + ".partial")
    aggregate: Counter[str] = Counter()
    source_digest = hashlib.sha256()
    rows = 0
    with tempfile.TemporaryDirectory(prefix="unique_perf_", dir=str(args.output.parent)) as temp_name:
        temp = Path(temp_name)
        paths = [temp / f"bucket_{index:04d}.csv" for index in range(args.buckets)]
        # Keep aggregate buffering bounded: 1024 * 16 KiB = 16 MiB. An 8 MiB
        # buffer per bucket would silently request 8 GiB before writing a row.
        handles = [path.open("w", encoding="utf-8", newline="", buffering=16 << 10) for path in paths]
        bucket_first: list[str | None] = [None] * args.buckets
        bucket_last: list[str | None] = [None] * args.buckets
        bucket_run = [0] * args.buckets
        bucket_max_run = [0] * args.buckets
        adjacent_same_source = 0
        try:
            for source_index, source in enumerate(chosen):
                selected, stats = select_targets(
                    source, targets, args.targets_per_source, quotas, args.seed,
                    args.profile, Architecture(), collect_environment=False,
                )
                aggregate.update(stats)
                source_digest.update((source.text + "\n").encode("utf-8"))
                source_token = stable_u64(source.text, args.shuffle_seed)
                bucket_offset = splitmix64(source_token) % args.buckets
                bucket_step = coprime_step(args.buckets, splitmix64(source_token >> 1))
                for target_index, target in enumerate(selected):
                    # When buckets >= targets-per-source (the formal mode uses
                    # 1024 >= 1000), the affine walk puts at most one row from
                    # a source in each bucket.  This removes adjacent-source
                    # locality instead of merely shortening its runs.
                    bucket = (bucket_offset + target_index * bucket_step) % args.buckets
                    if bucket_first[bucket] is None:
                        bucket_first[bucket] = source.text
                    if bucket_last[bucket] == source.text:
                        adjacent_same_source += 1
                        bucket_run[bucket] += 1
                    else:
                        bucket_run[bucket] = 1
                    bucket_last[bucket] = source.text
                    bucket_max_run[bucket] = max(bucket_max_run[bucket], bucket_run[bucket])
                    handles[bucket].write(f"{source.text},{target.text}\n")
                    rows += 1
                if args.progress_sources and (source_index + 1) % args.progress_sources == 0:
                    print(
                        f"generate-perf sources={source_index + 1}/{len(chosen)} rows={rows}",
                        file=sys.stderr, flush=True,
                    )
        finally:
            for handle in handles:
                handle.close()
        digest = hashlib.sha256()
        with publish.open("wb", buffering=8 << 20) as destination:
            header = b"From,To\n"
            destination.write(header)
            digest.update(header)
            order = list(range(args.buckets))
            random.Random(args.shuffle_seed).shuffle(order)
            previous_last: str | None = None
            for index in order:
                if previous_last is not None and bucket_first[index] == previous_last:
                    adjacent_same_source += 1
                if bucket_last[index] is not None:
                    previous_last = bucket_last[index]
                with paths[index].open("rb", buffering=8 << 20) as source_handle:
                    while True:
                        block = source_handle.read(1 << 20)
                        if not block:
                            break
                        destination.write(block)
                        digest.update(block)
    expected_rows = len(chosen) * args.targets_per_source
    if rows != expected_rows:
        raise RuntimeError(f"generated {rows} rows, expected {expected_rows}")
    publish.replace(args.output)
    manifest = {
        "schema": 1,
        "kind": "unique-srb-shuffled-performance-requests",
        "pool": str(args.pool.resolve()),
        "pool_rows": reference_rows,
        "pool_unique_sources_after_exclusion": len(sources),
        "pool_unique_targets": len(targets),
        "excluded_source_endpoints": len(excluded_sources),
        "profile": args.profile,
        "seed": args.seed,
        "shuffle_seed": args.shuffle_seed,
        "fold_count": args.fold_count,
        "fold_index": args.fold_index,
        "source_count": len(chosen),
        "targets_per_source": args.targets_per_source,
        "rows": rows,
        "duplicate_pairs": 0,
        "uniqueness_proof": "unique sources x per-source affine target selection without replacement",
        "distance_quota_per_source": dict(zip(DISTANCE_NAMES, quotas)),
        "actual_distance_categories": dict(sorted(aggregate.items())),
        "buckets": args.buckets,
        "one_row_per_source_per_bucket": args.buckets >= args.targets_per_source,
        "order_diagnostics": {
            "adjacent_same_source_rows": adjacent_same_source,
            "adjacent_same_source_pct": 100.0 * adjacent_same_source / max(1, rows - 1),
            "max_consecutive_same_source": max(bucket_max_run, default=0),
            "note": "Computed exactly from bucket write order and final bucket concatenation order.",
        },
        "source_set_sha256": source_digest.hexdigest().upper(),
        "source_sample": [source.text for source in chosen[:16]],
        "sha256": digest.hexdigest().upper(),
        "bytes": args.output.stat().st_size,
        "generation_seconds": time.perf_counter() - started,
        "note": "Generated directly into external hash buckets; no grouped full-size copy was retained.",
    }
    manifest_path = args.manifest or args.output.with_suffix(".manifest.json")
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


def command_generate(args: argparse.Namespace) -> int:
    started = time.perf_counter()
    sources, targets, reference_counts, reference_rows = pool_profile(args.pool)
    excluded_sources = load_source_names(args.exclude_sources_from)
    if excluded_sources:
        sources = [source for source in sources if source.text not in excluded_sources]
    if args.fold_count <= 0 or not (0 <= args.fold_index < args.fold_count):
        raise ValueError("fold-index must be in [0, fold-count)")
    folded = [
        source for source in sources
        if stable_u64(f"source-fold\0{source.text}", args.seed) % args.fold_count == args.fold_index
    ]
    chosen = choose_sources(folded, args.sources, args.seed)
    if args.profile == "official-like":
        weights = reference_counts
    elif args.profile == "balanced-distance":
        weights = [1] * len(DISTANCE_BINS)
    else:
        weights = [0, 0, 0, 0, 2, 3]
    quotas = largest_remainder(args.targets_per_source, weights)
    architecture = load_architecture(args.gap)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    aggregate: Counter[str] = Counter()
    digest = hashlib.sha256()
    with args.output.open("w", encoding="utf-8", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(["From", "To"])
        digest.update(b"From,To\n")
        for source in chosen:
            selected, stats = select_targets(
                source, targets, args.targets_per_source, quotas, args.seed,
                args.profile, architecture,
            )
            aggregate.update(stats)
            for target in selected:
                line = f"{source.text},{target.text}\n"
                handle.write(line)
                digest.update(line.encode("utf-8"))
    rows = len(chosen) * args.targets_per_source
    manifest = {
        "schema": 1,
        "kind": "unique-srb-requests",
        "pool": str(args.pool.resolve()),
        "pool_rows": reference_rows,
        "pool_unique_sources": len(sources),
        "pool_unique_targets": len(targets),
        "excluded_source_endpoints": len(excluded_sources),
        "profile": args.profile,
        "seed": args.seed,
        "fold_count": args.fold_count,
        "fold_index": args.fold_index,
        "source_count": len(chosen),
        "targets_per_source": args.targets_per_source,
        "rows": rows,
        "duplicate_pairs": 0,
        "distance_quota_per_source": dict(zip(DISTANCE_NAMES, quotas)),
        "actual_categories": dict(sorted(aggregate.items())),
        "sources": [source.text for source in chosen],
        "sha256": digest.hexdigest().upper(),
        "generation_seconds": time.perf_counter() - started,
    }
    manifest_path = args.manifest or args.output.with_suffix(".manifest.json")
    manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


def command_shuffle(args: argparse.Namespace) -> int:
    """Externally scatter grouped requests without holding all rows in RAM."""
    if args.buckets < 2 or args.buckets > 4096:
        raise ValueError("buckets must be between 2 and 4096")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    started = time.perf_counter()
    rows = 0
    with tempfile.TemporaryDirectory(prefix="unique_shuffle_", dir=str(args.output.parent)) as temp_name:
        temp = Path(temp_name)
        paths = [temp / f"bucket_{index:04d}.csv" for index in range(args.buckets)]
        handles = [path.open("w", encoding="utf-8", newline="") for path in paths]
        try:
            with args.input.open("r", encoding="utf-8-sig", newline="") as source:
                reader = csv.DictReader(source)
                if not reader.fieldnames or "From" not in reader.fieldnames or "To" not in reader.fieldnames:
                    raise ValueError("shuffle input must contain From and To")
                for row in reader:
                    from_value, to_value = row["From"].strip(), row["To"].strip()
                    bucket = stable_u64(from_value + "\0" + to_value, args.seed) % args.buckets
                    handles[bucket].write(f"{from_value},{to_value}\n")
                    rows += 1
        finally:
            for handle in handles:
                handle.close()
        digest = hashlib.sha256()
        with args.output.open("wb") as destination:
            header = b"From,To\n"
            destination.write(header)
            digest.update(header)
            # A seeded permutation avoids always placing bucket zero first.
            order = list(range(args.buckets))
            random.Random(args.seed).shuffle(order)
            for index in order:
                with paths[index].open("rb") as source:
                    while True:
                        block = source.read(8 << 20)
                        if not block:
                            break
                        destination.write(block)
                        digest.update(block)
    result = {
        "schema": 1,
        "kind": "unique-srb-shuffled-requests",
        "input": str(args.input.resolve()),
        "output": str(args.output.resolve()),
        "rows": rows,
        "buckets": args.buckets,
        "seed": args.seed,
        "sha256": digest.hexdigest().upper(),
        "wall_seconds": time.perf_counter() - started,
        "note": "Pair uniqueness is inherited from the generated input; rows are externally hash-scattered.",
    }
    manifest = args.manifest or args.output.with_suffix(".shuffle.json")
    manifest.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def command_merge(args: argparse.Namespace) -> int:
    args.output.parent.mkdir(parents=True, exist_ok=True)
    publish = args.output.with_suffix(args.output.suffix + ".partial")
    seen_sources: set[str] = set()
    inputs: list[dict[str, object]] = []
    total_rows = 0
    digest = hashlib.sha256()
    with publish.open("w", encoding="utf-8", newline="") as destination:
        writer = csv.writer(destination, lineterminator="\n")
        writer.writerow(["From", "To", "Delay"])
        digest.update(b"From,To,Delay\n")
        for path in args.input:
            local_sources: set[str] = set()
            rows = 0
            with path.open("r", encoding="utf-8-sig", newline="") as handle:
                reader = csv.DictReader(handle)
                delay = delay_column(reader.fieldnames)
                if not reader.fieldnames or "From" not in reader.fieldnames or "To" not in reader.fieldnames:
                    raise ValueError(f"merge input lacks endpoints: {path}")
                for row in reader:
                    source = row["From"].strip()
                    target = row["To"].strip()
                    value = int(row[delay])
                    local_sources.add(source)
                    line = f"{source},{target},{value}\n"
                    destination.write(line)
                    digest.update(line.encode("utf-8"))
                    rows += 1
            overlap = seen_sources.intersection(local_sources)
            if overlap:
                raise ValueError(
                    f"merge panels are not source-disjoint; first overlap: {sorted(overlap)[0]}"
                )
            seen_sources.update(local_sources)
            total_rows += rows
            inputs.append({"path": str(path.resolve()), "rows": rows, "sources": len(local_sources)})
    os.replace(publish, args.output)
    result = {
        "schema": 1,
        "kind": "source-disjoint-srb-panels",
        "output": str(args.output.resolve()),
        "inputs": inputs,
        "rows": total_rows,
        "sources": len(seen_sources),
        "sha256": digest.hexdigest().upper(),
    }
    manifest = args.manifest or args.output.with_suffix(".merge.json")
    manifest.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def request_groups(path: Path) -> Iterator[tuple[str, list[tuple[str, str]]]]:
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames or "From" not in reader.fieldnames or "To" not in reader.fieldnames:
            raise ValueError("request CSV must contain From and To")
        current = None
        rows: list[tuple[str, str]] = []
        seen_sources: set[str] = set()
        for row in reader:
            source, target = row["From"].strip(), row["To"].strip()
            if current is None:
                current = source
            if source != current:
                if source in seen_sources:
                    raise ValueError("grouped Teacher input requires each source in one contiguous block")
                seen_sources.add(current)
                yield current, rows
                current, rows = source, []
            rows.append((source, target))
        if current is not None:
            yield current, rows


def append_csv_without_header(source: Path, destination, expected_header: str) -> int:
    rows = 0
    with source.open("r", encoding="utf-8-sig", newline="") as handle:
        header = handle.readline().rstrip("\r\n")
        if header != expected_header:
            raise ValueError(f"unexpected header in {source}: {header!r}")
        for line in handle:
            destination.write(line)
            rows += 1
    return rows


def command_label(args: argparse.Namespace) -> int:
    args.labels.parent.mkdir(parents=True, exist_ok=True)
    args.summaries.parent.mkdir(parents=True, exist_ok=True)
    groups = list(request_groups(args.requests))
    if not groups:
        raise ValueError("no requests to label")
    shard_root = args.shard_dir or (args.labels.parent / f"{args.labels.stem}.shards")
    shard_root.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, object]] = []
    total_rows = 0
    all_started = time.perf_counter()
    shard_outputs: list[tuple[Path, Path, int]] = []
    for shard_index in range(0, len(groups), args.sources_per_shard):
            shard_number = shard_index // args.sources_per_shard
            shard_groups = groups[shard_index : shard_index + args.sources_per_shard]
            prefix = shard_root / f"shard_{shard_number:05d}"
            request_path = prefix.with_suffix(".requests.csv")
            label_path = prefix.with_suffix(".labels.csv")
            summary_path = prefix.with_suffix(".summaries.csv")
            status_path = prefix.with_suffix(".json")
            with request_path.open("w", encoding="utf-8", newline="") as handle:
                writer = csv.writer(handle, lineterminator="\n")
                writer.writerow(["From", "To"])
                for _, rows in shard_groups:
                    writer.writerows(rows)
            request_sha, request_rows, _ = sha256_and_rows(request_path)
            expected = sum(len(rows) for _, rows in shard_groups)
            if request_rows != expected:
                raise RuntimeError(f"internal shard row mismatch: {request_rows}/{expected}")
            if status_path.exists() and label_path.exists() and summary_path.exists():
                try:
                    old = json.loads(status_path.read_text(encoding="utf-8"))
                    _, old_label_rows, _ = sha256_and_rows(label_path)
                    _, old_summary_rows, _ = sha256_and_rows(summary_path)
                    if (
                        old.get("request_sha256") == request_sha
                        and int(old.get("rows", -1)) == expected
                        and old_label_rows == expected
                        and old_summary_rows == expected
                    ):
                        records.append(old)
                        total_rows += expected
                        shard_outputs.append((label_path, summary_path, expected))
                        print(f"teacher shard={shard_number} rows={expected} resumed=1", flush=True)
                        continue
                except (OSError, ValueError, TypeError, json.JSONDecodeError):
                    pass
            label_partial = prefix.with_suffix(".labels.partial")
            summary_partial = prefix.with_suffix(".summaries.partial")
            command = [
                str(args.labeler), "--graph", str(args.graph), "--gap", str(args.gap),
                "--input", str(request_path), "--labels", str(label_partial),
                "--summaries", str(summary_partial), "--grouped", "--delay-only",
                "--max-group-expanded", str(args.max_group_expanded),
            ]
            started = time.perf_counter()
            try:
                completed = subprocess.run(
                    command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                    timeout=args.shard_timeout, check=False,
                )
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(
                    f"Teacher shard {shard_number} timed out after {args.shard_timeout}s; "
                    f"sources={[item[0] for item in shard_groups]}"
                ) from error
            wall = time.perf_counter() - started
            if completed.returncode != 0:
                raise RuntimeError(
                    f"Teacher shard {shard_number} failed ({completed.returncode}):\n{completed.stderr[-4000:]}"
                )
            with label_partial.open("r", encoding="utf-8-sig", newline="") as handle:
                fields = [field.strip() for field in handle.readline().rstrip("\r\n").split(",")]
                if not {"From", "To", "Delay"}.issubset(fields):
                    raise ValueError(f"unexpected Teacher label header: {fields!r}")
            _, written, _ = sha256_and_rows(label_partial)
            _, summary_rows, _ = sha256_and_rows(summary_partial)
            if written != expected or summary_rows != expected:
                raise RuntimeError(
                    f"Teacher shard {shard_number} returned {written}/{summary_rows} rows, expected {expected}"
                )
            os.replace(label_partial, label_path)
            os.replace(summary_partial, summary_path)
            total_rows += written
            record = {
                "shard": shard_number,
                "sources": [item[0] for item in shard_groups],
                "rows": written,
                "wall_seconds": wall,
                "request_sha256": request_sha,
                "stderr_tail": completed.stderr[-1000:],
            }
            status_path.write_text(
                json.dumps(record, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
            )
            records.append(record)
            shard_outputs.append((label_path, summary_path, expected))
            print(f"teacher shard={record['shard']} rows={written} wall={wall:.3f}s", flush=True)

    # Publish only after every shard has passed its row-count and header gates.
    label_publish = args.labels.with_suffix(args.labels.suffix + ".partial")
    summary_publish = args.summaries.with_suffix(args.summaries.suffix + ".partial")
    label_header: str | None = None
    summary_header: str | None = None
    with label_publish.open("w", encoding="utf-8", newline="") as labels_out, \
         summary_publish.open("w", encoding="utf-8", newline="") as summaries_out:
        for label_path, summary_path, expected in shard_outputs:
            with label_path.open("r", encoding="utf-8-sig", newline="") as handle:
                header = handle.readline().rstrip("\r\n")
                if label_header is None:
                    label_header = header
                    labels_out.write(header + "\n")
                elif header != label_header:
                    raise ValueError("Teacher label headers changed between shards")
                rows = 0
                for line in handle:
                    labels_out.write(line)
                    rows += 1
                if rows != expected:
                    raise RuntimeError(f"label shard changed before publish: {label_path}")
            with summary_path.open("r", encoding="utf-8-sig", newline="") as handle:
                header = handle.readline().rstrip("\r\n")
                if summary_header is None:
                    summary_header = header
                    summaries_out.write(header + "\n")
                elif header != summary_header:
                    raise ValueError("Teacher summary headers changed between shards")
                rows = 0
                for line in handle:
                    summaries_out.write(line)
                    rows += 1
                if rows != expected:
                    raise RuntimeError(f"summary shard changed before publish: {summary_path}")
    os.replace(label_publish, args.labels)
    os.replace(summary_publish, args.summaries)
    result = {
        "schema": 1,
        "requests": str(args.requests.resolve()),
        "labels": str(args.labels.resolve()),
        "summaries": str(args.summaries.resolve()),
        "rows": total_rows,
        "sources": len(groups),
        "sources_per_shard": args.sources_per_shard,
        "shard_directory": str(shard_root.resolve()),
        "wall_seconds": time.perf_counter() - all_started,
        "shards": records,
    }
    report = args.report or args.labels.with_suffix(".teacher.json")
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return 0


def sha256_and_rows(path: Path) -> tuple[str, int, int]:
    digest = hashlib.sha256()
    newlines = 0
    size = 0
    with path.open("rb") as handle:
        while True:
            block = handle.read(8 << 20)
            if not block:
                break
            digest.update(block)
            newlines += block.count(b"\n")
            size += len(block)
    return digest.hexdigest().upper(), max(0, newlines - 1), size


def fit_line(points: Sequence[tuple[int, float]], scale_rows: int) -> dict[str, float | int | str]:
    if not points:
        raise ValueError("no timing points")
    if len(points) == 1 or len({item[0] for item in points}) == 1:
        rows, seconds = points[0]
        return {
            "method": "single-size proportional projection",
            "intercept_seconds": 0.0,
            "seconds_per_row": seconds / rows,
            "scale_rows": scale_rows,
            "estimate_seconds": seconds * scale_rows / rows,
        }
    xs = [float(item[0]) for item in points]
    ys = [float(item[1]) for item in points]
    mean_x, mean_y = statistics.mean(xs), statistics.mean(ys)
    denominator = sum((value - mean_x) ** 2 for value in xs)
    slope = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys)) / denominator
    intercept = mean_y - slope * mean_x
    if slope < 0:
        raise ValueError("negative timing slope; benchmark sizes are too noisy")
    if intercept < 0:
        intercept = 0.0
        slope = sum(x * y for x, y in zip(xs, ys)) / sum(x * x for x in xs)
    return {
        "method": "least-squares fixed-plus-linear projection",
        "intercept_seconds": intercept,
        "seconds_per_row": slope,
        "scale_rows": scale_rows,
        "estimate_seconds": intercept + slope * scale_rows,
    }


def add_cold_start(projection: dict[str, float | int | str] | None, seconds: float) -> None:
    if projection is None:
        return
    steady = float(projection["estimate_seconds"])
    projection["steady_state_estimate_seconds"] = steady
    projection["cold_start_overhead_seconds"] = seconds
    projection["estimate_seconds"] = steady + seconds


def benchmark_report(
    executable: str,
    runs_per_size: int,
    cases: list[dict[str, object]],
    scale_rows: int,
    host_platform: str,
    python_platform: str,
    executable_format: str,
    cold_start_seconds: float,
) -> dict[str, object]:
    center_points = [(int(case["rows"]), float(case["median_wall_seconds"])) for case in cases]
    low_points = [(int(case["rows"]), float(case["min_wall_seconds"])) for case in cases]
    high_points = [(int(case["rows"]), float(case["max_wall_seconds"])) for case in cases]
    distinct_sizes = len({item[0] for item in center_points})
    projection_allowed = distinct_sizes >= 2
    center = fit_line(center_points, scale_rows) if projection_allowed else None
    low = fit_line(low_points, scale_rows) if projection_allowed else None
    high = fit_line(high_points, scale_rows) if projection_allowed else None
    add_cold_start(center, cold_start_seconds)
    add_cold_start(low, cold_start_seconds)
    add_cold_start(high, cold_start_seconds)
    all_consistent = all(bool(case["consistent"]) for case in cases)
    formal_timing_gate = (
        python_platform.startswith("linux")
        and executable_format == "ELF"
        and runs_per_size >= 5
        and distinct_sizes >= 3
        and min(int(case["rows"]) for case in cases) <= 10_000_000
        and max(int(case["rows"]) for case in cases) >= 100_000_000
        and all_consistent
    )
    return {
        "schema": 2,
        "kind": "unique-srb-benchmark",
        "executable": executable,
        "runs_per_size": runs_per_size,
        "all_consistent": all_consistent,
        "host_platform": host_platform,
        "python_platform": python_platform,
        "executable_format": executable_format,
        "cold_start_overhead_seconds": cold_start_seconds,
        "formal_timing_gate": formal_timing_gate,
        "reference_quality": "release-reference" if formal_timing_gate else "pilot",
        "cases": cases,
        "extrapolation": center,
        "observed_run_range": None if center is None else {
            "low_seconds": min(float(low["estimate_seconds"]), float(high["estimate_seconds"])),
            "high_seconds": max(float(low["estimate_seconds"]), float(high["estimate_seconds"])),
            "warning": "This is an observed run range, not a statistical confidence interval.",
        },
        "warning": (
            "Release timing requires Linux + ELF, five runs, consistent outputs, and at least "
            "three unique input sizes spanning 10M-100M rows. Cold-start overhead is added once."
        ),
    }


def command_benchmark(args: argparse.Namespace) -> int:
    args.output_dir.mkdir(parents=True, exist_ok=True)
    cases: list[dict[str, object]] = []
    for input_path in args.input:
        scan_started = time.perf_counter()
        input_sha256, input_rows, input_bytes = sha256_and_rows(input_path)
        input_scan_seconds = time.perf_counter() - scan_started
        if input_rows <= 0:
            raise ValueError(f"empty benchmark input: {input_path}")
        runs: list[dict[str, object]] = []
        hashes: list[str] = []
        for run_index in range(args.runs):
            output_path = args.output_dir / f"{input_path.stem}.run{run_index + 1}.csv"
            command = [str(args.executable), "-in", str(input_path), "-out", str(output_path)]
            before = os.times()
            started = time.perf_counter()
            try:
                completed = subprocess.run(
                    command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                    timeout=args.timeout, check=False,
                )
            except subprocess.TimeoutExpired as error:
                raise RuntimeError(f"candidate timed out after {args.timeout}s: {input_path}") from error
            wall = time.perf_counter() - started
            after = os.times()
            child_cpu = (after.children_user + after.children_system) - (
                before.children_user + before.children_system
            )
            if completed.returncode != 0:
                raise RuntimeError(
                    f"candidate failed ({completed.returncode}) for {input_path}:\n{completed.stderr[-4000:]}"
                )
            verify_started = time.perf_counter()
            digest, output_rows, output_bytes = sha256_and_rows(output_path)
            output_verify_seconds = time.perf_counter() - verify_started
            if output_rows != input_rows:
                raise RuntimeError(f"candidate wrote {output_rows} rows, expected {input_rows}")
            hashes.append(digest)
            run = {
                "run": run_index + 1,
                "wall_seconds": wall,
                "child_cpu_seconds": child_cpu if child_cpu > 0 else None,
                "output_sha256": digest,
                "output_bytes": output_bytes,
                "output_verify_seconds": output_verify_seconds,
                "stderr_tail": completed.stderr[-1000:],
                "output": str(output_path.resolve()) if args.keep_outputs else None,
            }
            runs.append(run)
            print(
                f"benchmark input={input_path.name} run={run_index + 1}/{args.runs} "
                f"rows={input_rows} wall={wall:.6f}s",
                flush=True,
            )
            if not args.keep_outputs:
                output_path.unlink()
        steady_runs = runs[1:] if args.cold_first_run and not cases and len(runs) > 1 else runs
        steady_walls = [float(item["wall_seconds"]) for item in steady_runs]
        cold_overhead = (
            max(0.0, float(runs[0]["wall_seconds"]) - statistics.median(steady_walls))
            if args.cold_first_run and not cases and len(runs) > 1 else 0.0
        )
        cases.append({
            "input": str(input_path.resolve()),
            "rows": input_rows,
            "input_bytes": input_bytes,
            "input_sha256": input_sha256,
            "input_scan_seconds": input_scan_seconds,
            "consistent": len(set(hashes)) == 1,
            "runs": runs,
            "steady_run_count": len(steady_runs),
            "median_wall_seconds": statistics.median(steady_walls),
            "min_wall_seconds": min(steady_walls),
            "max_wall_seconds": max(steady_walls),
            "cold_start_overhead_seconds": cold_overhead,
        })
    with args.executable.open("rb") as executable_handle:
        executable_format = "ELF" if executable_handle.read(4) == b"\x7fELF" else "other"
    cold_start_seconds = sum(float(case["cold_start_overhead_seconds"]) for case in cases)
    result = benchmark_report(
        str(args.executable.resolve()), args.runs, cases, args.scale_rows,
        platform.platform(), sys.platform, executable_format, cold_start_seconds,
    )
    if len(cases) == 1 and args.allow_single_size_projection:
        case = cases[0]
        result["extrapolation"] = fit_line(
            [(int(case["rows"]), float(case["median_wall_seconds"]))], args.scale_rows,
        )
        add_cold_start(result["extrapolation"], cold_start_seconds)  # type: ignore[arg-type]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    projection = result["extrapolation"]
    if projection is None:
        print("benchmark complete; no score projection: provide at least two distinct input sizes")
    else:
        print(json.dumps(projection, ensure_ascii=False, indent=2))
    return 0


def command_combine_benchmarks(args: argparse.Namespace) -> int:
    reports = [json.loads(path.read_text(encoding="utf-8")) for path in args.input]
    if not reports:
        raise ValueError("no benchmark reports")
    keys = ("executable", "host_platform", "python_platform", "executable_format", "runs_per_size")
    for key in keys:
        values = {json.dumps(report.get(key), sort_keys=True) for report in reports}
        if len(values) != 1:
            raise ValueError(f"benchmark reports disagree on {key}: {values}")
    cases = [case for report in reports for case in report.get("cases", [])]
    rows = [int(case["rows"]) for case in cases]
    if len(rows) != len(set(rows)):
        raise ValueError("benchmark reports contain duplicate input sizes")
    cold_values = [float(report.get("cold_start_overhead_seconds", 0.0)) for report in reports]
    if sum(value > 0 for value in cold_values) > 1:
        raise ValueError("more than one component report claims a cold start")
    first = reports[0]
    result = benchmark_report(
        str(first["executable"]), int(first["runs_per_size"]), cases, args.scale_rows,
        str(first["host_platform"]), str(first["python_platform"]),
        str(first["executable_format"]), sum(cold_values),
    )
    result["component_reports"] = [str(path.resolve()) for path in args.input]
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["extrapolation"], ensure_ascii=False, indent=2))
    return 0


def point_score(golden: int, predicted: int) -> float:
    if golden < 0 or predicted < 0:
        raise ValueError("negative delay")
    if golden == 0:
        return float(predicted == 0)
    return 1.0 - math.tanh(4.0 * abs(predicted - golden) / golden)


def delay_column(fieldnames: Sequence[str] | None) -> str:
    for name in fieldnames or ():
        if name.strip().lower() in {"delay", "min delay", "predict_delay"}:
            return name
    raise ValueError("CSV has no delay column")


def percentile(values: Sequence[float], percent: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    position = (len(ordered) - 1) * percent / 100.0
    lower = int(math.floor(position))
    upper = int(math.ceil(position))
    if lower == upper:
        return ordered[lower]
    weight = position - lower
    return ordered[lower] * (1.0 - weight) + ordered[upper] * weight


def cluster_bootstrap(
    groups: dict[str, list[float]], samples: int, seed: int
) -> dict[str, float | int] | None:
    values = list(groups.values())
    if len(values) < 2:
        return None
    randomizer = random.Random(seed)
    estimates: list[float] = []
    for _ in range(samples):
        score_sum = 0.0
        rows = 0.0
        for _ in range(len(values)):
            score, count = values[randomizer.randrange(len(values))]
            score_sum += score
            rows += count
        estimates.append(100.0 * score_sum / rows)
    return {
        "groups": len(values),
        "samples": samples,
        "lower_95": percentile(estimates, 2.5),
        "median": percentile(estimates, 50.0),
        "upper_95": percentile(estimates, 97.5),
    }


def group_diagnostics(groups: dict[str, list[float]]) -> dict[str, float | int]:
    accuracies = [100.0 * score / rows for score, rows in groups.values() if rows]
    return {
        "groups": len(accuracies),
        "minimum_accuracy": min(accuracies) if accuracies else math.nan,
        "p10_accuracy": percentile(accuracies, 10.0),
        "median_accuracy": percentile(accuracies, 50.0),
        "maximum_accuracy": max(accuracies) if accuracies else math.nan,
    }


def official_score(accuracy: float, seconds: float, consistent: bool) -> dict[str, float]:
    time_score = 100.0 * (1.0 - seconds / 1800.0)
    consistency = 100.0 if consistent else 0.0
    return {
        "accuracy": accuracy,
        "seconds": seconds,
        "time_score": time_score,
        "consistency_score": consistency,
        "total_score": 0.8 * accuracy + 0.15 * time_score + 0.05 * consistency,
    }


def evaluate_files(
    golden_path: Path,
    prediction_path: Path,
    architecture: Architecture,
    bootstrap_samples: int,
    seed: int,
) -> dict[str, object]:
    overall = Metric()
    groups: dict[str, defaultdict[str, list[float]]] = {
        "source": defaultdict(lambda: [0.0, 0.0]),
        "source_region": defaultdict(lambda: [0.0, 0.0]),
        "target_region": defaultdict(lambda: [0.0, 0.0]),
    }
    segments: dict[str, defaultdict[str, Metric]] = {
        "distance": defaultdict(Metric),
        "block_count": defaultdict(Metric),
        "gap_count": defaultdict(Metric),
    }
    with golden_path.open("r", encoding="utf-8-sig", newline="") as golden_handle, \
         prediction_path.open("r", encoding="utf-8-sig", newline="") as prediction_handle:
        golden_reader = csv.DictReader(golden_handle)
        prediction_reader = csv.DictReader(prediction_handle)
        golden_delay = delay_column(golden_reader.fieldnames)
        prediction_delay = delay_column(prediction_reader.fieldnames)
        for line, pair in enumerate(zip_longest(golden_reader, prediction_reader), 2):
            golden_row, prediction_row = pair
            if golden_row is None or prediction_row is None:
                raise ValueError(f"row count mismatch at line {line}")
            if golden_row.get("From") != prediction_row.get("From") or golden_row.get("To") != prediction_row.get("To"):
                raise ValueError(f"endpoint/order mismatch at line {line}")
            source = parse_endpoint(golden_row["From"])
            target = parse_endpoint(golden_row["To"])
            golden = int(golden_row[golden_delay])
            predicted = int(prediction_row[prediction_delay])
            point = overall.add(golden, predicted)
            keys = {
                "source": source.text,
                "source_region": f"{source.x // 20}:{source.y // 100}",
                "target_region": f"{target.x // 20}:{target.y // 100}",
            }
            for name, key in keys.items():
                groups[name][key][0] += point
                groups[name][key][1] += 1.0
            distance = max(abs(target.x - source.x), abs(target.y - source.y))
            blocks, gaps = environment(source, target, architecture)
            segments["distance"][DISTANCE_NAMES[distance_bin(distance)]].add(golden, predicted)
            segments["block_count"][f"{min(blocks, 3)}{'+' if blocks >= 3 else ''}"].add(golden, predicted)
            segments["gap_count"][f"{min(gaps, 5)}{'+' if gaps >= 5 else ''}"].add(golden, predicted)
    if not overall.rows:
        raise ValueError("no aligned rows")
    bootstrap: dict[str, object] = {}
    diagnostics: dict[str, object] = {}
    for index, (name, values) in enumerate(groups.items()):
        plain = dict(values)
        bootstrap[name] = cluster_bootstrap(plain, bootstrap_samples, seed + index * 1009)
        diagnostics[name] = group_diagnostics(plain)
    bounds = [value for value in bootstrap.values() if isinstance(value, dict)]
    envelope = {
        "lower_95": min(float(value["lower_95"]) for value in bounds),
        "upper_95": max(float(value["upper_95"]) for value in bounds),
        "method": "conservative envelope of source/source-region/target-region cluster bootstraps",
    }
    quality_warnings: list[str] = []
    source_groups = len(groups["source"])
    if source_groups < 32:
        quality_warnings.append("fewer than 32 independent sources; treat this as a smoke test")
    if overall.rows < 100_000:
        quality_warnings.append("fewer than 100,000 exact rows; expand the Teacher before release decisions")
    if source_groups < 64 or overall.rows < 500_000:
        quality = "pilot"
    else:
        quality = "release-reference"
    return {
        "overall": overall.result(),
        "bootstrap_95": bootstrap,
        "conservative_accuracy_interval": envelope,
        "group_diagnostics": diagnostics,
        "reference_quality": quality,
        "quality_warnings": quality_warnings,
        "segments": {
            name: {key: metric.result() for key, metric in sorted(values.items())}
            for name, values in segments.items()
        },
    }


def render_markdown(result: dict[str, object]) -> str:
    accuracy = result["accuracy"]
    assert isinstance(accuracy, dict)
    overall = accuracy["overall"]
    interval = accuracy["conservative_accuracy_interval"]
    assert isinstance(overall, dict) and isinstance(interval, dict)
    lines = [
        "# Unique evaluation report",
        "",
        "## Accuracy",
        "",
        f"- Rows: `{overall['rows']}`",
        f"- Accuracy: `{float(overall['accuracy']):.10f}`",
        f"- Conservative grouped 95% interval: "
        f"`[{float(interval['lower_95']):.10f}, {float(interval['upper_95']):.10f}]`",
        f"- MAE: `{float(overall['mae_ps']):.6f} ps`",
        f"- Exact share: `{float(overall['exact_share_pct']):.6f}%`",
        "",
        "The interval is the envelope of source, source-region and target-region cluster bootstraps; "
        "it is intentionally wider than a random-row interval.",
    ]
    if "score" in result:
        score = result["score"]
        assert isinstance(score, dict)
        center = score["center"]
        lower = score["conservative_lower"]
        upper = score["conservative_upper"]
        assert isinstance(center, dict) and isinstance(lower, dict) and isinstance(upper, dict)
        lines += [
            "",
            "## Official-score projection",
            "",
            f"- Center runtime: `{float(center['seconds']):.3f} s`",
            f"- Center total: `{float(center['total_score']):.6f}`",
            f"- Conservative range: "
            f"`[{float(lower['total_score']):.6f}, {float(upper['total_score']):.6f}]`",
            f"- Time/score reference quality: `{score.get('reference_quality', 'scenario-only')}`",
            "",
            "This is a projection, not a hidden-platform score. The range combines grouped accuracy "
            "uncertainty with the observed benchmark run range.",
        ]
    lines += ["", "## Segment accuracy", ""]
    segments = accuracy["segments"]
    assert isinstance(segments, dict)
    for name, values in segments.items():
        lines += [f"### {name}", "", "| Group | Rows | Accuracy |", "|---|---:|---:|"]
        assert isinstance(values, dict)
        for key, metric in values.items():
            assert isinstance(metric, dict)
            lines.append(f"| {key} | {metric['rows']} | {float(metric['accuracy']):.8f} |")
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def command_evaluate(args: argparse.Namespace) -> int:
    accuracy = evaluate_files(
        args.golden, args.prediction, load_architecture(args.gap),
        args.bootstrap_samples, args.seed,
    )
    result: dict[str, object] = {
        "schema": 1,
        "kind": "unique-srb-evaluation",
        "golden": str(args.golden.resolve()),
        "prediction": str(args.prediction.resolve()),
        "accuracy": accuracy,
        "official_formula": (
            "total = 0.80*accuracy + 0.15*(100-100*seconds/1800) + 0.05*consistency"
        ),
    }
    if args.public_accuracy is not None:
        actual = float(accuracy["overall"]["accuracy"])  # type: ignore[index]
        result["public_comparison"] = {
            "public_accuracy": args.public_accuracy,
            "unique_accuracy": actual,
            "accuracy_gap": actual - args.public_accuracy,
            "weighted_total_gap": 0.8 * (actual - args.public_accuracy),
        }
    seconds_center: float | None = args.average_seconds
    seconds_low: float | None = args.average_seconds
    seconds_high: float | None = args.average_seconds
    consistent = True
    if args.timing is not None:
        timing = json.loads(args.timing.read_text(encoding="utf-8"))
        if timing.get("extrapolation") is None or timing.get("observed_run_range") is None:
            raise ValueError(
                "timing report has no safe extrapolation; benchmark at least two distinct sizes"
            )
        seconds_center = float(timing["extrapolation"]["estimate_seconds"])
        seconds_low = float(timing["observed_run_range"]["low_seconds"])
        seconds_high = float(timing["observed_run_range"]["high_seconds"])
        consistent = bool(timing["all_consistent"])
        result["timing"] = timing
    if seconds_center is not None and seconds_low is not None and seconds_high is not None:
        center_accuracy = float(accuracy["overall"]["accuracy"])  # type: ignore[index]
        interval = accuracy["conservative_accuracy_interval"]  # type: ignore[index]
        result["score"] = {
            "reference_quality": (
                result.get("timing", {}).get("reference_quality", "pilot")
                if isinstance(result.get("timing"), dict) else "scenario-only"
            ),
            "center": official_score(center_accuracy, seconds_center, consistent),
            "conservative_lower": official_score(float(interval["lower_95"]), seconds_high, consistent),
            "conservative_upper": official_score(float(interval["upper_95"]), seconds_low, consistent),
        }
    rendered = json.dumps(result, ensure_ascii=False, indent=2) + "\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(rendered, encoding="utf-8")
    markdown = args.markdown or args.output.with_suffix(".md")
    markdown.write_text(render_markdown(result), encoding="utf-8")
    print(rendered, end="")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    generate = subparsers.add_parser("generate", help="generate deterministic unique requests")
    generate.add_argument("--pool", type=Path, required=True, help="representative CSV endpoint pool")
    generate.add_argument("--output", type=Path, required=True)
    generate.add_argument("--manifest", type=Path)
    generate.add_argument("--gap", type=Path)
    generate.add_argument(
        "--exclude-sources-from", type=Path, action="append", default=[],
        help="CSV whose complete From endpoint groups must not appear in the new set",
    )
    generate.add_argument("--sources", type=int, default=32)
    generate.add_argument("--targets-per-source", type=int, default=3000)
    generate.add_argument("--profile", choices=("official-like", "balanced-distance", "stress"), default="official-like")
    generate.add_argument("--seed", type=int, default=20261025)
    generate.add_argument("--fold-count", type=int, default=5)
    generate.add_argument("--fold-index", type=int, default=0)
    generate.set_defaults(func=command_generate)

    generate_perf = subparsers.add_parser(
        "generate-perf", help="stream unique shuffled performance requests without a grouped copy",
    )
    generate_perf.add_argument("--pool", type=Path, required=True, help="representative CSV endpoint pool")
    generate_perf.add_argument("--output", type=Path, required=True)
    generate_perf.add_argument("--manifest", type=Path)
    generate_perf.add_argument(
        "--exclude-sources-from", type=Path, action="append", default=[],
        help="CSV whose complete From endpoint groups must not appear in the new set",
    )
    generate_perf.add_argument("--sources", type=int, required=True)
    generate_perf.add_argument("--targets-per-source", type=int, default=1000)
    generate_perf.add_argument(
        "--profile", choices=("official-like", "balanced-distance", "stress"), default="official-like",
    )
    generate_perf.add_argument("--seed", type=int, default=20261025)
    generate_perf.add_argument("--shuffle-seed", type=int, default=20261026)
    generate_perf.add_argument("--fold-count", type=int, default=5)
    generate_perf.add_argument("--fold-index", type=int, default=0)
    generate_perf.add_argument("--buckets", type=int, default=1024)
    generate_perf.add_argument("--progress-sources", type=int, default=1000)
    generate_perf.set_defaults(func=command_generate_perf)

    shuffle = subparsers.add_parser("shuffle", help="externally hash-scatter grouped unique requests")
    shuffle.add_argument("--input", type=Path, required=True)
    shuffle.add_argument("--output", type=Path, required=True)
    shuffle.add_argument("--manifest", type=Path)
    shuffle.add_argument("--buckets", type=int, default=128)
    shuffle.add_argument("--seed", type=int, default=20261025)
    shuffle.set_defaults(func=command_shuffle)

    merge = subparsers.add_parser("merge", help="merge source-disjoint label or prediction panels")
    merge.add_argument("--input", type=Path, action="append", required=True)
    merge.add_argument("--output", type=Path, required=True)
    merge.add_argument("--manifest", type=Path)
    merge.set_defaults(func=command_merge)

    label = subparsers.add_parser("label", help="run exact grouped Dijkstra Teacher")
    label.add_argument("--labeler", type=Path, required=True)
    label.add_argument("--graph", type=Path, required=True)
    label.add_argument("--gap", type=Path, required=True)
    label.add_argument("--requests", type=Path, required=True)
    label.add_argument("--labels", type=Path, required=True)
    label.add_argument("--summaries", type=Path, required=True)
    label.add_argument("--report", type=Path)
    label.add_argument("--shard-dir", type=Path)
    label.add_argument("--sources-per-shard", type=int, default=1)
    label.add_argument("--shard-timeout", type=float, default=1200.0)
    label.add_argument("--max-group-expanded", type=int, default=10_000_000)
    label.set_defaults(func=command_label)

    benchmark = subparsers.add_parser("benchmark", help="benchmark candidate on unique request streams")
    benchmark.add_argument("--executable", type=Path, required=True)
    benchmark.add_argument("--input", type=Path, action="append", required=True)
    benchmark.add_argument("--output-dir", type=Path, required=True)
    benchmark.add_argument("--report", type=Path, required=True)
    benchmark.add_argument("--runs", type=int, default=5)
    benchmark.add_argument("--timeout", type=float, default=3600.0)
    benchmark.add_argument("--scale-rows", type=int, default=1_000_000_000)
    benchmark.add_argument(
        "--cold-first-run", action="store_true",
        help="treat the first run as cold and add its excess over steady median exactly once",
    )
    benchmark.add_argument(
        "--keep-outputs", action="store_true",
        help="retain every large prediction file; by default outputs are hashed then deleted",
    )
    benchmark.add_argument(
        "--allow-single-size-projection", action="store_true",
        help="diagnostic only; a one-size projection confounds startup and per-row cost",
    )
    benchmark.set_defaults(func=command_benchmark)

    combine_benchmarks = subparsers.add_parser(
        "combine-benchmarks", help="combine sequential same-host benchmark reports",
    )
    combine_benchmarks.add_argument("--input", type=Path, action="append", required=True)
    combine_benchmarks.add_argument("--report", type=Path, required=True)
    combine_benchmarks.add_argument("--scale-rows", type=int, default=1_000_000_000)
    combine_benchmarks.set_defaults(func=command_combine_benchmarks)

    evaluate = subparsers.add_parser("evaluate", help="score unique predictions and build confidence bounds")
    evaluate.add_argument("--golden", type=Path, required=True)
    evaluate.add_argument("--prediction", type=Path, required=True)
    evaluate.add_argument("--gap", type=Path)
    evaluate.add_argument("--timing", type=Path)
    evaluate.add_argument("--average-seconds", type=float)
    evaluate.add_argument("--public-accuracy", type=float)
    evaluate.add_argument("--bootstrap-samples", type=int, default=4000)
    evaluate.add_argument("--seed", type=int, default=20261025)
    evaluate.add_argument("--output", type=Path, required=True)
    evaluate.add_argument("--markdown", type=Path)
    evaluate.set_defaults(func=command_evaluate)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "sources", 1) <= 0 or getattr(args, "targets_per_source", 1) <= 0:
        parser.error("source and target counts must be positive")
    if getattr(args, "sources_per_shard", 1) <= 0:
        parser.error("sources-per-shard must be positive")
    if getattr(args, "runs", 1) <= 0:
        parser.error("runs must be positive")
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
