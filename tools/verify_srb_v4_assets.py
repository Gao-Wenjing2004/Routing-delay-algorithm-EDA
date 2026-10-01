#!/usr/bin/env python3
"""Fail closed when V4 model, V3 inference sources, architecture, or Atlas drift."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import zlib
from pathlib import Path
from typing import Iterable


ARCHITECTURE_NAMES = {
    "inst": "SRB_Inst.json",
    "port": "SRB_Port.json",
    "arc": "SRB_Arc.json",
    "net": "SRB_Net.json",
    "gap": "SRB_Gap.json",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Verify locked SRB V4 inference assets")
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--residual-header", required=True, type=Path)
    parser.add_argument("--arch-dir", required=True, type=Path)
    parser.add_argument("--atlas-file", required=True, type=Path)
    parser.add_argument("--v3-source-dir", required=True, type=Path)
    parser.add_argument("--fixed-splits", required=True, type=Path)
    parser.add_argument("--full-atlas-sha256", action="store_true")
    return parser.parse_args()


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_files(paths: Iterable[Path]) -> str:
    digest = hashlib.sha256()
    for path in sorted(paths, key=lambda item: item.name):
        digest.update(path.name.encode("utf-8"))
        digest.update(b"\0")
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1 << 20), b""):
                digest.update(block)
        digest.update(b"\0")
    return digest.hexdigest()


def crc32_file(path: Path) -> int:
    value = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            value = zlib.crc32(block, value)
    return value & 0xFFFFFFFF


def require_equal(label: str, actual: object, expected: object) -> None:
    if actual != expected:
        raise ValueError(f"{label} mismatch: expected {expected}, got {actual}")


def header_string(text: str, name: str) -> str:
    match = re.search(rf'{re.escape(name)}\[\]\s*=\s*"([^"]+)"', text)
    if not match:
        raise ValueError(f"residual header is missing {name}")
    return match.group(1)


def header_uint(text: str, name: str) -> int:
    match = re.search(rf"{re.escape(name)}\s*=\s*(?:UINT32_C\(0x([0-9A-Fa-f]+)\)|(\d+))", text)
    if not match:
        raise ValueError(f"residual header is missing {name}")
    return int(match.group(1), 16) if match.group(1) else int(match.group(2))


def main() -> int:
    args = parse_args()
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    architecture_paths = {
        key: args.arch_dir / filename for key, filename in ARCHITECTURE_NAMES.items()
    }
    for key, path in architecture_paths.items():
        require_equal(
            f"architecture file {path.name}",
            sha256_file(path),
            manifest["architecture_files_sha256"][key],
        )
    require_equal(
        "architecture aggregate fingerprint",
        sha256_files(architecture_paths.values()),
        manifest["architecture_fingerprint_sha256"],
    )

    require_equal("Atlas byte size", args.atlas_file.stat().st_size, manifest["atlas"]["bytes"])
    require_equal(
        "Atlas CRC32", f"{crc32_file(args.atlas_file):08X}", manifest["atlas"]["crc32"]
    )
    if args.full_atlas_sha256:
        require_equal("Atlas SHA-256", sha256_file(args.atlas_file), manifest["atlas"]["sha256"])

    for name, expected in manifest["inference_sources_sha256"].items():
        require_equal(f"V3 inference source {name}", sha256_file(args.v3_source_dir / name), expected)
    require_equal(
        "fixed validation splits",
        sha256_file(args.fixed_splits),
        manifest["fixed_splits_sha256"],
    )

    require_equal(
        "generated residual header SHA-256",
        sha256_file(args.residual_header),
        manifest["residual_header_sha256"],
    )
    header = args.residual_header.read_text(encoding="utf-8")
    require_equal("feature version", header_string(header, "kFeatureVersion"), manifest["feature_version"])
    require_equal(
        "compiled architecture fingerprint",
        header_string(header, "kArchitectureSha256"),
        manifest["architecture_fingerprint_sha256"],
    )
    require_equal("compiled Atlas CRC32", header_uint(header, "kAtlasCrc32"), int(manifest["atlas"]["crc32"], 16))
    require_equal("compiled parameter count", header_uint(header, "kParameterCount"), manifest["parameter_count"])
    print(
        "verified "
        f"feature={manifest['feature_version']} parameters={manifest['parameter_count']} "
        f"architecture={manifest['architecture_fingerprint_sha256']} "
        f"atlas_crc32={manifest['atlas']['crc32']}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
