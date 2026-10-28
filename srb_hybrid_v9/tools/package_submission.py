#!/usr/bin/env python3
"""Create a contest ZIP with bin/estimate and Unix executable permissions."""

from __future__ import annotations

import argparse
import hashlib
import json
import zipfile
from pathlib import Path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--estimate", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    payload = args.estimate.read_bytes()
    if payload[:4] != b"\x7fELF":
        raise ValueError("estimate is not an ELF executable")
    if len(payload) >= 100_000_000:
        raise ValueError("estimate exceeds the 100 MB contest limit")

    info = zipfile.ZipInfo("bin/estimate")
    info.create_system = 3
    info.external_attr = (0o100755 << 16)
    info.compress_type = zipfile.ZIP_DEFLATED
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(args.output, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        archive.writestr(info, payload)

    with zipfile.ZipFile(args.output) as archive:
        member = archive.getinfo("bin/estimate")
        if member.external_attr >> 16 != 0o100755 or archive.read(member) != payload:
            raise ValueError("ZIP verification failed")
    print(json.dumps({
        "zip": str(args.output),
        "zip_bytes": args.output.stat().st_size,
        "member": "bin/estimate",
        "member_bytes": len(payload),
        "member_mode": "100755",
        "sha256": hashlib.sha256(payload).hexdigest(),
    }, indent=2))


if __name__ == "__main__":
    main()
