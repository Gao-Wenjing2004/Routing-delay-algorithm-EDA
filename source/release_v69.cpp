// V69 removes the 613 MB radius-73..104 endpoint Extension.  Queries in that
// band reuse the existing 10x12 periodic extrapolator with radius-72 Hot
// anchors.  The Base and Hot Atlas layouts and the online I/O path are
// unchanged, making this a narrow accuracy/cold-start ablation.
#define SRB_ATLAS_NO_EXTENSION
#ifndef SRB_RUNTIME_ATLAS_PATH
#define SRB_RUNTIME_ATLAS_PATH "/tmp/srb_runtime_atlas_v69.bin"
#endif
#ifndef SRB_RUNTIME_ATLAS_WIN_PATH
#define SRB_RUNTIME_ATLAS_WIN_PATH "srb_runtime_atlas_v69.tmp"
#endif
#include "release_v68.cpp"
