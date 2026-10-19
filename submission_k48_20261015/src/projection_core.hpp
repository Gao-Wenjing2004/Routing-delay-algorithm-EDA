#pragma once
#include <cstdint>

namespace srb_v11_adaptive_long {
inline unsigned signed_phase(int dx,int dy){return unsigned((dx%10+10)%10)*12+unsigned((dy%12+12)%12);}
inline unsigned sign_quadrant(int dx,int dy){return unsigned(dx<0)+2u*unsigned(dy<0);}
enum class Outcome:uint8_t {OldUnavailable=0,Legacy=1,Adaptive=2,ChosenMissingFallback=3};
struct ProjectionResult {bool available=false;uint32_t arc=0;Outcome outcome=Outcome::OldUnavailable;};
// Old SUCCESS is the gate, including the old biased value's nonnegative check.
// This helper receives direct signed Arc; no old-bias subtraction is permitted.
inline ProjectionResult project(bool old_available,uint32_t old_arc,bool eligible,
                                bool chosen_available,int32_t chosen_arc,int16_t phi){
    if(!old_available)return {};
    if(!eligible)return {true,old_arc,Outcome::Legacy};
    if(!chosen_available)return {true,old_arc,Outcome::ChosenMissingFallback};
    const int32_t value=chosen_arc+int32_t(phi);
    return {true,value<0?0u:uint32_t(value),Outcome::Adaptive};
}
}
