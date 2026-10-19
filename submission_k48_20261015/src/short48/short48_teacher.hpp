#pragma once
// Compile with the separately verified reader_v3 and frozen parent-v3 src include paths.
#include "short48_reader.hpp"
#include "pair30_teacher.hpp"

namespace srb_v11_short48_teacher {
using Reader=srb_v11_short48::Table;
using ParentTeacher=srb_v11_pair30_runtime::Teacher;
struct NewArc {srb_v11_short48::ProjectedRaw projected{};};
struct CompleteParentArc {uint32_t value=0xffffffffu;bool pair_used=false;};
enum class Origin:uint8_t {Unavailable,New48,CompleteParent};
struct AuditArc {uint32_t value=0xffffffffu;Origin origin=Origin::Unavailable;};
struct Trace {
    bool attempted=false,used=false,has_choice=false;
    uint8_t basis=255;
    int16_t phi=0;
    int32_t exact_basis_cost=-1;
    srb_v11_short48::Status status=srb_v11_short48::Status::Unprepared;
};

// The reader and complete parent teacher borrow the SAME actual Short instance;
// ParentTeacher also keeps the unchanged normalized Long and Pair30 providers.
class Teacher {
public:
    struct Prepared {
        Reader::Prepared selected{};
        unsigned source=496,target=496;
        int dx=0,dy=0;
        bool original_G=false,initialized=false;
    };
    Teacher(const Reader& reader,const ParentTeacher& parent):reader_(reader),parent_(parent){
        sources_.fill(-1);targets_.fill(-1);
        const auto& old=reader_.parent().legacy();
        for(unsigned row=0;row<old.sources().size();++row)sources_[old.sources()[row]]=int16_t(row);
        for(unsigned row=0;row<old.targets().size();++row)targets_[old.targets()[row]]=int16_t(row);
    }
    bool prepare_new(unsigned source,unsigned target,int dx,int dy,bool original_G,Prepared& out)const{
        out=Prepared{};out.source=source;out.target=target;out.dx=dx;out.dy=dy;
        out.original_G=original_G;out.initialized=true;
        return reader_.prepare_selected(source,target,dx,dy,original_G,out.selected);
    }
    // Production new-model API: NEVER silently substitute a parent Arc here.
    bool query_new_only(const Prepared& query,NewArc& out)const{
        out=NewArc{};
        return query.initialized&&query.original_G&&reader_.projected_teacher_raw(query.selected,out.projected);
    }
    // Explicit full Pair30-parent teacher. This is not parent_.parent_short(),
    // which means only the older C4 teacher inside the historical parent chain.
    bool query_complete_parent(const Prepared& query,CompleteParentArc& out)const{
        out=CompleteParentArc{};
        if(!query.initialized)return false;
        srb_v11_adaptive_d72::Table::PreparedQuery prepared{};bool pair=false,used=false;uint32_t arc=0;
        if(!parent_.prepare_short(query.source,query.target,query.dx,query.dy,query.original_G,prepared,pair)||
           !parent_.query_short(prepared,pair,arc,used))return false;
        out.value=arc;out.pair_used=used;return true;
    }
    // Diagnostic API only: candidate teacher on success, full current Parent on false.
    // Its result must NEVER be passed to a new or old model without checking Origin.
    bool query_for_audit(const Prepared& query,AuditArc& out,Trace* trace=nullptr)const{
        out=AuditArc{};NewArc candidate;bool available=query_new_only(query,candidate);
        if(available){out.value=candidate.projected.value;out.origin=Origin::New48;}
        else {CompleteParentArc parent;if(query_complete_parent(query,parent)){
            out.value=parent.value;out.origin=Origin::CompleteParent;available=true;}}
        if(trace)*trace=make_trace(query,out.origin==Origin::New48);
        return available;
    }
private:
    Trace make_trace(const Prepared& query,bool used)const{
        Trace out;out.attempted=query.original_G;out.used=used;out.status=query.selected.status;
        if(!query.initialized||!query.original_G||query.source>=496||query.target>=496||
           sources_[query.source]<0||targets_[query.target]<0||query.dx<-72||query.dx>72||query.dy<-72||query.dy>72)return out;
        const unsigned cell=unsigned(query.dy+72)*145+unsigned(query.dx+72);
        if(!Reader::domain(cell))return out;
        const unsigned sr=unsigned(sources_[query.source]),tr=unsigned(targets_[query.target]);
        const unsigned quad=srb_v11_adaptive_d72::quadrant(query.dx,query.dy),phase=srb_v11_adaptive_d72::phase(query.dx,query.dy);
        out.basis=reader_.choice_at(sr,quad,tr);out.phi=reader_.phi_at(out.basis,quad*120+phase,tr);out.has_choice=true;
        if(used){srb_v11_short48::BasisRaw raw;
            if(!reader_.selected_basis_raw(query.selected,raw)||query.selected.basis!=out.basis||query.selected.phi!=out.phi)
                throw std::runtime_error("K48 traced choice/raw/phi contradiction");
            out.exact_basis_cost=raw.value;
        }
        return out;
    }
    const Reader& reader_;const ParentTeacher& parent_;
    std::array<int16_t,496> sources_{},targets_{};
};
}
