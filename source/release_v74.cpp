// V74 capacity proof: embed the validated 46.16 MB P10 closure next to V73's
// radius-16 exact table.  Until local connector selection passes its own
// accuracy/time gates, the closure is validated and directly mapped but does
// not change predictions.
#define SRB_P10_PERIODIC_PORTAL
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v74.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v74.tmp"
#endif
#include "release_v73.cpp"
