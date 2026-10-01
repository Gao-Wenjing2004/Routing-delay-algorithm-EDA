#!/usr/bin/env python3
"""Verify V6 architecture, Atlas and generated model fingerprints before build."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
TOOLS = HERE.parent / "tools"
if str(TOOLS) not in sys.path:
    sys.path.insert(0, str(TOOLS))

import analyze_public_queries as public_analysis  # noqa: E402


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--manifest", required=True, type=Path)
    p.add_argument("--header", required=True, type=Path)
    p.add_argument("--arch-dir", required=True, type=Path)
    p.add_argument("--atlas", required=True, type=Path)
    opt = p.parse_args()
    manifest = json.loads(opt.manifest.read_text(encoding="utf-8"))
    arch = public_analysis.load_architecture(opt.arch_dir)
    checks = {
        "architecture": (
            arch.fingerprint_sha256,
            manifest["architecture_fingerprint_sha256"],
        ),
        "atlas": (sha256(opt.atlas), manifest["atlas_sha256"]),
        "header": (sha256(opt.header), manifest["header_sha256"]),
    }
    failed = [name for name, (actual, expected) in checks.items() if actual != expected]
    if failed:
        for name in failed:
            print(f"{name} fingerprint mismatch: actual={checks[name][0]} expected={checks[name][1]}")
        return 1
    print(
        f"verified feature={manifest['feature_version']} "
        f"parameters={manifest['parameter_count']} architecture={arch.fingerprint_sha256}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
