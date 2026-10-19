#pragma once
#include "optimized_long.hpp"
#include "pair30_map.hpp"

namespace srb_v11_pair30_runtime {
using Short=srb_v11_adaptive_d72::Table;
using Long=srb_v11_adaptive_long_control::Table;

// All objects are caller-owned and must outlive this small borrowing adapter.
// No second Short/Hull/fullphi/P/count/Atlas is allocated here.
class Teacher {
public:
    Teacher(const Short&s,const Long&l,const srb_v11_pair30::AssignmentMap&m):short_(s),long_(l),map_(m){
        if(l.legacy().sources()!=s.legacy().sources()||l.legacy().targets()!=s.legacy().targets()||l.legacy().medoids()!=s.legacy().medoids())
            throw std::runtime_error("pair30 shared provider rowmaps mismatch");
    }
    bool prepare_short(unsigned source,unsigned target,int dx,int dy,bool guarded_old_A,
                       Short::PreparedQuery&q,bool&pair)const{
        pair=false;
        if(!short_.legacy().prepare(source,target,dx,dy,q))return false;
        if(!guarded_old_A||(std::abs(dx)<=26&&std::abs(dy)<=26))return true;
        // Original Short constructor proves every actual stored field finite.
        // Without that proof retain its original availability checks.
        if(!short_.finite_fastpath_enabled()&&*q.value==65535)return true;
        const int sr=long_.legacy().source_row(source),tr=long_.legacy().target_row(target);
        if(sr<0||tr<0)throw std::runtime_error("pair30 Short ordered rows contradiction");
        const unsigned quad=srb_v11_adaptive_d72::quadrant(dx,dy),phase=srb_v11_adaptive_d72::phase(dx,dy);
        const unsigned m=map_.medoid_at(unsigned(sr),quad,phase,unsigned(tr));
        const uint16_t*chosen=q.value-short_.legacy().assignment()[unsigned(tr)]+m;
        if(!short_.finite_fastpath_enabled()&&*chosen==65535)
            return short_.prepare_adaptive(source,target,dx,dy,guarded_old_A,q); // Complete parent C4 teacher.
        q.value=chosen;q.bias=short_.adaptive_bias()[((m*4+quad)*120+phase)*256+unsigned(tr)];pair=true;return true;
    }
    bool query_short(const Short::PreparedQuery&q,bool prepared_pair,uint32_t&out,bool&pair)const{
        pair=false;if(!short_.query_prepared(q,out))return false;pair=prepared_pair;return true;
    }
    bool prepare_long(unsigned source,unsigned target,int dx,int dy,bool original_long_live,
                      Long::PreparedQuery&p,bool&pair)const{
        pair=false;
        if(!original_long_live||!long_.q6_gate_enabled())
            return long_.prepare_adaptive(source,target,dx,dy,original_long_live,p);
        const auto&h=long_.legacy();const int sr=h.source_row(source),tr=h.target_row(target);
        if(sr<0||tr<0)return false;
        if(!h.prepare_medoid(source,target,dx,dy,h.assignment()[unsigned(tr)],p.old_query))return false;
        p.quadrant=uint8_t(srb_v11_adaptive_d72::quadrant(dx,dy));p.phase=uint8_t(srb_v11_adaptive_d72::phase(dx,dy));
        const unsigned m=map_.medoid_at(unsigned(sr),p.quadrant,p.phase,unsigned(tr));p.medoid=uint8_t(m);
        if(!h.prepare_medoid(source,target,dx,dy,m,p.chosen_query))throw std::runtime_error("pair30 Long chosen prepare contradiction");
        p.phi=short_.adaptive_bias()[((m*4+p.quadrant)*120+p.phase)*256+unsigned(tr)];p.adaptive_eligible=true;pair=true;return true;
    }
    bool query_long(unsigned source,unsigned target,int dx,int dy,const Long::PreparedQuery&p,
                    bool prepared_pair,uint32_t&out,bool&pair,Long::Trace*trace=nullptr)const{
        pair=false;
        if(!prepared_pair)return long_.query_prepared(p,out,trace);
        const auto&h=long_.legacy();const auto&old=p.old_query;
        // This is exactly the parent's certified q6 range/presence contract.
        // No chosen presence establishes old availability and no negative
        // biased result is silently promoted to a lower bound.
        if(!long_.q6_gate_enabled()||old.q<6||old.q>45||old.direction>=2||old.target>=h.header().targets)
            return parent_long(source,target,dx,dy,out,trace);
        if(!h.curve_present(old)){if(trace)*trace=Long::Trace{};return false;}
        if(!h.curve_present(p.chosen_query))
            return parent_long(source,target,dx,dy,out,trace); // Never target-only fallback plus parent C4 model.
        if(!long_.query_prepared(p,out,trace))throw std::runtime_error("pair30 certified Long availability contradiction");
        pair=true;return true; // Production passes trace=null: one chosen curve, no parent C4 evaluation.
    }
    bool parent_short(unsigned source,unsigned target,int dx,int dy,uint32_t&out)const{
        Short::PreparedQuery p{};return short_.prepare_adaptive(source,target,dx,dy,true,p)&&short_.query_prepared(p,out);
    }
    bool parent_long(unsigned source,unsigned target,int dx,int dy,uint32_t&out,Long::Trace*trace=nullptr)const{
        Long::PreparedQuery p{};return long_.prepare_adaptive(source,target,dx,dy,true,p)&&long_.query_prepared(p,out,trace);
    }
private:
    const Short&short_;const Long&long_;const srb_v11_pair30::AssignmentMap&map_;
};
}
