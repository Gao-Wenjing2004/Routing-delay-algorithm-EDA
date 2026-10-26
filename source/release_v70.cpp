// V70 keeps only the 72 Extension source slices whose public-panel ranking
// also preserves V68 accuracy on the independent 608K Dijkstra panel.  It
// remains a fixed O(1) Atlas lookup; unretained sources use the existing
// radius-72 periodic path.
#define SRB_EXTENSION_V70_PORTS
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v70.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v70.tmp"
#endif
#include "release_v68.cpp"
