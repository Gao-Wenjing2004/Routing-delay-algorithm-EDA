#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CXX="${CXX:-g++}"
PYTHON="${PYTHON:-python3}"
FLAGS=(-std=c++17 -O3 -DNDEBUG -march=native -Wall -Wextra -pedantic)
"$PYTHON" prepare_local_arch.py
"$CXX" "${FLAGS[@]}" estimate.cpp -o estimate
echo "Built single-file runtime-preprocessing V5: $(pwd)/estimate"
