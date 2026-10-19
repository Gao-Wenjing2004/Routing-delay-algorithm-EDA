#pragma once
#include "short48_teacher.hpp"
#include <limits>

namespace srb_v11_short48_teacher {
// Deliberately no implicit uint32 conversion: old model APIs cannot accept this type.
struct NewModelRaw {uint32_t value=0xffffffffu;};
struct AFeatures {
    unsigned source=496,target=496;int dx=0,dy=0;
    unsigned nx=0,ny=0,tx=0,ty=0,native_code=0;
};
inline bool with_offset(const NewArc& arc,uint32_t extra,int gap,NewModelRaw& out){
    out=NewModelRaw{};
    if(arc.projected.value==0xffffffffu||gap<0)return false;
    const int64_t value=int64_t(arc.projected.value)+extra+gap;
    if(value>std::numeric_limits<int32_t>::max())return false;
    out.value=uint32_t(value);return true;
}
// Future separately trained/frozen model wrapper must implement:
// bool predict(NewModelRaw,const AFeatures&,uint32_t&) const.
// No existing Parent model has this typed signature; no weights are supplied here.
template<class FreshK48Model>
bool try_new_prediction(const Teacher& teacher,const Teacher::Prepared& query,
                       uint32_t extra,int gap,const AFeatures& features,const FreshK48Model& model,uint32_t& out){
    out=0xffffffffu;NewArc arc;
    if(!teacher.query_new_only(query,arc))return false; // Do not evaluate unused offset/model.
    NewModelRaw raw;if(!with_offset(arc,extra,gap,raw))return false;
    uint32_t candidate=0xffffffffu;
    if(!model.predict(raw,features,candidate))return false;
    if(candidate==0xffffffffu)return false;
    out=candidate;return true;
}
// Caller on false resumes the ENTIRE untouched Parent predict_prepared body,
// using its original Parent PreparedQuery and original request. Never feed arc
// to pair30_models/adaptive_a_models/short_models as a shortcut.
}
