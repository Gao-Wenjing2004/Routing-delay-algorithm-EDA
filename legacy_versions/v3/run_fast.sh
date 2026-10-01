#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")" && pwd)"
cd "$project_dir"

input_csv="${1:-./arch/queries.csv}"
output_csv="${2:-./output/fast_predictions.csv}"
[[ -x ./estimate ]] || { echo "estimate does not exist; run ./build_fast.sh first" >&2; exit 1; }
[[ -f ./local_atlas.bin ]] || { echo "local_atlas.bin does not exist; run ./build_fast.sh first" >&2; exit 1; }
mkdir -p "$(dirname "$output_csv")"
./estimate -in "$input_csv" -out "$output_csv"
echo "Output: $output_csv"
