#pragma once
#include "adaptive_long.hpp"

namespace srb_v11_adaptive_long_control {
using Original=srb_v11_adaptive_long::Table;
using Hull=Original::Hull;
using Outcome=srb_v11_adaptive_long::Outcome;
enum class Mode:uint8_t {ReuseOnly=0,CertifiedQ6=1};
struct GateCertificate {
    std::string hull_sha256;
    uint64_t graph_signature=0,all_target_fields=0,first_fields=0;
    uint64_t negative_finite_q6_fields=1;
    uint32_t old_bias_crc=0,ordered_metadata_crc=0,frames=0;
    int32_t minimum_finite_q6_biased=-1;
    bool every_slope_positive=false,original_envelope_q6_45_frozen=false;
    bool certified()const{
        return hull_sha256==srb_v11_adaptive_long::kHullSha
            &&graph_signature==srb_v11_adaptive_d72::kGraphSignature
            &&all_target_fields==127733760ull&&first_fields==15966720ull&&frames==220
            &&negative_finite_q6_fields==0&&minimum_finite_q6_biased>=0
            &&old_bias_crc==2393794328u&&ordered_metadata_crc==2518757483u
            &&every_slope_positive&&original_envelope_q6_45_frozen;
    }
};

// Generic core permits small fake-curve fixture tests without loading parents.
// Real Hull supplies query_medoid_prepared BEFORE any bias or clamp.
template<class HullLike>
bool query_core(const HullLike& h,const Original::PreparedQuery& p,bool certified_q6,
                uint32_t& out,Original::Trace* trace=nullptr,uint32_t* legacy_arc=nullptr){
    Original::Trace tr{};const auto& old=p.old_query;
    if(old.q<6||old.q>45||old.direction>=2||old.target>=h.header().targets){if(trace)*trace=tr;return false;}
    const unsigned ry=unsigned(old.index/32)%12;
    bool raw_old_ready=false;int32_t raw_old=0;uint32_t old_arc=0;
    auto old_from_raw=[&](int32_t raw)->bool{
        const int32_t value=raw+int32_t(h.legacy_bias()[(old.direction*12+ry)*h.header().targets+old.target]);
        if(value<0)return false;old_arc=uint32_t(value);return true;
    };
    auto evaluate_old=[&]()->bool{
        if(!raw_old_ready){if(!h.query_medoid_prepared(old,raw_old))return false;raw_old_ready=true;}
        return old_from_raw(raw_old);
    };
    // Never use chosenpresence to establish the original branch.
    bool old_available=certified_q6?h.curve_present(old):evaluate_old();
    if(!old_available){if(trace)*trace=tr;return false;}
    tr.old_available=true;
    if(!p.adaptive_eligible){
        if(!evaluate_old())throw std::runtime_error("q6 certificate legacy nonnegative contradiction");
        tr.old_arc=old_arc;tr.outcome=Outcome::Legacy;out=old_arc;
        if(legacy_arc)*legacy_arc=old_arc;if(trace)*trace=tr;return true;
    }
    int32_t chosen_raw=0;bool chosen_available=false;
    const bool same_curve=p.chosen_query.index==old.index&&p.chosen_query.q==old.q
        &&p.chosen_query.direction==old.direction&&p.chosen_query.target==old.target;
    if(same_curve&&raw_old_ready){chosen_raw=raw_old;chosen_available=true;}
    else chosen_available=h.query_medoid_prepared(p.chosen_query,chosen_raw);
    tr.chosen_available=chosen_available;tr.chosen_raw=chosen_available?chosen_raw:0;
    if(!chosen_available){
        if(!evaluate_old())throw std::runtime_error("q6 certificate missing fallback contradiction");
        tr.old_arc=old_arc;tr.outcome=Outcome::ChosenMissingFallback;out=old_arc;
    }else{
        if(same_curve){raw_old=chosen_raw;raw_old_ready=true;}
        const int32_t value=chosen_raw+int32_t(p.phi);out=value<0?0u:uint32_t(value);tr.outcome=Outcome::Adaptive;
        if(trace||legacy_arc){
            if(!evaluate_old())throw std::runtime_error("q6 certificate trace oldArc contradiction");
            tr.old_arc=old_arc;
        }
    }
    if(legacy_arc)*legacy_arc=old_arc;if(trace)*trace=tr;return true;
}

class Table {
public:
    using PreparedQuery=Original::PreparedQuery;
    using Trace=Original::Trace;
    Table(const std::string& hull_path,const std::string& adaptive_pack_path,
          const srb_v11_adaptive_d72::Table& short_adaptive,const srb_v11_adaptive_long::Binding& binding,
          Mode mode,const GateCertificate& certificate,const std::string& deployed_cost_path)
        :original_(hull_path,adaptive_pack_path,short_adaptive,binding,deployed_cost_path),
         fast_(mode==Mode::CertifiedQ6&&certificate.certified()){
        // Original constructor still checks actual Hull SHA/oldbias CRC and full
        // provider metadata/phi/assignment bindings. Certificate is compiled by a
        // future Root-approved exporter; an editable JSON is never runtime input.
    }
    bool prepare_adaptive(unsigned s,unsigned t,int dx,int dy,bool original_long,PreparedQuery& p)const{
        return original_.prepare_adaptive(s,t,dx,dy,original_long,p);
    }
    bool query_prepared(const PreparedQuery& p,uint32_t& out,Trace* trace=nullptr,uint32_t* old_arc=nullptr)const{
        return query_core(original_.legacy(),p,fast_,out,trace,old_arc);
    }
    void prefetch(const PreparedQuery& p)const{original_.prefetch(p);}
    bool q6_gate_enabled()const{return fast_;}
    size_t memory_bytes()const{return original_.memory_bytes();}
    const Hull& legacy()const{return original_.legacy();}
private:
    Original original_;bool fast_=false;
};
}
