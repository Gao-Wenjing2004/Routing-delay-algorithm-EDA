#!/bin/sh
set -eu
cd "$(dirname "$0")"

mkdir -p build submission/bin
g++ -std=c++17 -O3 -DNDEBUG -march=x86-64 -mtune=generic \
    -DP2_FAST_MODEL -DP2_POST_MEMORY -DV9_EXACT_MEMORY -DV9_COMPACT_LANDMARKS -DV9_COMPACT_SUBMISSION -DV9_STRUCTURED_P6 -DV9_STRUCTURED_P8 -static-libgcc -static-libstdc++ \
    src/estimate_v9.cpp src/p6_embedded_data.S src/p8_embedded_data.S -o build/estimate_v9_core
strip build/estimate_v9_core
cp build/estimate_v9_core submission/bin/estimate
chmod 755 submission/bin/estimate

bytes=$(wc -c < submission/bin/estimate)
if [ "$bytes" -ge 100000000 ]; then
    echo "error: executable is $bytes bytes; exceeds 100 MB" >&2
    exit 1
fi
echo "built submission/bin/estimate ($bytes bytes)"
