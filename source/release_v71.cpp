// V71 research branch: keep V69's fast no-Extension Atlas and replace its
// long-distance endpoint extrapolator with a direct-mapped 10x12 route-state
// period table generated from exact Dijkstra fields.
#define SRB_ROUTE_PERIOD_TABLE
#ifndef SRB_ROUTE_PERIOD_TABLE_PATH
#define SRB_ROUTE_PERIOD_TABLE_PATH "analysis/unique_eval/route_periodicity/route_period_table.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v71.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v69.tmp"
#endif
#include "release_v69.cpp"
