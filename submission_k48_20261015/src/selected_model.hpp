#pragma once
#include "model_maps.hpp"
#include "A/residual.hpp"
#include "B/residual.hpp"
#include "C/residual.hpp"

namespace srb_v11_short_models {
// Branch 1/2/3 is A/B/C from branch(). Caller keeps identity, forced-Net
// unreachable, exact, R24 Local, old safe-long hull, and unsafe branches.
// teacherRaw = query()/dualget() + physical forced-Net extra + Gap once.
// All deltas and source coordinates are after physical normalization.
inline uint32_t prediction(unsigned branchId,uint32_t teacherRaw,uint32_t normalizedNoAtlas,
                           unsigned source,unsigned target,int dx,int dy,
                           unsigned nx,unsigned ny,unsigned tx,unsigned ty){
    if(source>=496||target>=496)return normalizedNoAtlas;
    const int s=kSourceRow[source],t=kTargetRow[target];
    const unsigned p=kTargetPrototype[target];
    if(s<0||t<0||p>=32)return normalizedNoAtlas;
    switch(branchId){
        case 1:return srb_v11_short_a::prediction(teacherRaw,normalizedNoAtlas,s,t,p,dx,dy,nx,ny,tx,ty);
        case 2:return srb_v11_short_b::prediction(teacherRaw,normalizedNoAtlas,s,t,p,dx,dy,nx,ny,tx,ty);
        case 3:return srb_v11_short_c::prediction(teacherRaw,normalizedNoAtlas,s,t,p,dx,dy,nx,ny,tx,ty);
        default:return normalizedNoAtlas;
    }
}
inline constexpr unsigned kParameterBytes=507620,kMapBytes=2480;
}
