#!/bin/sh
set -eu
cd "$(dirname "$0")"

mkdir -p build submission_p38/bin
COMMON_FLAGS="-std=c++17 -O3 -DNDEBUG -march=x86-64 -mtune=generic \
-DP2_FAST_MODEL -DP18_FAST_BASE -DP18_TWO_COLUMN_IO \
-DP18_PERFECT_PORT_HASH -DP18_TRUST_PORTS -DP20_BLOCK_SIDES \
-DP21_TRUST_ENDPOINTS -DP24_DEFAULT_SUBMISSION_FAST_PATH \
-DP25_PAIR_NUMBER_FORMAT -DP35_FAST_SOA -DP36_AVX2_DISPATCH \
-DP38_FAST_SUBMISSION_PREDICT -DV9_COMPACT_SUBMISSION \
-DV9_PACKED_ATLAS -DV9_DISABLE_LEARNED_SELECTOR"

g++ $COMMON_FLAGS -c src/estimate_v9.cpp -o build/estimate_v9_p38.o
gcc -c src/p14_atlas_data.S -o build/p14_atlas_data_linux.o
g++ -static-libgcc -static-libstdc++ \
    build/estimate_v9_p38.o build/p14_atlas_data_linux.o \
    -o build/estimate_v9_p38_core
strip build/estimate_v9_p38_core
cp build/estimate_v9_p38_core submission_p38/bin/estimate
chmod 755 submission_p38/bin/estimate

bytes=$(wc -c < submission_p38/bin/estimate)
if [ "$bytes" -ge 100000000 ]; then
    echo "error: executable is $bytes bytes; exceeds 100 MB" >&2
    exit 1
fi
echo "built submission_p38/bin/estimate ($bytes bytes)"
