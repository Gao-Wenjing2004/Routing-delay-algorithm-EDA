#pragma once
#define SRB_MERGED_MODEL
#define SRB_MODEL_HEADER "model_data.hpp"
#define SRB_NORM_PHASE_HEADER "phase_data.hpp"
#include <stdexcept>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include "normalized_estimator.hpp"
#include "direct_port_parser.hpp"
#include "native_parameters.hpp"
#include "hull_table.hpp"
#include "expanded_residual.hpp"
#include "unsafe_residual.hpp"
#include "direct_teacher.hpp"
#include "local_expanded.hpp"
#include "short_table.hpp"
#include "selected_model.hpp"
#include "adaptive_d72.hpp"
#include "projection_binding.hpp"
#include "adaptive_model/selected_model.hpp"
#include "long_binding.hpp"
#include "long_model/selected_model.hpp"
#include "long_model/block_states.hpp"
#include "pair30_teacher.hpp"
#include "pair_model/selected_model.hpp"
#include "short48/generated_binding.hpp"
#include "short48/new_model_hook.hpp"
#include "short48_model/selected_model.hpp"
#include <filesystem>
#include <memory>

namespace srb_v11_hybrid {
class Estimator {
public:
    explicit Estimator(const std::string&path,int local_radius=24,bool use_atlas=true):local_radius_(local_radius){
        if(local_radius<0||local_radius>24)throw std::runtime_error("invalid local radius");
        if(use_atlas){
            const auto parent=std::filesystem::path(path).parent_path();
            local_=std::make_unique<srb_v11_local_expand::Local>((parent/"normalized_local.bin").string(),srb_v10_parameters::graph_signature);
            if(local_->radius()!=24)throw std::runtime_error("expected frozen R24 Local");
            short_=std::make_unique<srb_v11_adaptive_d72::Table>((parent/"short_k32.bin").string(),path,
                (parent/"adaptive_a.bin").string(),srb_v11_adaptive_d72_generated::binding());
            prototype_=std::make_unique<srb_v11_adaptive_long_control::Table>(
                (parent/"complete_hull.bin").string(),(parent/"adaptive_a.bin").string(),*short_,
                srb_v11_adaptive_long_runtime_generated::binding(),srb_v11_adaptive_long_control::Mode::CertifiedQ6,
                srb_v11_adaptive_long_control_generated::certificate(),(parent/"short_k32.bin").string());
            if(!prototype_->q6_gate_enabled())throw std::runtime_error("frozen q6 certificate unavailable");
            pair_map_=std::make_unique<srb_v11_pair30::AssignmentMap>((parent/"pair30.delta.bin").string(),*short_,
                srb_v11_adaptive_d72_generated::binding(),srb_v11_pair30::Deployment::D72,(parent/"short_k32.bin").string(),1);
            teacher_=std::make_unique<srb_v11_pair30_runtime::Teacher>(*short_,*prototype_,*pair_map_);
            short48_=std::make_unique<srb_v11_short48::Table>(*short_,(parent/"augment48_full110.bin").string(),
                (parent/"adaptive_a.bin").string(),srb_v11_adaptive_d72_generated::binding(),srb_v11_short48_generated::binding());
            short48_teacher_=std::make_unique<srb_v11_short48_teacher::Teacher>(*short48_,*teacher_);
        }
    }
    struct PreparedRequest{
        srb_fast::Endpoint a,b,n;
        uint32_t extra;int dx,dy,gap;unsigned code;
        const uint16_t*local_row;
        srb_v11_adaptive_long_control::Table::PreparedQuery hull;
        srb_v11_short_k32::Table::PreparedQuery short_query;
        srb_v11_short48_teacher::Teacher::Prepared short48_query;
        uint8_t state;bool lng,hull_valid,short_valid,pair_short,pair_long,short_deferred,short48_valid;
    };
    bool prepare_spec(srb_fast::Slice from,srb_fast::Slice to,PreparedRequest&q)const{
        srb_fast::Endpoint a,b;if(!parser_.parse_endpoint(from,a)||!parser_.parse_endpoint(to,b))return false;
        prepare(a,b,q);return true;
    }
    bool predict_spec(srb_fast::Slice from,srb_fast::Slice to,uint32_t&v,uint8_t&kind)const{
        PreparedRequest q;if(!prepare_spec(from,to,q))return false;v=predict_prepared(q,kind);return true;
    }
    uint32_t predict(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b,uint8_t&kind)const{
        PreparedRequest q;prepare(a,b,q);return predict_prepared(q,kind);
    }
    uint32_t predict_prepared(const PreparedRequest&q,uint8_t&kind)const{
        kind=0;if(q.state==1){kind=3;return 0;}
        if(q.state==2)return srb_normalization::kUnreachableDelay;
        if(q.state==3){
            if(q.local_row){const uint16_t v=*q.local_row;if(v!=65535){kind=1;return q.extra+q.gap+v;}}
            if(q.dx==0&&q.dy==0&&(q.n.port==q.b.port||srb_normalization::ForcedNetNormalizer::direct_arc(q.n.port,q.b.port)>=0))
                return model_.predict_normalized(q.a,q.b,q.n,q.extra);
            if(q.hull_valid){uint32_t arc;bool pair=false;
                if(teacher_->query_long(q.n.port,q.b.port,q.dx,q.dy,q.hull,q.pair_long,arc,pair)){
                    kind=2;const unsigned code=model_.native_correction_code(q.n,q.b);uint32_t out;
                    if(pair&&srb_v11_pair30_models::prediction_cached(3,arc+q.extra+q.gap,
                        q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y,code,255,255,out))return out;
                    if(pair&&!teacher_->parent_long(q.n.port,q.b.port,q.dx,q.dy,arc))throw std::runtime_error("pair30 safe fallback lost parent C4 availability");
                    return parent_long_prediction(q,3,arc,code,255,255);
                }
            }
            // Successful new teacher reaches only fresh typed weights. Any false resumes full Parent below.
            if(short48_teacher_&&!q.lng&&q.short48_valid&&(std::abs(q.dx)>26||std::abs(q.dy)>26)&&
               std::abs(q.dx)+std::abs(q.dy)<=72){
                srb_v11_short48_teacher::AFeatures features;
                features.source=q.n.port;features.target=q.b.port;features.dx=q.dx;features.dy=q.dy;
                features.nx=q.n.x;features.ny=q.n.y;features.tx=q.b.x;features.ty=q.b.y;
                features.native_code=model_.native_correction_code(q.n,q.b);uint32_t candidate=0xffffffffu;
                if(srb_v11_short48_teacher::try_new_prediction(*short48_teacher_,q.short48_query,q.extra,q.gap,features,short48_model_,candidate)){
                    kind=4;return candidate;
                }
            }
            // A false new teacher, offset or model resumes the complete Parent.
            // Only its preparation was deferred; no new Arc reaches old weights.
            srb_v11_short_k32::Table::PreparedQuery deferred_short{};
            const auto* short_query=&q.short_query;
            bool short_valid=q.short_valid,pair_short=q.pair_short;
            if(q.short_deferred){
                short_valid=teacher_->prepare_short(q.n.port,q.b.port,q.dx,q.dy,true,deferred_short,pair_short);
                short_query=&deferred_short;
            }
            uint32_t arc;unsigned branch=0;bool pair=false;
            if(!q.lng&&short_valid&&teacher_->query_short(*short_query,pair_short,arc,pair))branch=1;
            else if(short_->dualget(q.n.port,q.b.port,q.dx,q.dy,arc))branch=q.lng?3:2;
            if(branch){
                kind=4;
                if(branch==1&&(std::abs(q.dx)>26||std::abs(q.dy)>26)){
                    const unsigned code=model_.native_correction_code(q.n,q.b);uint32_t out;
                    if(pair&&srb_v11_pair30_models::prediction_cached(1,arc+q.extra+q.gap,
                        q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y,code,255,255,out))return out;
                    if(pair&&!teacher_->parent_short(q.n.port,q.b.port,q.dx,q.dy,arc))throw std::runtime_error("pair30 A fallback lost parent C4 availability");
                    if(srb_v11_adaptive_a_models::prediction_cached(1,arc+q.extra+q.gap,
                        q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y,code,255,255,out))return out;
                    if(!short_->legacy().query(q.n.port,q.b.port,q.dx,q.dy,arc))throw std::runtime_error("adaptive A fallback lost legacy availability");
                }
                // Original B/C/ring path unchanged; only complete parent Arc may reach it after A API fallback.
                return srb_v11_short_models::prediction(branch,arc+q.extra+q.gap,0,
                    q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y);
            }
        }
        if(q.hull_valid){uint32_t arc;bool pair=false;
            if(teacher_->query_long(q.n.port,q.b.port,q.dx,q.dy,q.hull,q.pair_long,arc,pair)){
                kind=2;const unsigned code=model_.native_correction_code(q.n,q.b);
                const unsigned sc=srb_v11_adaptive_long_models::block_class(q.n.x,q.n.y),tc=srb_v11_adaptive_long_models::block_class(q.b.x,q.b.y);uint32_t out;
                if(pair&&srb_v11_pair30_models::prediction_cached(4,arc+q.extra+q.gap,
                    q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y,code,sc,tc,out))return out;
                if(pair&&!teacher_->parent_long(q.n.port,q.b.port,q.dx,q.dy,arc))throw std::runtime_error("pair30 unsafe fallback lost parent C4 availability");
                return parent_long_prediction(q,4,arc,code,sc,tc);
            }
        }
        return model_.predict_normalized(q.a,q.b,q.n,q.extra);
    }
    size_t atlas_memory_bytes()const{return (local_?local_->memory_bytes():0)+(short_?short_->memory_bytes():0)+(prototype_?prototype_->memory_bytes():0)+(pair_map_?pair_map_->owned_payload_bytes():0)+(short48_?short48_->owned_payload_bytes():0);}
private:
    uint32_t parent_long_prediction(const PreparedRequest&q,unsigned branch,uint32_t arc,unsigned code,unsigned sc,unsigned tc)const{
        uint32_t out;
        if(srb_v11_adaptive_long_models::prediction_cached(branch,arc+q.extra+q.gap,
            q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y,code,sc,tc,out))return out;
        if(!prototype_->legacy().query_prepared(q.hull.old_query,arc))throw std::runtime_error("Long model fallback lost old availability");
        if(branch==3)return uint32_t(std::max(0,int(q.extra)+q.gap+int(arc)+
            srb_v11_expanded_entry::correction(q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y)));
        return srb_v11_unsafe_direct::prediction(arc+q.extra+q.gap,
            q.n.port,q.b.port,q.dx,q.dy,q.n.x,q.n.y,q.b.x,q.b.y);
    }
    void prepare(const srb_fast::Endpoint&a,const srb_fast::Endpoint&b,PreparedRequest&q)const{
        q.a=a;q.b=b;q.state=0;q.local_row=nullptr;
        q.hull_valid=q.short_valid=q.pair_short=q.pair_long=q.short_deferred=q.short48_valid=false;
        if(a.x==b.x&&a.y==b.y&&a.port==b.port){q.state=1;return;}
        if(!model_.normalize_source(a,q.n,q.extra)){q.state=2;return;}
        q.dx=int(b.x)-q.n.x;q.dy=int(b.y)-q.n.y;
        q.lng=std::max(std::abs(int(b.x)-a.x),std::abs(int(b.y)-a.y))>72;
        if(!short_)return;
        if(model_.local_lookup_allowed(q.n,b)){
            q.state=3;
            if(local_radius_&&std::abs(q.dx)<=local_radius_&&std::abs(q.dy)<=local_radius_)
                q.local_row=local_->get_pointer(q.n.port,b.port,q.dx,q.dy);
            if(!q.lng&&!q.local_row){
                if(short48_teacher_&&(std::abs(q.dx)>26||std::abs(q.dy)>26)&&std::abs(q.dx)+std::abs(q.dy)<=72){
                    // The reader repeats the original legacy availability gate.
                    // Prepare the new selection once and expose it to batch prefetch.
                    q.short_deferred=true;
                    q.short48_valid=short48_teacher_->prepare_new(q.n.port,b.port,q.dx,q.dy,true,q.short48_query);
                }else q.short_valid=teacher_->prepare_short(q.n.port,b.port,q.dx,q.dy,true,q.short_query,q.pair_short);
            }
        }
        if(q.state==3||q.lng){
            q.gap=std::abs(int(srb_fast_data::kXGapPrefix[b.x])-srb_fast_data::kXGapPrefix[q.n.x])
                 +std::abs(int(srb_fast_data::kYGapPrefix[b.y])-srb_fast_data::kYGapPrefix[q.n.y]);
            if(q.lng)q.hull_valid=teacher_->prepare_long(q.n.port,b.port,q.dx,q.dy,q.lng,q.hull,q.pair_long);
        }
#if !defined(SRB_BATCH_PREFETCH) || SRB_BATCH_PREFETCH
#if defined(__GNUC__) || defined(__clang__)
        if(q.local_row)__builtin_prefetch(q.local_row,0,3);
        if(q.short48_valid)__builtin_prefetch(q.short48_query.selected.value,0,3);
#endif
        if(q.short_valid)short_->prefetch(q.short_query);
        if(q.hull_valid)prototype_->prefetch(q.hull);
#endif
    }
    srb_normalization::NormalizedEstimator model_;DirectPortParser parser_;
    std::unique_ptr<srb_v11_local_expand::Local>local_;
    std::unique_ptr<srb_v11_adaptive_d72::Table>short_;
    std::unique_ptr<srb_v11_adaptive_long_control::Table>prototype_;
    std::unique_ptr<srb_v11_pair30::AssignmentMap>pair_map_;
    std::unique_ptr<srb_v11_pair30_runtime::Teacher>teacher_;
    std::unique_ptr<srb_v11_short48::Table>short48_;
    std::unique_ptr<srb_v11_short48_teacher::Teacher>short48_teacher_;
    srb_v11_short48_models::Model short48_model_;int local_radius_;
};
}
