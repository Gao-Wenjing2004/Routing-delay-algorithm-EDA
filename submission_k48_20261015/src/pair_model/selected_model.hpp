#pragma once
#include <cstdint>
#include <cstdlib>
#include "model_maps.hpp"
#include "A/residual.hpp"
#include "safeLong/residual.hpp"
#include "unsafeLong/residual.hpp"
namespace srb_v11_pair30_models {
inline bool prediction_cached(unsigned branch,uint32_t raw,unsigned source,unsigned target,int dx,int dy,unsigned nx,unsigned ny,unsigned tx,unsigned ty,unsigned code,unsigned sc,unsigned tc,uint32_t&out){
if(branch==1&&(dx<-72||dx>72||dy<-72||dy>72||(std::abs(dx)<=26&&std::abs(dy)<=26)))return false;
if(branch==1&&std::abs(dx)+std::abs(dy)>72)return false;
switch(branch){
case 1:return srb_v11_pair30_A::prediction_cached(raw,source,target,dx,dy,nx,ny,tx,ty,code,sc,tc,out);
case 3:return srb_v11_pair30_safeLong::prediction_cached(raw,source,target,dx,dy,nx,ny,tx,ty,code,sc,tc,out);
case 4:return srb_v11_pair30_unsafeLong::prediction_cached(raw,source,target,dx,dy,nx,ny,tx,ty,code,sc,tc,out);
default:return false;
}
}
inline void prefetch_cached(unsigned branch,unsigned source,unsigned target,unsigned code){
if(source>=496||target>=496||code>>8>=120||(code&255)>=72)return;
const int s=kSourceRow[source],t=kTargetRow[target];if(s<0||t<0)return;switch(branch){
case 1:srb_v11_pair30_A::prefetch_cached(unsigned(s),unsigned(t),code);break;
case 3:srb_v11_pair30_safeLong::prefetch_cached(unsigned(s),unsigned(t),code);break;
case 4:srb_v11_pair30_unsafeLong::prefetch_cached(unsigned(s),unsigned(t),code);break;
default:break;}
}
}
