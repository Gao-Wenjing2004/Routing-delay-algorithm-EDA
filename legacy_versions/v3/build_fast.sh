#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")" && pwd)"
cd "$project_dir"

skip_train=0
rebuild_atlas=0
for arg in "$@"; do
    case "$arg" in
        --skip-train) skip_train=1 ;;
        --rebuild-atlas) rebuild_atlas=1 ;;
        *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

if [[ "$skip_train" -eq 0 ]]; then
    python3 ./train_fast_model.py
fi
python3 ./prepare_local_arch.py

cxxflags=(-std=c++17 -O3 -DNDEBUG -march=native -Wall -Wextra -pedantic)
g++ "${cxxflags[@]}" ./generate_local_atlas.cpp -o ./generate_local_atlas
if [[ "$rebuild_atlas" -eq 1 ]] || ! ./generate_local_atlas --check ./local_atlas.bin; then
    ./generate_local_atlas ./local_atlas.bin
fi

g++ "${cxxflags[@]}" ./estimate.cpp -o ./estimate
echo "Built: $project_dir/estimate"
