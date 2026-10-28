#!/usr/bin/env python3
"""Generate legal, stratified SRB query pairs for offline exact labeling."""

from __future__ import annotations

import argparse
import csv
import json
import random
import re
from collections import Counter, defaultdict
from pathlib import Path


BUS = re.compile(r"\[\d+\]")


def port_stem(port: str) -> str:
    return BUS.sub("[*]", port)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--inst", type=Path, required=True)
    parser.add_argument("--port", type=Path, required=True)
    parser.add_argument(
        "--template-requests",
        type=Path,
        default=None,
        help="optional public-style requests used only to reproduce source/target port-pair frequencies",
    )
    parser.add_argument("--count", type=int, required=True)
    parser.add_argument("--min-cheb", type=int, default=0)
    parser.add_argument("--max-cheb", type=int, default=32)
    parser.add_argument(
        "--source-count",
        type=int,
        default=0,
        help="reuse this many source endpoints so a future one-to-many exact labeler can amortize search",
    )
    parser.add_argument(
        "--minimum-sources-per-port",
        type=int,
        default=0,
        help="with template requests, seed at least this many grouped sources for every observed source port",
    )
    parser.add_argument(
        "--portal-source-ratio",
        type=float,
        default=0.35,
        help="fraction of grouped sources sampled beside Block holes",
    )
    parser.add_argument(
        "--source-input-probability",
        type=float,
        default=0.32672,
        help="when source-direction=any, match the public Input/Output source mixture",
    )
    parser.add_argument(
        "--priority-source-stems",
        default="",
        help="comma-separated bus-normalized stems (for example ZLW[*],ZLE[*])",
    )
    parser.add_argument(
        "--priority-target-stems",
        default="",
        help="comma-separated bus-normalized target stems",
    )
    parser.add_argument(
        "--priority-port-ratio",
        type=float,
        default=0.0,
        help="probability of sampling from the priority source/target stem pools",
    )
    parser.add_argument("--seed", type=int, default=20261002)
    parser.add_argument("--source-direction", choices=("Input", "Output", "any"), default="any")
    parser.add_argument(
        "--target-direction",
        choices=("Input", "Output", "any"),
        default="Output",
        help="public requests always use Output targets; override only for structural research",
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, default=None)
    args = parser.parse_args()
    if args.count <= 0 or args.min_cheb < 0 or args.max_cheb < args.min_cheb:
        raise ValueError("count must be positive and 0 <= min-cheb <= max-cheb")
    if args.source_count < 0 or args.source_count > args.count:
        raise ValueError("source-count must be in [0, count]")
    if args.minimum_sources_per_port < 0:
        raise ValueError("minimum-sources-per-port must be non-negative")
    if not 0.0 <= args.portal_source_ratio <= 1.0:
        raise ValueError("portal-source-ratio must be in [0, 1]")
    if not 0.0 <= args.source_input_probability <= 1.0:
        raise ValueError("source-input-probability must be in [0, 1]")
    if not 0.0 <= args.priority_port_ratio <= 1.0:
        raise ValueError("priority-port-ratio must be in [0, 1]")
    rng = random.Random(args.seed)

    instances = json.loads(args.inst.read_text(encoding="utf-8"))["Inst"]
    coord_to_name = {(int(row["x"]), int(row["y"])): row["name"] for row in instances}
    name_to_coord = {name: coord for coord, name in coord_to_name.items()}
    cells = sorted(coord_to_name)
    ports = json.loads(args.port.read_text(encoding="utf-8"))["Port"]
    port_direction = {row["Name"]: row["Direction"] for row in ports}
    template_source_counts: Counter[str] = Counter()
    template_pair_counts: dict[str, Counter[str]] = defaultdict(Counter)
    if args.template_requests is not None:
        with args.template_requests.open("r", encoding="utf-8-sig", newline="") as stream:
            for row in csv.DictReader(stream):
                source_port = row["From"].split("/", 1)[1]
                target_port = row["To"].split("/", 1)[1]
                if source_port not in port_direction or target_port not in port_direction:
                    raise ValueError("template request references an unknown port")
                if args.source_direction != "any" and port_direction[source_port] != args.source_direction:
                    continue
                if args.target_direction != "any" and port_direction[target_port] != args.target_direction:
                    continue
                template_source_counts[source_port] += 1
                template_pair_counts[source_port][target_port] += 1
        if not template_source_counts:
            raise ValueError("template-requests contains no port pairs matching the direction filters")

    def names(direction: str) -> list[str]:
        values = [row["Name"] for row in ports if direction == "any" or row["Direction"] == direction]
        if not values:
            raise ValueError(f"no ports for direction {direction}")
        return values

    input_ports = names("Input")
    output_ports = names("Output")
    source_ports = names(args.source_direction)
    target_ports = names(args.target_direction)
    priority_source_stems = {
        value.strip() for value in args.priority_source_stems.split(",") if value.strip()
    }
    priority_target_stems = {
        value.strip() for value in args.priority_target_stems.split(",") if value.strip()
    }
    priority_source_ports = [
        port for port in source_ports if port_stem(port) in priority_source_stems
    ]
    priority_target_ports = [
        port for port in target_ports if port_stem(port) in priority_target_stems
    ]
    if priority_source_stems and not priority_source_ports:
        raise ValueError("priority-source-stems did not match any source port")
    if priority_target_stems and not priority_target_ports:
        raise ValueError("priority-target-stems did not match any target port")

    def choose_source_port() -> str:
        if priority_source_ports and rng.random() < args.priority_port_ratio:
            if template_source_counts:
                weights = [template_source_counts.get(port, 0) + 1 for port in priority_source_ports]
                return rng.choices(priority_source_ports, weights=weights, k=1)[0]
            return rng.choice(priority_source_ports)
        if template_source_counts:
            return rng.choices(
                list(template_source_counts), weights=list(template_source_counts.values()), k=1
            )[0]
        if args.source_direction != "any":
            return rng.choice(source_ports)
        return rng.choice(input_ports if rng.random() < args.source_input_probability else output_ports)

    def choose_target_port(source_port: str) -> str:
        conditional = template_pair_counts.get(source_port)
        if priority_target_ports and rng.random() < args.priority_port_ratio:
            if conditional:
                weights = [conditional.get(port, 0) + 1 for port in priority_target_ports]
                return rng.choices(priority_target_ports, weights=weights, k=1)[0]
            return rng.choice(priority_target_ports)
        if conditional:
            return rng.choices(list(conditional), weights=list(conditional.values()), k=1)[0]
        return rng.choice(target_ports)

    # Cells beside internal holes are Block-perimeter/portal samples.
    portal_cells = []
    for x, y in cells:
        if any(
            0 < nx < 119 and 0 < ny < 549 and (nx, ny) not in coord_to_name
            for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1))
        ):
            portal_cells.append((x, y))

    grouped_sources: list[tuple[int, int, str]] = []
    source_keys: set[tuple[int, int, str]] = set()

    required_source_ports = sorted(template_source_counts) if template_source_counts else []
    required_count = args.minimum_sources_per_port * len(required_source_ports)
    if required_count > args.source_count:
        raise ValueError(
            "source-count is too small for minimum-sources-per-port: "
            f"need at least {required_count}"
        )

    def add_grouped_source(source_port: str) -> None:
        while True:
            source_pool = (
                portal_cells
                if portal_cells and rng.random() < args.portal_source_ratio
                else cells
            )
            sx, sy = rng.choice(source_pool)
            item = (sx, sy, source_port)
            if item not in source_keys:
                source_keys.add(item)
                grouped_sources.append(item)
                return

    for _ in range(args.minimum_sources_per_port):
        for source_port in required_source_ports:
            add_grouped_source(source_port)
    while len(grouped_sources) < args.source_count:
        add_grouped_source(choose_source_port())

    def choose_radius() -> int:
        # Oversample the public high-loss short-distance bands.  The final band
        # automatically shrinks when max-cheb is below 64.
        boundaries = [
            (0, min(16, args.max_cheb), 0.40),
            (17, min(32, args.max_cheb), 0.30),
            (33, min(64, args.max_cheb), 0.20),
            (65, args.max_cheb, 0.10),
        ]
        valid = [
            (max(lo, args.min_cheb), hi, weight)
            for lo, hi, weight in boundaries
            if max(lo, args.min_cheb) <= hi
        ]
        pick = rng.random() * sum(weight for _, _, weight in valid)
        for lo, hi, weight in valid:
            if pick <= weight:
                return rng.randint(lo, hi)
            pick -= weight
        lo, hi, _ = valid[-1]
        return rng.randint(lo, hi)

    rows: set[tuple[str, str]] = set()
    attempts = 0
    while len(rows) < args.count:
        attempts += 1
        if attempts > args.count * 200:
            raise RuntimeError("could not generate enough unique legal queries")
        if grouped_sources:
            sx, sy, source_port = rng.choice(grouped_sources)
        else:
            source_pool = portal_cells if portal_cells and rng.random() < 0.35 else cells
            sx, sy = rng.choice(source_pool)
            source_port = choose_source_port()
        # Log-like distance mixture deliberately over-samples the very short cases
        # that are rare in a uniform whole-device draw.
        radius = choose_radius()
        if rng.random() < 0.5:
            dx = rng.choice((-radius, radius))
            dy = rng.randint(-radius, radius)
        else:
            dx = rng.randint(-radius, radius)
            dy = rng.choice((-radius, radius))
        target = (sx + dx, sy + dy)
        if target not in coord_to_name:
            continue
        source = f"{coord_to_name[(sx, sy)]}/{source_port}"
        destination = f"{coord_to_name[target]}/{choose_target_port(source_port)}"
        if source != destination:
            rows.add((source, destination))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(("From", "To"))
        writer.writerows(sorted(rows))

    distance_counts: Counter[str] = Counter()
    source_direction_counts: Counter[str] = Counter()
    for source, destination in rows:
        source_inst, source_port = source.split("/", 1)
        target_inst = destination.split("/", 1)[0]
        sx, sy = name_to_coord[source_inst]
        tx, ty = name_to_coord[target_inst]
        distance = max(abs(tx - sx), abs(ty - sy))
        band = "0-16" if distance <= 16 else "17-32" if distance <= 32 else "33-64" if distance <= 64 else "65+"
        distance_counts[band] += 1
        source_direction_counts[port_direction[source_port]] += 1

    manifest = {
        "rows": len(rows),
        "seed": args.seed,
        "min_cheb": args.min_cheb,
        "max_cheb": args.max_cheb,
        "source_count_requested": args.source_count,
        "minimum_sources_per_port": args.minimum_sources_per_port,
        "unique_sources": len({source for source, _ in rows}),
        "portal_cell_count": len(portal_cells),
        "source_direction": args.source_direction,
        "target_direction": args.target_direction,
        "template_requests": str(args.template_requests) if args.template_requests is not None else None,
        "template_source_ports": len(template_source_counts),
        "template_port_pairs": sum(len(values) for values in template_pair_counts.values()),
        "priority_source_stems": sorted(priority_source_stems),
        "priority_target_stems": sorted(priority_target_stems),
        "priority_port_ratio": args.priority_port_ratio,
        "source_direction_counts": dict(sorted(source_direction_counts.items())),
        "distance_counts": dict(sorted(distance_counts.items())),
    }
    if args.manifest is not None:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(
        f"wrote {len(rows)} legal queries; cheb={args.min_cheb}-{args.max_cheb}; "
        f"portal_cells={len(portal_cells)}; unique_sources={manifest['unique_sources']}; seed={args.seed}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
