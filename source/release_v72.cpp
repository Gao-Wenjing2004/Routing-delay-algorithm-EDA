// V72 research branch: replace the radius-24 endpoint Base Atlas with a
// lossless direct-mapped 160-state table.  The radius-72 Hot Atlas remains the
// V69 control so this stage isolates local-table size, startup and throughput.
#define SRB_EXACT_LOCAL_ROUTE_TABLE
#define SRB_ATLAS_NO_BASE
#define SRB_RUNTIME_ATLAS_MIN_BYTES 500000000ull
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v72.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v72.tmp"
#endif
#include "release_v69.cpp"
