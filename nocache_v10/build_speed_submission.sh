#!/usr/bin/env bash
set -euo pipefail

root="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
cd "$root"

work="$(realpath -m "${1:-analysis/nocache_v10/speed_submission}")"
case "$work" in
    "$root"/analysis/nocache_v10/*) ;;
    *) echo "refusing to replace build directory outside analysis/nocache_v10: $work" >&2; exit 1 ;;
esac
build="$work/build"
stage="$work/stage"
reports="$work/reports"
profile="$work/profile"
archive="$work/submission.zip"
requests="$work/unique_requests_608k.csv"
baseline="$work/k48.csv"

rm -rf "$work"
mkdir -p "$build" "$stage/bin/assets" "$reports" "$profile"

for tool in g++ strip zstd zip python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "missing build tool: $tool" >&2
        exit 1
    }
done

zstd -q -d -f benchmark_assets/unique_teacher_requests_608k.csv.zst -o "$requests"

compiler="${CXX:-g++}"
zlib_archive="$($compiler -print-file-name=libz.a)"
lzma_archive="$($compiler -print-file-name=liblzma.a)"
test -f "$zlib_archive"
test -f "$lzma_archive"

common=(
    -O3 -DNDEBUG -std=c++17 -pthread
    -DSRB_IO_OPTIMIZED=1 -DSRB_BATCH_PREFETCH=1
    -DSRB_PARALLEL_ASSET_LOAD=1
    -Isubmission_k48_20261015/src
    -march=x86-64 -mtune=generic
    -fomit-frame-pointer -ffunction-sections -fdata-sections -flto
    -static-libgcc -static-libstdc++
)
link=("$zlib_archive" "$lzma_archive" -Wl,--gc-sections)

# Train only control-flow/layout decisions.  The predictor and every frozen
# architecture asset remain byte-identical to K48.
"$compiler" "${common[@]}" \
    -fprofile-generate="$profile" -fprofile-update=atomic \
    nocache_v10/src/estimate_soa_mt.cpp "${link[@]}" -o "$build/estimate"
"$build/estimate" \
    -in "$requests" -out "$work/pgo_training.csv" \
    -atlas submission_k48_20261015/bin/assets/lower_k32.bin -threads 4 \
    2>"$reports/pgo_training.log"

"$compiler" "${common[@]}" \
    -fprofile-use="$profile" -fprofile-correction \
    nocache_v10/src/estimate_soa_mt.cpp "${link[@]}" -o "$build/estimate"
strip "$build/estimate"
chmod 755 "$build/estimate"

/usr/bin/time -f '%e' -o "$reports/k48.seconds" \
    submission_k48_20261015/bin/estimate -in "$requests" -out "$baseline" \
    2>"$reports/k48.log"

for threads in 1 2 4; do
    /usr/bin/time -f '%e' -o "$reports/candidate_${threads}.seconds" \
        "$build/estimate" -in "$requests" -out "$work/candidate_${threads}.csv" \
        -atlas submission_k48_20261015/bin/assets/lower_k32.bin -threads "$threads" \
        2>"$reports/candidate_${threads}.log"
    cmp "$baseline" "$work/candidate_${threads}.csv"
done

# This is the exact invocation style used by the platform.  It must select the
# quota-aware default automatically and remain byte-identical to K48.
/usr/bin/time -f '%e' -o "$reports/candidate_auto.seconds" \
    "$build/estimate" -in "$requests" -out "$work/candidate_auto.csv" \
    -atlas submission_k48_20261015/bin/assets/lower_k32.bin \
    2>"$reports/candidate_auto.log"
cmp "$baseline" "$work/candidate_auto.csv"

cp "$build/estimate" "$stage/bin/estimate"
cp submission_k48_20261015/bin/assets/* "$stage/bin/assets/"
chmod 755 "$stage/bin/estimate"
chmod 644 "$stage/bin/assets/"*

python3 - "$stage" "$reports" <<'PY'
import hashlib
import json
import re
import stat
import sys
from pathlib import Path

stage = Path(sys.argv[1])
reports = Path(sys.argv[2])
files = sorted(path for path in stage.rglob('*') if path.is_file())
relative = [path.relative_to(stage).as_posix() for path in files]
expected = ['bin/estimate'] + [
    'bin/assets/adaptive_a.bin',
    'bin/assets/augment48_full110.bin',
    'bin/assets/complete_hull.bin',
    'bin/assets/lower_k32.bin',
    'bin/assets/normalized_local.bin',
    'bin/assets/pair30.delta.bin',
    'bin/assets/short_k32.bin',
]
if relative != sorted(expected):
    raise SystemExit(f'unexpected package files: {relative!r}')

elf = stage / 'bin/estimate'
header = elf.read_bytes()[:20]
if not (header[:4] == b'\x7fELF' and header[4:6] == b'\x02\x01'
        and int.from_bytes(header[18:20], 'little') == 62):
    raise SystemExit('bin/estimate is not a Linux x86-64 ELF')
if stat.S_IMODE(elf.stat().st_mode) != 0o755:
    raise SystemExit('bin/estimate is not executable')

def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            value.update(block)
    return value.hexdigest()

def elapsed(name):
    return float((reports / name).read_text().strip())

def selected_threads():
    text = (reports / 'candidate_auto.log').read_text(errors='replace')
    match = re.search(r'\bthreads=(\d+)', text)
    if not match:
        raise SystemExit('auto-thread report is missing threads=...')
    return int(match.group(1))

total = sum(path.stat().st_size for path in files)
if total >= 100_000_000:
    raise SystemExit(f'uncompressed submission is {total:,} bytes; limit is 100,000,000')

baseline = elapsed('k48.seconds')
auto = elapsed('candidate_auto.seconds')
report = {
    'candidate': 'V10_speed_only_K48_byte_exact',
    'request_cache': False,
    'prediction_assets_changed': False,
    'rows': 608000,
    'byte_exact_threads': [1, 2, 4, 'auto'],
    'auto_threads': selected_threads(),
    'seconds': {
        'k48': baseline,
        'candidate_1': elapsed('candidate_1.seconds'),
        'candidate_2': elapsed('candidate_2.seconds'),
        'candidate_4': elapsed('candidate_4.seconds'),
        'candidate_auto': auto,
    },
    'speedup_vs_k48': baseline / auto,
    'uncompressed_runtime_bytes': total,
    'limit_bytes': 100_000_000,
    'files': {
        path.relative_to(stage).as_posix(): {
            'bytes': path.stat().st_size,
            'sha256': digest(path),
        }
        for path in files
    },
}
(reports / 'submission_report.json').write_text(
    json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, indent=2))
PY

rm -f "$archive"
(cd "$stage" && zip -q -9 -r "$archive" bin)
sha256sum "$archive" >"$reports/submission.sha256"
unzip -t "$archive" >/dev/null
echo "ready: $archive"
