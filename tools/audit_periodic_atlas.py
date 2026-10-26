#!/usr/bin/env python3
"""Audit whether the large Hot Atlas can be reconstructed from smaller anchors.

The audit reads the deterministic V42/V67 version-4 Atlas directly.  It does
not use Golden labels: candidates are compared with exact Dijkstra values
already stored in the Atlas.  This makes radius/period selection independent
of the contest validation rows.
"""

from __future__ import annotations

import argparse
import json
import mmap
import random
import struct
from dataclasses import asdict, dataclass
from pathlib import Path


HEADER = struct.Struct("<8s10I2Q")
EXCEPTION_HEADER = struct.Struct("<8s4I")
EXCEPTION = struct.Struct("<IH")
EXTENSION_HEADER = struct.Struct("<8s8IQ")


@dataclass
class Result:
    radius: int
    attempts: int = 0
    comparable: int = 0
    exact: int = 0
    absolute_error_sum: int = 0
    max_absolute_error: int = 0
    missing_anchor: int = 0

    def add(self, expected: int, predicted: int | None) -> None:
        self.attempts += 1
        if predicted is None:
            self.missing_anchor += 1
            return
        self.comparable += 1
        error = abs(predicted - expected)
        self.exact += error == 0
        self.absolute_error_sum += error
        self.max_absolute_error = max(self.max_absolute_error, error)

    def report(self) -> dict[str, object]:
        data = asdict(self)
        data["coverage"] = self.comparable / self.attempts if self.attempts else 0.0
        data["exact_rate"] = self.exact / self.comparable if self.comparable else 0.0
        data["mae_ps"] = (
            self.absolute_error_sum / self.comparable if self.comparable else None
        )
        return data


