#!/bin/sh
# Build the frozen original K48 Lazy estimator inside fpga-ziguang:latest.
set -eu
cd "$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"

mkdir -p build bin qa
compiler="${CXX:-g++}"
strip_tool="${STRIP:-strip}"
command -v "$compiler" >/dev/null 2>&1 || {
    printf '%s\n' "C++ compiler unavailable: $compiler" >&2
    exit 1
}
command -v "$strip_tool" >/dev/null 2>&1 || {
    printf '%s\n' "strip unavailable: $strip_tool" >&2
    exit 1
}
command -v python3 >/dev/null 2>&1 || {
    printf '%s\n' 'python3 is required for build verification.' >&2
    exit 1
}

test -f src/estimate.cpp || {
    printf '%s\n' 'Missing frozen K48 Lazy source: src/estimate.cpp' >&2
    exit 1
}
for asset in adaptive_a.bin augment48_full110.bin complete_hull.bin lower_k32.bin normalized_local.bin pair30.delta.bin short_k32.bin; do
    test -f "bin/assets/$asset" || {
        printf '%s\n' "Missing frozen K48 Lazy asset: bin/assets/$asset" >&2
        exit 1
    }
done

# Link compression and C++ runtime libraries into the ELF. No Windows DLLs
# belong in the Linux runtime directory.
zlib_archive="$("$compiler" -print-file-name=libz.a)"
lzma_archive="$("$compiler" -print-file-name=liblzma.a)"
for archive in "$zlib_archive" "$lzma_archive"; do
    case "$archive" in
        /*) test -f "$archive" || {
                printf '%s\n' "Static library not found: $archive" >&2
                exit 1
            } ;;
        *) printf '%s\n' "Static library unavailable in compiler search path: $archive" >&2
           exit 1 ;;
    esac
done

printf '%s\n' 'Building the original K48 Lazy estimator with batch width 4.'
if "$compiler" -O3 -DNDEBUG \
    -DSRB_IO_OPTIMIZED=1 -DSRB_BATCH_PREFETCH=1 -DSRB_BATCH_WIDTH=4 \
    -Isrc -std=c++17 -march=x86-64 -mtune=generic \
    -static-libgcc -static-libstdc++ \
    -MMD -MF build/estimate.d \
    src/estimate.cpp "$zlib_archive" "$lzma_archive" \
    -Wl,-t -o build/estimate.elf \
    >build/compiler.stdout.txt 2>build/compiler.stderr.txt; then
    :
else
    cat build/compiler.stderr.txt >&2
    exit 1
fi

cp build/estimate.elf bin/estimate
"$strip_tool" bin/estimate
chmod 755 bin/estimate
chmod 644 bin/assets/adaptive_a.bin bin/assets/augment48_full110.bin bin/assets/complete_hull.bin bin/assets/lower_k32.bin bin/assets/normalized_local.bin bin/assets/pair30.delta.bin bin/assets/short_k32.bin

python3 - "$compiler" "$zlib_archive" "$lzma_archive" <<'PY'
import hashlib
import json
import platform
import stat
import subprocess
import sys
from pathlib import Path

root = Path.cwd()
expected = ['bin/estimate', 'bin/assets/adaptive_a.bin', 'bin/assets/augment48_full110.bin', 'bin/assets/complete_hull.bin', 'bin/assets/lower_k32.bin', 'bin/assets/normalized_local.bin', 'bin/assets/pair30.delta.bin', 'bin/assets/short_k32.bin']

actual = sorted(p.relative_to(root).as_posix() for p in (root/'bin').rglob('*') if p.is_file())
if actual != sorted(expected):
    raise SystemExit('Unexpected runtime files: ' + repr(actual))
elf = root/'bin/estimate'
with elf.open('rb') as f:
    header = f.read(20)
if not (header[:4] == b'\x7fELF' and header[4:6] == b'\x02\x01'
        and int.from_bytes(header[18:20], 'little') == 62):
    raise SystemExit('bin/estimate is not a little-endian Linux x86-64 ELF.')
if stat.S_IMODE(elf.stat().st_mode) != 0o755:
    raise SystemExit('bin/estimate must have Unix mode 0755.')

def sha256(path):
    h = hashlib.sha256()
    with path.open('rb') as f:
        for block in iter(lambda: f.read(1024*1024), b''):
            h.update(block)
    return h.hexdigest()

files = {
    name: {'bytes': (root/name).stat().st_size, 'sha256': sha256(root/name)}
    for name in expected
}
total = sum(item['bytes'] for item in files.values())
if total >= 100_000_000:
    raise SystemExit(f'Uncompressed runtime must be below 100,000,000 bytes; got {total}.')
report = {
    'candidate': 'K48_Lazy_batch4_linux',
    'platform': platform.platform(),
    'compiler_version': subprocess.check_output([sys.argv[1], '--version'], text=True).splitlines()[0],
    'flags': ['-O3', '-DNDEBUG', '-DSRB_IO_OPTIMIZED=1', '-DSRB_BATCH_PREFETCH=1',
              '-DSRB_BATCH_WIDTH=4', '-Isrc', '-std=c++17', '-march=x86-64', '-mtune=generic',
              '-static-libgcc', '-static-libstdc++'],
    'static_compression_libraries': [sys.argv[2], sys.argv[3]],
    'files': files,
    'uncompressed_runtime_bytes': total,
    'size_limit_bytes': 100_000_000,
    'elf_linux_x86_64_little_endian': True,
    'compiler_dependency_file': 'build/estimate.d',
    'source_sha256': {
        p.relative_to(root/'src').as_posix(): sha256(p)
        for p in sorted((root/'src').rglob('*')) if p.is_file()
    },
}
(root/'qa/build_report.json').write_text(json.dumps(report, indent=2)+'\n', encoding='utf-8')
print(f'Build verified: {total:,} bytes in the eight runtime files.', flush=True)
PY
