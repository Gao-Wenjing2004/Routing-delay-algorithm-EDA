#!/usr/bin/env python3
"""Extract and validate the packed endpoint Atlas embedded in a V7 estimator."""

from __future__ import annotations

import argparse
import hashlib
import shutil
import struct
from pathlib import Path


FOOTER = struct.Struct("<16sQQ")
HEADER = struct.Struct("<8s10I2Q")
FOOTER_MAGIC = b"SRB7ATLAS2026091"
ATLAS_MAGIC = b"SRBE2E1\0"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("estimator", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    if args.estimator.resolve() == args.output.resolve():
        raise SystemExit("input and output paths must differ")

    size = args.estimator.stat().st_size
    with args.estimator.open("rb") as source:
        if source.read(4) != b"\x7fELF":
            raise SystemExit("input is not an ELF executable")
        if size < FOOTER.size:
            raise SystemExit("input is too small")
        source.seek(size - FOOTER.size)
        magic, offset, atlas_bytes = FOOTER.unpack(source.read(FOOTER.size))
        if magic != FOOTER_MAGIC:
            raise SystemExit("V7 Atlas footer was not found")
        if offset % 4096 or atlas_bytes != size - FOOTER.size - offset:
            raise SystemExit("invalid embedded Atlas bounds")
        source.seek(offset)
        header = HEADER.unpack(source.read(HEADER.size))
        if header[:5] != (ATLAS_MAGIC, 2, 496, 24, 72):
            raise SystemExit(f"unexpected Atlas header: {header[:5]!r}")
        source.seek(offset)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        remaining = atlas_bytes
        with args.output.open("wb") as output:
            while remaining:
                block = source.read(min(1024 * 1024, remaining))
                if not block:
                    raise SystemExit("truncated embedded Atlas")
                output.write(block)
                digest.update(block)
                remaining -= len(block)

    print(f"bytes={atlas_bytes} sha256={digest.hexdigest()} output={args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