class Atlas:
    def __init__(self, path: Path):
        self.path = path
        self.handle = path.open("rb")
        self.mm = mmap.mmap(self.handle.fileno(), 0, access=mmap.ACCESS_READ)
        values = HEADER.unpack_from(self.mm, 0)
        (
            magic,
            version,
            ports,
            base_radius,
            hot_radius,
            sources,
            targets,
            hot_sources,
            hot_targets,
            hot_cells,
            reserved,
            base_values,
            hot_values,
        ) = values
        if magic != b"SRBE2E1\0" or version != 4 or ports != 496:
            raise ValueError("expected a version-4 endpoint Atlas")
        if base_radius != 24 or hot_radius != 72:
            raise ValueError("unexpected Atlas radii")
        self.base_radius = base_radius
        self.hot_radius = hot_radius
        self.sources = sources
        self.targets = targets
        self.hot_source_count = hot_sources
        self.hot_target_count = hot_targets
        self.hot_cells = hot_cells
        self.base_values = base_values
        self.hot_values = hot_values
        self.base_bits = reserved & 255
        self.hot_bits = (reserved >> 8) & 255
        self.base_record_bytes = 2 + 2 * self.base_bits
        self.hot_record_bytes = 2 + 2 * self.hot_bits

        pos = 64
        self.base_source_ids = struct.unpack_from(f"<{sources}H", self.mm, pos)
        pos += 2 * sources
        self.base_target_ids = struct.unpack_from(f"<{targets}H", self.mm, pos)
        pos += 2 * targets
        self.hot_source_ids = struct.unpack_from(f"<{hot_sources}H", self.mm, pos)
        pos += 2 * hot_sources
        self.hot_target_ids = struct.unpack_from(f"<{hot_targets}H", self.mm, pos)
        pos += 2 * hot_targets
        self.prefix = pos
        self.base_offset = pos
        self.hot_offset = self.base_offset + base_values // 16 * self.base_record_bytes
        exception_offset = self.hot_offset + hot_values // 16 * self.hot_record_bytes
        emagic, eversion, base_count, hot_count, ereserved = EXCEPTION_HEADER.unpack_from(
            self.mm, exception_offset
        )
        if emagic[:7] != b"SRBEXC1" or eversion != 1 or ereserved:
            raise ValueError("invalid exception section")
        pos = exception_offset + EXCEPTION_HEADER.size
        self.base_exceptions = self._read_exceptions(pos, base_count)
        pos += base_count * EXCEPTION.size
        self.hot_exceptions = self._read_exceptions(pos, hot_count)
        pos += hot_count * EXCEPTION.size
        self.base_source_map = {port: index for index, port in enumerate(self.base_source_ids)}
        self.base_target_map = {port: index for index, port in enumerate(self.base_target_ids)}
        self.hot_source_map = {port: index for index, port in enumerate(self.hot_source_ids)}
        self.hot_target_map = {port: index for index, port in enumerate(self.hot_target_ids)}
        self.extension_offset = pos
        self.extension = None
        if pos + EXTENSION_HEADER.size <= len(self.mm):
            self._load_extension(pos)

    def _load_extension(self, offset: int) -> None:
        (
            magic,
            version,
            inner,
            radius,
            cells,
            sources,
            targets,
            bits,
            exception_count,
            values,
        ) = EXTENSION_HEADER.unpack_from(self.mm, offset)
        if magic[:7] != b"SRBEXT1" or version != 2 or inner != 72 or bits != 8:
            raise ValueError("invalid extension section")
        pos = offset + EXTENSION_HEADER.size
        source_ids = struct.unpack_from(f"<{sources}H", self.mm, pos)
        pos += 2 * sources
        target_ids = struct.unpack_from(f"<{targets}H", self.mm, pos)
        pos += 2 * targets
        data_offset = pos
        exception_offset = data_offset + values // 16 * 18
        exceptions = self._read_exceptions(exception_offset, exception_count)
        self.extension = {
            "inner": inner,
            "radius": radius,
            "cells": cells,
            "sources": sources,
            "targets": targets,
            "values": values,
            "source_ids": source_ids,
            "target_ids": target_ids,
            "source_map": {port: index for index, port in enumerate(source_ids)},
            "target_map": {port: index for index, port in enumerate(target_ids)},
            "data_offset": data_offset,
            "exceptions": exceptions,
            "bytes": exception_offset + exception_count * EXCEPTION.size - offset,
        }

    def close(self) -> None:
        self.mm.close()
        self.handle.close()

    def _read_exceptions(self, offset: int, count: int) -> dict[int, int]:
        # Only a tiny number of sampled mask values require this dictionary.
        return {
            index: value
            for index, value in struct.iter_unpack(
                "<IH", self.mm[offset : offset + count * EXCEPTION.size]
            )
        }

    def _get(
        self,
        index: int,
        offset: int,
        bits: int,
        record_bytes: int,
        exceptions: dict[int, int],
    ) -> int:
        record = offset + index // 16 * record_bytes
        lo = struct.unpack_from("<H", self.mm, record)[0]
        bit = index % 16 * bits
        byte = record + 2 + bit // 8
        shift = bit % 8
        raw = int.from_bytes(self.mm[byte : byte + 3], "little")
        value = (raw >> shift) & ((1 << bits) - 1)
        if value != (1 << bits) - 1:
            return lo + value
        return exceptions.get(index, 65535)

    def base(self, source: int, target: int, dx: int, dy: int) -> int | None:
        si = self.base_source_map.get(source)
        ti = self.base_target_map.get(target)
        if si is None or ti is None or max(abs(dx), abs(dy)) > 24:
            return None
        index = (si * 49 * 49 + (dy + 24) * 49 + dx + 24) * self.targets + ti
        value = self._get(
            index,
            self.base_offset,
            self.base_bits,
            self.base_record_bytes,
            self.base_exceptions,
        )
        return None if value == 65535 else value

    @staticmethod
    def hot_cell(dx: int, dy: int) -> int | None:
        if max(abs(dx), abs(dy)) <= 24 or max(abs(dx), abs(dy)) > 72:
            return None
        cell = 0
        # Same row-major annulus order used by the builder.  Closed form avoids
        # allocating the 145x145 loader table in this audit process.
        for y in range(-72, dy):
            cell += 145 if abs(y) > 24 else 96
        cell += dx + 72
        if abs(dy) <= 24:
            if dx < -24:
                pass
            elif dx > 24:
                cell -= 49
            else:
                return None
        return cell

    def hot(self, source: int, target: int, dx: int, dy: int) -> int | None:
        si = self.hot_source_map.get(source)
        ti = self.hot_target_map.get(target)
        cell = self.hot_cell(dx, dy)
        if si is None or ti is None or cell is None:
            return None
        index = (si * self.hot_cells + cell) * self.hot_target_count + ti
        value = self._get(
            index,
            self.hot_offset,
            self.hot_bits,
            self.hot_record_bytes,
            self.hot_exceptions,
        )
        return None if value == 65535 else value

    @staticmethod
    def extension_cell(dx: int, dy: int, radius: int) -> int | None:
        if max(abs(dx), abs(dy)) <= 72 or max(abs(dx), abs(dy)) > radius:
            return None
        width = 2 * radius + 1
        cell = 0
        for y in range(-radius, dy):
            cell += width if abs(y) > 72 else width - 145
        cell += dx + radius
        if abs(dy) <= 72:
            if dx < -72:
                pass
            elif dx > 72:
                cell -= 145
            else:
                return None
        return cell

    def extension_value(self, source: int, target: int, dx: int, dy: int) -> int | None:
        if self.extension is None:
            return None
        ext = self.extension
        si = ext["source_map"].get(source)
        ti = ext["target_map"].get(target)
        cell = self.extension_cell(dx, dy, ext["radius"])
        if si is None or ti is None or cell is None:
            return None
        index = (si * ext["cells"] + cell) * ext["targets"] + ti
        value = self._get(index, ext["data_offset"], 8, 18, ext["exceptions"])
        return None if value == 65535 else value

    def exact(self, source: int, target: int, dx: int, dy: int) -> int | None:
        if max(abs(dx), abs(dy)) <= 24:
            return self.base(source, target, dx, dy)
        return self.hot(source, target, dx, dy)


