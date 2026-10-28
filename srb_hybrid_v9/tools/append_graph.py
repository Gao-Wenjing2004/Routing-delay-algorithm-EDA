#!/usr/bin/env python3
"""Append the preprocessed graph and a fixed footer to a submission binary."""

from __future__ import annotations

import argparse
import shutil
import struct
from pathlib import Path


MAGIC = b"V9GRAPH1"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--graph", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as destination, args.executable.open("rb") as executable:
        shutil.copyfileobj(executable, destination, length=4 << 20)
        with args.graph.open("rb") as graph:
            shutil.copyfileobj(graph, destination, length=4 << 20)
        destination.write(struct.pack("<Q8s", args.graph.stat().st_size, MAGIC))
    print(f"embedded graph: {args.graph.stat().st_size} bytes; output: {args.output.stat().st_size} bytes")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
