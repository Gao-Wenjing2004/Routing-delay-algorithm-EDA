#!/usr/bin/env python3
"""Derive the V5 long-distance model only from JSON and the JSON-derived Atlas."""

from __future__ import annotations

import argparse
import json
import struct
from pathlib import Path

import numpy as np


def options():
    here = Path(__file__).resolve().parent
    p = argparse.ArgumentParser()
    p.add_argument("--ports", type=Path, default=here / "arch" / "SRB_Port.json")
    p.add_argument("--arcs", type=Path, default=here / "arch" / "SRB_Arc.json")
    p.add_argument("--nets", type=Path, default=here / "arch" / "SRB_Net.json")
    p.add_argument("--atlas", type=Path, default=here / "local_atlas.bin")
    p.add_argument("--out", type=Path, default=here / "arch_model.bin")
    return p.parse_args()


def load(path):
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def main():
    opt = options()
    port_rows = load(opt.ports).get("Port")
    names = [r["Name"] for r in port_rows]
    directions = [r["Direction"].lower() for r in port_rows]
    pid = {name: i for i, name in enumerate(names)}
    inputs = [p for p, d in enumerate(directions) if d == "input"]
    port_to_input = [-1] * len(names)
    for iid, p in enumerate(inputs):
        port_to_input[p] = iid

    input_to_route = [-1] * len(inputs)
    route_to_input = []
    output_net = [None] * len(names)
    for n in load(opt.nets)["Nets"]:
        op, ip = pid[n["from"]], pid[n["to"]]
        iid = port_to_input[ip]
        if input_to_route[iid] < 0:
            input_to_route[iid] = len(route_to_input)
            route_to_input.append(iid)
        output_net[op] = (input_to_route[iid], int(n["delta x"]), int(n["delta y"]))

    arc_map = {}
    for a in load(opt.arcs)["Arcs"]:
        sp, tp = pid[a["from"]], pid[a["to"]]
        iid = port_to_input[sp]
        key = (iid, tp)
        arc_map[key] = min(arc_map.get(key, 1 << 30), int(a["delay"]))

    transitions = [[] for _ in inputs]
    terminal = np.full((len(route_to_input), len(names)), 1 << 28, dtype=np.int32)
    for p in range(len(names)):
        iid = port_to_input[p]
        if iid >= 0 and input_to_route[iid] >= 0:
            terminal[input_to_route[iid], p] = 0
    for (iid, outp), cost in arc_map.items():
        net = output_net[outp]
        if net is not None:
            route, dx, dy = net
            transitions[iid].append((route, dx, dy, cost))
        sr = input_to_route[iid]
        if sr >= 0:
            terminal[sr, outp] = min(terminal[sr, outp], cost)

    with opt.atlas.open("rb") as f:
        raw = f.read(40)
    magic, version, radius, routes, cells, sources, header_bytes, values = struct.unpack(
        "<8sIIIIIIQ", raw
    )
    if not magic.startswith(b"SRBLAT2") or version != 1 or routes != len(route_to_input):
        raise ValueError("Atlas is incompatible with architecture")
    width = radius * 2 + 1
    atlas = np.memmap(
        opt.atlas, dtype="<u2", mode="r", offset=header_bytes,
        shape=(sources, cells, routes)
    )

    cycle = np.full((width, width), 65535, dtype=np.uint16)
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            cell = (dy + radius) * width + dx + radius
            # A long route is not required to return to the same routing Input at
            # every macro segment.  Use the cheapest source/target route pair as
            # the steady spatial cost; endpoint bias below pays the real access
            # and exit transients for the requested ports.
            matrix = np.asarray(atlas[:, cell, :], dtype=np.uint16)
            finite = matrix[matrix != 65535]
            if finite.size:
                cycle[dy + radius, dx + radius] = finite.min()

    signs = [(0, 0), (1, 0), (-1, 0), (0, 1), (1, 1), (-1, 1),
             (0, -1), (1, -1), (-1, -1)]
    direction_bins = len(signs)
    bias = np.zeros((direction_bins, len(names), len(names)), dtype=np.int16)
    inf = 1 << 28

    def source_seeds(port):
        iid = port_to_input[port]
        if iid >= 0:
            route = input_to_route[iid]
            if route >= 0:
                return [(route, 0, 0, 0)]
            return transitions[iid]
        net = output_net[port]
        if net is None:
            return []
        route, dx, dy = net
        return [(route, dx, dy, 0)]

    for q, (sign_x, sign_y) in enumerate(signs):
        samples = []
        for scale in (24, 34, 44):
            dx, dy = sign_x * scale, sign_y * scale
            base = int(cycle[dy + radius, dx + radius])
            if q == 0:
                base = 0
            if base == 65535:
                raise ValueError(f"no routing cost for canonical displacement {(dx, dy)}")
            residuals = np.zeros((len(names), len(names)), dtype=np.int32)
            for sp in range(len(names)):
                best = np.full(len(names), inf, dtype=np.int32)
                for route, sx, sy, initial in source_seeds(sp):
                    rdx, rdy = dx - sx, dy - sy
                    if abs(rdx) > radius or abs(rdy) > radius:
                        continue
                    cell = (rdy + radius) * width + rdx + radius
                    row = np.asarray(atlas[route, cell, :], dtype=np.int32)
                    row[row == 65535] = inf
                    candidate = np.min(row[:, None] + terminal, axis=0) + initial
                    best = np.minimum(best, candidate)
                valid = best < inf
                residuals[sp, valid] = np.clip(best[valid] - base, -32768, 32767)
            samples.append(residuals)
        bias[q] = np.median(np.stack(samples), axis=0).astype(np.int16)
        print(f"derived multi-scale endpoint bias {q + 1}/{direction_bins}")

    opt.out.parent.mkdir(parents=True, exist_ok=True)
    with opt.out.open("wb") as out:
        out.write(struct.pack("<8sIIIII", b"SRBAM01\0", 1, radius, routes, len(names), direction_bins))
        out.write(cycle.astype("<u2", copy=False).tobytes(order="C"))
        out.write(bias.astype("<i2", copy=False).tobytes(order="C"))
    print(f"generated={opt.out.resolve()} bytes={opt.out.stat().st_size}")
    print("data_source=SRB_Port.json+SRB_Arc.json+SRB_Net.json+local_atlas.bin (no Golden CSV)")


if __name__ == "__main__":
    main()
