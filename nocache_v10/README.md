# V10 no-cache research branch

This branch never retains request results, source histories, hot paths, LRU
entries or other input-dependent state.  Dijkstra is an offline Teacher only.
Static artifacts must be deterministic functions of the FPGA architecture and
must pass train, source-holdout and region-holdout panels independently.

`estimate_trace` reproduces the frozen K48 prediction and can emit an optional
evaluation sidecar identifying the selected K48 branch.  When a gated
`direct_residual.bin` is present it applies a fixed-index int8 model with one
fine lookup and at most one coarse lookup.  A rejected model is encoded as an
all-sentinel table and is byte-exact with K48.  The same source runs without a
sidecar as the release candidate; `build_trace.sh` emits both names.

See [docs/IMPLEMENTATION_AND_GATES.md](docs/IMPLEMENTATION_AND_GATES.md) for
the exact-data split, P10/connector boundary, asset-retirement gate and the
Linux AVX2/PGO ablation order.
