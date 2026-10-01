#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
input="${1:-arch/queries.csv}"
output="${2:-output/v5_predictions.csv}"
mkdir -p "$(dirname "$output")"
./estimate -in "$input" -out "$output"
