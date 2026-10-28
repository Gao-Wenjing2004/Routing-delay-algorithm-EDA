#!/usr/bin/env bash
set -euo pipefail
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$root/nocache_v10/bin"
g++ -O3 -DNDEBUG -DSRB_IO_OPTIMIZED=1 -DSRB_BATCH_PREFETCH=1 \
  -DSRB_BATCH_WIDTH=4 -std=c++17 -march=x86-64 -mtune=generic \
  "$root/nocache_v10/src/estimate_trace.cpp" -o "$root/nocache_v10/bin/estimate_trace" \
  -static-libgcc -static-libstdc++ -Wl,-Bstatic -llzma -lz -Wl,-Bdynamic
cp "$root/nocache_v10/bin/estimate_trace" "$root/nocache_v10/bin/estimate"
