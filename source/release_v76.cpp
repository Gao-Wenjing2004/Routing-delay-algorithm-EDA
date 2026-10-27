// V76: V75 radius-4 capacity plus exact P10 closure.  Reuse a Dijkstra field
// for Hot-Atlas ports that share one translated routing seed; this only changes
// cold construction order/cost, never the generated distances or online path.
#define SRB_HOT_INNER_RADIUS 4
#define SRB_HOT_ROUTE_REUSE
#define SRB_PARALLEL_HOT_ROUTE_REUSE
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v76.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v76.tmp"
#endif
#include "release_v74.cpp"
