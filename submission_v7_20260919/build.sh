#!/bin/sh
set -eu
cd "$(dirname "$0")"
mkdir -p build bin
g++ -std=c++17 -O3 -march=x86-64 -mtune=generic -static-libgcc -static-libstdc++ \
    -DSRB_EMBEDDED_ATLAS src/release.cpp -o build/estimate.elf
strip build/estimate.elf
python3 embed_atlas.py build/estimate.elf "${SRB_ATLAS_FILE:-../srb_fast_v7/endpoint_packed.bin}" bin/estimate
chmod 755 bin/estimate
