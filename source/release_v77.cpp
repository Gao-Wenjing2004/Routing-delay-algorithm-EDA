// V77 research base: the minimum lossless radius-1 local table plus the full
// validated P10 periodic Portal closure.  P10 remains dormant until connector
// selection passes held-out accuracy and timing gates.
#define SRB_HOT_INNER_RADIUS 1
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v77.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v77.tmp"
#endif
#include "release_v74.cpp"
