// V73 capacity branch: retain exact route-state lookup through radius 16 and
// let the existing Hot endpoint Atlas cover 17..72.  This preserves V69/V72
// predictions while freeing roughly 40 MB of the submission budget for the
// validated P10 periodic Portal closure and compact local connectors.
#define SRB_EXACT_LOCAL_ROUTE_TABLE
#define SRB_ATLAS_NO_BASE
#ifndef SRB_HOT_INNER_RADIUS
#define SRB_HOT_INNER_RADIUS 16
#endif
#define SRB_RUNTIME_ATLAS_MIN_BYTES 500000000ull
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v73.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v73.tmp"
#endif
#include "release_v69.cpp"
