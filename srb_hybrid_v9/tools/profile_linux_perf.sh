#!/bin/sh
set -eu

if [ "$#" -lt 2 ] || [ "$#" -gt 4 ]; then
    echo "usage: $0 BINARY INPUT [OUTPUT_DIR] [ROUNDS]" >&2
    exit 2
fi

BINARY=$(realpath "$1")
INPUT=$(realpath "$2")
OUTPUT_DIR=${3:-analysis/p36_linux_perf}
ROUNDS=${4:-7}
mkdir -p "$OUTPUT_DIR"

if ! command -v perf >/dev/null 2>&1; then
    echo "error: Linux perf is not installed" >&2
    exit 1
fi

export LC_ALL=C
EVENTS=task-clock,cycles,instructions,branches,branch-misses,cache-references,cache-misses,page-faults,context-switches,cpu-migrations

# Warm page cache and all lazy runtime state before measuring.
"$BINARY" -in "$INPUT" -out /dev/null --profile \
    2>"$OUTPUT_DIR/warmup_stage.log"

perf stat -x, -r "$ROUNDS" -e "$EVENTS" \
    -o "$OUTPUT_DIR/perf_stat.csv" -- \
    "$BINARY" -in "$INPUT" -out /dev/null --profile \
    2>"$OUTPUT_DIR/stage_profile.log"

# One real-output run catches formatting/write regressions and records a
# reproducible checksum independently from the /dev/null CPU profile.
"$BINARY" -in "$INPUT" -out "$OUTPUT_DIR/predictions.csv" --profile \
    2>"$OUTPUT_DIR/real_output_stage.log"
sha256sum "$OUTPUT_DIR/predictions.csv" > "$OUTPUT_DIR/predictions.sha256"

echo "perf report: $OUTPUT_DIR/perf_stat.csv"
echo "stage report: $OUTPUT_DIR/stage_profile.log"
echo "output hash: $OUTPUT_DIR/predictions.sha256"
