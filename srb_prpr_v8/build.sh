#!/bin/sh
set -eu
cd "$(dirname "$0")"

python3 tools/export_p2_trees.py \
    --teacher analysis/p2_8/p2_teacher_lightgbm_model.json \
    --student analysis/p2_8/p2_lightgbm_model.json \
    --output p2_runtime/p2_student_data_fast.hpp
mkdir -p build submission/bin
g++ -std=c++17 -O3 -DNDEBUG -march=x86-64 -mtune=generic \
    -DP2_FAST_MODEL -static-libgcc -static-libstdc++ \
    p2_runtime/estimate.cpp -o build/estimate
strip build/estimate
cp build/estimate submission/bin/estimate
chmod 755 submission/bin/estimate

bytes=$(wc -c < submission/bin/estimate)
if [ "$bytes" -ge 90000000 ]; then
    echo "error: executable is $bytes bytes (90 MB engineering limit)" >&2
    exit 1
fi
echo "built submission/bin/estimate ($bytes bytes)"
