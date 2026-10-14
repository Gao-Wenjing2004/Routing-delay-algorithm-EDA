#!/bin/sh
set -eu

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo "usage: $0 TRAINING_INPUT [CXX]" >&2
    exit 2
fi

cd "$(dirname "$0")/.."
TRAINING_INPUT=$(realpath "$1")
CXX=${2:-g++}
PROFILE_ROOT=$(pwd)/build/p39-profile
PROFILE_DATA=$PROFILE_ROOT/data
PROFILE_OBJECT=$PROFILE_ROOT/estimate_v9.o
ATLAS_OBJECT=build/p14_atlas_data_p39.o
TRAIN_BINARY=build/estimate_v9_p39_train
FINAL_BINARY=build/estimate_v9_p39_pgo
SUBMISSION_BINARY=submission_p39/bin/estimate

mkdir -p build "$PROFILE_ROOT" "$PROFILE_DATA" submission_p39/bin
rm -f "$PROFILE_DATA"/*.gcda "$PROFILE_DATA"/*.gcno "$PROFILE_OBJECT"

COMMON_FLAGS="-std=c++17 -O3 -DNDEBUG -march=x86-64 -mtune=generic \
-DP2_FAST_MODEL -DP18_FAST_BASE -DP18_TWO_COLUMN_IO \
-DP18_PERFECT_PORT_HASH -DP18_TRUST_PORTS -DP20_BLOCK_SIDES \
-DP21_TRUST_ENDPOINTS -DP24_DEFAULT_SUBMISSION_FAST_PATH \
-DP25_PAIR_NUMBER_FORMAT -DP35_FAST_SOA -DP36_AVX2_DISPATCH \
-DP38_FAST_SUBMISSION_PREDICT -DV9_COMPACT_SUBMISSION \
-DV9_PACKED_ATLAS -DV9_DISABLE_LEARNED_SELECTOR"

gcc -c src/p14_atlas_data.S -o "$ATLAS_OBJECT"

# Keep the same object basename for the generate/use phases so GCC resolves
# the .gcda file deterministically even though the final executable differs.
$CXX $COMMON_FLAGS -fprofile-generate="$PROFILE_DATA" \
    -c src/estimate_v9.cpp -o "$PROFILE_OBJECT"
$CXX -fprofile-generate="$PROFILE_DATA" \
    "$PROFILE_OBJECT" "$ATLAS_OBJECT" \
    -static-libgcc -static-libstdc++ -o "$TRAIN_BINARY"
"$TRAIN_BINARY" -in "$TRAINING_INPUT" -out /dev/null --profile \
    2>build/p39_training_stage.log

rm -f "$PROFILE_OBJECT"
$CXX $COMMON_FLAGS -flto -fprofile-use="$PROFILE_DATA" \
    -fprofile-correction -Wno-missing-profile \
    -c src/estimate_v9.cpp -o "$PROFILE_OBJECT"
$CXX -flto -fprofile-use="$PROFILE_DATA" -fprofile-correction \
    "$PROFILE_OBJECT" "$ATLAS_OBJECT" \
    -static-libgcc -static-libstdc++ -o "$FINAL_BINARY"
strip "$FINAL_BINARY"
cp "$FINAL_BINARY" "$SUBMISSION_BINARY"
chmod 755 "$SUBMISSION_BINARY"

bytes=$(wc -c < "$SUBMISSION_BINARY")
if [ "$bytes" -ge 100000000 ]; then
    echo "error: executable is $bytes bytes; exceeds 100 MB" >&2
    exit 1
fi

echo "built $SUBMISSION_BINARY ($bytes bytes)"
echo "next: byte-compare P38/P39 outputs, then profile both with tools/profile_linux_perf.sh"
