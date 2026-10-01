#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
python3 srb_fast_v6/verify_v6_assets.py \
  --manifest srb_fast_v6/v6_model_manifest.json \
  --header srb_fast_v6/v6_macro10_data.hpp \
  --arch-dir plusone-srb_fast_v3/srb_fast_v3/arch \
  --atlas plusone-srb_fast_v3/srb_fast_v3/local_atlas.bin
g++ -std=c++17 -O3 -DNDEBUG -Wall -Wextra -pedantic -static \
  srb_fast_v6/estimate_v6.cpp -o srb_fast_v6/estimate_v6