def extrapolate(
    atlas: Atlas,
    source: int,
    target: int,
    dx: int,
    dy: int,
    radius: int,
    x_period: int,
    y_period: int,
) -> int | None:
    if max(abs(dx), abs(dy)) <= radius:
        return atlas.exact(source, target, dx, dy)
    adx, ady = abs(dx), abs(dy)
    if adx > radius and ady > radius:
        sx = 1 if dx > 0 else -1
        sy = 1 if dy > 0 else -1
        ax0 = radius - x_period + 1
        ay0 = radius - y_period + 1
        ax = ax0 + (adx - ax0) % x_period
        ay = ay0 + (ady - ay0) % y_period
        nx = (adx - ax) // x_period
        ny = (ady - ay) // y_period
        value = atlas.exact(source, target, sx * ax, sy * ay)
        old_x = atlas.exact(source, target, sx * (ax - x_period), sy * ay)
        old_y = atlas.exact(source, target, sx * ax, sy * (ay - y_period))
        if value is None or old_x is None or old_y is None:
            return None
        return value + nx * (value - old_x) + ny * (value - old_y)
    if ady > radius:
        sign = 1 if dy > 0 else -1
        start = radius - y_period + 1
        anchor = start + (ady - start) % y_period
        steps = (ady - anchor) // y_period
        value = atlas.exact(source, target, dx, sign * anchor)
        old = atlas.exact(source, target, dx, sign * (anchor - y_period))
        return None if value is None or old is None else value + steps * (value - old)
    sign = 1 if dx > 0 else -1
    start = radius - x_period + 1
    anchor = start + (adx - start) % x_period
    steps = (adx - anchor) // x_period
    value = atlas.exact(source, target, sign * anchor, dy)
    old = atlas.exact(source, target, sign * (anchor - x_period), dy)
    return None if value is None or old is None else value + steps * (value - old)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("atlas", type=Path)
    parser.add_argument("--samples", type=int, default=1_000_000)
    parser.add_argument("--seed", type=int, default=0xA71A5)
    parser.add_argument("--radii", type=int, nargs="+", default=[24, 32, 40, 48, 56, 64])
    parser.add_argument("--x-period", type=int, default=10)
    parser.add_argument("--y-period", type=int, default=12)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--extension",
        action="store_true",
        help="compare radius-73..104 extension values against Hot anchors",
    )
    args = parser.parse_args()

    atlas = Atlas(args.atlas)
    try:
        if args.extension:
            if atlas.extension is None:
                raise ValueError("Atlas has no extension section")
            sources = sorted(
                set(atlas.extension["source_ids"])
                & set(atlas.hot_source_ids)
                & set(atlas.base_source_ids)
            )
            targets = sorted(
                set(atlas.extension["target_ids"])
                & set(atlas.hot_target_ids)
                & set(atlas.base_target_ids)
            )
            low, high = 73, atlas.extension["radius"]
        else:
            sources = sorted(set(atlas.hot_source_ids) & set(atlas.base_source_ids))
            targets = sorted(set(atlas.hot_target_ids) & set(atlas.base_target_ids))
            low, high = min(args.radii) + 1, 72
        rng = random.Random(args.seed)
        results = {radius: Result(radius) for radius in args.radii}
        generated = 0
        while generated < args.samples:
            dx = rng.randint(-high, high)
            dy = rng.randint(-high, high)
            distance = max(abs(dx), abs(dy))
            if distance < low or distance > high:
                continue
            source = rng.choice(sources)
            target = rng.choice(targets)
            expected = (
                atlas.extension_value(source, target, dx, dy)
                if args.extension
                else atlas.hot(source, target, dx, dy)
            )
            if expected is None:
                continue
            generated += 1
            for radius, result in results.items():
                if distance <= radius:
                    continue
                result.add(
                    expected,
                    extrapolate(
                        atlas,
                        source,
                        target,
                        dx,
                        dy,
                        radius,
                        args.x_period,
                        args.y_period,
                    ),
                )
        report = {
            "atlas": str(args.atlas.resolve()),
            "samples": generated,
            "seed": args.seed,
            "x_period": args.x_period,
            "y_period": args.y_period,
            "eligible_sources": len(sources),
            "eligible_targets": len(targets),
            "extension": args.extension,
            "atlas_extension_bytes": atlas.extension["bytes"] if atlas.extension else 0,
            "results": [results[radius].report() for radius in args.radii],
        }
        text = json.dumps(report, ensure_ascii=False, indent=2)
        print(text)
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(text + "\n", encoding="utf-8")
    finally:
        atlas.close()


if __name__ == "__main__":
    main()
