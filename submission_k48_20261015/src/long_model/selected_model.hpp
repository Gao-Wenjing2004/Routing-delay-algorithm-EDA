#pragma once
#include <cstdint>
#include "model_maps.hpp"
#include "safeLong/residual.hpp"
#include "unsafeLong/residual.hpp"
namespace srb_v11_adaptive_long_models {
inline bool prediction_cached(unsigned branch,uint32_t raw,unsigned source,unsigned target,int dx,int dy,unsigned nx,unsigned ny,unsigned tx,unsigned ty,unsigned code,unsigned sc,unsigned tc,uint32_t&out){
switch(branch){
case 3:return srb_v11_adaptive_long_safeLong::prediction_cached(raw,source,target,dx,dy,nx,ny,tx,ty,code,sc,tc,out);
case 4:return srb_v11_adaptive_long_unsafeLong::prediction_cached(raw,source,target,dx,dy,nx,ny,tx,ty,code,sc,tc,out);
default:return false;
}
}
inline void prefetch_cached(unsigned branch,unsigned source,unsigned target,unsigned code){
if(source>=496||target>=496||code>>8>=120||(code&255)>=72)return;
const int s=kSourceRow[source],t=kTargetRow[target];if(s<0||t<0)return;
switch(branch){case 3:srb_v11_adaptive_long_safeLong::prefetch_cached(s,t,code);break;case 4:srb_v11_adaptive_long_unsafeLong::prefetch_cached(s,t,code);break;default:break;}
}
}
