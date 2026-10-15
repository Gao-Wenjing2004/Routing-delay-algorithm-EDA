// V20: smaller exact-search halo plus cross-process shared architecture Atlas.
// The shared file contains only deterministic architecture-derived values,
// never request/result pairs.
#define SRB_BASE_MARGIN 8
#define SRB_SHARED_RUNTIME_ATLAS
#define SRB_MERGED_MODEL
#define SRB_RESIDUAL_COMPACT
#define SRB_RESIDUAL_HEADER "residual_model.hpp"
#define SRB_ENDPOINT_EXTENSION
#define SRB_LONG_EXTRAPOLATE_V2
#define SRB_PERIOD_GATE
#define SRB_GAP_GATE
#define SRB_ACTUAL_SOURCE_NET
#include "estimate_v18.cpp"
