#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")" && pwd)"
cd "$project_dir"

train_model=0
build_v3=0
native=0
for arg in "$@"; do
    case "$arg" in
        --train-model) train_model=1 ;;
        --build-v3-parity) build_v3=1 ;;
        --native) native=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$train_model" -eq 1 ]]; then
    python3 ./train_v4_residual.py \
        --golden ../plusone-srb_fast_v3/srb_fast_v3/arch/delay_estimate_ans.csv \
        --v3-predictions ../plusone-srb_fast_v3/srb_fast_v3/output/v3_predictions_1m.csv \
        --arch-dir ../plusone-srb_fast_v3/srb_fast_v3/arch \
        --atlas-file ../plusone-srb_fast_v3/srb_fast_v3/local_atlas.bin \
        --fixed-splits ../analysis/srb_fast_v4/v4_fixed_splits.npz
fi

python3 ../tools/verify_srb_v4_assets.py \
    --manifest ./v4_model_manifest.json \
    --residual-header ./v4_residual_data.hpp \
    --arch-dir ../plusone-srb_fast_v3/srb_fast_v3/arch \
    --atlas-file ../plusone-srb_fast_v3/srb_fast_v3/local_atlas.bin \
    --v3-source-dir ../plusone-srb_fast_v3/srb_fast_v3 \
    --fixed-splits ../analysis/srb_fast_v4/v4_fixed_splits.npz

flags=(-std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic -static -static-libgcc -static-libstdc++)
if [[ "$native" -eq 1 ]]; then flags+=(-march=native); fi
g++ "${flags[@]}" ./estimate_v4.cpp -o ./estimate_v4
if [[ "$build_v3" -eq 1 ]]; then
    g++ "${flags[@]}" ../plusone-srb_fast_v3/srb_fast_v3/estimate.cpp -o ./estimate_v3_parity
fi
