#pragma once
#include "unsafe_residual.hpp"
#include <algorithm>
#include <cstdint>
namespace srb_v11_unsafe_direct {
static_assert(srb_v11_unsafe_entry::kGroups==13,"frozen unsafe13 required");
// Same frozen signed Q3 correction; alpha=1 is fixed for this speed candidate.
// teacherRaw already includes target bias + physical forced-Net extra + Gap.
inline uint32_t prediction(uint32_t teacherRaw,unsigned source,unsigned target,
                           int dx,int dy,unsigned nx,unsigned ny,unsigned tx,unsigned ty){
    return uint32_t(std::max(0,int(teacherRaw)+srb_v11_unsafe_entry::correction(
        source,target,dx,dy,nx,ny,tx,ty)));
}
}
