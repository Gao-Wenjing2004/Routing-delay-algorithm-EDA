#!/usr/bin/env python3
"""Build deterministic, globally unique Teacher panels with hard holdouts.

The split is performed before candidate generation.  Region holdout owns whole
20x100 source regions; source holdout owns complete source endpoints outside
those regions.  No request-result cache or row-random split is used.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import random
import re
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path


ENDPOINT = re.compile(r"^SRB_(\d+)_(\d+)/(.+)$")
BOUNDARIES = (23, 24, 25, 26, 27, 71, 72, 73)


@dataclass(frozen=True)
class Point:
    text: str
    x: int
    y: int
    port: str


def parse_endpoint(value: str) -> Point:
    text = value.strip()
    match = ENDPOINT.fullmatch(text)
    if not match:
        raise ValueError(f"bad endpoint: {value!r}")
    return Point(text, int(match[1]), int(match[2]), match[3])


def token(value: str, seed: int) -> int:
    return int.from_bytes(
        hashlib.blake2b(f"{seed}\0{value}".encode(), digest_size=8).digest(),
        "little",
    )


def source_region(point: Point) -> tuple[int, int]:
    return point.x // 20, point.y // 100


def ring(x: int, y: int, radius: int) -> list[tuple[int, int]]:
    result: list[tuple[int, int]] = []
    for dx in range(-radius, radius + 1):
        result.append((x + dx, y - radius))
        result.append((x + dx, y + radius))
    for dy in range(-radius + 1, radius):
        result.append((x - radius, y + dy))
        result.append((x + radius, y + dy))
    return [(px, py) for px, py in result if 0 <= px < 120 and 0 <= py < 550]


def load_architecture(path: Path) -> tuple[tuple[int, ...], tuple[int, ...], tuple[tuple[int, int, int, int], ...]]:
    root = json.loads(path.read_text(encoding="utf-8-sig"))["Gap"]
    horizontal = tuple(sorted(int(row["site"]) for row in root["Line"] if row["direction"] == "horizontal"))
    vertical = tuple(sorted(int(row["site"]) for row in root["Line"] if row["direction"] == "vertical"))
    blocks = tuple(
        (int(row["left"]), int(row["right"]), int(row["lower"]), int(row["upper"]))
        for row in root["Block"]
    )
    return horizontal, vertical, blocks


def between(site: int, first: int, second: int) -> bool:
    return min(first, second) <= site < max(first, second)


def environment_category(
    source: Point,
    target: Point,
    architecture: tuple[tuple[int, ...], tuple[int, ...], tuple[tuple[int, int, int, int], ...]],
) -> str | None:
    horizontal, vertical, blocks = architecture
    gap_count = sum(between(site, source.y, target.y) for site in horizontal)
    gap_count += sum(between(site, source.x, target.x) for site in vertical)
    min_x, max_x = sorted((source.x, target.x))
    min_y, max_y = sorted((source.y, target.y))
    block_count = sum(
        min_x <= right and max_x >= left and min_y <= upper and max_y >= lower
        for left, right, lower, upper in blocks
    )
    near_gap = any(abs(value - site) <= 2 for value in (source.y, target.y) for site in horizontal)
    near_gap |= any(abs(value - site) <= 2 for value in (source.x, target.x) for site in vertical)
    near_block = any(
        min(abs(source.x - left), abs(source.x - right), abs(target.x - left), abs(target.x - right),
            abs(source.y - lower), abs(source.y - upper), abs(target.y - lower), abs(target.y - upper)) <= 2
        for left, right, lower, upper in blocks
    )
    if block_count:
        return "block_near" if near_block else "block_cross"
    if gap_count:
        return "gap_near" if near_gap else "gap_cross"
    return None


def choose_split(source: Point, seed: int) -> str:
    region = source_region(source)
    if token(f"region:{region[0]}:{region[1]}", seed) % 5 == 0:
        return "region_holdout"
    if token(f"source:{source.text}", seed) % 5 == 0:
        return "source_holdout"
    return "train"


def load_pool(path: Path) -> tuple[dict[str, Point], dict[tuple[int, int], list[Point]], dict[str, list[Point]]]:
    sources: dict[str, Point] = {}
    coordinates: dict[tuple[int, int], dict[str, Point]] = defaultdict(dict)
    observed: dict[str, dict[str, Point]] = defaultdict(dict)
    with path.open(encoding="utf-8-sig", newline="") as handle:
        reader = csv.DictReader(handle)
        if not reader.fieldnames or "From" not in reader.fieldnames or "To" not in reader.fieldnames:
            raise ValueError("pool must contain From and To")
        for row in reader:
            source, target = parse_endpoint(row["From"]), parse_endpoint(row["To"])
            sources[source.text] = source
            coordinates[(target.x, target.y)][target.text] = target
            observed[source.text][target.text] = target
    return (
        sources,
        {key: sorted(value.values(), key=lambda item: item.text) for key, value in coordinates.items()},
        {key: sorted(value.values(), key=lambda item: item.text) for key, value in observed.items()},
    )


def candidates_for_source(
    source: Point,
    coordinates: dict[tuple[int, int], list[Point]],
    observed: dict[str, list[Point]],
    architecture: tuple[tuple[int, ...], tuple[int, ...], tuple[tuple[int, int, int, int], ...]],
    seed: int,
    per_boundary: int,
    environment_limit: int,
) -> list[tuple[Point, str]]:
    selected: list[tuple[Point, str]] = []
    used: set[str] = set()
    for distance in BOUNDARIES:
        cells = ring(source.x, source.y, distance)
        random.Random(token(f"ring:{source.text}:{distance}", seed)).shuffle(cells)
        rows: list[Point] = []
        for cell in cells:
            targets = coordinates.get(cell, ())
            if not targets:
                continue
            start = token(f"target:{source.text}:{cell}", seed) % len(targets)
            for offset in range(len(targets)):
                target = targets[(start + offset) % len(targets)]
                if target.text != source.text and target.text not in used:
                    rows.append(target)
                    used.add(target.text)
                    break
            if len(rows) == per_boundary:
                break
        selected.extend((target, f"distance_{distance}") for target in rows)

    environment_rows: list[tuple[int, Point, str]] = []
    for target in observed.get(source.text, ()):
        if target.text in used or target.text == source.text:
            continue
        category = environment_category(source, target, architecture)
        if category:
            environment_rows.append((token(f"env:{source.text}:{target.text}", seed), target, category))
    for _, target, category in sorted(environment_rows)[:environment_limit]:
        selected.append((target, category))
        used.add(target.text)
    return selected


def write_panel(root: Path, name: str, rows: list[tuple[Point, Point, str]]) -> dict[str, object]:
    request_path = root / f"{name}_requests.csv"
    metadata_path = root / f"{name}_metadata.csv"
    digest = hashlib.sha256()
    categories: Counter[str] = Counter()
    sources: set[str] = set()
    regions: set[tuple[int, int]] = set()
    with request_path.open("w", encoding="utf-8", newline="") as requests, metadata_path.open(
        "w", encoding="utf-8", newline=""
    ) as metadata:
        request_writer = csv.writer(requests, lineterminator="\n")
        meta_writer = csv.writer(metadata, lineterminator="\n")
        request_writer.writerow(["From", "To"])
        meta_writer.writerow(["From", "To", "Category", "SourceRegion"])
        digest.update(b"From,To\n")
        for source, target, category in rows:
            line = f"{source.text},{target.text}\n"
            requests.write(line)
            digest.update(line.encode())
            meta_writer.writerow([source.text, target.text, category, f"{source_region(source)[0]}:{source_region(source)[1]}"])
            categories[category] += 1
            sources.add(source.text)
            regions.add(source_region(source))
    return {
        "rows": len(rows),
        "sources": len(sources),
        "regions": len(regions),
        "categories": dict(sorted(categories.items())),
        "requests": str(request_path),
        "metadata": str(metadata_path),
        "sha256": digest.hexdigest(),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pool", type=Path, required=True)
    parser.add_argument("--gap", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20261028)
    parser.add_argument("--per-boundary", type=int, default=8)
    parser.add_argument("--environment-per-source", type=int, default=64)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    architecture = load_architecture(args.gap)
    sources, coordinates, observed = load_pool(args.pool)
    panels: dict[str, list[tuple[Point, Point, str]]] = defaultdict(list)
    pairs: set[tuple[str, str]] = set()
    source_sets: dict[str, set[str]] = defaultdict(set)
    region_sets: dict[str, set[tuple[int, int]]] = defaultdict(set)
    for source in sorted(sources.values(), key=lambda item: item.text):
        split = choose_split(source, args.seed)
        for target, category in candidates_for_source(
            source, coordinates, observed, architecture, args.seed,
            args.per_boundary, args.environment_per_source,
        ):
            pair = source.text, target.text
            if pair in pairs:
                raise AssertionError(f"duplicate pair: {pair}")
            pairs.add(pair)
            panels[split].append((source, target, category))
        source_sets[split].add(source.text)
        region_sets[split].add(source_region(source))

    names = ("train", "source_holdout", "region_holdout")
    for left in names:
        for right in names:
            if left >= right:
                continue
            if source_sets[left] & source_sets[right]:
                raise AssertionError(f"source leakage: {left}/{right}")
    if region_sets["train"] & region_sets["region_holdout"]:
        raise AssertionError("region leakage: train/region_holdout")
    if region_sets["source_holdout"] & region_sets["region_holdout"]:
        raise AssertionError("region leakage: source_holdout/region_holdout")

    report = {
        "schema": 1,
        "kind": "cache-free-targeted-dijkstra-teacher-panels",
        "seed": args.seed,
        "boundary_distances": BOUNDARIES,
        "global_unique_pairs": len(pairs),
        "split_policy": {
            "region_holdout": "whole 20x100 source regions, stable hash mod 5 == 0",
            "source_holdout": "whole source endpoints outside held regions, stable hash mod 5 == 0",
            "train": "remaining complete source endpoints",
        },
        "panels": {name: write_panel(args.output_dir, name, panels[name]) for name in names},
        "leakage": {"pair": 0, "source": 0, "region_into_region_holdout": 0},
    }
    manifest = args.output_dir / "manifest.json"
    manifest.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
